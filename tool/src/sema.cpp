// C++2 语义分析实现(M2d:泛型/concept、variant、optional、模式匹配、UFCS)
#include "sema.hpp"

#include <functional>
#include <unordered_set>

namespace cpp2::sema {

namespace {

enum class SymKind { Local, Param, Field, Method, Global, Func };

struct Sym {
    Type type;
    bool is_const = false;
    SymKind kind = SymKind::Local;
    ast::FuncDecl* func = nullptr;          // kind == Func
    ast::MethodDecl* method = nullptr;      // kind == Method
};

// expected/optional 包装:throws 调用结果 / T?
Type as_wrapped(Type t, bool optional)
{
    Type w;
    if (optional) w.is_optional = true; else w.is_expected = true;
    w.value = std::make_shared<Type>(std::move(t));
    return w;
}

Type as_expected(Type t) { return as_wrapped(std::move(t), false); }

// TypeUse 的规范化键(模式匹配 / variant 替身比对):限定名 + 递归实参 + '?'
std::string type_use_key(ast::TypeUse const& tu)
{
    if (tu.parts.empty()) return "";
    std::string s = tu.parts[0];
    for (size_t i = 1; i < tu.parts.size(); ++i) s += "::" + tu.parts[i];
    if (!tu.args.empty()) {
        s += "<";
        for (size_t i = 0; i < tu.args.size(); ++i) {
            if (i) s += ",";
            s += type_use_key(tu.args[i]);
        }
        s += ">";
    }
    if (tu.is_optional) s += "?";
    return s;
}

// UFCS 首参比对的接收者键:与源码类型写法对齐
std::string base_key(Type const& t)
{
    switch (t.kind) {
    case Type::NamedStruct:
    case Type::NamedEnum:
    case Type::Variant:
    case Type::Container:
        return t.name;
    default:
        return t.display();
    }
}

class Checker {
public:
    Checker(ast::Module& m, std::vector<ast::Module*> imported)
        : m_(m), imported_(std::move(imported)) {}

    Result run()
    {
        // 0. 类型表:自身全部 + 直接 import 的导出(DESIGN §3.3:import 非传递)
        for (auto& s : m_.structs) {
            if (!structs_.emplace(s.name, &s).second)
                err(s.line, 0, "duplicate type name '" + s.name + "'");
        }
        for (auto& e : m_.enums) {
            if (!enums_.emplace(e.name, &e).second)
                err(e.line, 0, "duplicate enum name '" + e.name + "'");
        }
        for (auto& v : m_.variants) {
            if (!variants_.emplace(v.name, &v).second)
                err(v.line, 0, "duplicate variant name '" + v.name + "'");
        }
        for (auto& c : m_.concepts) {
            if (!concepts_.emplace(c.name, &c).second)
                err(c.line, 0, "duplicate concept name '" + c.name + "'");
        }
        for (auto* im : imported_) {
            for (auto& s : im->structs)
                if (s.exported) structs_.emplace(s.name, &s);
            for (auto& e : im->enums)
                if (e.exported) enums_.emplace(e.name, &e);
            for (auto& v : im->variants)
                if (v.exported) variants_.emplace(v.name, &v);
            for (auto& c : im->concepts)
                if (c.exported) concepts_.emplace(c.name, &c);
            for (auto& f : im->funcs) {
                if (!f.exported) continue;
                Sym s; s.kind = SymKind::Func; s.func = &f;
                top_.emplace(f.name, s);
            }
            for (auto& g : im->globals) {
                if (!g.exported) continue;
                Sym s; s.kind = SymKind::Global;
                s.type = type_from_use(g.type);
                s.is_const = g.is_const;
                top_.emplace(g.name, s);
            }
        }

        register_top();

        for (auto& s : m_.structs) check_struct(s);
        for (auto& v : m_.variants) check_variant(v);
        for (auto& c : m_.concepts) check_concept(c);
        for (auto& f : m_.funcs)   check_func(f);
        for (auto& g : m_.globals) {
            if (g.init) infer(*g.init);
        }

        if (!m_.find_func("main"))
            warn(m_.name_line, 0, "no 'main' function found; output is a library translation unit");

        return std::move(res_);
    }

private:
    ast::Module& m_;
    std::vector<ast::Module*> imported_;
    std::unordered_map<std::string, ast::StructDecl*> structs_;   // 自身 + 导入导出
    std::unordered_map<std::string, ast::EnumDecl*> enums_;
    std::unordered_map<std::string, ast::VariantDecl*> variants_;
    std::unordered_map<std::string, ast::ConceptDecl*> concepts_;
    Result res_;
    std::unordered_map<std::string, Sym> top_;
    std::vector<std::unordered_map<std::string, Sym>> scopes_;
    ast::StructDecl* cur_struct_ = nullptr;
    ast::MethodDecl* cur_method_ = nullptr;
    bool cur_throws_ = false;               // 当前函数在错误通道上(可用 '?')
    bool prop_ok_ = false;                  // 当前表达式位置允许 '?'(语句顶层)
    bool in_post_ = false;                  // 在 post 契约内(old() 合法,result 可见)
    // 当前函数的类型参数名(检查函数体/调用点时 T 解析为 Generic)
    std::unordered_set<std::string> generic_names_;

    // ── M5 生存期 Lite ──────────────────────────────────────────────
    // L2:已 move 的名字。语句线性近似:调用实参 move 后标记,'=' 重赋值
    // 复活;分支内 move 保守延续;不做跨函数/循环迭代展开
    std::unordered_set<std::string> moved_;
    // L1:当前函数签名中 (in s: string) 形参名 —— 返回其视图即悬垂
    std::unordered_set<std::string> in_string_params_;
    bool cur_ret_is_view_ = false;              // 返回类型为 string_view

    ast::StructDecl* find_struct(std::string const& n) {
        auto it = structs_.find(n);
        return it != structs_.end() ? it->second : nullptr;
    }
    ast::EnumDecl* find_enum(std::string const& n) {
        auto it = enums_.find(n);
        return it != enums_.end() ? it->second : nullptr;
    }
    ast::VariantDecl* find_variant(std::string const& n) {
        auto it = variants_.find(n);
        return it != variants_.end() ? it->second : nullptr;
    }
    ast::ConceptDecl* find_concept(std::string const& n) {
        auto it = concepts_.find(n);
        return it != concepts_.end() ? it->second : nullptr;
    }

    void err(int line, int col, std::string msg)  { res_.errors.push_back({line, col, std::move(msg)}); }
    void warn(int line, int col, std::string msg) { res_.warnings.push_back({line, col, std::move(msg)}); }

    // ── 顶层登记(顺序无关:先全部登记再检查)───────────────────────
    void register_top()
    {
        auto add = [&](std::string const& n, Sym s, int line) {
            if (!top_.emplace(n, s).second)
                err(line, 0, "duplicate top-level name '" + n + "'");
        };
        for (auto& f : m_.funcs) {
            Sym s; s.kind = SymKind::Func; s.func = &f;
            add(f.name, s, f.line);
        }
        for (auto& g : m_.globals) {
            Sym s; s.kind = SymKind::Global;
            s.type = type_from_use(g.type);
            s.is_const = g.is_const;
            add(g.name, s, g.line);
        }
    }

    // ── 类型解析 ────────────────────────────────────────────────────
    Type type_from_use(ast::TypeUse const& tu)
    {
        Type t = type_from_use_core(tu);
        if (tu.is_optional) return as_wrapped(std::move(t), /*optional*/true);
        return t;
    }

    Type type_from_use_core(ast::TypeUse const& tu)
    {
        Type t;
        t.is_const = tu.is_const;
        if (tu.parts.empty()) return t;

        auto last = tu.parts.back();

        auto scalar = [&](std::string const& n) -> Type {
            Type r;
            if (n == "int")    r.kind = Type::Int;
            else if (n == "i8") r.kind = Type::I8;
            else if (n == "i16") r.kind = Type::I16;
            else if (n == "i32" || n == "int32_t") r.kind = Type::I32;
            else if (n == "i64" || n == "int64_t") r.kind = Type::I64;
            else if (n == "u8") r.kind = Type::U8;
            else if (n == "u16") r.kind = Type::U16;
            else if (n == "u32" || n == "uint32_t") r.kind = Type::U32;
            else if (n == "u64" || n == "uint64_t") r.kind = Type::U64;
            else if (n == "double") r.kind = Type::Double;
            else if (n == "float") r.kind = Type::Float;
            else if (n == "bool") r.kind = Type::Bool;
            else if (n == "char") r.kind = Type::Char;
            else if (n == "string") r.kind = Type::String;
            else if (n == "string_view") r.kind = Type::StringView;
            return r;
        };
        auto smart = [&]() -> Type {
            Type r;
            r.kind = Type::SmartPtr;
            if (!tu.args.empty())
                r.pointee = std::make_shared<Type>(type_from_use(tu.args[0]));
            return r;
        };
        auto container = [&]() -> Type {
            Type r;
            r.kind = Type::Container;
            r.name = last;
            if (!tu.args.empty())
                r.element = std::make_shared<Type>(type_from_use(tu.args[0]));
            return r;
        };

        if (tu.parts.size() == 1) {
            if (last == "self") return t;            // concept 接口里的占位类型
            if (generic_names_.count(last)) {        // 泛型参数 T(当前函数的)
                t.kind = Type::Generic;
                t.name = last;
                return t;
            }
            if (auto s = scalar(last); s.known()) return s;
            if (last == "unique" || last == "shared" || last == "weak"
                || last == "unique_ptr" || last == "shared_ptr" || last == "weak_ptr")
            { t = smart(); t.is_const = tu.is_const; return t; }
            if (last == "vector" || last == "list" || last == "array"
                || last == "span" || last == "map" || last == "set")
            { t = container(); t.is_const = tu.is_const; return t; }
            if (last == "void") return t;      // Unknown
            if (find_struct(last)) {
                t.kind = Type::NamedStruct; t.name = last;
                return t;
            }
            if (find_enum(last)) {
                t.kind = Type::NamedEnum; t.name = last;
                return t;
            }
            if (find_variant(last)) {
                t.kind = Type::Variant;
                t.name = last;
                return t;
            }
            if (find_concept(last))
                err(tu.line, 0, "concept '" + last + "' is a constraint, not a value type");
            if (last == "_") return t;
            err(tu.line, 0, "unknown type '" + last + "'");
            return t;
        }

        // 限定名(std::...):按最后一段映射,未知的放行(交由编译器解析)
        if (tu.parts[0] == "std") {
            if (auto s = scalar(last); s.known()) { s.is_const = tu.is_const; return s; }
            if (last == "vector" || last == "array" || last == "span")
            { t = container(); t.is_const = tu.is_const; return t; }
            if (last == "unique_ptr" || last == "shared_ptr" || last == "weak_ptr")
            { t = smart(); t.is_const = tu.is_const; return t; }
        }
        return t; // Unknown
    }

    // ── 结构体检查 ──────────────────────────────────────────────────
    void check_struct(ast::StructDecl& s)
    {
        // 字段类型解析 + 重名
        std::unordered_set<std::string> names;
        for (auto& f : s.fields) {
            if (!names.insert(f.name).second)
                err(f.line, 0, "duplicate field '" + f.name + "' in type '" + s.name + "'");
            f.type.line = f.type.line ? f.type.line : f.line;
            type_from_use(f.type);
            if (f.init) infer(*f.init);
        }
        std::unordered_set<std::string> mnames;
        for (auto& md : s.methods) {
            if (!mnames.insert(md.name).second)
                err(md.line, 0, "duplicate method '" + md.name + "' in type '" + s.name + "'");
        }
        for (auto& md : s.methods) check_method(s, md);

        // 类型不变量:bool 表达式,成员作用域内推断(字段可见;禁止 old/result)
        if (s.invariant) {
            cur_struct_ = &s;
            cur_method_ = nullptr;
            cur_throws_ = false;
            scopes_.clear();
            scopes_.emplace_back();
            std::vector<ast::FieldDecl*> fields;
            gather_fields(s, fields);
            for (auto* f : fields) {
                Sym sym;
                sym.kind = SymKind::Field;
                sym.type = type_from_use(f->type);
                sym.is_const = f->is_const;
                scopes_.back()[f->name] = sym;
            }
            Type t = infer(*s.invariant);
            if (t.known() && t.kind != Type::Bool)
                err(s.invariant_line, 0, "invariant must be a bool expression");
        }
    }

    void gather_fields(ast::StructDecl& s, std::vector<ast::FieldDecl*>& out)
    {
        if (s.base && s.base->parts.size() == 1) {
            if (auto* b = find_struct(s.base->parts[0])) gather_fields(*b, out);
        }
        for (auto& f : s.fields) out.push_back(&f);
    }

    // 字段查找沿基类链(派生同名隐藏基类;DESIGN §5.1 公有继承的成员访问)
    ast::FieldDecl* find_field_deep(ast::StructDecl& s, std::string const& n)
    {
        if (auto* f = s.find_field(n)) return f;
        if (s.base && s.base->parts.size() == 1)
            if (auto* b = find_struct(s.base->parts[0]))
                return find_field_deep(*b, n);
        return nullptr;
    }

    void check_variant(ast::VariantDecl& v)
    {
        for (auto& alt : v.alternatives) type_from_use(alt);
    }

    // concept 检查:要求签名可解析(self 占位类型);语义满足由降低后的
    // C++20 concept 在实例化点判定(v0.1 委托,见 IMPLEMENTATION 偏差表)
    void check_concept(ast::ConceptDecl& c)
    {
        for (auto& req : c.reqs) {
            for (auto& p : req.params) type_from_use(p.type);
            if (req.ret) type_from_use(*req.ret);
        }
    }

    // 概念约束存在性:<T: C> 与 requires 子句(std:: 前缀交由编译器)
    void check_constraint_name(std::vector<std::string> const& parts, int line)
    {
        if (parts.empty() || parts[0] == "std") return;
        if (!find_concept(parts[0]))
            err(line, 0, "unknown concept '" + parts[0]
                         + "' (concepts are declared with 'name: concept = {...}')");
    }

    void check_method(ast::StructDecl& s, ast::MethodDecl& md)
    {
        cur_struct_ = &s;
        cur_method_ = &md;
        cur_throws_ = md.throws;
        if (md.name == "destructor" && (md.pre || md.post))
            err(md.line, 0, "destructor cannot have pre/post contracts");

        scopes_.clear();
        scopes_.emplace_back();                 // 成员作用域(字段 + 方法)
        std::vector<ast::FieldDecl*> fields;
        gather_fields(s, fields);
        for (auto* f : fields) {
            Sym sym;
            sym.kind = SymKind::Field;
            sym.type = type_from_use(f->type);
            sym.is_const = f->is_const;
            scopes_.back()[f->name] = sym;
        }
        for (auto& om : s.methods) {
            if (&om == &md) continue;
            Sym sym;
            sym.kind = SymKind::Method;
            sym.method = &om;
            scopes_.back()[om.name] = sym;
        }

        scopes_.emplace_back();                 // 参数作用域
        for (auto& p : md.params) {
            Sym sym;
            sym.kind = SymKind::Param;
            sym.type = type_from_use(p.type);
            sym.is_const = p.mode == ast::ParamMode::In;   // in = 只读
            scopes_.back()[p.name] = sym;
        }
        // M5 生存期 Lite:方法级状态复位 + L1 视图参数登记
        moved_.clear();
        in_string_params_.clear();
        cur_ret_is_view_ = md.ret
            && type_from_use(*md.ret).kind == Type::StringView;
        for (auto& p : md.params)
            if (p.mode == ast::ParamMode::In
                && type_from_use(p.type).kind == Type::String)
                in_string_params_.insert(p.name);

        if (md.ret) type_from_use(*md.ret);
        if (md.pre) infer(*md.pre);
        if (md.post) check_post(*md.post, md.ret);

        if (md.has_block_body && md.block_body) check_stmt(*md.block_body);
        if (md.expr_body) {
            infer_top(*md.expr_body);                 // 短体可为 match 表达式 / '?'
            check_view_escape(*md.expr_body);
        }

        cur_struct_ = nullptr;
        cur_method_ = nullptr;
    }

    void check_func(ast::FuncDecl& f)
    {
        cur_struct_ = nullptr;
        cur_method_ = nullptr;
        cur_throws_ = f.throws;
        if (f.name == "main" && f.throws)
            err(f.line, 0, "main cannot be 'throws' (errors must be handled in main)");
        if (f.name == "main" && !f.type_params.empty())
            err(f.line, 0, "main cannot be generic");

        // 泛型环境:函数体内 T → Generic;约束概念必须存在
        for (auto& tp : f.type_params) check_constraint_name(tp.concept_parts, tp.line);
        for (auto& r : f.requires_list) check_constraint_name(r.name_parts, f.line);
        for (auto& tp : f.type_params) generic_names_.insert(tp.name);

        scopes_.clear();
        scopes_.emplace_back();                 // 参数作用域
        for (auto& p : f.params) {
            Sym sym;
            sym.kind = SymKind::Param;
            sym.type = type_from_use(p.type);
            sym.is_const = p.mode == ast::ParamMode::In;
            scopes_.back()[p.name] = sym;
        }
        // M5 生存期 Lite:函数级状态复位 + L1 视图参数登记
        moved_.clear();
        in_string_params_.clear();
        cur_ret_is_view_ = f.ret
            && type_from_use(*f.ret).kind == Type::StringView;
        for (auto& p : f.params)
            if (p.mode == ast::ParamMode::In
                && type_from_use(p.type).kind == Type::String)
                in_string_params_.insert(p.name);

        if (f.ret) type_from_use(*f.ret);
        if (f.pre) infer(*f.pre);
        if (f.post) check_post(*f.post, f.ret);
        if (f.has_block_body && f.block_body) check_stmt(*f.block_body);
        if (f.expr_body) {
            infer_top(*f.expr_body);
            check_view_escape(*f.expr_body);    // 短体同样受 L1 约束
        }

        for (auto& tp : f.type_params) generic_names_.erase(tp.name);
    }

    // M5-L1:返回表达式若是 in string 参数的裸名 → 视图悬垂
    void check_view_escape(ast::Expr& e)
    {
        if (!cur_ret_is_view_ || in_string_params_.empty()) return;
        if (e.kind() == ast::Expr::Name
            && !static_cast<ast::NameExpr&>(e).qualified()
            && in_string_params_.count(static_cast<ast::NameExpr&>(e).parts[0]))
            err(e.line, e.col,
                "returning view of 'in' parameter '"
                + static_cast<ast::NameExpr&>(e).parts[0]
                + "' dangles after return (M5-L1)");
    }

    // post 契约:old() 合法、result 绑定返回值(DESIGN §6.5)
    void check_post(ast::Expr& post, std::optional<ast::TypeUse> const& ret)
    {
        scopes_.emplace_back();
        if (ret) {
            Sym s;
            s.type = type_from_use(*ret);
            s.is_const = true;
            s.kind = SymKind::Local;
            scopes_.back()["result"] = s;
        }
        bool prev = in_post_;
        in_post_ = true;
        infer(post);
        in_post_ = prev;
        scopes_.pop_back();
    }

    // ── 语句 ────────────────────────────────────────────────────────
    void check_stmt(ast::Stmt& s)
    {
        switch (s.kind()) {
        case ast::Stmt::Block: {
            auto& b = static_cast<ast::BlockStmt&>(s);
            scopes_.emplace_back();
            for (auto& st : b.stmts) check_stmt(*st);
            scopes_.pop_back();
            return;
        }
        case ast::Stmt::ExprStmt: {
            auto& x = static_cast<ast::ExprStmt&>(s);
            Type t = infer_top(*x.expr);    // 裸 f()?; = 检查 + 传播,丢弃值
            if (x.expr->kind() == ast::Expr::Call && t.is_expected)
                err(x.line, x.col,
                    "unhandled error-channel call; handle it with '?', '!', 'or', match, or if-let");
            return;
        }
        case ast::Stmt::Return: {
            auto& r = static_cast<ast::ReturnStmt&>(s);
            if (r.value) {
                Type t = infer_top(*r.value);
                if (t.is_expected && !cur_throws_)
                    err(r.line, r.col,
                        "cannot return an error-channel value from a function that is not 'throws'");
                check_view_escape(*r.value);    // M5-L1
            }
            return;
        }
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(s);
            Type init_t = v.init ? infer_top(*v.init) : Type{};
            if (init_t.is_expected)
                err(v.line, v.col,
                    "unhandled error-channel value; handle it with '?', '!', 'or', match, or if-let");
            if (!v.has_type && v.init
                && v.init->kind() == ast::Expr::ListLit
                && static_cast<ast::ListLitExpr&>(*v.init).elements.empty())
                err(v.line, v.col, "empty list literal '{}' needs an explicit type");
            Type t = v.has_type ? type_from_use(v.type) : init_t;
            t.is_const = v.is_const;
            if (v.has_type && v.init && v.init->kind() == ast::Expr::ListLit) {
                auto& lit = static_cast<ast::ListLitExpr&>(*v.init);
                for (auto& el : lit.elements) infer(*el);
            }
            declare(v.name, t, v.is_const, SymKind::Local, v.line, v.col);
            return;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(s);
            if (i.is_let()) {
                // if-let:x := f() { } else e := it { }(DESIGN §8.3);
                // M2d:亦接受 T?(DESIGN §6.4:if p := w.lock() { ... })
                Type t = infer(*i.let_init);
                if (!t.is_expected && !t.is_optional)
                    err(i.line, i.col,
                        "if-let initializer must be on the error channel (a 'throws' call) "
                        "or an optional (T?)");
                if (!i.else_binding.empty() && t.is_optional)
                    err(i.line, i.col,
                        "'else NAME := it' binds an error; optionals have no error value");
                scopes_.emplace_back();
                declare(i.let_name, t.val(), true, SymKind::Local, i.line, i.col);
                check_stmt(*i.then_block);
                scopes_.pop_back();
                if (i.else_block) {
                    scopes_.emplace_back();
                    if (!i.else_binding.empty())
                        declare(i.else_binding, Type::of(Type::Error), true,
                                SymKind::Local, i.line, i.col);
                    check_stmt(*i.else_block);
                    scopes_.pop_back();
                }
                return;
            }
            Type ct = infer(*i.cond);
            if (ct.is_expected)
                err(i.line, i.col,
                    "if condition is an unhandled error-channel value; use if-let, match, '?', '!' or 'or'");
            check_stmt(*i.then_block);
            if (i.else_block) check_stmt(*i.else_block);
            return;
        }
        case ast::Stmt::While: {
            auto& w = static_cast<ast::WhileStmt&>(s);
            Type ct = infer(*w.cond);
            if (ct.is_expected)
                err(w.line, w.col, "while condition is an unhandled error-channel value");
            check_stmt(*w.body);
            return;
        }
        case ast::Stmt::For: {
            auto& f = static_cast<ast::ForStmt&>(s);
            scopes_.emplace_back();
            if (f.is_range) {
                infer(*f.range_begin);
                infer(*f.range_end);
                declare(f.var, Type::of(Type::Int), false, SymKind::Local, f.line, f.col);
            } else {
                Type it = infer(*f.iterable);
                Type elem = it.kind == Type::Container ? it.elem()
                          : it.is_indexable() ? Type::of(Type::Char)
                          : Type{};
                declare(f.var, elem, true, SymKind::Local, f.line, f.col);  // 只读迭代变量
            }
            check_stmt(*f.body);
            scopes_.pop_back();
            return;
        }
        case ast::Stmt::Break:
        case ast::Stmt::Continue:
            return;
        case ast::Stmt::Match: {
            auto& x = static_cast<ast::MatchStmt&>(s);
            Type t = infer(*x.scrutinee);
            check_match_common(x.line, x.col, t, x.arms, /*value_form*/false);
            return;
        }
        }
    }

    // ── 模式匹配(M2d,DESIGN §4.5/§5.4/§5.5/§8.3)────────────────────
    // 按 scrutinee 类型分派合法模式集,检查绑定/守卫/穷尽性;
    // value_form 时臂体为表达式(已包成 ExprStmt),返回公共值类型。
    Type check_match_common(int line, int col, Type t,
                            std::vector<ast::MatchArm>& arms, bool value_form)
    {
        ast::EnumDecl* ed = nullptr;
        ast::VariantDecl* vd = nullptr;
        ast::StructDecl* sd = nullptr;
        enum class Scrut { Expected, Optional, Enum, Variant, Struct, Bad } sc;
        if (t.is_expected)                          sc = Scrut::Expected;
        else if (t.is_optional)                     sc = Scrut::Optional;
        else if (t.kind == Type::NamedEnum)  { sc = Scrut::Enum;    ed = find_enum(t.name); }
        else if (t.kind == Type::Variant)    { sc = Scrut::Variant; vd = find_variant(t.name); }
        else if (t.kind == Type::NamedStruct){ sc = Scrut::Struct;  sd = find_struct(t.name); }
        else                                         sc = Scrut::Bad;

        if (sc == Scrut::Bad) {
            err(line, col,
                "match scrutinee must be an error-channel value, enum, variant, "
                "optional (T?), or struct; got '" + t.display() + "'");
            return {};
        }
        if (arms.empty()) {
            err(line, col, "match needs at least one arm");
            return {};
        }

        std::unordered_set<std::string> covered;      // 覆盖(含守卫臂)
        std::unordered_set<std::string> covered_strict; // 无守卫覆盖(再入 = 不可达)
        bool has_wild = false;
        Type value_t;
        bool have_value = false;

        for (size_t ai = 0; ai < arms.size(); ++ai) {
            auto& arm = arms[ai];
            Type bind_type;                          // 整体绑定(ok 值 / 替身值 / 自身)
            std::vector<std::pair<std::string, Type>> sub_binds;

            switch (arm.pat) {
            case ast::MatchArm::Pat::Wildcard:
                if (ai + 1 != arms.size())
                    err(arm.line, 0, "'_' must be the last match arm");
                has_wild = true;
                break;
            case ast::MatchArm::Pat::Ok:
            case ast::MatchArm::Pat::Err:
                if (sc != Scrut::Expected) {
                    err(arm.line, 0, "'ok'/'err' patterns require an error-channel scrutinee");
                } else {
                    bind_type = arm.pat == ast::MatchArm::Pat::Ok
                              ? t.val() : Type::of(Type::Error);
                    if (!covered.insert(arm.pat == ast::MatchArm::Pat::Ok ? "ok" : "err").second)
                        err(arm.line, 0, "duplicate 'ok'/'err' match arm");
                }
                break;
            case ast::MatchArm::Pat::Some:
                if (sc != Scrut::Optional) {
                    err(arm.line, 0, "'some' patterns require an optional (T?) scrutinee");
                } else {
                    bind_type = t.val();
                    if (!covered.insert("some").second)
                        err(arm.line, 0, "duplicate 'some' match arm");
                }
                break;
            case ast::MatchArm::Pat::None:
                if (sc != Scrut::Optional)
                    err(arm.line, 0, "'none' patterns require an optional (T?) scrutinee");
                else if (!covered.insert("none").second)
                    err(arm.line, 0, "duplicate 'none' match arm");
                break;
            case ast::MatchArm::Pat::EnumMember:
                if (sc != Scrut::Enum) {
                    err(arm.line, 0, "'.member' patterns require an enum scrutinee");
                } else if (ed) {
                    bool known = false;
                    for (auto& mem : ed->members) known = known || mem == arm.enum_member;
                    if (!known)
                        err(arm.line, 0, "enum '" + ed->name + "' has no member '"
                                         + arm.enum_member + "'");
                    // 同一成员多次出现:仅"无守卫臂后跟无守卫臂"(不可达)报错
                    if (!arm.guard && !covered_strict.insert(arm.enum_member).second)
                        err(arm.line, 0, "duplicate match arm for '." + arm.enum_member + "'");
                    covered.insert(arm.enum_member);
                }
                break;
            case ast::MatchArm::Pat::TypePat: {
                std::string key = type_use_key(arm.type_pattern);
                if (sc == Scrut::Variant && vd) {
                    bool found = false;
                    for (auto& alt : vd->alternatives) {
                        if (type_use_key(alt) != key) continue;
                        found = true;
                        bool destr_ok = false;      // 替身是结构体才能解构
                        if (!arm.sub.empty()) {
                            if (alt.parts.size() == 1)
                                if (auto* as = find_struct(alt.parts[0])) {
                                    sub_binds = validate_subs(arm, *as);
                                    destr_ok = true;
                                }
                            if (!destr_ok)
                                err(arm.line, 0, "only struct alternatives can be destructured");
                        }
                        if (sub_binds.empty() && !arm.binding.empty() && arm.binding != "_")
                            bind_type = type_from_use(alt);
                        break;
                    }
                    if (!found)
                        err(arm.line, 0, "variant '" + vd->name
                                         + "' has no alternative '" + key + "'");
                    else {
                        if (!arm.guard && !covered_strict.insert(key).second)
                            err(arm.line, 0,
                                "duplicate match arm for alternative '" + key + "'");
                        covered.insert(key);
                    }
                } else if (sc == Scrut::Struct && sd) {
                    if (key != sd->name)
                        err(arm.line, 0, "type pattern for a struct scrutinee must be '"
                                         + sd->name + "' itself");
                    if (!arm.sub.empty()) sub_binds = validate_subs(arm, *sd);
                    if (sub_binds.empty() && !arm.binding.empty() && arm.binding != "_")
                        bind_type = t;
                    covered.insert(sd->name);
                } else {
                    err(arm.line, 0,
                        "type patterns require a variant or struct scrutinee");
                }
                break;
            }
            }

            scopes_.emplace_back();                 // 臂作用域:绑定 + 守卫 + 体
            if (!arm.binding.empty() && arm.binding != "_" && bind_type.known())
                declare(arm.binding, bind_type, true, SymKind::Local, arm.line, 0);
            for (auto& [n, ty] : sub_binds)
                declare(n, ty, true, SymKind::Local, arm.line, 0);

            if (arm.guard) {
                Type g = infer(*arm.guard);
                if (g.known() && g.kind != Type::Bool)
                    err(arm.line, 0, "match guard must be a bool expression");
            }

            if (!value_form) {
                check_stmt(*arm.body);
            } else if (arm.body->kind() == ast::Stmt::Block) {
                check_stmt(*arm.body);
            } else if (arm.body->kind() == ast::Stmt::ExprStmt) {
                Type bt = infer(*static_cast<ast::ExprStmt&>(*arm.body).expr);
                if (bt.known()) {
                    if (!have_value) { value_t = bt; have_value = true; }
                    else if (value_t.known()) {
                        Type merged = arm_common_type(value_t, bt);
                        if (!merged.known())
                            err(arm.line, 0,
                                "match arms must all produce the same type ('"
                                + value_t.display() + "' vs '" + bt.display() + "')");
                        value_t = merged;
                    }
                }
            }
            scopes_.pop_back();
        }

        // 穷尽性:通配兜底,否则全部候选必须被覆盖
        auto missing = [&]() -> std::string {
            std::string m;
            auto need = [&](std::string const& k) {
                if (!covered.count(k)) m += (m.empty() ? "" : ", ") + k;
            };
            switch (sc) {
            case Scrut::Expected: need("ok"); need("err"); break;
            case Scrut::Optional: need("some"); need("none"); break;
            case Scrut::Enum:    if (ed) for (auto& mem : ed->members) need(mem); break;
            case Scrut::Variant: if (vd) { for (auto& alt : vd->alternatives)
                                             need(type_use_key(alt)); } break;
            case Scrut::Struct:  if (sd) need(sd->name); break;
            case Scrut::Bad: break;
            }
            return m;
        };
        if (!has_wild && !missing().empty())
            err(line, col, "match arms must be exhaustive (or end with '_'); missing: "
                 + missing());

        return value_t;
    }

    // 解构子模式:位置绑定(Rect(w, h))与命名字段(Point(.x, .y));返回绑定表。
    // 就地把 arm.sub 归一化为 ".字段=绑定名"(发射侧无需再查字段序,导入类型同样可用)
    // 臂间公共类型:同型;均为算术 → 宽化(0 与 2.5 统一为 double)
    Type arm_common_type(Type const& a, Type const& b)
    {
        if (a.display() == b.display()) return a;
        if (a.is_arith() && b.is_arith()) {
            if (a.is_floaty() || b.is_floaty()) return Type::of(Type::Double);
            if (a.is_signed() && b.is_signed())
                return a.signed_rank() >= b.signed_rank() ? a : b;
            if (a.is_unsigned() && b.is_unsigned())
                return a.signed_rank() >= b.signed_rank() ? a : b;
        }
        return {};
    }

    std::vector<std::pair<std::string, Type>> validate_subs(ast::MatchArm& arm,
                                                            ast::StructDecl& sd)
    {
        std::vector<std::pair<std::string, Type>> binds;
        for (size_t i = 0; i < arm.sub.size(); ++i) {
            std::string const& sp = arm.sub[i];
            if (sp == "_") continue;
            if (sp[0] == '.') {
                std::string field = sp.substr(1);
                if (auto* fd = find_field_deep(sd, field)) {
                    binds.push_back({field, type_from_use(fd->type)});
                    arm.sub[i] = "." + field + "=" + field;
                } else
                    err(arm.line, 0, "type '" + sd.name + "' has no field '" + field + "'");
            } else {
                if (i >= sd.fields.size()) {
                    err(arm.line, 0, "type '" + sd.name + "' has only "
                                     + std::to_string(sd.fields.size())
                                     + " field(s); too many sub-patterns");
                    continue;
                }
                binds.push_back({sp, type_from_use(sd.fields[i].type)});
                arm.sub[i] = "." + sd.fields[i].name + "=" + sp;
            }
        }
        return binds;
    }

    void declare(std::string const& name, Type t, bool is_const, SymKind k, int line, int col)
    {
        if (scopes_.empty()) scopes_.emplace_back();
        if (scopes_.back().count(name))
            err(line, col, "redeclaration of '" + name + "' in the same scope");
        Sym s; s.type = t; s.is_const = is_const; s.kind = k;
        scopes_.back()[name] = s;
    }

    Sym* resolve(std::string const& name)
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        auto t = top_.find(name);
        return t != top_.end() ? &t->second : nullptr;
    }

    // 解析命中字段/方法 → 标记 uses_members(决定是否 static)
    void note_member_use(Sym& sym)
    {
        if (cur_method_ && (sym.kind == SymKind::Field || sym.kind == SymKind::Method))
            cur_method_->uses_members = true;
    }

    // M5-L2:调用实参中的 move x → 标记(操作数此时已推断,不会自伤)
    void note_call_moves(ast::Expr& e)
    {
        if (e.kind() != ast::Expr::Call) return;
        auto& c = static_cast<ast::CallExpr&>(e);
        for (auto& arg : c.args) {
            if (arg->kind() != ast::Expr::Unary) continue;
            auto& u = static_cast<ast::UnaryExpr&>(*arg);
            if (u.op != "move") continue;
            if (u.operand->kind() == ast::Expr::Name
                && !static_cast<ast::NameExpr&>(*u.operand).qualified())
                moved_.insert(static_cast<ast::NameExpr&>(*u.operand).parts[0]);
        }
    }

    // ── 表达式推断 ──────────────────────────────────────────────────
    // infer:嵌套位置——'?' 在此不合法(机械展开需要语句级拆分)
    Type infer(ast::Expr& e)
    {
        bool prev = prop_ok_;
        prop_ok_ = false;
        Type t = infer_inner(e);
        note_call_moves(e);
        prop_ok_ = prev;
        res_.expr_types[&e] = t;
        return t;
    }

    // infer_top:语句顶层(变量初始化 / return / 赋值右值)——允许整层 '?'
    Type infer_top(ast::Expr& e)
    {
        bool prev = prop_ok_;
        prop_ok_ = true;
        Type t = infer_inner(e);
        note_call_moves(e);
        prop_ok_ = prev;
        res_.expr_types[&e] = t;
        return t;
    }

    Type infer_inner(ast::Expr& e)
    {
        switch (e.kind()) {
        case ast::Expr::Literal: {
            auto& l = static_cast<ast::LiteralExpr&>(e);
            switch (l.lit) {
            case ast::LitKind::Int:    return Type::of(Type::Int);
            case ast::LitKind::Double: return Type::of(Type::Double);
            case ast::LitKind::String: return Type::of(Type::String);
            case ast::LitKind::Char:   return Type::of(Type::Char);
            case ast::LitKind::Bool:   return Type::of(Type::Bool);
            }
            return {};
        }
        case ast::Expr::Name:
            return infer_name(static_cast<ast::NameExpr&>(e));
        case ast::Expr::Paren:
            return infer(*static_cast<ast::ParenExpr&>(e).inner);
        case ast::Expr::Call:
            return infer_call(static_cast<ast::CallExpr&>(e));
        case ast::Expr::Binary:
            return infer_binary(static_cast<ast::BinaryExpr&>(e));
        case ast::Expr::Unary: {
            auto& u = static_cast<ast::UnaryExpr&>(e);
            Type t = infer(*u.operand);
            if (u.op == "!") return Type::of(Type::Bool);
            return t;
        }
        case ast::Expr::Assign: {
            auto& a = static_cast<ast::AssignExpr&>(e);
            check_assign_target(*a.target, a.line, a.col);
            if (a.op != "=" && a.value->kind() == ast::Expr::Try)
                err(a.line, a.col,
                    "'?' with compound assignment is not supported; unwrap into a variable first");
            // M5-L2:plain '=' 重赋值复活被 move 的名字(复合赋值是使用,已在上报错)
            if (a.op == "=" && a.target->kind() == ast::Expr::Name
                && !static_cast<ast::NameExpr&>(*a.target).qualified())
                moved_.erase(static_cast<ast::NameExpr&>(*a.target).parts[0]);
            Type vt = infer_top(*a.value);
            if (vt.is_expected)
                err(a.line, a.col,
                    "unhandled error-channel value; handle it with '?', '!', or 'or'");
            return infer_existing(*a.target);
        }
        case ast::Expr::Index: {
            auto& x = static_cast<ast::IndexExpr&>(e);
            Type b = infer(*x.base);
            infer(*x.index);
            if (b.kind == Type::Container) return b.elem();
            if (b.kind == Type::String || b.kind == Type::StringView)
                return Type::of(Type::Char);
            return {};                              // 未知基类型:放行
        }
        case ast::Expr::Member:
            return infer_member(static_cast<ast::MemberExpr&>(e), nullptr);
        case ast::Expr::StructLit:
            return infer_struct_lit(static_cast<ast::StructLitExpr&>(e));
        case ast::Expr::AsCast: {
            auto& x = static_cast<ast::AsCastExpr&>(e);
            infer(*x.operand);
            return type_from_use(x.target);
        }
        case ast::Expr::ListLit: {
            auto& l = static_cast<ast::ListLitExpr&>(e);
            if (l.elements.empty()) {
                // 空 {}:元素类型由声明侧决定(list<int> x = {})
                Type c;
                c.kind = Type::Container;
                c.name = "list";
                return c;
            }
            Type first = infer(*l.elements[0]);
            for (size_t i = 1; i < l.elements.size(); ++i) {
                Type t = infer(*l.elements[i]);
                if (t.kind != first.kind)
                    err(l.elements[i]->line, l.elements[i]->col,
                        "heterogeneous list literal ('" + first.display()
                        + "' vs '" + t.display() + "')");
            }
            Type c;
            c.kind = Type::Container;
            c.name = "list";
            c.element = std::make_shared<Type>(first);
            return c;
        }
        case ast::Expr::Try: {
            auto& x = static_cast<ast::TryExpr&>(e);
            if (!prop_ok_)
                err(x.line, x.col,
                    "'?' is only supported as a whole initializer, assignment value, "
                    "return value, or statement in this milestone");
            Type t = infer(*x.operand);
            if (!t.is_expected)
                err(x.line, x.col,
                    "'?' requires a call result on the error channel (a 'throws' function)");
            if (!cur_throws_)
                err(x.line, x.col,
                    "'?' propagation requires the enclosing function to be 'throws'");
            return t.val();
        }
        case ast::Expr::Must: {
            auto& x = static_cast<ast::MustExpr&>(e);
            Type t = infer(*x.operand);
            if (!t.is_expected)
                err(x.line, x.col,
                    "'!' requires a call result on the error channel (a 'throws' function)");
            return t.val();
        }
        case ast::Expr::OrDefault: {
            auto& x = static_cast<ast::OrDefaultExpr&>(e);
            Type l = infer(*x.lhs);
            Type r = infer(*x.rhs);
            if (!l.is_expected)
                err(x.line, x.col,
                    "'or' default requires an error-channel left side (a 'throws' call)");
            (void)r;
            return l.val();
        }
        case ast::Expr::Match: {
            // match 产值(DESIGN §5.5):与 '?' 同样只支持语句级位置
            // (整 initializer / return / 赋值右值 / 函数体),机械展开需要拆语句
            auto& x = static_cast<ast::MatchExpr&>(e);
            if (!prop_ok_)
                err(x.line, x.col,
                    "match as a value is only supported as a whole initializer, "
                    "assignment value, return value, or function body in this milestone");
            Type t = infer(*x.scrutinee);
            return check_match_common(x.line, x.col, t, x.arms, /*value_form*/true);
        }
        case ast::Expr::Lambda: {
            // (x: int) -> int = x * x(DESIGN §4.6);闭包类型交由 C++ 推导
            auto& l = static_cast<ast::LambdaExpr&>(e);
            scopes_.emplace_back();
            for (auto& p : l.params) {
                Sym sym;
                sym.kind = SymKind::Param;
                sym.type = type_from_use(p.type);
                sym.is_const = p.mode == ast::ParamMode::In;
                scopes_.back()[p.name] = sym;
            }
            if (l.ret) type_from_use(*l.ret);
            if (l.has_block_body && l.block_body) check_stmt(*l.block_body);
            if (l.expr_body) infer(*l.expr_body);
            scopes_.pop_back();
            return {};
        }
        }
        return {};
    }

    Type infer_existing(ast::Expr& e)
    {
        auto it = res_.expr_types.find(&e);
        return it != res_.expr_types.end() ? it->second : infer(e);
    }

    Type infer_name(ast::NameExpr& n)
    {
        if (n.qualified()) {
            // Color::green / Color::red
            if (auto* ed = find_enum(n.parts[0])) {
                for (auto& mem : ed->members)
                    if (mem == n.parts[1]) {
                        Type t = Type::of(Type::NamedEnum);
                        t.name = ed->name;
                        return t;
                    }
                err(n.line, n.col, "enum '" + ed->name + "' has no member '" + n.parts[1] + "'");
                Type t = Type::of(Type::NamedEnum);
                t.name = ed->name;
                return t;
            }
            return {};                              // std::... 交由桥接/编译器
        }
        if (auto* sym = resolve(n.parts[0])) {
            note_member_use(*sym);
            if (moved_.count(n.parts[0]))       // M5-L2:move 后使用
                err(n.line, n.col, "'" + n.parts[0]
                    + "' used after being moved");
            return sym->type;
        }
        if (n.parts[0] == "none")                    // 空 optional 字面量(DESIGN §6.4)
            return Type::of(Type::NoneVal);
        if (find_struct(n.parts[0]) || find_enum(n.parts[0]) || find_variant(n.parts[0]))
            err(n.line, n.col, "use of type '" + n.parts[0] + "' where a value is expected");
        else
            err(n.line, n.col, "use of undeclared name '" + n.parts[0] + "'");
        return {};
    }

    Type infer_call(ast::CallExpr& c)
    {
        for (auto& a : c.args) infer(*a);

        // 契约/错误内建:old(...) / err(...)(用户同名声明优先)
        if (c.callee->kind() == ast::Expr::Name) {
            auto& name = static_cast<ast::NameExpr&>(*c.callee);
            if (!name.qualified()) {
                if (name.parts[0] == "old" && !resolve("old")) {
                    if (!in_post_)
                        err(c.line, c.col, "'old(...)' is only valid in 'post' conditions");
                    if (c.args.size() != 1)
                        err(c.line, c.col, "'old(...)' takes exactly one argument");
                    else
                        return infer_existing(*c.args[0]);
                    return {};
                }
                if (name.parts[0] == "err" && !resolve("err")) {
                    if (c.args.size() != 1)
                        err(c.line, c.col, "'err(...)' takes exactly one message argument");
                    else if (Type t = infer_existing(*c.args[0]);
                             t.kind != Type::String && t.kind != Type::StringView)
                        err(c.line, c.col, "'err(...)' message must be a string");
                    Type t;
                    t.kind = Type::ErrVal;
                    return t;
                }
            }
        }

        // make_unique/make_shared 工厂
        if (c.callee->kind() == ast::Expr::Name && !c.args.empty()) {
            auto& name = static_cast<ast::NameExpr&>(*c.callee);
            bool is_make = (name.parts.size() == 1
                            && (name.parts[0] == "make_unique" || name.parts[0] == "make_shared"))
                        || (name.parts.size() == 2 && name.parts[0] == "std"
                            && (name.parts[1] == "make_unique" || name.parts[1] == "make_shared"));
            if (is_make) {
                Type t;
                t.kind = Type::SmartPtr;
                t.pointee = std::make_shared<Type>(infer_existing(*c.args[0]));
                return t;
            }
        }

        // 模块函数:返回类型(throws → 错误通道值;泛型 → 直位参数合一)
        if (c.callee->kind() == ast::Expr::Name) {
            auto& name = static_cast<ast::NameExpr&>(*c.callee);
            if (!name.qualified()) {
                if (auto* sym = resolve(name.parts[0])) {
                    note_member_use(*sym);
                    if (sym->kind == SymKind::Func && sym->func)
                        return infer_func_call(*sym->func, c);
                    if (sym->type.kind == Type::Generic)
                        return {};                 // 泛型值实参(如 lambda)可调用,C++ 判定
                    err(c.line, c.col,
                        "'" + name.parts[0] + "' is not a function");
                    return {};
                }
                // 限定名(std::…)交给 C++ 解析;非限定未知名 = 拼写/作用域错误
                err(c.line, c.col,
                    "call to undeclared function '" + name.parts[0] + "'");
                return {};
            }
            return {};
        }

        // 方法调用:obj.method(...);成员不存在时尝试 UFCS 自由函数(DESIGN §4.7)
        if (c.callee->kind() == ast::Expr::Member) {
            auto& mem = static_cast<ast::MemberExpr&>(*c.callee);
            Type t = infer_member(mem, &c);
            if (!t.known()) {
                if (Type u = try_ufcs(mem, c, type_of_expr(*mem.base)); u.known())
                    return u;
                if (res_.ufcs.count(&c)) return {};
            }
            return t;
        }
        return {};
    }

    Type type_of_expr(ast::Expr& e)
    {
        auto it = res_.expr_types.find(&e);
        return it != res_.expr_types.end() ? it->second : Type{};
    }

    // 泛型函数调用推断:临时泛型环境 + 直位合一(参数类型恰为 T 时绑定实参类型),
    // 返回类型做 T → 实参类型替换。嵌套容器型(vector<T>)不做深度合一(v0.1)。
    // 调用点实参相容性:刻意宽松——只拦"确定不兼容"(结构体名不符、
    // expected/optional 包装不符),算术宽化/字符串字面量/泛型一律放行
    // (收窄类问题由运行期检查兜底)。未知类型放行,避免误报。
    bool arg_assignable(Type const& param, Type const& arg)
    {
        if (!param.known() || !arg.known()) return true;
        if (param.kind == Type::Generic || arg.kind == Type::Generic) return true;
        if (param.display() == arg.display()) return true;
        if (param.is_arith() && arg.is_arith()) return true;
        auto stry = [](Type const& t) {
            return t.kind == Type::String || t.kind == Type::StringView;
        };
        if (stry(param) && stry(arg)) return true;
        // struct → variant 候选隐式转换(DESIGN §5.5)
        if (param.kind == Type::Variant && arg.kind == Type::NamedStruct) {
            if (auto* vd = find_variant(param.name))
                for (auto& alt : vd->alternatives)
                    if (!alt.parts.empty() && alt.parts.back() == arg.name)
                        return true;
            return false;
        }
        if (param.is_expected == arg.is_expected && param.is_optional == arg.is_optional
            && param.value && arg.value)
            return arg_assignable(*param.value, *arg.value);
        return false;
    }

    void check_call_args(std::string const& name, std::vector<ast::Param>& params,
                         ast::CallExpr& c, ast::FuncDecl* fd = nullptr)
    {
        size_t required = 0;
        for (auto& p : params)
            if (!p.default_value) ++required;
        if (c.args.size() < required || c.args.size() > params.size()) {
            std::string expect = std::to_string(required);
            if (required != params.size())
                expect += " to " + std::to_string(params.size());
            err(c.line, c.col,
                "function '" + name + "' expects " + expect + " argument(s), got "
                + std::to_string(c.args.size()));
            return;
        }
        // 被调函数自身的类型参数(T)不在此环境解析(避免 unknown type 误报):
        // 形参类型任意位置(含 vector<T> 等嵌套实参)引用 T 时整参放行
        std::unordered_set<std::string> own_tp;
        if (fd)
            for (auto& t : fd->type_params) own_tp.insert(t.name);
        auto mentions_tp = [&](ast::TypeUse const& t) -> bool {
            if (own_tp.empty()) return false;
            std::function<bool(ast::TypeUse const&)> walk =
                [&](ast::TypeUse const& u) {
                    for (auto& p : u.parts)
                        if (own_tp.count(p)) return true;
                    for (auto& a : u.args)
                        if (walk(a)) return true;
                    return false;
                };
            return walk(t);
        };
        for (size_t i = 0; i < c.args.size() && i < params.size(); ++i) {
            if (mentions_tp(params[i].type))
                continue;
            Type pt = type_from_use(params[i].type);
            Type at = type_of_expr(*c.args[i]);
            if (!arg_assignable(pt, at))
                err(c.line, c.col,
                    "argument " + std::to_string(i + 1) + " of '" + name
                    + "' expects '" + pt.display() + "', got '"
                    + (at.known() ? at.display() : "?") + "'");
        }
    }

    Type infer_func_call(ast::FuncDecl& fd, ast::CallExpr& c)
    {
        check_call_args(fd.name, fd.params, c, &fd);
        if (fd.type_params.empty()) {
            if (!fd.ret) return {};
            Type t = type_from_use(*fd.ret);
            if (fd.throws) return as_expected(t);
            return t;
        }

        for (auto& tp : fd.type_params) generic_names_.insert(tp.name);
        std::unordered_map<std::string, Type> subst;
        for (size_t i = 0; i < fd.params.size() && i < c.args.size(); ++i) {
            auto& p = fd.params[i];
            if (p.type.parts.size() != 1 || p.type.args.size() > 1) continue;
            bool is_tp = false;
            for (auto& tp : fd.type_params)
                is_tp = is_tp || tp.name == p.type.parts[0];
            if (!is_tp) continue;
            Type at = type_of_expr(*c.args[i]);
            if (at.known() && !at.is_expected && !at.is_optional
                && !subst.count(p.type.parts[0]))
                subst[p.type.parts[0]] = at;
        }
        Type t = fd.ret ? type_from_use(*fd.ret) : Type{};
        if (t.kind == Type::Generic) {
            auto it = subst.find(t.name);
            if (it != subst.end()) {
                t = it->second;
                t.is_const = false;
            }
        }
        for (auto& tp : fd.type_params) generic_names_.erase(tp.name);
        if (fd.throws) return as_expected(t);
        return t;
    }

    // UFCS:x.f(args) → f(x, args)(DESIGN §4.7)。模块级自由函数首参类型
    // 匹配接收者(或泛型首参);标量 .to_string() 桥到 std::to_string。
    Type try_ufcs(ast::MemberExpr& mem, ast::CallExpr& call, Type const& base)
    {
        if (base.is_expected || base.is_optional) return {};
        auto* sym = resolve(mem.name);
        if (sym && sym->kind == SymKind::Func && sym->func
            && !sym->func->params.empty()) {
            auto* fd = sym->func;
            auto& p0 = fd->params[0];
            bool ok = false;
            if (fd->type_params.empty()) {
                ok = !type_use_key(p0.type).empty()
                     && type_use_key(p0.type) == base_key(base);
            } else {
                for (auto& tp : fd->type_params)
                    if (p0.type.parts.size() == 1 && tp.name == p0.type.parts[0])
                        ok = true;
            }
            if (ok) {
                res_.ufcs[&call] = fd->name;
                return infer_func_call(*fd, call);
            }
        }
        if (mem.name == "to_string"
            && (base.is_arith() || base.kind == Type::Char)) {
            res_.ufcs[&call] = "std::to_string";
            return Type::of(Type::String);
        }
        return {};
    }

    // base.name 的类型;call 非 nullptr 时做 mutates/const 检查并返回方法返回类型
    Type infer_member(ast::MemberExpr& mem, ast::CallExpr* call)
    {
        Type base = infer(*mem.base);

        Type obj = base;
        if (obj.is_smart()) obj = obj.deref();      // 智能指针自动解引用
        if (obj.kind == Type::NamedStruct) {
            if (auto* sd = find_struct(obj.name)) {
                if (auto* fd = find_field_deep(*sd, mem.name)) {
                    Type t = type_from_use(fd->type);
                    t.is_const = t.is_const || base.is_const;   // const 传播
                    return t;
                }
                if (auto* md = sd->find_method(mem.name)) {
                    if (!call) {
                        // 方法不是值:成员访问位置引用方法(如赋值目标)→ 干净诊断
                        err(mem.line, mem.col,
                            "'" + mem.name + "' is a method, not a value");
                        return {};
                    }
                    // mutates 方法不能作用在 const 接收者上
                    if (md->mutates && (base.is_const || receiver_is_const(*mem.base)))
                        err(mem.line, mem.col,
                            "cannot call mutates method '" + mem.name
                            + "' on a const value");
                    check_call_args(mem.name, md->params, *call);
                    if (md->ret) {
                        Type t = type_from_use(*md->ret);
                        if (md->throws) return as_expected(t);
                        return t;
                    }
                    return {};
                }
                if (call) {                          // UFCS:x.f(y) ≡ f(x, y)
                    if (Type u = try_ufcs(mem, *call, obj); u.known())
                        return u;
                    if (res_.ufcs.count(call)) return {};
                }
                err(mem.line, mem.col,
                    "type '" + obj.name + "' has no member '" + mem.name + "'");
            }
            return {};
        }
        infer(*mem.base);                           // 确保 base 已注解(已注解时幂等)
        return {};                                  // 未知对象类型:放行
    }

    bool receiver_is_const(ast::Expr& base)
    {
        if (base.kind() == ast::Expr::Name && !static_cast<ast::NameExpr&>(base).qualified()) {
            auto& n = static_cast<ast::NameExpr&>(base);
            if (auto* sym = resolve(n.parts[0])) return sym->is_const;
        }
        return false;
    }

    Type infer_struct_lit(ast::StructLitExpr& lit)
    {
        for (auto& [name, val] : lit.fields) infer(*val);

        if (lit.type_parts.size() != 1) return {};  // std 聚合:放行
        std::string const& tn = lit.type_parts[0];
        auto* sd = find_struct(tn);
        if (!sd) {
            err(lit.line, lit.col, "unknown type '" + tn + "' in struct literal");
            return {};
        }
        std::unordered_set<std::string> seen;
        for (auto& [name, val] : lit.fields) {
            if (!seen.insert(name).second)
                err(lit.line, lit.col, "duplicate field '" + name + "' in struct literal");
            if (!sd->find_field(name))
                err(lit.line, lit.col,
                    "type '" + tn + "' has no field '" + name + "'");
        }
        Type t;
        t.kind = Type::NamedStruct;
        t.name = tn;
        return t;
    }

    void check_assign_target(ast::Expr& target, int line, int col)
    {
        if (target.kind() == ast::Expr::Name) {
            auto& n = static_cast<ast::NameExpr&>(target);
            if (!n.qualified()) {
                if (auto* sym = resolve(n.parts[0])) {
                    note_member_use(*sym);
                    if (sym->is_const)
                        err(line, col, "cannot assign to const '" + n.parts[0] + "'");
                    if (sym->kind == SymKind::Param && sym->is_const)
                        err(line, col, "cannot assign to 'in' parameter '" + n.parts[0] + "'");
                    return;
                }
                err(line, col, "use of undeclared name '" + n.parts[0] + "'");
            }
            return;
        }
        if (target.kind() == ast::Expr::Member) {
            auto& m = static_cast<ast::MemberExpr&>(target);
            if (m.base->kind() == ast::Expr::Name
                && !static_cast<ast::NameExpr&>(*m.base).qualified()) {
                auto& n = static_cast<ast::NameExpr&>(*m.base);
                if (auto* sym = resolve(n.parts[0])) {
                    note_member_use(*sym);
                    if (sym->is_const)
                        err(m.line, m.col,
                            "cannot modify member '" + m.name + "' of const '" + n.parts[0] + "'");
                }
            }
        }
    }

    Type infer_binary(ast::BinaryExpr& b)
    {
        Type l = infer(*b.lhs);
        Type r = infer(*b.rhs);

        if (l.is_expected || r.is_expected) {
            err(b.line, b.col,
                "cannot use an error-channel value in a binary operation; "
                "handle it first ('?', '!', 'or', match, if-let)");
            return {};
        }

        if (b.op == "&&" || b.op == "||"
         || b.op == "==" || b.op == "!="
         || b.op == "<" || b.op == ">" || b.op == "<=" || b.op == ">=")
            return Type::of(Type::Bool);

        if (b.op == "+" && (l.kind == Type::String || r.kind == Type::String))
            return Type::of(Type::String);

        if (!l.is_arith() || !r.is_arith()) return {};

        if (l.is_floaty() || r.is_floaty())
            return Type::of(Type::Double);
        if (l.is_signed() && r.is_signed())
            return l.signed_rank() >= r.signed_rank() ? l : r;
        if (l.is_unsigned() && r.is_unsigned())
            return l.signed_rank() >= r.signed_rank() ? l : r;
        return {};                                   // 混号:放行(不注入检查)
    }
};

} // namespace

Result check(ast::Module& m, std::vector<ast::Module*> const& imported)
{
    return Checker(m, imported).run();
}

} // namespace cpp2::sema
