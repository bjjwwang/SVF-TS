//===- cts-test.cpp -- Automated SVFIR build assertions -------------------//
//
// Each test provides a minimal C snippet and a set of assertions on the
// generated SVFStmt count/type/structure.  The harness writes the snippet
// to a temp file, runs the full CTS pipeline, checks assertions, then
// tears everything down.
//
// Usage: cts-test            (runs all tests, skips XFAIL)
//        cts-test <name>     (runs a single named test)
//        cts-test --all      (runs all tests including XFAIL)
//
// Test naming convention:
//   Section_Pattern  e.g. decl_int_init, expr_binary_add, ctrl_if_basic
//
//===----------------------------------------------------------------------//

#include "CTS/CTSSVFIRBuilder.h"
#include "CTS/CTSModule.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "Graphs/ICFG.h"

#include <iostream>
#include <fstream>
#include <functional>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

using namespace SVF;

// =========================================================================
// Helper functions for assertions
// =========================================================================

/// Count SVFStmts of a given kind across the whole PAG.
static size_t countStmts(SVFIR* pag, SVFStmt::PEDGEK kind)
{
    return pag->getSVFStmtSet(kind).size();
}

/// Count SVFStmts of a given kind that belong to a named function.
static size_t countStmtsInFunc(SVFIR* pag, SVFStmt::PEDGEK kind,
                               const std::string& funcName)
{
    size_t n = 0;
    for (const SVFStmt* s : pag->getSVFStmtSet(kind))
    {
        const ICFGNode* node = s->getICFGNode();
        if (!node) continue;
        const FunObjVar* fun = node->getFun();
        if (fun && fun->getName() == funcName) ++n;
    }
    return n;
}

/// Check whether any SVFVar in the PAG has the given name.
static bool hasVarNamed(SVFIR* pag, const std::string& name)
{
    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        if (it->second->getName() == name) return true;
    }
    return false;
}

/// Count FunObjVar nodes (represents functions in the PAG).
static size_t countFunctions(SVFIR* pag)
{
    size_t n = 0;
    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        if (SVFUtil::isa<FunObjVar>(it->second)) ++n;
    }
    return n;
}

/// Count ICFG nodes of a specific type.
static size_t countICFGNodes(SVFIR* pag, ICFGNode::ICFGNodeK kind)
{
    size_t n = 0;
    ICFG* icfg = pag->getICFG();
    for (auto it = icfg->begin(), eit = icfg->end(); it != eit; ++it)
    {
        if (it->second->getNodeKind() == kind) ++n;
    }
    return n;
}

/// Count CallICFGNodes (call sites).
static size_t countCallSites(SVFIR* pag)
{
    return countICFGNodes(pag, ICFGNode::FunCallBlock);
}

// =========================================================================
// Test infrastructure
// =========================================================================

struct TestCase
{
    std::string name;
    std::string code;
    std::function<bool(SVFIR*)> check;
    bool xfail;  // expected failure (crash or wrong result)
};

static int passed = 0;
static int failed = 0;
static int xfailed = 0;   // expected failures that did fail
static int xpassed = 0;   // expected failures that unexpectedly passed

/// Build SVFIR from a C source string in a forked subprocess.
/// Returns: 0=check passed, 1=check failed, 2=crash/signal
static int runTestForked(const TestCase& tc)
{
    std::string tmpPath = "/tmp/cts_test_" + tc.name + ".c";
    {
        std::ofstream ofs(tmpPath);
        if (!ofs) return 2;
        ofs << tc.code;
    }

    // Fork to isolate crashes (assertion failures, segfaults)
    pid_t pid = fork();
    if (pid < 0)
    {
        std::remove(tmpPath.c_str());
        return 2;
    }

    if (pid == 0)
    {
        // Child process: run the test
        // Suppress stdout/stderr from builder via dup2
        int devnull = open("/dev/null", O_WRONLY);
        int savedErr = dup(STDERR_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);

        try
        {
            CTSSVFIRBuilder builder;
            SVFIR* pag = builder.build({tmpPath});

            // Restore stderr for assertion messages
            dup2(savedErr, STDERR_FILENO);
            close(savedErr);

            bool ok = tc.check(pag);
            _exit(ok ? 0 : 1);
        }
        catch (...)
        {
            _exit(2);
        }
    }
    else
    {
        // Parent: wait for child
        int status = 0;
        waitpid(pid, &status, 0);
        std::remove(tmpPath.c_str());

        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        else
            return 2;  // killed by signal
    }
}

// =========================================================================
// Assertion macros — print context on failure
// =========================================================================

#define EXPECT(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "    FAIL: %s  (%s)\n", msg, #cond);               \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_EQ(actual, expected, msg)                                       \
    do {                                                                       \
        auto _a = (actual); auto _e = (expected);                              \
        if (_a != _e) {                                                        \
            fprintf(stderr, "    FAIL: %s  (got %lu, expected %lu)\n",          \
                    msg, (unsigned long)_a, (unsigned long)_e);                 \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_GE(actual, lower, msg)                                          \
    do {                                                                       \
        auto _a = (actual); auto _l = (lower);                                 \
        if (_a < _l) {                                                         \
            fprintf(stderr, "    FAIL: %s  (got %lu, expected >= %lu)\n",       \
                    msg, (unsigned long)_a, (unsigned long)_l);                 \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_GT(actual, lower, msg)                                          \
    do {                                                                       \
        auto _a = (actual); auto _l = (lower);                                 \
        if (_a <= _l) {                                                        \
            fprintf(stderr, "    FAIL: %s  (got %lu, expected > %lu)\n",        \
                    msg, (unsigned long)_a, (unsigned long)_l);                 \
            return false;                                                      \
        }                                                                      \
    } while (0)

// =========================================================================
// TEST CASES
// =========================================================================

static std::vector<TestCase> allTests = {

// =========================================================================
// SECTION 1: DECLARATIONS — types, initializers, pointers, arrays
// =========================================================================

    // --- 1.1 Primitive types with initializer ---

    {
        "decl_int_init",
        "int main() { int a = 1; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'a'");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 1u,
                      "StoreStmt for 'a = 1'");
            EXPECT(hasVarNamed(pag, "a"), "variable 'a' exists");
            return true;
        },
        false
    },

    {
        "decl_char_init",
        "int main() { char c = 'A'; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'c'");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 1u,
                      "StoreStmt for 'c = A'");
            EXPECT(hasVarNamed(pag, "c"), "variable 'c' exists");
            return true;
        },
        false
    },

    {
        "decl_float_init",
        "int main() { float f = 3.14; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'f'");
            EXPECT(hasVarNamed(pag, "f"), "variable 'f' exists");
            return true;
        },
        false
    },

    {
        "decl_double_init",
        "int main() { double d = 2.718; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'd'");
            EXPECT(hasVarNamed(pag, "d"), "variable 'd' exists");
            return true;
        },
        false
    },

    // --- 1.2 Sized type specifiers ---

    {
        "decl_unsigned_int",
        "int main() { unsigned int u = 42; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'u'");
            EXPECT(hasVarNamed(pag, "u"), "variable 'u' exists");
            return true;
        },
        false
    },

    {
        "decl_long_long",
        "int main() { long long ll = 100; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'll'");
            EXPECT(hasVarNamed(pag, "ll"), "variable 'll' exists");
            return true;
        },
        false
    },

    {
        "decl_short_int",
        "int main() { short int s = 5; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 's'");
            EXPECT(hasVarNamed(pag, "s"), "variable 's' exists");
            return true;
        },
        false
    },

    {
        "decl_unsigned_char",
        "int main() { unsigned char uc = 255; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for 'uc'");
            EXPECT(hasVarNamed(pag, "uc"), "variable 'uc' exists");
            return true;
        },
        false
    },

    // --- 1.3 Declaration without initializer ---

    {
        "decl_no_init",
        "int main() { int a; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for uninitialized 'a'");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 0u,
                      "no StoreStmt without initializer");
            return true;
        },
        false
    },

    // --- 1.4 Two separate declarations ---

    {
        "decl_two_vars",
        "int main() { int a = 1; int b = 2; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt count");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt count");
            EXPECT(hasVarNamed(pag, "a"), "'a' exists");
            EXPECT(hasVarNamed(pag, "b"), "'b' exists");
            return true;
        },
        false
    },

    // --- 1.5 Multi-declarator on one line (XFAIL: only first is handled) ---

    {
        "decl_multi_int",
        "int main() { int a, b, c; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt for a, b, c");
            EXPECT(hasVarNamed(pag, "a"), "'a' exists");
            EXPECT(hasVarNamed(pag, "b"), "'b' exists");
            EXPECT(hasVarNamed(pag, "c"), "'c' exists");
            return true;
        },
        false
    },

    {
        "decl_multi_ptr",
        "int main() { int *a, *b; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt for a and b");
            EXPECT(hasVarNamed(pag, "a"), "'a' exists");
            EXPECT(hasVarNamed(pag, "b"), "'b' exists");
            return true;
        },
        false
    },

    {
        "decl_multi_init",
        "int main() { int a = 1, b = 2; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt for a, b");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt for a=1, b=2");
            return true;
        },
        false
    },

    // --- 1.6 Pointer declarations ---

    {
        "decl_ptr",
        R"(int main() {
            int x = 1;
            int *p = &x;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (x, p)");
            EXPECT(hasVarNamed(pag, "x"), "'x' exists");
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            return true;
        },
        false
    },

    {
        "decl_double_ptr",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int **pp = &p;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (x, p, pp)");
            EXPECT(hasVarNamed(pag, "pp"), "'pp' exists");
            return true;
        },
        false
    },

    // --- 1.7 Array declarations ---

    {
        "decl_array",
        "int main() { int arr[10]; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for arr");
            EXPECT(hasVarNamed(pag, "arr"), "'arr' exists");
            return true;
        },
        false
    },

    {
        "decl_ptr_array",
        "int main() { int *arr[5]; return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for arr");
            EXPECT(hasVarNamed(pag, "arr"), "'arr' exists");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 2: POINTER OPERATIONS — store, load, copy, address-of
// =========================================================================

    {
        "ptr_store",
        R"(int main() {
            int x = 1;
            int *p = &x;
            *p = 2;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 3u,
                      "StoreStmt (x=1, p=&x, *p=2)");
            return true;
        },
        false
    },

    {
        "ptr_load",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int y = *p;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for *p");
            EXPECT(hasVarNamed(pag, "y"), "'y' exists");
            return true;
        },
        false
    },

    {
        "ptr_copy",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int *q = p;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (x, p, q)");
            EXPECT(hasVarNamed(pag, "q"), "'q' exists");
            return true;
        },
        false
    },

    {
        "ptr_deref_chain",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int **pp = &p;
            int y = **pp;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // **pp needs 2 loads: load pp → p_val, load p_val → x_val
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 2u,
                      "LoadStmt chain for **pp");
            EXPECT(hasVarNamed(pag, "y"), "'y' exists");
            return true;
        },
        false
    },

    {
        "ptr_null",
        R"(int main() {
            int *p = 0;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for p");
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 3: ASSIGNMENTS — simple, compound, chained
// =========================================================================

    {
        "assign_simple",
        R"(int main() {
            int a = 1;
            int b = 2;
            a = b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // a=1, b=2, a=b → 3 stores
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 3u,
                      "StoreStmt: a=1, b=2, a=b");
            // Reading b in "a = b"
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for reading b");
            return true;
        },
        false
    },

    {
        "assign_ptr",
        R"(int main() {
            int x = 1;
            int y = 2;
            int *p = &x;
            p = &y;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // p reassigned: Store(p=&x), Store(p=&y)
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 4u,
                      "StoreStmt (x=1, y=2, p=&x, p=&y)");
            return true;
        },
        false
    },

    // Compound assignment (XFAIL: only = is handled)
    {
        "assign_compound_add",
        R"(int main() {
            int a = 1;
            a += 2;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // a += 2 should produce: Load(a), BinaryOp(add), Store(a)
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt for a=1 and a+=2");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for reading a in a+=2");
            return true;
        },
        false
    },

    {
        "assign_compound_sub",
        R"(int main() {
            int a = 10;
            a -= 3;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt for a=10 and a-=3");
            return true;
        },
        false
    },

    {
        "assign_compound_mul",
        R"(int main() {
            int a = 2;
            a *= 3;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt for a=2 and a*=3");
            return true;
        },
        false
    },

    // Chained assignment
    {
        "assign_chained",
        R"(int main() {
            int a, b, c;
            a = b = c = 5;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // c=5, b=c, a=b — need at least 3 stores
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 3u,
                      "StoreStmt for chained a=b=c=5");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 4: EXPRESSIONS — binary, unary, cmp, cast, sizeof, ternary, comma
// =========================================================================

    // --- 4.1 Binary arithmetic (side effects evaluated, no BinaryOpStmt) ---

    {
        "expr_binary_add",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = a + b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            // a and b are loaded for the expression
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 2u,
                      "LoadStmt for a and b");
            // c gets a store
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 3u,
                      "StoreStmt (a=1, b=2, c=a+b)");
            EXPECT(hasVarNamed(pag, "c"), "'c' exists");
            return true;
        },
        false
    },

    {
        "expr_binary_sub",
        R"(int main() {
            int a = 10;
            int b = 3;
            int c = a - b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            EXPECT(hasVarNamed(pag, "c"), "'c' exists");
            return true;
        },
        false
    },

    {
        "expr_binary_mul",
        R"(int main() {
            int a = 2;
            int b = 3;
            int c = a * b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_binary_div",
        R"(int main() {
            int a = 10;
            int b = 2;
            int c = a / b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_binary_mod",
        R"(int main() {
            int a = 10;
            int b = 3;
            int c = a % b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    // --- 4.2 Bitwise operators ---

    {
        "expr_binary_bitand",
        R"(int main() {
            int a = 0xFF;
            int b = 0x0F;
            int c = a & b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_binary_bitor",
        R"(int main() {
            int a = 0xF0;
            int b = 0x0F;
            int c = a | b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_binary_xor",
        R"(int main() {
            int a = 0xFF;
            int b = 0x0F;
            int c = a ^ b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_binary_shift",
        R"(int main() {
            int a = 1;
            int b = a << 3;
            int c = a >> 1;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    // --- 4.3 Comparison operators ---

    {
        "expr_cmp_eq",
        R"(int main() {
            int a = 1;
            int b = 1;
            int c = (a == b);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_cmp_ne",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = (a != b);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "expr_cmp_lt_gt",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = (a < b);
            int d = (a > b);
            int e = (a <= b);
            int f = (a >= b);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 6u,
                      "AddrStmt (a, b, c, d, e, f)");
            return true;
        },
        false
    },

    // --- 4.4 Logical operators ---

    {
        "expr_logical_and_or",
        R"(int main() {
            int a = 1;
            int b = 0;
            int c = (a && b);
            int d = (a || b);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 4u,
                      "AddrStmt (a, b, c, d)");
            return true;
        },
        false
    },

    // --- 4.5 Unary operators ---

    {
        "expr_unary_neg",
        R"(int main() {
            int a = 5;
            int b = -a;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            EXPECT(hasVarNamed(pag, "b"), "'b' exists");
            return true;
        },
        false
    },

    {
        "expr_unary_not",
        R"(int main() {
            int a = 1;
            int b = !a;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            return true;
        },
        false
    },

    {
        "expr_unary_bitnot",
        R"(int main() {
            int a = 0xFF;
            int b = ~a;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            return true;
        },
        false
    },

    {
        "expr_unary_addr_of",
        R"(int main() {
            int x = 1;
            int *p = &x;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (x, p)");
            // &x should store address into p
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt (x=1, p=&x)");
            return true;
        },
        false
    },

    {
        "expr_unary_deref",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int y = *p;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for *p");
            return true;
        },
        false
    },

    // --- 4.6 Increment/decrement ---

    {
        "expr_update_postinc",
        R"(int main() {
            int a = 0;
            a++;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // a++ should at least load a for side effects
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for a");
            return true;
        },
        false
    },

    {
        "expr_update_predec",
        R"(int main() {
            int a = 5;
            --a;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for a");
            return true;
        },
        false
    },

    // --- 4.7 Cast expression ---

    {
        "expr_cast",
        R"(int main() {
            double d = 3.14;
            int a = (int)d;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (d, a)");
            // Cast should produce a CopyStmt
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Copy, "main"), 1u,
                      "CopyStmt for (int)d");
            return true;
        },
        false
    },

    {
        "expr_cast_ptr",
        R"(int main() {
            int x = 1;
            void *p = (void *)&x;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (x, p)");
            return true;
        },
        false
    },

    // --- 4.8 Sizeof expression ---

    {
        "expr_sizeof",
        R"(int main() {
            int a = sizeof(int);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for a");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Store, "main"), 1u,
                      "StoreStmt for a = sizeof(int)");
            return true;
        },
        false
    },

    // --- 4.9 Ternary/conditional expression ---

    {
        "expr_ternary",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = (a > 0) ? a : b;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            EXPECT(hasVarNamed(pag, "c"), "'c' exists");
            return true;
        },
        false
    },

    // --- 4.10 Comma expression ---

    {
        "expr_comma",
        R"(int main() {
            int a = 1;
            int b = (a = 2, a + 1);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            return true;
        },
        false
    },

    // --- 4.11 Parenthesized expression ---

    {
        "expr_paren",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = (a + b) * 2;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    // --- 4.12 String literal ---

    {
        "expr_string_literal",
        R"(int main() {
            char *s = "hello";
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for s");
            EXPECT(hasVarNamed(pag, "s"), "'s' exists");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 5: STRUCT AND ARRAY — GEP patterns
// =========================================================================

    {
        "struct_field_store",
        R"(struct S { int x; int y; };
        int main() {
            struct S s;
            s.x = 1;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 1u,
                      "GepStmt for s.x");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 1u,
                      "StoreStmt for s.x = 1");
            return true;
        },
        false
    },

    {
        "struct_field_load",
        R"(struct S { int x; int y; };
        int main() {
            struct S s;
            s.x = 1;
            int v = s.x;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for s.x (store + load)");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for reading s.x");
            return true;
        },
        false
    },

    {
        "struct_two_fields",
        R"(struct Point { int x; int y; };
        int main() {
            struct Point p;
            p.x = 10;
            p.y = 20;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for p.x and p.y");
            return true;
        },
        false
    },

    {
        "struct_nested",
        R"(struct Inner { int x; };
        struct Outer { struct Inner inner; int y; };
        int main() {
            struct Outer o;
            o.inner.x = 42;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt chain for o.inner.x");
            return true;
        },
        false
    },

    {
        "struct_arrow",
        R"(struct S { int x; int y; };
        int main() {
            struct S s;
            struct S *p = &s;
            p->x = 1;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 1u,
                      "GepStmt for p->x");
            return true;
        },
        false
    },

    {
        "struct_arrow_load",
        R"(struct S { int x; int y; };
        int main() {
            struct S s;
            struct S *p = &s;
            p->x = 1;
            int v = p->y;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for p->x and p->y");
            return true;
        },
        false
    },

    {
        "struct_ptr_field",
        R"(struct Node { int val; struct Node *next; };
        int main() {
            struct Node n;
            n.val = 1;
            n.next = 0;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for n.val and n.next");
            return true;
        },
        false
    },

    {
        "array_const_idx",
        R"(int main() {
            int arr[10];
            arr[0] = 1;
            arr[5] = 2;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for arr[0] and arr[5]");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt for arr[0]=1, arr[5]=2");
            return true;
        },
        false
    },

    {
        "array_var_idx",
        R"(int main() {
            int arr[10];
            int i = 3;
            arr[i] = 42;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 1u,
                      "GepStmt for arr[i]");
            return true;
        },
        false
    },

    {
        "array_read",
        R"(int main() {
            int arr[10];
            arr[0] = 1;
            int x = arr[0];
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for arr[0] write and read");
            EXPECT(hasVarNamed(pag, "x"), "'x' exists");
            return true;
        },
        false
    },

    {
        "array_of_structs",
        R"(struct S { int x; int y; };
        int main() {
            struct S arr[5];
            arr[0].x = 1;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // arr[0] → GEP, then .x → GEP
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt for arr[0].x");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 6: FUNCTION CALLS — parameters, returns, multiple args
// =========================================================================

    {
        "func_call_basic",
        R"(int add(int a, int b) { return a; }
        int main() {
            int r = add(1, 2);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmts(pag, SVFStmt::Call), 1u,
                      "CallPE for add()");
            EXPECT_GE(countStmts(pag, SVFStmt::Ret), 1u,
                      "RetPE for add()");
            EXPECT_GE(countFunctions(pag), 2u,
                      "2 functions (main, add)");
            return true;
        },
        false
    },

    {
        "func_call_void",
        R"(void noop() {}
        int main() {
            noop();
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            EXPECT_GE(countCallSites(pag), 1u, "1 call site");
            return true;
        },
        false
    },

    {
        "func_call_chain",
        R"(int f1(int x) { return x; }
        int f2(int x) { return f1(x); }
        int main() {
            int r = f2(1);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 3u,
                      "3 functions (main, f1, f2)");
            EXPECT_GE(countCallSites(pag), 2u,
                      "2 call sites (f2(1) and f1(x))");
            return true;
        },
        false
    },

    {
        "func_multiple",
        R"(void foo() { int a = 1; }
        void bar() { int b = 2; }
        int main() { foo(); bar(); return 0; })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 3u,
                      "3 functions: main, foo, bar");
            EXPECT(hasVarNamed(pag, "a"), "'a' in foo");
            EXPECT(hasVarNamed(pag, "b"), "'b' in bar");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "foo"), 1u,
                      "AddrStmt in foo");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "bar"), 1u,
                      "AddrStmt in bar");
            return true;
        },
        false
    },

    {
        "func_empty",
        "void noop() {} int main() { return 0; }",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "noop"), 0u,
                      "no AddrStmt in empty noop");
            return true;
        },
        false
    },

    {
        "func_extern_decl",
        R"(extern void ext_func(int x);
        int main() {
            ext_func(42);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u,
                      "2 functions (main, ext_func)");
            EXPECT_GE(countCallSites(pag), 1u,
                      "1 call site for ext_func");
            return true;
        },
        false
    },

    {
        "func_return_value",
        R"(int compute() {
            int x = 42;
            return x;
        }
        int main() {
            int r = compute();
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "compute"), 1u,
                      "StoreStmt for x = 42");
            EXPECT_GE(countStmts(pag, SVFStmt::Ret), 1u,
                      "RetPE for compute()");
            EXPECT(hasVarNamed(pag, "r"), "'r' exists");
            return true;
        },
        false
    },

    {
        "func_param_pointer",
        R"(void set(int *p, int v) { *p = v; }
        int main() {
            int x = 0;
            set(&x, 42);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "set"), 1u,
                      "StoreStmt for *p = v");
            EXPECT_GE(countStmts(pag, SVFStmt::Call), 1u,
                      "CallPE for set()");
            return true;
        },
        false
    },

    {
        "func_recursive",
        R"(int fact(int n) {
            if (n <= 1) return 1;
            return fact(n);
        }
        int main() {
            int r = fact(5);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            // fact calls itself
            EXPECT_GE(countCallSites(pag), 2u,
                      "2 call sites (main→fact, fact→fact)");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 7: CONTROL FLOW — if, while, for (ICFG structure)
// =========================================================================

    {
        "ctrl_if_basic",
        R"(int main() {
            int a = 1;
            if (a) {
                int b = 2;
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            return true;
        },
        false
    },

    {
        "ctrl_if_else",
        R"(int main() {
            int a = 1;
            int b;
            if (a) {
                b = 10;
            } else {
                b = 20;
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (a, b)");
            // b is stored in both branches
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 3u,
                      "StoreStmt (a=1, b=10, b=20)");
            return true;
        },
        false
    },

    {
        "ctrl_while_basic",
        R"(int main() {
            int i = 0;
            while (i) {
                int x = 1;
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (i, x)");
            return true;
        },
        false
    },

    {
        "ctrl_for_basic",
        R"(int main() {
            int sum = 0;
            for (int i = 0; i < 10; i++) {
                sum = i;
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 2u,
                      "AddrStmt (sum, i)");
            return true;
        },
        false
    },

    {
        "ctrl_nested_if",
        R"(int main() {
            int a = 1;
            int b = 2;
            if (a) {
                if (b) {
                    int c = 3;
                }
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (a, b, c)");
            return true;
        },
        false
    },

    {
        "ctrl_nested_loops",
        R"(int main() {
            int i = 0;
            while (i) {
                int j = 0;
                while (j) {
                    int k = 1;
                }
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 3u,
                      "AddrStmt (i, j, k)");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 8: GLOBALS — global variables, initialization
// =========================================================================

    {
        "global_int",
        R"(int g = 42;
        int main() { return 0; })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "g"), "global 'g' exists");
            // Global should have AddrStmt
            EXPECT_GE(countStmts(pag, SVFStmt::Addr), 1u,
                      "AddrStmt for global g");
            return true;
        },
        false
    },

    {
        "global_ptr",
        R"(int x = 1;
        int *p = &x;
        int main() { return 0; })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "x"), "global 'x' exists");
            EXPECT(hasVarNamed(pag, "p"), "global 'p' exists");
            return true;
        },
        false
    },

    {
        "global_uninitialized",
        R"(int g;
        int main() { return 0; })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "g"), "global 'g' exists");
            return true;
        },
        false
    },

    {
        "global_read_in_func",
        R"(int g = 42;
        int main() {
            int a = g;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "g"), "global 'g' exists");
            EXPECT(hasVarNamed(pag, "a"), "local 'a' exists");
            // Reading g should produce a LoadStmt
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for reading global g");
            return true;
        },
        false
    },

    {
        "global_write_in_func",
        R"(int g = 0;
        void set() { g = 42; }
        int main() { set(); return 0; })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "g"), "global 'g' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "set"), 1u,
                      "StoreStmt for g = 42");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 9: HEAP — malloc, free
// =========================================================================

    {
        "heap_malloc",
        R"(extern void *malloc(int);
        int main() {
            int *p = malloc(4);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            // malloc should create a HeapObjVar → AddrStmt
            EXPECT_GE(countStmts(pag, SVFStmt::Addr), 1u,
                      "AddrStmt for heap object");
            return true;
        },
        false
    },

    {
        "heap_malloc_free",
        R"(extern void *malloc(int);
        extern void free(void *);
        int main() {
            int *p = malloc(4);
            *p = 42;
            free(p);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 1u,
                      "StoreStmt for *p = 42");
            return true;
        },
        false
    },

// =========================================================================
// SECTION 10: COMPLEX / COMBINED PATTERNS
// =========================================================================

    {
        "complex_struct_array_ptr",
        R"(struct S { int data[5]; };
        int main() {
            struct S s;
            s.data[0] = 42;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // s.data → GEP, then [0] → GEP
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 2u,
                      "GepStmt chain for s.data[0]");
            return true;
        },
        false
    },

    {
        "complex_ptr_to_ptr_deref",
        R"(int main() {
            int x = 1;
            int *p = &x;
            int **pp = &p;
            **pp = 42;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // **pp = 42: load pp → load p_val → store to target
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Load, "main"), 1u,
                      "LoadStmt for deref chain");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 4u,
                      "StoreStmt (x=1, p=&x, pp=&p, **pp=42)");
            return true;
        },
        false
    },

    {
        "complex_func_struct_param",
        R"(struct Point { int x; int y; };
        int get_x(struct Point *p) { return p->x; }
        int main() {
            struct Point pt;
            pt.x = 10;
            pt.y = 20;
            int v = get_x(&pt);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "get_x"), 1u,
                      "GepStmt for p->x in get_x");
            EXPECT_GE(countStmts(pag, SVFStmt::Call), 1u,
                      "CallPE for get_x");
            return true;
        },
        false
    },

    {
        "complex_linked_list_traverse",
        R"(struct Node { int val; struct Node *next; };
        int main() {
            struct Node a;
            struct Node b;
            a.val = 1;
            a.next = &b;
            b.val = 2;
            b.next = 0;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 4u,
                      "GepStmt (a.val, a.next, b.val, b.next)");
            return true;
        },
        false
    },

    {
        "complex_array_of_ptrs",
        R"(int main() {
            int a = 1;
            int b = 2;
            int c = 3;
            int *arr[3];
            arr[0] = &a;
            arr[1] = &b;
            arr[2] = &c;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 4u,
                      "AddrStmt (a, b, c, arr)");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 3u,
                      "GepStmt for arr[0], arr[1], arr[2]");
            return true;
        },
        false
    },

    {
        "complex_void_ptr_cast",
        R"(extern void *malloc(int);
        struct S { int x; };
        int main() {
            struct S *p = (struct S *)malloc(8);
            p->x = 42;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 1u,
                      "GepStmt for p->x");
            return true;
        },
        false
    },

    {
        "complex_multi_level_struct",
        R"(struct A { int x; };
        struct B { struct A a; int y; };
        struct C { struct B b; int z; };
        int main() {
            struct C c;
            c.b.a.x = 1;
            c.b.y = 2;
            c.z = 3;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            // c.b.a.x → 3 GEPs (c→b, b→a, a→x)
            // c.b.y → 2 GEPs (c→b, b→y)
            // c.z → 1 GEP (c→z)
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 6u,
                      "GepStmt for multi-level struct access");
            return true;
        },
        false
    },

    {
        "complex_call_with_expr_args",
        R"(int add(int a, int b) { return a; }
        int main() {
            int x = 1;
            int y = 2;
            int r = add(x + 1, y * 2);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            EXPECT_GE(countStmts(pag, SVFStmt::Call), 1u,
                      "CallPE for add()");
            return true;
        },
        false
    },

    {
        "complex_nested_call",
        R"(int f(int x) { return x; }
        int g(int x) { return x; }
        int main() {
            int r = f(g(1));
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 3u,
                      "3 functions (main, f, g)");
            EXPECT_GE(countCallSites(pag), 2u,
                      "2 call sites (g(1) and f(g_result))");
            return true;
        },
        false  // was XFAIL: nested call decomposition now creates per-call ICFGNodes
    },

    // --- 10.x: Patterns that are currently XFAIL ---

    {
        "complex_switch",
        R"(int main() {
            int x = 1;
            int y;
            switch (x) {
                case 0: y = 0; break;
                case 1: y = 1; break;
                default: y = -1; break;
            }
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "x"), "'x' exists");
            EXPECT(hasVarNamed(pag, "y"), "'y' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 4u,
                      "StoreStmt (x=1, y=0, y=1, y=-1)");
            return true;
        },
        false
    },

    {
        "complex_do_while",
        R"(int main() {
            int i = 0;
            do {
                i = 1;
            } while (i);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "i"), "'i' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Store, "main"), 2u,
                      "StoreStmt (i=0, i=1)");
            return true;
        },
        false
    },

    {
        "complex_typedef",
        R"(typedef int MyInt;
        typedef struct { int x; int y; } Point;
        int main() {
            MyInt a = 1;
            Point p;
            p.x = 2;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "a"), "'a' exists");
            EXPECT(hasVarNamed(pag, "p"), "'p' exists");
            EXPECT_GE(countStmtsInFunc(pag, SVFStmt::Gep, "main"), 1u,
                      "GepStmt for p.x");
            return true;
        },
        false
    },

    {
        "complex_enum",
        R"(enum Color { RED, GREEN, BLUE };
        int main() {
            enum Color c = RED;
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "c"), "'c' exists");
            EXPECT_EQ(countStmtsInFunc(pag, SVFStmt::Addr, "main"), 1u,
                      "AddrStmt for c");
            return true;
        },
        false
    },

    {
        "complex_func_ptr",
        R"(int add(int a, int b) { return a; }
        int main() {
            int (*fp)(int, int) = add;
            int r = fp(1, 2);
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT(hasVarNamed(pag, "fp"), "'fp' exists");
            EXPECT(hasVarNamed(pag, "r"), "'r' exists");
            return true;
        },
        true  // XFAIL: function pointer decl/call may crash
    },

    {
        "complex_static_local",
        R"(int counter() {
            static int n = 0;
            n = n + 1;
            return n;
        }
        int main() {
            counter();
            return 0;
        })",
        [](SVFIR* pag) -> bool {
            EXPECT_GE(countFunctions(pag), 2u, "2 functions");
            return true;
        },
        false
    },
};

// =========================================================================
// Main
// =========================================================================

int main(int argc, char** argv)
{
    std::string filter;
    bool runXfail = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--all") == 0)
            runXfail = true;
        else
            filter = argv[i];
    }

    std::cout << "CTS SVFIR Build Tests\n";
    std::cout << "=====================\n\n";

    for (const auto& tc : allTests)
    {
        if (!filter.empty() && tc.name != filter)
            continue;

        if (tc.xfail && !runXfail && filter.empty())
        {
            std::cout << "  " << tc.name << " ... SKIP (xfail)\n";
            continue;
        }

        std::cout << "  " << tc.name << " ... " << std::flush;
        int result = runTestForked(tc);

        if (tc.xfail)
        {
            if (result == 0)
            {
                std::cout << "XPASS (unexpected pass!)\n";
                ++xpassed;
            }
            else
            {
                std::cout << "XFAIL (expected)\n";
                ++xfailed;
            }
        }
        else
        {
            if (result == 0)
            {
                std::cout << "PASS\n";
                ++passed;
            }
            else if (result == 2)
            {
                std::cout << "CRASH\n";
                ++failed;
            }
            else
            {
                std::cout << "FAIL\n";
                ++failed;
            }
        }
    }

    std::cout << "\n---------------------\n";
    std::cout << "Passed: " << passed
              << "  Failed: " << failed
              << "  XFail: " << xfailed
              << "  XPass: " << xpassed
              << "  Total: " << (passed + failed + xfailed + xpassed) << "\n";

    // Fail only on unexpected failures, not on xfail
    return (failed > 0 || xpassed > 0) ? 1 : 0;
}
