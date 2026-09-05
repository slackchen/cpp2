# cpp2 运行时 ABI 冻结文档 v3(P0)

> 状态:**v3 冻结草案** | v1 2026-08-24,v2 2026-09-02(string 三槽),v3 2026-09-05(自研 std 面,见 §6) | 关联 [native-backend-eval.md](native-backend-eval.md) P0
> 本文定义任何原生后端(P1 直译后端起)必须保持的对外可见契约。
> 冻结范围 = **转译模式下已隐式依赖的全部可观察行为**;未列出的 C++ 实现细节不属于 ABI。

## 1. 数据表示

| cpp2 类型 | 表示(64 位平台) | 备注 |
|---|---|---|
| `i8/i16/i32/i64` | 对应宽度二补码,LE | `int` = 32 位 |
| `u8/u16/u32/u64` | 对应宽度无符号,LE | |
| `float/double` | IEEE-754 单/双精度 | |
| `bool/char/byte` | 1 字节 | `byte` = std::byte |
| `string` | 转译模式:`cpp2::string`(rt/cpp2/std/string.hpp,私有 `rep_` = std::string;**v3 变更**);native(Win64):自有 `ptr+size+cap` 三槽(8B×3,二进制安全,`resize/data/size` 经槽访问) | v2 变更:native 自有表示取代"按不透明处理"(0667516);跨边界仅经 §5 运行时函数触碰的约束不变;`rep_` 私有,外部码不得依赖 std::string 布局细节以外的东西(互操作仅经隐式转换 `std::string const&` / `std::string_view`) |
| `string_view` | ptr + len(16B) | 不包装,维持 std |
| `vector<T>/list<T>` | 转译模式:`cpp2::vector<T>`(rt/cpp2/std/vector.hpp,私有 `rep_` = std::vector;**v3 变更**);native:自有堆块 `[count][elems]` | 互操作经隐式转换 `std::vector<T>&/const&` |
| `map<K,V>` | 转译模式:`cpp2::map<K,V>`(rt/cpp2/std/map.hpp,有序,私有 `rep_` = std::map;**v3 新增**——v2 中 bare map 无映射属破损状态) | native:unsup(转译回退) |
| `T?` | std::optional<T> | |
| `unique<T>/shared<T>/weak<T>` | 对应 std 智能指针 | |
| `arena_ptr<T>` | {T* p; arena* a;}(16B) | rt/cpp2/arena.hpp |
| `variant<...>` | std::variant(index + 成员,对齐取最大) | |
| `expected<T>`(错误通道) | index + 存储(T 或 error) | 见 §3 |

**规则**:以上类型一律**按值传递其 C++ 表示**,不做自定义布局。这是"直译后端仍链接 libstdc++"(P1)的根基。

## 2. 参数传递(IMPL §4.2)

| 模式 | 调用约定 |
|---|---|
| `in`(默认) | 可平凡拷贝且 ≤ 2×指针宽 → 按值;否则 `T const&` |
| `inout / out` | `T&` |
| `move` | `T&&` |
| `copy` | `T`(按值) |
| `forward f: F`(F 为本函数类型参数)| `F&&`(转发引用)|
| `forward x: 具体类型` | `auto&&` |

## 3. 错误通道

```
struct error { std::string text; int64 category = 0; shared_ptr<error> cause; }
expected<T> ≈ { uint8 index; union { T value; error error; } }   // index: 0=value, 1=error
```

- `has_value()` ≡ index == 0;`*e` 取值;`e.error()` 取错误
- 传播(`?`)≡ 提前 `return std::move(e)`
- 类别 0 = 通用;非 0 为库自定义整数码(M7b)
- **冻结点**:index 宽度与排列、error 三字段顺序仅为文档性描述——原生后端只需与 **rt/cpp2/support.hpp 中 `cpp2::expected`/`cpp2::error` 的当前定义逐位一致**

## 4. 名字与链接

- 命名空间:每模块 → `cpp2mod::<点分路径>`,导出名落于该命名空间(headers 后端例外:全局命名空间,M3b 约束)
- 函数签名:C++ 普通函数(Itanium mangling 随编译器);`main` 特例:`int main()`
- 无体声明 = 外部符号,按声明原型解析(M6)
- 内部实体:匿名命名空间(内部链接)

## 5. 运行时入口(rt/cpp2)

原生后端必须提供或链接以下符号(当前全部为头文件 inline):

| 符号 | 用途 |
|---|---|
| `cpp2::trap(what,file,line)` | 检查失败统一终止(noreturn) |
| `cpp2::checked_add/sub/mul/div/mod` | 有符号算术检查(--release 下后端可不生成调用) |
| `cpp2::narrow_cast<T>(v,...)` | 收窄转换检查(浮点重载含 2^N 边界) |
| `cpp2::deref(p)` | 智能指针解引用 + 空检 trap |
| `cpp2::in<T>` | in 参数包装(conditional_t) |
| `cpp2::err / err(带类别)` | 失败值构造 |
| `cpp2::gc_new / gc_collect / gc_set_stack_top` | 保守式 GC(M6) |
| `cpp2::arena::create/reset`、`arena_ptr<T>` | 区域内存 |
| `cpp2::gc_stack_top 注入` | main 入口锚变量(生成代码注入,GC 扫描界) |

## 6. 版本化

- `CPP2_ABI_VERSION = 3`(本文)
- **v1 → v2(2026-09-02)**:native(Win64)string 表示从"std::string 不透明"改为自有 `ptr+size+cap` 三槽——破坏性变更,缓存全量失效
- **v2 → v3(2026-09-05)**:转译模式 string/vector/list 载体从 std 类型改为 `cpp2::string`/`cpp2::vector`(rt/cpp2/std,内部仍包 std 实现),bare map 从破损透传改为 `cpp2::map`——接口面变更,缓存全量失效(kVersion 1→2);native 侧表示不变,string/vector 机器码布局与 v2 一致
- 工具版本串(kVersion)混入缓存键:任何 ABI 破坏性变更必须提升版本 → 缓存全量失效
- .c2i v1(SHA-256 接口哈希)独立演进;接口文本格式变更同样使缓存失效

## 7. 明确不冻结

- 生成码的中间形态(part 划分、装箱片段、#line 细节)
- std 桥接的具体头集合(prelude_includes)
- 诊断文本措辞(位置映射除外)
