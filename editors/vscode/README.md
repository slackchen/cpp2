# C++2 for Visual Studio Code

C++2 语言(`.cpp2`)的 IDE 体验支持:语法分色、实时诊断、智能提示、大纲导航、一键运行。

## 功能

| 功能 | 说明 |
|---|---|
| **语法分色** | TextMate 文法:声明黄金法则(`name: type =`)、关键词、内建类型、`std::` 桥接、`@unsafe`/`@unchecked`、`$param`、数字/字符串/注释 |
| **实时诊断** | 保存/输入时(防抖 500ms)调用 `cpp2 check`,错误/警告映射回源位置(Problems 面板 + 波浪线) |
| **智能提示** | 关键词、内建类型、代码片段(`fn`/`struct`/`matchv`/`iflet`/`main` 等 12 个)、`std::` 常用函数 |
| **大纲** | DocumentSymbol:类型/枚举/variant/concept/函数/方法/全局变量,支持 Ctrl+Shift+O 跳转 |
| **CodeLens** | `main:` 上方显示 `▶ Run` |
| **命令** | `C++2: Run File`(Alt+R 可自绑)、`C++2: Build`、`C++2: Check Current File` |

## 安装

开发调试安装(推荐):

```
cd editors/vscode
# 复制或软链到 %USERPROFILE%\.vscode\extensions\cpp2-lang-0.1.0
# 或在 VSCode 里 F5 启动扩展开发宿主
```

打包 VSIX(需要 `npm i -g @vscode/vsce`):

```
cd editors/vscode && vsce package
code --install-extension cpp2-lang-0.1.0.vsix
```

## 配置

| 设置 | 默认 | 说明 |
|---|---|---|
| `cpp2.toolPath` | `"cpp2"` | cpp2 工具可执行文件路径,如 `D:/project/AI/glm/cpp2/.cpp2build/cpp2.exe` |
| `cpp2.checkOnSave` | `true` | 保存时检查 |
| `cpp2.checkOnChange` | `true` | 输入时防抖检查 |

## 注意

- `.cppm` 是标准 C++ module 接口的惯用后缀,C++2 已改用 **`.cpp2`** 避免冲突;本插件只关联 `.cpp2`
- 诊断依赖工具链版本 ≥ M3b(`cpp2 check` 子命令)
