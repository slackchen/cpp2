// 后端(C++23 编译器)诊断过滤与归一化。
// 目标:漏过 sema 到编译期的错误,呈现为"少噪声、路径可读、来源明确"的诊断,
// 而不是原始编译器倾泻。位置信息经 #line 已映射回 .cpp2,原样保留;
// 生成文件自身的帧打 [generated] 标记(那代表降低器缺陷,需要报告)。
#pragma once

#include <string>

namespace cpp2::diagfilter {

// 过滤并归一化编译器输出。build_dir 用于识别生成文件帧。
std::string filter(std::string const& raw, std::string const& build_dir);

constexpr char const* banner =
    "[cpp2] ---- backend (C++23) diagnostics;locations map back to .cpp2 via #line ----\n";

} // namespace cpp2::diagfilter
