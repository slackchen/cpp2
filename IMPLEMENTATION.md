# C++2 实现方案

> **版本** v0.1 | **日期** 2026-08-20 | **关联** [DESIGN.md](DESIGN.md) v0.1
> 本文档回答"怎么把 DESIGN.md 造出来":编译管线、降低规则、运行时库、测试策略与里程碑切片。

---

## 1. 总体策略

### 1.1 转译优先(承接 D5)

`cpp2c` 把 `.cppm` 转译为 C++23,交给现有编译器:

| 收益 | 说明 |
|---|---|
| 优化与移植白拿 | clang / gcc / MSVC 三家后端、三平台开箱即用 |
| 互操作天然成立 | 生成的就是 C++,ABI 一致,链接器无感 |
| 语义锚定 | "C++2 = C++ 语义 + 显式检查",转译器即规范的参考实现 |

关键配套技巧:**生成代码穿插 `#line` 指令**,把编译器诊断与调试信息映射回 `.cppm` 源位置——用户永远面对 C++2 源码,不面对生成物。

代价(接受):一次构建经过两个前端(cpp2c + C++ 编译器),编译时间高于纯 C++;生成码可读性有损。M3 的模块化发射与长期原生后端逐步消化。

### 1.2 两阶段发射(D8)

| 阶段 | 模式 | 说明 |
|---|---|---|
| M2 | **整程序模式** | 入口 `.cppm` + 传递 import → 生成**单个** `.cpp`(所有模块摊平,非导出声明内部链接) |
| M3 | **模块模式** | 每个 `.cppm` → 一个 C++20 named module(`export module app.config;`),`.c2i` 提供 C++2 层的增量与接口哈希 |

整程序模式先行:最快跑通端到端、完全避开三家 C++20 模块实现差异、便于调试生成物;并且**永远保留**(单文件脚本、调试退化路径)。模块模式解决规模化编译。

### 1.3 实现语言:C++23(D7)

- 与目标同工具链:不引入第二套编译器/构建体系,贡献者就是 C++ 程序员
- `rt/` 运行时是头文件库,被工具自身与生成代码共用,天然只有一份
- 先例:cppfront 以约 3 万行可读 C++ 实现了同类系统

否决项:Rust(双工具链、生态错位、贡献者错位);立即自举 C++2(鸡生蛋问题,推迟到 M7+ 作为语言验证实验,非工程必需)。

---

## 2. 工具形态与代码库布局

单一可执行 `cpp2`(子命令式),编译核心是库 `libcpp2`:

```
$ cpp2 run hello.cppm          # 转译 + 编译 + 执行
$ cpp2 build                   # 扫描 import → DAG → 并行编译
$ cpp2 check                   # 快速语义检查,不生成代码
$ cpp2 audit                   # 列出全部 @unsafe / @unchecked / memory_only
$ cpp2 export-headers ...      # 见 DESIGN §9.3
```

```
cpp2/
  DESIGN.md  IMPLEMENTATION.md  README.md
  tool/src/        # libcpp2 + CLI:lexer / parser / sema / ifc / lower / emit / cli
  rt/cpp2/         # 运行时支持头(生成代码与工具共用)
  stdbridge/       # std 模块桥接描述 + 生成脚本
  examples/        # DESIGN.md 全部示例 = 验收用例
  tests/           # cases/*.cppm + expected/*.txt + runner
```

---

## 3. 编译管线

```
.cppm ─► Lexer ─► Parser ─► Sema ─► IFC 提取 ─► Lower ─► Emit ─► .cpp ─► clang/gcc/MSVC
```

| 阶段 | 职责 | 要点 |
|---|---|---|
| Lexer | → Token 流(带行列位置) | UTF-8 源;v0.x 标识符 ASCII |
| Parser | 手写递归下降 → AST | 不用生成器:错误恢复与诊断质量优先;文法即 DESIGN §14 速查表 |
| Sema | 名字解析、类型检查、重载、泛型约束、错误通道核对、可见性 | **必须有完整语义层**,见下 |
| IFC | 提取 export 签名 → `.c2i` | 序列化 + 接口哈希;M2 只产出不消费,先行稳定格式 |
| Lower | AST → 生成树:检查注入、`?` 展开、UFCS 定向、match 展开 | 全部类型相关决策在此定型 |
| Emit | 生成树 → C++23 文本 | `#line` 映射;确定性输出(便于 diff 与缓存) |

**为什么不是 cppfront 式"纯语法转译"**:接口提取需要导出声明的完整签名;UFCS 要先做名字解析才知道 `x.f()` 该发射成成员调用还是自由函数;下标/溢出检查依赖表达式类型;`unique` 的 `.` 自动解引用依赖接收者类型。**模块机制是语义级特性,决定了前端必须是语义级实现**——这是与 cppfront 最大的架构差异,也是主要工作量来源。

---

## 4. 降低规则(核心)

### 4.1 声明与函数

| C++2 | 生成 C++23 |
|---|---|
| `i: int = 0;` | `int i = 0;` |
| `x := expr;` | `auto x = expr;` |
| `c: const int = 1;` | `int const c = 1;` |
| 简短体 `= x * x;` | `{ return x * x; }` |
| 无 `->` 的函数 | 返回 `void` |
| `Point: type = { ... }` | `struct Point { ... };`,成员默认值 → 成员默认初始化器 |
| `Dog: type: Animal = { ... }` | `struct Dog : public Animal { ... };` |
| `Color: enum = { red }` | `enum class Color { red };` |
| `Value: variant = { int, string }` | `std::variant<int, std::string>`(类型别名) |
| `list<T>` | `std::vector<T>`(语言名即"序列") |
| `Point{.x = 1}` | C++20 designated initializers(直接兼容) |
| `destructor: () mutates = { ... }` | `~T() { ... }` |

### 4.2 参数模式

| C++2 | 生成 | 说明 |
|---|---|---|
| `(x: T)` / `in x: T` | `cpp2::in<T> x` | `in<T>`:不超过两指针大小且可平凡拷贝 → 按值,否则 `T const&`;接口统一、零开销 |
| `inout x: T` | `T& x` | sema 强制调用侧传可变左值 |
| `out x: T` | `T& x` + 出口检查已赋值(v0.3) | 鼓励用返回值替代 |
| `move x: T` | `T&& x`;调用侧 `move(x)` → `std::move(x)` | sema 强制 use-after-move 报错 |
| `copy x: T` | `T x` | |
| `forward x: T` | `auto&& x` + `std::forward<decltype(x)>` | |

方法:`mutates` → 非 const 成员函数;默认 → `const` 成员函数;不引用 `self` 的成员 → `static` 成员函数(如工厂 `File::open`)。

### 4.3 错误通道:expected,不是异常(M2c 已落地)

`-> R throws [E]` → 返回 `cpp2::expected<R>`(即 `std::expected<R, cpp2::error>`);`?` 机械展开:

```cpp
// C++2
text: string := read_file(path)?;

// 生成(v0.x 实际形态)
#line 3 "app/config.cppm"
auto __c2_try_0 = (read_file(path));
if (!__c2_try_0) { return std::unexpected(std::move(__c2_try_0).error()); }
std::string text = *std::move(__c2_try_0);
```

- `?` 的合法位置(M2c 子集):变量初始化、赋值右值、`return`、裸语句(丢弃值);嵌套进更大表达式 → sema 干净报错(机械展开需要语句级拆分,表达式级 IIFE 会破坏返回类型推导)
- `f()!` → `cpp2::must(f(), "file", line)`:失败即 `cpp2::trap`;可在任意表达式位置
- `match f() { ok x => / err e => }` → `has_value()` 分支;`f() or "d"` → `value_or`;`if x := f() { } else e := it { }` → 同 if-let 展开
- 失败值构造:`return err("原因");` → `cpp2::err("原因", "file", line)`,源位置并入消息(错误链的确定性替代)
- **编译器强制处理**:错误通道值出现在未处理位置(裸调用/条件/运算对象)→ sema 报错,不允许静默丢弃
- 错误类型列表(`throws E`)v0.1 解析后丢弃;类别体系 v0.3 起静态核对(DESIGN §8.4)

**为什么不用异常承载 throws**(备选曾认真考虑):异常从 Cpp1 侧可被捕获,破坏"错误是值、bug 是 trap"的二分语义;失败密集路径回溯成本高;expected 让失败路径在生成码中显式可见。Cpp1 异常在 `cxx_legacy` 边界由生成的 `try/catch` 包装转换为 `cpp2::error`(DESIGN §9.2)。保留 `--lower-errors=exceptions` 实验开关,供互操作密集的迁移场景评估。

### 4.4 安全检查注入

| 检查 | 生成 |
|---|---|
| 下标 | `v[cpp2::index(v, i, "app.cppm:42")]`;`index()` 内联比较 `size()`,失败 trap |
| 有符号算术 | `cpp2::checked_add(a, b)` 等;gcc/clang 用 `__builtin_*_overflow`,MSVC 用可移植预检 |
| `n as i32` | `cpp2::narrow_cast<i32>(n)`,溢出 trap;浮点→整型经 2^N 边界重载(NaN/越界 trap) |
| pre / post | 入口/出口 `if (!(...)) cpp2::trap(...)`;`old()` 入口求值缓存;post 时体包进 lambda(throws 函数 lambda 返回 `expected<R>`,`?` 传播同型直达出口),`result` 绑定返回值(M2c 已落地) |
| invariant | 注入公开成员函数出入口(v0.3) |
| 空安全 | 智能指针解引用 → `cpp2::deref(p)->m` 空检 trap(M4 已接入);`T?` 解包经 `has_value()` 显式路径(M2d);legacy 指针解引用 → 空检 trap(M6) |

`@unchecked` 块内不注入;`@unsafe` 块内容逐字转译并登记 audit。

### 4.5 模式匹配

按 scrutinee 静态类型选择降低:enum → `switch`(穷尽性由 sema 检查背书);variant → `std::visit` 或持有类型 if 链;optional → `has_value()`;守卫/解构绑定 → if 链 + 结构化绑定。`if x := opt { }` → `if (auto x = opt) { } else { }`。

### 4.6 UFCS 与智能指针

- 解析顺序:`x.f(a)` 先查成员,未命中查可见自由函数 → 发射 `f(x, a)`
- 接收者类型为 `unique`/`shared` → 发射 `cpp2::deref(x)->f(a)`(`.` 自动解引用 + 空检查,M4)
- `w.lock()` → `std::weak_ptr::lock`

### 4.7 模块与可见性

- **整程序模式**:全部声明进一个 `.cpp`;非导出声明进匿名命名空间(内部链接),导出声明置于 `cpp2mod::<module>` 命名空间供整程序内互见
- **模块模式(M3)**:`module app.config;` → `export module app.config;` + 定义;`.c2i` 同时产出
- `import std;` → 生成物顶部按需 `#include <...>` 标准头(**"无头文件"是对用户源码的保证,生成物是实现细节**);stdbridge 提供名字映射表,`std.c2i` 随工具链预置

### 4.8 `cxx_legacy` 与 `param`

- legacy 块内容**逐字**复制到生成 `.cpp` 顶部;块内名字按 C++ 规则解析;`cpp2 audit` 报告全部块位置
- `param debug: bool;` + CLI `--param debug=true` → 生成 `inline constexpr bool cpp2_param_debug = true;`;`if $debug` → `if constexpr (cpp2_param_debug)`

### 4.9 生成示例(端到端)

```cpp
// ── 输入:app/config.cppm(节选)────────────────────────────
export load: (path: string) -> Config throws = {
    text: string := read_file(path)?;
    return parse_config(text)?;
}

// ── 生成(整程序模式节选,#line 已映射回 .cppm)─────────────
#line 1 "app/config.cppm"
namespace cpp2mod::app::config {

struct Config {
    std::string name  = "";
    std::int64_t level = 0;
};

cpp2::expected<Config>
load(cpp2::in<std::string> path)
{
#line 10 "app/config.cppm"
    auto __c2_try_0 = (read_file(path));
    if (!__c2_try_0) { return std::unexpected(std::move(__c2_try_0).error()); }
    std::string text = *std::move(__c2_try_0);
    auto __c2_try_1 = (parse_config(text));
    if (!__c2_try_1) { return std::unexpected(std::move(__c2_try_1).error()); }
    return *__c2_try_1;
}

} // namespace cpp2mod::app::config
```

---

## 5. 运行时库 `cpp2::rt`(`rt/cpp2/`)

| 组件 | 说明 |
|---|---|
| `trap(msg, file, line)` | 打印原因与位置后 `std::abort`;不抛出 |
| `checked_add/sub/mul`、`narrow_cast<T>`、`index(c, i, loc)` | 溢出/越界检查;优先编译器 builtin |
| `expected` / `error` | `std::expected<T, cpp2::error>` 单参别名;`error` = 消息 + `message()`;`err(msg, loc)` 构造失败值(位置并入消息);`must(e, loc)` 断言必成功(M2c 已落地;类别集合/错误链 v0.3) |
| `in<T>` | 参数传递模式包装(§4.2) |
| `arena` | 段式分配 + 析构登记(创建序);`reset()` 逆序执行析构后整块归还 |
| `unique` / `shared` / `weak` | std 别名 + 工厂函数 |
| `gc<T>` | M6:先保守式收集器,可插拔后端 |

约定:纯头文件、无外部构建依赖、与生成码同编译器编译;`rt` 自身禁用异常与 RTTI。

---

## 6. 构建与增量

- **import 扫描**:词法级完成,产出依赖 DAG;检测到环 → 报错并指出环路径
- **`.c2i` 格式 v1(M2e 冻结)**:二进制容器,小端布局——magic `C2IF`(4B)/ `u32` 版本 = 1 / `u32` 名长 + 模块名 / 4 × 32B SHA-256 原始摘要(src、iface、gen、deps)/ `u64` 长度 + 导出接口规范化文本(UTF-8,调试可读)。哈希均为 64 位 hex 的 SHA-256;iface = SHA-256(接口规范化文本)。magic 或版本不符 → 视为无缓存(旧格式自然失效重建);缓存目录 `.cpp2cache/`
- **增量规则**:实现变化但导出接口哈希不变 → 依赖单元不重编
- M3 模块模式下,C++ 侧 BMI 由各编译器自管(构建目录内,不跨机器)

### 6.1 编译性能的账:什么时候真正快于 Cpp1

分三层,结论各不相同(避免高估现状):

| 对比对象 | 结论 | 依据 |
|---|---|---|
| 头文件 Cpp1(现实主流) | **结构性更快,确定** | 头文件解析成本 O(TU 数 × 平均传递头质量),准二次行为;模块接口解析一次、二进制复用,近线性。C++20 modules 实测大项目解析阶段 2–4× 收益 |
| Cpp1 C++20 modules | **略快,工程层显著更舒适** | 生成码同样骑在 C++20 named module 上,单 TU 编译天花板相同。赢在:词法级可靠 import 扫描(无宏)、`.c2i` 接口哈希增量(改实现不改接口 → 依赖者不重编)、接口自动提取免手工维护。输在:检查注入使生成码膨胀(估计 1.2–1.5×) |
| M2a 现状(整程序模式) | **最慢,刻意为之** | 双前端 + 全模块摊平成单个巨型 TU(基本单线程),牺牲全部并行度;仅为验收期的过渡形态,永非交付形态 |

"整体更快"成立的必要条件与手段:

1. **M3 模块模式**:每 `.cppm` → 独立 C++20 named module,构建图并行;
2. **`.cpp2cache` 转译层缓存**:源哈希不变 → 直接复用生成物,cpp2c 只处理变更模块;
3. **集中模板实例化**:高频实例化(`vector<int>` 等)由转译器统一提升到预编译支持模块,避免逐模块重复实例化——机器生成代码比手写代码更易做到;
4. **检查按构建档位裁剪**(DESIGN §6.7):Release 关闭溢出检查,生成码膨胀随之下降。

要彻底免除双前端税(转译一遍 + C++ 前端再解析一遍),需 M7+ 原生后端。在那之前的性能模型:

```
总耗时 ≈ cpp2c 线性扫描 + C++ 编译(模块化节省 − 生成码膨胀)
```

大项目下模块化节省远超两项成本,故净收益为正;小项目可能持平甚至略慢——预期管理明确写在案。

---

## 7. 测试策略

1. **用例驱动**:每个 `tests/cases/*.cppm` 附 `expected/*.txt`(stdout)与可选 `errors.txt`(诊断);runner:转译 → 三编译器矩阵编译 → 运行 → diff
2. **规范即测试**:DESIGN.md 每个示例进 `examples/`;文档改动必须过测试(doc-driven,规范与实现不漂移)
3. **降低快照**:代表性用例的生成码快照,PR 中审查降低规则变化
4. **故障注入(M4/M2c,已落地)**:注入越界/溢出/空解引用/浮点转换/契约违反(pre/post)/`!` 断言失败的用例必须 trap,且断言 trap 消息携带 `.cppm` 源位置——验证检查存在且生效
5. **模糊测试(M4,已落地)**:内置 `cpp2 fuzz` 确定性变异 fuzzer(seed 可复现,CI 友好);libFuzzer harness 备于 `tool/fuzz/`(支持该 sanitizer 的平台使用)

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 编译器诊断穿透到生成码 | `#line` 全覆盖;trap 消息携带 C++2 源位置 |
| 双前端编译时间偏高 | 整程序模式仅原型期使用;M3 模块 + BMI 缓存;长期评估原生后端(完整测算见 §6.1) |
| expected 展开致生成码膨胀 | 薄包装依赖内联消除;微基准守门(与手写 C++ 对照) |
| 三家 C++20 模块实现差异 | M3 起矩阵 CI;整程序模式永久保留为退化路径 |
| 语义层工作量失控 | 严格按 §9 切片;类型系统先最小集,泛型后置 |

---

## 9. 里程碑切片

| 切片 | 内容 | 出口判据 |
|---|---|---|
| M2a | 词法/语法子集、整程序模式、Emit + `#line`、rt 骨架 | `hello.cppm` 在 clang/gcc/MSVC 跑通 |
| M2b | type/成员/`mutates`、参数模式、enum、检查注入(下标/溢出) | DESIGN §5–§6 示例全绿 |
| M2c | ~~错误通道(expected / `?` / `!` / match ok-err / `or`)、契约~~ **已完成** | DESIGN §8 示例全绿 |
| M2d | ~~泛型 + concept、UFCS、variant/optional、模式匹配~~ **已完成** | ~~DESIGN §4.5–§5.6 示例全绿~~ |
| M2e | ~~`.c2i` 产出、`cpp2 audit`、`cpp2 run` / `check` CLI~~ **已完成** | ~~`.c2i` 格式冻结 v1~~ |
| M3 | C++20 模块发射、`cpp2 build` 并行增量、`export-headers` | 千单元项目增量构建正确 |

**M3 完成记录(2026-08-20)**:模块图加载(import 点分名解析、拓扑、环检测)、跨模块 sema 可见性(直接 import 的导出符号,非传递)、三种发射模式(摊平命名空间 / C++20 named module / 桥接头)、`cpp2 build`(拓扑分层 `std::thread` 并行、源哈希跳过转译、接口哈希控制下游重编)、`export-headers` 生成 `.h`/`.cpp` 并经 Cpp1 消费者实测互操作。增量语义三场景实测:no-op 零编译、实现变更仅重编本模块、接口变更传播至依赖者。

M3 实现偏差(相对本章早先的设想,均待后续里程碑消除):

| 偏差 | 现状 | 计划 |
|---|---|---|
| `.c2i` 格式 | ~~文本键值 + 接口文本(哈希为双种子 FNV-128,非 SHA-256)~~ **M2e 已冻结 v1**:二进制容器 + SHA-256(见 §6) | — |
| 模块编译参数 | ~~仅 clang 族~~ M4 起按家族分派(toolchain.cpp;clang 实测,gcc/msvc 为文档形态参数) | 有 gcc/MSVC 的环境实测并修正 |
| `export-headers` | 摊平式桥接(导出实体落在全局命名空间);限制:导出函数体不得引用跨模块未导出名 | 模块附着(attachment)语义研究后再决定是否包 `import` 式桥接 |
| 并行编译 | 拓扑分层 + 每层线程池 | 千单元级压测后引入就绪队列调度 |
| M4 | 检查器完备 + 故障注入 + 模糊测试 | 全部检查项验收 |
| M5 | 生存期 Lite L1–L6 | 悬垂测试集编译期捕获率报告 |
| M6 | `cxx_legacy` 增强、`gc<T>`(保守式) | 与 zlib 双向互操作 |
| M7+ | 自举实验、原生后端评估 | — |

**M4 完成记录(2026-08-20)**:检查器补齐空安全(智能指针 `.` 自动解引用 → `cpp2::deref(p)->m` 空检 trap,`@unsafe`/`@unchecked` 可退出,块形式语法落地——DESIGN §6.2/§6.6 原样)与浮点→整型转换越界检查(`narrow_cast` 浮点重载,2^N 精确边界,NaN 一并捕获);`cpp2 audit` 输出每模块检查注入点计数(arith/index/deref/narrow,谓词与发射侧一致)与全部 `@unsafe`/`@unchecked` 位置(含行号);故障注入套件扩至 5 例(溢出/越界/除零/空解引用/浮点转换),全部断言 trap 消息**与 `.cppm` 源位置**;模糊测试:`cpp2 fuzz` 内置确定性变异 fuzzer(seed 可复现,regression 内置 10000 次迭代零崩溃),parser 增加递归深度防护(3000 层嵌套 → 干净诊断而非爆栈);编译器矩阵:`toolchain.cpp` 按 `--version` 输出探测家族(本机 `g++` 为 clang 别名,仅看名字会误判),模块编译参数按 clang/gcc/msvc 分派;生成码缓存键混入工具版本,emit 演进不再被旧缓存掩盖。

M4 实现偏差:

| 偏差 | 现状 | 计划 |
|---|---|---|
| ~~契约检查项~~ | ~~audit 不含 pre/post/invariant~~ **M2c 已落地**:pre/post 注入 + audit 计数 + 故障注入用例;invariant 仍挂账 | invariant v0.3 注入公开成员函数出入口 |
| gcc/msvc 模块参数 | 文档形态,未实测(本机无对应编译器) | 取得环境后跑 `tests/run.sh` 修正 |
| libFuzzer | llvm-mingw(windows-gnu)不支持 `-fsanitize=fuzzer`(实测报错);harness 已备(`tool/fuzz/`),内置变异 fuzzer 承担本机与 CI 职责 | Linux/MSVC CI 接入 |
| fuzz 覆盖 | 词法/语法/sema;emit 未入 fuzz(其崩溃面小且有快照测试) | M5 起纳入 emit 与模块图加载 |

**M2c 完成记录(2026-08-21)**:错误通道全链路落地——`throws` 函数签名降为 `cpp2::expected<R>`(`rt`:`std::expected` 单参别名 + `error{message()}` + `err(msg, loc)` + `must(e, loc)`);`?` 机械展开(求值到临时量 → 失败提前 `return std::unexpected(error)` → 解包),合法位置 = 变量初始化/赋值右值/`return`/裸语句,嵌套使用 sema 干净报错;`f()!` → `cpp2::must`(任意表达式位置);`f() or 默认` → `value_or`;`match f() { ok x / err e }` → `has_value()` 分支(穷尽性 = 恰好一 ok 一 err);`if x := f() { } else e := it { }` 同构展开;`err("消息")` 自动附 `.cppm:行` 源位置。**编译器强制处理**:错误通道值出现在裸调用/`if`/`while` 条件/二元运算等未处理位置一律 sema 报错(DESIGN §8.1 "调用方必须处理"由类型系统背书)。契约:`pre:`/`post:`(函数与方法),`old()` 入口求值缓存,`result` 绑定返回值;post 时体包进 lambda——throws 函数 lambda 返回 `expected<R>`(`return R` 隐式转换、`?` 传播同型直达出口,失败跳过 post),非 throws 函数返回 `R` 本身;契约违反 → trap 不可捕获。audit 新增 contract 计数。接口哈希:方法签名补 `throws` 标记(签名变更触发依赖者重编)。过程中修复一个存量词法缺陷:`!=` 从未有过双字符规则(此前所有示例恰好只用 `==`),postfix `!` 落地后暴露,已补 `Ne` 词法。

M2c 实现偏差:

| 偏差 | 现状 | 计划 |
|---|---|---|
| `?` 嵌套进更大表达式 | sema 报错(仅语句级四种位置) | M2d+ 评估语句拆分或表达式级 lowering |
| `throws E` 显式类别 | 解析后丢弃 | v0.3 错误类型体系(DESIGN §8.4) |
| `err()` 消息 | 纯字符串 + 位置;无类别/错误链 | v0.3 同上 |
| invariant | 未实现(DESIGN §6.5 类型不变量) | v0.3 注入公开成员函数出入口 |
| 契约 + 简短体外的复杂控制流 | lambda 包裹对 NRVO 有影响(post 存在时) | Release 档位裁剪 post 时零开销(M4 §6.7 规则不变) |

**M2d 完成记录(2026-08-21)**:类型系统第二切片全链路落地——**泛型**:`<T: Concept>` 类型参数 + `requires A<T> && B<T>` 子句,`main` 拒绝泛型;**concept**:接口块(`self` 占位类型)降为 C++20 `template <class __c2_Self> concept ... = requires {...}`,满足性在实例化点由编译器判定,sema 负责约束名解析与"concept 不是值类型";**variant**:`Value: variant = {int, string}` → `std::variant` 别名,match 是唯一合法访问,穷尽性 sema 检查(缺臂/`_` 非末臂/重复臂均干净诊断);**optional**:`T?` → `std::optional<T>`,`none` 字面量,if-let(含 `_` 忽略绑定)/ match some-none / `or` 同构展开;**模式匹配扩展**:类型模式、解构(位置与 `.field` 命名,就地归一化为 `.field=绑定`)、`if` 守卫、通配、match **表达式**(产值,臂间公共类型 = 同型或算术宽化);**lambda**:函数声明去名字,表达式体/块体,v0.1 隐式 `[&]`;**UFCS**:成员未命中查可见自由函数,标量/char 接收者桥接 `std::to_string`(DESIGN §4.7 首例);**继承补全**:字段查找沿基类链(`find_field_deep`,派生同名隐藏基类),方法体内基类字段照常注入。示例:shapes(variant/守卫/enum 模式)、optional(if-let/some-none)、generics(concept/requires/UFCS/lambda)、types(继承/析构器/命名工厂);回归新增 16 正向 + 7 负例。

M2d 实现偏差:

| 偏差 | 现状 | 计划 |
|---|---|---|
| concept 满足性 | sema 只查约束名可解析;违反在 C++ 编译期报错(委托 requires 表达式,诊断经 `#line` 回源) | v0.x 保持委托;自研核对随错误类别体系评估 |
| 递归 variant(DESIGN §5.5 `list<Value>` 原例) | `std::variant` 不接受不完备类型,不可直接降级 | 装箱(`unique<Value>` 间接层)方案挂账 |
| 方法查找不走基类 | 字段已沿基类链;派生调用仅基类有的方法需自行转发 | 与虚分发一并评估(M5+) |
| lambda 显式捕获 `[...]` / 禁隐式 `this`(须 `[self]`) | v0.1 隐式 `[&]`,无捕获列表语法 | 后续里程碑落地捕获语法与规则强制 |
| UFCS 管道 `v \| filter(...)` | 未实现(仅成员形式 `x.f(a)`) | v0.x 后评估 |
| match 表达式臂类型 | 同型或算术宽化;无用户自定义转换 | 按需扩展 |

**M2e 完成记录(2026-08-21)**:CLI 补齐 `cpp2 check <root.cppm>`——模块图加载 + 全模块 sema,不发射不编译,ok 打印 `<n> module(s) ok`(快速语法/语义反馈路径,IMPLEMENTATION §2 承诺的最后一块)。`.c2i` 格式**冻结 v1**(§6 白纸黑字):自包含 SHA-256(`tool/src/sha256.hpp`,FIPS 180-4,零依赖,与 rt 同一"纯头文件"约定);src/iface/gen/deps 四处缓存哈希从 FNV-128 统一升级 SHA-256;缓存记录从文本键值改为二进制容器(magic `C2IF` + 版本 + 模块名 + 4 × 32B 摘要 + 长度前缀接口文本),magic/版本不符视为无缓存——旧文本缓存自然失效重建,迁移路径实测通过。增量三场景回归:no-op 零转译零编译、实现变更仅重编本模块(接口哈希不变 → 依赖者不动)、接口变更传播依赖者。过程中修复一个往返失真缺陷:缓存哈希在文件中存原始字节、内存中为 hex 串,非 hex 内容(如旧的空 deps 串)写出会失真为全零摘要——依赖组合现先归约为 SHA-256 hex 再入缓存,四个字段保证可无损往返。

M2e 实现偏差:

| 偏差 | 现状 | 计划 |
|---|---|---|
| `.c2i` 体部 | 接口规范化文本(长度前缀),非逐签名二进制编码 | 消费侧(跨模块 sema 直读 .c2i)落地时再定编码 |
| 哈希算法自实现 | SHA-256 工具内实现,未经第三方审计 | 仅用于缓存/增量键,非安全边界;保持现状 |
| `check` 范围 | 全模块 sema;无 `--quick`(单模块跳依赖)模式 | 千单元压测后按需加 |

---

## 10. v0.x 明确不做

原生后端、完整 borrow checker、反射、async/并发模型、跨编译器 BMI 共享、调试器专有扩展(DWARF/PDB 经 `#line` 已基本可用)。
