// C++2 语法分析器实现(M2b)
#include "parser.hpp"

#include <stdexcept>

namespace cpp2::parse {

namespace {

class Parser {
public:
    Parser(std::vector<lex::Token> toks, std::string src_name)
        : toks_(std::move(toks)), src_name_(std::move(src_name)) {}

    ast::Module run()
    {
        ast::Module m;
        if (peek().tok == lex::Tok::Module) {
            m.name_line = peek().line;
            advance();
            m.name = dotted_name("module name");
            expect(lex::Tok::Semi, "';' after module declaration");
        }
        while (peek().tok == lex::Tok::Import) {
            ast::ImportDecl im;
            im.line = peek().line;
            advance();
            im.module_parts = dotted_name_parts("import module name");
            if (peek().tok == lex::Tok::Ident && peek().text == "as") {
                advance();
                dotted_name("import alias");        // M3:记录但不使用
            }
            expect(lex::Tok::Semi, "';' after import");
            m.imports.push_back(std::move(im));
        }
        while (peek().tok != lex::Tok::Eof) {
            top_decl(m);
        }
        return m;
    }

private:
    std::vector<lex::Token> toks_;
    std::string src_name_;
    size_t i_ = 0;
    // 条件/迭代位置守卫:禁止 `Ident{}` 解析为空 struct 字面量,
    // 否则 `if x { }` / `for e in items { }` 会被吞掉块括号
    bool cond_like_ = false;
    // 递归深度防护:fuzz/畸形输入下递归下降会爆栈(M4);
    // 超限按语法错误报告,而不是进程崩溃
    static constexpr int kMaxDepth = 200;
    int depth_ = 0;

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) {
            if (++p.depth_ > kMaxDepth) {
                --p.depth_;
                p.err("expression or statement nesting too deep");
            }
        }
        ~DepthGuard() { --p.depth_; }
    };

    // ── 基础设施 ────────────────────────────────────────────────────
    lex::Token const& peek(size_t ahead = 0) const
    {
        size_t k = i_ + ahead;
        return k < toks_.size() ? toks_[k] : toks_.back();
    }

    lex::Token advance()
    {
        lex::Token t = peek();
        if (i_ + 1 < toks_.size()) ++i_;
        return t;
    }

    bool check(lex::Tok t) const { return peek().tok == t; }

    bool accept(lex::Tok t)
    {
        if (check(t)) { advance(); return true; }
        return false;
    }

    void expect(lex::Tok t, char const* what)
    {
        if (!accept(t)) err(std::string("expected ") + what);
    }

    [[noreturn]] void err(std::string msg)
    {
        throw ParseError{peek().line, peek().col,
                         "at '" + peek().text + "': " + std::move(msg)};
    }

    void unsupported(char const* feature, char const* milestone)
    {
        throw ParseError{peek().line, peek().col,
                         std::string("'") + peek().text + "' (" + feature
                         + ") is not supported yet — arrives in " + milestone};
    }

    std::string qualified_name(char const* what)
    {
        auto parts = qualified_name_parts(what);
        std::string s = parts[0];
        for (size_t k = 1; k < parts.size(); ++k) s += "::" + parts[k];
        return s;
    }

    std::vector<std::string> qualified_name_parts(char const* what)
    {
        if (!check(lex::Tok::Ident)) err(std::string("expected ") + what);
        std::vector<std::string> parts;
        parts.push_back(advance().text);
        while (accept_double_colon()) {
            parts.push_back(require_ident());
        }
        return parts;
    }

    bool accept_double_colon()
    {
        if (check(lex::Tok::Colon) && peek(1).tok == lex::Tok::Colon) {
            advance();
            advance();
            return true;
        }
        return false;
    }

    std::string require_ident()
    {
        if (!check(lex::Tok::Ident)) err("expected identifier after '::'");
        return advance().text;
    }

    // 模块名是点分的:app.util
    std::vector<std::string> dotted_name_parts(char const* what)
    {
        if (!check(lex::Tok::Ident)) err(std::string("expected ") + what);
        std::vector<std::string> parts;
        parts.push_back(advance().text);
        while (check(lex::Tok::Dot) && peek(1).tok == lex::Tok::Ident) {
            advance();
            parts.push_back(advance().text);
        }
        return parts;
    }

    std::string dotted_name(char const* what)
    {
        auto parts = dotted_name_parts(what);
        std::string s = parts[0];
        for (size_t k = 1; k < parts.size(); ++k) s += "." + parts[k];
        return s;
    }

    // ── 顶层声明 ────────────────────────────────────────────────────
    void top_decl(ast::Module& m)
    {
        bool exported = false;
        if (accept(lex::Tok::Export)) exported = true;

        if (check(lex::Tok::At)) unsupported("@unsafe/@unchecked annotation", "M2b");
        if (check(lex::Tok::Module) || check(lex::Tok::Import))
            err("module/import declarations must appear before other declarations");

        if (!check(lex::Tok::Ident)) err("expected a declaration (name: kind = value)");
        if (peek(1).tok != lex::Tok::Colon) {
            if (peek().text == "cxx_legacy") unsupported("cxx_legacy block", "M6");
            err("expected ':' after declaration name");
        }

        // name ':' ... — 看第三个 token 分派 type / enum / 函数 / 变量
        lex::Tok after_colon = peek(2).tok;
        if (after_colon == lex::Tok::Type)             type_decl(m, exported);
        else if (after_colon == lex::Tok::Enum)        enum_decl(m, exported);
        else if (after_colon == lex::Tok::Variant)     unsupported("variant", "M2d");
        else if (after_colon == lex::Tok::Concept)     unsupported("concept", "M2d");
        else if (after_colon == lex::Tok::LParen)      func_decl(m, exported);
        else                                           var_decl(m, exported);
    }

    void type_decl(ast::Module& m, bool exported)
    {
        ast::StructDecl s;
        s.exported = exported;
        s.line = peek().line;
        s.name = advance().text;
        expect(lex::Tok::Colon, "':' after type name");
        expect(lex::Tok::Type, "'type'");
        if (accept(lex::Tok::Colon)) {                 // 基类列表(M2b:单个)
            ast::TypeUse base;
            base.line = peek().line;
            base.parts = qualified_name_parts("base type name");
            s.base = base;
        }
        expect(lex::Tok::Assign, "'=' before type body");
        expect(lex::Tok::LBrace, "'{' to start type body");

        while (!check(lex::Tok::RBrace)) {
            if (check(lex::Tok::Eof)) err("expected '}' before end of file");
            if (accept(lex::Tok::Semi)) continue;      // 允许成员间空行后残留分号
            struct_member(s);
        }
        expect(lex::Tok::RBrace, "'}' to end type body");
        m.structs.push_back(std::move(s));
    }

    void struct_member(ast::StructDecl& s)
    {
        if (!check(lex::Tok::Ident)) err("expected a field or method declaration");
        if (peek(1).tok == lex::Tok::LParen)
            err("unexpected '(' — method declarations look like 'name: (params) ...'");
        if (peek(1).tok != lex::Tok::Colon) err("expected ':' after member name");

        // name ':' '(' → 方法;'name' ':' type → 字段
        if (peek(2).tok == lex::Tok::LParen) {
            method_decl(s);
            return;
        }

        ast::FieldDecl f;
        f.line = peek().line;
        f.name = advance().text;
        expect(lex::Tok::Colon, "':' after field name");
        f.type = parse_type();
        f.is_const = f.type.is_const;
        if (!accept(lex::Tok::Assign)) err("expected initializer — every field must have a default value");
        f.init = expression();
        expect(lex::Tok::Semi, "';' after field declaration");
        s.fields.push_back(std::move(f));
    }

    void method_decl(ast::StructDecl& s)
    {
        ast::MethodDecl md;
        md.line = peek().line;
        md.name = advance().text;
        expect(lex::Tok::Colon, "':' after method name");
        expect(lex::Tok::LParen, "'(' to start parameter list");
        md.params = param_list();
        expect(lex::Tok::RParen, "')' to end parameter list");

        if (accept(lex::Tok::Arrow)) md.ret = parse_type();
        if (check(lex::Tok::Throws)) {
            advance();
            md.throws = true;
            if (check(lex::Tok::Ident)) parse_type(); // 错误类别:M2c 起使用
        }
        if (accept(lex::Tok::Mutates)) md.mutates = true;
        if (check(lex::Tok::Ident) && peek().text == "pre")  unsupported("pre/post contracts", "M2c");
        if (check(lex::Tok::Ident) && peek().text == "post") unsupported("pre/post contracts", "M2c");

        expect(lex::Tok::Assign, "'=' before method body");
        if (check(lex::Tok::LBrace)) {
            md.has_block_body = true;
            md.block_body = block();
        } else {
            md.expr_body = expression();
            expect(lex::Tok::Semi, "';' after short method body");
        }
        if (md.name == "destructor" && (!md.params.empty() || md.ret))
            err("destructor takes no parameters and returns nothing");
        s.methods.push_back(std::move(md));
    }

    void enum_decl(ast::Module& m, bool exported)
    {
        ast::EnumDecl e;
        e.exported = exported;
        e.line = peek().line;
        e.name = advance().text;
        expect(lex::Tok::Colon, "':' after enum name");
        expect(lex::Tok::Enum, "'enum'");
        if (accept(lex::Tok::Colon)) e.underlying = parse_type();
        expect(lex::Tok::Assign, "'=' before enum body");
        expect(lex::Tok::LBrace, "'{' to start enum body");
        if (!check(lex::Tok::Ident)) err("expected enum member name");
        for (;;) {
            if (!check(lex::Tok::Ident)) err("expected enum member name");
            e.members.push_back(advance().text);
            if (!accept(lex::Tok::Comma)) break;
            if (check(lex::Tok::RBrace)) break;       // 允许尾逗号
        }
        expect(lex::Tok::RBrace, "'}' to end enum body");
        m.enums.push_back(std::move(e));
    }

    void func_decl(ast::Module& m, bool exported)
    {
        ast::FuncDecl f;
        f.exported = exported;
        f.line = peek().line;
        f.name = advance().text;
        expect(lex::Tok::Colon, "':' after function name");
        expect(lex::Tok::LParen, "'(' to start parameter list");
        f.params = param_list();
        expect(lex::Tok::RParen, "')' to end parameter list");

        if (accept(lex::Tok::Arrow)) f.ret = parse_type();
        if (check(lex::Tok::Throws)) {
            advance();
            f.throws = true;
            if (check(lex::Tok::Ident)) parse_type();
        }
        if (accept(lex::Tok::Mutates))
            err("'mutates' is only valid on type member methods");
        if (check(lex::Tok::Ident) && peek().text == "pre")  unsupported("pre/post contracts", "M2c");
        if (check(lex::Tok::Ident) && peek().text == "post") unsupported("pre/post contracts", "M2c");

        expect(lex::Tok::Assign, "'=' before function body");
        if (check(lex::Tok::LBrace)) {
            f.has_block_body = true;
            f.block_body = block();
        } else {
            f.expr_body = expression();
            expect(lex::Tok::Semi, "';' after short function body");
        }
        m.funcs.push_back(std::move(f));
    }

    std::vector<ast::Param> param_list()
    {
        std::vector<ast::Param> params;
        if (check(lex::Tok::RParen)) return params;
        for (;;) {
            ast::Param p;
            p.line = peek().line;
            p.mode = param_mode();               // 模式在名字之前: (inout n: int)
            if (!check(lex::Tok::Ident)) err("expected parameter name");
            p.name = advance().text;
            expect(lex::Tok::Colon, "':' after parameter name");
            p.type = parse_type();
            if (accept(lex::Tok::Assign)) p.default_value = expression();
            params.push_back(std::move(p));
            if (!accept(lex::Tok::Comma)) break;
        }
        return params;
    }

    ast::ParamMode param_mode()
    {
        if (check(lex::Tok::In)) { advance(); return ast::ParamMode::In; }
        auto is_mode = [&](char const* s) {
            return check(lex::Tok::Ident) && peek().text == s && peek(1).tok == lex::Tok::Ident;
        };
        if (is_mode("inout"))   { advance(); return ast::ParamMode::Inout; }
        if (is_mode("out"))     { advance(); return ast::ParamMode::Out; }
        if (is_mode("move"))    { advance(); return ast::ParamMode::Move; }
        if (is_mode("copy"))    { advance(); return ast::ParamMode::Copy; }
        if (is_mode("forward")) { advance(); return ast::ParamMode::Forward; }
        return ast::ParamMode::In; // 默认
    }

    void var_decl(ast::Module& m, bool exported)
    {
        ast::GlobalVar g;
        g.exported = exported;
        g.line = peek().line;
        g.name = advance().text;
        expect(lex::Tok::Colon, "':' after variable name");

        if (check(lex::Tok::Walrus)) {
            advance();
            g.init = expression();
            expect(lex::Tok::Semi, "';' after variable initializer");
            m.globals.push_back(std::move(g));
            return;
        }
        g.type = parse_type();
        g.has_type = true;
        g.is_const = g.type.is_const;
        if (accept(lex::Tok::Assign) || accept(lex::Tok::Walrus)) {
            g.init = expression();
        } else {
            err("expected initializer (variables must be initialized; use '= _' for explicit uninitialized)");
        }
        expect(lex::Tok::Semi, "';' after variable declaration");
        m.globals.push_back(std::move(g));
    }

    // ── 类型 ────────────────────────────────────────────────────────
    ast::TypeUse parse_type()
    {
        DepthGuard g{*this};
        ast::TypeUse t;
        t.line = peek().line;
        if (accept(lex::Tok::Const)) t.is_const = true;
        if (!check(lex::Tok::Ident)) err("expected a type name");
        t.parts.push_back(advance().text);
        while (accept_double_colon()) {
            t.parts.push_back(require_ident());
        }
        if (accept(lex::Tok::Lt)) {
            if (check(lex::Tok::Gt)) err("expected type argument");
            for (;;) {
                t.args.push_back(parse_type());
                if (!accept(lex::Tok::Comma)) break;
            }
            expect(lex::Tok::Gt, "'>' to close type arguments");
        }
        return t;
    }

    // ── 语句 ────────────────────────────────────────────────────────
    ast::StmtP block()
    {
        auto b = std::make_unique<ast::BlockStmt>();
        b->line = peek().line; b->col = peek().col;
        expect(lex::Tok::LBrace, "'{'");
        while (!check(lex::Tok::RBrace)) {
            if (check(lex::Tok::Eof)) err("expected '}' before end of file");
            b->stmts.push_back(statement());
        }
        expect(lex::Tok::RBrace, "'}'");
        return b;
    }

    ast::StmtP statement()
    {
        DepthGuard g{*this};

        // @unchecked / @unsafe 注解:作用于其后的一条语句(递归进入其块)
        if (check(lex::Tok::At)) {
            advance();
            if (!check(lex::Tok::Ident)
                || (peek().text != "unchecked" && peek().text != "unsafe"))
                err("expected 'unchecked' or 'unsafe' after '@'");
            bool unsafe = peek().text == "unsafe";
            advance();
            auto s = statement();
            s->no_check = true;
            s->is_unsafe = unsafe;
            return s;
        }

        switch (peek().tok) {
        case lex::Tok::Return:   return return_stmt();
        case lex::Tok::If:       return if_stmt();
        case lex::Tok::While:    return while_stmt();
        case lex::Tok::For:      return for_stmt();
        case lex::Tok::Break: {
            auto s = std::make_unique<ast::BreakStmt>();
            s->line = peek().line; s->col = peek().col;
            advance(); expect(lex::Tok::Semi, "';' after 'break'");
            return s;
        }
        case lex::Tok::Continue: {
            auto s = std::make_unique<ast::ContinueStmt>();
            s->line = peek().line; s->col = peek().col;
            advance(); expect(lex::Tok::Semi, "';' after 'continue'");
            return s;
        }
        case lex::Tok::Match:    unsupported("match expression", "M2d");
        case lex::Tok::LBrace:   return block();   // 裸块:@unsafe/@unchecked 块形式(DESIGN §6.2/§6.6)
        default: break;
        }

        // 变量声明:IDENT ':' / IDENT ':='(排除限定名的 '::')
        bool starts_decl = check(lex::Tok::Ident)
            && (peek(1).tok == lex::Tok::Walrus
                || (peek(1).tok == lex::Tok::Colon && peek(2).tok != lex::Tok::Colon));
        if (starts_decl) {
            return local_var_stmt();
        }

        // 表达式语句 / 赋值
        auto e = expression();
        std::string op = assign_op();
        if (!op.empty()) {
            auto a = std::make_unique<ast::AssignExpr>();
            a->line = e->line; a->col = e->col;
            a->op = op;
            a->target = std::move(e);
            a->value = expression();
            expect(lex::Tok::Semi, "';' after assignment");
            return wrap_stmt(std::move(a));
        }
        expect(lex::Tok::Semi, "';' after expression statement");
        auto s = std::make_unique<ast::ExprStmt>();
        s->line = e->line; s->col = e->col;
        s->expr = std::move(e);
        return s;
    }

    static ast::StmtP wrap_stmt(ast::ExprP e)
    {
        auto s = std::make_unique<ast::ExprStmt>();
        s->line = e->line; s->col = e->col;
        s->expr = std::move(e);
        return s;
    }

    std::string assign_op()
    {
        switch (peek().tok) {
        case lex::Tok::Assign:    advance(); return "=";
        case lex::Tok::PlusEq:    advance(); return "+=";
        case lex::Tok::MinusEq:   advance(); return "-=";
        case lex::Tok::StarEq:    advance(); return "*=";
        case lex::Tok::SlashEq:   advance(); return "/=";
        case lex::Tok::PercentEq: advance(); return "%=";
        default: return "";
        }
    }

    ast::StmtP return_stmt()
    {
        auto s = std::make_unique<ast::ReturnStmt>();
        s->line = peek().line; s->col = peek().col;
        advance();
        if (!check(lex::Tok::Semi)) s->value = expression();
        expect(lex::Tok::Semi, "';' after return");
        return s;
    }

    ast::StmtP if_stmt()
    {
        auto s = std::make_unique<ast::IfStmt>();
        s->line = peek().line; s->col = peek().col;
        advance(); // if
        bool prev = cond_like_;
        cond_like_ = true;
        s->cond = expression();
        cond_like_ = prev;
        s->then_block = block();
        if (accept(lex::Tok::Else)) {
            if (check(lex::Tok::If)) s->else_block = if_stmt();
            else                     s->else_block = block();
        }
        return s;
    }

    ast::StmtP while_stmt()
    {
        auto s = std::make_unique<ast::WhileStmt>();
        s->line = peek().line; s->col = peek().col;
        advance();
        bool prev = cond_like_;
        cond_like_ = true;
        s->cond = expression();
        cond_like_ = prev;
        s->body = block();
        return s;
    }

    ast::StmtP for_stmt()
    {
        auto s = std::make_unique<ast::ForStmt>();
        s->line = peek().line; s->col = peek().col;
        advance(); // for
        if (!check(lex::Tok::Ident)) err("expected loop variable name");
        s->var = advance().text;
        expect(lex::Tok::In, "'in' after loop variable");
        s->range_begin = expression();
        if (check(lex::Tok::DotDot) || check(lex::Tok::DotDotEq)) {
            s->is_range = true;
            s->inclusive = advance().tok == lex::Tok::DotDotEq;
            s->range_end = expression();
        } else {
            bool prev = cond_like_;
            cond_like_ = true;
            s->iterable = std::move(s->range_begin);
            s->range_begin.reset();
            cond_like_ = prev;
        }
        s->body = block();
        return s;
    }

    ast::StmtP local_var_stmt()
    {
        auto s = std::make_unique<ast::VarStmt>();
        s->line = peek().line; s->col = peek().col;
        s->name = advance().text;
        if (accept(lex::Tok::Walrus)) {
            s->init = expression();
            expect(lex::Tok::Semi, "';' after variable initializer");
            return s;
        }
        expect(lex::Tok::Colon, "':'");
        s->type = parse_type();
        s->has_type = true;
        s->is_const = s->type.is_const;
        if (accept(lex::Tok::Assign) || accept(lex::Tok::Walrus)) {
            s->init = expression();
        } else {
            err("expected initializer (variables must be initialized)");
        }
        expect(lex::Tok::Semi, "';' after variable declaration");
        return s;
    }

    // ── 表达式(优先级爬升)────────────────────────────────────────
    ast::ExprP expression()
    {
        DepthGuard g{*this};
        return logical_or();
    }

    ast::ExprP logical_or()
    {
        auto l = logical_and();
        while (check(lex::Tok::OrOr)) {
            int ln = peek().line, cl = peek().col;
            advance();
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = "||";
            b->lhs = std::move(l); b->rhs = logical_and();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP logical_and()
    {
        auto l = equality();
        while (check(lex::Tok::AndAnd)) {
            int ln = peek().line, cl = peek().col;
            advance();
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = "&&";
            b->lhs = std::move(l); b->rhs = equality();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP equality()
    {
        auto l = relational();
        while (check(lex::Tok::Eq) || check(lex::Tok::Ne)) {
            std::string op = advance().text;
            int ln = peek().line, cl = peek().col;
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = op;
            b->lhs = std::move(l); b->rhs = relational();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP relational()
    {
        auto l = additive();
        while (check(lex::Tok::Lt) || check(lex::Tok::Gt)
            || check(lex::Tok::Le) || check(lex::Tok::Ge)) {
            std::string op = advance().text;
            int ln = peek().line, cl = peek().col;
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = op;
            b->lhs = std::move(l); b->rhs = additive();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP additive()
    {
        auto l = multiplicative();
        while (check(lex::Tok::Plus) || check(lex::Tok::Minus)) {
            std::string op = advance().text;
            int ln = peek().line, cl = peek().col;
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = op;
            b->lhs = std::move(l); b->rhs = multiplicative();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP multiplicative()
    {
        auto l = unary();
        while (check(lex::Tok::Star) || check(lex::Tok::Slash) || check(lex::Tok::Percent)) {
            std::string op = advance().text;
            int ln = peek().line, cl = peek().col;
            auto b = std::make_unique<ast::BinaryExpr>();
            b->line = ln; b->col = cl; b->op = op;
            b->lhs = std::move(l); b->rhs = unary();
            l = std::move(b);
        }
        return l;
    }

    ast::ExprP unary()
    {
        if (check(lex::Tok::Bang) || check(lex::Tok::Minus) || check(lex::Tok::Plus)
            || check(lex::Tok::Tilde)) {
            DepthGuard g{*this};                // `- - - ...` 前缀链同样可能爆栈
            std::string op = advance().text;
            auto u = std::make_unique<ast::UnaryExpr>();
            u->line = peek().line; u->col = peek().col;
            u->op = op;
            u->operand = unary();
            return u;
        }
        return postfix();
    }

    ast::ExprP postfix()
    {
        auto e = primary();
        for (;;) {
            if (check(lex::Tok::LParen)) {
                auto c = std::make_unique<ast::CallExpr>();
                c->line = peek().line; c->col = peek().col;
                c->callee = std::move(e);
                advance();
                if (!check(lex::Tok::RParen)) {
                    for (;;) {
                        // 调用侧显式 move:consume(move p)(DESIGN §4.3)
                        if (check(lex::Tok::Ident) && peek().text == "move"
                            && peek(1).tok == lex::Tok::Ident && peek(1).text != "move") {
                            auto mv = std::make_unique<ast::UnaryExpr>();
                            mv->line = peek().line; mv->col = peek().col;
                            mv->op = "move";
                            advance(); // move
                            mv->operand = expression();
                            c->args.push_back(std::move(mv));
                        } else {
                            c->args.push_back(expression());
                        }
                        if (!accept(lex::Tok::Comma)) break;
                    }
                }
                expect(lex::Tok::RParen, "')' to close call arguments");
                e = std::move(c);
            } else if (check(lex::Tok::LBracket)) {
                auto x = std::make_unique<ast::IndexExpr>();
                x->line = peek().line; x->col = peek().col;
                x->base = std::move(e);
                advance();
                x->index = expression();
                expect(lex::Tok::RBracket, "']' to close index");
                e = std::move(x);
            } else if (check(lex::Tok::Dot)) {
                advance();
                auto x = std::make_unique<ast::MemberExpr>();
                x->line = peek().line; x->col = peek().col;
                x->base = std::move(e);
                if (!check(lex::Tok::Ident)) err("expected member name after '.'");
                x->name = advance().text;
                e = std::move(x);
            } else if (check(lex::Tok::Ident) && peek().text == "as"
                       && peek(1).tok == lex::Tok::Ident) {
                // 显式转换:n as i32(限定后缀不是 'as' 类型开头时自然回退)
                auto x = std::make_unique<ast::AsCastExpr>();
                x->line = peek().line; x->col = peek().col;
                x->operand = std::move(e);
                advance(); // as
                x->target = parse_type();
                e = std::move(x);
            } else if (check(lex::Tok::Question) || check(lex::Tok::Bang)) {
                unsupported("postfix '?'/'!' (error propagation)", "M2c");
            } else {
                break;
            }
        }
        return e;
    }

    ast::ExprP primary()
    {
        switch (peek().tok) {
        case lex::Tok::IntLit: case lex::Tok::DoubleLit:
        case lex::Tok::StringLit: case lex::Tok::CharLit: {
            auto l = std::make_unique<ast::LiteralExpr>();
            l->line = peek().line; l->col = peek().col;
            l->text = peek().text;
            l->lit = peek().tok == lex::Tok::IntLit    ? ast::LitKind::Int
                   : peek().tok == lex::Tok::DoubleLit ? ast::LitKind::Double
                   : peek().tok == lex::Tok::StringLit ? ast::LitKind::String
                                                       : ast::LitKind::Char;
            advance();
            return l;
        }
        case lex::Tok::True: case lex::Tok::False: {
            auto l = std::make_unique<ast::LiteralExpr>();
            l->line = peek().line; l->col = peek().col;
            l->text = peek().text;
            l->lit = ast::LitKind::Bool;
            advance();
            return l;
        }
        case lex::Tok::Ident: {
            auto n = std::make_unique<ast::NameExpr>();
            n->line = peek().line; n->col = peek().col;
            n->parts.push_back(advance().text);
            while (accept_double_colon()) n->parts.push_back(require_ident());
            // StructLit:Name{.field = value, ...} 或 Name{}(空);
            // 条件位置不接受空字面量,避免吞掉 if/while/for 的块括号
            if (check(lex::Tok::LBrace)
                && (peek(1).tok == lex::Tok::Dot
                    || (peek(1).tok == lex::Tok::RBrace && !cond_like_))) {
                auto lit = std::make_unique<ast::StructLitExpr>();
                lit->line = n->line; lit->col = n->col;
                lit->type_parts = n->parts;
                advance(); // {
                if (!check(lex::Tok::RBrace)) {       // Name{} → 全默认值
                    for (;;) {
                        expect(lex::Tok::Dot, "'.field' in struct literal");
                        if (!check(lex::Tok::Ident)) err("expected field name after '.'");
                        std::string field = advance().text;
                        expect(lex::Tok::Assign, "'=' after field name");
                        lit->fields.push_back({field, expression()});
                        if (!accept(lex::Tok::Comma)) break;
                    }
                }
                expect(lex::Tok::RBrace, "'}' to close struct literal");
                return lit;
            }
            return n;
        }
        case lex::Tok::LParen: {
            advance();
            auto p = std::make_unique<ast::ParenExpr>();
            p->line = peek().line; p->col = peek().col;
            bool prev = cond_like_;
            cond_like_ = false;                       // 括号内回到普通表达式上下文
            p->inner = expression();
            cond_like_ = prev;
            expect(lex::Tok::RParen, "')'");
            return p;
        }
        case lex::Tok::LBracket:
        case lex::Tok::LBrace: {
            // 列表字面量:[1, 2, 3] 或 {1, 2, 3};空 {} 常用作容器默认值。
            // (struct 字面量在 Ident 分支以 Name{.field 形式处理,不冲突;
            //  语句/条件位置的块括号不会进入 primary)
            lex::Tok closer = peek().tok == lex::Tok::LBracket ? lex::Tok::RBracket
                                                               : lex::Tok::RBrace;
            lex::Tok opener = advance().tok;
            (void)opener;
            auto l = std::make_unique<ast::ListLitExpr>();
            l->line = peek().line; l->col = peek().col;
            if (!check(closer)) {
                for (;;) {
                    l->elements.push_back(expression());
                    if (!accept(lex::Tok::Comma)) break;
                }
            }
            expect(closer, closer == lex::Tok::RBracket ? "']' to close list literal"
                                                        : "'}' to close list literal");
            return l;
        }
        case lex::Tok::At:       unsupported("@unsafe/@unchecked annotation", "M2b");
        default: err("expected an expression");
        }
    }
};

} // namespace

ast::Module parse(std::vector<lex::Token> toks, std::string const& source_name)
{
    return Parser(std::move(toks), source_name).run();
}

} // namespace cpp2::parse
