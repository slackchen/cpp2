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

    // operator 后缀 token(operator< / operator== / ...;concept 需求与方法名共用)
    static bool is_operator_token(lex::Tok t)
    {
        switch (t) {
        case lex::Tok::Lt: case lex::Tok::Gt: case lex::Tok::Le: case lex::Tok::Ge:
        case lex::Tok::Eq: case lex::Tok::Ne: case lex::Tok::Plus: case lex::Tok::Minus:
        case lex::Tok::Star: case lex::Tok::Slash: case lex::Tok::Percent:
        case lex::Tok::AndAnd: case lex::Tok::OrOr: case lex::Tok::Bang:
            return true;
        default:
            return false;
        }
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

        // name ':' ... — 看第三个 token 分派 type / enum / variant / concept / 函数 / 变量
        lex::Tok after_colon = peek(2).tok;
        if (after_colon == lex::Tok::Type)             type_decl(m, exported);
        else if (after_colon == lex::Tok::Enum)        enum_decl(m, exported);
        else if (after_colon == lex::Tok::Variant)     variant_decl(m, exported);
        else if (after_colon == lex::Tok::Concept)     concept_decl(m, exported);
        else if (after_colon == lex::Tok::LParen)      func_decl(m, exported);
        else if (after_colon == lex::Tok::Lt)          func_decl(m, exported); // 泛型 <T: C>
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

        // operator 方法:operator<: (that: T) -> bool = ...(运算符成员,M2d)
        if (peek().text == "operator" && is_operator_token(peek(1).tok)
            && peek(2).tok == lex::Tok::Colon) {
            method_decl(s);
            return;
        }
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
        if (md.name == "operator" && is_operator_token(peek().tok))
            md.name += advance().text;           // operator< / operator== / ...
        expect(lex::Tok::Colon, "':' after method name");
        expect(lex::Tok::LParen, "'(' to start parameter list");
        md.params = param_list();
        expect(lex::Tok::RParen, "')' to end parameter list");

        if (accept(lex::Tok::Arrow)) md.ret = parse_type();
        if (check(lex::Tok::Throws)) {
            advance();
            md.throws = true;
            parse_error_category();
        }
        if (accept(lex::Tok::Mutates)) md.mutates = true;
        parse_contracts(md.pre, md.post);

        expect(lex::Tok::Assign, "'=' before method body");
        if (check(lex::Tok::LBrace)) {
            md.has_block_body = true;
            md.block_body = block();
        } else {
            md.expr_body = expression();
            // match 表达式以 '}' 结尾,分号可省(与 func_decl 一致)
            if (md.expr_body->kind() != ast::Expr::Match)
                expect(lex::Tok::Semi, "';' after short method body");
            else accept(lex::Tok::Semi);
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

    // Value: variant = { int, string, vector<Value> }(DESIGN §5.5)
    void variant_decl(ast::Module& m, bool exported)
    {
        ast::VariantDecl v;
        v.exported = exported;
        v.line = peek().line;
        v.name = advance().text;
        expect(lex::Tok::Colon, "':' after variant name");
        expect(lex::Tok::Variant, "'variant'");
        expect(lex::Tok::Assign, "'=' before variant body");
        expect(lex::Tok::LBrace, "'{' to start variant alternatives");
        if (check(lex::Tok::RBrace)) err("variant needs at least one alternative type");
        for (;;) {
            v.alternatives.push_back(parse_type());
            if (!accept(lex::Tok::Comma)) break;
            if (check(lex::Tok::RBrace)) break;       // 允许尾逗号
        }
        expect(lex::Tok::RBrace, "'}' to end variant");
        m.variants.push_back(std::move(v));
    }

    // Ordered: concept = { operator<: (that: self) -> bool; ... }(DESIGN §5.6)
    // 接口块:成员是签名(无体),self 指代满足概念的类型。
    void concept_decl(ast::Module& m, bool exported)
    {
        ast::ConceptDecl c;
        c.exported = exported;
        c.line = peek().line;
        c.name = advance().text;
        expect(lex::Tok::Colon, "':' after concept name");
        expect(lex::Tok::Concept, "'concept'");
        expect(lex::Tok::Assign, "'=' before concept body");
        expect(lex::Tok::LBrace, "'{' to start concept requirements");
        while (!check(lex::Tok::RBrace)) {
            if (check(lex::Tok::Eof)) err("expected '}' before end of file");
            if (accept(lex::Tok::Semi)) continue;
            if (!check(lex::Tok::Ident)) err("expected a requirement 'name: (params) -> ret;'");
            ast::MethodDecl req;
            req.line = peek().line;
            req.name = advance().text;
            if (req.name == "operator" && is_operator_token(peek().tok))
                req.name += advance().text;      // operator< / operator== / ...
            expect(lex::Tok::Colon, "':' after requirement name");
            expect(lex::Tok::LParen, "'(' to start parameter list");
            req.params = param_list();
            expect(lex::Tok::RParen, "')' to end parameter list");
            if (accept(lex::Tok::Arrow)) req.ret = parse_type();
            expect(lex::Tok::Semi, "';' after requirement (concepts have no bodies)");
            c.reqs.push_back(std::move(req));
        }
        expect(lex::Tok::RBrace, "'}' to end concept");
        if (c.reqs.empty()) err("concept needs at least one requirement");
        m.concepts.push_back(std::move(c));
    }

    void func_decl(ast::Module& m, bool exported)
    {
        ast::FuncDecl f;
        f.exported = exported;
        f.line = peek().line;
        f.name = advance().text;
        expect(lex::Tok::Colon, "':' after function name");

        // 泛型参数:<T: Concept>(DESIGN §5.6)
        if (accept(lex::Tok::Lt)) {
            for (;;) {
                ast::TypeParam tp;
                tp.line = peek().line;
                if (!check(lex::Tok::Ident)) err("expected type parameter name");
                tp.name = advance().text;
                if (accept(lex::Tok::Colon))
                    tp.concept_parts = qualified_name_parts("concept name");
                f.type_params.push_back(std::move(tp));
                if (!accept(lex::Tok::Comma)) break;
            }
            expect(lex::Tok::Gt, "'>' to close type parameters");
        }

        expect(lex::Tok::LParen, "'(' to start parameter list");
        f.params = param_list();
        expect(lex::Tok::RParen, "')' to end parameter list");

        if (accept(lex::Tok::Arrow)) f.ret = parse_type();
        if (check(lex::Tok::Throws)) {
            advance();
            f.throws = true;
            parse_error_category();
        }
        if (accept(lex::Tok::Mutates))
            err("'mutates' is only valid on type member methods");
        parse_contracts(f.pre, f.post);
        parse_requires(f.requires_list);

        expect(lex::Tok::Assign, "'=' before function body");
        if (check(lex::Tok::LBrace)) {
            f.has_block_body = true;
            f.block_body = block();
        } else {
            f.expr_body = expression();
            // match 表达式以 '}' 结尾,分号可省(DESIGN §5.5 形式)
            if (f.expr_body->kind() != ast::Expr::Match)
                expect(lex::Tok::Semi, "';' after short function body");
            else accept(lex::Tok::Semi);
        }
        m.funcs.push_back(std::move(f));
    }

    // requires Ordered<T> && Printable<T>(DESIGN §5.6;位于 '=' 之前)
    void parse_requires(std::vector<ast::RequiresItem>& out)
    {
        if (!(check(lex::Tok::Ident) && peek().text == "requires"
              && (peek(1).tok == lex::Tok::Ident || peek(1).tok == lex::Tok::Lt)))
            return;
        advance(); // requires
        for (;;) {
            ast::RequiresItem r;
            r.name_parts = qualified_name_parts("concept name in requires");
            if (accept(lex::Tok::Lt)) {
                for (;;) {
                    if (!check(lex::Tok::Ident)) err("expected type parameter name");
                    r.args.push_back(advance().text);
                    if (!accept(lex::Tok::Comma)) break;
                }
                expect(lex::Tok::Gt, "'>' to close requires arguments");
            }
            out.push_back(std::move(r));
            if (!accept(lex::Tok::AndAnd)) break;
        }
    }

    // throws E 的错误类别:v0.1 解析后丢弃(§8.4 错误类型体系 v0.3 落地);
    // "pre:"/"post:" 不是类别(后跟 ':'),不能吞掉
    void parse_error_category()
    {
        if (check(lex::Tok::Ident)
            && !(peek(1).tok == lex::Tok::Colon
                 && (peek().text == "pre" || peek().text == "post"))) {
            parse_type();
        }
    }

    // 契约:pre: expr / post: expr(DESIGN §6.5)。表达式在 '=' 前自然终止
    // ('=' 与 'post'/'pre' 均不参与二元运算)。old()/result 由 sema/emit 处理。
    void parse_contracts(ast::ExprP& pre, ast::ExprP& post)
    {
        if (check(lex::Tok::Ident) && peek().text == "pre" && peek(1).tok == lex::Tok::Colon) {
            advance(); advance();
            pre = expression();
        }
        if (check(lex::Tok::Ident) && peek().text == "post" && peek(1).tok == lex::Tok::Colon) {
            advance(); advance();
            post = expression();
        }
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
        if (accept(lex::Tok::Question)) t.is_optional = true;   // T?(DESIGN §6.4)
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
        case lex::Tok::Match:    return match_stmt();
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

        // if-let:x := f() { ... } else e := it { ... }(DESIGN §8.3;':=' 不出现在普通表达式)
        if ((check(lex::Tok::Ident) || check(lex::Tok::Underscore))
            && peek(1).tok == lex::Tok::Walrus) {
            s->let_name = advance().text;
            advance(); // :=
            s->let_init = expression();
        } else {
            s->cond = expression();
        }
        cond_like_ = prev;
        s->then_block = block();

        if (accept(lex::Tok::Else)) {
            // else e := it { }:错误分支绑定(DESIGN §8.3)
            if (check(lex::Tok::Ident) && peek(1).tok == lex::Tok::Walrus
                && peek(2).tok == lex::Tok::Ident && peek(2).text == "it") {
                if (!s->is_let())
                    err("'else NAME := it' binds the error of an if-let condition");
                s->else_binding = advance().text;
                advance(); // :=
                advance(); // it
                s->else_block = block();
            } else if (check(lex::Tok::If)) {
                s->else_block = if_stmt();
            } else {
                s->else_block = block();
            }
        }
        return s;
    }

    // match v { pattern [if guard] => body; ... } — 语句形式(臂体 = 块/语句)
    // 模式(M2d,DESIGN §4.5/§5.4/§5.5):_ / .member / ok|err [name] / some|none /
    //   Type [name] / Type(bind|​.field, ...) — 按 scrutinee 类型分派合法集合
    ast::StmtP match_stmt()
    {
        auto s = std::make_unique<ast::MatchStmt>();
        s->line = peek().line; s->col = peek().col;
        advance(); // match
        bool prev = cond_like_;
        cond_like_ = true;                     // scrutinee 不吞块括号
        s->scrutinee = expression();
        cond_like_ = prev;
        expect(lex::Tok::LBrace, "'{' to start match arms");

        while (!check(lex::Tok::RBrace)) {
            if (check(lex::Tok::Eof)) err("expected '}' before end of file");
            if (accept(lex::Tok::Semi)) continue;           // 臂之间空行残留分号
            parse_arm(s->arms, /*expr_form*/false);
        }
        expect(lex::Tok::RBrace, "'}' to end match");
        return s;
    }

    // 一条 match 臂;expr_form = 表达式 match(臂体是表达式,包成 ExprStmt)
    void parse_arm(std::vector<ast::MatchArm>& arms, bool expr_form)
    {
        ast::MatchArm arm;
        arm.line = peek().line;

        if (accept(lex::Tok::Underscore)) {
            arm.pat = ast::MatchArm::Pat::Wildcard;
        } else if (check(lex::Tok::Dot)) {
            advance();
            if (!check(lex::Tok::Ident)) err("expected enum member after '.'");
            arm.pat = ast::MatchArm::Pat::EnumMember;
            arm.enum_member = advance().text;
        } else if (check(lex::Tok::Ident)
                   && (peek().text == "ok" || peek().text == "err"
                       || peek().text == "some" || peek().text == "none")
                   && (peek(1).tok == lex::Tok::Ident
                       || peek(1).tok == lex::Tok::Underscore
                       || peek(1).tok == lex::Tok::FatArrow
                       || peek(1).tok == lex::Tok::If)) {
            std::string kw = advance().text;
            arm.pat = kw == "ok"   ? ast::MatchArm::Pat::Ok
                    : kw == "err"  ? ast::MatchArm::Pat::Err
                    : kw == "some" ? ast::MatchArm::Pat::Some
                                   : ast::MatchArm::Pat::None;
            if (check(lex::Tok::Ident) || check(lex::Tok::Underscore))
                arm.binding = advance().text;
            else
                arm.binding = "_";
        } else {
            arm.pat = ast::MatchArm::Pat::TypePat;
            arm.type_pattern = parse_type();
            if (check(lex::Tok::Ident))             arm.binding = advance().text;
            else if (check(lex::Tok::Underscore)) { advance(); arm.binding = "_"; }
            if (accept(lex::Tok::LParen)) {                 // 解构:Point(.x, .y) / Rect(w, h)
                if (!check(lex::Tok::RParen)) {
                    for (;;) {
                        if (accept(lex::Tok::Underscore))
                            arm.sub.push_back("_");
                        else if (accept(lex::Tok::Dot)) {
                            if (!check(lex::Tok::Ident)) err("expected field name after '.'");
                            arm.sub.push_back("." + advance().text);
                        } else if (check(lex::Tok::Ident)) {
                            arm.sub.push_back(advance().text);
                        } else {
                            err("expected binding, '.field' or '_' in destructuring pattern");
                        }
                        if (!accept(lex::Tok::Comma)) break;
                    }
                }
                expect(lex::Tok::RParen, "')' to close destructuring pattern");
            }
        }

        if (check(lex::Tok::If)) {                           // 守卫:pattern if cond =>
            advance();
            bool prev = cond_like_;
            cond_like_ = true;
            arm.guard = expression();
            cond_like_ = prev;
        }
        expect(lex::Tok::FatArrow, "'=>' after match arm pattern");

        if (expr_form) {
            if (check(lex::Tok::LBrace)) {
                arm.body = block();
            } else {
                auto e = expression();
                accept(lex::Tok::Semi);                      // 臂尾分号可省(块前无歧义)
                auto s = std::make_unique<ast::ExprStmt>();
                s->line = e->line; s->col = e->col;
                s->expr = std::move(e);
                arm.body = std::move(s);
            }
        } else {
            arm.body = check(lex::Tok::LBrace) ? block() : statement();
        }
        arms.push_back(std::move(arm));
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
        return or_default();
    }

    // 最低优先级:err-or-default(f() or "fallback",DESIGN §8.3)
    ast::ExprP or_default()
    {
        auto l = logical_or();
        while (check(lex::Tok::Ident) && peek().text == "or"
               && starts_expression(1)) {
            int ln = peek().line, cl = peek().col;
            advance(); // or
            auto b = std::make_unique<ast::OrDefaultExpr>();
            b->line = ln; b->col = cl;
            b->lhs = std::move(l); b->rhs = logical_or();
            l = std::move(b);
        }
        return l;
    }

    // 'or' 之后是否跟着一个表达式的开头(排除把普通标识符误当运算符)
    bool starts_expression(size_t ahead) const
    {
        switch (peek(ahead).tok) {
        case lex::Tok::IntLit: case lex::Tok::DoubleLit:
        case lex::Tok::StringLit: case lex::Tok::CharLit:
        case lex::Tok::True: case lex::Tok::False:
        case lex::Tok::Ident: case lex::Tok::LParen:
        case lex::Tok::LBracket: case lex::Tok::LBrace:
        case lex::Tok::Bang: case lex::Tok::Minus: case lex::Tok::Plus:
            return true;
        default:
            return false;
        }
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
            } else if (check(lex::Tok::Question)) {
                // f()? — 解包 + 失败向上传播(DESIGN §8.2;M2c)
                auto x = std::make_unique<ast::TryExpr>();
                x->line = peek().line; x->col = peek().col;
                x->operand = std::move(e);
                advance();
                e = std::move(x);
            } else if (check(lex::Tok::Bang)) {
                // f()! — 确信必成功,失败即 bug → trap(前缀 ! 已在 unary 消费)
                auto x = std::make_unique<ast::MustExpr>();
                x->line = peek().line; x->col = peek().col;
                x->operand = std::move(e);
                advance();
                e = std::move(x);
            } else {
                break;
            }
        }
        return e;
    }

    // 匿名函数:函数声明去掉名字(DESIGN §4.6)。捕获:v0.1 隐式 [&] 按引用
    // (显式捕获列表与 [self] 约定随后续里程碑落地,见 IMPLEMENTATION 偏差表)。
    ast::ExprP lambda_expr()
    {
        auto l = std::make_unique<ast::LambdaExpr>();
        l->line = peek().line; l->col = peek().col;
        advance(); // (
        l->params = param_list();
        expect(lex::Tok::RParen, "')' to end lambda parameters");
        if (accept(lex::Tok::Arrow)) l->ret = parse_type();
        if (check(lex::Tok::Throws))
            unsupported("'throws' lambda", "later milestone");
        expect(lex::Tok::Assign, "'=' before lambda body");
        if (check(lex::Tok::LBrace)) {
            l->has_block_body = true;
            l->block_body = block();
        } else {
            l->expr_body = expression();
        }
        return l;
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
            // 条件位置不接受空字面量,避免吞掉 if/while/for 的块括号;
            // '.field' 后必须跟 '=' 才算字段初始化——match 臂的 '.member =>'
            // 不是赋值形状,scrutinee 后的臂块不能被吞成 struct 字面量
            if (check(lex::Tok::LBrace)
                && ((peek(1).tok == lex::Tok::Dot && peek(2).tok == lex::Tok::Ident
                     && peek(3).tok == lex::Tok::Assign)
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
            // 匿名函数(DESIGN §4.6):(x: int) -> int = x * x
            // 识别:'(' 后跟 'name :'(普通表达式无此形状)或 '() ->'
            if ((peek(1).tok == lex::Tok::Ident && peek(2).tok == lex::Tok::Colon
                 && peek(3).tok != lex::Tok::Colon)
                || (peek(1).tok == lex::Tok::RParen
                    && peek(2).tok == lex::Tok::Arrow)) {
                return lambda_expr();
            }
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
        case lex::Tok::Match: {                       // match 表达式(DESIGN §5.5)
            auto x = std::make_unique<ast::MatchExpr>();
            x->line = peek().line; x->col = peek().col;
            advance(); // match
            bool prev = cond_like_;
            cond_like_ = true;
            x->scrutinee = expression();
            cond_like_ = prev;
            expect(lex::Tok::LBrace, "'{' to start match arms");
            while (!check(lex::Tok::RBrace)) {
                if (check(lex::Tok::Eof)) err("expected '}' before end of file");
                if (accept(lex::Tok::Semi)) continue;
                parse_arm(x->arms, /*expr_form*/true);
            }
            expect(lex::Tok::RBrace, "'}' to end match");
            return x;
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
