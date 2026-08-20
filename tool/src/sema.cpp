// C++2 语义分析实现(M2b:类型推断 + 表注解)
#include "sema.hpp"

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

class Checker {
public:
    explicit Checker(ast::Module& m) : m_(m) {}

    Result run()
    {
        register_top();

        for (auto& s : m_.structs) check_struct(s);
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
    Result res_;
    std::unordered_map<std::string, Sym> top_;
    std::vector<std::unordered_map<std::string, Sym>> scopes_;
    ast::StructDecl* cur_struct_ = nullptr;
    ast::MethodDecl* cur_method_ = nullptr;

    void err(int line, int col, std::string msg)  { res_.errors.push_back({line, col, std::move(msg)}); }
    void warn(int line, int col, std::string msg) { res_.warnings.push_back({line, col, std::move(msg)}); }

    // ── 顶层登记(顺序无关:先全部登记再检查)───────────────────────
    void register_top()
    {
        auto add = [&](std::string const& n, Sym s, int line) {
            if (!top_.emplace(n, s).second)
                err(line, 0, "duplicate top-level name '" + n + "'");
        };
        for (auto& s : m_.structs) add(s.name, Sym{}, s.line);
        for (auto& e : m_.enums)   add(e.name, Sym{}, e.line);
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
            if (auto s = scalar(last); s.known()) return s;
            if (last == "unique" || last == "shared" || last == "weak"
                || last == "unique_ptr" || last == "shared_ptr" || last == "weak_ptr")
            { t = smart(); t.is_const = tu.is_const; return t; }
            if (last == "vector" || last == "list" || last == "array"
                || last == "span" || last == "map" || last == "set")
            { t = container(); t.is_const = tu.is_const; return t; }
            if (last == "void") return t;      // Unknown
            if (m_.find_struct(last)) {
                t.kind = Type::NamedStruct; t.name = last;
                return t;
            }
            if (m_.find_enum(last)) {
                t.kind = Type::NamedEnum; t.name = last;
                return t;
            }
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
    }

    void gather_fields(ast::StructDecl& s, std::vector<ast::FieldDecl*>& out)
    {
        if (s.base && s.base->parts.size() == 1) {
            if (auto* b = m_.find_struct(s.base->parts[0])) gather_fields(*b, out);
        }
        for (auto& f : s.fields) out.push_back(&f);
    }

    void check_method(ast::StructDecl& s, ast::MethodDecl& md)
    {
        cur_struct_ = &s;
        cur_method_ = &md;

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
        if (md.ret) type_from_use(*md.ret);

        if (md.has_block_body && md.block_body) check_stmt(*md.block_body);
        if (md.expr_body) infer(*md.expr_body);

        cur_struct_ = nullptr;
        cur_method_ = nullptr;
    }

    void check_func(ast::FuncDecl& f)
    {
        cur_struct_ = nullptr;
        cur_method_ = nullptr;

        scopes_.clear();
        scopes_.emplace_back();                 // 参数作用域
        for (auto& p : f.params) {
            Sym sym;
            sym.kind = SymKind::Param;
            sym.type = type_from_use(p.type);
            sym.is_const = p.mode == ast::ParamMode::In;
            scopes_.back()[p.name] = sym;
        }
        if (f.ret) type_from_use(*f.ret);
        if (f.has_block_body && f.block_body) check_stmt(*f.block_body);
        if (f.expr_body) infer(*f.expr_body);
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
            infer(*static_cast<ast::ExprStmt&>(s).expr);
            return;
        }
        case ast::Stmt::Return: {
            auto& r = static_cast<ast::ReturnStmt&>(s);
            if (r.value) infer(*r.value);
            return;
        }
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(s);
            Type init_t = v.init ? infer(*v.init) : Type{};
            if (v.init && v.init->kind() == ast::Expr::ListLit && !v.has_type)
                err(v.line, v.col, "list literals need an explicit type or ':='");
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
            infer(*i.cond);
            check_stmt(*i.then_block);
            if (i.else_block) check_stmt(*i.else_block);
            return;
        }
        case ast::Stmt::While: {
            auto& w = static_cast<ast::WhileStmt&>(s);
            infer(*w.cond);
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
        }
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

    // ── 表达式推断 ──────────────────────────────────────────────────
    Type infer(ast::Expr& e)
    {
        Type t = infer_inner(e);
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
            infer(*a.value);
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
                err(l.line, l.col, "cannot infer element type of an empty list literal");
                return {};
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
            if (auto* ed = m_.find_enum(n.parts[0])) {
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
            return sym->type;
        }
        if (m_.find_struct(n.parts[0]) || m_.find_enum(n.parts[0]))
            err(n.line, n.col, "use of type '" + n.parts[0] + "' where a value is expected");
        else
            err(n.line, n.col, "use of undeclared name '" + n.parts[0] + "'");
        return {};
    }

    Type infer_call(ast::CallExpr& c)
    {
        for (auto& a : c.args) infer(*a);

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

        // 模块函数:返回类型
        if (c.callee->kind() == ast::Expr::Name) {
            auto& name = static_cast<ast::NameExpr&>(*c.callee);
            if (!name.qualified()) {
                if (auto* sym = resolve(name.parts[0])) {
                    note_member_use(*sym);
                    if (sym->kind == SymKind::Func && sym->func && sym->func->ret)
                        return type_from_use(*sym->func->ret);
                }
                return {};
            }
            return {};
        }

        // 方法调用:obj.method(...)
        if (c.callee->kind() == ast::Expr::Member) {
            auto& mem = static_cast<ast::MemberExpr&>(*c.callee);
            return infer_member(mem, &c);
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
            if (auto* sd = m_.find_struct(obj.name)) {
                if (auto* fd = sd->find_field(mem.name)) {
                    Type t = type_from_use(fd->type);
                    t.is_const = t.is_const || base.is_const;   // const 传播
                    return t;
                }
                if (auto* md = sd->find_method(mem.name)) {
                    if (call) {
                        // mutates 方法不能作用在 const 接收者上
                        if (md->mutates && (base.is_const || receiver_is_const(*mem.base)))
                            err(mem.line, mem.col,
                                "cannot call mutates method '" + mem.name
                                + "' on a const value");
                    }
                    if (md->ret) return type_from_use(*md->ret);
                    return {};
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
        auto* sd = m_.find_struct(tn);
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

Result check(ast::Module& m)
{
    return Checker(m).run();
}

} // namespace cpp2::sema
