// Minimal x86-64 assembler: only supports instruction patterns emitted by native.cpp.
// Contract: when native.cpp adds a new instruction pattern, extend here too.
#include "asm64.hpp"
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cpp2::native::asm64 {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t; using i32 = int32_t; using i64 = int64_t;

static int reg64(const std::string& r) {
    static const std::unordered_map<std::string,int> m{
        {"rax",0},{"rcx",1},{"rdx",2},{"rbx",3},{"rsp",4},{"rbp",5},{"rsi",6},{"rdi",7},
        {"r8",8},{"r9",9},{"r10",10},{"r11",11},{"r12",12},{"r13",13},{"r14",14},{"r15",15}};
    auto it = m.find(r);
    return it != m.end() ? it->second : -1;
}

static bool is_r64(const std::string& t) { return reg64(t) >= 0; }

static u8 rex(int reg, int rm) {
    u8 r = 0x48;
    if (reg >= 8) r |= 0x04;
    if (rm >= 8) r |= 0x01;
    return r;
}

Result assemble(const std::string& source) {
    Result res;
    std::vector<Reloc> fixes;
    std::set<std::string> rodata_labels_set;
    bool in_text = true;
    std::set<std::string> defined_labels;   // 预扫描:所有 label 定义(含 .L/rodata)
    {
        std::istringstream pre(source);
        std::string pl;
        while (std::getline(pre, pl)) {
            size_t pa = pl.find_first_not_of(" \t\r");
            if (pa == std::string::npos) continue;
            pl = pl.substr(pa);
            if (pl == ".text" || pl == ".intel_syntax noprefix") continue;
            if (pl.rfind(".section",0)==0 || pl.rfind(".globl",0)==0 ||
                pl.rfind(".data",0)==0 || pl.rfind(".quad",0)==0) continue;
            if (pl.find(".string") != std::string::npos) continue;
            if (pl.back() == ':' && pl.find(' ') == std::string::npos) {
                defined_labels.insert(pl.substr(0, pl.size()-1));
                continue;
            }
            size_t pcolon = pl.find(':');
            if (pcolon != std::string::npos && pcolon > 1) {
                std::string ml = pl.substr(0, pcolon);
                size_t mla = ml.find_first_not_of(" \t");
                if (mla != std::string::npos) {
                    ml = ml.substr(mla);
                    if (ml.find(' ')==std::string::npos && ml.find('(')==std::string::npos && ml[0]!='#') {
                        std::string rest = pl.substr(pcolon+1);
                        if (rest.find_first_not_of(" \t") == std::string::npos)
                            defined_labels.insert(ml);
                    }
                }
            }
        }
    }
    std::istringstream in(source);
    std::string line;
    size_t pc = 0;

    auto emit8 = [&](u8 b){ res.text.push_back(b); ++pc; };
    auto emit32 = [&](i32 v){ for(int i=0;i<4;++i) emit8(uint8_t((v>>(i*8))&0xff)); };

    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t b = line.find_last_not_of(" \t\r");
        line = line.substr(a, b-a+1);
        if (line.empty()) continue;

        // Skip directives / track section
        if (line == ".text") { in_text = true; continue; }
        if (line == ".intel_syntax noprefix") continue;
        if (line == ".section .rodata") { in_text = false; continue; }
        if (line.rfind(".section",0)==0 || line.rfind(".globl",0)==0 ||
            line.rfind(".data",0)==0 || line.rfind(".quad",0)==0) continue;
        if (line.find(".string") != std::string::npos) continue;

        // Label definition "xxx:" alone on line
        if (line.back() == ':' && line.find(' ') == std::string::npos) {
            std::string lbl = line.substr(0, line.size()-1);
            if (in_text) res.symbols[lbl] = pc;
            else rodata_labels_set.insert(lbl);
            continue;
        }

        // Handle "label: instruction" on same line
        size_t colon = line.find(':');
        bool had_label = false;
        if (colon != std::string::npos && colon > 1) {
            // Check if part before colon is a valid label
            std::string maybe_lbl = line.substr(0, colon);
            size_t la = maybe_lbl.find_first_not_of(" \t");
            if (la != std::string::npos) {
                maybe_lbl = maybe_lbl.substr(la);
                // Check it looks like a label (no spaces, no parens)
                if (maybe_lbl.find(' ') == std::string::npos &&
                    maybe_lbl.find('(') == std::string::npos &&
                    maybe_lbl[0] != '#') {
                    // Check rest after colon has content
                    std::string rest = line.substr(colon+1);
                    size_t ra = rest.find_first_not_of(" \t");
                    if (ra == std::string::npos) {
                        // Pure label
                        if (in_text) res.symbols[maybe_lbl] = pc;
                        else rodata_labels_set.insert(maybe_lbl);
                        continue;
                    }
                    // Label + instruction
                    if (in_text) res.symbols[maybe_lbl] = pc;
                    else rodata_labels_set.insert(maybe_lbl);
                    line = rest.substr(ra);
                    had_label = true;
                }
            }
        }
        (void)had_label;

        // Tokenize
        std::vector<std::string> tok;
        {
            std::istringstream ss(line);
            std::string w;
            while (ss >> w) tok.push_back(w);
        }
        if (tok.empty()) continue;
        std::string op = tok[0];

        auto strip_comma = [](std::string s2) {
            if (!s2.empty() && s2.back()==',') s2.pop_back();
            return s2;
        };

        // ret / cqo / nop
        if (op == "ret") { emit8(0xC3); continue; }
        if (op == "cqo") { emit8(0x48); emit8(0x99); continue; }

        // push/pop reg
        if ((op=="push"||op=="pop") && tok.size()>=2) {
            std::string r = strip_comma(tok[1]);
            int rr = reg64(r);
            if (rr >= 0) {
                if (rr >= 8) emit8(0x41);
                emit8((op=="push"?0x50:0x58) | (rr&7));
                continue;
            }
        }

        // mov variants
        if (op=="mov" && tok.size()>=3) {
            std::string dst = strip_comma(tok[1]);
            std::string src = tok[2];
            int dd = reg64(dst), ss2 = reg64(src);

            // mov rbp,rsp / mov rsp,rbp
            if (dst=="rbp"&&src=="rsp"){emit8(0x48);emit8(0x89);emit8(0xE5);continue;}
            if (dst=="rsp"&&src=="rbp"){emit8(0x48);emit8(0x89);emit8(0xEC);continue;}

            // mov reg,reg
            if (dd>=0 && ss2>=0) {
                emit8(rex(dd,ss2)); emit8(0x89); emit8(0xC0|((ss2&7)<<3)|(dd&7));
                continue;
            }

            // mov rax/rdi/rsi..., imm32 (B8+r for short forms; also negative)
            if (dd>=0 && dd<=7 && (src[0]=='-' || (src[0]>='0' && src[0]<='9'))) {
                i32 imm = (i32)std::stoll(src);
                if (dd==0) emit8(0xB8);
                else if (dd==1) emit8(0xB9);
                else if (dd==2) emit8(0xBA);
                else if (dd==6) emit8(0xBE);
                else if (dd==7) emit8(0xBF);
                else { emit8(0x48); emit8(0xC7); emit8(0xC0|dd); }
                for(int i=0;i<4;++i) emit8(uint8_t(((u32)imm>>(i*8))&0xff));
                continue;
            }

            // mov QWORD PTR [rbp-N], reg|imm
            if (dst.rfind("QWORD")==0) {
                auto pos = dst.find("[rbp-");
                if (pos != std::string::npos) {
                    int disp = -std::stoi(dst.substr(pos+5));
                    int rr = reg64(src);
                    if (rr >= 0) {
                        emit8(rex(rr,5)); emit8(0x89);
                        emit8(0x40|((rr&7)<<3)|0x05);
                        emit8((uint8_t)disp);
                    } else {
                        // imm32: 48 C7 45 disp imm32
                        i32 imm = (i32)std::stoll(src);
                        emit8(0x48); emit8(0xC7);
                        emit8(0x45);
                        emit8((uint8_t)disp);
                        for(int i=0;i<4;++i) emit8(uint8_t(((u32)imm>>(i*8))&0xff));
                    }
                    continue;
                }
            }

            // mov reg, QWORD PTR [rbp-N]
            if (dd>=0 && src.rfind("QWORD")==0) {
                auto pos = src.find("[rbp-");
                if (pos != std::string::npos) {
                    int disp = -std::stoi(src.substr(pos+5));
                    emit8(rex(dd,5)); emit8(0x8B);
                    emit8(0x40|((dd&7)<<3)|0x05);
                    emit8((uint8_t)disp);
                    continue;
                }
            }

            // mov edi/ecx/edx, eax (32-bit)
            if ((dd==7||dd==1||dd==2) && ss2==0) {
                emit8(0x89); emit8(0xC0|(0<<3)|dd);
                continue;
            }
            // mov esi, eax
            if (dd==6 && ss2==0) { emit8(0x89); emit8(0xC0|(0<<3)|6); continue; }

            // mov esi/ecx/edx, imm
            if ((dd==6||dd==2) && (src[0]=='-' || (src[0]>='0' && src[0]<='9'))) {
                i32 imm = (i32)std::stoi(src);
                if (dd==6) emit8(0xBE);
                else if (dd==2) emit8(0xBA);
                else if (dd==7) emit8(0xBF);
                for(int i=0;i<4;++i) emit8(uint8_t(((u32)imm>>(i*8))&0xff));
                continue;
            }
            continue;
        }

        // lea reg, label[rip]
        if (op=="lea" && tok.size()>=3) {
            std::string dst = strip_comma(tok[1]);
            std::string src = tok[2];
            int dd = reg64(dst);
            if (dd>=0 && src.find("[rip]")!=std::string::npos) {
                std::string tgt = src.substr(0, src.find("[rip]"));
                emit8(rex(dd,0)); emit8(0x8D);
                emit8(0x05|((dd&7)<<3));
                fixes.push_back({pc, tgt, pc+4, false});
                emit32(0);
                continue;
            }
            // lea reg, [rbp-N]
            if (dd>=0) {
                auto pos = src.find("[rbp-");
                if (pos != std::string::npos) {
                    int disp = -std::stoi(src.substr(pos+5));
                    emit8(rex(dd,5)); emit8(0x8D);
                    emit8(0x45|((dd&7)<<3));
                    emit8((uint8_t)disp);
                    continue;
                }
            }
        }

        // add/sub/and/or/xor/cmp reg, imm8|imm32
        if ((op=="add"||op=="sub"||op=="and"||op=="or"||op=="xor"||op=="cmp") && tok.size()>=3) {
            std::string dst = strip_comma(tok[1]);
            std::string src = tok[2];
            int dd = reg64(dst);
            if (dd>=0 && (src[0]=='-' || (src[0]>='0' && src[0]<='9'))) {
                i64 v = std::stoll(src);
                u8 ext = (op=="add")?0:(op=="or")?1:(op=="and")?4:(op=="sub")?5:
                          (op=="xor")?6:(op=="cmp")?7:0;
                if (v >= -128 && v <= 127) {
                    emit8(rex(dd,0)); emit8(0x83);
                    emit8(0xC0|((ext&7)<<3)|(dd&7));
                    emit8((uint8_t)(i32)v);
                } else {
                    emit8(rex(dd,0)); emit8(0x81);
                    emit8(0xC0|((ext&7)<<3)|(dd&7));
                    i32 imm=(i32)v; for(int i=0;i<4;++i) emit8(uint8_t(((u32)imm>>(i*8))&0xff));
                }
                continue;
            }
        }

        // call label
        if (op=="call" && tok.size()>=2) {
            std::string tgt = tok[1];
            if (is_r64(tgt)) {
                emit8(0xFF); emit8(0xD0|(reg64(tgt)&7));
                continue;
            }
            if (defined_labels.count(tgt)) {
                // 内部调用:直接 E8 rel32
                emit8(0xE8);
                fixes.push_back({pc, tgt, pc+4, true});
                emit32(0);
            } else {
                // 外部符号:FF 15 disp32 (间接调用,经 IAT)
                emit8(0xFF); emit8(0x15);
                fixes.push_back({pc, tgt, pc+4, true});
                emit32(0);
            }
            continue;
        }

        // jmp label
        if (op=="jmp" && tok.size()>=2) {
            if (is_r64(tok[1])) {
                emit8(0xFF); emit8(0xE0|(reg64(tok[1])&7));
                continue;
            }
            if (defined_labels.count(tok[1])) {
                emit8(0xE9);
                fixes.push_back({pc, tok[1], pc+4, false});
                emit32(0);
            } else {
                emit8(0xFF); emit8(0x25);
                fixes.push_back({pc, tok[1], pc+4, false});
                emit32(0);
            }
            continue;
        }

        // jcc label
        if ((op=="je"||op=="jne"||op=="jl"||op=="jg"||op=="jle"||op=="jge") && tok.size()>=2) {
            static const std::unordered_map<std::string,u8> cc{
                {"je",0x84},{"jne",0x85},{"jl",0x8C},{"jg",0x8F},{"jle",0x8E},{"jge",0x8D}};
            auto it2 = cc.find(op);
            if (it2 != cc.end()) {
                emit8(0x0F); emit8(it2->second);
                fixes.push_back({pc, tok[1], pc+4, false});
                emit32(0);
                continue;
            }
        }

        // Binary ops with two register operands
        if (tok.size()>=3) {
            std::string dst = strip_comma(tok[1]);
            std::string src = tok[2];
            int dd=reg64(dst), ss2=reg64(src);
            if (dd>=0 && ss2>=0) {
                u8 opc = 0; bool found = false;
                if(op=="add"){opc=0x01;found=true;}
                else if(op=="or"){opc=0x09;found=true;}
                else if(op=="and"){opc=0x21;found=true;}
                else if(op=="sub"){opc=0x29;found=true;}
                else if(op=="xor"){opc=0x31;found=true;}
                else if(op=="cmp"){opc=0x39;found=true;}
                if(found){emit8(rex(dd,ss2));emit8(opc);emit8(0xC0|((ss2&7)<<3)|(dd&7));continue;}
                if(op=="imul"){emit8(rex(dd,ss2));emit8(0x0F);emit8(0xAF);emit8(0xC0|((dd&7)<<3)|(ss2&7));continue;}
                if(op=="xchg"){emit8(rex(dd,ss2));emit8(0x87);emit8(0xC0|((dd&7)<<3)|(ss2&7));continue;}
            }
        }

        // neg reg
        if (op=="neg" && tok.size()>=2) {
            int dd = reg64(strip_comma(tok[1]));
            if (dd>=0) { emit8(rex(dd,0)); emit8(0xF7); emit8(0xD8|(dd&7)); continue; }
        }

        // inc QWORD PTR [rbp-N]
        if (op=="inc" && tok.size()>=2) {
            std::string t = tok[1];
            auto pos = t.find("[rbp-");
            if (pos != std::string::npos) {
                int disp = -std::stoi(t.substr(pos+5));
                emit8(0x48); emit8(0xFF); emit8(0x45); emit8((uint8_t)disp);
                continue;
            }
        }

        // idiv reg
        if (op=="idiv" && tok.size()>=2) {
            int dd = reg64(strip_comma(tok[1]));
            if (dd>=0) { emit8(rex(dd,0)); emit8(0xF7); emit8(0xF8|(dd&7)); continue; }
        }

        // test rax, rax / test rcx, rcx
        if (op=="test" && tok.size()>=3) {
            std::string d=strip_comma(tok[1]);
            int dd=reg64(d), ss2=reg64(tok[2]);
            if (dd==ss2 && dd>=0) {
                emit8(rex(dd,0)); emit8(0x85); emit8(0xC0|((dd&7)<<3)|(dd&7));
                continue;
            }
        }

        // setCC al
        if (op.length()>3 && op.substr(0,3)=="set" && tok.size()>=2) {
            static const std::unordered_map<std::string,u8> scc{
                {"sete",0x94},{"setne",0x95},{"setl",0x9C},
                {"setg",0x9F},{"setle",0x9E},{"setge",0x9D}};
            auto it2 = scc.find(op);
            if (it2 != scc.end()) { emit8(0x0F); emit8(it2->second); emit8(0xC0); continue; }
        }

        // movzx rax, al
        if (op=="movzx" && tok.size()>=3) {
            emit8(0x0F); emit8(0xB6); emit8(0xC0);
            continue;
        }

        // cmp byte PTR [rax], 0
        if (op=="cmp" && tok.size()>=4) {
            std::string full="";
            for(size_t i=1;i<tok.size();++i) full+=strip_comma(tok[i]);
            if (full.rfind("bytePTR[rax],0",0)==0){emit8(0x80);emit8(0x38);emit8(0);continue;}
            if (full.rfind("bytePTR[rcx],0",0)==0){emit8(0x80);emit8(0x39);emit8(0);continue;}
        }

        // movsx rax, byte PTR [rax+rcx]
        if (op=="movsx" && tok.size()>=4) {
            std::string full="";
            for(size_t i=1;i<tok.size();++i) full+=strip_comma(tok[i]);
            if (full.rfind("bytePTR[rax+rcx]",0)==0) {
                emit8(0x48); emit8(0x0F); emit8(0xBE); emit8(0x04); emit8(0x08);
                continue;
            }
        }

        // Unknown instruction - skip silently for now
    }

    // Resolve label references
    for (auto& f : fixes) {
        auto it = res.symbols.find(f.target);
        if (it != res.symbols.end()) {
            // 内部标签:直接回填 disp32
            i32 rel = (i32)((i64)it->second - (i64)f.ref);
            for(int i=0;i<4;++i)
                res.text[f.offset+i] = uint8_t(((u32)rel>>(i*8))&0xff);
            continue;
        }
        if (rodata_labels_set.count(f.target)) {
            // rodata 标签:发 RIP 相对重定位,交给 PE 层填 rodata RVA
            res.relocs.push_back({f.offset, f.target, f.ref, false});
            continue;
        }
        // 外部符号(printf/sys_exit/...):发重定位,目标 = 导入表 IAT
        bool have = false;
        for (auto& e : res.externs) if (e == f.target) { have = true; break; }
        if (!have) res.externs.push_back(f.target);
        res.relocs.push_back({f.offset, f.target, f.ref, f.is_call});
    }

    // Extract rodata strings (second pass over source)
    {
        std::istringstream in2(source);
        std::string ln;
        std::string cur_lbl;
        bool in_rodata = false;
        while (std::getline(in2, ln)) {
            size_t a2 = ln.find_first_not_of(" \t\r");
            if (a2 == std::string::npos) continue;
            ln = ln.substr(a2);
            if (ln == ".section .rodata") { in_rodata = true; cur_lbl=""; continue; }
            if (ln == ".text") { in_rodata = false; cur_lbl=""; continue; }
            if (!in_rodata) continue;
            if (ln.back()==':' && ln.find(' ')==std::string::npos) {
                cur_lbl = ln.substr(0,ln.size()-1);
                continue;
            }
            if (!cur_lbl.empty() && ln.rfind(".string ",0)==0) {
                std::string str = ln.substr(8);
                if (str.size()>=2 && str.front()=='"' && str.back()=='"')
                    str = str.substr(1,str.size()-2);
                std::vector<u8> bytes;
                for (size_t i=0;i<str.size();++i) {
                    if (str[i]=='\\' && i+1<str.size()) {
                        char c = str[++i];
                        if(c=='n')bytes.push_back('\n');
                        else if(c=='t')bytes.push_back('\t');
                        else if(c=='\\')bytes.push_back('\\');
                        else if(c=='"')bytes.push_back('"');
                        else if(c=='0')bytes.push_back('\0');
                        else bytes.push_back(c);
                    } else bytes.push_back(str[i]);
                }
                bytes.push_back(0);
                res.rodata[cur_lbl] = bytes;
                cur_lbl = "";
            }
        }
    }

    return res;
}

} // namespace cpp2::native::asm64
