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

进度:M2a/M2b 已完成——`bash build.sh` 构建工具后,`./.cpp2build/cpp2.exe run examples/hello.cppm` 一条命令完成转译→编译→执行;`bash tests/run.sh` 跑全量回归(含溢出/越界/除零 trap 用例)。当前语言子集:函数、六种参数模式、类型与方法(`mutates`)、enum、`as` 转换、struct 字面量、所有权(`unique`/`shared` + 调用侧 `move`)、安全检查默认开启(`@unchecked` 退出)。

下一步:M2c——错误通道(`throws` + `?` 传播 + `match ok/err`)与契约(`pre`/`post`)。
