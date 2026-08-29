// Minimal x86-64 assembler: only supports instruction patterns emitted by native.cpp.
// Contract: when native.cpp adds a new instruction pattern, extend here too.
// v2: proper operand tokenization ("QWORD PTR [rbp-8]" is ONE operand, not three
// tokens), hex literals, generic ModRM/SIB memory encoding, 32-bit registers.
// Unknown instructions THROW — silent drop once erased every stack-slot access
// (mov QWORD PTR [rbp-N], reg) and produced programs reading stale registers.
#include "asm64.hpp"
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cpp2::native::asm64 {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using i8 = int8_t; using i32 = int32_t; using i64 = int64_t;

namespace {

enum class RegSize { R8, R32, R64, XMM };
struct Reg { RegSize size; int id; };   // id = x86 register number (0=rax ... 15=r15)

const std::unordered_map<std::string, Reg>& reg_table() {
    static const std::unordered_map<std::string, Reg> m{
        // 64
        {"rax",{RegSize::R64,0}},{"rcx",{RegSize::R64,1}},{"rdx",{RegSize::R64,2}},
        {"rbx",{RegSize::R64,3}},{"rsp",{RegSize::R64,4}},{"rbp",{RegSize::R64,5}},
        {"rsi",{RegSize::R64,6}},{"rdi",{RegSize::R64,7}},
        {"r8",{RegSize::R64,8}},{"r9",{RegSize::R64,9}},{"r10",{RegSize::R64,10}},
        {"r11",{RegSize::R64,11}},{"r12",{RegSize::R64,12}},{"r13",{RegSize::R64,13}},
        {"r14",{RegSize::R64,14}},{"r15",{RegSize::R64,15}},
        // 32
        {"eax",{RegSize::R32,0}},{"ecx",{RegSize::R32,1}},{"edx",{RegSize::R32,2}},
        {"ebx",{RegSize::R32,3}},{"esp",{RegSize::R32,4}},{"ebp",{RegSize::R32,5}},
        {"esi",{RegSize::R32,6}},{"edi",{RegSize::R32,7}},
        {"r8d",{RegSize::R32,8}},{"r9d",{RegSize::R32,9}},{"r10d",{RegSize::R32,10}},
        {"r11d",{RegSize::R32,11}},{"r12d",{RegSize::R32,12}},{"r13d",{RegSize::R32,13}},
        {"r14d",{RegSize::R32,14}},{"r15d",{RegSize::R32,15}},
        // 8
        {"al",{RegSize::R8,0}},{"cl",{RegSize::R8,1}},{"dl",{RegSize::R8,2}},
        {"bl",{RegSize::R8,3}},
        // SSE(xmm 仅作独立操作数,不进内存寻址)
        {"xmm0",{RegSize::XMM,0}},{"xmm1",{RegSize::XMM,1}},{"xmm2",{RegSize::XMM,2}},
        {"xmm3",{RegSize::XMM,3}},{"xmm4",{RegSize::XMM,4}},{"xmm5",{RegSize::XMM,5}},
        {"xmm6",{RegSize::XMM,6}},{"xmm7",{RegSize::XMM,7}},
    };
    return m;
}

const Reg* find_reg(std::string const& s) {
    auto const& t = reg_table();
    auto it = t.find(s);
    return it == t.end() ? nullptr : &it->second;
}

// 完整消费的整数解析:十进制 / 0x 十六进制 / 负号。
// stoll 默认按十进制解析 "0x30" 得 0(在 'x' 处停),曾把 sub rsp,0x30 编成 sub rsp,0。
i64 parse_int(std::string const& s) {
    size_t pos = 0;
    i64 v;
    try {
        v = std::stoll(s, &pos, 0);          // base 0: 0x 前缀 → 十六进制
    } catch (...) {
        throw std::runtime_error("asm64: bad integer '" + s + "'");
    }
    if (pos != s.size())
        throw std::runtime_error("asm64: trailing junk in integer '" + s + "'");
    return v;
}

struct Op {
    enum Kind { NONE, REG, IMM, MEM, SYM } kind = NONE;
    Reg reg{};                      // REG
    i64 imm = 0;                    // IMM
    int size = 8;                   // MEM: 操作数字节宽(byte PTR=1, qword PTR=8)
    int base = -1, index = -1;      // MEM: 64 位寄存器编号,-1 表示无
    i64 disp = 0;                   // MEM: 位移
    std::string sym;                // SYM(含 label[rip] 形式,ripret = true)
    bool rip_rel = false;           // SYM 且形如 label[rip]
};

std::string trim(std::string const& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

Op parse_operand(std::string tok) {
    Op o;
    tok = trim(tok);
    if (tok.empty()) throw std::runtime_error("asm64: empty operand");

    // 段大小前缀(QWORD PTR / DWORD PTR / WORD PTR / BYTE PTR)
    static const std::pair<char const*, int> sizes[]{
        {"QWORD",8},{"DWORD",4},{"WORD",2},{"BYTE",1}};
    std::string up = tok;
    for (auto& c : up) c = char(toupper(c));
    for (auto const& [name, sz] : sizes) {
        std::string pfx = name;
        if (up.rfind(pfx, 0) == 0) {
            std::string rest = trim(tok.substr(pfx.size()));
            if (rest.size() >= 3 && (rest[0]=='P'||rest[0]=='p') &&
                (rest[1]=='T'||rest[1]=='t') && (rest[2]=='R'||rest[2]=='r'))
                rest = trim(rest.substr(3));
            if (rest.empty() || (rest[0] != '[' && find_reg(rest) == nullptr))
                throw std::runtime_error("asm64: dangling size prefix '" + tok + "'");
            o = parse_operand(rest);
            if (o.kind == Op::MEM) o.size = sz;
            return o;
        }
    }

    if (tok[0] == '[') {
        if (tok.back() != ']') throw std::runtime_error("asm64: unterminated memory operand '" + tok + "'");
        std::string inner = trim(tok.substr(1, tok.size() - 2));
        o.kind = Op::MEM;
        // 拆 base/index/disp:按 +/- 切(寄存器与数字交替,形如 rbp-8、rax+rcx、rax+8、rsp)
        std::string cur;
        int sign = 1;
        auto flush_term = [&]() {
            cur = trim(cur);
            if (cur.empty()) { cur = ""; return; }
            if (Reg const* r = find_reg(cur)) {
                if (r->size != RegSize::R64)
                    throw std::runtime_error("asm64: non-64-bit register in memory operand '" + cur + "'");
                if (o.base < 0) o.base = r->id;
                else if (o.index < 0) o.index = r->id;
                else throw std::runtime_error("asm64: too many registers in '" + tok + "'");
            } else {
                o.disp += sign * parse_int(cur);
            }
            cur = "";
        };
        for (size_t i = 0; i <= inner.size(); ++i) {
            char c = i < inner.size() ? inner[i] : '+';
            if (c == '+' || c == '-') { flush_term(); sign = (c == '-') ? -1 : 1; }
            else cur += c;
        }
        flush_term();
        if (o.base < 0 && o.index < 0)
            throw std::runtime_error("asm64: pure-displacement memory operand '" + tok + "' (unsupported)");
        return o;
    }

    if (tok[0] == '-' || tok[0] == '+' || isdigit((unsigned char)tok[0])) {
        o.kind = Op::IMM;
        o.imm = parse_int(tok);
        return o;
    }

    if (Reg const* r = find_reg(tok)) { o.kind = Op::REG; o.reg = *r; return o; }

    // label[rip]
    if (tok.size() > 5 && tok.compare(tok.size() - 5, 5, "[rip]") == 0) {
        o.kind = Op::SYM;
        o.sym = trim(tok.substr(0, tok.size() - 5));
        o.rip_rel = true;
        return o;
    }

    o.kind = Op::SYM;
    o.sym = tok;
    return o;
}

// 汇编器主体:单遍编码 + 末端回填
struct Asm {
    Result& res;
    std::set<std::string> const& defined;
    std::vector<Reloc>& fixes;
    size_t& pc;          // 全局程序计数器(跨行共享,标签/重定位按此回填)
    int lineno = 0;

    explicit Asm(Result& r, std::set<std::string> const& d, std::vector<Reloc>& f, size_t& pc_ref)
        : res(r), defined(d), fixes(f), pc(pc_ref) {}

    [[noreturn]] void fail(std::string const& what, std::string const& line) {
        throw std::runtime_error("asm64 line " + std::to_string(lineno)
                                 + ": " + what + " — '" + line + "'");
    }

    void emit8(u8 b) { res.text.push_back(b); ++pc; }
    void emit32(i32 v) { for (int i = 0; i < 4; ++i) emit8(u8((u32(v) >> (i * 8)) & 0xff)); }
    void emit64(u64 v) { for (int i = 0; i < 8; ++i) emit8(u8((v >> (i * 8)) & 0xff)); }

    static u8 rex(bool w, int reg, int index, int rm) {
        u8 r = u8(w ? 0x48 : 0x40);
        if (reg >= 8) r |= 0x04;
        if (index >= 8) r |= 0x02;
        if (rm >= 8) r |= 0x01;
        return r;
    }
    static u8 modrm(unsigned mod, unsigned reg, unsigned rm) {
        return u8((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    }

    // 编码 ModRM+SIB+disp(mem 操作数),regfield 为 /r 的 reg 位段。
    // 注意:REX 由调用方先发(需要知道完整 reg/index/base),此处只发 ModRM 及之后。
    void emit_mem_rm(Op const& m, unsigned regfield) {
        if (m.kind != Op::MEM) fail("internal: not a memory operand", "");
        int base = m.base, index = m.index;
        bool need_disp = m.disp != 0;
        // SIB 需要的情形:base=rsp(4) 或有 index
        bool use_sib = base == 4 || index >= 0;
        if (base < 0) fail("absolute [disp32] addressing not supported", "");
        // rbp/r13 在 mod=00 下是 [rip+disp32],必须用 mod=01 disp0
        if (base == 5 && !need_disp) need_disp = true;   // disp 0

        unsigned mod = need_disp ? (m.disp >= -128 && m.disp <= 127 ? 1u : 2u) : 0u;
        if (use_sib) {
            emit8(modrm(mod, regfield, 4));      // SIB 时 rm=100b;顺序:ModRM → SIB → disp
            u8 sib = u8(((index >= 0 ? index & 7 : 4) << 3) | (base & 7));  // scale=0
            emit8(sib);
        } else {
            emit8(modrm(mod, regfield, base & 7));
        }
        if (mod == 1) emit8(u8(i8(m.disp)));
        else if (mod == 2) emit32(i32(m.disp));
    }

    void branch(std::string const& target, bool is_call, u8 opcode, bool two_byte) {
        if (defined.count(target)) {
            if (is_call) { emit8(0xE8); }
            else if (opcode) { if (two_byte) { emit8(0x0F); } emit8(opcode); }
            else { emit8(0xE9); }
            fixes.push_back({pc, target, pc + 4, is_call});
            emit32(0);
        } else {
            // 外部:仅支持经 IAT 的间接调用/跳转
            if (!is_call && opcode) fail("external jcc not supported", target);
            emit8(0xFF); emit8(is_call ? 0x15 : 0x25);
            fixes.push_back({pc, target, pc + 4, is_call});
            emit32(0);
        }
    }

    // 通用 ALU:op∈{add,or,adc,sbb,and,sub,xor,cmp},ext 为 /digit
    void alu(std::string const& mn, Op const& dst, Op const& src, std::string const& line) {
        static const std::unordered_map<std::string, std::pair<u8, u8>> tbl{
            // {reg,rm 形式的 opcode(8 位基), /ext}
            {"add",{0x00,0}},{"or",{0x08,1}},{"adc",{0x10,2}},{"sbb",{0x18,3}},
            {"and",{0x20,4}},{"sub",{0x28,5}},{"xor",{0x30,6}},{"cmp",{0x38,7}}};
        auto it = tbl.find(mn);
        if (it == tbl.end()) fail("unknown ALU op", line);
        auto const& [base_op, ext] = it->second;

        if (dst.kind == Op::REG && src.kind == Op::REG) {
            if (dst.reg.size != src.reg.size) fail("operand size mismatch", line);
            bool w = dst.reg.size == RegSize::R64;
            emit8(rex(w, src.reg.id, -1, dst.reg.id));
            emit8(base_op + 1);                       // 32/64 位形式
            emit8(modrm(3, src.reg.id, dst.reg.id));
            return;
        }
        if (dst.kind == Op::REG && src.kind == Op::IMM) {
            bool w = dst.reg.size == RegSize::R64;
            if (dst.reg.size == RegSize::R8) fail("8-bit imm ALU not supported", line);
            emit8(rex(w, 0, -1, dst.reg.id));    // reg 位段是 /ext,恒 0;rm 扩展在 B 位
            if (src.imm >= -128 && src.imm <= 127) { emit8(0x83); emit8(modrm(3, ext, dst.reg.id)); emit8(u8(i8(src.imm))); }
            else { emit8(0x81); emit8(modrm(3, ext, dst.reg.id)); emit32(i32(src.imm)); }
            return;
        }
        if (dst.kind == Op::REG && src.kind == Op::MEM) {
            // r ← m:方向反转 opcode(+3 = 32/64 位 r,r/m 形态;+2 恒为 8 位,
            // REX.W 不会升级 8 位 opcode —— 曾用 +2 致 64 位比较变字节比较)
            bool w = dst.reg.size == RegSize::R64;
            if (src.size != (w ? 8 : 4)) fail("memory operand size mismatch", line);
            emit8(rex(w, dst.reg.id, src.index, src.base));
            emit8(base_op + 3);
            emit_mem_rm(src, dst.reg.id);
            return;
        }
        if (dst.kind == Op::MEM && src.kind == Op::REG) {
            bool w = src.reg.size == RegSize::R64;
            if (dst.size != (w ? 8 : 4)) fail("memory operand size mismatch", line);
            emit8(rex(w, src.reg.id, dst.index, dst.base));
            emit8(base_op + 1);
            emit_mem_rm(dst, src.reg.id);
            return;
        }
        if (dst.kind == Op::MEM && src.kind == Op::IMM) {
            bool w = dst.size == 8;
            emit8(rex(w, 0, dst.index, dst.base));
            if (dst.size == 1) {
                emit8(0x80); emit_mem_rm(dst, ext); emit8(u8(i8(src.imm)));
            } else if (src.imm >= -128 && src.imm <= 127) {
                emit8(0x83); emit_mem_rm(dst, ext); emit8(u8(i8(src.imm)));
            } else {
                emit8(0x81); emit_mem_rm(dst, ext); emit32(i32(src.imm));
            }
            return;
        }
        fail("unsupported ALU operand combination", line);
    }

    void mov(Op const& dst, Op const& src, std::string const& line) {
        if (dst.kind == Op::REG && src.kind == Op::REG) {
            if (dst.reg.size != src.reg.size) fail("operand size mismatch", line);
            bool w = dst.reg.size == RegSize::R64;
            if (w || dst.reg.size == RegSize::R32) {
                emit8(rex(w, src.reg.id, -1, dst.reg.id));
                emit8(0x89);
                emit8(modrm(3, src.reg.id, dst.reg.id));
            } else fail("8-bit reg-to-reg mov not supported", line);
            return;
        }
        if (dst.kind == Op::REG && src.kind == Op::IMM) {
            if (dst.reg.size == RegSize::R64) {
                if (src.imm >= -2147483648LL && src.imm <= 2147483647LL) {
                    emit8(rex(true, -1, -1, dst.reg.id));
                    emit8(0xC7); emit8(modrm(3, 0, dst.reg.id)); emit32(i32(src.imm));
                } else {
                    emit8(rex(true, -1, -1, dst.reg.id));
                    emit8(0xB8 | (dst.reg.id & 7)); emit64(u64(src.imm));
                }
            } else if (dst.reg.size == RegSize::R32) {
                if (dst.reg.id >= 8) emit8(0x41);   // REX.B
                emit8(0xB8 | (dst.reg.id & 7)); emit32(i32(src.imm));
            } else fail("8-bit mov imm not supported", line);
            return;
        }
        if (dst.kind == Op::REG && src.kind == Op::MEM) {
            bool w = dst.reg.size == RegSize::R64;
            int want = w ? 8 : 4;
            if (src.size != want) fail("memory operand size mismatch", line);
            emit8(rex(w, dst.reg.id, src.index, src.base));
            emit8(0x8B);
            emit_mem_rm(src, dst.reg.id);
            return;
        }
        if (dst.kind == Op::MEM && src.kind == Op::REG) {
            if (src.reg.size == RegSize::R8 && dst.size == 1) {
                emit8(rex(false, src.reg.id, dst.index, dst.base));
                emit8(0x88);
                emit_mem_rm(dst, src.reg.id);
                return;
            }
            bool w = src.reg.size == RegSize::R64;
            int want = w ? 8 : 4;
            if (dst.size != want) fail("memory operand size mismatch", line);
            emit8(rex(w, src.reg.id, dst.index, dst.base));
            emit8(0x89);
            emit_mem_rm(dst, src.reg.id);
            return;
        }
        if (dst.kind == Op::MEM && src.kind == Op::IMM) {
            if (dst.size == 1) {
                emit8(rex(false, 0, dst.index, dst.base));
                emit8(0xC6); emit_mem_rm(dst, 0); emit8(u8(i8(src.imm)));
                return;
            }
            bool w = dst.size == 8;
            emit8(rex(w, 0, dst.index, dst.base));
            emit8(0xC7); emit_mem_rm(dst, 0); emit32(i32(src.imm));
            return;
        }
        fail("unsupported mov operand combination", line);
    }

    void run(std::vector<std::string> const& tok, std::string const& line) {
        std::string op = tok[0];
        // 操作数:助记符后余文按逗号切
        std::vector<Op> ops;
        {
            std::string rest;
            for (size_t i = 1; i < tok.size(); ++i) {
                rest += tok[i];
                if (i + 1 < tok.size()) rest += " ";
            }
            std::vector<std::string> parts;
            std::string cur;
            for (size_t i = 0; i <= rest.size(); ++i) {
                char c = i < rest.size() ? rest[i] : ',';
                if (c == ',') { parts.push_back(trim(cur)); cur = ""; }
                else cur += c;
            }
            if (!(parts.size() == 1 && parts[0].empty()))
                for (auto& p : parts) if (!p.empty()) ops.push_back(parse_operand(p));
        }

        if (op == "ret" && ops.empty()) { emit8(0xC3); return; }
        if (op == "cqo" && ops.empty()) { emit8(0x48); emit8(0x99); return; }

        if ((op == "push" || op == "pop") && ops.size() == 1) {
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64) {
                if (ops[0].reg.id >= 8) emit8(0x41);
                emit8(u8((op == "push" ? 0x50 : 0x58) | (ops[0].reg.id & 7)));
                return;
            }
            fail("push/pop only supports 64-bit registers", line);
        }

        if (op == "mov" && ops.size() == 2) { mov(ops[0], ops[1], line); return; }
        if (op == "lea" && ops.size() == 2) {
            if (ops[0].kind != Op::REG) fail("lea destination must be register", line);
            if (ops[1].kind == Op::SYM && ops[1].rip_rel) {
                emit8(rex(true, ops[0].reg.id, -1, -1));
                emit8(0x8D); emit8(modrm(0, ops[0].reg.id, 5));
                fixes.push_back({pc, ops[1].sym, pc + 4, false});
                emit32(0);
                return;
            }
            if (ops[1].kind == Op::MEM) {
                emit8(rex(true, ops[0].reg.id, ops[1].index, ops[1].base));
                emit8(0x8D);
                emit_mem_rm(ops[1], ops[0].reg.id);
                return;
            }
            fail("unsupported lea", line);
        }

        static const char* alu_ops[] = {"add","or","adc","sbb","and","sub","xor","cmp"};
        for (auto* a : alu_ops)
            if (op == a && ops.size() == 2) { alu(op, ops[0], ops[1], line); return; }

        if (op == "test" && ops.size() == 2) {
            if (ops[0].kind == Op::REG && ops[1].kind == Op::REG &&
                ops[0].reg.id == ops[1].reg.id && ops[0].reg.size == RegSize::R64) {
                emit8(rex(true, ops[0].reg.id, -1, ops[0].reg.id));
                emit8(0x85); emit8(modrm(3, ops[0].reg.id, ops[0].reg.id));
                return;
            }
            fail("unsupported test", line);
        }

        if (op == "xchg" && ops.size() == 2 &&
            ops[0].kind == Op::REG && ops[1].kind == Op::REG &&
            ops[0].reg.size == RegSize::R64 && ops[1].reg.size == RegSize::R64) {
            emit8(rex(true, ops[1].reg.id, -1, ops[0].reg.id));
            emit8(0x87); emit8(modrm(3, ops[1].reg.id, ops[0].reg.id));
            return;
        }

        if (op == "imul" && ops.size() == 2 &&
            ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64 &&
            ops[1].kind == Op::REG && ops[1].reg.size == RegSize::R64) {
            emit8(rex(true, ops[0].reg.id, -1, ops[1].reg.id));
            emit8(0x0F); emit8(0xAF); emit8(modrm(3, ops[0].reg.id, ops[1].reg.id));
            return;
        }

        if (op == "idiv" && ops.size() == 1 && ops[0].kind == Op::REG &&
            ops[0].reg.size == RegSize::R64) {
            emit8(rex(true, -1, -1, ops[0].reg.id));
            emit8(0xF7); emit8(modrm(3, 7, ops[0].reg.id));
            return;
        }
        if (op == "neg" && ops.size() == 1 && ops[0].kind == Op::REG &&
            ops[0].reg.size == RegSize::R64) {
            emit8(rex(true, -1, -1, ops[0].reg.id));
            emit8(0xF7); emit8(modrm(3, 3, ops[0].reg.id));
            return;
        }

        if ((op == "inc" || op == "dec") && ops.size() == 1) {
            u8 base = op == "inc" ? 0x00 : 0x08;
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64) {
                emit8(rex(true, -1, -1, ops[0].reg.id));
                emit8(0xFF); emit8(modrm(3, base == 0 ? 0 : 1, ops[0].reg.id));
                return;
            }
            if (ops[0].kind == Op::MEM && ops[0].size == 8) {
                emit8(rex(true, -1, ops[0].index, ops[0].base));
                emit8(0xFF); emit_mem_rm(ops[0], base == 0 ? 0 : 1);
                return;
            }
            fail("unsupported inc/dec", line);
        }

        if ((op == "shl" || op == "sar" || op == "shr") && ops.size() == 2) {
            unsigned ext = op == "shl" ? 4 : op == "sar" ? 7 : 5;
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64) {
                if (ops[1].kind == Op::REG && ops[1].reg.size == RegSize::R8 && ops[1].reg.id == 1) {
                    emit8(rex(true, -1, -1, ops[0].reg.id));
                    emit8(0xD3); emit8(modrm(3, ext, ops[0].reg.id));
                    return;
                }
                if (ops[1].kind == Op::IMM) {
                    emit8(rex(true, -1, -1, ops[0].reg.id));
                    emit8(0xC1); emit8(modrm(3, ext, ops[0].reg.id)); emit8(u8(i8(ops[1].imm)));
                    return;
                }
            }
            fail("unsupported shift", line);
        }

        if (op.size() > 3 && op.compare(0, 3, "set") == 0 && ops.size() == 1) {
            static const std::unordered_map<std::string, u8> scc{
                {"sete",0x94},{"setne",0x95},{"setl",0x9C},{"setg",0x9F},
                {"setle",0x9E},{"setge",0x9D},{"setb",0x92},{"seta",0x97}};
            auto it = scc.find(op);
            if (it != scc.end() && ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R8) {
                emit8(0x0F); emit8(it->second); emit8(modrm(3, 0, ops[0].reg.id));
                return;
            }
        }

        if (op == "movzx" && ops.size() == 2) {
            bool dst64 = ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64;
            bool dst32 = ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R32;
            if ((dst64 || dst32) &&
                ops[1].kind == Op::REG && ops[1].reg.size == RegSize::R8) {
                emit8(rex(dst64, ops[0].reg.id, -1, ops[1].reg.id));
                emit8(0x0F); emit8(0xB6); emit8(modrm(3, ops[0].reg.id, ops[1].reg.id));
                return;
            }
            if ((dst64 || dst32) &&
                ops[1].kind == Op::MEM && ops[1].size == 1) {
                emit8(rex(dst64, ops[0].reg.id, ops[1].index, ops[1].base));
                emit8(0x0F); emit8(0xB6); emit_mem_rm(ops[1], ops[0].reg.id);
                return;
            }
            fail("unsupported movzx", line);
        }
        if (op == "movsx" && ops.size() == 2) {
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64 &&
                ops[1].kind == Op::MEM && ops[1].size == 1) {
                emit8(rex(true, ops[0].reg.id, ops[1].index, ops[1].base));
                emit8(0x0F); emit8(0xBE); emit_mem_rm(ops[1], ops[0].reg.id);
                return;
            }
            fail("unsupported movsx", line);
        }

        if (op == "call" && ops.size() == 1) {
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64) {
                emit8(rex(false, -1, -1, ops[0].reg.id));
                emit8(0xFF); emit8(modrm(3, 2, ops[0].reg.id));
                return;
            }
            if (ops[0].kind == Op::SYM && !ops[0].rip_rel) {
                branch(ops[0].sym, /*is_call=*/true, 0, false);
                return;
            }
            fail("unsupported call", line);
        }
        if (op == "jmp" && ops.size() == 1) {
            if (ops[0].kind == Op::REG && ops[0].reg.size == RegSize::R64) {
                emit8(0xFF); emit8(modrm(3, 4, ops[0].reg.id));
                return;
            }
            if (ops[0].kind == Op::SYM && !ops[0].rip_rel) {
                branch(ops[0].sym, /*is_call=*/false, 0, false);
                return;
            }
            fail("unsupported jmp", line);
        }
        static const std::unordered_map<std::string, u8> jcc{
            {"je",0x84},{"jne",0x85},{"jz",0x84},{"jnz",0x85},{"jl",0x8C},{"jg",0x8F},{"jle",0x8E},{"jge",0x8D},
            {"ja",0x87},{"jae",0x83},{"jb",0x82},{"jbe",0x86},{"js",0x88},{"jns",0x89}};
        if (jcc.count(op) && ops.size() == 1 && ops[0].kind == Op::SYM) {
            branch(ops[0].sym, /*is_call=*/false, jcc.at(op), /*two_byte=*/true);
            return;
        }

        // ── SSE2 double 子集(native.cpp 的 double 路径专用)──────────
        // movq GPR↔XMM(双向:第 1/2 操作数任一为 XMM)。
        // 编码上 XMM 恒占 modrm.reg,GPR 恒占 modrm.rm:
        //   66 0F 6E /r = movq xmm, r/m64   66 0F 7E /r = movq r/m64, xmm
        if (op == "movq" && ops.size() == 2 && ops[0].kind == Op::REG &&
            ops[1].kind == Op::REG &&
            ((ops[0].reg.size == RegSize::XMM) != (ops[1].reg.size == RegSize::XMM))) {
            bool to_xmm = ops[0].reg.size == RegSize::XMM;
            int xmm_id = to_xmm ? ops[0].reg.id : ops[1].reg.id;   // → reg 字段
            int gpr_id = to_xmm ? ops[1].reg.id : ops[0].reg.id;   // → rm 字段
            emit8(0x66);
            emit8(rex(true, xmm_id, -1, gpr_id));
            emit8(0x0F); emit8(to_xmm ? 0x6E : 0x7E);
            emit8(modrm(3, xmm_id, gpr_id));
            return;
        }
        if (ops.size() == 2 && ops[0].kind == Op::REG && ops[0].reg.size == RegSize::XMM) {
            // 单目 xmm 源:movsd/cvtsi2sd/sqrtsd/comisd/ucomisd/addsd/subsd/mulsd/divsd
            static const std::unordered_map<std::string, std::tuple<u8, u8, u8>> sse2{
                // 前缀, OPCODE, 允许 mem 源
                {"movsd",   {0xF2, 0x10, 1}},
                {"addsd",   {0xF2, 0x58, 1}},
                {"subsd",   {0xF2, 0x5C, 1}},
                {"mulsd",   {0xF2, 0x59, 1}},
                {"divsd",   {0xF2, 0x5E, 1}},
                {"sqrtsd",  {0xF2, 0x51, 1}},
                {"comisd",  {0x66, 0x2F, 1}},
                {"ucomisd", {0x66, 0x2E, 1}},
            };
            if (op == "cvtsi2sd") {
                if (ops[1].kind == Op::REG && ops[1].reg.size == RegSize::R64) {
                    emit8(0xF2);
                    emit8(rex(true, ops[0].reg.id, -1, ops[1].reg.id));
                    emit8(0x0F); emit8(0x2A);
                    emit8(modrm(3, ops[0].reg.id, ops[1].reg.id));
                    return;
                }
                if (ops[1].kind == Op::MEM && ops[1].size == 8) {
                    emit8(0xF2);
                    emit8(rex(true, ops[0].reg.id, ops[1].index, ops[1].base));
                    emit8(0x0F); emit8(0x2A);
                    emit_mem_rm(ops[1], ops[0].reg.id);
                    return;
                }
                fail("unsupported cvtsi2sd", line);
            }
            if (op == "cvtsd2si") {
                if (ops[1].kind == Op::REG && ops[1].reg.size == RegSize::XMM && ops[0].reg.size == RegSize::R64) {
                    emit8(0xF2);
                    emit8(rex(true, ops[0].reg.id, -1, ops[1].reg.id));
                    emit8(0x0F); emit8(0x2C);
                    emit8(modrm(3, ops[0].reg.id, ops[1].reg.id));
                    return;
                }
                fail("unsupported cvtsd2si", line);
            }
            auto sit = sse2.find(op);
            if (sit != sse2.end()) {
                auto const& [pfx, opc, allow_mem] = sit->second;
                auto& src2 = ops[1];
                if (src2.kind == Op::REG && src2.reg.size == RegSize::XMM) {
                    emit8(pfx);
                    emit8(rex(false, ops[0].reg.id, -1, src2.reg.id));
                    emit8(0x0F); emit8(opc);
                    emit8(modrm(3, ops[0].reg.id, src2.reg.id));
                    return;
                }
                if (allow_mem && src2.kind == Op::MEM && src2.size == 8) {
                    emit8(pfx);
                    emit8(rex(false, ops[0].reg.id, src2.index, src2.base));
                    emit8(0x0F); emit8(opc);
                    emit_mem_rm(src2, ops[0].reg.id);
                    return;
                }
                fail("unsupported " + op + " source", line);
            }
        }
        if (op == "movsd" && ops.size() == 2 && ops[0].kind == Op::MEM && ops[0].size == 8 &&
            ops[1].kind == Op::REG && ops[1].reg.size == RegSize::XMM) {
            emit8(0xF2);
            emit8(rex(false, ops[1].reg.id, ops[0].index, ops[0].base));
            emit8(0x0F); emit8(0x11);
            emit_mem_rm(ops[0], ops[1].reg.id);
            return;
        }
        fail("unknown instruction", line);
    }
};

} // namespace

Result assemble(const std::string& source) {
    Result res;
    std::vector<Reloc> fixes;
    std::set<std::string> rodata_labels_set;
    std::set<std::string> data_labels_set;
    bool in_text = true;
    bool in_data = false;
    std::string pending_data_lbl;
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

    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t b = line.find_last_not_of(" \t\r");
        line = line.substr(a, b-a+1);
        if (line.empty()) continue;

        // Skip directives / track section
        if (line == ".text") { in_text = true; in_data = false; continue; }
        if (line == ".intel_syntax noprefix") continue;
        if (line == ".section .rodata") { in_text = false; in_data = false; continue; }
        if (line == ".data") { in_text = false; in_data = true; pending_data_lbl.clear(); continue; }
        if (line == ".globl" || line.rfind(".globl ",0)==0) continue;
        // .data 段:label 后跟 .quad N → 全局初始 qword
        if (in_data && line.rfind(".quad",0)==0) {
            if (!pending_data_lbl.empty())
                res.data[pending_data_lbl] = (uint64_t)std::stoll(line.substr(5), nullptr, 0);
            continue;
        }
        if (line.rfind(".section",0)==0 || line.rfind(".quad",0)==0) continue;
        if (line.find(".string") != std::string::npos) continue;

        // Label definition "xxx:" alone on line
        if (line.back() == ':' && line.find(' ') == std::string::npos) {
            std::string lbl = line.substr(0, line.size()-1);
            if (in_text) res.symbols[lbl] = pc;
            else if (in_data) { data_labels_set.insert(lbl); pending_data_lbl = lbl; }
            else rodata_labels_set.insert(lbl);
            continue;
        }

        // Handle "label: instruction" on same line
        size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 1) {
            std::string maybe_lbl = line.substr(0, colon);
            size_t la = maybe_lbl.find_first_not_of(" \t");
            if (la != std::string::npos) {
                maybe_lbl = maybe_lbl.substr(la);
                if (maybe_lbl.find(' ') == std::string::npos &&
                    maybe_lbl.find('(') == std::string::npos &&
                    maybe_lbl[0] != '#') {
                    std::string rest = line.substr(colon+1);
                    size_t ra = rest.find_first_not_of(" \t");
                    if (ra == std::string::npos) {
                        if (in_text) res.symbols[maybe_lbl] = pc;
                        else if (in_data) { data_labels_set.insert(maybe_lbl); pending_data_lbl = maybe_lbl; }
                        else rodata_labels_set.insert(maybe_lbl);
                        continue;
                    }
                    if (in_text) res.symbols[maybe_lbl] = pc;
                    else if (in_data) { data_labels_set.insert(maybe_lbl); pending_data_lbl = maybe_lbl; }
                    else rodata_labels_set.insert(maybe_lbl);
                    line = rest.substr(ra);
                }
            }
        }

        // Tokenize(助记符 + 余文;操作数级切分交给 Asm::run)
        std::vector<std::string> tok;
        {
            std::istringstream ss(line);
            std::string w;
            while (ss >> w) tok.push_back(w);
        }
        if (tok.empty()) continue;

        Asm{res, defined_labels, fixes, pc}.run(tok, line);
    }

    // Resolve label references
    for (auto& f : fixes) {
        auto it = res.symbols.find(f.target);
        if (it != res.symbols.end()) {
            i32 rel = (i32)((i64)it->second - (i64)f.ref);
            for(int i=0;i<4;++i)
                res.text[f.offset+i] = uint8_t(((u32)rel>>(i*8))&0xff);
            continue;
        }
        if (rodata_labels_set.count(f.target)) {
            res.relocs.push_back({f.offset, f.target, f.ref, false});
            continue;
        }
        if (data_labels_set.count(f.target)) {
            res.relocs.push_back({f.offset, f.target, f.ref, false});
            continue;
        }
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
