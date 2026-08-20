// C++2 AST(M2b:类型定义、方法/mutates、enum、检查注解、struct 字面量、as 转换)
// 节点带源位置;dispatch 用 kind() + static_cast,不依赖 RTTI。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cpp2::ast {

struct Node {
    int line = 0;
    int col  = 0;
    virtual ~Node() = default;
};

// ── 类型使用 ────────────────────────────────────────────────────────
struct TypeUse {
    int line = 0;
    bool is_const = false;
    std::vector<std::string> parts;          // 限定名,如 {std, string}
    std::vector<TypeUse> args;               // 泛型实参

    bool empty() const { return parts.empty(); }
};

// ── 表达式 ──────────────────────────────────────────────────────────
struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr : Node {
    enum Kind { Literal, Name, Call, Binary, Unary, Assign, Index, Member, Paren,
                StructLit, AsCast, ListLit };
    virtual Kind kind() const = 0;
};

enum class LitKind { Int, Double, String, Char, Bool };

struct LiteralExpr : Expr {
    std::string text;                        // 原文(字符串含引号与转义)
    LitKind lit = LitKind::Int;
    Kind kind() const override { return Kind::Literal; }
};

struct NameExpr : Expr {
    std::vector<std::string> parts;
    Kind kind() const override { return Kind::Name; }
    bool qualified() const { return parts.size() > 1; }
};

struct CallExpr : Expr {
    ExprP callee;
    std::vector<ExprP> args;
    Kind kind() const override { return Kind::Call; }
};

struct BinaryExpr : Expr {
    std::string op;
    ExprP lhs, rhs;
    Kind kind() const override { return Kind::Binary; }
};

struct UnaryExpr : Expr {
    std::string op;
    ExprP operand;
    Kind kind() const override { return Kind::Unary; }
};

struct AssignExpr : Expr {                   // 仅作语句出现
    std::string op;                          // = += -= *= /= %=
    ExprP target, value;
    Kind kind() const override { return Kind::Assign; }
};

struct IndexExpr : Expr {
    ExprP base, index;
    Kind kind() const override { return Kind::Index; }
};

struct MemberExpr : Expr {
    ExprP base;
    std::string name;
    Kind kind() const override { return Kind::Member; }
};

struct ParenExpr : Expr {
    ExprP inner;
    Kind kind() const override { return Kind::Paren; }
};

struct StructLitExpr : Expr {                // Point{.x = 1, .y = 2}
    std::vector<std::string> type_parts;
    std::vector<std::pair<std::string, ExprP>> fields;
    Kind kind() const override { return Kind::StructLit; }
};

struct AsCastExpr : Expr {                   // n as i32 / c as int
    ExprP operand;
    TypeUse target;
    Kind kind() const override { return Kind::AsCast; }
};

struct ListLitExpr : Expr {                  // [1, 2, 3]
    std::vector<ExprP> elements;
    Kind kind() const override { return Kind::ListLit; }
};

// ── 语句 ────────────────────────────────────────────────────────────
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Stmt : Node {
    enum Kind { ExprStmt, Return, Var, If, While, For, Break, Continue, Block };
    virtual Kind kind() const = 0;
    bool no_check = false;                   // @unchecked / @unsafe 注解
};

struct ExprStmt : Stmt {
    ExprP expr;
    Kind kind() const override { return Kind::ExprStmt; }
};

struct ReturnStmt : Stmt {
    ExprP value;                             // 可空 = return;
    Kind kind() const override { return Kind::Return; }
};

struct VarStmt : Stmt {                      // 局部变量声明(三种形式)
    std::string name;
    bool is_const = false;
    bool has_type = false;
    TypeUse type;
    ExprP init;
    Kind kind() const override { return Kind::Var; }
};

struct IfStmt : Stmt {
    ExprP cond;
    StmtP then_block;
    StmtP else_block;                        // BlockStmt 或 IfStmt(else-if 链)
    Kind kind() const override { return Kind::If; }
};

struct WhileStmt : Stmt {
    ExprP cond;
    StmtP body;
    Kind kind() const override { return Kind::While; }
};

struct ForStmt : Stmt {
    std::string var;
    bool is_range = false;
    bool inclusive = false;                  // ..=
    ExprP range_begin, range_end;            // is_range 时有效
    ExprP iterable;                          // 非 range 时有效
    StmtP body;
    Kind kind() const override { return Kind::For; }
};

struct BreakStmt : Stmt    { Kind kind() const override { return Kind::Break; } };
struct ContinueStmt : Stmt { Kind kind() const override { return Kind::Continue; } };

struct BlockStmt : Stmt {
    std::vector<StmtP> stmts;
    Kind kind() const override { return Kind::Block; }
};

// ── 顶层声明 ────────────────────────────────────────────────────────
enum class ParamMode { In, Inout, Out, Move, Copy, Forward };

struct Param {
    int line = 0;
    std::string name;
    ParamMode mode = ParamMode::In;
    TypeUse type;
    ExprP default_value;                     // 可空
};

struct FuncDecl {
    int line = 0;
    bool exported = false;
    std::string name;
    std::vector<Param> params;
    std::optional<TypeUse> ret;
    bool throws = false;
    bool has_block_body = false;
    StmtP block_body;                        // BlockStmt
    ExprP expr_body;                         // 简短体
};

struct FieldDecl {
    int line = 0;
    std::string name;
    bool is_const = false;
    TypeUse type;
    ExprP init;
};

struct MethodDecl {                          // 类型成员函数;name=="destructor" 为析构
    int line = 0;
    std::string name;
    std::vector<Param> params;
    std::optional<TypeUse> ret;
    bool throws = false;
    bool mutates = false;
    bool has_block_body = false;
    StmtP block_body;
    ExprP expr_body;
    bool uses_members = false;               // sema 填写:引用了成员 → 非 static
};

struct StructDecl {
    int line = 0;
    bool exported = false;
    std::string name;
    std::optional<TypeUse> base;             // 公有继承基类
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;

    FieldDecl* find_field(std::string const& n) {
        for (auto& f : fields) if (f.name == n) return &f;
        return nullptr;
    }
    MethodDecl* find_method(std::string const& n) {
        for (auto& m : methods) if (m.name == n) return &m;
        return nullptr;
    }
};

struct EnumDecl {
    int line = 0;
    bool exported = false;
    std::string name;
    std::optional<TypeUse> underlying;       // 底层类型
    std::vector<std::string> members;
};

struct GlobalVar {
    int line = 0;
    bool exported = false;
    std::string name;
    bool is_const = false;
    bool has_type = false;
    TypeUse type;
    ExprP init;
};

struct ImportDecl {
    int line = 0;
    std::vector<std::string> module_parts;
};

struct Module {
    std::string name;                        // 可空(省略时 = 文件名)
    int name_line = 0;
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<GlobalVar> globals;
    std::vector<FuncDecl> funcs;

    StructDecl* find_struct(std::string const& n) {
        for (auto& s : structs) if (s.name == n) return &s;
        return nullptr;
    }
    EnumDecl* find_enum(std::string const& n) {
        for (auto& e : enums) if (e.name == n) return &e;
        return nullptr;
    }
    FuncDecl* find_func(std::string const& n) {
        for (auto& f : funcs) if (f.name == n) return &f;
        return nullptr;
    }
    GlobalVar* find_global(std::string const& n) {
        for (auto& g : globals) if (g.name == n) return &g;
        return nullptr;
    }
};

} // namespace cpp2::ast
