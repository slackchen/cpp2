# cpp2 — C++ 2.0 语言设计

演进式的"下一代 C++":与 C++ 生态 100% 互操作、零开销,重写语法与编译模型。

**当前状态**:设计阶段,规范见 [DESIGN.md](DESIGN.md)(v0.1 草案)。

核心特性:

- 单文件 `.cppm` 模块机制(类 C#),彻底消灭头文件——`.h` 只在与旧 C++ 互操作时出现
- 安全默认(越界 / 溢出 / 空指针 / 强制初始化),退出需显式 `@unchecked` / `@unsafe`
- 统一声明语法 `name: kind = value`,参数可变性写进签名
- 错误即值:`throws` + `?` 传播,bug 即 trap
- 实现策略:先转译到 C++23,复用现有编译器

文档:语言规范 [DESIGN.md](DESIGN.md) · 实现方案 [IMPLEMENTATION.md](IMPLEMENTATION.md)。

进度:**M2a/M2b/M3 已完成**——`bash build.sh` 构建工具;`cpp2 run` 摊平转译执行;`cpp2 build` 为 C++20 模块模式并行增量构建(实测:no-op 零编译、实现变更不惊动依赖者、接口变更才传播);`cpp2 export-headers` 生成桥接头,纯 Cpp1 代码可直接 `#include` 消费 C++2 模块;`bash tests/run.sh` 全量回归(19 项)。语言子集在 M2b 基础上新增:多模块 `import`(点分名、非传递、环检测)、空列表字面量 `{}`。

下一步:M2c——错误通道(`throws` + `?` 传播 + `match ok/err`)与契约(`pre`/`post`)。
