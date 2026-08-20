// C++2 手写递归下降语法分析器(M2a 子集)
#pragma once

#include "ast.hpp"
#include "lexer.hpp"

#include <string>
#include <vector>

namespace cpp2::parse {

struct ParseError {
    int line, col;
    std::string msg;
};

// 解析整个翻译单元;失败抛 ParseError。
ast::Module parse(std::vector<lex::Token> toks, std::string const& source_name);

} // namespace cpp2::parse
