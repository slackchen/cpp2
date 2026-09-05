# cpp2 — 演进式 C++ 2.0

与 C++ 生态 100% 互操作、零开销、重写语法与编译模型的"下一代 C++"。
**M1–M10 完成**(转译器全链路 + 生存期安全 + 区域/保守 GC 内存层 + 原生后端:Win64 零 CRT 直出 PE、契约落地、多模块摊平;**自研 std 面**:string/vector/map 接口 cpp2 风格、rt/cpp2/std 承载;**固定长度数组** `T[N]`:双后端直译、越界 trap),回归 **143 用例全绿**,examples 于 native 模式 **26 例全对拍**。

## 快速上手

```bash
bash build.sh                       # 构建 cpp2 工具(llvm-mingw / gcc / clang,C++23)
./.cpp2build/cpp2.exe run examples/showcase.cpp2    # 全特性一览:编译 + 运行(Linux/macOS 产物无 .exe 后缀)
bash tests/run.sh                   # 全量回归 143 项(末段含 native 对拍与插件补全单测)
```

编辑器:`editors/vscode` 插件(语法分色 / `cpp2 check` 实时诊断 / 上下文感知补全:std 双轨 API·数组·链式推断·import 模块 / 大纲 / Run),
安装方式见其 README。

## 语言一瞥

```cpp
module demo;
import std;

Point: type = {
    x: int = 0;
    y: int = 0;
    norm2: () -> int = x * x + y * y;          // 方法默认只读
    translate: (dx: int, dy: int) mutates = { x += dx; y += dy; }
    invariant: x >= -1_000_000;                // 类型不变量:公开方法出入口检查
}

clamp: <T: Ordered> (v: T, lo: T, hi: T) -> T = {   // 泛型 + concept
    if v < lo { return lo; }
    return v;
}

load: (path: string) -> string throws = {      // 错误即值:expected + '?' 传播
    return read_text(path)?;
}

main: () -> int = {
    p := Point{.x = 3, .y = 4};                // 聚合构造,默认值兜底
    s := load("a.txt") or "<empty>";           // or 默认值;f()! 断言必成功
    match p.norm2() {                          // 模式匹配(enum/variant/解构/守卫)
        0 => std::println("zero");
        n => std::println("{0}", n);
    }
    return 0;
}
```

## 特性矩阵(M1–M10)

| 域 | 内容 |
|---|---|
| 模块 | 单文件 `.cpp2`、`import`(点分名/非传递/环检测)、三后端:**headers 并行装箱(默认)** / C++20 modules(opt-in)/ 整程序摊平 |
| 类型 | type/方法/继承/invariant、enum、variant(match 穷尽)、泛型 + concept、optional(`T?`) |
| 安全 | 下标/溢出/除零/空解引用/收窄 trap;契约 pre/post/old/result;**生存期 Lite L1–L6**(悬垂捕获率 14/14);`@unsafe`/`@unchecked` 白纸黑字 |
| 错误 | `throws` → `cpp2::expected`;`?`/`!`/`or`/if-let/match;编译器强制处理 |
| std | **自研 std 面(M9)**:`string`/`vector`/`map` = rt/cpp2/std(接口 cpp2 风格:len/at/find→`T?`/push/pop/get/insert;内部私有 rep_ 包 std);map 无 `m[k]` 下标(缺失键走 get/at);`std::` 直写 = 互操作 escape hatch |
| 数组 | **固定长度 `T[N]`(M10)**:`{1, 2, 3}` 字面量、下标读写越界 trap、for-in、整体拷贝/相等(`?`/`*` 先于 `[N]` = 修饰元素类型;`int[]` 干净诊断指向 vector) |
| 互操作 | cxx_legacy 原文块、无体声明、zlib 双向(vendored 库本机实测 + CI 矩阵);export-headers 供纯 Cpp1 消费 |
| 内存 | unique/shared/weak、arena(+arena_ptr 逃逸检查)、可选保守式 gc |
| 工具 | run/check/build/transpile/export-headers/audit/fuzz;千单元压测(24.7s 全量 / 1.8s no-op);`.c2i` v1(SHA-256) |
| 原生后端 | Win64 零 CRT 直出 PE(examples 26 例对拍)+ SysV 整数核心 smoke(CI);契约 pre/post/invariant/old()/result 出入口检查;泛型单态化、虚分发、保守 GC、string 三槽、vector v0、**T[N] 栈上 N 连续槽直译(M10)**、cxx_legacy mini-C + unsafe 指针、zlib;多模块整程序摊平;map 显式 unsup(转译回退) |

## 文档

- [DESIGN.md](DESIGN.md) — 语言规范(v0.1)
- [IMPLEMENTATION.md](IMPLEMENTATION.md) — 实现方案 + 全里程碑完成记录与偏差表
- [docs/abi-freeze.md](docs/abi-freeze.md) — 运行时 ABI 冻结文档(v4)
- [docs/native-backend-eval.md](docs/native-backend-eval.md) — M7 原生后端评估

## 定位

演进派:不做 Rust 式全程序 borrow checker(DESIGN §7.8),用**可静态检查的局部规则 +
显式逃生舱**消灭绝大多数悬垂;与 C++ 双向互操作是第一约束,转译到 C++23 复用三家编译器矩阵。
