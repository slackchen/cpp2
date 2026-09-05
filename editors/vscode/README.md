# C++2 for Visual Studio Code

C++2 语言(`.cpp2`)的 IDE 体验支持:语法分色、实时诊断、上下文感知智能提示、大纲导航、一键运行。

## 功能

| 功能 | 说明 |
|---|---|
| **语法分色** | TextMate 文法:声明黄金法则(`name: type =`)、关键词、内建类型、`std::` 桥接、`@unsafe`/`@unchecked`、`$param`、数字/字符串/注释 |
| **实时诊断** | 保存/输入时(防抖 500ms)调用 `cpp2 check`,错误/警告映射回源位置(Problems 面板 + 波浪线) |
| **智能提示** | 上下文感知:按接收者类型/语法位出候选(见下表),条目带签名、层级标签与文档 |
| **大纲** | DocumentSymbol:类型/枚举/variant/concept/函数/方法/全局变量,支持 Ctrl+Shift+O 跳转 |
| **CodeLens** | `main:` 上方显示 `▶ Run` |
| **命令** | `C++2: Run File`(Alt+R 可自绑)、`C++2: Build`、`C++2: Check Current File` |

## 智能提示(上下文分发)

| 输入上下文 | 候选 |
|---|---|
| `s.`(接收者为 `string`) | **M9 自研 std 双轨 API**:cpp2 风格层(`len`/`at`/`find→u64?`/`substr`/`starts_with`/`ends_with`/`to_string`)排前,std 兼容层(`size`/`push_back`/`c_str`/…)靠后并标注 |
| `v.`(`vector<T>` / `list<T>` 别名) | cpp2 风格层 `push`/`pop→T?`/`at`/`first`/`last`/`len` + std 兼容层;签名中 `T` 按实参替换(`push(v: int)`) |
| `m.`(`map<K,V>`) | `insert`/`get→V?`/`at`(受检)/`contains`/`remove`/`len`;文档注明**无 `m[k]` 下标** |
| `a.`(`a: int[3]`) | **M10 固定长度数组**:cpp2 风格层 `len`(编译期常量 N)/`at`(受检)/`first`/`last` + std 兼容层(`size`/`fill`/…);签名中 `T` 按元素类型替换(`string[2]` 的 `at → string`) |
| `x.`(`x: T?`) | `has_value`/`value`/`value_or` |
| `p.`(`unique<T>`/`shared<T>`) | 自动解引用:直接列出 `T` 的成员 |
| `a[1].`(下标接收者) | 元素类型推断:数组/`vector`/`string` 下标均解析为元素/`char` |
| `kv.`(`for kv in ages`) | map for-in 元素 `pair<K,V>` 的 `first`/`second`;`for x in a`(T[N])循环变量 = 元素类型 |
| 链式调用 | 返回值类型推断:`s.find("x").` → optional 成员、`v.pop().` → `T?`、`d.norm2().` → `int` 的 UFCS 桥 |
| `Point.`(类型名) | 用户类型字段/方法(沿基类链,方法带完整签名与 `mutates`/`throws` 标注) |
| `std::` | 常用函数带签名(`print`/`println`/`to_string`/`make_unique`/`move`/…)、类型、`nullopt` |
| `EnumName::` | 枚举成员;match 臂行首 `.x` 同样列出全部枚举成员 |
| `import ` | `std` + 工作区全部 `.cpp2` 文件里声明的 module 名(60s 缓存) |
| `@` | `unsafe` / `unchecked` |
| `-> ` / `name: ` | 类型位:内建类型(带一句话 API 摘要)+ 工作区类型/枚举/variant;参数括号内附参数模式(`in`/`inout`/`out`/`move`/`copy`/`forward`) |
| 其他位置 | 关键词(含契约/参数模式,附说明)、内建类型、本地符号(函数带签名、变量带类型)、17 个片段(含 M10 数组声明 `arrd`)、`std::` 函数、内建 `err`/`old`/`result` |

字符串与注释内不出候选。

## 安装

开发调试安装(推荐):

```
cd editors/vscode
# 复制或软链到 %USERPROFILE%\.vscode\extensions\cpp2-lang-0.3.0
# 或在 VSCode 里 F5 启动扩展开发宿主
```

打包 VSIX(需要 `npm i -g @vscode/vsce`):

```
cd editors/vscode && vsce package
code --install-extension cpp2-lang-0.3.0.vsix
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
- 类型推断覆盖 `T[N]`/`vector<T>`/`map<K,V>`/`T?`/`unique<T>` 等的声明、参数、for-in 与下标形式;引擎侧语义以 `cpp2 check` 为准
