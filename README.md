<img src="./docs/images/svf_logo_2.png" width="15%"><img src="./docs/images/svf_logo_3.png" width="85%">


## SVF-TS Progress: Tree-Sitter C Frontend (Phase 1 — AE)

**Target**: 108 ae\_assert\_tests | **Current**: 69 / 108 (64%) | **Baseline**: 47 / 108 (43%) | **ICFG Viewer**: [icfg.bjjwwangs.win](https://icfg.bjjwwangs.win)

### Detailed Results (69 PASS / 15 NC / 12 VF / 12 complex)

| Status | Cases |
|--------|-------|
| **PASS (69)** | BASIC\_assign\_0/1/2/3, BASIC\_bi\_add\_0/1/2, BASIC\_bi\_div\_0, BASIC\_bi\_mix\_0, BASIC\_bi\_mul\_0, BASIC\_br\_false/true\_0, BASIC\_br\_nd\_0/1/2, BASIC\_funcall\_ref\_0/1/2, BASIC\_ptr\_assign\_0, BASIC\_ptr\_call1/2, BASIC\_ptr\_func\_0, BASIC\_nullptr\_def, BASIC\_struct\_assign, BASIC\_array\_struct, BASIC\_array\_2d, BASIC\_array\_varIdx\_1, BASIC\_arraycopy2, BASIC\_switch/01-10, BASIC\_test\_11, CAST\_fptosi/fptoui/fptrunc/sext/sitofp/uitofp/zext, cwe121\_{char,int,int64,struct}\_alloc, cwe190\_int\_max, INTERVAL\_test\_6/8/9/11/12/13/15/16, LOOP\_for01/for\_call/for\_inc/while01, wto\_assert\_01-05 |
| **NC (15)** | BASIC\_array\_func\_3, BASIC\_array\_int, BASIC\_br\_nd\_malloc, BASIC\_ptr\_s32\_2, BASIC\_struct\_array, BUF\_OVERFLOW\_47, cwe190\_char\_fscanf, INTERVAL\_test\_58/64, CVE-2019-19847, CVE-2021-44975, CVE-2021-45341, CVE-2022-29023, CVE-2022-34835, CVE-2022-34918 |
| **VF (12)** | BASIC\_arraycopy1/3, BASIC\_array\_func\_0/4/6, BASIC\_ptr\_func\_1/4/6, CAST\_trunc, cwe126\_char, CWE127\_har, INTERVAL\_test\_14/19/2/20/36-1/49 |
| **NC (12 CVE)** | CVE-2020-13598, CVE-2020-29203, CVE-2021-39602, CVE-2022-23850, CVE-2022-26129, CVE-2022-27239, CVE-2022-34913 |

### Remaining Failures by Root Cause (39)

| Root Cause | Count | Status |
|------------|-------|--------|
| foo(&a) pointer-param write-back not tracked after return | 9 | Blocker: AE address domain propagation through CallPE→Store→Load→Store chain |
| Cross-function array/pointer GEP (address domain) | 4 | Same root cause as foo(&a) — address doesn't propagate through call |
| String ops modeled but alloca/pointer combo fails | 3 | memcpy/strcpy registered, but alloca→pointer→GEP chain incomplete |
| Function pointer indirect call | 3 | PTA-based indirect call resolution not wired in CTS |
| scanf/rand return value not modeled | 2 | Need ExtAPI annotation or special handling |
| && condition narrowing | 1 | Needs nested condNode ICFG structure |
| Cast truncation (int64→int8) | 1 | CopyStmt doesn't model truncation |
| Missing extern decl (svf\_assert) | 1 | Test file missing `extern void svf_assert(...)` |
| Global array init (AE doesn't GEP on GlobalICFGNode) | 1 | ICFG correct but AE may skip global GEP processing |
| Uninitialized vars + goto | 2 | goto not modeled in ICFG |
| Complex CVE patterns (struct+string+branch) | 12 | Multiple issues combined |

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


