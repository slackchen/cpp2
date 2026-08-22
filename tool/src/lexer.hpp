// C++2 词法分析器(M2a 子集)
#pragma once

#include <string>
#include <vector>

namespace cpp2::lex {

enum class Tok {
    // 关键字
    Module, Import, Export, Type, Enum, Variant, Concept,
    If, Else, While, For, In, Return, Break, Continue,
    Const, Mutates, Throws, Match, True, False,
    // 字面量与名字
    Ident, IntLit, DoubleLit, StringLit, CharLit,
    // 标点
    Colon, Walrus, Semi, Comma, Dot, DotDot, DotDotEq,
    Arrow, FatArrow, Question, Bang,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Lt, Gt, Assign, Eq, Ne, Le, Ge,
    AndAnd, OrOr, Amp, Pipe, Caret, Tilde,
    Plus, Minus, Star, Slash, Percent,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    At, Underscore,
    LegacyBlock,                            // cxx_legacy { … } 原文(M6)
    Eof
};

struct Token {
    Tok tok = Tok::Eof;
    std::string text;                        // 原文
    int line = 0, col = 0;
};

inline bool is_keyword(Tok t)
{
    switch (t) {
    case Tok::Module: case Tok::Import: case Tok::Export: case Tok::Type:
    case Tok::Enum: case Tok::Variant: case Tok::Concept:
    case Tok::If: case Tok::Else: case Tok::While: case Tok::For:
    case Tok::In: case Tok::Return: case Tok::Break: case Tok::Continue:
    case Tok::Const: case Tok::Mutates: case Tok::Throws: case Tok::Match:
    case Tok::True: case Tok::False:
        return true;
    default:
        return false;
    }
}

struct LexError {
    int line, col;
    std::string msg;
};

// 返回 Token 流(以 Eof 结尾);失败抛 LexError。
std::vector<Token> lex(std::string const& src);

} // namespace cpp2::lex
