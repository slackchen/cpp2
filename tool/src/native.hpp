// 原生后端 v0(P1 原型,docs/native-backend-eval.md P1):
// x86-64 System V 汇编直译。子集 = 整数/控制流核心:
//   函数(≤6 整形参数)、递归调用、if/else、while、for 范围、break/continue、
//   算术(+ - * / % << >>)、比较、&& || !、一元 -、局部变量、int 全局、
//   print_int(int) / print_str? 否——字符串不支持(v0)。
// 子集外任何构造 → NativeUnsupported → CLI 自动回退转译路径(需求:平台/特性回退)。
// 代码模型:栈机风格——表达式值经 rax 传递,临时量压栈;无寄存器分配。
#pragma once

#include "ast.hpp"
#include "sema.hpp"

#include <stdexcept>
#include <string>

namespace cpp2::native {

struct Unsupported : std::runtime_error {
    std::string msg;
    explicit Unsupported(std::string m) : std::runtime_error(m), msg(std::move(m)) {}
};

// 单模块 → x86-64 汇编(AT&T/GAS,intel_syntax noprefix)。
// 抛 Unsupported 表示含子集外构造(调用方回退转译)。
std::string emit_asm(ast::Module& m, sema::Result const& r);
// Windows 直出 PE（不经 g++），仅 hello 等最小子集
std::vector<uint8_t> emit_pe(ast::Module& m, sema::Result const& r);
// 通用 native: .s 文本 → asm64 汇编 → PE/ELF 字节(零外部工具)
// 抛 Unsupported 表示含不支持构造
std::vector<uint8_t> emit_native(const std::string& asm_text);

} // namespace cpp2::native
