#pragma once
// x64 编码 helpers（Win64/SysV 通用，RIP 相对重定位由 pe/elf 处理）
#include <cstdint>
#include <string>
#include <vector>

namespace cpp2::native::x64 {

enum Reg { RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7, R8=8, R9=9, R10=10, R11=11, R12=12, R13=13, R14=14, R15=15 };

struct Label { std::string name; size_t pos = 0; bool bound = false; };
struct Reloc { size_t pos; std::string target; int addend = 0; bool is_call = false; };

struct Emitter {
    std::vector<uint8_t> code;
    std::vector<Reloc> relocs;
    // 简单 label 管理（仅用于 jmp/call 的 RIP 相对）
    size_t cur() const { return code.size(); }
    void put8(uint8_t b) { code.push_back(b); }
    void put32(int32_t v) { for(int i=0;i<4;++i) code.push_back(uint8_t((v>>(i*8))&0xff)); }
    void put64(int64_t v) { for(int i=0;i<8;++i) code.push_back(uint8_t((v>>(i*8))&0xff)); }
    // 常用指令
    void push_rbp() { put8(0x55); }
    void mov_rbp_rsp() { put8(0x48); put8(0x89); put8(0xE5); }
    void sub_rsp_imm8(int8_t v) { put8(0x48); put8(0x83); put8(0xEC); put8(uint8_t(v)); }
    void add_rsp_imm8(int8_t v) { put8(0x48); put8(0x83); put8(0xC4); put8(uint8_t(v)); }
    void mov_rsp_rbp() { put8(0x48); put8(0x89); put8(0xEC); }
    void pop_rbp() { put8(0x5D); }
    void ret() { put8(0xC3); }
    void xor_eax_eax() { put8(0x31); put8(0xC0); }
    void xor_ecx_ecx() { put8(0x31); put8(0xC9); }
    void mov_rax_imm32(int32_t v) { put8(0x48); put8(0xC7); put8(0xC0); put32(v); }
    void mov_rcx_imm32(int32_t v) { put8(0xB9); put32(v); }
    void mov_edx_imm32(int32_t v) { put8(0xBA); put32(v); }   // edx = imm32(Win64 第2参)
    // lea rcx, [rip+disp32]  -> 48 8D 0D disp32
    void lea_rcx_rip(const std::string& target) {
        put8(0x48); put8(0x8D); put8(0x0D);
        relocs.push_back({cur(), target, 0, false});
        put32(0);
    }
    void lea_rdi_rip(const std::string& target) {
        put8(0x48); put8(0x8D); put8(0x3D);
        relocs.push_back({cur(), target, 0, false});
        put32(0);
    }
    void call_rel(const std::string& target) {
        put8(0xE8);
        relocs.push_back({cur(), target, 0, true});
        put32(0);
    }
    void call_indirect_rip(const std::string& target) {
        put8(0xFF); put8(0x15);
        relocs.push_back({cur(), target, 0, true});
        put32(0);
    }
    void mov_mem_rbp_disp8(Reg r, int8_t disp) {
        // mov QWORD PTR [rbp+disp], r  (r = rcx,rdx,r8,r9 etc)
        // REX.W + 89 /r  modrm
        uint8_t rex = 0x48;
        if (r >= 8) rex |= 0x01; // REX.B for r
        // actually for mov [rbp+disp], reg
        // mod=01 (disp8), reg, rm=101 (rbp)
        put8(rex);
        put8(0x89);
        uint8_t modrm = 0x40 | ((r & 7) << 3) | 0x05;
        put8(modrm);
        put8(uint8_t(disp));
    }
};

} // namespace cpp2::native::x64
