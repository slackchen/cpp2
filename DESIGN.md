# C++2 语言设计稿

> **版本** v0.1(草案,供讨论)
> **日期** 2026-08-20
> **一句话定位**:一门演进式的"下一代 C++"——保持与 C++ 生态 100% 互操作和零开销,重写语法与编译模型,消灭四十年积累的安全坑和语法不一致,并以模块单文件机制彻底取代头文件。

本设计大量参考 Herb Sutter 的 Cpp2/cppfront 实验、Carbon 与 Rust 的公开经验,定位是一份**可讨论、可实现**的规范起点,而非最终定稿。

---

## 0. 目录

1. [目标与非目标](#1-目标与非目标)
2. [语言速览](#2-语言速览)
3. [模块与编译模型(核心特性)](#3-模块与编译模型)
4. [基础语法](#4-基础语法)
5. [类型系统](#5-类型系统)
6. [安全模型](#6-安全模型)
7. [所有权与生存期](#7-所有权与生存期)
8. [错误处理](#8-错误处理)
9. [与 Cpp1(C++)互操作](#9-与-cpp1互操作)
10. [工具链与构建](#10-工具链与构建)
11. [方案对比](#11-方案对比)
12. [路线图](#12-路线图)
13. [开放问题(下一轮讨论)](#13-开放问题)
14. [语法速查表](#14-语法速查表)
- [附录 A:术语](#附录-a术语)

---

## 1. 目标与非目标

### 1.1 要解决的问题

按优先级排列(均为本项目立项时确认的目标):

1. **内存与类型安全**:悬垂指针、数组越界、未初始化、整型溢出——系统性地消灭未定义行为(UB)的主要来源。
2. **语法统一简洁**:一件事只有一种写法——统一初始化、参数可变性写进签名、消灭声明/定义二重性。
3. **错误处理**:错误作为值传播,替代"异常与错误码混用"的现状,强制调用方可见失败路径。
4. **编译模型与速度**:模块优先。**单文件 `.cpp2` 机制(类似 C#),完全不需要 `.h`**;`.h` 只在与旧 C++ 互操作时出现。

### 1.2 非目标

- **不以追踪式 GC 作为运行时基座**:确定性析构是默认且不可关闭的语义;GC 不承载资源生命周期(文件、锁等永远走 RAII)。纯内存对象图的可选 GC 以库形式提供(见 7.5)。
- **不放弃 C++ 生态**:不要求重写库、不改 ABI、不引入独立运行时。
- **不追求语法上的"复古兼容"**:源码级逐文件迁移是支持的,但同一个文件内不混用新旧语法(与 Cpp2 一致)。
- **不发明新后端**(至少前期):原型阶段转译到 C++23,复用现有编译器全部优化能力。

### 1.3 设计原则

| # | 原则 | 含义 |
|---|------|------|
| P1 | **演进,不革命** | 100% C++ 互操作,同 ABI,同对象模型;任何 C++2 程序都可调用既有 C++ 库,反之亦然 |
| P2 | **安全默认,退出需显式** | 越界检查、溢出检查、强制初始化默认开启;退出用 `@unchecked`/`@unsafe` 标注,必须看得见 |
| P3 | **一件事一种写法** | 只有一种声明语法、一种初始化语法、一种函数定义语法 |
| P4 | **声明即定义** | 无前向声明、无头文件;模块内名字顺序无关,单遍可读 |
| P5 | **模块优先** | 没有文本包含,没有宏跨模块;编译单元 = 一个 `.cpp2` 文件 |
| P6 | **错误是值,bug 是陷阱** | 可预期失败走类型化错误值;程序 bug(契约违反)直接 trap,不可捕获 |
| P7 | **确定性** | 析构时机确定、内存模型确定、无隐式分配风暴 |
| P8 | **编译性能是一等功能** | 接口缓存、依赖隔离、增量编译内建于模型而非事后补救 |

### 1.4 关键决策记录

| # | 决策 | 状态 |
|---|------|------|
| D1 | 设计哲学:渐进演进派(Cpp2 路线) | 已确认 |
| D2 | 模块机制:单文件 `.cpp2`,类似 C#;`.h` 仅作互操作桥接产物 | 已确认 |
| D3 | 安全检查默认开启,提供显式退出通道 | 已确认 |
| D4 | 错误处理:错误即值 + `?` 传播 | 本稿提议 |
| D5 | 实现策略:先做转译器(.cpp2 → C++23)再考虑原生后端 | 本稿提议 |
| D6 | 内存回收:确定性分层(栈 → unique/shared → arena → 分配器);追踪式 GC 仅作可选库 `gc<T>`,不进运行时基座 | 本稿提议 |
| D7 | 实现语言:工具链自身用 C++23 编写(与目标同编译器,`rt` 共用) | 本稿提议 |
| D8 | 发射两阶段:M2 整程序单 `.cpp` → M3 每模块 C++20 named module + `.c2i` 增量 | 本稿提议 |

---

## 2. 语言速览

### 2.1 Hello, C++2

```cpp
// hello.cpp2
module hello;          // 可省略:省略时模块名 = 文件名
import std;

main: () -> int = {
    std::print("Hello, C++2!\n");
    return 0;
}
```

运行:`cpp2 run hello.cpp2`。没有头文件,没有 `#include`,没有前向声明。

### 2.2 一个真实例子

读取并解析配置文件,展示错误传播、类型定义与模块导出:

```cpp
// app/config.cpp2
module app.config;

import std;

export Config: type = {
    name:  string = "";
    level: int    = 0;
}

export load: (path: string) -> Config throws = {
    text: string := read_file(path)?;   // ? :向调用方传播失败
    return parse_config(text)?;
}

// 模块内部实现,未 export,外部不可见
read_file: (path: string) -> string throws = {
    f: file := file::open(path)?;       // file 析构时自动关闭(RAII)
    return f.read_all()?;
}

parse_config: (text: string) -> Config throws = {
    result: Config := Config{};         // 每个成员都有默认值,构造总是安全的
    for line in text.split('\n') {
        kv: list<string_view> := line.split('=');
        if kv.size() != 2 { continue; }
        if kv[0] == "name"  { result.name  = string(kv[1]); }
        if kv[0] == "level" { result.level = parse_int(kv[1])?; }
    }
    return result;
}
```

调用方视角:

```cpp
import app.config;

main: () -> int = {
    match load("app.ini") {
        ok  cfg => std::print("level = {0}\", cfg.level);
        err e   => std::print("加载失败: {0}\", e.message());   // 失败路径强制可见
    }
    return 0;
}
```

### 2.3 声明的黄金法则

一切声明都遵循同一种顺序——**名字在前,种类其次,值在后**:

```
name : kind = value
```

- 变量:`i: int = 0`
- 函数:`square: (x: int) -> int = x * x`
- 类型:`Point: type = { ... }`
- 枚举:`Color: enum = { red, green, blue }`
- 类型推断:`i := 0`(等价 `i: auto = 0`)

名字在前带来的直接收益:**初始化语法天然统一**,不再有 `()`/`{}`/`=` 三种初始化的语义分歧。

---

## 3. 模块与编译模型

> 本章是本设计的头号特性,对应目标 4:**默认模块化,单文件 `.cpp2` 机制,类似 C#;完全不需要 `.h`,`.h` 只在兼容旧 C++ 时出现。**

### 3.1 单文件单元 `.cpp2`

- **一个 `.cpp2` 文件 = 一个模块单元**。文件顶部声明所属模块:`module app.config;`
- 允许省略模块声明,此时模块名 = 文件名(便于单文件脚本)。
- **多个文件可以声明同一个模块名**,它们是同一模块的片段(fragment),彼此完全可见、**顺序无关**——不需要 C++20 模块分区的仪式感,也不需要 `mod.rs` 式的登记。
- 目录结构只是组织手段,不隐式影响模块名。

```cpp
// app/math/vector.cpp2
module app.math;

export Vec2: type = { x: double = 0; y: double = 0 }
```

```cpp
// app/math/matrix.cpp2
module app.math;      // 同一模块的另一个片段

export Matrix22: type = { /* ... */ }
// 可以直接使用 Vec2,无需 import,无需前向声明
```

### 3.2 可见性:`export` 与模块内部

| 可见性 | 写法 | 类比 |
|--------|------|------|
| 公开(跨模块) | `export` 前缀 | C# `public` |
| 模块内部(默认) | 不写 | C# `internal` |

- 默认**不导出**——公共 API 是显式选择的安全边界。
- `namespace` 仍然存在,但只是**逻辑命名空间**,与模块、与文件路径均解耦:

```cpp
module app.math;
namespace math::geom {
    export area: (r: double) -> double = 3.14159 * r * r;
}
// 使用:math::geom::area(2.0)
```

### 3.3 `import`

```cpp
import std;                        // 导入整个模块
import std { vector, string };     // 选择性导入,缩小依赖面
import app.math as m;              // 别名
```

规则:

- **非传递**:A import B,B import C,A 看不到 C。依赖必须显式声明,杜绝头文件的"传递污染"。
- **必须无环**:import 关系构成编译 DAG;环是编译错误(模块层面的循环依赖应在设计层解决)。
- 模块内**顺序无关**:同一模块各片段互相可见,无论文件排列顺序——前向声明由此消失。

### 3.4 被删除的东西

| C++ 机制 | C++2 的替代 |
|----------|--------------|
| `#include`(文本包含) | `import`(编译期符号导入) |
| 头文件 `.h`/`.hpp` | 不存在;接口由编译器从 `.cpp2` 自动提取 |
| 前向声明 | 模块内顺序无关,直接使用 |
| 宏(`#define` 等) | 语言层面不存在(见 3.6 替代方案);仅存活于 3.3 之外的 `cxx_legacy` 桥接块内,且不能逃逸 |
| `#ifdef` 条件编译 | `param` + `if $cond`(编译期常量分支) |
| 声明/定义二重性 | 声明即定义;接口提取交给编译器 |

### 3.5 接口提取与增量缓存

C# 程序员不需要写 `.h`,因为编译器自己从实现中提取公开接口。C++2 同理:

1. 编译器扫描 `.cpp2`,提取 `export` 声明的**二进制接口**(类型 `.c2i`),缓存在 `.cpp2/ifc/<module>.c2i`。
2. 依赖该模块的其他单元只读 `.c2i`,**不重新解析实现**——解析成本是 O(自己的文件 + 直接 import 的接口),而不是 O(传递依赖的全部文本)。
3. **增量规则**:实现改动但导出接口哈希未变 → 依赖者不重编。这是对 C++ 头文件"改一行、全量重编"顽疾的结构性修复。
4. `.c2i` 带内容哈希校验,跨机器分发可复现。

### 3.6 条件编译的替代

宏最正当的用途(按构建配置裁剪)由一等公民承担:

```cpp
param debug: bool;               // 由构建系统注入
param platform: string;

log_verbose: (msg: string) = {
    if $debug {                  // 编译期常量分支,死代码不生成
        std::print("[verbose] {0}\", msg);
    }
}
```

`$` 前缀表示编译期常量;`param` 未提供缺省值时必须由构建注入,否则编译错误——比"宏未定义静默走另一分支"安全得多。

---

## 4. 基础语法

### 4.1 变量与常量

```cpp
i: int = 0;               // 可变局部
c: const int = 42;        // 不可变绑定
d := 3.14;                // 类型推断(同 d: auto = 3.14)
big: i64 = 1'000'000;     // 数字分隔符
```

- **必须初始化**(见 6.1),或显式写 `x: int = _` 声明"我知道它未初始化"(lint 警告,通常只应出现在 `@unsafe` 块)。
- 花括号块是表达式,其值为最后一个表达式——`m := if a > b { a } else { b };` 合法。

### 4.2 函数

```cpp
// 块体
greet: (name: string_view) = {
    std::print("hi, {0}\", name);
}

// 简短体:单个表达式,推荐优先使用
square: (x: int) -> int = x * x;

// 默认参数、重载(与 C++ 语义一致)
scale: (v: double, factor: double = 2.0) -> double = v * factor;
```

无返回值时省略 `->`,不存在单独的 `void` 噪音。

### 4.3 参数传递:可变性写进签名

这是"语法统一"与"安全"两条主线的交汇点。**每个参数的访问模式在签名处一目了然**:

| 模式 | 写法 | 语义 | 调用侧要求 |
|------|------|------|-----------|
| 只读(默认) | `x: T` 或 `in x: T` | 只读访问;编译器自选传值/传引用,无用户可见拷贝 | 无 |
| 读写 | `inout x: T` | 可修改调用者的对象 | 必须传可变左值 |
| 输出 | `out x: T` | 被调用方必须赋值(主要服务互操作) | 无 |
| 移动 | `move x: T` | 获得所有权 | **调用侧必须显式写 `move`** |
| 拷贝 | `copy x: T` | 明确要一份独立副本 | 无 |
| 转发 | `forward x: T` | 泛型完美转发 | 无 |

```cpp
reset: (inout v: vector<int>) = { v.clear(); }
consume: (move buf: buffer) -> size_t = buf.size();

// 调用:
reset(items);
total := consume(move buf);   // move 在调用侧可见:buf 之后不可再用
```

**收益**:读函数签名即可知道副作用;`move` 出现在调用点,所有权转移不会被静默发生。

### 4.4 控制流

```cpp
if x > 0 { ... } else if x == 0 { ... } else { ... }

while running { ... }
for i in 0..10 { ... }        // 0..10 左闭右开;0..=10 含 10
for item in collection { ... }
break / continue               // 语义同 C++
```

- 条件**不需要**圆括号,**必须**花括号(消灭 dangling-else 与单语句陷阱)。
- `if` 可带声明(常见于可选值,见 6.4):
  `if user := find_user(id) { greet(user); } else { std::print("not found"); }`

### 4.5 模式匹配

```cpp
match shape {
    Circle(r) if r > 0    => 3.14159 * r * r;    // 带守卫
    Rect(w, h)            => w * h;
    _                     => 0;                   // 通配
}
```

支持:类型模式(`int n =>`)、解构模式(`Point(.x, .y) =>`)、枚举成员(`.red =>`)、`if` 守卫、`_` 通配。match 必须穷尽或以 `_` 兜底,编译器检查。

### 4.6 匿名函数

函数声明去掉名字即是 lambda:

```cpp
transform(v, (x: int) -> int = x * x);
```

无捕获语法噪音;需要捕获时用 `[...]` 同 C++(`[i]`, `[=]`),但**禁止隐式捕获 `this`**(须写 `[self]`)。

### 4.7 统一调用语法(UFCS)

成员函数与自由函数统一调用,`x.f(args)` 与 `f(x, args)` 完全等价:

```cpp
import std;

n: int = 42;
s := n.to_string();      // to_string 是 std 里的自由函数
// 等价于 to_string(n)
```

**收益**:扩展他人类型无需侵入;ranges 风格管道 `v | filter(...) | take(3)` 可自然落地为链式自由函数。

---

## 5. 类型系统

### 5.1 定义类型

```cpp
Point: type = {
    x: int = 0;          // 每个成员必须带默认值 → 默认构造永远安全
    y: int = 0;
}
```

继承(仅公有继承;私有继承请用组合):

```cpp
Dog: type: Animal = {
    name: string = "";
    speak: () -> string = "woof";
}
```

### 5.2 方法与 `mutates`

成员方法内直接使用成员名;`self` 也可显式引用。**修改自身的方法必须标注 `mutates`**,与参数的 `inout` 语义对齐:

```cpp
Counter: type = {
    value: int = 0;

    // 默认 self 只读(等价参数 in)
    get: () -> int = value;

    // 修改 self:签名处可见
    increment: (step: int = 1) mutates = { value += step; }
}
```

在 `const` 绑定上调用 `mutates` 方法 → 编译错误。const 正确性由此贯穿。

### 5.3 构造、析构与值语义

- **聚合构造**:`p := Point{.x = 3, .y = 4};` 指定初始化器,未给值的成员用其默认值。构造语法全语言仅此一种。
- **自定义构造逻辑**:用命名工厂(惯例 `make_` 前缀或类型内静态风格方法):
  ```cpp
  File: type = {
      h: handle = invalid_handle();     // 默认值保证"空文件"安全

      open: (path: string) -> File throws = {
          f: File := File{};
          f.h = sys::open(path)?;       // 失败走错误通道,不存在"半构造对象"
          return f;
      }
  }
  ```
  不存在构造函数重载列表;**不存在"构造失败抛异常"问题**——失败就是返回错误值,对象要么完整、要么不存在。
- **自定义赋值/转换**:类型内重载 `operator=`(值语义定制点,与 Cpp2 一致)。
- **析构**:`destructor: () mutates = { ... }`;确定性调用(作用域结束),顺序与 C++ 相同。

### 5.4 enum:默认 scoped

```cpp
Color: enum = { red, green, blue }

c: Color = Color::red;
match c {
    .red   => stop();
    .green => go();
    _      => slow();      // 显式处理剩余,或写穷尽三项
}
```

- 无隐式整型转换:`c == 1` 是编译错误;需要底层值写 `c as int`。
- 可指定底层类型:`Flags: enum: u8 = { a, b, c }`。

### 5.5 variant:类型安全的联合

```cpp
Value: variant = { int, string, list<Value> }

describe: (v: Value) -> string = match v {
    int n        => "int: " + n.to_string();
    string s     => "string: " + s;
    list<Value> xs => "list of {0}" + xs.size().to_string();
}
```

替代裸 `union`;`match` 是唯一合法的访问方式,穷尽性由编译器保证。

### 5.6 泛型与 concept

```cpp
// 类型参数在名字后的 <> 中;约束用 : 附加
clamp: <T: Ordered> (v: T, lo: T, hi: T) -> T = if v < lo { lo } else if v > hi { hi } else { v };

// 复杂约束用 requires 子句
max3: <T> (a: T, b: T, c: T) -> T
    requires Ordered<T> && Printable<T>
= { ... }

// concept 定义(接口块)
Ordered: concept = {
    operator<: (that: self) -> bool;
}
```

语义仍是 C++ 模板(编译期单态化、零开销),concept 取代 SFINAE/`enable_if` 体系。反射 API(`meta::`)列入路线图,不在本稿展开。

### 5.7 const 与类型转换

- `const` 修饰类型:`x: const int = 42;`、`(in s: const string)`。字符串字面量类型为 `const string_view`。
- **隐式转换只保留安全的宽化**(如同符号整型放宽);一切收窄、符号变更必须显式:
  ```cpp
  n: i64 = ...;
  m: i32 = n as i32;             // 显式收窄,溢出仍会检查(trap)
  u: u32 = (-1) as u32;          // 显式符号变更
  d: double = n;                 // 宽化 OK
  ```
- 用户自定义转换只能通过命名函数(`to_string` 风格)或类型内 `operator=`,不存在无痕隐式转换。

---

## 6. 安全模型

> 原则 P2:**安全默认,退出需显式。**所有检查在语义上"如同"trap(打印位置与原因后终止);Release 模式的裁剪规则见 6.7。

### 6.1 强制初始化

```cpp
i: int = 0;        // 正常
j: int;            // 编译错误:必须初始化
k: int = _;        // 显式未初始化:合法但 lint 警告,建议仅限 @unsafe 块
```

配合"类型成员必须带默认值"(5.1),**默认构造的对象永远是安全状态**。

### 6.2 越界检查

```cpp
v: vector<int> = ...;
v[i]            // 越界 → trap,不是 UB
```

退出通道(按粒度从细到粗):

```cpp
v.unchecked(i)              // 单次访问不检查
@unchecked { ... }          // 块级退出(含下标与算术检查)
```

热点循环的惯用写法:

```cpp
@unchecked for i in 0..n {  // 仅对有把握的热点使用
    sum += data[i];
}
```

### 6.3 算术溢出检查

```cpp
a + b          // 有符号溢出 → trap
math::wrapping_add(a, b)   // 明确要回绕语义(哈希、加密场景)
a as u32       // 显式转换的溢出同样检查
```

除零、浮点到整型的越界转换同属检查范围。

### 6.4 空安全

- **引用与指针类型默认非空**:`string_view`、`Widget&` 均不可为 null,`f(nullptr)` 是类型错误。
- 可空用 `T?`(即 `optional<T>`):

```cpp
find_user: (id: int) -> User? = ...

if u := find_user(7) {          // if-let:存在分支
    u.greet();
} else {
    std::print("not found");
}
```

- 互操作引入的原生指针(9.1)在解引用处做廉价空检查(可 `@unchecked` 退出)。

### 6.5 契约(pre / post / invariant)

```cpp
withdraw: (inout acct: Account, amount: i64)
    pre:  amount > 0 && amount <= acct.balance
    post: acct.balance == old(acct.balance) - amount
= {
    acct.balance -= amount;
}

Account: type = {
    balance: i64 = 0;
    invariant: balance >= 0;     // 公开操作入口/出口检查
}
```

- 契约违反 = **bug** → trap,**不可被捕获**(与错误值通道严格分离,见 8.1)。
- `old(expr)` 在入口求值;`post` 中 `result` 绑定返回值。

### 6.6 `@unsafe`:逃生舱

```cpp
@unsafe {
    p: byte* := legacy_buffer();      // 原生指针运算、无检查下标
    copy_bytes(dst, p, n);            // 仅允许调用 @unsafe 或 legacy 函数
}
```

- `@unsafe` 块内允许:指针算术、无检查下标、跳过空检查、调用 legacy 代码。
- `@unsafe` 标注**可被审计**:工具一条命令列出全项目所有 `@unsafe`/`@unchecked` 位置——安全边界是白纸黑字,而非散落代码库的约定。
- `@unsafe` 函数若被安全代码调用,调用点必须同样处于 `@unsafe` 内(感染式标注,同 Rust)。

### 6.7 零开销原则与裁剪

| 检查 | Debug/默认 | Release |
|------|-----------|---------|
| 强制初始化 | 编译期强制,无运行时成本 | 同左 |
| 越界 | trap | **保留**(推荐);构建级可关 |
| 算术溢出 | trap | 可配置为关闭(游戏/信号处理惯例) |
| 契约 | 全开 | 保留廉价断言,`post`/`old` 可按构建关 |
| 空检查 | trap | 保留(分支预测下近零成本) |

默认不追求"把检查全部裁掉换性能",而是**让退出的地方显式且局部**——安全默认、可审计的退出,优于到处手写不检查的代码。

---

## 7. 所有权与生存期

> 本章回答三个问题:堆对象何时回收(7.3 分层策略)、`shared` 成环怎么办(7.4)、GC 到底要不要(7.5)。

### 7.1 值语义默认

- 局部对象栈分配,作用域结束确定析构;参数默认 `in`(只读视图,无隐式拷贝);真拷贝必须 `copy` 模式或显式赋值。
- 没有语言级 `new`/`delete` 关键字——动态分配全部经由库类型,资源管理 = RAII。

### 7.2 `unique` / `shared`

```cpp
import std;

p: unique<Point> := make_unique(Point{.x = 1, .y = 2});
q: shared<Point> := make_shared(Point{.x = 3, .y = 4});

p.translate(1, 1);        // . 自动解引用,不需要 ->
consume_point(move p);    // 所有权转移,显式
```

- `unique` 只可移动;`shared` 引用计数、原子性可配置(`shared_mt`/`shared_st`)。
- 追踪式 GC 不进入运行时基座(P7);重度成环场景的可选库方案见 7.5。

### 7.3 内存回收分层策略

C++2 不把"要不要 GC"当二选一,而是给出一组**确定性优先、逐层升级**的回收策略;从 L0 到 L5 自动化程度上升、确定性下降,选择由程序员显式做出并在代码中可见:

| 层 | 机制 | 回收方式 | 典型场景 |
|----|------|---------|---------|
| L0 | 栈与值语义(默认) | 作用域结束确定析构 | 绝大多数对象 |
| L1 | `unique<T>` | 所有权移转,最后持有者析构 | 单一拥有者的动态对象 |
| L2 | `shared<T>`(`shared_mt` 原子计数 / `shared_st` 单线程免原子) | 引用计数归零即回收 | 真正共享所有权的对象 |
| L3 | `arena`(区域分配) | 一次性整体回收 | 同生命周期的批量对象:每请求、每帧、每趟编译 |
| L4 | 分配器参数化 | 由分配器决定 | 池化、对齐、嵌入式无全局堆 |
| L5 | `gc<T>`(可选库,**不在运行时基座内**,见 7.5) | 增量追踪式收集 | 重度互指的纯内存对象图 |

**arena(区域分配)**——确定性最强的批量回收手段,服务端"每请求一 arena"、游戏"每帧一 arena"、编译器"每趟一 arena":

```cpp
a: arena := arena{};
for req in requests {
    ctx := a.create(RequestContext{ .id = req.id });  // 纯分配,零逐个回收开销
    handle(ctx, a);                                    // 处理过程的子分配也从 a 走
}
// a 析构(或显式 a.reset()):按创建逆序运行析构函数,然后整块归还内存
```

- `arena_ptr<T>` 不得逃逸出 arena 的生存期(生存期规则 L6,见 7.7)。
- arena 内对象不得被 `shared` 接管——区域语义与计数语义不混用。

### 7.4 循环引用:`shared` 的已知缺口

`shared` 是引用计数(ARC),**成环则计数永不归零**——这不是实现缺陷,是数学性质(Swift 同样如此)。C++2 用四道防线正面处理,而不是假装问题不存在:

1. **建模指南**:层级方向用 `shared`,回边/缓存/观察者一律用 `weak<T>`:
   ```cpp
   Node: type = {
       value: int = 0;
       kids:  list<shared<Node>> = {};   // 层级边:强引用
       owner: weak<Node> = {};           // 回边:弱引用,不计数
   }

   if p := node.owner.lock() {           // lock() 返回 T?
       p.kids.add(make_shared(Node{ .value = node.value + 1 }));
   }
   ```
2. **静态告警**:`cpp2 check --cycles` 对类型图中互指的 `shared` 边做保守告警(可按注解关闭)。
3. **动态检测**:`cpp2 check --leaks` 在进程退出时报告未释放的 `unique`/`shared` 图(集成 LSan),测试期兜底。
4. **显式收集**:确有运行时成环需求的共享图,注册根后显式调用 `cpp2::collect_cycles(root)`——同步调用、无后台线程、无隐藏停顿。

若对象图**重度成环且生命周期模糊**,正确的答案不是第四道防线,而是 7.5 的 `gc<T>`,或干脆重新建模为 arena。

### 7.5 可选 GC:`gc<T>` 是库,不是运行时

**立场**:语言基座没有 GC(P1/P7)——不强制运行时、不引入全局停顿、不稀释确定性语义。但"图重度成环 + 生命周期模糊 + 纯内存"的场景真实存在(编辑器对象模型、脚本引擎对象图、缓存层),为此提供**库级**方案,而不是逼用户手维护 weak 拓扑:

```cpp
// memory_only 是标记接口:声明类型的析构无资源副作用
Node: type implements memory_only = {
    value: int = 0;
    kids:  list<gc<Node>> = {};   // 成环?随便环,由收集器处理
}

g: gc<Node> := gc::make(Node{ .value = 1 });
g.kids.add(gc::make(Node{ .value = 2 }));
```

规则(强制):

- **只管内存,不管资源**:`gc<T>` 要求 `T` 实现 `memory_only` 标记接口。文件、锁、连接等确定性资源永远走 RAII(`unique`/`shared`),职责严格二分;误声明 `memory_only` 与 `@unsafe` 同级,进入 `cpp2 audit` 报告。
- **无 finalizer 语义**:回收时机不确定,任何逻辑不得依赖"何时被回收"。确定性逻辑 = 离开作用域或计数归零。
- **不用则零成本**:收集器只随 `gc<T>` 的实例化进入产物;不写 `gc` 的程序,链接产物中没有收集器一行代码。
- **增量、参数化**:可配置标记步长与停顿预算;默认并发标记、增量式。
- **精确优先**:C++2 类型布局全知(无宏生成类型),编译器可为纯 C++2 图生成精确栈图/对象图;与 Cpp1 混链的场景回退为保守式扫描(Boehm 路线)。

前车之鉴:D 语言把 GC 放进默认路径,生态被逼出 `@nogc` 方言、库分裂成两半。C++2 反其道:**确定性子集就是语言本身,GC 是可选择挂载的库**——不存在方言,只有选择。标记接口的声明语法(此处暂写作 `implements`)随 OQ3 的接口讨论一并确定。

### 7.6 分配器

- 全局堆是默认分配来源;泛型容器与 `make_*` 系列接受分配器实参(编译期单态化、零间接;运行时多态场景提供 `any_allocator` 类型擦除版)。
- arena 可作为分配器来源,整批对象的内存随 arena 回收:
  ```cpp
  a: arena := arena{};
  v: vector<int> := vector(a);   // v 的分配走 arena;a 回收时 v 必须已亡(L6 约束)
  ```
- 嵌入式/内核场景可整体替换全局堆来源(构建配置),语言不依赖全局堆的任何隐式行为。

### 7.7 生存期规则(静态检查 Lite)

C++2 不引入完整 borrow checker(那是 v2 的研究题,见 13/OQ4),但用一组**可静态检查的局部规则**消灭最常见的悬垂:

| # | 规则 | 效果 |
|---|------|------|
| L1 | 不允许返回指向 `in` 参数的引用/指针 | 消灭"返回参数内部视图"类悬垂 |
| L2 | `move` 之后源对象不可再使用(编译错误) | 消灭 use-after-move |
| L3 | 临时对象的引用不可逃逸出所在语句(赋值给更长生存期时诊断) | 消灭最常见的临时悬垂 |
| L4 | 成员引用的生存期不得超过所属对象(函数粒度检查) | 消灭容器返回内部引用后扩容悬垂的大部分 |
| L5 | 原生指针算术、取局部地址仅存在于 `@unsafe` | 把"检查不了的"隔离到可审计区域 |
| L6 | `arena_ptr<T>` 与源自 arena 的对象不得逃逸出 arena 生存期(含 `reset()` 之后) | 区域整体回收的安全性前提 |

规则组合的净效果:**safe 子集中的悬垂,绝大多数在编译期报错**;`@unsafe` 与互操作边界之外的 UB 面积大幅缩小。

### 7.8 与完整 borrow checker 的距离

Rust 式全程序生存期标注不在本设计内(会推翻 C++ 引用语义,违背 P1)。长期方向是**推断为主、标注为辅**的渐进检查(参考 Cpp2 的 lifetime safety 提案思路),作为独立工具(`cpp2 check --lifetimes`)先落地,验证充分后再考虑进编译器。

---

## 8. 错误处理

### 8.1 两类失败,两条通道

| 类别 | 例子 | 通道 | 行为 |
|------|------|------|------|
| **可预期失败**(错误) | 文件不存在、网络超时、解析失败 | `throws` + 错误值 | 调用方**必须**处理,编译器强制 |
| **程序 bug** | 契约违反、越界、溢出、空解引用 | trap | 打印原因与位置后终止,**不可捕获** |

异常(可捕获的控制流)不再作为语言机制;Cpp1 异常在互操作边界被转换(9.2)。

### 8.2 声明与传播

```cpp
// 可能失败的函数:返回类型后加 throws(错误集合由编译器推断)
read_file: (path: string) -> string throws = { ... }

// 显式列举错误类型(可文档化、可静态核对)
parse_int: (s: string_view) -> int throws parse_error = { ... }

// 调用侧: ? 向上传播;函数必须因此标注 throws
text: string := read_file(path)?;

// 确信不会失败 → 失败即 bug → trap(少量、自证的正确性假设)
text: string := read_file(path)!;
```

`?` 是唯一的传播语法糖;`!` 表达"此处失败即程序 bug"。`throws` 裸写 = 编译器推断并合并所有被传播的错误类型;`throws E` = 显式收窄(传播了列表外类型 → 编译错误)。

### 8.3 处理错误

```cpp
// 1. 模式匹配(推荐:分支强制显式)
match read_file(path) {
    ok  text => std::print(text);
    err e   => std::print("failed: {0} ({1})\", e.message(), e.source().path());
}

// 2. if-let 成功路径
if text := read_file(path) {
    process(text);
} else e := it {                  // else 分支绑定错误
    log(e);
}

// 3. 短路默认值
text := read_file(path) or "fallback";
```

### 8.4 错误类型

- 每个错误值携带:类别(库定义的 enum/类型)、消息、来源位置(构建可选)。
- 标准库提供 `error`(通用)与错误码工具;鼓励库定义自己的错误 enum,跨层传播时自动装箱保留链(错误链可追溯,类似异常栈的确定性替代)。
- 与 Cpp1 异常的边界规则见 9.2。

---

## 9. 与 Cpp1(C++)互操作

> 原则 P1 的落地点:**双向、零开销、逐文件迁移。**

### 9.1 `cxx_legacy` 桥接

`.h` 唯一可能出现的地方——桥接块内部:

```cpp
// vendor/zlib.cpp2
module vendor.zlib;
import std;

cxx_legacy {
    #include <zlib.h>          // 预处理器只在这个块内存活
}

// legacy 名字进入 legacy:: 命名空间,可直接使用:
export compress: (data: span<const byte>) -> vector<byte> throws = {
    // 调用 zlib 的 C 接口,指针操作包在 @unsafe 里
    ...
}
```

桥接规则(强制):

1. **legacy 名字默认模块内部**,不得出现在 `export` 签名中——公开 API 必须用 C++2 类型重新包裹。
2. legacy 代码中的指针运算、可疑转换必须包在 `@unsafe` 块内(6.6 的审计因此覆盖互操作面)。
3. 宏不会逃逸出桥接块;桥接单元是被审计、被隔离的"防波堤"。
4. 标准库以 `std` 模块形式随工具链提供(优先映射到 C++ 标准库模块,缺省时由工具链自动生成的桥接垫片承担)。

### 9.2 异常边界

- Cpp1 代码抛出的异常穿过 C++2 函数 → 在最近的桥接边界捕获,转换为 `error`(`legacy_exception` 类别)沿错误通道传播。
- C++2 的错误值进入 Cpp1 侧 → 桥接包装层选择:转异常、转错误码,由包裹函数决定(见 9.3 的生成物)。

### 9.3 `export-headers`:反向生成 `.h`

让旧 C++ 项目消费 C++2 模块——`.h` 作为**编译产物**出现,永不手写:

```
$ cpp2 export-headers app/config.cpp2 -o bridge/
    生成 bridge/app/config.h + config.cpp
```

- 生成的 `.h`/`.cpp` 使用纯 Cpp1 语法,`throws` 映射为异常或 `std::expected`(可配置),检查与契约按 Debug/Release 语义保留。
- 生成物带接口哈希,`throws` 等签名变化触发下游重编。

### 9.4 混合构建与迁移路径

- 同一程序可自由混链 Cpp1 `.cpp` 与 C++2 `.cpp2` 目标文件(同 ABI)。
- 推荐迁移节奏:**新代码直接用 C++2 → 高风险旧模块逐步包裹桥接 → 按模块逐文件重写**。任何阶段程序都可构建、可发布。

---

## 10. 工具链与构建

### 10.1 `cpp2 build`

```
$ cpp2 build                # 扫描 import → 依赖 DAG → 并行编译
$ cpp2 run hello.cpp2       # 单文件直跑
$ cpp2 check                # 快速语义检查(不生成代码)
$ cpp2 audit                # 列出全部 @unsafe/@unchecked 位置
$ cpp2 export-headers ...   # 见 9.3
```

- import 扫描在词法层完成(没有宏,扫描是可靠且微秒级的)。
- 构建图 = import DAG;接口缓存(3.5)使常规增量构建只触及真正受影响的单元。
- 输出物:目标文件 + `.c2i` 接口;链接仍用系统链接器,产物是普通可执行文件/库。

### 10.2 编译管线

```
.cpp2 ─词法/语法/语义─► AST ─接口提取─► .c2i(缓存)
                      │
                      └─代码生成─► v0.x:转译 C++23 .cpp → clang/gcc/MSVC
                                  v1.x:可选原生后端(LLVM)
```

### 10.3 转译优先策略(v0.x)

原型期不写后端,转译到 C++23:

| 收益 | 说明 |
|------|------|
| 即时可移植 | clang/gcc/MSVC 全支持,三平台白送 |
| 互操作免费 | 生成的就是 C++,桥接层薄 |
| 语义锚定 | C++2 语义 = C++ 语义 + 显式检查,转译器即规范的参考实现 |

检查、契约、错误通道在转译产物中体现为显式运行时代码(库支持:`cpp2::rt`)。此策略已被 cppfront 验证可行。

实现细节(编译管线、降低规则总表、里程碑切片)见 [IMPLEMENTATION.md](IMPLEMENTATION.md)。

---

## 11. 方案对比

| 维度 | C++(Cpp1) | **C++2(本设计)** | Carbon | Rust |
|------|-----------|------------------|--------|------|
| 与 C++ 互操作 | — | 源级双向,同 ABI,逐文件迁移 | 设计目标,实验中 | FFI 边界层,生态隔离 |
| 头文件 | 必须 | **无**(`.h` 仅桥接产物) | 无 | 无 |
| 内存安全 | 手工自律 | 默认安全 + 显式退出 + 生存期 Lite | 默认安全 | 编译期保证 |
| 动态内存 | 手工 new/delete + 智能指针 | 分层回收:unique/shared/arena/可选 gc 库 | ARC(引用计数) | 所有权 + borrow |
| 错误处理 | 异常/错误码混用 | 错误即值,`?` 传播,bug 即 trap | Result 风格 | Result + panic |
| 语法一致性 | 低(40 年沉积) | 单一声明法则 | 中 | 高 |
| 学习曲线(自 C++) | — | **数天**(心智模型不变) | 数周 | 数月 |
| 确定性析构 | ✔ | ✔ | ✔ | Drop ✔ |
| 现状 | 工业标准 | 设计稿 | 实验 | 成熟生产 |

定位差异:Carbon 面向 Google 内部大规模迁移;Rust 用所有权换取安全但放弃兼容;**C++2 的独特卖点是"以最低迁移成本换取最大安全与工效改进"**。

---

## 12. 路线图

| 里程碑 | 内容 | 验收标准 |
|--------|------|---------|
| M1 | 规范 v0.1(本文档)评审定稿 | 开放问题 13 章收敛 |
| M2 | 转译器原型 `cpp2c`:词法/语法/AST → C++23 | `examples/` 全部转译后可在三大编译器编译运行 |
| M3 | 模块工具链:`.c2i` 接口缓存、`cpp2 build` 增量并行 | 千单元级玩具项目的增量构建正确且快 |
| M4 | 检查器:越界/溢出/空/契约 + `cpp2 audit` | 注入故障的测试套件全部按预期 trap |
| M5 | 生存期 Lite(L1–L5)静态检查 | 悬垂测试集编译期捕获率报告 |
| M6 | `cxx_legacy` 桥接 + `export-headers` | 与一个真实 C++ 库(如 zlib)双向互操作 |

---

## 13. 开放问题(下一轮讨论)

| # | 问题 | 备选 |
|---|------|------|
| OQ1 | 错误传播糖:`?` / `!` 是否够用?是否要 `catch`-风格的组合子? | `?`(本稿)vs `: =`(Cpp2 风格)vs 无糖 |
| OQ2 | 错误类型体系:统一注册表(类 `std::error_code`)vs 每库独立 enum | 倾向每库 enum + 自动装箱保留链 |
| OQ3 | `concept`/接口:结构化(duck-typing)vs 名义(需显式 `impl`) | 模板生态偏结构化;互操作偏名义;可能双轨 |
| OQ4 | 生存期检查深度:L1–L5 之外是否推进完整 borrow 推断 | 独立工具先行(本稿立场)vs 直接进编译器 |
| OQ5 | 并发模型:channel/`synchronized`/无数据竞争类型系统 | 列为 v0.2 主题 |
| OQ6 | 反射 API 形态(`meta::`)与编译期求值边界 | 跟踪 C++26 反射进展 |
| OQ7 | 数字类型:隐式宽化的具体矩阵(无符号→有符号?) | 本稿:同号宽化隐式,其余显式 |
| OQ8 | `param` 注入与构建系统的接口(多配置、交叉编译) | 类 CMake preset 的声明文件? |
| OQ9 | `arena_ptr` 逃逸检查(L6):区域类型系统(全静态)vs 句柄代际校验(廉价运行时) | 倾向:函数粒度静态 + 跨函数保守 |
| OQ10 | `gc<T>` 收集器:精确式(利用编译器布局元数据)vs 保守式(Boehm 式混链兼容) | 倾向:C++2-only 精确、混链回退保守 |

---

## 14. 语法速查表

```cpp
// ─── 模块 ───────────────────────────────────────
module app.core;                 // 本文件所属模块(可省略)
import std;                      // 导入(非传递)
import std { vector, string };   // 选择性导入
export Any: declaration;         // 导出(默认模块内部)

// ─── 声明(黄金法则 name: kind = value)─────────
i: int = 0;                      // 可变变量(必须初始化)
c: const int = 42;               // 不可变绑定
d := 3.14;                       // 类型推断
f: (x: int) -> int = x * x;      // 函数(简短体)
g: (s: string_view) = { ... }    // 函数(块体,无返回省略 ->)
Point: type = { x: int = 0 }     // 类型(成员必须带默认值)
Dog: type: Animal = { ... }      // 公有继承
Color: enum = { red, green }     // 枚举(scoped)
Value: variant = { int, string } // 标签联合
Ordered: concept = { ... }       // 约束

// ─── 参数模式 ───────────────────────────────────
(in x: T)      (inout x: T)      (out x: T)
(move x: T)    (copy x: T)       (forward x: T)
// 调用侧:consume(move x)

// ─── 方法 ───────────────────────────────────────
get: () -> int = value;                   // self 只读
inc: () mutates = { value += 1; }         // 修改 self
destructor: () mutates = { ... }          // 析构
invariant: value >= 0;                    // 类型不变量

// ─── 控制流与模式 ───────────────────────────────
if x > 0 { ... } else { ... }             // 必花括号,条件无圆括号
for i in 0..10 { ... }                    // 半开区间;..= 含端点
match v { int n => ...; _ => ...; }       // 穷尽检查

// ─── 安全与错误 ─────────────────────────────────
v[i]                            // 越界 → trap
a + b                           // 溢出 → trap
@unchecked { ... }              // 块级退出检查
@unsafe { ... }                 // 原生指针/legacy 逃生舱
x := f()?;                      // 传播失败(函数需 throws)
x := f()!;                      // 失败即 bug → trap
match f() { ok x => ...; err e => ...; }
if x := f() { ... } else e := it { ...; }
x := f() or "default";

// ─── 所有权 ─────────────────────────────────────
p: unique<Point> := make_unique(Point{.x = 1});
consume(move p);                // 所有权转移显式
q: shared<Point> := make_shared(Point{});
w: weak<Node> = {};  if p := w.lock() { ... } // 弱引用破环;lock() → T?
a: arena := arena{};  x := a.create(Point{}); // 区域批量分配,整体回收
g: gc<Node> := gc::make(Node{});              // 可选追踪 GC(仅 memory_only 类型)

// ─── 其他 ───────────────────────────────────────
x.f() ≡ f(x)                    // UFCS
n as i32                         // 显式转换
u := find(id)                    // User? 可空;if-let 解包
param debug: bool;  if $debug { ... }   // 条件编译替代
cxx_legacy { #include <zlib.h> }         // 唯一的 .h 栖身之处
```

---

## 附录 A:术语

| 术语 | 含义 |
|------|------|
| **Cpp1** | 现有 C++(C++98–C++26) |
| **C++2** | 本设计定义的语言 |
| **模块单元** | 一个 `.cpp2` 文件 |
| **片段(fragment)** | 声明同一模块名的多个文件之一 |
| **`.c2i`** | 编译器提取的二进制模块接口(缓存) |
| **桥接单元** | 含 `cxx_legacy` 块的 `.cpp2`,legacy 代码的隔离防波堤 |
| **trap** | 契约/检查失败的终止行为:打印原因与位置后终止,不可捕获 |
| **错误值** | `throws` 通道携带的失败信息,可匹配、可传播 |
| **模式** | in / inout / out / move / copy / forward 六种参数访问模式 |
| **`mutates`** | 成员方法修改 `self` 的签名标注 |
| **`arena`** | 区域分配器:批量分配、整体回收;析构按创建逆序执行 |
| **`weak<T>`** | 弱引用,不参与引用计数;`lock()` 返回 `T?` |
| **`gc<T>`** | 可选的库级追踪式 GC 指针,仅接受 `memory_only` 纯内存类型 |
| **`memory_only`** | 标记接口:声明类型的析构无资源副作用,`gc<T>` 的准入条件 |
