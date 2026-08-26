#pragma once
// 简易 x86-64 汇编器：只支持 native.cpp 生成的指令子集。
// 输入: .s 文本(intel_syntax noprefix) 输出: 机器码字节 + 重定位表。
// 设计约束: 不追求完整汇编器，只覆盖自身发射器的输出模式，
// 新增指令时在此同步扩展即可 —— 与 native.cpp 是同一作者的契约。
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cpp2::native::asm64 {

struct Reloc { size_t offset; std::string target; bool is_call; };

struct Result {
    std::vector<uint8_t> text;
    // 数据段(字符串字面量等): name -> bytes
    std::map<std::string, std::vector<uint8_t>> rodata;
    // 全局数据段: name -> initial qword
    std::map<std::string, uint64_t> data;
    std::vector<Reloc> relocs;
    // 已定义符号(text 内): name -> offset
    std::map<std::string, size_t> symbols;
    // 外部符号(需导入/运行时): printf, sys_exit 等
    std::vector<std::string> externs;
};

// 解析并汇编
Result assemble(std::string const& source);

} // namespace cpp2::native::asm64
