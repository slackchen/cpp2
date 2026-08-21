// C++2 AST(M2c:错误通道 ?/!/or/match/if-let、契约 pre/post)
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
                StructLit, AsCast, ListLit,
                Try, OrDefault, Must };
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

struct TryExpr : Expr {                      // f()? — 解包,失败向上传播(DESIGN §8.2)
    ExprP operand;
    Kind kind() const override { return Kind::Try; }
};

struct OrDefaultExpr : Expr {                // f() or "fallback" — 失败取默认(DESIGN §8.3)
    ExprP lhs, rhs;
    Kind kind() const override { return Kind::OrDefault; }
};

struct MustExpr : Expr {                     // f()! — 确信必成功,失败即 bug → trap
    ExprP operand;
    Kind kind() const override { return Kind::Must; }
};

// ── 语句 ────────────────────────────────────────────────────────────
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Stmt : Node {
    enum Kind { ExprStmt, Return, Var, If, While, For, Break, Continue, Block, Match };
    virtual Kind kind() const = 0;
    bool no_check = false;                   // @unchecked / @unsafe 注解
    bool is_unsafe = false;                  // 注解具体是 @unsafe(audit 区分两者)
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
    ExprP cond;                              // 普通形式
    std::string let_name;                    // if-let:x := f() { }(非空 = if-let,DESIGN §8.3)
    ExprP let_init;                          // if-let 的初始化表达式(错误通道值)
    StmtP then_block;
    StmtP else_block;                        // BlockStmt 或 IfStmt(else-if 链)
    std::string else_binding;                // else e := it { }:绑定错误值
    bool is_let() const { return !let_name.empty(); }
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

struct MatchArm {                            // ok NAME => / err NAME =>(M2c 子集)
    int line = 0;
    bool is_ok = false;
    std::string binding;                     // 绑定名("_" = 忽略)
    StmtP body;                              // 块或单条语句
};

struct MatchStmt : Stmt {                    // match f() { ok x => ...; err e => ...; }
    ExprP scrutinee;                         // 必须是错误通道值(sema 检查)
    std::vector<MatchArm> arms;
    Kind kind() const override { return Kind::Match; }
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
    ExprP pre;                               // 契约:入口检查(M2c)
    ExprP post;                              // 契约:出口检查;old()/result 在此可用
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
    ExprP pre;                               // 契约(M2c)
    ExprP post;
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
