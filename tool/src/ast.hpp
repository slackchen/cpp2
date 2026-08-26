// C++2 AST(M2d:泛型/concept、variant、T?、模式匹配扩展、lambda、UFCS)
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
    bool is_optional = false;                // T? → std::optional<T>(M2d,DESIGN §6.4)
    bool is_pointer = false;                 // T* → 裸指针(M6,仅 @unsafe 内可产生)
    std::vector<std::string> parts;          // 限定名,如 {std, string}
    std::vector<TypeUse> args;               // 泛型实参

    bool empty() const { return parts.empty(); }
};

// ── 表达式 ──────────────────────────────────────────────────────────
struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Expr : Node {
    enum Kind { Literal, Name, Call, Binary, Unary, Assign, Index, Member, Paren,
                StructLit, AsCast, ListLit,
                Try, OrDefault, Must,
                Match, Lambda };
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

// match 臂:模式 + 可选守卫 + 体(表达式臂在解析层包成 ExprStmt)
struct MatchArm {
    enum class Pat { Ok, Err, Wildcard, EnumMember, TypePat, Some, None };
    int line = 0;
    Pat pat = Pat::Wildcard;
    std::string enum_member;                 // Pat::EnumMember:.red
    TypeUse type_pattern;                    // Pat::TypePat:int n / Circle / vector<Value>
    std::string binding;                     // 绑定名("_" = 忽略;空 = 无绑定)
    std::vector<std::string> sub;            // 解构子模式:".field" / 名字(按位)/ "_"
    ExprP guard;                             // 可空:pattern if guard => body
    StmtP body;                              // 块或单条语句 / 表达式(ExprStmt 包装)
};

struct MatchExpr : Expr {                    // match v { pat => expr; ... } — 产值(DESIGN §5.5)
    ExprP scrutinee;
    std::vector<MatchArm> arms;
    Kind kind() const override { return Kind::Match; }
};

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

// MatchArm 定义在表达式区(match 表达式与语句共用)

struct MatchStmt : Stmt {                    // match v { pattern [if guard] => body; ... }
    ExprP scrutinee;                         // 错误通道 / enum / variant / optional / struct
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

struct LambdaExpr : Expr {                   // (x: int) -> int = x * x(DESIGN §4.6)
    std::vector<std::string> captures;       // 显式捕获:"=" / "&" / "x" / "&x"(空 = 无捕获)
    std::vector<Param> params;
    std::optional<TypeUse> ret;
    bool has_block_body = false;
    StmtP block_body;
    ExprP expr_body;
    Kind kind() const override { return Kind::Lambda; }
};

// 泛型参数:<T: Concept>(DESIGN §5.6)
struct TypeParam {
    int line = 0;
    std::string name;
    std::vector<std::string> concept_parts;  // 约束概念(限定名;空 = 无约束)
};

// requires 子句项:Ordered<T> && Printable<T>
struct RequiresItem {
    std::vector<std::string> name_parts;
    std::vector<std::string> args;           // 类型参数名
};

struct FuncDecl {
    int line = 0;
    bool exported = false;
    std::string name;
    std::vector<TypeParam> type_params;      // <T: Concept>(M2d)
    std::vector<RequiresItem> requires_list; // requires A<T> && B<T>(M2d)
    bool is_extern = false;                  // 无体声明:name: (...)->ret;(M6 互操作)
    bool c_linkage = false;                  // extern "C" 前缀:C 链接(std 直连 CRT/OS);
                                             // 非 c_linkage 的无体声明按 C++ 链接(cxx_legacy 配对)
    bool builtin = false;                    // builtin 前缀:编译器内建原语(无体,不发原型)。
                                             // 平台差异由各后端直接实现(headers=rt cpp2;
                                             // native=机器码/syscall)。std 专用,用户库不可注册。
    std::vector<std::string> error_categories; // throws E1, E2(M6 收口:存档+audit)
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
    std::vector<std::string> error_categories;   // throws E1, E2(M6 收口)
    bool mutates = false;
    bool is_virtual = false;                     // 虚分发(M7,DESIGN §5.2 扩展)
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
    ExprP invariant;                         // 类型不变量 `invariant: <expr>;`(DESIGN §6.5)
    int invariant_line = 0;                  // 公开方法出入口注入检查
    bool needs_virtual_dtor = false;         // 有 virtual 方法 → 基类析构自动虚化

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

struct VariantDecl {                         // Value: variant = { int, string }(DESIGN §5.5)
    int line = 0;
    bool exported = false;
    std::string name;
    std::vector<TypeUse> alternatives;       // 候选类型;match 是唯一合法访问方式
};

struct ConceptDecl {                         // Ordered: concept = { ... }(DESIGN §5.6)
    int line = 0;
    bool exported = false;
    std::string name;
    std::vector<MethodDecl> reqs;            // 接口要求(签名,无体)
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

struct LegacyBlock {                          // cxx_legacy { … }(M6,DESIGN §9.1)
    int line = 0;
    std::string code;                         // 原文(不含花括号),逐字复制到生成码
};

struct Module {
    std::string name;                        // 可空(省略时 = 文件名)
    int name_line = 0;
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<VariantDecl> variants;
    std::vector<ConceptDecl> concepts;
    std::vector<GlobalVar> globals;
    std::vector<FuncDecl> funcs;
    std::vector<LegacyBlock> legacy_blocks;  // cxx_legacy {} 原文(M6):逐字复制到生成码全局域

    StructDecl* find_struct(std::string const& n) {
        for (auto& s : structs) if (s.name == n) return &s;
        return nullptr;
    }
    EnumDecl* find_enum(std::string const& n) {
        for (auto& e : enums) if (e.name == n) return &e;
        return nullptr;
    }
    VariantDecl* find_variant(std::string const& n) {
        for (auto& v : variants) if (v.name == n) return &v;
        return nullptr;
    }
    ConceptDecl* find_concept(std::string const& n) {
        for (auto& c : concepts) if (c.name == n) return &c;
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
