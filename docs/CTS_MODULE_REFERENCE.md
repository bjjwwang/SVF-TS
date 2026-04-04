# CTS Module Reference

## Overview

The CTS (C Tree-Sitter) module provides a Tree-Sitter based C frontend for SVF, bypassing LLVM entirely. It parses C source files directly into SVF's intermediate representation (SVFIR) for static analysis.

```
C source → CTSParser (Tree-Sitter AST)
         → CTSModuleSet (functions, globals, structs, types)
         → CTSICFGBuilder (Interprocedural Control Flow Graph)
         → CTSSVFIRBuilder (SVFIR: Addr/Copy/Load/Store/GEP/Call/Ret/Cmp/Branch stmts)
         → Andersen PTA + Abstract Execution (AE)
```

## Source Layout

```
svf-cts/
├── include/CTS/
│   ├── CTSModule.h          # CTSSourceFile, CTSFunction, CTSGlobalVar, CTSStructDef, CTSModuleSet
│   ├── CTSParser.h          # Tree-Sitter C parser wrapper
│   ├─�� CTSICFGBuilder.h     # ICFG construction from AST
│   ├── CTSSVFIRBuilder.h    # SVFIR statement construction
│   ├── ScopeManager.h       # Variable scope chain
│   └── SSABuilder.h         # SSA form construction (Braun et al.)
├── lib/
│   ├── CTSModule.cpp         (708 lines)  - File parsing, symbol collection
│   ├── CTSParser.cpp         (351 lines)  - Tree-Sitter wrapper utilities
│   ├── CTSICFGBuilder.cpp    (770 lines)  - ICFG builder
│   ├── CTSSVFIRBuilder.cpp   (2542 lines) - Main workhorse: SVFIR construction
│   ├── ScopeManager.cpp      (54 lines)   - Scope push/pop/lookup
│   ├── SSABuilder.cpp        (180 lines)  - SSA phi node insertion
│   └── CTSOverrides.cpp      (33 lines)   - SVF function overrides
└── tools/
    ├── cts-ae/cts-ae.cpp     - Abstract Execution entry point
    └── cts-svf/cts-svf.cpp   - SVF analysis tool (--dump-icfg, --dump-pag)
```

## Classes

### CTSParser

Static utility class wrapping Tree-Sitter C grammar.

**Parsing**
- `parse(source)` → `TSTree*` — parse C source string

**Node type checks** (all `static bool`):
`isFunctionDef`, `isDeclaration`, `isAssignmentExpr`, `isCallExpr`, `isPointerDeref`, `isAddressOf`, `isFieldExpr`, `isSubscriptExpr`, `isReturnStmt`, `isIfStmt`, `isWhileStmt`, `isForStmt`, `isCompoundStmt`, `isExprStmt`, `isInitDeclarator`, `isStructSpecifier`

**Node extraction** (all `static`):
| Method | Returns |
|--------|---------|
| `getNodeText(node, source)` | Text content of AST node |
| `getFunctionName(funcDef, source)` | Function name string |
| `getFunctionBody(funcDef)` | Body TSNode |
| `getFunctionParams(funcDef)` | Vector of parameter TSNodes |
| `getDeclarator(decl)` | First declarator from declaration |
| `getDeclarators(decl)` | All declarators (`int a, b, c;`) |
| `getDeclaratorName(declarator, source)` | Declarator name string |
| `getTypeSpecifier(decl)` | Type specifier TSNode |
| `getInitializer(initDecl)` | Initializer expression TSNode |
| `getArraySize(arrayDecl, source)` | Array dimension (0 if non-constant) |
| `getStructName(structSpec, source)` | Struct name |
| `getStructFields(structSpec)` | Field declaration TSNodes |
| `formatSourceLoc(node, filePath)` | `{"ln": N, "cl": N, "fl": "path"}` |

### CTSModuleSet (Singleton)

Manages all parsed C source files and their symbols.

```cpp
CTSModuleSet* ms = CTSModuleSet::getModuleSet();
ms->buildFromFiles({"main.c", "util.c"});

// Access symbols
CTSFunction* fn = ms->getFunction("main");
CTSGlobalVar* gv = ms->getGlobalVar("count");
CTSStructDef* sd = ms->getStructDef("Point");

// Built-in types
const SVFType* ptr = ms->getPtrType();
const SVFType* i32 = ms->getIntType();
const SVFType* arr = ms->createArrayType(i32, 10);  // [10 x i32]
const SVFType* st = ms->createStructType("Point", {i32, i32}, 8);
```

**Key data structures:**
- `CTSFunction` — name, AST node, params, body, linked FunObjVar
- `CTSGlobalVar` — name, AST node, type node, initializer node
- `CTSStructDef` — name, field names/types/declarators, linked SVFType
- `CTSSourceFile` — file path, source text, TSTree root

**NodeID tracking** (AST node → SVFIR node mapping):
- `setValID(node, file, id)` / `getValID(node, file)` — value nodes
- `setObjID(node, file, id)` / `getObjID(node, file)` — object nodes

### CTSICFGBuilder

Builds ICFG from Tree-Sitter AST. Called before CTSSVFIRBuilder.

```cpp
CTSICFGBuilder builder;
ICFG* icfg = builder.build(moduleSet);
```

**Key methods:**
- `build(CTSModuleSet*)` → `ICFG*` — main entry, creates all ICFG nodes/edges
- `getStmtICFGNode(node, file)` → `ICFGNode*` — look up ICFG node for AST statement
- `setEdgeCondition(edge, condVar, branchVal)` — set branch condition on IntraCFGEdge

**ICFG node types created:**
- `FunEntryICFGNode` / `FunExitICFGNode` — per function
- `CallICFGNode` / `RetICFGNode` — per function call site
- `IntraICFGNode` ��� per statement/expression
- `GlobalICFGNode` — single node for global initializations

**Edge patterns:**
- Internal call: `CallNode --CallCFGEdge--> FunEntry`, `FunExit --RetCFGEdge--> RetNode`
- External call: `CallNode --IntraCFGEdge--> RetNode`
- Branch: `condNode --IntraCFGEdge(cond=0/1)--> thenNode/elseNode`

### CTSSVFIRBuilder

Main workhorse. Builds SVFIR statements and attaches them to ICFG nodes.

```cpp
CTSSVFIRBuilder builder;
builder.build({"test.c"});
// SVFIR (PAG) is now populated and ready for analysis
```

**Node creation:**
| Method | Creates | Use case |
|--------|---------|----------|
| `createLocalVar(name, node, file, type, icfgNode)` | StackObjVar + ValVar + AddrStmt | Local variable declaration |
| `createGlobalVar(name, node, file, type)` | GlobalObjVar + GlobalValVar + AddrStmt | Global variable |
| `createHeapObj(allocSite, file, icfgNode)` | HeapObjVar + ValVar + AddrStmt | malloc/calloc/alloca |
| `createValNode(type, icfgNode)` | ValVar | Temporaries, SSA versions |
| `createConstIntNode(value, icfgNode)` | ConstIntObjVar + ConstIntValVar + AddrStmt | Integer literal |
| `createConstNullNode(icfgNode)` | ConstNullPtrObjVar + ConstNullPtrValVar + AddrStmt | NULL pointer |

**Edge creation:**
| Method | SVFIR Edge | Semantics |
|--------|-----------|-----------|
| `addAddrEdge(src, dst)` | AddrStmt | `dst = &src` |
| `addCopyEdge(src, dst)` | CopyStmt | `dst = src` |
| `addLoadEdge(src, dst)` | LoadStmt | `dst = *src` |
| `addStoreEdge(src, dst, node)` | StoreStmt | `*dst = src` |
| `addGepEdge(src, dst, ap, const)` | GepStmt | `dst = src + offset` |
| `addCallEdge(src, dst, call, entry)` | CallPE | Actual → formal param |
| `addRetEdge(src, dst, call, exit)` | RetPE | Callee ret → caller result |
| `addCmpEdge(op1, op2, dst, pred)` | CmpStmt | `dst = op1 cmp op2` |
| `addBinaryOPEdge(op1, op2, dst, op)` | BinaryOPStmt | `dst = op1 binop op2` |

**Expression evaluation:**
- `getExprValue(node, file)` → NodeID — rvalue (loads from memory if needed)
- `getExprLValue(node, file)` → NodeID — lvalue (address for store targets)
- `getFieldGepNode(fieldExpr, file)` → NodeID — struct field GEP (handles `.` vs `->`)
- `getArrayGepNode(subscriptExpr, file)` → NodeID — array subscript GEP (handles array vs pointer base)

**Type resolution:**
- `resolveType(typeSpec, file)` → `SVFType*` — base type from type specifier node
- `resolveFullType(typeSpec, declarator, file)` → `SVFType*` — full type including pointer/array qualifiers

**Statement processing pipeline:**
```
processFunction → processCompoundStmt → processStatement →
  processDeclaration    — variable declarations
  processExpression     — expression statements
  processAssignment     — x = expr
  processCallExpr       — function calls (internal, external, allocators)
  processReturn         — return statements
  processIfStatement    — if/else with branch conditions
  processWhileStatement — while loops
  processForStatement   — for loops
  processDoWhileStatement
  processSwitchStatement
```

### ScopeManager

Variable scope chain for name resolution.

```cpp
ScopeManager sm;
sm.pushScope();                                    // enter function
sm.declareVar("x", valId, objId, intType);         // declare x
sm.pushScope();                                    // enter block
sm.declareVar("x", valId2, objId2, intType);       // shadow x
auto* info = sm.lookupVar("x");                    // finds inner x
sm.popScope();                                     // exit block
info = sm.lookupVar("x");                          // finds outer x
sm.popScope();                                     // exit function
```

**VarInfo struct:**
```cpp
struct VarInfo {
    NodeID valNode;       // pointer-to-variable (address)
    NodeID objNode;       // memory object
    const SVFType* type;  // declared type (base type for params, e.g. int for int*)
};
```

### SSABuilder

SSA construction using Braun et al. (2013) algorithm. Inserts phi nodes at join points.

```cpp
SSABuilder ssa;
ssa.setCreateNodeFn([&](const std::string&) -> NodeID {
    return createValNode(ptrType, currentICFGNode);
});
ssa.writeVariable("x", block1, val1);   // x = val1 in block1
ssa.sealBlock(block2);                   // all preds of block2 known
NodeID v = ssa.readVariable("x", block2); // may insert phi
auto& phis = ssa.getPendingPhis();       // materialize as PhiStmts
```

## Build & Run

```bash
# Build
./build.sh          # Release build
./build.sh debug    # Debug build

# Or incremental
cmake --build Release-build -j$(nproc) --target cts-ae
cmake --build Release-build -j$(nproc) --target cts-svf

# Run AE analysis
source setup.sh Release
Release-build/bin/cts-ae test.c

# Dump ICFG
Release-build/bin/cts-svf --dump-icfg test.c
# Produces test_icfg.dot with rich stmt info:
#   GepStmt: [Var19 <-- Var14(a)] (offset=0) srcType:[3xi32]
#   StoreStmt: [*Var22 <-- Var16(const=0)]
#   CmpStmt: [Var50 <-- (Var47 pred32 Var49(const=65))]

# Compare with LLVM
clang -c -emit-llvm -g test.c -o test.bc
Release-build/bin/ae test.bc

# Web viewer (108 test cases, CTS vs LLVM side-by-side)
# https://icfg.bjjwwangs.win
```

## Test Suite

108 AE test cases in `TS-TestSuite/src/ae_assert_tests/`. Each uses `svf_assert(condition)` to verify analysis results.

```bash
# Run all tests
pass=0; for f in TS-TestSuite/src/ae_assert_tests/*.c; do
  result=$(timeout 30 Release-build/bin/cts-ae "$f" 2>&1)
  echo "$result" | grep -q "successfully verified" && pass=$((pass+1))
done; echo "PASS: $pass / 108"
```

Current pass rate: **65/108 (60%)**
