// C++2 语义分析(M2b:类型推断 + 符号/const/mutates 检查)
#pragma once

#include "ast.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpp2::sema {

// 表达式类型(M2b 子集:标量、字符串、结构体、枚举、容器、智能指针)
struct Type {
    enum Kind {
        Unknown, Bool, Char,
        Int, I8, I16, I32, I64,          // 有符号
        U8, U16, U32, U64,               // 无符号
        Float, Double, String, StringView,
        NamedStruct, NamedEnum,
        Container,                        // vector/list/map/... 有下标
        SmartPtr                          // unique/shared/weak → pointee
    };

    Kind kind = Unknown;
    std::string name;                     // 结构体/枚举名,或容器名
    std::shared_ptr<Type> pointee;        // SmartPtr 指向类型(间接:避免自包含)
    std::shared_ptr<Type> element;        // Container 元素类型
    bool is_const = false;

    Type deref() const { return pointee ? *pointee : Type{}; }
    Type elem()  const { return element ? *element : Type{}; }

    bool known()     const { return kind != Unknown; }
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

    std::string display() const {
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
        case SmartPtr: return "unique<" + deref().display() + ">";
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

    bool ok() const { return errors.empty(); }
    Type type_of(ast::Expr& e) const {
        auto it = expr_types.find(&e);
        return it != expr_types.end() ? it->second : Type{};
    }
};

// 检查单个模块。imported = 直接 import 的模块(非传递),其导出符号可见;
// 导入的声明只读使用(参数不加 const:符号表持有非 const 声明指针)。
Result check(ast::Module& m, std::vector<ast::Module*> const& imported = {});

} // namespace cpp2::sema
