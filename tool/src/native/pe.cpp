#include "pe.hpp"
#include <cstring>
#include <map>

namespace cpp2::native::pe {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

static void put_u16(std::vector<u8>& o, u16 v){ o.push_back(v&0xff); o.push_back((v>>8)&0xff); }
static void put_u32(std::vector<u8>& o, u32 v){ for(int i=0;i<4;++i) o.push_back((v>>(i*8))&0xff); }
static void put_u64(std::vector<u8>& o, u64 v){ for(int i=0;i<8;++i) o.push_back((v>>(i*8))&0xff); }
static void patch_u32(std::vector<u8>& o, size_t pos, u32 v){ for(int i=0;i<4;++i) o[pos+i]= (v>>(i*8))&0xff; }
static void patch_u64(std::vector<u8>& o, size_t pos, u64 v){ for(int i=0;i<8;++i) o[pos+i]= (v>>(i*8))&0xff; }

std::vector<u8> build_exe(
    const std::vector<u8>& text,
    const std::vector<u8>& rodata,
    const std::vector<Reloc>& relocs,
    const std::vector<std::pair<std::string,std::string>>& rodata_labels)
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

    std::map<std::string, u32> rodata_label_rva;
    for(auto &p: rodata_labels){
        rodata_label_rva[p.first] = rodata_rva + (u32)std::stoi(p.second);
    }
    if(rodata_label_rva.empty() && !rodata.empty()){
        rodata_label_rva[".LS0"] = rodata_rva;
    }
    // idata for 2 DLLs: msvcrt:printf, kernel32:ExitProcess
    // Layout: descriptors 2*20+20 null =60 bytes, then strings, then hint/names, then IATs
    // Offsets relative to idata start
    // 0x00: msvcrt descriptor, 0x14: kernel32 descriptor, 0x28: null, 0x3C: dll strings, etc.
    std::string dll1 = "msvcrt.dll";
    std::string sym1 = "printf";
    std::string dll2 = "kernel32.dll";
    std::string sym2 = "ExitProcess";
    // Calculate offsets
    // descriptors at 0x00
    u32 off_desc1 = 0x00;
    u32 off_desc2 = 0x14;
    u32 off_null = 0x28;
    u32 off_dll1 = 0x3C;
    u32 off_dll2 = off_dll1 + (u32)dll1.size() + 1;
    // align to 2
    u32 off_hint1 = (off_dll2 + (u32)dll2.size() + 1 + 1) & ~1;
    u32 off_hint2 = off_hint1 + 2 + (u32)sym1.size() + 1;
    if(off_hint2 & 1) off_hint2++;
    u32 off_iat1 = (off_hint2 + 2 + (u32)sym2.size() + 1 + 0xF) & ~0xF; // align 16
    u32 off_thunk1 = off_iat1 + 16;
    u32 off_thunk2 = off_thunk1 + 16;
    u32 idata_sz_needed = off_thunk2 + 16;
    // Build idata
    std::vector<u8> imp(idata_sz_needed, 0);
    // Helper to put at offset
    auto put_at = [&](u32 off, auto fn){
        size_t old = imp.size();
        if(off + 64 > imp.size()) imp.resize(off+64,0);
        // fn will put at current imp size, so we need to ensure
    };
    // Instead, build sequentially with exact offsets
    imp.assign(idata_sz_needed, 0);
    // Descriptor 1
    auto put_desc = [&](u32 off, u32 oft, u32 name, u32 ft){
        put_u32(imp, 0); // will patch via direct
        // Actually we need to patch at off
        // Use patch
        auto patch_desc = [&](u32 base, u32 oft_, u32 name_, u32 ft_){
            patch_u32(imp, base+0, oft_);
            patch_u32(imp, base+4, 0);
            patch_u32(imp, base+8, 0);
            patch_u32(imp, base+12, name_);
            patch_u32(imp, base+16, ft_);
        };
        patch_desc(off, oft, name, ft);
    };
    // We'll fill after calculating RVAs
    u32 dll1_rva = idata_rva + off_dll1;
    u32 dll2_rva = idata_rva + off_dll2;
    u32 hint1_rva = idata_rva + off_hint1;
    u32 hint2_rva = idata_rva + off_hint2;
    u32 iat1_rva = idata_rva + off_iat1;
    u32 iat2_rva = idata_rva + off_iat1 + 8; // second entry in same IAT? For simplicity, two separate IATs
    // Actually we have two IATs: one for printf at off_iat1, one for ExitProcess at off_iat1+8?
    // Simpler: put both in same IAT area: IAT for msvcrt at off_iat1 (8 bytes), IAT for kernel32 at off_iat1+8
    // But we have separate thunks. Let's define:
    // IAT1 (for msvcrt) at off_iat1: [hint1_rva, 0]
    // IAT2 (for kernel32) at off_iat1+16: [hint2_rva, 0]  (we already have off_thunk handling)
    // For simplicity, use off_iat1 for printf, off_iat1+16 for ExitProcess

    // Recompute with simpler layout: use off_iat1 for printf IAT, off_iat1+16 for ExitProcess IAT
    u32 iat_printf_rva = idata_rva + off_iat1;
    u32 iat_exit_rva = idata_rva + off_iat1 + 16;
    u32 thunk_printf_rva = idata_rva + off_thunk1;
    u32 thunk_exit_rva = idata_rva + off_thunk2;

    // Now build imp with correct offsets
    // Clear and rebuild
    imp.assign(idata_sz_needed, 0);
    // Descriptor 1: msvcrt
    {
        size_t p = off_desc1;
        patch_u32(imp, p+0, thunk_printf_rva);
        patch_u32(imp, p+12, dll1_rva);
        patch_u32(imp, p+16, iat_printf_rva);
    }
    // Descriptor 2: kernel32
    {
        size_t p = off_desc2;
        patch_u32(imp, p+0, thunk_exit_rva);
        patch_u32(imp, p+12, dll2_rva);
        patch_u32(imp, p+16, iat_exit_rva);
    }
    // Null descriptor at off_null already zero
    // DLL strings
    memcpy(imp.data()+off_dll1, dll1.c_str(), dll1.size()+1);
    memcpy(imp.data()+off_dll2, dll2.c_str(), dll2.size()+1);
    // Hint/Name 1
    imp[off_hint1]=0; imp[off_hint1+1]=0;
    memcpy(imp.data()+off_hint1+2, sym1.c_str(), sym1.size()+1);
    // Hint/Name 2
    imp[off_hint2]=0; imp[off_hint2+1]=0;
    memcpy(imp.data()+off_hint2+2, sym2.c_str(), sym2.size()+1);
    // IATs
    patch_u64(imp, off_iat1, hint1_rva);
    patch_u64(imp, off_iat1+8, 0);
    patch_u64(imp, off_iat1+16, hint2_rva);
    patch_u64(imp, off_iat1+24, 0);
    // Thunks
    patch_u64(imp, off_thunk1, hint1_rva);
    patch_u64(imp, off_thunk1+8, 0);
    patch_u64(imp, off_thunk2, hint2_rva);
    patch_u64(imp, off_thunk2+8, 0);

    u32 idata_sz = (u32)imp.size();
    u32 idata_raw_sz = align(idata_sz, file_align);
    u32 image_sz = align(idata_rva + idata_sz, sect_align);
    u32 hdr_raw_sz = hdr_size;

    std::vector<u8> out;
    out.reserve(image_sz + hdr_raw_sz);
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
    put_u32(out, text_rva);
    put_u32(out, text_rva);
    put_u64(out, image_base);
    put_u32(out, sect_align);
    put_u32(out, file_align);
    put_u16(out, 6); put_u16(out, 0);
    put_u16(out, 0); put_u16(out, 0);
    put_u16(out, 6); put_u16(out, 0);
    put_u32(out, 0);
    put_u32(out, image_sz);
    put_u32(out, hdr_raw_sz);
    put_u32(out, 0);
    put_u16(out, 3);
    put_u16(out, 0x8160);
    put_u64(out, 0x100000);
    put_u64(out, 0x1000);
    put_u64(out, 0x100000);
    put_u64(out, 0x1000);
    put_u32(out, 0);
    put_u32(out, 16);
    put_u32(out,0); put_u32(out,0); // 0 Export
    put_u32(out, idata_rva); put_u32(out, idata_sz); // 1 Import
    for(int i=2;i<16;++i){ put_u32(out,0); put_u32(out,0); }
    size_t iat_dir_off = opt_start + 112 + 12*8;
    patch_u32(out, iat_dir_off, iat_printf_rva);
    patch_u32(out, iat_dir_off+4, 32);
    auto put_sect = [&](const char* name, u32 vsz, u32 va, u32 rawsz, u32 rawptr, u32 chars){
        char n[8]={0}; strncpy(n,name,8);
        for(int i=0;i<8;++i) out.push_back(n[i]);
        put_u32(out, vsz);
        put_u32(out, va);
        put_u32(out, rawsz);
        put_u32(out, rawptr);
        put_u32(out, 0); put_u32(out, 0);
        put_u16(out, 0); put_u16(out, 0);
        put_u32(out, chars);
    };
    put_sect(".text", text_sz, text_rva, text_raw_sz, text_raw, 0x60000020);
    put_sect(".rdata", rodata_sz, rodata_rva, rodata_raw_sz, rodata_raw, 0x40000040);
    put_sect(".idata", idata_sz, idata_rva, idata_raw_sz, idata_raw, 0x40000040);
    while(out.size() < hdr_raw_sz) out.push_back(0);
    size_t text_off = out.size();
    out.insert(out.end(), text.begin(), text.end());
    while(out.size() < text_raw + text_raw_sz) out.push_back(0);
    // patch relocs
    for(auto &r: relocs){
        u32 target_rva = 0;
        if(rodata_label_rva.count(r.target)){
            target_rva = rodata_label_rva[r.target];
        } else if(r.target == "printf"){
            target_rva = iat_printf_rva;
        } else if(r.target == "ExitProcess"){
            target_rva = iat_exit_rva;
        } else {
            auto it = rodata_label_rva.find(r.target);
            if(it!=rodata_label_rva.end()) target_rva = it->second;
            else target_rva = rodata_rva;
        }
        u32 rel_pos = (u32)(text_rva + r.offset + 4);
        int32_t disp = (int32_t)(target_rva - rel_pos);
        patch_u32(out, text_off + r.offset, (u32)disp);
    }
    size_t rodata_off = out.size();
    out.insert(out.end(), rodata.begin(), rodata.end());
    while(out.size() < rodata_raw + rodata_raw_sz) out.push_back(0);
    size_t idata_off = out.size();
    out.insert(out.end(), imp.begin(), imp.end());
    while(out.size() < idata_raw + idata_raw_sz) out.push_back(0);
    return out;
}

} // namespace
