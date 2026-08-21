// C++2 词法分析器实现
#include "lexer.hpp"

#include <cctype>
#include <stdexcept>

namespace cpp2::lex {

namespace {

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    explicit Lexer(std::string const& src) : src_(src) {}

    std::vector<Token> run()
    {
        std::vector<Token> out;
        for (;;) {
            skip_ws_and_comments();
            Token t = next();
            out.push_back(t);
            if (t.tok == Tok::Eof) break;
        }
        return out;
    }

private:
    std::string const& src_;
    size_t pos_ = 0;
    int line_ = 1, col_ = 1;

    [[noreturn]] void err(std::string msg)
    {
        throw LexError{line_, col_, std::move(msg)};
    }

    char peek(size_t ahead = 0) const
    {
        size_t i = pos_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }

    char get()
    {
        char c = peek();
        if (c == '\0') err("unexpected end of file");
        ++pos_;
        if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
        return c;
    }

    bool try_get(char c)
    {
        if (peek() == c) { get(); return true; }
        return false;
    }

    void skip_ws_and_comments()
    {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { get(); continue; }
            if (c == '/' && peek(1) == '/') {
                while (peek() != '\n' && peek() != '\0') get();
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                int sl = line_, sc = col_;
                get(); get();
                for (;;) {
                    if (peek() == '\0') { line_ = sl; col_ = sc; err("unterminated block comment"); }
                    if (peek() == '*' && peek(1) == '/') { get(); get(); break; }
                    get();
                }
                continue;
            }
            break;
        }
    }

    Token make(Tok t, std::string text, int line, int col)
    {
        Token tok; tok.tok = t; tok.text = std::move(text); tok.line = line; tok.col = col;
        return tok;
    }

    Token next()
    {
        if (peek() == '\0') return make(Tok::Eof, "", line_, col_);

        int sl = line_, sc = col_;
        char c = peek();

        if (is_ident_start(c)) return ident();
        if (std::isdigit(static_cast<unsigned char>(c))) return number();
        if (c == '"') return string_lit();
        if (c == '\'') return char_lit();

        // 多字符运算符优先
        if (c == '.') {
            get();
            if (peek() == '.') {
                get();
                if (try_get('=')) return make(Tok::DotDotEq, "..=", sl, sc);
                return make(Tok::DotDot, "..", sl, sc);
            }
            return make(Tok::Dot, ".", sl, sc);
        }
        if (c == ':') {
            get();
            if (try_get('=')) return make(Tok::Walrus, ":=", sl, sc);
            return make(Tok::Colon, ":", sl, sc);
        }
        if (c == '-') {
            get();
            if (try_get('>')) return make(Tok::Arrow, "->", sl, sc);
            if (try_get('=')) return make(Tok::MinusEq, "-=", sl, sc);
            return make(Tok::Minus, "-", sl, sc);
        }
        if (c == '=') {
            get();
            if (try_get('=')) return make(Tok::Eq, "==", sl, sc);
            if (try_get('>')) return make(Tok::FatArrow, "=>", sl, sc);
            return make(Tok::Assign, "=", sl, sc);
        }
        if (c == '!') {
            get();
            if (try_get('=')) return make(Tok::Ne, "!=", sl, sc);
            return make(Tok::Bang, "!", sl, sc);
        }
        if (c == '<') { get(); if (try_get('=')) return make(Tok::Le, "<=", sl, sc); return make(Tok::Lt, "<", sl, sc); }
        if (c == '>') { get(); if (try_get('=')) return make(Tok::Ge, ">=", sl, sc); return make(Tok::Gt, ">", sl, sc); }
        if (c == '&') { get(); if (try_get('&')) return make(Tok::AndAnd, "&&", sl, sc); return make(Tok::Amp, "&", sl, sc); }
        if (c == '|') { get(); if (try_get('|')) return make(Tok::OrOr, "||", sl, sc); return make(Tok::Pipe, "|", sl, sc); }
        if (c == '+') { get(); if (try_get('=')) return make(Tok::PlusEq, "+=", sl, sc); return make(Tok::Plus, "+", sl, sc); }
        if (c == '*') { get(); if (try_get('=')) return make(Tok::StarEq, "*=", sl, sc); return make(Tok::Star, "*", sl, sc); }
        if (c == '/') { get(); if (try_get('=')) return make(Tok::SlashEq, "/=", sl, sc); return make(Tok::Slash, "/", sl, sc); }
        if (c == '%') { get(); if (try_get('=')) return make(Tok::PercentEq, "%=", sl, sc); return make(Tok::Percent, "%", sl, sc); }

        get(); // 单字符
        switch (c) {
        case ';': return make(Tok::Semi, ";", sl, sc);
        case ',': return make(Tok::Comma, ",", sl, sc);
        case '(': return make(Tok::LParen, "(", sl, sc);
        case ')': return make(Tok::RParen, ")", sl, sc);
        case '{': return make(Tok::LBrace, "{", sl, sc);
        case '}': return make(Tok::RBrace, "}", sl, sc);
        case '[': return make(Tok::LBracket, "[", sl, sc);
        case ']': return make(Tok::RBracket, "]", sl, sc);
        case '?': return make(Tok::Question, "?", sl, sc);
        case '~': return make(Tok::Tilde, "~", sl, sc);
        case '^': return make(Tok::Caret, "^", sl, sc);
        case '@': return make(Tok::At, "@", sl, sc);
        default: {
            line_ = sl; col_ = sc;
            err(std::string("unexpected character '") + c + "'");
        }
        }
    }

    Token ident()
    {
        int sl = line_, sc = col_;
        std::string s;
        while (is_ident_char(peek())) s += get();
        if (s == "_") return make(Tok::Underscore, s, sl, sc);
        return make(keyword_or_ident(s), s, sl, sc);
    }

    static Tok keyword_or_ident(std::string const& s)
    {
        if (s == "module")    return Tok::Module;
        if (s == "import")    return Tok::Import;
        if (s == "export")    return Tok::Export;
        if (s == "type")      return Tok::Type;
        if (s == "enum")      return Tok::Enum;
        if (s == "variant")   return Tok::Variant;
        if (s == "concept")   return Tok::Concept;
        if (s == "if")        return Tok::If;
        if (s == "else")      return Tok::Else;
        if (s == "while")     return Tok::While;
        if (s == "for")       return Tok::For;
        if (s == "in")        return Tok::In;
        if (s == "return")    return Tok::Return;
        if (s == "break")     return Tok::Break;
        if (s == "continue")  return Tok::Continue;
        if (s == "const")     return Tok::Const;
        if (s == "mutates")   return Tok::Mutates;
        if (s == "throws")    return Tok::Throws;
        if (s == "match")     return Tok::Match;
        if (s == "true")      return Tok::True;
        if (s == "false")     return Tok::False;
        return Tok::Ident;
    }

    Token number()
    {
        int sl = line_, sc = col_;
        std::string s;
        bool is_double = false;
        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '\'') s += get();
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            is_double = true;
            s += get();
            while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '\'') s += get();
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t ahead = 1;
            if (peek(1) == '+' || peek(1) == '-') ahead = 2;
            if (std::isdigit(static_cast<unsigned char>(peek(ahead)))) {
                is_double = true;
                while (ahead--) s += get();
                while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '\'') s += get();
            }
        }
        return make(is_double ? Tok::DoubleLit : Tok::IntLit, s, sl, sc);
    }

    Token string_lit()
    {
        int sl = line_, sc = col_;
        std::string s;
        s += get(); // 开引号
        for (;;) {
            char c = peek();
            if (c == '\0' || c == '\n') err("unterminated string literal");
            s += get();
            if (c == '\\') {
                if (peek() == '\0' || peek() == '\n') err("unterminated escape in string literal");
                s += get();
            } else if (c == '"') break;
        }
        return make(Tok::StringLit, s, sl, sc);
    }

    Token char_lit()
    {
        int sl = line_, sc = col_;
        std::string s;
        s += get();
        for (;;) {
            char c = peek();
            if (c == '\0' || c == '\n') err("unterminated character literal");
            s += get();
            if (c == '\\') {
                if (peek() == '\0' || peek() == '\n') err("unterminated escape in character literal");
                s += get();
            } else if (c == '\'') break;
        }
        return make(Tok::CharLit, s, sl, sc);
    }
};

} // namespace

std::vector<Token> lex(std::string const& src)
{
    return Lexer(src).run();
}

} // namespace cpp2::lex
