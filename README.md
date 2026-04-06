<img src="./docs/images/svf_logo_2.png" width="15%"><img src="./docs/images/svf_logo_3.png" width="85%">


## SVF-TS Progress: Tree-Sitter C Frontend (Phase 1 — AE)

**ICFG Viewer**: [icfg.bjjwwangs.win](https://icfg.bjjwwangs.win)

| Test Suite | CTS Pass | Total | CTS Rate | LLVM Rate |
|-----------|----------|-------|----------|-----------|
| ae\_assert\_tests | 72 | 108 | **67%** | ~100% |
| ae\_overflow\_tests | 29 | 63 | **46%** | 86% |

### Detailed Results (72 PASS / 14 VF / 22 NC)

| Status | Cases |
|--------|-------|
| **PASS (72)** | BASIC\_assign\_0/1/2/3, BASIC\_bi\_add/div/mix/mul, BASIC\_br\_false/true/nd\_0/1/2, BASIC\_funcall\_ref\_0/1/2, BASIC\_ptr\_assign/call1/call2/func\_0, BASIC\_struct\_assign, BASIC\_array\_{2d,struct,func\_0/4/6,varIdx\_1,copy2}, BASIC\_switch/01-10, BASIC\_test\_11, CAST\_fptosi/fptoui/fptrunc/sext/sitofp/uitofp/zext, cwe121\_{char,int,int64,struct}\_alloc, cwe190\_{int\_max,char\_fscanf}, INTERVAL\_test\_6/8/9/11/12/13/15/16, LOOP\_for01/for\_call/for\_inc/while01, wto\_assert\_01-05 |
| **VF (14)** | BASIC\_arraycopy1/3, BASIC\_array\_int, BASIC\_nullptr\_def, BASIC\_ptr\_func\_1/4/6, CAST\_trunc, cwe126\_char, CWE127\_har, INTERVAL\_test\_2/14/19/20/36-1/49 |
| **NC (22)** | BASIC\_array\_func\_3, BASIC\_br\_nd\_malloc, BASIC\_ptr\_s32\_2, BASIC\_struct\_array, BUF\_OVERFLOW\_47, INTERVAL\_test\_58/64, 13× CVE (no svf\_assert) |

### Remaining Failures by Root Cause (36)

| Root Cause | Count | Cases | Difficulty |
|------------|-------|-------|------------|
| CVE/BUF (no svf\_assert — buffer overflow detection, not assertion verification) | 14 | all CVE + BUF\_OVERFLOW | N/A (LLVM also NC) |
| Function pointer indirect call (`q=c; q(&y)`) | 3 | ptr\_func\_1/4/6 | Need PTA indirect call resolution |
| alloca + memcpy/strcpy chain | 3 | cwe126, CWE127, INTERVAL\_36-1 | ExtAPI registered but byte-level copy incomplete |
| switch inside callee | 1 | INTERVAL\_test\_19 | BranchStmt added, narrowing needs work |
| && double narrowing | 1 | INTERVAL\_test\_14 | Needs nested condNode |
| goto loop (WTO) | 1 | INTERVAL\_test\_49 | goto edge added, WTO convergence issue |
| scanf/rand | 2 | INTERVAL\_test\_2/20 | External return value modeling |
| cast trunc (int64→int8) | 1 | CAST\_trunc | Need truncation BinaryOPStmt |
| global array init | 1 | BASIC\_arraycopy3 | GlobalICFGNode GEP exists, AE skips |
| typedef struct + alloca combo | 1 | BASIC\_nullptr\_def (regression) | cpp expanded ALLOCA, typedef struct interaction |
| pointer arithmetic (`a+9`) | 1 | BASIC\_array\_int | `int *p = a + 9` pointer add not modeled |
| missing svf\_assert decl | 1 | BASIC\_arraycopy1 | Source file lacks `extern void svf_assert` |
| struct + typedef + dynamic index | 2 | array\_func\_3, struct\_array | typedef struct field access |
| uninitialized + complex control flow | 2 | INTERVAL\_58/64 | do-while + for nesting |
| malloc + branch + deref | 1 | br\_nd\_malloc | malloc + if branch + pointer deref |
| alloca + sizeof + no assert | 1 | ptr\_s32\_2 | No svf\_assert in source |

### Overflow Detection Results (29 PASS / 14 FP / 6 BOT / 14 NC)

| Status | Count | Cases |
|--------|-------|-------|
| **PASS (29)** | memcpy/memmove/snprintf/cpy/strcat types | CWE121\_CWE129\_rand, CWE121\_CWE131\_memcpy, CWE121\_CWE805\_{char\_alloca\_memcpy, char\_declare\_memmove, char\_declare\_ncpy, int64\_t\_alloca\_memmove, int64\_t\_declare\_memcpy, int64\_t\_declare\_memmove, int\_alloca\_memcpy, wchar\_t\_alloca\_loop/snprintf, wchar\_t\_declare\_loop/memcpy/memmove/snprintf}, CWE121\_{dest\_wchar\_t\_alloca\_cpy, src\_char\_alloca\_cpy, src\_wchar\_t\_declare\_cpy}, CWE122\_{int64\_t\_memcpy/memmove, int\_memmove, wchar\_t\_memmove/snprintf, dest\_char/wchar\_t\_cpy, CWE131\_memcpy}, CWE126\_CWE129\_fgets, ExtAPI\_strcat\_01/02 |
| **FP (14)** | SAFE\_BUFACCESS false positive (loop widen) | CWE121\_{CWE129\_fgets, CWE131\_loop, CWE193\_char\_alloca\_cpy, CWE193\_wchar\_t\_declare\_cpy, CWE805\_int64\_t\_declare\_loop, CWE805\_int\_alloca\_loop, CWE805\_struct\_alloca/declare\_memmove}, CWE122\_{CWE805\_struct\_memcpy, CWE131\_loop}, CWE126\_{char\_declare\_loop, malloc\_char/wchar\_t\_loop, wchar\_t\_declare\_loop} |
| **BOT (6)** | SAFE\_BUFACCESS size is bottom | CWE121\_CWE805\_char\_alloca\_loop, CWE121\_CWE806\_{char\_declare\_loop, wchar\_t\_declare\_memcpy}, CWE122\_CWE806\_wchar\_t\_memmove, CWE126\_{malloc\_wchar\_t\_memmove, wchar\_t\_declare\_memmove} |
| **NC (14)** | AE doesn't reach stub calls | CWE121\_{CWE129\_listen\_socket, CWE193\_char\_alloca\_memcpy/memmove, CWE193\_char\_declare\_memmove, CWE806\_char\_declare\_ncpy}, CWE122\_{CWE806\_char\_memmove, src\_char\_cpy}, CWE126\_{char\_alloca/declare\_memcpy, CWE129\_fscanf/listen\_socket, malloc\_char\_memmove}, ExtAPI\_strcat\_03/04 |

### Overflow Failure Root Causes (34)

| Root Cause | Count | Difficulty |
|------------|-------|------------|
| Loop widen false positive: `SAFE_BUFACCESS` inside for-loop, AE widens loop var to \[0,+INF\], access size upper bound exceeds alloca size | 14 | AE loop analysis precision (not CTS issue) |
| strlen/wcslen return value bottom: size argument `strlen(s)*sizeof(char)` not evaluable | 6 | Need strlen ExtAPI handler to work with CTS FunObjVar |
| Control flow unreachable: AE doesn't traverse into callee or listen\_socket/fscanf blocks | 14 | Mixed: network API stubs, strlen-dependent branches, char literal init |

### Key Fixes Applied

1. **External call ICFG edges** — intra-edge instead of call/ret for external functions
2. **If-branch condition labeling** — fixed empty-body and unresolvable then-node cases
3. **While/for loop branch conditions** — switch `currentICFGNode` to condNode
4. **CallPE/RetPE registration** — register on ICFGNode stmt list + CallCFGEdge/RetCFGEdge
5. **Return value on RetICFGNode** — StoreStmt after RetPE, not before
6. **External call return** — no RetPE for externals; handleExtAPI sets actualRet
7. **ArgValVar type** — use pointer type so CallPE enters Andersen's constraint graph
8. **`&&`/`||` modeling** — return CmpStmt result instead of BinaryOPStmt::And
9. **Constants** — `true`/`false`/`NULL`, char literals, hex/octal, update expressions
10. **Parameter processing** — currentICFGNode = FunEntry before param AddrStmt/StoreStmt
11. **Allocator modeling** — malloc/calloc/alloca/ALLOCA create HeapObjVar + AddrStmt
12. **Pointer subscript** — `int* p; p[i]` loads pointer before GEP; `->` vs `.` distinguished
13. **Array initializer list** — `{1,2,3}` generates per-element GEP+Store (local + global)
14. **ExtAPI annotations** — memcpy/strcpy/memset registered via `registerKnownExternalCalls()`
15. **Variant GEP type** — pass ptr type for variable-index subscript (matches LLVM getElementIndex)
16. **ICFG stmt display** — variable names, const values, GEP offsets, Load/Store dereference marks
17. **Array name decay** — array identifier in expression returns address directly (no Load), matching C array-to-pointer decay
18. **C preprocessor** — `cpp -P` runs before tree-sitter to expand macros and process `#include`
19. **goto/label ICFG edges** — labeled\_statement records label→node map, goto\_statement creates deferred edge
20. **Switch BranchStmt** — switch generates BranchStmt with case values for AE's isSwitchBranchFeasible
21. **sizeof resolution** — `sizeof(int)=4`, `sizeof(char)=1`, `sizeof(long)=8`, struct lookup. Was hardcoded 8.
22. **Alloca/malloc size tracking** — `createHeapObj` sets ObjTypeInfo byte size from `tryEvalConstExpr(sizeArg)`
23. **Stack object byte size** — `createLocalVar` sets ObjTypeInfo byte size from SVFType (e.g. `int[10]`→40)
24. **Stub headers** — `svf-cts/include/CTS/stubs/` provides empty system headers for `-nostdinc` preprocessor
25. **Dynamic FunObjVar** — `getOrCreateExtFunObjVar` creates FunObjVar + ExtAPI annotations for undeclared external calls
26. **Overflow stub functions** — `UNSAFE_BUFACCESS`/`SAFE_BUFACCESS` registered as known externals for BufOverflowDetector

---

## News
* <b>SVF now supports new [build system](https://github.com/SVF-tools/SVF/pull/1703) (Thank [Johannes](https://github.com/Johanmyst) for his help!). </b>
* <b> [SVF-Python](https://github.com/SVF-tools/SVF-Python) is now available, enabling developers to write static analyzers in Python by leveraging the SVF library (Contributed by [Jiawei Wang](https://github.com/bjjwwang)). </b>
* <b>New course [Software Security Analysis](https://github.com/SVF-tools/Software-Security-Analysis) for learning code analysis and verification with SVF for fun and expertise! </b>
* <b>SVF now supports LLVM-16.0.0 with opaque pointers (Contributed by [Xiao Cheng](https://github.com/jumormt)). </b>
* <b>Modernize SVF's CMake (Contributed by [Johannes](https://github.com/Johanmyst)). </b>
* <b>SVF now supports LLVM-13.0.0 (Thank [Shengjie Xu](https://github.com/xushengj) for his help!). </b>
* <b>[Object clustering](https://github.com/SVF-tools/SVF/wiki/Object-Clustering) published in our [OOPSLA paper](https://yuleisui.github.io/publications/oopsla21.pdf) is now available in SVF </b>
* <b>[Hash-Consed Points-To Sets](https://github.com/SVF-tools/SVF/wiki/Hash-Consed-Points-To-Sets) published in our [SAS paper](https://yuleisui.github.io/publications/sas21.pdf) is now available in SVF </b>
* <b> Learning or teaching Software Analysis? Check out [SVF-Teaching](https://github.com/SVF-tools/SVF-Teaching)! </b>
* <b>SVF now supports LLVM-12.0.0 (Thank [Xiyu Yang](https://github.com/sherlly/) for her help!). </b>
* <b>[VSFS](https://github.com/SVF-tools/SVF/wiki/VSFS) published in our [CGO paper](https://yuleisui.github.io/publications/cgo21.pdf) is now available in SVF </b>
* <b>[TypeClone](https://github.com/SVF-tools/SVF/wiki/TypeClone) published in our [ECOOP paper](https://yuleisui.github.io/publications/ecoop20.pdf) is now available in SVF </b>
* <b>SVF now uses a single script for its build. Just type [`source ./build.sh`](https://github.com/SVF-tools/SVF/blob/master/build.sh) in your terminal, that's it!</b>
* <b>SVF now supports LLVM-10.0.0! </b>
* <b>We thank [bsauce](https://github.com/bsauce) for writing a user manual of SVF ([link1](https://www.jianshu.com/p/068a08ec749c) and [link2](https://www.jianshu.com/p/777c30d4240e)) in Chinese </b>
* <b>SVF now supports LLVM-9.0.0 (Thank [Byoungyoung Lee](https://github.com/SVF-tools/SVF/issues/142) for his help!). </b>
* <b>SVF now supports a set of [field-sensitive pointer analyses](https://yuleisui.github.io/publications/sas2019a.pdf). </b>
* <b>[Use SVF as an external lib](https://github.com/SVF-tools/SVF-example) for your own project (Contributed by [Hongxu Chen](https://github.com/HongxuChen)). </b>
* <b>SVF now supports LLVM-7.0.0. </b>
* <b>SVF now supports Docker. [Try SVF in Docker](https://github.com/SVF-tools/SVF/wiki/Try-SVF-in-Docker)! </b>
* <b>SVF now supports [LLVM-6.0.0](https://github.com/svf-tools/SVF/pull/38) (Contributed by [Jack Anthony](https://github.com/jackanth)). </b>
* <b>SVF now supports [LLVM-4.0.0](https://github.com/svf-tools/SVF/pull/23) (Contributed by Jared Carlson. Thank [Jared](https://github.com/jcarlson23) and [Will](https://github.com/dtzWill) for their in-depth [discussions](https://github.com/svf-tools/SVF/pull/18) about updating SVF!) </b>
* <b>SVF now supports analysis for C++ programs.</b>
<br />

## Documentation

<br />

<b>SVF</b> is a static value-flow analysis tool for LLVM-based languages. <b>SVF</b> ([CC'16](https://yuleisui.github.io/publications/cc16.pdf)) is able to perform
* [AE](https://github.com/SVF-tools/SVF/tree/master/svf/include/AE) (<b>abstract execution</b>): cross-domain execution ([ICSE'24](https://yuleisui.github.io/publications/icse24a.pdf)), recursion analysis ([ECOOP'25](https://yuleisui.github.io/publications/ecoop25.pdf)) typestate analysis ([FSE'24](https://yuleisui.github.io/publications/fse24a.pdf));
* [WPA](https://github.com/SVF-tools/SVF/tree/master/svf/include/WPA) (<b>whole program analysis</b>): field-sensitive ([SAS'19](https://yuleisui.github.io/publications/sas2019a.pdf)), flow-sensitive ([CGO'21](https://yuleisui.github.io/publications/cgo21.pdf), [OOPSLA'21](https://yuleisui.github.io/publications/oopsla21.pdf)) analysis;
* [DDA](https://github.com/SVF-tools/SVF/tree/master/svf/include/DDA) (<b>demand-driven analysis</b>): flow-sensitive, context-sensitive points-to analysis ([FSE'16](https://yuleisui.github.io/publications/fse16.pdf), [TSE'18](https://yuleisui.github.io/publications/tse18.pdf));
* [MSSA](https://github.com/SVF-tools/SVF/tree/master/svf/include/MSSA) (<b>memory SSA form construction</b>): memory regions, side-effects, SSA form ([JSS'18](https://yuleisui.github.io/publications/jss18.pdf));
* [SABER](https://github.com/SVF-tools/SVF/tree/master/svf/include/SABER) (<b>memory error checking</b>): memory leaks and double-frees ([ISSTA'12](https://yuleisui.github.io/publications/issta12.pdf), [TSE'14](https://yuleisui.github.io/publications/tse14.pdf), [ICSE'18](https://yuleisui.github.io/publications/icse18a.pdf));
* [MTA](https://github.com/SVF-tools/SVF/tree/master/svf/include/MTA) (<b>analysis of multithreaded programs</b>): value-flows for multithreaded programs ([CGO'16](https://yuleisui.github.io/publications/cgo16.pdf));
* [CFL](https://github.com/SVF-tools/SVF/tree/master/svf/include/CFL) (<b>context-free-reachability analysis</b>): standard CFL solver, graph and grammar ([OOPSLA'22](https://yuleisui.github.io/publications/oopsla22.pdf), [PLDI'23](https://yuleisui.github.io/publications/pldi23.pdf));
* [SVFIR](https://github.com/SVF-tools/SVF/tree/master/svf/include/SVFIR) and [MemoryModel](https://github.com/SVF-tools/SVF/tree/master/svf/include/MemoryModel) (<b>SVFIR</b>): SVFIR, memory abstraction and points-to data structure ([SAS'21](https://yuleisui.github.io/publications/sas21.pdf));
* [Graphs](https://github.com/SVF-tools/SVF/tree/master/svf/include/Graphs): <b> generating a variety of graphs</b>, including call graph, ICFG, class hierarchy graph, constraint graph, value-flow graph for static analyses and code embedding ([OOPSLA'20](https://yuleisui.github.io/publications/oopsla20.pdf), [TOSEM'21](https://yuleisui.github.io/publications/tosem21.pdf))

<p>We release the SVF source code with the hope of benefiting the open-source community. You are kindly requested to acknowledge usage of the tool by referring to or citing relevant publications above. </p>

<b>SVF</b>'s doxygen document is available [here](https://svf-tools.github.io/SVF-doxygen/html).

<br />

| About SVF       | Setup  Guide         | User Guide  | Developer Guide  |
| ------------- |:-------------:| -----:|-----:|
| ![About](https://github.com/svf-tools/SVF/blob/master/docs/images/help.png?raw=true)| ![Setup](https://github.com/svf-tools/SVF/blob/master/docs/images/tools.png?raw=true)  | ![User](https://github.com/svf-tools/SVF/blob/master/docs/images/users.png?raw=true)  |  ![Developer](https://github.com/svf-tools/SVF/blob/master/docs/images/database.png?raw=true) 
| Introducing SVF -- [what it does](https://github.com/svf-tools/SVF/wiki/About#what-is-svf) and [how we design it](https://github.com/svf-tools/SVF/wiki/SVF-Design#svf-design)      | A step by step [setup guide](https://github.com/svf-tools/SVF/wiki/Setup-Guide#getting-started) to build SVF | Command-line options to [run SVF](https://github.com/svf-tools/SVF/wiki/User-Guide#quick-start), get [analysis outputs](https://github.com/svf-tools/SVF/wiki/User-Guide#analysis-outputs), and test SVF with [an example](https://github.com/svf-tools/SVF/wiki/Analyze-a-Simple-C-Program) or [PTABen](https://github.com/SVF-tools/PTABen) | Detailed [technical documentation](https://github.com/svf-tools/SVF/wiki/Technical-documentation) and how to [write your own analyses](https://github.com/svf-tools/SVF/wiki/Write-your-own-analysis-in-SVF) in SVF or [use SVF as a lib](https://github.com/SVF-tools/SVF-example) for your tool, and the [course](https://github.com/SVF-tools/Software-Security-Analysis) on SVF  |

<br />


