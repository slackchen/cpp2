#pragma once
// PE/COFF 直写（Windows x64, 零 CRT:kernel32!GetStdHandle/WriteFile/ExitProcess）
// 跨平台约定:本层只产 PE;ELF 由 native/elf.cpp 对等实现(同一 Reloc 抽象)。
#include <cstdint>
#include <string>
#include <vector>

namespace cpp2::native::pe {

// RIP 相对重定位:target ∈ rodata 标签 | text 标签(thunk) | 导入符号(IAT)
// offset = disp32 位置; ref = 指令尾(用于计算 RIP 相对位移的基准)
struct Reloc { size_t offset; std::string target; size_t ref; bool is_call; };

// 生成最小 PE64 可执行（入口 main，零 CRT）
// text:   .text 内容（已含入口 + 可选 cpp2_write thunk，RIP 重定位待填）
// rodata: .rodata 内容（字符串等）
// relocs: text 中对 rodata 标签 / 导入符号 / text 内标签 的 RIP 相对重定位
//         target 解析顺序: rodata_labels → "cpp2_write"(text 内) → IAT(GetStdHandle/
//         WriteFile/ExitProcess)
// rodata_labels: label -> offset in rodata
// text_labels:   label -> offset in text（thunk 等内部调用目标）
std::vector<uint8_t> build_exe(
    const std::vector<uint8_t>& text,
    const std::vector<uint8_t>& rodata,
    const std::vector<Reloc>& relocs,
    const std::vector<std::pair<std::string,std::string>>& rodata_labels,
    const std::vector<std::pair<std::string,size_t>>& text_labels = {},
    // 动态导入表: {DLL名, 符号列表}
    const std::vector<std::pair<std::string, std::vector<std::string>>>& imports = {
        {"kernel32.dll", {"GetStdHandle", "WriteFile", "ExitProcess"}}
    }
);

} // namespace cpp2::native::pe
