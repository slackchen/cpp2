// C++2 安全审计实现(M4):遍历 AST,镜像 emit 的检查注入谓词
#include "audit.hpp"

namespace cpp2::audit {

int Report::unchecked() const
{
    int n = 0;
    for (auto const& o : opt_outs) if (!o.unsafe) ++n;
    return n;
}

int Report::unsafe() const
{
    int n = 0;
    for (auto const& o : opt_outs) if (o.unsafe) ++n;
    return n;
}

namespace {

class Walker {
public:
    explicit Walker(sema::Result const& r) : r_(r) {}

    Report run(ast::Module& m)
    {
        for (auto& s : m.structs) {
            for (auto& f : s.fields)
                if (f.init) expr(*f.init);
            for (auto& md : s.methods) {
                count_contracts(md.pre.get(), md.post.get());
                if (md.pre)  expr(*md.pre);
                if (md.post) expr(*md.post);
                if (md.has_block_body && md.block_body) stmt(*md.block_body);
                if (md.expr_body) expr(*md.expr_body);
            }
        }
        for (auto& g : m.globals)
            if (g.init) expr(*g.init);
        for (auto& f : m.funcs) {
            count_contracts(f.pre.get(), f.post.get());
            if (f.pre)  expr(*f.pre);
            if (f.post) expr(*f.post);
            if (f.has_block_body && f.block_body) stmt(*f.block_body);
            if (f.expr_body) expr(*f.expr_body);
        }
        return rep_;
    }

    void count_contracts(ast::Expr* pre, ast::Expr* post)
    {
        if (pre)  ++rep_.checked_contract;
        if (post) ++rep_.checked_contract;
    }

private:
    sema::Result const& r_;
    Report rep_;
    bool checks_on_ = true;

    sema::Type type_of(ast::Expr& e) const { return r_.type_of(e); }

    // 与 emit::type_from_target 相同的目标类型归类
    static sema::Type target_kind(ast::TypeUse const& tu)
    {
        std::string n = tu.parts.empty() ? "" : tu.parts.back();
        sema::Type t;
        if (n == "int" || n == "i8" || n == "i16" || n == "i32" || n == "i64"
         || n == "int32_t" || n == "int64_t") t.kind = sema::Type::Int;
        else if (n == "u8" || n == "u16" || n == "u32" || n == "u64"
              || n == "uint32_t" || n == "uint64_t") t.kind = sema::Type::U32;
        else if (n == "double" || n == "float") t.kind = sema::Type::Double;
        else if (n == "bool") t.kind = sema::Type::Bool;
        else if (n == "char") t.kind = sema::Type::Char;
        return t;
    }

    void stmt(ast::Stmt& s)
    {
        bool prev = checks_on_;
        if (s.no_check) {
            checks_on_ = false;
            rep_.opt_outs.push_back({s.line, s.is_unsafe});
        }

        switch (s.kind()) {
        case ast::Stmt::Block:
            for (auto& st : static_cast<ast::BlockStmt&>(s).stmts) stmt(*st);
            break;
        case ast::Stmt::ExprStmt: {
            auto& x = static_cast<ast::ExprStmt&>(s);
            // 复合赋值与 emit 相同:目标为有符号整型时按算术检查计
            if (x.expr->kind() == ast::Expr::Assign) {
                auto& a = static_cast<ast::AssignExpr&>(*x.expr);
                if (a.op != "=" && checks_on_ && type_of(*a.target).is_signed())
                    ++rep_.checked_arith;
            }
            expr(*x.expr);
            break;
        }
        case ast::Stmt::Return: {
            auto& r = static_cast<ast::ReturnStmt&>(s);
            if (r.value) expr(*r.value);
            break;
        }
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(s);
            if (v.init) expr(*v.init);
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(s);
            if (i.is_let()) {
                expr(*i.let_init);
            } else {
                expr(*i.cond);
            }
            stmt(*i.then_block);
            if (i.else_block) stmt(*i.else_block);
            break;
        }
        case ast::Stmt::While: {
            auto& w = static_cast<ast::WhileStmt&>(s);
            expr(*w.cond);
            stmt(*w.body);
            break;
        }
        case ast::Stmt::For: {
            auto& f = static_cast<ast::ForStmt&>(s);
            if (f.is_range) {
                expr(*f.range_begin);
                expr(*f.range_end);
            } else {
                expr(*f.iterable);
            }
            stmt(*f.body);
            break;
        }
        case ast::Stmt::Break:
        case ast::Stmt::Continue:
            break;
        case ast::Stmt::Match: {
            auto& x = static_cast<ast::MatchStmt&>(s);
            expr(*x.scrutinee);
            for (auto& arm : x.arms) {
                if (arm.guard) expr(*arm.guard);
                stmt(*arm.body);
            }
            break;
        }
        }

        checks_on_ = prev;
    }

    void expr(ast::Expr& e)
    {
        switch (e.kind()) {
        case ast::Expr::Literal:
        case ast::Expr::Name:
            break;
        case ast::Expr::Paren:
            expr(*static_cast<ast::ParenExpr&>(e).inner);
            break;
        case ast::Expr::Call: {
            auto& c = static_cast<ast::CallExpr&>(e);
            expr(*c.callee);
            for (auto& a : c.args) expr(*a);
            break;
        }
        case ast::Expr::Binary: {
            auto& b = static_cast<ast::BinaryExpr&>(e);
            if (checks_on_ && (b.op == "+" || b.op == "-" || b.op == "*"
                            || b.op == "/" || b.op == "%")) {
                sema::Type l = type_of(*b.lhs), r = type_of(*b.rhs);
                if (l.is_signed() && r.is_signed()) ++rep_.checked_arith;
            }
            expr(*b.lhs);
            expr(*b.rhs);
            break;
        }
        case ast::Expr::Unary:
            expr(*static_cast<ast::UnaryExpr&>(e).operand);
            break;
        case ast::Expr::Assign: {
            auto& a = static_cast<ast::AssignExpr&>(e);
            expr(*a.target);
            expr(*a.value);
            break;
        }
        case ast::Expr::Index: {
            auto& x = static_cast<ast::IndexExpr&>(e);
            if (checks_on_ && type_of(*x.base).is_indexable()) ++rep_.checked_index;
            expr(*x.base);
            expr(*x.index);
            break;
        }
        case ast::Expr::Member: {
            auto& x = static_cast<ast::MemberExpr&>(e);
            if (checks_on_ && type_of(*x.base).is_smart()) ++rep_.checked_deref;
            expr(*x.base);
            break;
        }
        case ast::Expr::StructLit: {
            auto& l = static_cast<ast::StructLitExpr&>(e);
            for (auto& [name, val] : l.fields) expr(*val);
            break;
        }
        case ast::Expr::AsCast: {
            auto& x = static_cast<ast::AsCastExpr&>(e);
            if (checks_on_ && type_of(*x.operand).is_arith()
                          && target_kind(x.target).is_arith())
                ++rep_.checked_narrow;
            expr(*x.operand);
            break;
        }
        case ast::Expr::ListLit: {
            auto& l = static_cast<ast::ListLitExpr&>(e);
            for (auto& el : l.elements) expr(*el);
            break;
        }
        case ast::Expr::Try:
            expr(*static_cast<ast::TryExpr&>(e).operand);
            break;
        case ast::Expr::OrDefault: {
            auto& x = static_cast<ast::OrDefaultExpr&>(e);
            expr(*x.lhs);
            expr(*x.rhs);
            break;
        }
        case ast::Expr::Must:
            expr(*static_cast<ast::MustExpr&>(e).operand);
            break;
        case ast::Expr::Match: {
            auto& x = static_cast<ast::MatchExpr&>(e);
            expr(*x.scrutinee);
            for (auto& arm : x.arms) {
                if (arm.guard) expr(*arm.guard);
                stmt(*arm.body);
            }
            break;
        }
        case ast::Expr::Lambda: {
            auto& l = static_cast<ast::LambdaExpr&>(e);
            for (auto& p : l.params)
                if (p.default_value) expr(*p.default_value);
            if (l.has_block_body && l.block_body) stmt(*l.block_body);
            if (l.expr_body) expr(*l.expr_body);
            break;
        }
        }
    }
};

} // namespace

Report report_for(ast::Module& m, sema::Result const& r)
{
    return Walker(r).run(m);
}

std::string format_section(std::string const& module_name, std::string const& file,
                           Report const& rep)
{
    std::string s = "== " + module_name + " (" + file + ") ==\n";
    s += "   checks : arith " + std::to_string(rep.checked_arith)
       + ", index "  + std::to_string(rep.checked_index)
       + ", deref "  + std::to_string(rep.checked_deref)
       + ", narrow " + std::to_string(rep.checked_narrow)
       + ", contract " + std::to_string(rep.checked_contract) + "\n";
    if (rep.opt_outs.empty()) {
        s += "   opt-out: none\n";
    } else {
        for (auto const& o : rep.opt_outs)
            s += std::string("   opt-out: @") + (o.unsafe ? "unsafe" : "unchecked")
               + " at line " + std::to_string(o.line) + "\n";
    }
    return s;
}

} // namespace cpp2::audit
