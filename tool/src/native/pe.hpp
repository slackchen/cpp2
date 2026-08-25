#pragma once
// PE/COFF 直写（Windows x64, 最小可执行，msvcrt:printf）
#include <cstdint>
#include <string>
#include <vector>

namespace cpp2::native::pe {

// 生成最小 PE64 可执行（入口 main，依赖 msvcrt:printf）
// code: .text 内容（已含 main，RIP 重定位待填）
// rodata: .rodata 内容（字符串等）
// relocs: code 中对 rodata/导入的 RIP 相对重定位
struct Reloc { size_t offset; std::string target; bool is_call; };

std::vector<uint8_t> build_exe(
    const std::vector<uint8_t>& text,
    const std::vector<uint8_t>& rodata,
    const std::vector<Reloc>& relocs,
    const std::vector<std::pair<std::string,std::string>>& rodata_labels // label -> offset in rodata
);

} // namespace cpp2::native::pe
