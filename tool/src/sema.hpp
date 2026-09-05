// C++2 语义分析(M2d:泛型/concept、variant、optional、模式匹配、UFCS)
#pragma once

#include "ast.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpp2::sema {

// 表达式类型(M2d 子集:标量、字符串、结构体、枚举、variant、容器、智能指针、
// 错误通道、optional、泛型参数)
struct Type {
    enum Kind {
        Unknown, Bool, Char,
        Int, I8, I16, I32, I64,          // 有符号
        U8, U16, U32, U64,               // 无符号
        Float, Double, String, StringView,
        NamedStruct, NamedEnum,
        Container,                        // vector/list/... 有下标
        Map,                              // map<K,V>:键值容器,无整数下标(M9)
        SmartPtr,                         // unique/shared/weak → pointee
        Error,                            // cpp2::error 值(match err 臂 / else 绑定)
        ErrVal,                           // err("msg") 的构造值(return 时转 unexpected)
        Variant,                          // variant 声明(name 定位 alternatives)
        Generic,                          // 泛型类型参数 T(name 即参数名)
        NoneVal,                          // none 字面量 → std::nullopt
        Pointer,                          // T* 裸指针(M6;产生仅限 @unsafe,M5-L5)
        ArenaPtr                          // arena_ptr<T>(M6;不得逃逸 arena 域,M5-L6)
    };

    Kind kind = Unknown;
    std::string name;                     // 结构体/枚举/variant 名,或容器名
    std::shared_ptr<Type> pointee;        // SmartPtr 指向类型(间接:避免自包含)
    std::shared_ptr<Type> element;        // Container 元素类型 / Map 值类型
    std::shared_ptr<Type> key;            // Map 键类型(M9)
    bool is_const = false;
    bool is_expected = false;             // 错误通道值:expected<value, error>(throws 调用结果)
    std::shared_ptr<Type> value;          // expected 的值类型(is_expected 时有效)
    bool is_optional = false;             // T? → optional<value>(M2d,DESIGN §6.4)

    Type deref() const { return pointee ? *pointee : Type{}; }
    Type elem()  const { return element ? *element : Type{}; }
    Type val()   const { return value ? *value : Type{}; }

    bool known()     const { return kind != Unknown; }
    bool wrapped()  const { return is_expected || is_optional; }
    bool is_signed() const {
        return kind == Int || kind == I8 || kind == I16 || kind == I32 || kind == I64;
    }
    bool is_unsigned() const {
        return kind == U8 || kind == U16 || kind == U32 || kind == U64;
    }
    bool is_int()    const { return is_signed() || is_unsigned(); }
    bool is_arith()  const { return is_int() || kind == Float || kind == Double; }
    bool is_floaty() const { return kind == Float || kind == Double; }
    bool is_indexable() const {
        // Pointer 不在此列:指针下标无长度概念,越界检查不适用
        // (sema.cpp Infer 已放行 p[i] = *(p+i);emit 走裸下标)
        return kind == Container || kind == String || kind == StringView;
    }
    bool is_smart() const {
        return kind == SmartPtr;          // weak 也在此,成员访问按解引用处理
    }

    // 有符号整型的位宽排序(Int 视为 32 位)
    int signed_rank() const {
        switch (kind) {
        case I8:  case U8:  return 8;
        case I16: case U16: return 16;
        case Int: case I32: case U32: return 32;
        case I64: case U64: return 64;
        default: return 0;
        }
    }

    // 宽化判定(目标值域 ⊇ 源值域):宽化 as 不注入运行期检查——
    // narrow_cast 的边界用 static_cast<From>(To::max()) 回卷,对宽化是必错路径
    // (int→i64 / int→double 实测踩坑,M6)
    bool is_widening_to(Type const& dst) const {
        if (!is_arith() || !dst.is_arith()) return false;
        if (display() == dst.display()) return true;
        if (is_floaty()) return dst.kind == Double;      // float→double 宽;double→float 窄
        if (dst.is_floaty()) return true;                // 整型 → 浮点:宽化
        if (is_signed() == dst.is_signed())
            return dst.signed_rank() >= signed_rank();
        if (is_signed() && dst.is_unsigned())
            return false;                                // 符号变更:显式且受检
        return dst.signed_rank() > signed_rank();        // unsigned → 更宽 signed
    }

    std::string display() const {
        if (is_expected) return "expected<" + val().display() + ">";
        if (is_optional) return val().display() + "?";
        switch (kind) {
        case Unknown: return "unknown";
        case Bool: return "bool";
        case Char: return "char";
        case Int: return "int";
        case I8: return "i8";   case I16: return "i16";
        case I32: return "i32"; case I64: return "i64";
        case U8: return "u8";   case U16: return "u16";
        case U32: return "u32"; case U64: return "u64";
        case Float: return "float";
        case Double: return "double";
        case String: return "string";
        case StringView: return "string_view";
        case NamedStruct: return name;
        case NamedEnum: return name;
        case Container: return name + "<" + elem().display() + ">";
        case Map: return name + "<" + (key ? key->display() : "?") + ", "
                       + elem().display() + ">";
        case SmartPtr: return "unique<" + deref().display() + ">";
        case Error: return "error";
        case ErrVal: return "err-value";
        case Variant: return name;
        case Generic: return name;
        case NoneVal: return "none";
        case Pointer: return deref().display() + "*";
        case ArenaPtr: return "arena_ptr<" + deref().display() + ">";
        }
        return "?";
    }

    static Type const unknown_type() { return Type{}; }
    static Type of(Kind k) { Type t; t.kind = k; return t; }
};

struct Diagnostic {
    int line, col;
    std::string msg;
};

struct Result {
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
    std::unordered_map<ast::Expr*, Type> expr_types;   // 推断结果,发射器消费
    // UFCS 定向(DESIGN §4.7):CallExpr(成员调用形式)→ 自由函数名;
    // "std::to_string" 为标量成员调用的标准库桥接目标
    std::unordered_map<ast::Expr*, std::string> ufcs;

    bool ok() const { return errors.empty(); }
    Type type_of(ast::Expr& e) const {
        auto it = expr_types.find(&e);
        return it != expr_types.end() ? it->second : Type{};
    }
    std::string ufcs_of(ast::Expr& e) const {
        auto it = ufcs.find(&e);
        return it != ufcs.end() ? it->second : "";
    }
};

// 检查单个模块。imported = 直接 import 的模块(非传递),其导出符号可见;
// 导入的声明只读使用(参数不加 const:符号表持有非 const 声明指针)。
Result check(ast::Module& m, std::vector<ast::Module*> const& imported = {});

} // namespace cpp2::sema
