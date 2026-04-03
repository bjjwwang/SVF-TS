# CTS Frontend Developer Notes

## Architecture Overview

CTS (C Tree-Sitter) frontend replaces LLVM frontend for building SVFIR from C source directly.

```
C source → Tree-Sitter AST → CTSModuleSet (symbols) → CTSICFGBuilder (ICFG) → CTSSVFIRBuilder (SVFIR)
                                                                                      ↓
                                                                                Andersen PTA → AE
```

Two-pass design (unlike LLVM's single-pass):
- **Pass 1 (ICFG)**: CTSICFGBuilder creates ICFG nodes and edges from AST structure
- **Pass 2 (SVFIR)**: CTSSVFIRBuilder creates SVFIR statements (Addr/Copy/Load/Store/Call/Ret/Cmp/BinaryOP) and attaches them to ICFG nodes

LLVM frontend does both in one pass because each LLVM instruction maps 1:1 to an ICFG node.

## Key Files

| File | Role |
|------|------|
| `svf-cts/lib/CTSICFGBuilder.cpp` | Builds ICFG from AST. Creates IntraICFGNode, CallICFGNode, RetICFGNode, FunEntry/Exit. |
| `svf-cts/lib/CTSSVFIRBuilder.cpp` | Builds SVFIR statements. The main workhorse (~1900 lines). |
| `svf-cts/lib/CTSModule.cpp` | Parses files, collects functions/globals/structs, manages types. |
| `svf-cts/lib/CTSParser.cpp` | Tree-Sitter wrapper. Node type checking, text extraction. |
| `svf-cts/lib/ScopeManager.cpp` | Variable scope stack (alloca model, not SSA). |
| `svf-cts/tools/cts-ae/cts-ae.cpp` | AE analysis entry point. |

## Critical Differences from LLVM Frontend

### 1. `currentICFGNode` Must Be Manually Managed

LLVM: each instruction has a 1:1 mapping to its ICFGNode via `llvmModuleSet()->getICFGNode(inst)`.

CTS: `currentICFGNode` is a mutable field that tracks which ICFG node SVFIR statements attach to. **If you forget to switch it, statements attach to the wrong node.** This was the root cause of multiple bugs:

- **For-loop conditions**: `processForStatement` starts with `currentICFGNode = initNode`. The condition must be evaluated with `currentICFGNode = condNode` (found as initNode's successor). The update expression needs `currentICFGNode = updateNode` (condNode's non-init predecessor).
  
- **Function call return values**: After creating RetPE for internal calls, switch `currentICFGNode = retICFGNode` so that subsequent StoreStmt (e.g., `int x = foo()`) attaches to retNode, not callNode. AE processes SVFStmts BEFORE handleCallSite, so if the store is on callNode, it executes before the return value is available.

### 2. External vs Internal Function Calls — Completely Different ICFG

**Internal functions** (have body): CallICFGNode --CallCFGEdge--> FunEntry → body → FunExit --RetCFGEdge--> RetICFGNode

**External functions** (declaration only): CallICFGNode --IntraCFGEdge--> RetICFGNode

This matches LLVM frontend behavior (`svf-llvm/lib/ICFGBuilder.cpp:289-291`). Without this, AE's state can't flow past external calls because:
- `handleExtCall` doesn't propagate state to RetICFGNode
- RetICFGNode's `mergeStatesFromPredecessors` via RetCFGEdge needs FunExit to have state, but AE never processes external function bodies

### 3. CallPE/RetPE Must Be Registered in THREE Places

LLVM frontend (`SVFIRBuilder.cpp:1777-1792`) does all three:
1. `pag->addToSVFStmtList(icfgNode, edge)` + `icfgNode->addSVFStmt(edge)` — so AE's `handleSVFStatement` loop sees it
2. `CallCFGEdge->addCallPE(callPE)` — for ICFG edge metadata
3. `RetCFGEdge->addRetPE(retPE)` — for ICFG edge metadata

CTS frontend's `addCallEdge`/`addRetEdge` originally only called `pag->addCallPE()` which adds to the PAG graph but NOT to ICFG nodes/edges. Fixed by adding all three registrations.

### 4. External Calls Must NOT Create RetPE

External functions have no body — their RetValPN is never initialized. If you create a RetPE for an external call, `updateStateOnRet` copies from an uninitialized RetValPN, overwriting the value that `handleExtAPI` correctly set.

For external calls: only create result node + `addCallSiteRets`, no RetPE.

### 5. `isPTAEdge()` Gate — ArgValVar Type Matters

`SVFStmt::isPTAEdge()` checks `getSrcNode()->isPointer() && getDstNode()->isPointer()`. Only PTA edges enter Andersen's constraint graph (`KindToPTASVFStmtSetMap`).

If ArgValVar is created with `int` type instead of pointer type, CallPE for pointer parameters becomes invisible to PTA. **Always use `getPtrType()` for ArgValVar** — SVF's PTA needs pointer-typed variables.

Symptom: `CopyProcessed=0, StoreProcessed=0` in Andersen stats when there should be pointer argument passing.

### 6. If-Branch Condition Labeling

ICFG builder creates condNode with two outgoing edges (then-branch and else/merge). SVFIRBuilder sets conditions on these edges via `setEdgeCondition(edge, condVar, 0/1)`.

Pitfalls:
- **Must have ≥2 distinct destination nodes** — if both edges go to mergeNode (empty then-body, no else), there's no real branch. Don't set conditions.
- **Must find thenICFGNode** — if `getICFGNode(consequence)` returns null (empty compound_statement), search children of the compound_statement.
- **If thenICFGNode not found, leave edges unconditional** — setting all edges to condition=0 makes the entire path infeasible.

### 7. `&&` / `||` — Not Bitwise, Short-Circuit

`a > 5 && a < 7` must NOT be modeled as `BinaryOPStmt::And(cmp1, cmp2)`. AE's `isBranchFeasible` expects condVar's in-edge to be a CmpStmt, not BinaryOPStmt. With BinaryOPStmt it falls back to `isSwitchBranchFeasible` which can't do constraint narrowing.

Current approximation: return rhs for `&&`, lhs for `||`. This gives single-constraint narrowing. Full fix: generate nested if branches in ICFG (matching Clang's short-circuit lowering).

## AE Execution Flow for Function Calls

For `foo(&a)` where foo does `*p = 1`:

```
main's WTO:
  handleICFGNode(CallICFGNode)
    SVFStmts executed (Addr, Store for declarations)
    handleCallSite → handleFunCall:
      handleFunction(fooEntry):
        handleICFGNode(FunEntry): merge from CallCFGEdge → CallPE copies &a → p_formal
        handleICFGNode(fooBody): process arg-to-local store, then *p=1 (store 1 to a_obj via PTA)
        handleICFGNode(FunExit): merge from body → has a_obj=1
      abstractTrace[retNode] = abstractTrace[callNode]  ← temporary, gets overwritten

  handleICFGNode(RetICFGNode)
    mergeStatesFromPredecessors: merge from FunExit via RetCFGEdge → gets a_obj=1
    SVFStmts: RetPE copies callee's return value to result node

  handleICFGNode(next statement)
    merge from retNode → has a_obj=1
    load a → read a_obj → should get 1
```

**Current status**: PTA correctly resolves p→a_obj (CopyProcessed=3, StoreProcessed=1). But AE's abstract state still doesn't carry a_obj=1 through the call chain. Suspected issue: the store `*p = 1` inside foo requires the address domain to propagate through CallPE → Store(arg to local) → Load(local) → Store(*p). If any step drops the address information, the chain breaks.

## How to Run Tests

```bash
# Single test
./Release-build/bin/cts-ae TS-TestSuite/src/ae_assert_tests/BASIC_assign_0-0.c

# All tests with pass rate
pass=0; total=0; for f in TS-TestSuite/src/ae_assert_tests/*.c; do
  total=$((total+1))
  timeout 30 ./Release-build/bin/cts-ae "$f" 2>&1; [ $? -eq 0 ] && pass=$((pass+1))
done; echo "PASS:$pass / $total"

# Categorize failures
# NC = "not been checked" (svf_assert unreachable)
# VF = "cannot be verified" (svf_assert reached but wrong value)  
# CR = crash (assertion failure in SVF internals)

# Compare with LLVM pipeline
export PATH=$PWD/llvm-18.1.0.obj/bin:$PATH
clang -c -emit-llvm -g test.c -o test.bc
./Release-build/bin/ae test.bc

# Dump constraint graph (for PTA debugging)
./Release-build/bin/cts-ae -print-constraint-graph=true test.c
```

## How to Debug

### Check if ICFG structure is correct
```bash
./Release-build/bin/cts-svf --dump-icfg test.c
# Look at the .dot file for edge connectivity
```
Note: cts-svf --dump-icfg currently crashes after RetPE registration fix (DOT output issue). Use cts-ae instead.

### Check Andersen PTA stats
```bash
./Release-build/bin/cts-ae test.c 2>&1 | grep -E "CopyProcessed|StoreProcessed|LoadProcessed|AddrProcessed"
```
- `CopyProcessed=0` with pointer args → CallPE not in constraint graph → check ArgValVar type
- `StoreProcessed=0` with pointer stores → pointer target unknown → check PTA resolution

### Check which SVFStmts are on each ICFGNode
Add temporary debug in `handleSVFStatement` or use `--dump-stmts` in cts-svf.

### Key comparison points with LLVM
| Metric | LLVM typical | CTS if wrong |
|--------|-------------|--------------|
| CopyProcessed | >0 | 0 (CallPE type issue) |
| StoreProcessed | >0 | 0 (PTA can't resolve pointer) |
| CallsNum | matches # of internal calls | lower (external calls wrongly creating CallPE) |

## Common Gotchas

1. **Tree-Sitter doesn't preprocess** — `#define`, `#include <system>`, `true`/`false` from stdbool.h are all raw text. Handle `true`/`false`/`NULL` as identifier special cases.

2. **Tree-Sitter's `parenthesized_expression`** — if-conditions are wrapped in `(...)`. Always strip: `if (strcmp(ts_node_type(cond), "parenthesized_expression") == 0) innerCond = ts_node_named_child(cond, 0);`

3. **Type specifier vs declarator** — `int* p` has type specifier `int` and pointer_declarator `*p`. `resolveType(getTypeSpecifier(...))` returns `int`, not `int*`. Use `resolveFullType` or just `getPtrType()` when pointer type is needed.

4. **`getExprValue` vs `getExprLValue`** — `getExprValue` loads the value (creates LoadStmt). `getExprLValue` returns the address (for store targets). For `x = expr`, LHS uses `getExprLValue`, RHS uses `getExprValue`.

5. **`blackHoleNode`** — sentinel value for "unknown/unresolvable". Check `!= blackHoleNode` before using a NodeID.

6. **Multiple calls in one statement** — `f(g(x))` has two calls. ICFG builder finds ALL calls bottom-up and chains them: CallNode_g → RetNode_g → CallNode_f → RetNode_f.

7. **`addStoreEdge` third parameter** — explicitly pass the ICFGNode for the store. Don't rely on `currentICFGNode` being correct after a function call.
