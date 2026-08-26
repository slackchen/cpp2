#include "pe.hpp"
#include <cstring>
#include <map>

namespace cpp2::native::pe {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

static void put_u16(std::vector<u8>& o, u16 v){ o.push_back(v&0xff); o.push_back((v>>8)&0xff); }
static void put_u32(std::vector<u8>& o, u32 v){ for(int i=0;i<4;++i) o.push_back((v>>(i*8))&0xff); }
static void put_u64(std::vector<u8>& o, u64 v){ for(int i=0;i<8;++i) o.push_back((v>>(i*8))&0xff); }
static void patch_u16(std::vector<u8>& o, size_t pos, u16 v){ for(int i=0;i<2;++i) o[pos+i]= (v>>(i*8))&0xff; }
static void patch_u32(std::vector<u8>& o, size_t pos, u32 v){ for(int i=0;i<4;++i) o[pos+i]= (v>>(i*8))&0xff; }
static void put_u64_at(std::vector<u8>& o, size_t pos, u64 v){ for(int i=0;i<8;++i) o[pos+i]= (v>>(i*8))&0xff; }

// ── 导入表布局（单 DLL:kernel32,三符号,零 CRT）────────────────────
// 布局(相对 .idata 起):
//   0x00 descriptor(kernel32)  0x14 null descriptor
//   0x28 dll name "kernel32.dll"
//   0x40.. hint/name[3] (各对齐2)
//   IAT 区(off_iat): 3 项 x (8+8 终结0),OriginalFirstThunk 同尺寸随其后
struct Imports {
    std::string dll = "kernel32.dll";
    std::string syms[3] = { "GetStdHandle", "WriteFile", "ExitProcess" };
    // 计算后填充
    u32 off_hint[3] = {0,0,0};
    u32 off_iat = 0;      // 相对偏移;IAT[k] 在 off_iat + k*16
    u32 off_thunk = 0;
    u32 size = 0;
};

static void layout_imports(Imports& im)
{
    u32 cur = 0x40;                       // descriptors(0x00/0x14)+dll 名(0x28..0x3F)
    for (int k = 0; k < 3; ++k) {
        if (cur & 1) ++cur;               // hint/name 需 2 对齐
        im.off_hint[k] = cur;
        cur += 2 + (u32)im.syms[k].size() + 1;
    }
    im.off_iat = (cur + 15) & ~15u;
    im.off_thunk = im.off_iat + 32;       // 3 x 8B 打包 + 1 null 槽(数组连续!)
    im.size = im.off_thunk + 32;
}

std::vector<u8> build_exe(
    const std::vector<u8>& text,
    const std::vector<u8>& rodata,
    const std::vector<Reloc>& relocs,
    const std::vector<std::pair<std::string,std::string>>& rodata_labels,
    const std::vector<std::pair<std::string,size_t>>& text_labels)
{
    const u64 image_base = 0x140000000ULL;
    const u32 sect_align = 0x1000;
    const u32 file_align = 0x200;
    auto align = [](u32 v, u32 a){ return (v + a - 1) & ~(a - 1); };

    u32 hdr_size = 0x400;
    u32 text_rva = 0x1000;
    u32 text_raw = hdr_size;
    u32 text_sz = (u32)text.size();
    u32 text_raw_sz = align(text_sz, file_align);
    u32 rodata_rva = align(text_rva + text_sz, sect_align);
    u32 rodata_raw = align(text_raw + text_raw_sz, file_align);
    u32 rodata_sz = (u32)rodata.size();
    u32 rodata_raw_sz = align(rodata_sz, file_align);
    u32 idata_rva = align(rodata_rva + rodata_sz, sect_align);
    u32 idata_raw = align(rodata_raw + rodata_raw_sz, file_align);

    std::map<std::string, u32> ro_label_rva;
    for (auto& p : rodata_labels)
        ro_label_rva[p.first] = rodata_rva + (u32)std::stoi(p.second);
    std::map<std::string, u32> tx_label_rva;
    for (auto& p : text_labels)
        tx_label_rva[p.first] = text_rva + (u32)p.second;

    Imports imp;
    layout_imports(imp);
    u32 iat_rva[3];
    for (int k = 0; k < 3; ++k)
        iat_rva[k] = idata_rva + imp.off_iat + (u32)(k * 8);   // 打包数组,步长 8

    // ── 组装 .idata ──
    std::vector<u8> idata((size_t)imp.size, 0);
    auto desc = [&](u32 base, u32 oft, u32 name, u32 ft){
        patch_u32(idata, base+0, oft);
        patch_u32(idata, base+12, name);
        patch_u32(idata, base+16, ft);
    };
    desc(0x00, idata_rva + imp.off_thunk, idata_rva + 0x28, idata_rva + imp.off_iat);
    memcpy(idata.data()+0x28, imp.dll.c_str(), imp.dll.size()+1);
    for (int k = 0; k < 3; ++k) {
        u32 hn = idata_rva + imp.off_hint[k];
        patch_u16(idata, imp.off_hint[k], 0);
        memcpy(idata.data()+imp.off_hint[k]+2, imp.syms[k].c_str(), imp.syms[k].size()+1);
        put_u64_at(idata, imp.off_iat   + (size_t)k*8, hn);   // IAT:连续数组
        put_u64_at(idata, imp.off_thunk + (size_t)k*8, hn);   // LookupTable:同
    }

    u32 idata_sz = (u32)idata.size();
    u32 idata_raw_sz = align(idata_sz, file_align);
    u32 image_sz = align(idata_rva + idata_sz, sect_align);

    // ── 头 ──
    std::vector<u8> out;
    out.reserve(image_sz + hdr_size);
    out.resize(0x80, 0);
    out[0]='M'; out[1]='Z';
    out[0x3C]=0x80;
    out.push_back('P'); out.push_back('E'); out.push_back(0); out.push_back(0);
    put_u16(out, 0x8664);
    put_u16(out, 3);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u16(out, 0xF0);
    put_u16(out, 0x22);
    size_t opt_start = out.size();
    put_u16(out, 0x020B);
    out.push_back(0x0E); out.push_back(0x1E);
    put_u32(out, text_raw_sz + rodata_raw_sz + idata_raw_sz);
    put_u32(out, 0);
    put_u32(out, 0);
    put_u32(out, text_rva);                    // AddressOfEntryPoint
    put_u32(out, text_rva);
    put_u64(out, image_base);
    put_u32(out, sect_align);
    put_u32(out, file_align);
    put_u16(out, 6); put_u16(out, 0);
    put_u16(out, 0); put_u16(out, 0);
    put_u16(out, 6); put_u16(out, 0);
    put_u32(out, 0);
    put_u32(out, image_sz);
    put_u32(out, hdr_size);
    put_u32(out, 0);
    put_u16(out, 3);                           // CUI
    put_u16(out, 0x8160);
    put_u64(out, 0x100000);
    put_u64(out, 0x1000);
    put_u64(out, 0x100000);
    put_u64(out, 0x1000);
    put_u32(out, 0);
    put_u32(out, 16);
    put_u32(out,0);  put_u32(out,0);           // [0] Export
    put_u32(out, idata_rva); put_u32(out, idata_sz);  // [1] Import
    for (int i=2;i<12;++i){ put_u32(out,0); put_u32(out,0); }   // [2..11]
    put_u32(out, idata_rva + imp.off_iat); put_u32(out, 32);    // [12] IAT
    for (int i=13;i<16;++i){ put_u32(out,0); put_u32(out,0); }

    auto put_sect = [&](const char* name, u32 vsz, u32 va, u32 rawsz, u32 rawptr, u32 chars){
        char n[8]={0}; strncpy(n,name,8);
        for(int i=0;i<8;++i) out.push_back(n[i]);
        put_u32(out, vsz); put_u32(out, va);
        put_u32(out, rawsz); put_u32(out, rawptr);
        put_u32(out, 0); put_u32(out, 0);
        put_u16(out, 0); put_u16(out, 0);
        put_u32(out, chars);
    };
    put_sect(".text",  text_sz,  text_rva,  text_raw_sz,  text_raw,  0x60000020);
    put_sect(".rdata", rodata_sz,rodata_rva,rodata_raw_sz,rodata_raw,0x40000040);
    put_sect(".idata", idata_sz, idata_rva, idata_raw_sz, idata_raw, 0xC0000040);

    while (out.size() < hdr_size) out.push_back(0);

    // ── .text + 重定位 ──
    size_t text_off = out.size();
    out.insert(out.end(), text.begin(), text.end());
    while (out.size() < text_raw + text_raw_sz) out.push_back(0);
    for (auto& r : relocs) {
        u32 target_rva = 0;
        bool resolved = false;
        if (tx_label_rva.count(r.target)) {              // text 内标签(thunk 等)
            target_rva = tx_label_rva[r.target]; resolved = true;
        } else if (ro_label_rva.count(r.target)) {       // rodata 字符串
            target_rva = ro_label_rva[r.target]; resolved = true;
        } else {
            for (int k = 0; k < 3; ++k)                  // kernel32 IAT
                if (r.target == imp.syms[k]) {
                    target_rva = iat_rva[k]; resolved = true; break;
                }
        }
        if (!resolved) target_rva = rodata_rva;          // 兜底(不应发生)
        u32 rel_pos = text_rva + (u32)r.offset + 4;
        int32_t disp = (int32_t)(target_rva - rel_pos);
        patch_u32(out, text_off + r.offset, (u32)disp);
    }

    // ── .rodata / .idata 原文 ──
    out.insert(out.end(), rodata.begin(), rodata.end());
    while (out.size() < rodata_raw + rodata_raw_sz) out.push_back(0);
    out.insert(out.end(), idata.begin(), idata.end());
    while (out.size() < idata_raw + idata_raw_sz) out.push_back(0);
    return out;
}

} // namespace cpp2::native::pe
