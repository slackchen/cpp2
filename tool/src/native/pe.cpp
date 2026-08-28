#include "pe.hpp"
#include <cstring>
#include <map>
#include <unordered_map>

namespace cpp2::native::pe {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

static void put_u16(std::vector<u8>& o, u16 v){ o.push_back(v&0xff); o.push_back((v>>8)&0xff); }
static void put_u32(std::vector<u8>& o, u32 v){ for(int i=0;i<4;++i) o.push_back((v>>(i*8))&0xff); }
static void put_u64(std::vector<u8>& o, u64 v){ for(int i=0;i<8;++i) o.push_back((v>>(i*8))&0xff); }
static void patch_u16(std::vector<u8>& o, size_t pos, u16 v){ for(int i=0;i<2;++i) o[pos+i]= (v>>(i*8))&0xff; }
static void patch_u32(std::vector<u8>& o, size_t pos, u32 v){ for(int i=0;i<4;++i) o[pos+i]= (v>>(i*8))&0xff; }
static void put_u64_at(std::vector<u8>& o, size_t pos, u64 v){ for(int i=0;i<8;++i) o[pos+i]= (v>>(i*8))&0xff; }

std::vector<u8> build_exe(
    const std::vector<u8>& text,
    const std::vector<u8>& rodata,
    const std::vector<Reloc>& relocs,
    const std::vector<std::pair<std::string,std::string>>& rodata_labels,
    const std::vector<std::pair<std::string,size_t>>& text_labels,
    // 动态导入表:DLL名 → 符号列表。至少含 kernel32 三件套;
    // 额外 DLL(如 msvcrt.dll:printf)按需追加。
    const std::vector<std::pair<std::string, std::vector<std::string>>>& imports,
    // 可写 .data 段(全局槽,如错误消息指针);data_labels = name → offset in data
    const std::vector<u8>& data,
    const std::vector<std::pair<std::string,std::string>>& data_labels)
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
    u32 rodata_sz = (u32)rodata.size();
    bool has_rodata = rodata_sz > 0;
    u32 rodata_rva = has_rodata ? align(text_rva + text_sz, sect_align) : 0;
    u32 rodata_raw = has_rodata ? align(text_raw + text_raw_sz, file_align) : 0;
    u32 rodata_raw_sz = has_rodata ? align(rodata_sz, file_align) : 0;
    u32 data_sz = (u32)data.size();
    bool has_data = data_sz > 0;
    u32 data_rva = has_rodata ? align(rodata_rva + rodata_sz, sect_align)
                              : align(text_rva + text_sz, sect_align);
    u32 data_raw = has_rodata ? align(rodata_raw + rodata_raw_sz, file_align)
                              : align(text_raw + text_raw_sz, file_align);
    u32 data_raw_sz = has_data ? align(data_sz, file_align) : 0;
    u32 idata_rva = has_data ? align(data_rva + data_sz, sect_align)
                             : has_rodata ? align(rodata_rva + rodata_sz, sect_align)
                               : align(text_rva + text_sz, sect_align);
    u32 idata_raw = has_data ? align(data_raw + data_raw_sz, file_align)
                             : has_rodata ? align(rodata_raw + rodata_raw_sz, file_align)
                               : align(text_raw + text_raw_sz, file_align);

    // ── 标签映射 ──
    std::map<std::string, u32> ro_label_rva;
    for (auto& p : rodata_labels)
        ro_label_rva[p.first] = rodata_rva + (u32)std::stoul(p.second);
    std::map<std::string, u32> da_label_rva;
    for (auto& p : data_labels)
        da_label_rva[p.first] = data_rva + (u32)std::stoul(p.second);
    std::map<std::string, u32> tx_label_rva;
    for (auto& p : text_labels)
        tx_label_rva[p.first] = text_rva + (u32)p.second;
    u32 entry_rva = text_rva;
    if (tx_label_rva.count("main")) entry_rva = tx_label_rva["main"];
    else if (tx_label_rva.count("_main")) entry_rva = tx_label_rva["_main"];

    // ── 导入表布局(多 DLL 动态)──
    int ndll = (int)imports.size();
    int total_syms = 0;
    for (auto& [dll, syms] : imports) total_syms += (int)syms.size();

    // 布局计算(相对 .idata 起):
    //   descriptors: ndll * 20 + 20(null) = idata_desc_end
    u32 desc_size = (u32)((ndll + 1) * 20);
    u32 cur = desc_size;

    // 每个 DLL 的名字
    std::vector<u32> dll_name_off(ndll);
    for (int d = 0; d < ndll; ++d) {
        dll_name_off[d] = cur;
        cur += (u32)imports[d].first.size() + 1;
    }
    // hint/name 条目
    // sym_global_idx[d][k] = 全局符号索引
    std::vector<std::vector<u32>> sym_hint_off(ndll);
    std::vector<int> sym_gidx(ndll);
    int gi = 0;
    for (int d = 0; d < ndll; ++d) {
        sym_gidx[d] = gi;
        sym_hint_off[d].resize(imports[d].second.size());
        for (size_t k = 0; k < imports[d].second.size(); ++k) {
            if (cur & 1) ++cur;
            sym_hint_off[d][k] = cur;
            cur += 2 + (u32)imports[d].second[k].size() + 1;
            ++gi;
        }
    }
    u32 sym_slots = 0;
    for (int d = 0; d < ndll; ++d)
        sym_slots += (u32)(imports[d].second.size() + 1);  // 每 DLL +1 空项
    u32 off_iat = (cur + 15) & ~15u;
    u32 off_thunk = off_iat + sym_slots * 8;
    u32 idata_sz_calc = off_thunk + sym_slots * 8;

    // 每 DLL 独立 IAT/INT 段偏移(段间留空项)
    std::vector<u32> dll_iat_off(ndll), dll_thunk_off(ndll);
    {
        u32 acc = 0;
        for (int d = 0; d < ndll; ++d) {
            dll_iat_off[d] = off_iat + acc;
            dll_thunk_off[d] = off_thunk + acc;
            acc += (u32)(imports[d].second.size() + 1) * 8;
        }
    }

    // symbol name → IAT RVA
    std::unordered_map<std::string, u32> sym_to_iat;
    {
        for (int d = 0; d < ndll; ++d)
            for (size_t k = 0; k < imports[d].second.size(); ++k)
                sym_to_iat[imports[d].second[k]] = idata_rva + dll_iat_off[d] + (u32)(k * 8);
    }

    // ── 组装 .idata ──
    std::vector<u8> idata((size_t)idata_sz_calc, 0);
    // descriptors
    {
        for (int d = 0; d < ndll; ++d) {
            u32 base = (u32)(d * 20);
            u32 thunk_rva = idata_rva + dll_thunk_off[d];
            u32 ft_rva = idata_rva + dll_iat_off[d];
            patch_u32(idata, base+0, thunk_rva);
            patch_u32(idata, base+12, idata_rva + dll_name_off[d]);
            patch_u32(idata, base+16, ft_rva);
        }
        // null descriptor at base = ndll*20 (already zeroed)
    }
    // DLL names
    for (int d = 0; d < ndll; ++d)
        memcpy(idata.data()+dll_name_off[d], imports[d].first.c_str(), imports[d].first.size()+1);
    // Hint/Name + IAT + Thunk
    {
        for (int d = 0; d < ndll; ++d) {
            for (size_t k = 0; k < imports[d].second.size(); ++k) {
                u32 hn_rva = idata_rva + sym_hint_off[d][k];
                patch_u16(idata, sym_hint_off[d][k], 0);
                memcpy(idata.data()+sym_hint_off[d][k]+2, imports[d].second[k].c_str(), imports[d].second[k].size()+1);
                put_u64_at(idata, dll_iat_off[d]   + (size_t)k*8, hn_rva);
                put_u64_at(idata, dll_thunk_off[d] + (size_t)k*8, hn_rva);
            }
        }
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
    put_u16(out, (u16)ndll);       // NumberOfSections = ndll for now, fix below
    put_u32(out, 0); put_u32(out, 0); put_u32(out, 0);
    put_u16(out, 0xF0);
    put_u16(out, 0x22);
    size_t opt_start = out.size();
    put_u16(out, 0x020B);
    out.push_back(0x0E); out.push_back(0x1E);
    put_u32(out, text_raw_sz + rodata_raw_sz + data_raw_sz + idata_raw_sz);
    put_u32(out, 0); put_u32(out, 0);
    put_u32(out, entry_rva);       // entry point
    put_u32(out, text_rva);
    put_u64(out, image_base);
    put_u32(out, sect_align); put_u32(out, file_align);
    put_u16(out,6);put_u16(out,0); put_u16(out,0);put_u16(out,0); put_u16(out,6);put_u16(out,0);
    put_u32(out,0); put_u32(out,image_sz); put_u32(out,hdr_size); put_u32(out,0);
    put_u16(out,3); put_u16(out,0x0100);   // NX_COMPAT;无 .reloc,必须关 DYNAMIC_BASE/HIGH_ENTROPY_VA(ASLR 下加载器重定位会崩)
    put_u64(out,0x100000);put_u64(out,0x1000);put_u64(out,0x100000);put_u64(out,0x1000);
    put_u32(out,0); put_u32(out,16);
    put_u32(out,0);put_u32(out,0);
    put_u32(out,idata_rva);put_u32(out,idata_sz);
    for(int i=2;i<12;++i){put_u32(out,0);put_u32(out,0);}
    put_u32(out,idata_rva+off_iat);put_u32(out,sym_slots*8);
    for(int i=13;i<16;++i){put_u32(out,0);put_u32(out,0);}

    // Fix section count
    patch_u16(out, 0x80+6, (has_rodata ? 1 : 0) + (has_data ? 1 : 0) + 2);

    auto put_sect = [&](const char* name, u32 vsz, u32 va, u32 rawsz, u32 rawptr, u32 chars){
        char n[8]={0}; strncpy(n,name,8);
        for(int i=0;i<8;++i) out.push_back(n[i]);
        put_u32(out,vsz);put_u32(out,va);put_u32(out,rawsz);put_u32(out,rawptr);
        put_u32(out,0);put_u32(out,0);put_u16(out,0);put_u16(out,0);put_u32(out,chars);
    };
    put_sect(".text",  text_sz,  text_rva,  text_raw_sz,  text_raw,  0x60000020);
    if (has_rodata)
        put_sect(".rdata", rodata_sz,rodata_rva,rodata_raw_sz,rodata_raw,0x40000040);
    if (has_data)
        put_sect(".data",  data_sz,  data_rva,  data_raw_sz,  data_raw,  0xC0000040);
    put_sect(".idata", idata_sz, idata_rva, idata_raw_sz, idata_raw, 0xC0000040);

    while (out.size() < hdr_size) out.push_back(0);

    size_t text_off = out.size();
    out.insert(out.end(), text.begin(), text.end());
    while (out.size() < text_raw + text_raw_sz) out.push_back(0);

    // ── 重定位回填 ──
    for (auto& r : relocs) {
        u32 target_rva = 0;
        bool resolved = false;
        if (tx_label_rva.count(r.target)) { target_rva = tx_label_rva[r.target]; resolved=true; }
        else if (ro_label_rva.count(r.target)) { target_rva = ro_label_rva[r.target]; resolved=true; }
        else if (da_label_rva.count(r.target)) { target_rva = da_label_rva[r.target]; resolved=true; }
        else if (sym_to_iat.count(r.target)) { target_rva = sym_to_iat[r.target]; resolved=true; }
        if (!resolved) target_rva = rodata_rva;
        u32 rel_pos = text_rva + (u32)r.ref;
        int32_t disp = (int32_t)(target_rva - rel_pos);
        patch_u32(out, text_off + r.offset, (u32)disp);
    }

    if (has_rodata) {
        out.insert(out.end(), rodata.begin(), rodata.end());
        while (out.size() < rodata_raw + rodata_raw_sz) out.push_back(0);
    }
    if (has_data) {
        out.insert(out.end(), data.begin(), data.end());
        while (out.size() < data_raw + data_raw_sz) out.push_back(0);
    }
    out.insert(out.end(), idata.begin(), idata.end());
    while (out.size() < idata_raw + idata_raw_sz) out.push_back(0);
    return out;
}

} // namespace cpp2::native::pe
