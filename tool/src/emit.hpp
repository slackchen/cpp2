// C++2 → C++23 发射器(整程序模式,IMPLEMENTATION.md §4)
#pragma once

#include "ast.hpp"
#include "sema.hpp"

#include <string>

namespace cpp2::emit {

// 生成完整翻译单元文本。sema 提供表达式类型注解(检查注入依据);
// source_name 用于 #line 映射。
std::string emit_translation_unit(ast::Module& m, sema::Result const& sema,
                                  std::string const& source_name);

} // namespace cpp2::emit
