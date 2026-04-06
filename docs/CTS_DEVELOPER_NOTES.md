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
| `svf-cts/lib/CTSSVFIRBuilder.cpp` | Builds SVFIR statements. The main workhorse (~2100 lines). |
| `svf-cts/lib/CTSModule.cpp` | Parses files, collects functions/globals/structs, manages types. |
| `svf-cts/lib/CTSParser.cpp` | Tree-Sitter wrapper. Node type checking, text extraction. |
| `svf-cts/lib/ScopeManager.cpp` | Variable scope stack (alloca model, not SSA). |
| `svf-cts/tools/cts-ae/cts-ae.cpp` | AE analysis entry point. |
| `svf-cts/tools/cts-svf/cts-svf.cpp` | SVF analysis tool with --dump-icfg/--dump-pag. |

## Critical Differences from LLVM Frontend

### 1. `currentICFGNode` Must Be Manually Managed

LLVM: each instruction has a 1:1 mapping to its ICFGNode via `llvmModuleSet()->getICFGNode(inst)`.

CTS: `currentICFGNode` is a mutable field that tracks which ICFG node SVFIR statements attach to. **If you forget to switch it, statements attach to the wrong node.** This was the root cause of multiple bugs:

- **For-loop conditions**: `processForStatement` starts with `currentICFGNode = initNode`. The condition must be evaluated with `currentICFGNode = condNode` (found as initNode's successor). The update expression needs `currentICFGNode = updateNode` (condNode's non-init predecessor).
  
- **Function call return values**: After creating RetPE for internal calls, switch `currentICFGNode = retICFGNode` so that subsequent StoreStmt (e.g., `int x = foo()`) attaches to retNode, not callNode. AE processes SVFStmts BEFORE handleCallSite, so if the store is on callNode, it executes before the return value is available.

- **Function parameter processing**: Set `currentICFGNode = FunEntryICFGNode` before the parameter loop, so AddrStmt/StoreStmt for parameters register on the entry node.

- **Global variable init**: Set `currentICFGNode = GlobalICFGNode` before processing global initializers, so GepStmt/StoreStmt register on the global node.

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

### 8. Allocator Calls (malloc/calloc/alloca) Must Create HeapObjVar

LLVM: `alloca` is a dedicated instruction (`visitAllocaInst`), `malloc` is identified via ExtAPI annotation `ALLOC_HEAP_RET`. Both create ObjVar + AddrStmt so the return pointer has a concrete address in the abstract domain.

CTS: Tree-Sitter sees `malloc(...)` and `alloca(...)` as plain function calls. Without special handling, the return value is an empty ValVar with no address — subsequent GEP/Store through the pointer have no base object to operate on.

**Fix**: In `processCallExpr`, detect allocator names (`malloc`, `calloc`, `alloca`, `ALLOCA`, `realloc`, `strdup`, etc.) and call `createHeapObj()` which creates HeapObjVar + AddrStmt. Note: Tree-Sitter doesn't expand macros, so also match macro names like `ALLOCA`, `MALLOC`.

### 9. Pointer Subscript vs Array Subscript — Must Distinguish

For `base[idx]`, the GEP base depends on `base`'s type:

| Declaration | VarInfo::type | GEP base | Why |
|-------------|---------------|----------|-----|
| `int arr[3]` | SVFArrayType | `getExprLValue(arr)` | Array address IS the pointer to first element |
| `int* ptr` (param) | int (base type) | `getExprValue(ptr)` (Load first) | `&ptr` ≠ `ptr`; must load pointer value |
| `a[i][j]` inner | — | `getExprLValue(a[i])` (prior GEP result) | No extra load; previous GEP already gives pointer |

**Key rule**: Only the first subscript on a non-array identifier needs a Load. Nested subscripts and array-typed variables don't.

For struct field access, same principle: `s.field` → GEP on lvalue; `p->field` → Load pointer first, then GEP. Detect via tree-sitter `->` operator child node.

### 10. Variant GEP Type Pair Must Be Pointer Type

In `getElementIndex()` (AE), the type in `IdxOperandPair` determines the offset calculation:
- **SVFPointerType** → `elemNum * idx` (correct for array element access)
- **Other types** → `getFlattenedElemIdx(type, idx)` (for struct field flattening)

LLVM always passes pointer type for array element GEPs. CTS must do the same: `ap.addOffsetVarAndGepTypePair(indexVar, moduleSet->getPtrType())` for variant (non-constant) array subscripts.

Passing the element type (e.g., `i32`) causes `getFlattenedElemIdxVec()` to return `{0}`, forcing all variable indices to 0.

### 11. ExtAPI Annotations Must Be Registered for CTS Functions

LLVM frontend loads `extapi.bc` and extracts function annotations (e.g., `ALLOC_HEAP_RET`, `MEMCPY`) from LLVM metadata. CTS doesn't load extapi.bc.

**Fix**: In `createFunctionObjects()`, after creating FunObjVar for each external function, call `ExtAPI::getExtAPI()->setExtFuncAnnotations(funObj, {...})` with the appropriate annotations. (`CTSSVFIRBuilder` is added as `friend` of `ExtAPI`.)

Currently registers: heap allocators (malloc, calloc, alloca, etc.). TODO: register MEMCPY, MEMSET, STRCPY annotations for string operation modeling.

### 12. Array Initializer Lists Must Generate Per-Element GEP+Store

`int a[3] = {1, 2, 3}` — the initializer_list must generate:
```
GepStmt(a, offset=0) → StoreStmt(1)
GepStmt(a, offset=1) → StoreStmt(2)
GepStmt(a, offset=2) → StoreStmt(3)
```

Previously: `getExprValue(initializer_list)` only returned the first element. Fixed in both `processDeclaration` (local arrays) and `processGlobalVars` (global arrays).

For globals: `currentICFGNode` must be set to `GlobalICFGNode` before processing initializers.

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

## Remaining Failures (65/108 pass, 43 fail)

| Root Cause | Count | Cases |
|------------|-------|-------|
| string ops not modeled (strcpy/memcpy/sprintf) | 11 | BASIC_ptr_s32_2, BUF_OVERFLOW_47, CVE-2020-13598, CVE-2020-29203, CVE-2021-39602, CVE-2021-45341, CVE-2022-23850, CVE-2022-27239, CVE-2022-34913, cwe126_char, CWE127_har, INTERVAL_test_36-1 |
| foo(&a) pointer param write-back | 9 | CVE-2019-19847, CVE-2021-44975, CVE-2022-26129, CVE-2022-29023, CVE-2022-34835, CVE-2022-34918, INTERVAL_test_13, INTERVAL_test_19, INTERVAL_test_49 |
| cross-function array/pointer GEP | 6 | BASIC_array_func_0/3/4/6, BASIC_array_int, BASIC_struct_array |
| function pointer indirect call | 3 | BASIC_ptr_func_1/4/6 |
| scanf/rand not modeled | 3 | INTERVAL_test_2, INTERVAL_test_20, INTERVAL_test_9 |
| nested if narrowing | 2 | BASIC_br_nd_malloc, BASIC_nullptr_def |
| && condition narrowing | 1 | INTERVAL_test_14 |
| uninitialized variable / goto loop | 2 | INTERVAL_test_58, INTERVAL_test_64 |
| switch statement | 1 | BASIC_switch (missing extern decl for svf_assert) |
| cast/trunc | 1 | CAST_trunc |
| global array init (AE doesn't process GlobalICFGNode GEP) | 1 | BASIC_arraycopy3 |
| missing extern declaration | 1 | BASIC_arraycopy1 |
| foo(&a) + switch | 1 | INTERVAL_test_12 |

## How to Run Tests

```bash
# Single test
./Release-build/bin/cts-ae TS-TestSuite/src/ae_assert_tests/BASIC_assign_0-0.c

# All tests with pass rate
pass=0; total=0; for f in TS-TestSuite/src/ae_assert_tests/*.c; do
  total=$((total+1))
  result=$(timeout 30 ./Release-build/bin/cts-ae "$f" 2>&1)
  echo "$result" | grep -q "successfully verified" && pass=$((pass+1))
done; echo "PASS:$pass / $total"

# Dump ICFG for debugging
./Release-build/bin/cts-svf --dump-icfg test.c

# Compare with LLVM pipeline
export PATH=$PWD/llvm-18.1.0.obj/bin:$PATH
clang -c -emit-llvm -g test.c -o test.bc
./Release-build/bin/ae test.bc
```

## How to Debug

### Check ICFG structure
```bash
./Release-build/bin/cts-svf --dump-icfg test.c
# Statements now show rich info: variable names, constant values, GEP offsets
# e.g. GepStmt: [Var19 <-- Var14(a)] (offset=0) srcType:[3xi32]
#      StoreStmt: [*Var22 <-- Var16(const=0)]
#      CmpStmt: [Var50 <-- (Var47 pred32 Var49(const=65))]
```

### Check Andersen PTA stats
```bash
./Release-build/bin/cts-ae test.c 2>&1 | grep -E "CopyProcessed|StoreProcessed|LoadProcessed|AddrProcessed"
```
- `CopyProcessed=0` with pointer args → CallPE not in constraint graph → check ArgValVar type
- `StoreProcessed=0` with pointer stores → pointer target unknown → check PTA resolution

### Web ICFG viewer
https://icfg.bjjwwangs.win — side-by-side CTS vs LLVM ICFG for all 108 test cases, with source code and independent pan/zoom.

## Common Gotchas

1. **Tree-Sitter doesn't preprocess** — `#define`, `#include <system>`, `true`/`false` from stdbool.h are all raw text. Handle `true`/`false`/`NULL` as identifier special cases. Macro names like `ALLOCA` won't expand to `alloca` — match both.

2. **Tree-Sitter's `parenthesized_expression`** — if-conditions are wrapped in `(...)`. Always strip: `if (strcmp(ts_node_type(cond), "parenthesized_expression") == 0) innerCond = ts_node_named_child(cond, 0);`

3. **Type specifier vs declarator** — `int* p` has type specifier `int` and pointer_declarator `*p`. `resolveType(getTypeSpecifier(...))` returns `int`, not `int*`. Parameter types stored in VarInfo are base types (e.g. `int` for `int* arr`), not pointer types.

4. **`getExprValue` vs `getExprLValue`** — `getExprValue` loads the value (creates LoadStmt). `getExprLValue` returns the address (for store targets). For `x = expr`, LHS uses `getExprLValue`, RHS uses `getExprValue`.

5. **`blackHoleNode`** — sentinel value for "unknown/unresolvable". Check `!= blackHoleNode` before using a NodeID.

6. **Multiple calls in one statement** — `f(g(x))` has two calls. ICFG builder finds ALL calls bottom-up and chains them: CallNode_g → RetNode_g → CallNode_f → RetNode_f.

7. **`addStoreEdge` third parameter** — explicitly pass the ICFGNode for the store. Don't rely on `currentICFGNode` being correct after a function call.

8. **CallPE/RetPE `setValue()`** — Must call `callPE->setValue(pag->getGNode(dst))` after creation. Without it, `getValue()` returns null causing segfault in DOT dump toString().

9. **AccessPath srcType for GepStmt** — The `gepSrcPointeeType` in AccessPath is what shows as `srcType:` in the dump. For array subscript, pass the element type (e.g. `i32` for `int[]`). For struct field, pass the struct type. Pass null/omit for pointer-typed GEPs (the pointer type is inferred).

## Remaining Work Priority (as of 2026-04-06)

### Current Pass Rates
| Test Suite | CTS | LLVM |
|-----------|-----|------|
| ae_assert_tests | 72/108 (67%) | ~100% |
| ae_overflow_tests | 32/63 (51%) | 54/63 (86%) |

### Priority Table

| Priority | Task | Impact | Difficulty | Details |
|----------|------|--------|------------|---------|
| P0 | For-loop in-loop narrowing | overflow +14 | Hard | BranchStmt now registered on ICFGNode. After-loop narrowing works. In-loop narrowing fails because AE's WTO cycle widen doesn't narrow back inside the body. Need to compare LLVM's WTO cycle structure vs CTS's — may need PhiStmt at loop header for the induction variable. |
| P1 | Preprocessed header interference | overflow +several | Medium | Simplified test cases pass but full test cases with std_testcase.h fail. Likely typedef struct, global variable ObjTypeInfo, or function name collision. |
| P2 | Pointer arithmetic `a + 9` | assert +1 | Low | `int *p = a + 9` → GepStmt(a, offset=9). CTS binary_expression for pointer+int not modeled as GEP. |
| P3 | Cast trunc `(int8_t)256` | assert +1 | Low | Need truncation modeling in CopyStmt or UnaryOPStmt. |
| P4 | && double narrowing | assert +1 | Medium | Needs nested condNode ICFG structure (partially implemented). |
| P5 | Function pointer indirect call | assert +3 | High | Need Andersen PTA indirect call resolution → dynamic CallCFGEdge. |
| P6 | wchar_t strlen/wcslen | overflow +6 | Medium | AE strlen handler doesn't handle wchar_t. Need wcslen → wchar_t byte-level copy. |

### Key Discoveries This Session
1. **BranchStmt must be on ICFGNode stmt list** — `addBranchEdge` was missing `addToSVFStmtList + addSVFStmt`. Without this, AE's `isBranchFeasible` never fires.
2. **recordStmtNode overwrite bug** — expression_statement and its first call_expression child share the same start_byte. Outer call's args were registered on inner call's CallICFGNode.
3. **Array name decay** — array identifier in getExprValue must return address directly (no Load) to match C array-to-pointer decay semantics.
4. **sizeof hardcoded to 8** — was causing all buffer size calculations to be wrong.
5. **alloca size not tracked** — ObjTypeInfo had no byte size for heap objects.
6. **String literal init** — `char s[] = "ABC"` needs per-character GEP+Store.
7. **Constant expression array size** — `char s[10+1]` fails stoul, needs tryEvalConstExpr.
