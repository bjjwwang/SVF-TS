# SVF-TS 开发计划

## 现状总结

`svf-cts` 模块已搭建完基础框架（~4,300 行），具备：
- Tree-Sitter C 语法解析（CTSParser）
- 模块管理与符号收集（CTSModuleSet）
- SVFIR 构建（CTSSVFIRBuilder）— 支持局部/全局/堆变量、基本语句、函数调用
- ICFG 构建（CTSICFGBuilder）— 支持 if/while/for/switch/do-while
- 作用域管理（ScopeManager）与 SSA 构建（SSABuilder，Braun 算法）
- 三个工具：`cts-svf`（通用分析+DOT输出）、`cts-ae`（抽象执行）、`cts-test`（单元测试）
- A/B 测试框架（对比 LLVM frontend 输出的 DOT）

测试套件 `TS-TestSuite/` 已从 SVF-tools/Test-Suite 导入，含 847 个用例的 C 源码（`src/`）和 LLVM bitcode（`test_cases_bc/`）。

---

## 开发阶段

### Phase 0：构建与冒烟测试（基础设施就绪）
> **目标**：确保项目能编译，现有 10 个小测试全部通过 A/B 对比

| 任务 | 说明 | 验证方式 |
|------|------|---------|
| 0.1 编译通过 | `build.sh` 完整编译 svf + svf-llvm + svf-cts | `make -j` 无错误 |
| 0.2 冒烟测试 | 跑通 `svf-cts/test/` 下现有 10 个 c_sources | `run_ab_test.sh` 全 PASS |
| 0.3 CI 基础 | 在 GitHub Actions 中跑编译 + 冒烟测试 | PR 自动检查 |

**产出**：可靠的开发-验证循环，后续每个 Phase 都能快速回归。

---

### Phase 1：AE 工具跑通（最短路径证明可用性）
> **目标**：`cts-ae` 跑通 TS-TestSuite 中 `ae_assert_tests` 的 108 个用例
> **为什么先做 AE**：`svf_assert()` 只是普通函数调用，`// CHECK:` 是注释——对 frontend 语义精度要求最低

| 任务 | 说明 | 依赖 |
|------|------|------|
| 1.1 支持 `svf_assert()` 识别 | CTSSVFIRBuilder 识别 `svf_assert` 调用并生成对应 SVFIR 节点 | — |
| 1.2 CHECK 注释解析 | 解析源码中 `// CHECK: pattern` 注释，传递给 AE 验证器 | — |
| 1.3 表达式完备性补齐 | 补全 AE 测试中用到的表达式：三元运算符 `?:`、复合赋值 `+=/-=`、强制转换 `(type)expr`、逗号表达式 | 1.1 |
| 1.4 控制流边界情况 | `break`/`continue`/`goto`/`label`（AE 测试中可能用到） | — |
| 1.5 外部函数建模 | `malloc`/`free`/`printf` 等常用 libc 函数的 SVFIR 建模（extapi 等价物） | — |
| 1.6 逐个攻克失败用例 | 从 108 个中先挑最简单的跑，逐步修 frontend bug | 1.1-1.5 |

**验证**：
```bash
cd TS-TestSuite && ctest -R ae_assert_tests   # 或等价的 cts-ae 批量跑
```

**里程碑**：108/108 ae_assert_tests PASS → 宣布 Phase 1 完成

---

### Phase 2：Andersen 指针分析跑通 basic_c_tests
> **目标**：`wpa -ander` 等价功能在 cts-svf 上跑通 `basic_c_tests` 的 62 个用例
> **为什么第二做**：Andersen 是 flow-insensitive 的，对 CFG 精度要求低，但对内存模型要求高——验证 frontend 核心能力

| 任务 | 说明 | 依赖 |
|------|------|------|
| 2.1 cts-wpa 工具 | 新建 `cts-wpa` 工具，接入 `AndersenWaveDiff` | Phase 0 |
| 2.2 aliascheck.h 宏识别 | 识别 `MUSTALIAS(p,q)` / `NOALIAS(p,q)` / `MAYALIAS` / `PARTIALALIAS` 等宏展开后的函数调用 | — |
| 2.3 完善内存模型 | 局部变量 alloca 等价、全局变量、堆对象（malloc/calloc/realloc）、字符串字面量 | Phase 1.5 |
| 2.4 指针算术 | `p + n`、`p++`、`p[i]`（等价于 GEP） | — |
| 2.5 结构体字段精度 | 嵌套结构体的 GEP 链、结构体指针 `p->field` 的 Load+GEP | Phase 1 |
| 2.6 函数指针 | 间接调用的 SVFIR 建模（`(*fp)(args)`） | 2.1 |
| 2.7 差分测试框架 | 扩展 `run_ab_test.sh`：对每个 basic_c_test 同时跑 LLVM 路径和 TS 路径，比对 Andersen 分析结果 | 2.1 |

**验证**：
```bash
# 差分验证：对每个用例比较两条 pipeline 的别名查询结果
for f in TS-TestSuite/src/basic_c_tests/*.c; do
    diff <(llvm-pipeline $f) <(cts-pipeline $f)
done
```

**里程碑**：62/62 basic_c_tests 别名结果与 LLVM 路径一致

---

### Phase 3：C++ 基础支持
> **目标**：跑通 `basic_cpp_tests` 的 52 个用例
> **为什么第三做**：C++ 在 C 的基础上增加类、继承、虚函数，是递进式难度

| 任务 | 说明 | 依赖 |
|------|------|------|
| 3.1 集成 tree-sitter-cpp | 在 CMakeLists.txt 中 FetchContent 引入 `tree-sitter-cpp` grammar | — |
| 3.2 类与成员函数 | 类定义 → SVFType、成员函数 → 隐式 `this` 参数、构造/析构函数识别 | 3.1 |
| 3.3 继承与虚函数表 | Class Hierarchy Graph (CHG) 构建、vtable 建模、虚调用 → 间接调用 SVFIR 节点 | 3.2 |
| 3.4 名字修饰 (mangling) | C++ 符号名需要与 SVF 内部期望的格式对齐（或建立映射） | 3.1 |
| 3.5 引用类型 | `T&` 作为指针的语法糖处理 | 3.1 |
| 3.6 new/delete | `new` → HeapObjVar + 构造调用，`delete` → 析构调用 + free | 3.2 |
| 3.7 模板（基础） | 简单模板实例化（basic_cpp_tests 中可能用到的程度） | 3.1 |

**验证**：52/52 basic_cpp_tests 别名结果与 LLVM 路径一致

---

### Phase 4：流敏感 & 上下文敏感分析
> **目标**：`dvf` 跑通 `fs_tests`（26 个）+ `cs_tests`（33 个）

| 任务 | 说明 | 依赖 |
|------|------|------|
| 4.1 cts-dvf 工具 | 新建工具，接入 DDA/DVF 分析 | Phase 2 |
| 4.2 精确 CFG | 确保 ICFG 的基本块划分、phi 节点位置与 LLVM mem2reg 后一致 | Phase 1.4 |
| 4.3 SSA 精度验证 | 对比 SSABuilder 输出与 LLVM `opt -mem2reg` 的 SSA 形式 | — |
| 4.4 调用图精度 | 上下文敏感需要精确的调用图，验证递归、回调等场景 | Phase 2.6 |
| 4.5 cs_tests 特有场景 | 调用链追踪、递归调用、多级间接调用 | 4.4 |

**验证**：59/59 dvf 测试全部 PASS

---

### Phase 5：Saber 漏洞检测
> **目标**：跑通 `mem_leak`（92 个）+ `double_free`（47 个）

| 任务 | 说明 | 依赖 |
|------|------|------|
| 5.1 cts-saber 工具 | 新建工具，接入 Saber 分析 | Phase 2 |
| 5.2 SVFG 精度 | SVFG 建立在 SVFIR 之上，验证 value-flow 边的完整性 | Phase 4 |
| 5.3 验证宏识别 | `SAFEMALLOC`/`NFRMALLOC`/`PLKMALLOC`/`CLKMALLOC` 宏展开后的函数名识别 | Phase 2.2 |
| 5.4 路径敏感边界 | 条件分支下的 malloc-free 配对，复杂控制流中的路径可行性 | Phase 4.2 |

**验证**：139/139 saber 测试全部 PASS

---

### Phase 6：MTA 多线程分析
> **目标**：跑通 `mta` 目录 59 个用例

| 任务 | 说明 | 依赖 |
|------|------|------|
| 6.1 cts-mta 工具 | 新建工具，接入 MTA 分析 | Phase 4 |
| 6.2 线程 API 建模 | `pthread_create`/`pthread_join`/`pthread_mutex_*` 等 API 的 SVFIR 建模 | Phase 1.5 |
| 6.3 验证宏识别 | `RC_ACCESS`/`CXT_THREAD`/`TCT_ACCESS` 等 | Phase 2.2 |

**验证**：59/59 mta 测试全部 PASS

---

### Phase 7：CFL 与差分测试完善
> **目标**：CFL 工具跑通，差分测试覆盖所有工具

| 任务 | 说明 | 依赖 |
|------|------|------|
| 7.1 cts-cfl 工具 | 接入 CFL 分析，加载 grammar 文件 | Phase 2 |
| 7.2 差分测试全量化 | 对所有 847 个用例，自动化对比 LLVM vs TS 两条 pipeline | Phase 5 |
| 7.3 svf-ex 等价的序列化测试 | write-ander / read-ander 的序列化/反序列化验证 | Phase 2 |

---

### Phase 8：真实程序 & 性能
> **目标**：在真实项目上验证可用性和性能

| 任务 | 说明 | 依赖 |
|------|------|------|
| 8.1 替代 crux-bc | crux-bc 无源码，选取有源码的真实程序（coreutils、busybox 等）替代 | Phase 5 |
| 8.2 多文件项目支持 | 多个 .c/.cpp 文件的跨文件分析、头文件处理、链接 | Phase 3 |
| 8.3 性能基准 | 与 LLVM 路径对比分析时间和内存占用 | Phase 7 |
| 8.4 预处理器 | `#define`/`#ifdef`/`#include` 的完整处理（当前只处理 `#include`） | — |

---

## TS-TestSuite 配套改造

| 阶段 | TestSuite 改造 | 说明 |
|------|---------------|------|
| Phase 0 | 保持原样 | LLVM bitcode 作为 ground truth |
| Phase 1 | 新增 `CMakeLists.txt` 中的 cts-ae 测试目标 | `add_test(NAME cts_ae_xxx COMMAND cts-ae src/ae_assert_tests/xxx.c)` |
| Phase 2 | 新增 cts-wpa 测试目标 + 差分对比脚本 | 同一源码跑两条 pipeline 自动 diff |
| Phase 3-6 | 逐步新增各工具的 cts-* 测试目标 | 镜像 LLVM 侧测试结构 |
| Phase 7 | 可选：移除 `test_cases_bc/` | 当 TS 路径完全覆盖后，.bc 文件不再需要 |

---

## 优先级排序原则

```
高价值 & 低难度 ──────────────────────── 高价值 & 高难度
     │                                        │
     │  Phase 0 (构建)                         │
     │  Phase 1 (AE) ◄── 最快出成果            │
     │  Phase 2 (Andersen) ◄── 核心能力        │  Phase 4 (流/上下文敏感)
     │                                        │  Phase 6 (多线程)
     │                                        │  Phase 8 (真实程序)
     │                                        │
低价值 & 低难度 ──────────────────────── 低价值 & 高难度
     │                                        │
     │  Phase 7 (CFL/差分)                     │  Phase 3.7 (模板)
     │                                        │  Phase 8.4 (预处理器)
```

**关键路径**：`Phase 0 → 1 → 2 → 3 → 4 → 5 → 6`
**可并行**：Phase 7 可在 Phase 2 之后随时开展；Phase 8.4 可独立推进

---

## 风险与应对

| 风险 | 影响 | 应对 |
|------|------|------|
| Tree-Sitter CST 缺少语义信息，某些 SVFIR 节点无法等价构建 | SVFIR 不等价 → 分析结果不一致 | 差分测试尽早暴露，逐 case 修复 |
| C 预处理器宏展开 | `#define` 背后的代码 Tree-Sitter 看不到 | Phase 8.4 引入预处理阶段，或要求输入已预处理代码 |
| C++ 模板 | 模板实例化在 LLVM 中由 Clang 完成 | 先限定支持范围，不追求完整模板 |
| 性能回退 | TS 路径可能比 LLVM 路径慢（额外语义分析开销） | Phase 8.3 做基准测试，按需优化 |
