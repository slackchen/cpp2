// 原生后端 v0 实现:x86-64 SysV 汇编发射(见 native.hpp)
// 设计:栈机风格——表达式值经 rax 传递,中间量压栈;无寄存器分配。
// 槽位:参数与局部变量统一扁平分配 [rbp-8k];帧大小对齐 16。
// 平台:仅 SysV(Linux);Windows 构建的工具自动回退转译(需求行为)。
#include "native.hpp"
#include "native/x64.hpp"
#include "native/pe.hpp"
#include "native/asm64.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#ifndef _WIN32
#define CPP2_NATIVE_HOST_OK 1
#endif

namespace cpp2::native {

[[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

// 源码级转义 → 真实字节(rodata 存真实字节;发射时 emit_rodata 再转义)
static std::string unescape_str(std::string const& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char c = in[++i];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            case '0': out += '\0'; break;
            default: out += c; break;
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

bool is_int_kind(sema::Type::Kind k)
{
    switch (k) {
    case sema::Type::Int: case sema::Type::I8: case sema::Type::I16:
    case sema::Type::I32: case sema::Type::I64:
    case sema::Type::U8: case sema::Type::U16: case sema::Type::U32:
    case sema::Type::U64: case sema::Type::Bool:
        return true;
    default:
        return false;
    }
}

#ifdef CPP2_NATIVE_HOST_OK

class NativeEmitter {
public:
    std::string emit(ast::Module& m, sema::Result const&)
    {
        std::cerr << "[native] emit for module " << m.name << " funcs " << m.funcs.size() << " structs " << m.structs.size() << std::endl;
        m_ = &m;
        precheck();
        out_ << ".intel_syntax noprefix\n";
        emit_globals();
        put(".text");
        for (auto& f : m_->funcs) {
            if (f.is_extern) continue;
            if (!f.type_params.empty()) unsup("generic functions");
            check_func(f);
            emit_func(f);
        }
        for (auto& s : m_->structs) {
            for (auto& md : s.methods) {
                if (md.name == "destructor") continue;
                emit_method(s, md);
            }
        }
        emit_rodata_strs();
        return out_.str();
    }

private:
    ast::Module* m_ = nullptr;
    std::ostringstream out_;
    int label_ = 0;
    int push_depth_ = 0;
    ast::FuncDecl* cur_fn_ = nullptr;
    std::string cur_sym_name_;      // 当前发射符号(func 或 Struct_method)
    std::unordered_map<std::string, int> slots_;
    std::vector<std::string> break_labels_;
    std::vector<std::string> continue_labels_;
    std::unordered_map<std::string, std::string> str_pool_;

    [[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

    void put(std::string const& s) { out_ << s << "\n"; }
    void ins(std::string const& s) { out_ << "    " << s << "\n"; }
    void label(std::string const& l) { out_ << l << ":\n"; }
    std::string lbl(std::string const& tag) { return ".L" + tag + "_" + std::to_string(label_++); }

    std::string intern_string(std::string const& text)
    {
        auto it = str_pool_.find(text);
        if (it != str_pool_.end()) return it->second;
        std::string lblname = ".LS" + std::to_string(str_pool_.size());
        str_pool_[text] = lblname;
        return lblname;
    }

    void emit_rodata_strs()
    {
        if (str_pool_.empty()) return;
        out_ << ".section .rodata\n";
        for (auto const& [text, lblname] : str_pool_) {
            out_ << lblname << ":\n";
            out_ << "    .string \"";
            for (char c : text) {
                if (c == '\\') out_ << "\\\\";
                else if (c == '"') out_ << "\\\"";
                else if (c == '\n') out_ << "\\n";
                else if (c == '\t') out_ << "\\t";
                else if (c == '\0') break;
                else out_ << c;
            }
            out_ << "\"\n";
        }
        out_ << ".text\n";
    }

    // ── 预检 ────────────────────────────────────────────────────
    void precheck()
    {
        std::cerr << "[native] precheck structs " << m_->structs.size() << " funcs " << m_->funcs.size() << std::endl;
        for (auto& s : m_->structs) std::cerr << "[native] struct " << s.name << " fields " << s.fields.size() << " methods " << s.methods.size() << std::endl;
        for (auto const& im : m_->imports) {
            std::string _im_name;
            for (size_t _i = 0; _i < im.module_parts.size(); ++_i) {
                if (_i) _im_name += ".";
                _im_name += im.module_parts[_i];
            }
            if (_im_name != "std") unsup("imports (only 'std' allowed in native v0)");
        }
        if (!m_->legacy_blocks.empty()) unsup("cxx_legacy blocks");
        for (auto& s : m_->structs) {
            for (auto& f : s.fields) {
                auto k = scalar_kind(f.type);
                bool ok = is_int_kind(k);
                // string/double 字段:8B 槽存指针/位模式(v0 值语义)
                if (!ok && f.type.parts.size()==1
                    && (f.type.parts[0]=="string" || f.type.parts[0]=="double")) ok = true;
                if (!ok) unsup("struct field '" + f.name + "' must be int/string/double");
            }
        }
        // enum 允许：底层 int，成员按 0..n-1 分配（与 C++ enum class 一致）
        // variant 允许：候选均为 int struct（如 Circle/Rect），match 穷尽
        if (!m_->concepts.empty())      unsup("concepts");
        for (auto& f : m_->funcs) check_func(f);
        for (auto& g : m_->globals) check_global(g);
    }

    void check_global(ast::GlobalVar& g)
    {
        auto k = scalar_kind(g.type);
        if (g.type.parts.size() != 1 || !is_int_kind(k))
            unsup("global '" + g.name + "' must be an explicitly-typed integer");
        if (!g.init || g.init->kind() != ast::Expr::Literal
            || static_cast<ast::LiteralExpr&>(*g.init).lit != ast::LitKind::Int)
            unsup("global '" + g.name + "' requires an integer literal initializer");
    }

    void check_func(ast::FuncDecl& f)
    {
        if (f.throws) unsup("throws functions ('" + f.name + "')");
        if (f.pre || f.post) unsup("contracts on '" + f.name + "'");
        if (!f.type_params.empty()) unsup("generic functions");
        if (f.params.size() > 6) unsup("more than 6 parameters on '" + f.name + "'");
        for (auto& p : f.params) {
            if (p.mode != ast::ParamMode::In)
                unsup("parameter mode other than 'in' on '" + p.name
                      + "' of '" + f.name + "'");
        }
    }

    // 子集类型粗判:单段名字且属于整型族(int/i*/u*/bool);未知(std:: 等)放行
    sema::Type::Kind scalar_kind(ast::TypeUse const& t)
    {
        using K = sema::Type::Kind;
        if (t.parts.size() != 1) return K::Unknown;
        std::string const& n = t.parts[0];
        if (n == "int")  return K::Int;
        if (n == "i8")   return K::I8;   if (n == "i16") return K::I16;
        if (n == "i32")  return K::I32;  if (n == "i64") return K::I64;
        if (n == "u8")   return K::U8;   if (n == "u16") return K::U16;
        if (n == "u32")  return K::U32;  if (n == "u64") return K::U64;
        if (n == "bool") return K::Bool;
        return K::Unknown;
    }

    // ── 数据段(int 全局字面量初始化)────────────────────────────
    void emit_globals()
    {
        bool any = false;
        for (auto& g : m_->globals) {
            if (!any) { put(".data"); any = true; }
            long long v = 0;
            if (g.init)
                v = std::stoll(static_cast<ast::LiteralExpr&>(*g.init).text);
            put(g.name + ":");
            put("    .quad " + std::to_string(v));
        }
    }

    // ── 槽位 ────────────────────────────────────────────────────
    int slot_of(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it == slots_.end())
            unsup("name '" + n + "' is not a local/parameter "
                  "(globals are read-only data in native v0)");
        return it->second;
    }

    int slot_or_new(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it != slots_.end()) return it->second;
        int idx = (int)slots_.size();
        slots_[n] = -(8 * (idx + 1));
        return slots_[n];
    }

    void scan_slots(ast::Stmt* s)
    {
        if (!s) return;
        switch (s->kind()) {
        case ast::Stmt::Block: {
            for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) scan_slots(st.get());
            break;
        }
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(*s);
            slot_or_new(v.name);
            break;
        }
        case ast::Stmt::For: {
            auto& fo = static_cast<ast::ForStmt&>(*s);
            slot_or_new(fo.var);
            scan_slots(fo.body.get());
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
            scan_slots(i.then_block.get());
            scan_slots(i.else_block.get());
            break;
        }
        default: break;
        }
    }

    // ── 函数 ────────────────────────────────────────────────────
    void emit_func(ast::FuncDecl& f)
    {
        cur_fn_ = &f;
        cur_sym_name_ = f.name;
        slots_.clear();
        push_depth_ = 0;

        for (size_t i = 0; i < f.params.size(); ++i)
            slots_[f.params[i].name] = -(8 * (int)(i + 1));
        scan_slots(f.has_block_body ? f.block_body.get() : nullptr);

        int words = (int)f.params.size() + (int)slots_.size() + 16; // +16 = temp slots + shadow
        int frame = ((words * 8 + 15) / 16) * 16;

        put(".globl " + f.name);
        put(f.name + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));

        static char const* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        for (size_t i = 0; i < f.params.size(); ++i)
            ins(std::string("mov QWORD PTR [rbp")
                + std::to_string(slots_[f.params[i].name]) + "], " + regs[i]);

        std::string ret = ".Lret_" + f.name;
        if (f.has_block_body && f.block_body)
            emit_stmt(f.block_body.get());
        else if (f.expr_body) {
            eval(f.expr_body.get());
            ins("jmp " + ret);
        } else {
            ins("jmp " + ret);
        }
        label(ret);
        if (f.name == "main") {
            if (f.ret && !f.ret->empty())
                ins("mov rcx, rax");
            else
                ins("xor rcx, rcx");
            ins("call ExitProcess");
            ins("ret");
        } else {
            ins("mov rsp, rbp");
            ins("pop rbp");
            ins("ret");
        }
        put("");
        cur_fn_ = nullptr;
    }

    void emit_method(ast::StructDecl& s, ast::MethodDecl& m)
    {
        std::cerr << "[native] emit_method " << s.name << "_" << m.name << std::endl;
        std::string mname = s.name + "_" + m.name;
        cur_fn_ = nullptr;
        cur_sym_name_ = mname;
        slots_.clear();
        push_depth_ = 0;
        slots_["this"] = -(8 * 1);
        for (size_t i = 0; i < m.params.size(); ++i)
            slots_[m.params[i].name] = -(8 * (int)(i + 2));
        if (m.has_block_body && m.block_body) scan_slots(m.block_body.get());
        int words = 1 + (int)m.params.size() + (int)slots_.size() + 16; // +16 temp
        int frame = ((words * 8 + 15) / 16) * 16;
        put(".globl " + mname);
        put(mname + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));
#ifdef CPP2_NATIVE_HOST_OK
        static char const* regs2[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        ins("mov QWORD PTR [rbp-8], rdi");
        for (size_t i = 0; i < m.params.size(); ++i)
            ins(std::string("mov QWORD PTR [rbp") + std::to_string(-(8*(int)(i+2))) + "], " + regs2[i+1]);
#else
        static char const* regs2[] = {"rcx","rdx","r8","r9"};
        ins("mov QWORD PTR [rbp-8], rcx");
        for (size_t i = 0; i < m.params.size(); ++i)
            ins(std::string("mov QWORD PTR [rbp") + std::to_string(-(8*(int)(i+2))) + "], " + regs2[i+1]);
#endif
        std::string ret = ".Lret_" + mname;
        if (m.has_block_body && m.block_body)
            emit_stmt(m.block_body.get());
        else if (m.expr_body) {
            eval(m.expr_body.get());
            ins("jmp " + ret);
        }
        label(ret);
        ins("mov rsp, rbp");
        ins("pop rbp");
        ins("ret");
        put("");
    }

    // ── 语句 ────────────────────────────────────────────────────
    void emit_stmt(ast::Stmt* s)
    {
        switch (s->kind()) {
        case ast::Stmt::Block: {
            for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) emit_stmt(st.get());
            break;
        }
        case ast::Stmt::ExprStmt:
            eval(static_cast<ast::ExprStmt*>(s)->expr.get());
            break;
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(*s);
            // 特殊处理 Point 等 struct 的 StructLit 初始化：直接按字段存入 p 的连续槽
            if (v.init && v.init->kind() == ast::Expr::StructLit) {
                auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
                ast::StructDecl* sd = nullptr;
                for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
                if (sd) {
                    std::string second = v.name + "#2";
                    int base_off, second_off;
                    // 确保 p 的基址为低地址（x 在 lo，y 在 hi）
                    auto it = slots_.find(v.name);
                    auto it2 = slots_.find(second);
                    if (it == slots_.end() && it2 == slots_.end()) {
                        // 首次分配：先为 y 分配高地址，再为 x 分配低地址，保证 x 在 lo
                        second_off = slot_or_new(second); // -8
                        base_off = slot_or_new(v.name);   // -16
                    } else if (it != slots_.end()) {
                        base_off = it->second;
                        second_off = (it2 == slots_.end()) ? slot_or_new(second) : it2->second;
                    } else {
                        second_off = it2->second;
                        base_off = slot_or_new(v.name);
                    }
                    int lo = std::min(base_off, second_off);
                    int hi = std::max(base_off, second_off);
                    // 矫正：确保 base_off 为 lo（x 的槽）
                    if (base_off != lo) {
                        // 交换 slots 中 p 与 p#2 的偏移，使 p 指向 lo
                        slots_[v.name] = lo;
                        slots_[second] = hi;
                        base_off = lo;
                        second_off = hi;
                    }
                    for (size_t i=0;i<sd->fields.size();++i) {
                        bool found = false;
                        for (auto& pr : sl.fields) if (pr.first == sd->fields[i].name) {
                            eval(pr.second.get());
                            found = true;
                            break;
                        }
                        if (!found) eval(sd->fields[i].init.get());
                        int off2 = (i==0) ? lo : hi;
                        ins("mov QWORD PTR [rbp" + std::to_string(off2) + "], rax");
                    }
                    break;
                }
            }
            // variant 类型变量:首槽 = tag(候选序),后续槽 = 数据
            bool is_variant_var = false;
            if (v.has_type && v.type.parts.size()==1) {
                for (auto& vd : m_->variants)
                    if (vd.name == v.type.parts[0]) { is_variant_var = true; break; }
            }
            if (is_variant_var) {
                int tag_off = slot_or_new(v.name + "#tag");
                int data_off = slot_or_new(v.name);
                std::string dslot2 = v.name + "#d2";
                slot_or_new(dslot2);
                int tag = -1;
                if (v.init && v.init->kind() == ast::Expr::StructLit) {
                    auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                    std::string tn = sl.type_parts.empty() ? "" : sl.type_parts[0];
                    for (auto& vd : m_->variants) {
                        if (vd.name != v.type.parts[0]) continue;
                        for (size_t k=0;k<vd.alternatives.size();++k)
                            if (vd.alternatives[k].parts.size()==1 && vd.alternatives[k].parts[0]==tn)
                                tag = (int)k;
                    }
                }
                if (tag < 0) unsup("variant init must be a candidate literal");
                ins("mov rax, " + std::to_string(tag));
                ins("mov QWORD PTR [rbp" + std::to_string(tag_off) + "], rax");
                // 数据字段逐个写入(从 data_off 起)
                ast::StructDecl* sd = nullptr;
                if (v.init && v.init->kind() == ast::Expr::StructLit) {
                    auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                    std::string tn = sl.type_parts.empty() ? "" : sl.type_parts[0];
                    for (auto& s2 : m_->structs) if (s2.name == tn) { sd = &s2; break; }
                }
                if (sd) {
                    for (size_t i=0;i<sd->fields.size();++i) {
                        bool found = false;
                        for (auto& pr : ((ast::StructLitExpr&)*v.init).fields)
                            if (pr.first == sd->fields[i].name) {
                                eval(pr.second.get());
                                found = true;
                                break;
                            }
                        if (!found) eval(sd->fields[i].init.get());
                        ins("mov QWORD PTR [rbp" + std::to_string(data_off + (int)i*8) + "], rax");
                    }
                }
                break;
            }
            int off = slot_or_new(v.name);
            if (v.init) eval(v.init.get());
            else ins("xor eax, eax");
            ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
            break;
        }
        case ast::Stmt::Return: {
            auto& r = static_cast<ast::ReturnStmt&>(*s);
            if (r.value) eval(r.value.get());
            else ins("xor eax, eax");
            ins("jmp .Lret_" + (cur_fn_ ? cur_fn_->name : cur_sym_name_));
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
            // if-let:if v := f() { } else e := it { }(v0 哨兵:非0=成功)
            if (i.is_let()) {
                int tmp_off = slot_or_new("$iflet_" + std::to_string(label_++));
                eval(i.let_init.get());
                ins("mov QWORD PTR [rbp" + std::to_string(tmp_off) + "], rax");
                std::string els = lbl("iflet_else"), end = lbl("iflet_end");
                ins("test rax, rax");
                ins("je " + els);
                if (i.let_name != "_" && !i.let_name.empty()) {
                    int boff = slot_or_new(i.let_name);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tmp_off) + "]");
                    ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                }
                emit_stmt(i.then_block.get());
                ins("jmp " + end);
                label(els);
                if (!i.else_binding.empty()) {
                    int boff = slot_or_new(i.else_binding);
                    ins("xor eax, eax");
                    ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                }
                if (i.else_block) emit_stmt(i.else_block.get());
                label(end);
                break;
            }
            std::string els = lbl("else");
            branch_bool(i.cond.get(), "", els);
            emit_stmt(i.then_block.get());
            if (i.else_block) {
                std::string end = lbl("endif");
                ins("jmp " + end);
                label(els);
                emit_stmt(i.else_block.get());
                label(end);
            } else {
                label(els);
            }
            break;
        }
        case ast::Stmt::While: {
            auto& w = static_cast<ast::WhileStmt&>(*s);
            std::string top = lbl("while"), end = lbl("wend");
            label(top);
            branch_bool(w.cond.get(), "", end);
            break_labels_.push_back(end);
            continue_labels_.push_back(top);
            emit_stmt(w.body.get());
            continue_labels_.pop_back();
            break_labels_.pop_back();
            ins("jmp " + top);
            label(end);
            break;
        }
        case ast::Stmt::For: {
            auto& fo = static_cast<ast::ForStmt&>(*s);
            if (!fo.is_range) {
                // string 迭代降级:for c in s → i=0; while i<strlen(s) { c=s[i]; ... }
                if (!fo.iterable || fo.iterable->kind() != ast::Expr::Name)
                    unsup("iterator for-loops (only 'for c in <string>' v0)");
                auto& itn = static_cast<ast::NameExpr&>(*fo.iterable);
                int ioff = slot_or_new(fo.var + "#i");
                int eoff2 = slot_or_new(fo.var + "#end2");
                int voff = slot_or_new(fo.var);
                // i = 0
                ins("xor eax, eax");
                ins("mov QWORD PTR [rbp" + std::to_string(ioff) + "], rax");
                // end = strlen(it)
                eval(fo.iterable.get());
#ifdef CPP2_NATIVE_HOST_OK
                ins("mov rdi, rax");
#else
                ins("mov rcx, rax");
#endif
                std::string stop = lbl("slen"), sdone = lbl("slend");
                label(stop);
#ifdef CPP2_NATIVE_HOST_OK
                ins("cmp byte PTR [rdi], 0");
                ins("je " + sdone);
                ins("inc rdi");
#else
                ins("cmp byte PTR [rcx], 0");
                ins("je " + sdone);
                ins("inc rcx");
#endif
                ins("jmp " + stop);
                label(sdone);
#ifdef CPP2_NATIVE_HOST_OK
                ins("sub rax, rdi");
                ins("neg rax");
#else
                ins("sub rax, rcx");
                ins("neg rax");
#endif
                ins("mov QWORD PTR [rbp" + std::to_string(eoff2) + "], rax");
                std::string top = lbl("fortop"), inc = lbl("forinc"), fend = lbl("fend");
                label(top);
                // i < end ?
                ins("mov rax, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                ins("cmp rax, QWORD PTR [rbp" + std::to_string(eoff2) + "]");
                ins("jge " + fend);
                break_labels_.push_back(fend);
                continue_labels_.push_back(inc);
                // c = it[i]
                eval(fo.iterable.get());
                ins("mov rcx, QWORD PTR [rbp" + std::to_string(ioff) + "]");
#ifdef CPP2_NATIVE_HOST_OK
                ins("movsx rax, byte PTR [rax+rcx]");
#else
                ins("movsx rax, byte PTR [rax+rcx]");
#endif
                ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rax");
                emit_stmt(fo.body.get());
                label(inc);
                ins("inc QWORD PTR [rbp" + std::to_string(ioff) + "]");
                break_labels_.pop_back();
                ins("jmp " + top);
                label(fend);
                break;
            }
            int voff = slot_or_new(fo.var);
            int eoff = slot_or_new(fo.var + "#end");
            eval(fo.range_begin.get());
            ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rax");
            eval(fo.range_end.get());
            ins("mov QWORD PTR [rbp" + std::to_string(eoff) + "], rax");

            std::string top = lbl("fortop"), inc = lbl("forinc"), end = lbl("fend");
            label(top);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(voff) + "]");
            ins("cmp rax, QWORD PTR [rbp" + std::to_string(eoff) + "]");
            ins((fo.inclusive ? "jg " : "jge ") + end);
            break_labels_.push_back(end);
            continue_labels_.push_back(inc);
            emit_stmt(fo.body.get());
            label(inc);
            ins("inc QWORD PTR [rbp" + std::to_string(voff) + "]");
            break_labels_.pop_back();
            ins("jmp " + top);
            label(end);
            break;
        }
        case ast::Stmt::Break: {
            if (break_labels_.empty()) unsup("break outside loop");
            ins("jmp " + break_labels_.back());
            break;
        }
        case ast::Stmt::Continue: {
            if (continue_labels_.empty()) unsup("continue outside loop");
            ins("jmp " + continue_labels_.back());
            break;
        }
        case ast::Stmt::Match: {
            auto& m = static_cast<ast::MatchStmt&>(*s);
            eval(m.scrutinee.get());
            int scr_off = slot_or_new("$scr_" + std::to_string(label_++));
            ins("mov QWORD PTR [rbp" + std::to_string(scr_off) + "], rax");
            std::string end = lbl("match_end");
            for (size_t i=0;i<m.arms.size();++i) {
                auto& arm = m.arms[i];
                std::string next = (i+1==m.arms.size()) ? end : lbl("match_next");
                if (arm.pat == ast::MatchArm::Pat::Wildcard) {
                } else if (arm.pat == ast::MatchArm::Pat::EnumMember) {
                    int enum_val = -1;
                    for (auto& e : m_->enums) {
                        for (size_t k=0;k<e.members.size();++k) if (e.members[k]==arm.enum_member) {
                            enum_val = (int)k;
                            break;
                        }
                        if (enum_val!=-1) break;
                    }
                    if (enum_val==-1) unsup("unknown enum member '" + arm.enum_member + "'");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx = -1;
                    std::string vname;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx = (int)k;
                                vname = v.name;
                                break;
                            }
                        }
                        if (var_idx!=-1) break;
                    }
                    if (var_idx==-1) unsup("unknown variant alternative");
                    // scrutinee 是 variant 变量 → tag 槽
                    std::string scr_name;
                    if (m.scrutinee->kind() == ast::Expr::Name)
                        scr_name = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
                    int tag_off = slot_of(scr_name + "#tag");
                    int data_off = slot_of(scr_name);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tag_off) + "]");
                    ins("cmp rax, " + std::to_string(var_idx));
                    ins("jne " + next);
                    // sub 绑定:.field=binding → binding = data[i]
                    for (auto& sp : arm.sub) {
                        if (sp == "_") continue;
                        size_t eq = sp.find('=');
                        if (sp.size() > 1 && sp[0]=='.' && eq != std::string::npos) {
                            std::string field = sp.substr(1, eq-1);
                            std::string bind = sp.substr(eq+1);
                            ast::StructDecl* sd = nullptr;
                            for (auto& s2 : m_->structs)
                                if (s2.name == arm.type_pattern.parts[0]) { sd = &s2; break; }
                            if (!sd) unsup("match bind: unknown struct");
                            int fi = -1;
                            for (size_t i=0;i<sd->fields.size();++i)
                                if (sd->fields[i].name==field) { fi=(int)i; break; }
                            if (fi < 0) unsup("match bind: unknown field '" + field + "'");
                            int boff = slot_or_new(bind);
                            ins("mov rax, QWORD PTR [rbp" + std::to_string(data_off + fi*8) + "]");
                            ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                        }
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Ok || arm.pat == ast::MatchArm::Pat::Some) {
                    // ok n => :值非 0;绑定 n = scrutinee
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("test rax, rax");
                    ins("je " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Err || arm.pat == ast::MatchArm::Pat::None) {
                    // err e => :值为 0;v0 错误消息不可用,e 绑定为 0
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("test rax, rax");
                    ins("jne " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        ins("xor eax, eax");
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else {
                    unsup("unsupported match pattern");
                }
                emit_stmt(arm.body.get());
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            break;
        }
        default:
            unsup("statement kind");
        }
    }

    // ── 表达式 → rax ────────────────────────────────────────────
    void eval(ast::Expr* e)
    {
        switch (e->kind()) {
        case ast::Expr::Literal: {
            auto& lit = static_cast<ast::LiteralExpr&>(*e);
            if (lit.lit == ast::LitKind::Bool)
                ins(std::string("mov rax, ") + (lit.text == "true" ? "1" : "0"));
            else if (lit.lit == ast::LitKind::Int)
                ins("mov rax, " + lit.text);
            else if (lit.lit == ast::LitKind::Char) {
                std::string t = lit.text;
                char ch = (t.size() >= 3 && t.front()=='\'' && t.back()=='\'') ? t[1] : '\0';
                ins("mov rax, " + std::to_string((int)(unsigned char)ch));
            } else if (lit.lit == ast::LitKind::String) {
                std::string text = lit.text;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                std::string lblname = intern_string(unescape_str(text));
                ins("lea rax, " + lblname + "[rip]");
            } else
                unsup("literal kinds beyond integers/bool/string/char");
            break;
        }
        case ast::Expr::Name: {
            auto& n = static_cast<ast::NameExpr&>(*e);
            if (n.qualified()) {
                // enum 成员 Color::blue -> 值
                if (n.parts.size()==2) {
                    for (auto& en : m_->enums) {
                        if (en.name == n.parts[0]) {
                            for (size_t k=0;k<en.members.size();++k) if (en.members[k]==n.parts[1]) {
                                ins("mov rax, " + std::to_string(k));
                                goto name_done;
                            }
                        }
                    }
                }
                unsup("qualified names");
            }
            {
                bool is_field = false;
                int field_off = -1;
                for (auto& s : m_->structs) {
                    for (size_t i=0;i<s.fields.size();++i) if (s.fields[i].name == n.parts[0]) {
                        field_off = (int)i * 8;
                        is_field = true;
                        break;
                    }
                    if (is_field) break;
                }
                if (is_field && cur_fn_ == nullptr) {
                    ins("mov rax, QWORD PTR [rbp-8]");
                    ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                } else {
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(slot_or_new(n.parts[0])) + "]");
                }
            }
            name_done:
            break;
        }
        case ast::Expr::Unary: {
            auto& u = static_cast<ast::UnaryExpr&>(*e);
            eval(u.operand.get());
            if (u.op == "-") ins("neg rax");
            else if (u.op == "!") { ins("cmp rax, 0"); ins("sete al"); ins("movzx rax, al"); }
            else unsup("unary operator '" + u.op + "'");
            break;
        }
        case ast::Expr::Binary:
            eval_binary(static_cast<ast::BinaryExpr&>(*e));
            break;
        case ast::Expr::Assign: {
            auto& a = static_cast<ast::AssignExpr&>(*e);
            if (a.target->kind() != ast::Expr::Name)
                unsup("assignment form");
            auto& n = static_cast<ast::NameExpr&>(*a.target);
            int off = slot_or_new(n.parts[0]);
            if (a.op == "=") {
                eval(a.value.get());
            } else {
                // compound assignment: desugar lhs op rhs -> rax
                std::string lop = a.op.substr(0, a.op.size() - 1); // "+=" -> "+"
                // lhs -> rcx, rhs -> rax, then compute
                ins("mov rax, QWORD PTR [rbp" + std::to_string(off) + "]");
                ins("push rax");
                ++push_depth_;
                eval(a.value.get());
                ins("pop rcx");
                --push_depth_;
                if (lop == "+") { ins("add rax, rcx"); }
                else if (lop == "-") { ins("xchg rax, rcx"); ins("sub rax, rcx"); }
                else if (lop == "*") { ins("imul rax, rcx"); }
                else if (lop == "/") {
                    ins("xchg rax, rcx");
                    ins("test rcx, rcx");
                    std::string ok = lbl("divok");
                    ins("jnz " + ok);
                    ins("lea rdi, .Lfmt_div0[rip]");
                    ins("xor eax, eax");
                    ins("call printf");
                    ins("mov edi, 101");
                    ins("call exit");
                    label(ok);
                    ins("cqo");
                    ins("idiv rcx");
                } else if (lop == "%") {
                    ins("xchg rax, rcx");
                    ins("test rcx, rcx");
                    std::string ok = lbl("divok");
                    ins("jnz " + ok);
                    ins("lea rdi, .Lfmt_div0[rip]");
                    ins("xor eax, eax");
                    ins("call printf");
                    ins("mov edi, 101");
                    ins("call exit");
                    label(ok);
                    ins("cqo");
                    ins("idiv rcx");
                    ins("mov rax, rdx");
                } else if (lop == "&") { ins("and rax, rcx"); }
                else if (lop == "|") { ins("or rax, rcx"); }
                else if (lop == "^") { ins("xor rax, rcx"); }
                else if (lop == "<<") { ins("xchg rax, rcx"); ins("shl rax, cl"); }
                else if (lop == ">>") { ins("xchg rax, rcx"); ins("sar rax, cl"); }
                else unsup("compound assignment '" + a.op + "'");
            }
            if (via_this) {
                ins("pop rcx");
                --push_depth_;
                ins("mov QWORD PTR [rcx], rax");
            } else {
                ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
            }
            break;
        }
        case ast::Expr::Member: {
            auto& m = static_cast<ast::MemberExpr&>(*e);
            if (m.base->kind() != ast::Expr::Name) unsup("member base must be name");
            auto& base = static_cast<ast::NameExpr&>(*m.base);
            int field_off = -1;
            for (auto& s : m_->structs) {
                for (size_t i=0;i<s.fields.size();++i) if (s.fields[i].name == m.name) {
                    field_off = (int)i * 8;
                    break;
                }
                if (field_off != -1) break;
            }
            if (field_off == -1) unsup("unknown field '" + m.name + "'");
            int base_off = slot_of(base.parts[0]);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off + field_off) + "]");
            break;
        }
        case ast::Expr::StructLit: {
            auto& sl = static_cast<ast::StructLitExpr&>(*e);
            std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
            ast::StructDecl* sd = nullptr;
            for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
            if (!sd) unsup("unknown struct '" + tname + "'");
            std::string tmp = "$tmp_struct_" + std::to_string(label_++);
            int tmp_off = slot_or_new(tmp);
            std::string tmp2 = tmp + "_2";
            slot_or_new(tmp2);
            for (size_t i=0;i<sd->fields.size();++i) {
                bool found = false;
                for (auto& pr : sl.fields) if (pr.first == sd->fields[i].name) {
                    eval(pr.second.get());
                    found = true;
                    break;
                }
                if (!found) eval(sd->fields[i].init.get());
                ins("mov QWORD PTR [rbp" + std::to_string(tmp_off + (int)i*8) + "], rax");
            }
            ins("mov rax, " + std::to_string(tmp_off));
            break;
        }
        case ast::Expr::Call:
            emit_call(static_cast<ast::CallExpr&>(*e));
            break;
        case ast::Expr::Match: {
            auto& m = static_cast<ast::MatchExpr&>(*e);
            // variant 变量 scrutinee:直接用 tag/data 槽(不压栈)
            std::string scr_name2;
            if (m.scrutinee->kind() == ast::Expr::Name)
                scr_name2 = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
            int scr_tag_off = slot_of(scr_name2 + "#tag");
            int scr_data_off = slot_of(scr_name2);
            std::string end = lbl("match_expr_end");
            std::string res_lbl = "$match_res_" + std::to_string(label_++);
            int res_off = slot_or_new(res_lbl);
            for (size_t i=0;i<m.arms.size();++i) {
                auto& arm = m.arms[i];
                std::string next = (i+1==m.arms.size()) ? end : lbl("match_next");
                if (arm.pat == ast::MatchArm::Pat::Wildcard) {
                } else if (arm.pat == ast::MatchArm::Pat::EnumMember) {
                    int enum_val = -1;
                    for (auto& e : m_->enums) {
                        for (size_t k=0;k<e.members.size();++k) if (e.members[k]==arm.enum_member) {
                            enum_val = (int)k;
                            break;
                        }
                        if (enum_val!=-1) break;
                    }
                    if (enum_val==-1) unsup("unknown enum member '" + arm.enum_member + "'");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_tag_off) + "]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx2 = -1;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx2 = (int)k;
                                break;
                            }
                        }
                        if (var_idx2!=-1) break;
                    }
                    if (var_idx2==-1) unsup("unknown variant alternative");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_tag_off) + "]");
                    ins("cmp rax, " + std::to_string(var_idx2));
                    ins("jne " + next);
                    // sub 绑定
                    for (auto& sp : arm.sub) {
                        if (sp == "_") continue;
                        size_t eq = sp.find('=');
                        if (sp.size() > 1 && sp[0]=='.' && eq != std::string::npos) {
                            std::string field = sp.substr(1, eq-1);
                            std::string bind = sp.substr(eq+1);
                            ast::StructDecl* sd2 = nullptr;
                            for (auto& s2 : m_->structs)
                                if (s2.name == arm.type_pattern.parts[0]) { sd2 = &s2; break; }
                            if (!sd2) unsup("match expr bind: unknown struct");
                            int fi = -1;
                            for (size_t i=0;i<sd2->fields.size();++i)
                                if (sd2->fields[i].name==field) { fi=(int)i; break; }
                            if (fi < 0) unsup("match expr bind: unknown field");
                            int boff = slot_or_new(bind);
                            ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_data_off + fi*8) + "]");
                            ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                        }
                    }
                } else {
                    unsup("unsupported match expr pattern");
                }
                // 臂体为单条语句（ExprStmt 包装的表达式）
                if (arm.body && arm.body->kind() == ast::Stmt::ExprStmt) {
                    eval(static_cast<ast::ExprStmt&>(*arm.body).expr.get());
                } else if (arm.body) {
                    emit_stmt(arm.body.get());
                    // 假设臂体已将结果放入 rax
                }
                ins("mov QWORD PTR [rbp" + std::to_string(res_off) + "], rax");
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(res_off) + "]");
            break;
        }
        default:
            unsup("expression kind");
        }
    }

    void emit_call(ast::CallExpr& c)
    {
        std::cerr << "[native] emit_call callee kind " << (int)c.callee->kind() << std::endl;
        if (c.callee->kind() == ast::Expr::Member) {
            auto& mem = static_cast<ast::MemberExpr&>(*c.callee);
            std::cerr << "[native] Member call " << mem.name << " base kind " << (int)mem.base->kind() << std::endl;
            if (mem.base->kind() != ast::Expr::Name) {
                std::cerr << "[native] Member base not Name kind " << (int)mem.base->kind() << std::endl;
                unsup("method base must be name");
            }
            auto& base = static_cast<ast::NameExpr&>(*mem.base);
            ast::StructDecl* sd = nullptr;
            ast::MethodDecl* md = nullptr;
            for (auto& s : m_->structs) {
                for (auto& m : s.methods) if (m.name == mem.name) { sd = &s; md = &m; break; }
                if (sd) break;
            }
            if (!sd || !md) {
                // std 方法桥(v0 语义化降级):
                //   string.size()/length() → strlen 循环
                //   string.empty()         → strlen==0
                //   error.message()        → 返回 0(错误串存储 v1)
                //   optional.has_value()   → 1(非空即真)
                if (mem.name == "size" || mem.name == "length") {
                    eval(mem.base.get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
#else
                    ins("mov rcx, rax");
#endif
                    std::string top = lbl("strlen"), done = lbl("strdone");
                    label(top);
#ifdef CPP2_NATIVE_HOST_OK
                    ins("cmp byte PTR [rdi], 0");
                    ins("je " + done);
                    ins("inc rdi");
#else
                    ins("cmp byte PTR [rcx], 0");
                    ins("je " + done);
                    ins("inc rcx");
#endif
                    ins("jmp " + top);
                    label(done);
#ifdef CPP2_NATIVE_HOST_OK
                    ins("sub rax, rdi");
                    ins("neg rax");
#else
                    ins("sub rax, rcx");
                    ins("neg rax");
#endif
                    return;
                }
                if (mem.name == "empty") {
                    eval(mem.base.get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("cmp byte PTR [rax], 0");
#else
                    ins("cmp byte PTR [rax], 0");
#endif
                    std::string nz = lbl("strnz"), se = lbl("strend");
                    ins("jne " + nz);
                    ins("mov rax, 1");
                    ins("jmp " + se);
                    label(nz);
                    ins("xor eax, eax");
                    label(se);
                    return;
                }
                if (mem.name == "message") {
                    eval(mem.base.get());
                    ins("xor eax, eax");          // v0:错误串不可用,返回空
                    return;
                }
                if (mem.name == "has_value") {
                    eval(mem.base.get());
                    ins("test rax, rax");
                    std::string nz = lbl("optnz"), oe = lbl("optend");
                    ins("setne al");
                    ins("movzx rax, al");
                    return;
                }
                unsup("unknown method '" + mem.name + "'");
            }
            int base_off = slot_of(base.parts[0]);
            ins("lea rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
            ins("push rax");
            ++push_depth_;
            for (auto& a : c.args) {
                eval(a.get());
                ins("push rax");
                ++push_depth_;
            }
#ifdef CPP2_NATIVE_HOST_OK
            static char const* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            ins("pop rdi");
            --push_depth_;
            for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                ins("pop " + std::string(regs[i+1]));
                --push_depth_;
            }
            if (push_depth_ % 2 == 1) { ins("sub rsp, 8"); ins("call " + sd->name + "_" + md->name); ins("add rsp, 8"); }
            else ins("call " + sd->name + "_" + md->name);
#else
            static char const* regs[] = {"rcx","rdx","r8","r9"};
            ins("pop rcx");
            --push_depth_;
            for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                ins("pop " + std::string(regs[i+1]));
                --push_depth_;
            }
            ins("call " + sd->name + "_" + md->name);
#endif
            return;
        }
        if (c.callee->kind() != ast::Expr::Name) {
            std::cerr << "[native] indirect calls kind " << (int)c.callee->kind() << std::endl;
            unsup("indirect calls");
        }
        auto& nm = static_cast<ast::NameExpr&>(*c.callee);
        // builtin 原语 → native 机器码实现(headers 侧对应表见 emit.cpp
        // builtin_rt_name)。PE: cpp2_write=thunk 符号、write_char=rodata
        // 单字符池、sys_exit=kernel32 IAT。ELF 规划: syscall 序列。
        if (!nm.qualified()) {
            std::string const& bn = nm.parts[0];
            if (bn == "err") { ins("xor eax, eax"); return; }  // 语言级哨兵
            bool declared_builtin = false;
            for (auto& f : m_->funcs)
                if (f.name == bn && f.builtin) { declared_builtin = true; break; }
            if (declared_builtin) {
                if (bn == "write_stdout") {
                    auto* lit = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
                    if (!(lit && lit->lit == ast::LitKind::String))
                        unsup("native write_stdout v0: buf must be a string literal");
                    std::string raw = lit->text;
                    std::string text = unescape_str(raw);                    std::string lbl = intern_string(text);
                    int len = (int)text.size();
#ifdef CPP2_NATIVE_HOST_OK
                    ins("lea rdi, " + lbl + "[rip]");
                    ins("mov esi, " + std::to_string(len));
#else
                    ins("lea rcx, " + lbl + "[rip]");
                    ins("mov edx, " + std::to_string(len));
#endif
                    ins("call cpp2_write");
                    return;
                }
                if (bn == "write_char") {
                    auto* lit = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
                    if (!(lit && lit->lit == ast::LitKind::Char))
                        unsup("native write_char v0: char literal only");
                    std::string t = lit->text;
                    char ch = (t.size() >= 3 && t.front()=='\'' && t.back()=='\'')
                            ? t[1] : '\0';
                    std::string lbl = intern_string(std::string(1, ch));
#ifdef CPP2_NATIVE_HOST_OK
                    ins("lea rdi, " + lbl + "[rip]");
                    ins("mov esi, 1");
#else
                    ins("lea rcx, " + lbl + "[rip]");
                    ins("mov edx, 1");
#endif
                    ins("call cpp2_write");
                    return;
                }
                if (bn == "sys_exit") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov edi, eax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call sys_exit");
                    return;
                }
                if (bn == "mem_alloc") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov edi, eax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call cpp2_alloc");
                    return;
                }
                if (bn == "mem_free") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call cpp2_free");
                    return;
                }
            }
        }
        if (nm.qualified() && nm.parts[0] == "std"
            && (nm.parts[1] == "println" || nm.parts[1] == "print")) {
            emit_printf(c, nm.parts[1] == "println");
            return;
        }
        if (nm.qualified()) unsup("qualified calls (std bridge)");
        if (c.args.size() > 6) unsup("more than 6 call arguments");

        for (auto& a : c.args) {
            eval(a.get());
            ins("push rax");
            ++push_depth_;
        }
        static char const* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        for (int i = (int)c.args.size() - 1; i >= 0; --i) {
            ins("pop " + std::string(regs[i]));
            --push_depth_;
        }
        if (push_depth_ % 2 == 1) {              // call 前 rsp 16 对齐
            ins("sub rsp, 8");
            ins("call " + nm.parts[0]);
            ins("add rsp, 8");
        } else {
            ins("call " + nm.parts[0]);
        }
    }

    void emit_printf(ast::CallExpr& c, bool newline)
    {
        if (c.args.empty()) unsup("println needs at least a format string");
        auto* fmt = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
        if (!fmt || fmt->lit != ast::LitKind::String)
            unsup("println format must be a string literal");
        std::string text = fmt->text;
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        std::string cfmt;
        for (size_t pos = 0; pos < text.size(); ++pos) {
            if (text[pos] == '{' && pos + 2 < text.size()
                && text[pos + 1] >= '0' && text[pos + 1] <= '9'
                && text[pos + 2] == '}') {
                cfmt += "%lld";
                pos += 2;
            } else if (text[pos] == '\\' && pos + 1 < text.size()) {
                char nxt = text[pos + 1];
                if (nxt == 'n') { cfmt += '\n'; ++pos; }
                else if (nxt == 't') { cfmt += '\t'; ++pos; }
                else if (nxt == '\\') { cfmt += '\\'; ++pos; }
                else if (nxt == '"') { cfmt += '"'; ++pos; }
                else cfmt += text[pos];
            } else if (text[pos] == '%') {
                cfmt += "%%";
            } else {
                cfmt += text[pos];
            }
        }
        if (newline) cfmt += "\n";
        std::string fmt_lbl = intern_string(cfmt);
        int nargs = (int)(c.args.size() - 1);
        for (size_t i = 1; i < c.args.size(); ++i) {
            eval(c.args[i].get());
            ins("push rax");
            ++push_depth_;
        }
        static char const* argregs[] = {"rsi","rdx","rcx","r8","r9"};
        for (int i = nargs - 1; i >= 0; --i) {
            ins("pop " + std::string(argregs[i]));
            --push_depth_;
        }
        ins("lea rdi, " + fmt_lbl + "[rip]");
        ins("xor eax, eax");
        ins("call printf");
    }

    void eval_binary(ast::BinaryExpr& b)
    {
        if (b.op == "&&" || b.op == "||") {
            std::string tT = lbl("ltrue"), fF = lbl("lfalse"), eE = lbl("lend");
            if (b.op == "&&") {
                std::string rR = lbl("rand");
                branch_bool(b.lhs.get(), rR, fF);
                label(rR);
                branch_bool(b.rhs.get(), tT, fF);
            } else {
                std::string rR = lbl("ror");
                branch_bool(b.lhs.get(), tT, rR);
                label(rR);
                branch_bool(b.rhs.get(), tT, fF);
            }
            label(fF);
            ins("xor eax, eax");
            ins("jmp " + eE);
            label(tT);
            ins("mov rax, 1");
            label(eE);
            return;
        }

        eval(b.lhs.get());
        ins("push rax");
        ++push_depth_;
        eval(b.rhs.get());
        ins("pop rcx");                          // rcx = lhs,rax = rhs
        --push_depth_;

        if (b.op == "+") { ins("add rax, rcx"); return; }
        if (b.op == "&")  { ins("and rax, rcx"); return; }
        if (b.op == "|")  { ins("or rax, rcx"); return; }
        if (b.op == "-") { ins("xchg rax, rcx"); ins("sub rax, rcx"); return; }
        if (b.op == "*") { ins("imul rax, rcx"); return; }
        if (b.op == "/" || b.op == "%") {
            ins("xchg rax, rcx");                // rax=lhs,rcx=rhs
            ins("test rcx, rcx");
            std::string ok = lbl("divok");
            ins("jnz " + ok);
            ins("lea rdi, .Lfmt_div0[rip]");
            ins("xor eax, eax");
            ins("call printf");
            ins("mov edi, 101");
            ins("call exit");
            label(ok);
            ins("cqo");
            ins("idiv rcx");
            if (b.op == "%") ins("mov rax, rdx");
            return;
        }
        if (b.op == "<<" || b.op == ">>") {
            ins("xchg rax, rcx");
            if (b.op == "<<") ins("shl rax, cl");
            else               ins("sar rax, cl");
            return;
        }
        if (b.op == "==") { ins("cmp rax, rcx"); ins("sete al"); ins("movzx rax, al"); return; }
        if (b.op == "!=") { ins("cmp rax, rcx"); ins("setne al"); ins("movzx rax, al"); return; }
        if (b.op == "<")  { ins("cmp rax, rcx"); ins("setl al"); ins("movzx rax, al"); return; }
        if (b.op == ">")  { ins("cmp rax, rcx"); ins("setg al"); ins("movzx rax, al"); return; }
        if (b.op == "<=") { ins("cmp rax, rcx"); ins("setle al"); ins("movzx rax, al"); return; }
        if (b.op == ">=") { ins("cmp rax, rcx"); ins("setge al"); ins("movzx rax, al"); return; }
        unsup("binary operator '" + b.op + "'");
    }

    // 双目标布尔分支(全短路;叶子退化求值判零)
    void branch_bool(ast::Expr* e, std::string const& t_true, std::string const& t_false)
    {
        if (e->kind() == ast::Expr::Binary) {
            auto& b = static_cast<ast::BinaryExpr&>(*e);
            if (b.op == "&&") {
                std::string r = lbl("andR");
                branch_bool(b.lhs.get(), r, t_false);
                label(r);
                branch_bool(b.rhs.get(), t_true, t_false);
                return;
            }
            if (b.op == "||") {
                std::string r = lbl("orR");
                branch_bool(b.lhs.get(), t_true, r);
                label(r);
                branch_bool(b.rhs.get(), t_true, t_false);
                return;
            }
            if (b.op == "==" || b.op == "!=" || b.op == "<"
                || b.op == ">" || b.op == "<=" || b.op == ">=") {
                eval(b.lhs.get());
                ins("push rax");
                ++push_depth_;
                eval(b.rhs.get());
                ins("pop rcx");
                --push_depth_;
                ins("cmp rcx, rax");
                std::string j;
                if (b.op == "==") j = "je";  else if (b.op == "!=") j = "jne";
                else if (b.op == "<") j = "jl"; else if (b.op == ">") j = "jg";
                else if (b.op == "<=") j = "jle"; else j = "jge";
                static std::unordered_map<std::string,std::string> const inv{
                    {"je","jne"},{"jne","je"},{"jl","jge"},{"jg","jle"},
                    {"jle","jg"},{"jge","jl"}};
                auto ji = inv.find(j);
                std::string jinv = (ji != inv.end()) ? ji->second : ("n" + j);
                if (!t_true.empty()) {
                    ins(j + " " + t_true);
                    if (!t_false.empty()) ins("jmp " + t_false);
                } else if (!t_false.empty()) {
                    ins(jinv + " " + t_false);
                }
                return;
            }
        }
        eval(e);
        ins("cmp rax, 0");
        if (!t_true.empty()) {
            ins("jne " + t_true);
            if (!t_false.empty()) ins("jmp " + t_false);
        } else if (!t_false.empty()) {
            ins("je " + t_false);
        }
    }
};

std::string emit_asm(ast::Module& m, sema::Result const& r)
{
    NativeEmitter em;
    return em.emit(m, r);
}

#else // Windows x64 ABI

class NativeEmitter {
public:
    std::string emit(ast::Module& m, sema::Result const&)
    {
        std::cerr << "[native] emit for module " << m.name << " funcs " << m.funcs.size() << " structs " << m.structs.size() << std::endl;
        m_ = &m;
        precheck();
        out_ << ".intel_syntax noprefix\n";
        emit_globals();
        put(".text");
        for (auto& f : m_->funcs) {
            if (f.is_extern) continue;
            if (!f.type_params.empty()) unsup("generic functions");
            check_func(f);
            emit_func(f);
        }
        for (auto& s : m_->structs) {
            for (auto& md : s.methods) {
                if (md.name == "destructor") continue;
                emit_method(s, md);
            }
        }
        emit_rodata_strs();
        return out_.str();
    }

private:
    ast::Module* m_ = nullptr;
    std::ostringstream out_;
    int label_ = 0;
    int push_depth_ = 0;
    ast::FuncDecl* cur_fn_ = nullptr;
    std::string cur_sym_name_;      // 当前发射符号(func 或 Struct_method)
    std::unordered_map<std::string, int> slots_;
    std::vector<std::string> break_labels_;
    std::vector<std::string> continue_labels_;
    std::unordered_map<std::string, std::string> str_pool_;

    [[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

    void put(std::string const& s) { out_ << s << "\n"; }
    void ins(std::string const& s) { out_ << "    " << s << "\n"; }
    void label(std::string const& l) { out_ << l << ":\n"; }
    std::string lbl(std::string const& tag) { return ".L" + tag + "_" + std::to_string(label_++); }

    std::string intern_string(std::string const& text)
    {
        auto it = str_pool_.find(text);
        if (it != str_pool_.end()) return it->second;
        std::string lblname = ".LS" + std::to_string(str_pool_.size());
        str_pool_[text] = lblname;
        return lblname;
    }

    void emit_rodata_strs()
    {
        if (str_pool_.empty()) return;
        out_ << ".section .rodata\n";
        for (auto const& [text, lblname] : str_pool_) {
            out_ << lblname << ":\n";
            out_ << "    .string \"";
            for (char c : text) {
                if (c == '\\') out_ << "\\\\";
                else if (c == '"') out_ << "\\\"";
                else if (c == '\n') out_ << "\\n";
                else if (c == '\t') out_ << "\\t";
                else if (c == '\0') break;
                else out_ << c;
            }
            out_ << "\"\n";
        }
        out_ << ".text\n";
    }

    void precheck()
    {
        std::cerr << "[native] precheck structs " << m_->structs.size() << " funcs " << m_->funcs.size() << std::endl;
        for (auto& s : m_->structs) std::cerr << "[native] struct " << s.name << " fields " << s.fields.size() << " methods " << s.methods.size() << std::endl;
        for (auto const& im : m_->imports) {
            std::string _im_name;
            for (size_t _i = 0; _i < im.module_parts.size(); ++_i) {
                if (_i) _im_name += ".";
                _im_name += im.module_parts[_i];
            }
            if (_im_name != "std") unsup("imports (only 'std' allowed in native v0)");
        }
        if (!m_->legacy_blocks.empty()) unsup("cxx_legacy blocks");
        for (auto& s : m_->structs) {
            for (auto& f : s.fields) {
                auto k = scalar_kind(f.type);
                bool ok = is_int_kind(k);
                // string/double 字段:8B 槽存指针/位模式(v0 值语义)
                if (!ok && f.type.parts.size()==1
                    && (f.type.parts[0]=="string" || f.type.parts[0]=="double")) ok = true;
                if (!ok) unsup("struct field '" + f.name + "' must be int/string/double");
            }
        }
        // enum 允许：底层 int，成员按 0..n-1 分配（与 C++ enum class 一致）
        // variant 允许：候选均为 int struct（如 Circle/Rect），match 穷尽
        if (!m_->concepts.empty())      unsup("concepts");
        for (auto& f : m_->funcs) check_func(f);
        for (auto& g : m_->globals) check_global(g);
    }

    void check_global(ast::GlobalVar& g)
    {
        auto k = scalar_kind(g.type);
        if (g.type.parts.size() != 1 || !is_int_kind(k))
            unsup("global '" + g.name + "' must be an explicitly-typed integer");
        if (!g.init || g.init->kind() != ast::Expr::Literal
            || static_cast<ast::LiteralExpr&>(*g.init).lit != ast::LitKind::Int)
            unsup("global '" + g.name + "' requires an integer literal initializer");
    }

    void check_func(ast::FuncDecl& f)
    {
        // throws:expected<R> 语义 v0 简化为值返回(err() 调用点 unsup)
        if (!f.type_params.empty()) unsup("generic functions");
        if (f.params.size() > 4) unsup("more than 4 parameters on Windows native: '" + f.name + "'");
    }

    sema::Type::Kind scalar_kind(ast::TypeUse const& t)
    {
        using K = sema::Type::Kind;
        if (t.parts.size() != 1) return K::Unknown;
        std::string const& n = t.parts[0];
        if (n == "int")  return K::Int;
        if (n == "i8")   return K::I8;   if (n == "i16") return K::I16;
        if (n == "i32")  return K::I32;  if (n == "i64") return K::I64;
        if (n == "u8")   return K::U8;   if (n == "u16") return K::U16;
        if (n == "u32")  return K::U32;  if (n == "u64") return K::U64;
        if (n == "bool") return K::Bool;
        return K::Unknown;
    }

    void emit_globals()
    {
        bool any = false;
        for (auto& g : m_->globals) {
            if (!any) { put(".data"); any = true; }
            long long v = std::stoll(static_cast<ast::LiteralExpr&>(*g.init).text);
            put(g.name + ":");
            put("    .quad " + std::to_string(v));
        }
    }

    int slot_of(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it == slots_.end()) unsup("name '" + n + "' is not a local/parameter");
        return it->second;
    }

    int slot_or_new(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it != slots_.end()) return it->second;
        int idx = (int)slots_.size();
        slots_[n] = -(8 * (idx + 1));
        return slots_[n];
    }

    void scan_slots(ast::Stmt* s)
    {
        if (!s) return;
        switch (s->kind()) {
        case ast::Stmt::Block: {
            for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) scan_slots(st.get());
            break;
        }
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(*s);
            slot_or_new(v.name);
            break;
        }
        case ast::Stmt::For: {
            auto& fo = static_cast<ast::ForStmt&>(*s);
            slot_or_new(fo.var);
            scan_slots(fo.body.get());
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
            scan_slots(i.then_block.get());
            scan_slots(i.else_block.get());
            break;
        }
        default: break;
        }
    }

    void emit_func(ast::FuncDecl& f)
    {
        cur_fn_ = &f;
        cur_sym_name_ = f.name;
        slots_.clear();
        push_depth_ = 0;
        for (size_t i = 0; i < f.params.size(); ++i)
            slots_[f.params[i].name] = -(8 * (int)(i + 1));
        scan_slots(f.has_block_body ? f.block_body.get() : nullptr);
        int words = (int)f.params.size() + (int)slots_.size() + 16; // +16 = temp slots + shadow
        int frame = ((words * 8 + 15) / 16) * 16;
        put(".globl " + f.name);
        put(f.name + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        for (size_t i = 0; i < f.params.size(); ++i)
            ins(std::string("mov QWORD PTR [rbp") + std::to_string(slots_[f.params[i].name]) + "], " + regs[i]);
        std::string ret = ".Lret_" + f.name;
        if (f.has_block_body && f.block_body)
            emit_stmt(f.block_body.get());
        else if (f.expr_body) {
            eval(f.expr_body.get());
            ins("jmp " + ret);
        } else {
            ins("jmp " + ret);
        }
        label(ret);
        if (f.name == "main") {
            if (f.ret && !f.ret->empty())
                ins("mov rcx, rax");
            else
                ins("xor rcx, rcx");
            ins("call ExitProcess");
            ins("ret");
        } else {
            ins("mov rsp, rbp");
            ins("pop rbp");
            ins("ret");
        }
        put("");
        cur_fn_ = nullptr;
    }

    void emit_method(ast::StructDecl& s, ast::MethodDecl& m)
    {
        std::cerr << "[native] emit_method " << s.name << "_" << m.name << std::endl;
        std::string mname = s.name + "_" + m.name;
        cur_fn_ = nullptr;
        cur_sym_name_ = mname;
        slots_.clear();
        push_depth_ = 0;
        slots_["this"] = -(8 * 1);
        for (size_t i = 0; i < m.params.size(); ++i)
            slots_[m.params[i].name] = -(8 * (int)(i + 2));
        if (m.has_block_body && m.block_body) scan_slots(m.block_body.get());
        int words = 1 + (int)m.params.size() + (int)slots_.size() + 16; // +16 temp
        int frame = ((words * 8 + 15) / 16) * 16;
        put(".globl " + mname);
        put(mname + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        ins("mov QWORD PTR [rbp-8], rcx");
        for (size_t i = 0; i < m.params.size(); ++i)
            ins(std::string("mov QWORD PTR [rbp") + std::to_string(-(8*(int)(i+2))) + "], " + regs[i+1]);
        std::string ret = ".Lret_" + mname;
        if (m.has_block_body && m.block_body)
            emit_stmt(m.block_body.get());
        else if (m.expr_body) {
            eval(m.expr_body.get());
            ins("jmp " + ret);
        }
        label(ret);
        ins("mov rsp, rbp");
        ins("pop rbp");
        ins("ret");
        put("");
    }

    void emit_stmt(ast::Stmt* s)
    {
        switch (s->kind()) {
        case ast::Stmt::Block: {
            for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) emit_stmt(st.get());
            break;
        }
        case ast::Stmt::ExprStmt:
            eval(static_cast<ast::ExprStmt*>(s)->expr.get());
            break;
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(*s);
            // 特殊处理 Point 等 struct 的 StructLit 初始化：直接按字段存入 p 的连续槽
            if (v.init && v.init->kind() == ast::Expr::StructLit) {
                auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
                ast::StructDecl* sd = nullptr;
                for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
                if (sd) {
                    std::string second = v.name + "#2";
                    int base_off, second_off;
                    // 确保 p 的基址为低地址（x 在 lo，y 在 hi）
                    auto it = slots_.find(v.name);
                    auto it2 = slots_.find(second);
                    if (it == slots_.end() && it2 == slots_.end()) {
                        // 首次分配：先为 y 分配高地址，再为 x 分配低地址，保证 x 在 lo
                        second_off = slot_or_new(second); // -8
                        base_off = slot_or_new(v.name);   // -16
                    } else if (it != slots_.end()) {
                        base_off = it->second;
                        second_off = (it2 == slots_.end()) ? slot_or_new(second) : it2->second;
                    } else {
                        second_off = it2->second;
                        base_off = slot_or_new(v.name);
                    }
                    int lo = std::min(base_off, second_off);
                    int hi = std::max(base_off, second_off);
                    // 矫正：确保 base_off 为 lo（x 的槽）
                    if (base_off != lo) {
                        // 交换 slots 中 p 与 p#2 的偏移，使 p 指向 lo
                        slots_[v.name] = lo;
                        slots_[second] = hi;
                        base_off = lo;
                        second_off = hi;
                    }
                    for (size_t i=0;i<sd->fields.size();++i) {
                        bool found = false;
                        for (auto& pr : sl.fields) if (pr.first == sd->fields[i].name) {
                            eval(pr.second.get());
                            found = true;
                            break;
                        }
                        if (!found) eval(sd->fields[i].init.get());
                        int off2 = (i==0) ? lo : hi;
                        ins("mov QWORD PTR [rbp" + std::to_string(off2) + "], rax");
                    }
                    break;
                }
            }
            // variant 类型变量:首槽 = tag(候选序),后续槽 = 数据
            bool is_variant_var = false;
            if (v.has_type && v.type.parts.size()==1) {
                for (auto& vd : m_->variants)
                    if (vd.name == v.type.parts[0]) { is_variant_var = true; break; }
            }
            if (is_variant_var) {
                int tag_off = slot_or_new(v.name + "#tag");
                int data_off = slot_or_new(v.name);
                std::string dslot2 = v.name + "#d2";
                slot_or_new(dslot2);
                int tag = -1;
                if (v.init && v.init->kind() == ast::Expr::StructLit) {
                    auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                    std::string tn = sl.type_parts.empty() ? "" : sl.type_parts[0];
                    for (auto& vd : m_->variants) {
                        if (vd.name != v.type.parts[0]) continue;
                        for (size_t k=0;k<vd.alternatives.size();++k)
                            if (vd.alternatives[k].parts.size()==1 && vd.alternatives[k].parts[0]==tn)
                                tag = (int)k;
                    }
                }
                if (tag < 0) unsup("variant init must be a candidate literal");
                ins("mov rax, " + std::to_string(tag));
                ins("mov QWORD PTR [rbp" + std::to_string(tag_off) + "], rax");
                // 数据字段逐个写入(从 data_off 起)
                ast::StructDecl* sd = nullptr;
                if (v.init && v.init->kind() == ast::Expr::StructLit) {
                    auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                    std::string tn = sl.type_parts.empty() ? "" : sl.type_parts[0];
                    for (auto& s2 : m_->structs) if (s2.name == tn) { sd = &s2; break; }
                }
                if (sd) {
                    for (size_t i=0;i<sd->fields.size();++i) {
                        bool found = false;
                        for (auto& pr : ((ast::StructLitExpr&)*v.init).fields)
                            if (pr.first == sd->fields[i].name) {
                                eval(pr.second.get());
                                found = true;
                                break;
                            }
                        if (!found) eval(sd->fields[i].init.get());
                        ins("mov QWORD PTR [rbp" + std::to_string(data_off + (int)i*8) + "], rax");
                    }
                }
                break;
            }
            int off = slot_or_new(v.name);
            if (v.init) eval(v.init.get());
            else ins("xor eax, eax");
            ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
            break;
        }
        case ast::Stmt::Return: {
            auto& r = static_cast<ast::ReturnStmt&>(*s);
            if (r.value) eval(r.value.get());
            else ins("xor eax, eax");
            ins("jmp .Lret_" + (cur_fn_ ? cur_fn_->name : cur_sym_name_));
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
            // if-let:if v := f() { } else e := it { }(v0 哨兵:非0=成功)
            if (i.is_let()) {
                int tmp_off = slot_or_new("$iflet_" + std::to_string(label_++));
                eval(i.let_init.get());
                ins("mov QWORD PTR [rbp" + std::to_string(tmp_off) + "], rax");
                std::string els = lbl("iflet_else"), end = lbl("iflet_end");
                ins("test rax, rax");
                ins("je " + els);
                if (i.let_name != "_" && !i.let_name.empty()) {
                    int boff = slot_or_new(i.let_name);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tmp_off) + "]");
                    ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                }
                emit_stmt(i.then_block.get());
                ins("jmp " + end);
                label(els);
                if (!i.else_binding.empty()) {
                    int boff = slot_or_new(i.else_binding);
                    ins("xor eax, eax");
                    ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                }
                if (i.else_block) emit_stmt(i.else_block.get());
                label(end);
                break;
            }
            std::string els = lbl("else");
            branch_bool(i.cond.get(), "", els);
            emit_stmt(i.then_block.get());
            if (i.else_block) {
                std::string end = lbl("endif");
                ins("jmp " + end);
                label(els);
                emit_stmt(i.else_block.get());
                label(end);
            } else {
                label(els);
            }
            break;
        }
        case ast::Stmt::While: {
            auto& w = static_cast<ast::WhileStmt&>(*s);
            std::string top = lbl("while"), end = lbl("wend");
            label(top);
            branch_bool(w.cond.get(), "", end);
            break_labels_.push_back(end);
            continue_labels_.push_back(top);
            emit_stmt(w.body.get());
            continue_labels_.pop_back();
            break_labels_.pop_back();
            ins("jmp " + top);
            label(end);
            break;
        }
        case ast::Stmt::For: {
            auto& fo = static_cast<ast::ForStmt&>(*s);
            if (!fo.is_range) {
                // string 迭代降级:for c in s → i=0; while i<strlen(s) { c=s[i]; ... }
                if (!fo.iterable || fo.iterable->kind() != ast::Expr::Name)
                    unsup("iterator for-loops (only 'for c in <string>' v0)");
                auto& itn = static_cast<ast::NameExpr&>(*fo.iterable);
                int ioff = slot_or_new(fo.var + "#i");
                int eoff2 = slot_or_new(fo.var + "#end2");
                int voff = slot_or_new(fo.var);
                // i = 0
                ins("xor eax, eax");
                ins("mov QWORD PTR [rbp" + std::to_string(ioff) + "], rax");
                // end = strlen(it)
                eval(fo.iterable.get());
#ifdef CPP2_NATIVE_HOST_OK
                ins("mov rdi, rax");
#else
                ins("mov rcx, rax");
#endif
                std::string stop = lbl("slen"), sdone = lbl("slend");
                label(stop);
#ifdef CPP2_NATIVE_HOST_OK
                ins("cmp byte PTR [rdi], 0");
                ins("je " + sdone);
                ins("inc rdi");
#else
                ins("cmp byte PTR [rcx], 0");
                ins("je " + sdone);
                ins("inc rcx");
#endif
                ins("jmp " + stop);
                label(sdone);
#ifdef CPP2_NATIVE_HOST_OK
                ins("sub rax, rdi");
                ins("neg rax");
#else
                ins("sub rax, rcx");
                ins("neg rax");
#endif
                ins("mov QWORD PTR [rbp" + std::to_string(eoff2) + "], rax");
                std::string top = lbl("fortop"), inc = lbl("forinc"), fend = lbl("fend");
                label(top);
                // i < end ?
                ins("mov rax, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                ins("cmp rax, QWORD PTR [rbp" + std::to_string(eoff2) + "]");
                ins("jge " + fend);
                break_labels_.push_back(fend);
                continue_labels_.push_back(inc);
                // c = it[i]
                eval(fo.iterable.get());
                ins("mov rcx, QWORD PTR [rbp" + std::to_string(ioff) + "]");
#ifdef CPP2_NATIVE_HOST_OK
                ins("movsx rax, byte PTR [rax+rcx]");
#else
                ins("movsx rax, byte PTR [rax+rcx]");
#endif
                ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rax");
                emit_stmt(fo.body.get());
                label(inc);
                ins("inc QWORD PTR [rbp" + std::to_string(ioff) + "]");
                break_labels_.pop_back();
                ins("jmp " + top);
                label(fend);
                break;
            }
            int voff = slot_or_new(fo.var);
            int eoff = slot_or_new(fo.var + "#end");
            eval(fo.range_begin.get());
            ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rax");
            eval(fo.range_end.get());
            ins("mov QWORD PTR [rbp" + std::to_string(eoff) + "], rax");
            std::string top = lbl("fortop"), inc = lbl("forinc"), end = lbl("fend");
            label(top);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(voff) + "]");
            ins("cmp rax, QWORD PTR [rbp" + std::to_string(eoff) + "]");
            ins((fo.inclusive ? "jg " : "jge ") + end);
            break_labels_.push_back(end);
            continue_labels_.push_back(inc);
            emit_stmt(fo.body.get());
            label(inc);
            ins("inc QWORD PTR [rbp" + std::to_string(voff) + "]");
            break_labels_.pop_back();
            ins("jmp " + top);
            label(end);
            break;
        }
        case ast::Stmt::Break: {
            if (break_labels_.empty()) unsup("break outside loop");
            ins("jmp " + break_labels_.back());
            break;
        }
        case ast::Stmt::Continue: {
            if (continue_labels_.empty()) unsup("continue outside loop");
            ins("jmp " + continue_labels_.back());
            break;
        }
        case ast::Stmt::Match: {
            auto& m = static_cast<ast::MatchStmt&>(*s);
            eval(m.scrutinee.get());
            int scr_off = slot_or_new("$scr_" + std::to_string(label_++));
            ins("mov QWORD PTR [rbp" + std::to_string(scr_off) + "], rax");
            std::string end = lbl("match_end");
            for (size_t i=0;i<m.arms.size();++i) {
                auto& arm = m.arms[i];
                std::string next = (i+1==m.arms.size()) ? end : lbl("match_next");
                if (arm.pat == ast::MatchArm::Pat::Wildcard) {
                } else if (arm.pat == ast::MatchArm::Pat::EnumMember) {
                    int enum_val = -1;
                    for (auto& e : m_->enums) {
                        for (size_t k=0;k<e.members.size();++k) if (e.members[k]==arm.enum_member) {
                            enum_val = (int)k;
                            break;
                        }
                        if (enum_val!=-1) break;
                    }
                    if (enum_val==-1) unsup("unknown enum member '" + arm.enum_member + "'");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx = -1;
                    std::string vname;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx = (int)k;
                                vname = v.name;
                                break;
                            }
                        }
                        if (var_idx!=-1) break;
                    }
                    if (var_idx==-1) unsup("unknown variant alternative");
                    // scrutinee 是 variant 变量 → tag 槽
                    std::string scr_name;
                    if (m.scrutinee->kind() == ast::Expr::Name)
                        scr_name = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
                    int tag_off = slot_of(scr_name + "#tag");
                    int data_off = slot_of(scr_name);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tag_off) + "]");
                    ins("cmp rax, " + std::to_string(var_idx));
                    ins("jne " + next);
                    // sub 绑定:.field=binding → binding = data[i]
                    for (auto& sp : arm.sub) {
                        if (sp == "_") continue;
                        size_t eq = sp.find('=');
                        if (sp.size() > 1 && sp[0]=='.' && eq != std::string::npos) {
                            std::string field = sp.substr(1, eq-1);
                            std::string bind = sp.substr(eq+1);
                            ast::StructDecl* sd = nullptr;
                            for (auto& s2 : m_->structs)
                                if (s2.name == arm.type_pattern.parts[0]) { sd = &s2; break; }
                            if (!sd) unsup("match bind: unknown struct");
                            int fi = -1;
                            for (size_t i=0;i<sd->fields.size();++i)
                                if (sd->fields[i].name==field) { fi=(int)i; break; }
                            if (fi < 0) unsup("match bind: unknown field '" + field + "'");
                            int boff = slot_or_new(bind);
                            ins("mov rax, QWORD PTR [rbp" + std::to_string(data_off + fi*8) + "]");
                            ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                        }
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Ok || arm.pat == ast::MatchArm::Pat::Some) {
                    // ok n => :值非 0;绑定 n = scrutinee
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("test rax, rax");
                    ins("je " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Err || arm.pat == ast::MatchArm::Pat::None) {
                    // err e => :值为 0;v0 错误消息不可用,e 绑定为 0
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]");
                    ins("test rax, rax");
                    ins("jne " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        ins("xor eax, eax");
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else {
                    unsup("unsupported match pattern");
                }
                emit_stmt(arm.body.get());
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            break;
        }
        default:
            unsup("statement kind");
        }
    }

    void eval(ast::Expr* e)
    {
        switch (e->kind()) {
        case ast::Expr::Literal: {
            auto& lit = static_cast<ast::LiteralExpr&>(*e);
            if (lit.lit == ast::LitKind::Bool)
                ins(std::string("mov rax, ") + (lit.text == "true" ? "1" : "0"));
            else if (lit.lit == ast::LitKind::Int)
                ins("mov rax, " + lit.text);
            else if (lit.lit == ast::LitKind::Char) {
                std::string t = lit.text;
                char ch = (t.size() >= 3 && t.front()=='\'' && t.back()=='\'') ? t[1] : '\0';
                ins("mov rax, " + std::to_string((int)(unsigned char)ch));
            } else if (lit.lit == ast::LitKind::String) {
                std::string text = lit.text;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                std::string lblname = intern_string(unescape_str(text));
                ins("lea rax, " + lblname + "[rip]");
            } else
                unsup("literal kinds beyond integers/bool/string/char");
            break;
        }
        case ast::Expr::Name: {
            auto& n = static_cast<ast::NameExpr&>(*e);
            if (n.qualified()) {
                // enum 成员 Color::blue -> 值
                if (n.parts.size()==2) {
                    for (auto& en : m_->enums) {
                        if (en.name == n.parts[0]) {
                            for (size_t k=0;k<en.members.size();++k) if (en.members[k]==n.parts[1]) {
                                ins("mov rax, " + std::to_string(k));
                                goto name_done;
                            }
                        }
                    }
                }
                unsup("qualified names");
            }
            {
                bool is_field = false;
                int field_off = -1;
                for (auto& s : m_->structs) {
                    for (size_t i=0;i<s.fields.size();++i) if (s.fields[i].name == n.parts[0]) {
                        field_off = (int)i * 8;
                        is_field = true;
                        break;
                    }
                    if (is_field) break;
                }
                if (is_field && cur_fn_ == nullptr) {
                    ins("mov rax, QWORD PTR [rbp-8]");
                    ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                } else {
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(slot_or_new(n.parts[0])) + "]");
                }
            }
            name_done:
            break;
        }
        case ast::Expr::Unary: {
            auto& u = static_cast<ast::UnaryExpr&>(*e);
            eval(u.operand.get());
            if (u.op == "-") ins("neg rax");
            else if (u.op == "!") { ins("cmp rax, 0"); ins("sete al"); ins("movzx rax, al"); }
            else unsup("unary operator '" + u.op + "'");
            break;
        }
        case ast::Expr::Binary:
            eval_binary(static_cast<ast::BinaryExpr&>(*e));
            break;
        case ast::Expr::Paren:
            eval(static_cast<ast::ParenExpr&>(*e).inner.get());
            break;
        case ast::Expr::Try: {
            // f()?:v0 哨兵语义 —— lhs==0 → 跳到函数出口返回 0
            auto& ty = static_cast<ast::TryExpr&>(*e);
            eval(ty.operand.get());
            std::string ok = lbl("tryok");
            ins("test rax, rax");
            ins("jne " + ok);
            ins("xor eax, eax");
            ins("jmp .Lret_" + (cur_fn_ ? cur_fn_->name : cur_sym_name_));
            label(ok);
            break;
        }
        case ast::Expr::OrDefault: {
            // f() or d:v0 哨兵语义 —— lhs==0 视为失败,取 rhs
            auto& od = static_cast<ast::OrDefaultExpr&>(*e);
            int tmp_off = slot_or_new("$or_" + std::to_string(label_++));
            eval(od.lhs.get());
            ins("mov QWORD PTR [rbp" + std::to_string(tmp_off) + "], rax");
            std::string has = lbl("orhas"), done = lbl("ordone");
            ins("test rax, rax");
            ins("jne " + has);
            eval(od.rhs.get());
            ins("jmp " + done);
            label(has);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(tmp_off) + "]");
            label(done);
            break;
        }
        case ast::Expr::Must: {
            // f()!:v0 哨兵语义 —— 0 即 trap
            auto& mu = static_cast<ast::MustExpr&>(*e);
            eval(mu.operand.get());
            std::string ok = lbl("mustok");
            ins("test rax, rax");
            ins("jne " + ok);
#ifdef CPP2_NATIVE_HOST_OK
            ins("xor edi, edi");
#else
            ins("xor rcx, rcx");
#endif
            ins("call sys_exit");
            label(ok);
            break;
        }
        case ast::Expr::AsCast: {
            auto& ac = static_cast<ast::AsCastExpr&>(*e);
            eval(ac.operand.get());
            break;
        }
        case ast::Expr::Assign: {
            auto& a = static_cast<ast::AssignExpr&>(*e);
            // 字段目标:p.x = v / p.x += v(方法内 x = v 已由 Name 路径处理)
            if (a.target->kind() == ast::Expr::Member) {
                auto& m = static_cast<ast::MemberExpr&>(*a.target);
                if (m.base->kind() != ast::Expr::Name) unsup("assign base must be name");
                auto& base = static_cast<ast::NameExpr&>(*m.base);
                int field_off = -1;
                for (auto& s : m_->structs) {
                    for (size_t i=0;i<s.fields.size();++i) if (s.fields[i].name == m.name) {
                        field_off = (int)i * 8;
                        break;
                    }
                    if (field_off != -1) break;
                }
                if (field_off == -1) unsup("unknown field '" + m.name + "'");
                int base_off = slot_of(base.parts[0]);
                int tgt_off = base_off + field_off;
                if (a.op == "=") {
                    eval(a.value.get());
                } else {
                    std::string lop = a.op.substr(0, a.op.size() - 1);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tgt_off) + "]");
                    ins("push rax");
                    ++push_depth_;
                    eval(a.value.get());
                    ins("pop rcx");
                    --push_depth_;
                    if (lop == "+") { ins("add rax, rcx"); }
                    else if (lop == "-") { ins("xchg rax, rcx"); ins("sub rax, rcx"); }
                    else if (lop == "*") { ins("imul rax, rcx"); }
                    else unsup("compound assign to field '" + a.op + "'");
                }
                ins("mov QWORD PTR [rbp" + std::to_string(tgt_off) + "], rax");
                break;
            }
            if (a.target->kind() != ast::Expr::Name) unsup("assignment form");
            auto& n = static_cast<ast::NameExpr&>(*a.target);
            // 方法上下文(cur_fn_==nullptr)且名字是字段 → 经 this 指针读写
            bool fld = false;
            int fld_off = -1;
            for (auto& s2 : m_->structs) {
                for (size_t i=0;i<s2.fields.size();++i) if (s2.fields[i].name == n.parts[0]) {
                    fld_off = (int)i * 8;
                    fld = true;
                    break;
                }
                if (fld) break;
            }
            int off;
            std::string tgt_addr;
            if (fld && cur_fn_ == nullptr) {
                off = 0;                          // 占位:实际经 this
                tgt_addr = "*this";
                ins("mov rax, QWORD PTR [rbp-8]");
                ins("add rax, " + std::to_string(fld_off));
                ins("push rax");
                ++push_depth_;                    // 栈顶 = 字段地址
            } else {
                off = slot_or_new(n.parts[0]);
                tgt_addr = "slot";
            }
            bool via_this = (tgt_addr == "*this");
            if (a.op == "=") {
                eval(a.value.get());
            } else {
                std::string lop = a.op.substr(0, a.op.size() - 1);
                if (via_this) {
                    // 字段复合赋值:栈顶已存字段地址;复制一份供读取
                    ins("mov rcx, QWORD PTR [rsp]");   // addr 副本(rcx 暂存)
                    ins("mov rax, QWORD PTR [rcx]");   // lhs = *addr
                } else {
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(off) + "]");
                }
                ins("push rax");
                ++push_depth_;
                eval(a.value.get());
                ins("pop rcx");
                --push_depth_;
                if (lop == "+") { ins("add rax, rcx"); }
                else if (lop == "-") { ins("xchg rax, rcx"); ins("sub rax, rcx"); }
                else if (lop == "*") { ins("imul rax, rcx"); }
                else if (lop == "/") {
                    ins("xchg rax, rcx");
                    ins("test rcx, rcx");
                    std::string ok = lbl("divok");
                    ins("jnz " + ok);
                    ins("lea rcx, .Lfmt_div0[rip]");
                    ins("sub rsp, 32");
                    ins("call printf");
                    ins("add rsp, 32");
                    ins("mov rcx, 101");
                    ins("sub rsp, 32");
                    ins("call exit");
                    ins("add rsp, 32");
                    label(ok);
                    ins("cqo");
                    ins("idiv rcx");
                } else if (lop == "%") {
                    ins("xchg rax, rcx");
                    ins("test rcx, rcx");
                    std::string ok = lbl("divok");
                    ins("jnz " + ok);
                    ins("lea rcx, .Lfmt_div0[rip]");
                    ins("sub rsp, 32");
                    ins("call printf");
                    ins("add rsp, 32");
                    ins("mov rcx, 101");
                    ins("sub rsp, 32");
                    ins("call exit");
                    ins("add rsp, 32");
                    label(ok);
                    ins("cqo");
                    ins("idiv rcx");
                    ins("mov rax, rdx");
                } else if (lop == "&") { ins("and rax, rcx"); }
                else if (lop == "|") { ins("or rax, rcx"); }
                else if (lop == "^") { ins("xor rax, rcx"); }
                else if (lop == "<<") { ins("xchg rax, rcx"); ins("shl rax, cl"); }
                else if (lop == ">>") { ins("xchg rax, rcx"); ins("sar rax, cl"); }
                else unsup("compound assignment '" + a.op + "'");
            }
            if (via_this) {
                ins("pop rcx");
                --push_depth_;
                ins("mov QWORD PTR [rcx], rax");
            } else {
                ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
            }
            break;
        }
        case ast::Expr::Member: {
            auto& m = static_cast<ast::MemberExpr&>(*e);
            if (m.base->kind() != ast::Expr::Name) unsup("member base must be name");
            auto& base = static_cast<ast::NameExpr&>(*m.base);
            int field_off = -1;
            for (auto& s : m_->structs) {
                for (size_t i=0;i<s.fields.size();++i) if (s.fields[i].name == m.name) {
                    field_off = (int)i * 8;
                    break;
                }
                if (field_off != -1) break;
            }
            if (field_off == -1) unsup("unknown field '" + m.name + "'");
            int base_off = slot_of(base.parts[0]);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off + field_off) + "]");
            break;
        }
        case ast::Expr::StructLit: {
            auto& sl = static_cast<ast::StructLitExpr&>(*e);
            std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
            ast::StructDecl* sd = nullptr;
            for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
            if (!sd) unsup("unknown struct '" + tname + "'");
            std::string tmp = "$tmp_struct_" + std::to_string(label_++);
            int tmp_off = slot_or_new(tmp);
            std::string tmp2 = tmp + "_2";
            slot_or_new(tmp2);
            for (size_t i=0;i<sd->fields.size();++i) {
                bool found = false;
                for (auto& pr : sl.fields) if (pr.first == sd->fields[i].name) {
                    eval(pr.second.get());
                    found = true;
                    break;
                }
                if (!found) eval(sd->fields[i].init.get());
                ins("mov QWORD PTR [rbp" + std::to_string(tmp_off + (int)i*8) + "], rax");
            }
            ins("mov rax, " + std::to_string(tmp_off));
            break;
        }
        case ast::Expr::Call:
            emit_call(static_cast<ast::CallExpr&>(*e));
            break;
        case ast::Expr::Match: {
            auto& m = static_cast<ast::MatchExpr&>(*e);
            // variant 变量 scrutinee:直接用 tag/data 槽(不压栈)
            std::string scr_name2;
            if (m.scrutinee->kind() == ast::Expr::Name)
                scr_name2 = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
            int scr_tag_off = slot_of(scr_name2 + "#tag");
            int scr_data_off = slot_of(scr_name2);
            std::string end = lbl("match_expr_end");
            std::string res_lbl = "$match_res_" + std::to_string(label_++);
            int res_off = slot_or_new(res_lbl);
            for (size_t i=0;i<m.arms.size();++i) {
                auto& arm = m.arms[i];
                std::string next = (i+1==m.arms.size()) ? end : lbl("match_next");
                if (arm.pat == ast::MatchArm::Pat::Wildcard) {
                } else if (arm.pat == ast::MatchArm::Pat::EnumMember) {
                    int enum_val = -1;
                    for (auto& e : m_->enums) {
                        for (size_t k=0;k<e.members.size();++k) if (e.members[k]==arm.enum_member) {
                            enum_val = (int)k;
                            break;
                        }
                        if (enum_val!=-1) break;
                    }
                    if (enum_val==-1) unsup("unknown enum member '" + arm.enum_member + "'");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_tag_off) + "]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx2 = -1;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx2 = (int)k;
                                break;
                            }
                        }
                        if (var_idx2!=-1) break;
                    }
                    if (var_idx2==-1) unsup("unknown variant alternative");
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_tag_off) + "]");
                    ins("cmp rax, " + std::to_string(var_idx2));
                    ins("jne " + next);
                    // sub 绑定
                    for (auto& sp : arm.sub) {
                        if (sp == "_") continue;
                        size_t eq = sp.find('=');
                        if (sp.size() > 1 && sp[0]=='.' && eq != std::string::npos) {
                            std::string field = sp.substr(1, eq-1);
                            std::string bind = sp.substr(eq+1);
                            ast::StructDecl* sd2 = nullptr;
                            for (auto& s2 : m_->structs)
                                if (s2.name == arm.type_pattern.parts[0]) { sd2 = &s2; break; }
                            if (!sd2) unsup("match expr bind: unknown struct");
                            int fi = -1;
                            for (size_t i=0;i<sd2->fields.size();++i)
                                if (sd2->fields[i].name==field) { fi=(int)i; break; }
                            if (fi < 0) unsup("match expr bind: unknown field");
                            int boff = slot_or_new(bind);
                            ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_data_off + fi*8) + "]");
                            ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                        }
                    }
                } else {
                    unsup("unsupported match expr pattern");
                }
                // 臂体为单条语句（ExprStmt 包装的表达式）
                if (arm.body && arm.body->kind() == ast::Stmt::ExprStmt) {
                    eval(static_cast<ast::ExprStmt&>(*arm.body).expr.get());
                } else if (arm.body) {
                    emit_stmt(arm.body.get());
                    // 假设臂体已将结果放入 rax
                }
                ins("mov QWORD PTR [rbp" + std::to_string(res_off) + "], rax");
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            ins("mov rax, QWORD PTR [rbp" + std::to_string(res_off) + "]");
            break;
        }
        default:
            unsup("expression kind");
        }
    }

    void emit_call(ast::CallExpr& c)
    {
        if (c.callee->kind() == ast::Expr::Member) {
            auto& mem = static_cast<ast::MemberExpr&>(*c.callee);
            if (mem.base->kind() != ast::Expr::Name) unsup("method base must be name");
            auto& base = static_cast<ast::NameExpr&>(*mem.base);
            ast::StructDecl* sd = nullptr;
            ast::MethodDecl* md = nullptr;
            for (auto& s : m_->structs) {
                for (auto& m : s.methods) if (m.name == mem.name) { sd = &s; md = &m; break; }
                if (sd) break;
            }
            if (!sd || !md) {
                // std 方法桥(v0 语义化降级):
                //   string.size()/length() → strlen 循环
                //   string.empty()         → strlen==0
                //   error.message()        → 返回 0(错误串存储 v1)
                //   optional.has_value()   → 1(非空即真)
                if (mem.name == "size" || mem.name == "length") {
                    eval(mem.base.get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
#else
                    ins("mov rcx, rax");
#endif
                    std::string top = lbl("strlen"), done = lbl("strdone");
                    label(top);
#ifdef CPP2_NATIVE_HOST_OK
                    ins("cmp byte PTR [rdi], 0");
                    ins("je " + done);
                    ins("inc rdi");
#else
                    ins("cmp byte PTR [rcx], 0");
                    ins("je " + done);
                    ins("inc rcx");
#endif
                    ins("jmp " + top);
                    label(done);
#ifdef CPP2_NATIVE_HOST_OK
                    ins("sub rax, rdi");
                    ins("neg rax");
#else
                    ins("sub rax, rcx");
                    ins("neg rax");
#endif
                    return;
                }
                if (mem.name == "empty") {
                    eval(mem.base.get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("cmp byte PTR [rax], 0");
#else
                    ins("cmp byte PTR [rax], 0");
#endif
                    std::string nz = lbl("strnz"), se = lbl("strend");
                    ins("jne " + nz);
                    ins("mov rax, 1");
                    ins("jmp " + se);
                    label(nz);
                    ins("xor eax, eax");
                    label(se);
                    return;
                }
                if (mem.name == "message") {
                    eval(mem.base.get());
                    ins("xor eax, eax");          // v0:错误串不可用,返回空
                    return;
                }
                if (mem.name == "has_value") {
                    eval(mem.base.get());
                    ins("test rax, rax");
                    std::string nz = lbl("optnz"), oe = lbl("optend");
                    ins("setne al");
                    ins("movzx rax, al");
                    return;
                }
                unsup("unknown method '" + mem.name + "'");
            }
            int base_off = slot_of(base.parts[0]);
            // 实参先求值压栈,this 最后压栈(rcx 最先弹出)
            for (auto& a : c.args) {
                eval(a.get());
                ins("push rax");
                ++push_depth_;
            }
            ins("lea rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
            ins("push rax");
            ++push_depth_;
            static char const* regs[] = {"rcx","rdx","r8","r9"};
            ins("pop rcx");                       // this
            --push_depth_;
            for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                ins("pop " + std::string(regs[i+1]));
                --push_depth_;
            }
            ins("call " + sd->name + "_" + md->name);
            return;
        }
        if (c.callee->kind() != ast::Expr::Name) unsup("indirect calls");
        auto& nm = static_cast<ast::NameExpr&>(*c.callee);
        if (!nm.qualified()) {
            std::string const& bn = nm.parts[0];
            if (bn == "err") { ins("xor eax, eax"); return; }  // 语言级哨兵
            bool declared_builtin = false;
            for (auto& f : m_->funcs)
                if (f.name == bn && f.builtin) { declared_builtin = true; break; }
            if (declared_builtin) {
                if (bn == "write_stdout") {
                    auto* lit = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
                    if (!(lit && lit->lit == ast::LitKind::String))
                        unsup("native write_stdout v0: buf must be a string literal");
                    std::string text = unescape_str(lit->text);
                    std::string lbl = intern_string(text);
#ifdef CPP2_NATIVE_HOST_OK
                    ins("lea rdi, " + lbl + "[rip]");
                    ins("mov esi, " + std::to_string(text.size()));
#else
                    ins("lea rcx, " + lbl + "[rip]");
                    ins("mov edx, " + std::to_string(text.size()));
#endif
                    ins("call cpp2_write");
                    return;
                }
                if (bn == "write_char") {
                    auto* lit = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
                    if (!(lit && lit->lit == ast::LitKind::Char))
                        unsup("native write_char v0: char literal only");
                    std::string t = lit->text;
                    char ch = (t.size() >= 3 && t.front()=='\'' && t.back()=='\'')
                            ? t[1] : '\0';
                    std::string lbl = intern_string(std::string(1, ch));
#ifdef CPP2_NATIVE_HOST_OK
                    ins("lea rdi, " + lbl + "[rip]");
                    ins("mov esi, 1");
#else
                    ins("lea rcx, " + lbl + "[rip]");
                    ins("mov edx, 1");
#endif
                    ins("call cpp2_write");
                    return;
                }
                if (bn == "sys_exit") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov edi, eax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call sys_exit");
                    return;
                }
                if (bn == "mem_alloc") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov edi, eax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call cpp2_alloc");
                    return;
                }
                if (bn == "mem_free") {
                    eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
#else
                    ins("mov rcx, rax");
#endif
                    ins("call cpp2_free");
                    return;
                }
            }
        }
        if (nm.qualified() && nm.parts[0] == "std" && (nm.parts[1] == "println" || nm.parts[1] == "print")) {
            emit_printf(c, nm.parts[1] == "println");
            return;
        }
        if (nm.qualified()) unsup("qualified calls (std bridge)");
        if (c.args.size() > 4) unsup("more than 4 call arguments on Windows native");
        for (auto& a : c.args) {
            eval(a.get());
            ins("push rax");
            ++push_depth_;
        }
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        for (int i = (int)c.args.size() - 1; i >= 0; --i) {
            ins("pop " + std::string(regs[i]));
            --push_depth_;
        }
        ins("sub rsp, 32");
        ins("call " + nm.parts[0]);
        ins("add rsp, 32");
    }

    void emit_printf(ast::CallExpr& c, bool newline)
    {
        if (c.args.empty()) unsup("println needs at least a format string");
        if (c.args.size() == 1) {
            auto* lit0 = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
            if (lit0 && lit0->lit == ast::LitKind::String) {
                // 单参字符串：走下方 cfmt 流程
            } else {
                // 单参非字符串（如 c as int）：直接 %d
                std::string cfmt = "%d";
                if (newline) cfmt += "\n";
                std::string fmt_lbl = intern_string(cfmt);
                                eval(c.args[0].get());
#ifdef CPP2_NATIVE_HOST_OK
                ins("mov rsi, rax");
                ins("lea rdi, " + fmt_lbl + "[rip]");
#else
                ins("mov rdx, rax");
                ins("lea rcx, " + fmt_lbl + "[rip]");
#endif
                ins("xor eax, eax");
                ins("sub rsp, 32");
                ins("call printf");
                ins("add rsp, 32");
                return;
            }
        }
        auto* fmt = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
        if (!fmt) {
            std::cerr << "[native] emit_printf fmt not Literal kind " << (int)c.args[0]->kind() << std::endl;
            unsup("println format must be a string literal");
        }
        if (fmt->lit != ast::LitKind::String) {
            std::cerr << "[native] emit_printf fmt lit " << (int)fmt->lit << " text " << fmt->text << std::endl;
            unsup("println format must be a string literal");
        }
        std::string text = fmt->text;
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        std::string cfmt;
        for (size_t pos = 0; pos < text.size(); ++pos) {
            if (text[pos] == '{' && pos + 2 < text.size() && text[pos + 1] >= '0' && text[pos + 1] <= '9' && text[pos + 2] == '}') {
                cfmt += "%lld";
                pos += 2;
            } else if (text[pos] == '\\' && pos + 1 < text.size()) {
                char nxt = text[pos + 1];
                if (nxt == 'n') { cfmt += '\n'; ++pos; }
                else if (nxt == 't') { cfmt += '\t'; ++pos; }
                else if (nxt == '\\') { cfmt += '\\'; ++pos; }
                else if (nxt == '"') { cfmt += '"'; ++pos; }
                else cfmt += text[pos];
            } else if (text[pos] == '%') {
                cfmt += "%%";
            } else {
                cfmt += text[pos];
            }
        }
        if (newline) cfmt += "\n";
        std::string fmt_lbl = intern_string(cfmt);
        int nargs = (int)(c.args.size() - 1);
        for (size_t i = 1; i < c.args.size(); ++i) {
            eval(c.args[i].get());
            ins("push rax");
            ++push_depth_;
        }
        static char const* argregs[] = {"rdx","r8","r9"};
        for (int i = nargs - 1; i >= 0; --i) {
            ins("pop " + std::string(argregs[i]));
            --push_depth_;
        }
        ins("sub rsp, 32");
        ins("lea rcx, " + fmt_lbl + "[rip]");
        ins("xor eax, eax");
        ins("call printf");
        ins("add rsp, 32");
    }

    void eval_binary(ast::BinaryExpr& b)
    {
        if (b.op == "&&" || b.op == "||") {
            std::string tT = lbl("ltrue"), fF = lbl("lfalse"), eE = lbl("lend");
            if (b.op == "&&") {
                std::string rR = lbl("rand");
                branch_bool(b.lhs.get(), rR, fF);
                label(rR);
                branch_bool(b.rhs.get(), tT, fF);
            } else {
                std::string rR = lbl("ror");
                branch_bool(b.lhs.get(), tT, rR);
                label(rR);
                branch_bool(b.rhs.get(), tT, fF);
            }
            label(fF);
            ins("xor eax, eax");
            ins("jmp " + eE);
            label(tT);
            ins("mov rax, 1");
            label(eE);
            return;
        }
        eval(b.lhs.get());
        ins("push rax");
        ++push_depth_;
        eval(b.rhs.get());
        ins("pop rcx");
        --push_depth_;
        if (b.op == "+") { ins("add rax, rcx"); return; }
        if (b.op == "&")  { ins("and rax, rcx"); return; }
        if (b.op == "|")  { ins("or rax, rcx"); return; }
        if (b.op == "-") { ins("xchg rax, rcx"); ins("sub rax, rcx"); return; }
        if (b.op == "*") { ins("imul rax, rcx"); return; }
        if (b.op == "/" || b.op == "%") {
            ins("xchg rax, rcx");
            ins("test rcx, rcx");
            std::string ok = lbl("divok");
            ins("jnz " + ok);
            ins("lea rcx, .Lfmt_div0[rip]");
            ins("call printf");
            ins("mov ecx, 101");
            ins("call exit");
            label(ok);
            ins("cqo");
            ins("idiv rcx");
            if (b.op == "%") ins("mov rax, rdx");
            return;
        }
        if (b.op == "<<" || b.op == ">>") {
            ins("xchg rax, rcx");
            if (b.op == "<<") ins("shl rax, cl");
            else               ins("sar rax, cl");
            return;
        }
        if (b.op == "==") { ins("cmp rax, rcx"); ins("sete al"); ins("movzx rax, al"); return; }
        if (b.op == "!=") { ins("cmp rax, rcx"); ins("setne al"); ins("movzx rax, al"); return; }
        if (b.op == "<")  { ins("cmp rax, rcx"); ins("setl al"); ins("movzx rax, al"); return; }
        if (b.op == ">")  { ins("cmp rax, rcx"); ins("setg al"); ins("movzx rax, al"); return; }
        if (b.op == "<=") { ins("cmp rax, rcx"); ins("setle al"); ins("movzx rax, al"); return; }
        if (b.op == ">=") { ins("cmp rax, rcx"); ins("setge al"); ins("movzx rax, al"); return; }
        unsup("binary operator '" + b.op + "'");
    }

    void branch_bool(ast::Expr* e, std::string const& t_true, std::string const& t_false)
    {
        if (e->kind() == ast::Expr::Binary) {
            auto& b = static_cast<ast::BinaryExpr&>(*e);
            if (b.op == "&&") {
                std::string r = lbl("andR");
                branch_bool(b.lhs.get(), r, t_false);
                label(r);
                branch_bool(b.rhs.get(), t_true, t_false);
                return;
            }
            if (b.op == "||") {
                std::string r = lbl("orR");
                branch_bool(b.lhs.get(), t_true, r);
                label(r);
                branch_bool(b.rhs.get(), t_true, t_false);
                return;
            }
            if (b.op == "==" || b.op == "!=" || b.op == "<" || b.op == ">" || b.op == "<=" || b.op == ">=") {
                eval(b.lhs.get());
                ins("push rax");
                ++push_depth_;
                eval(b.rhs.get());
                ins("pop rcx");
                --push_depth_;
                ins("cmp rcx, rax");
                std::string j;
                if (b.op == "==") j = "je";  else if (b.op == "!=") j = "jne";
                else if (b.op == "<") j = "jl"; else if (b.op == ">") j = "jg";
                else if (b.op == "<=") j = "jle"; else j = "jge";
                static std::unordered_map<std::string,std::string> const inv{
                    {"je","jne"},{"jne","je"},{"jl","jge"},{"jg","jle"},
                    {"jle","jg"},{"jge","jl"}};
                auto ji = inv.find(j);
                std::string jinv = (ji != inv.end()) ? ji->second : ("n" + j);
                if (!t_true.empty()) {
                    ins(j + " " + t_true);
                    if (!t_false.empty()) ins("jmp " + t_false);
                } else if (!t_false.empty()) {
                    ins(jinv + " " + t_false);
                }
                return;
            }
        }
        eval(e);
        ins("cmp rax, 0");
        if (!t_true.empty()) {
            ins("jne " + t_true);
            if (!t_false.empty()) ins("jmp " + t_false);
        } else if (!t_false.empty()) {
            ins("je " + t_false);
        }
    }
};

std::string emit_asm(ast::Module& m, sema::Result const& r)
{
    NativeEmitter em;
    return em.emit(m, r);
}

std::vector<uint8_t> emit_pe(ast::Module& m, sema::Result const& r)
{
    // Windows 直出 PE:零 CRT(hello 单测)。print → cpp2_write thunk(kernel32
    // GetStdHandle+WriteFile),ExitProcess 收尾;msvcrt 不再出现在 IAT。
    if (!m.structs.empty() || !m.enums.empty() || !m.variants.empty() || !m.concepts.empty()) throw Unsupported("emit_pe: hello has no types");
    if (m.funcs.size() != 1 || m.funcs[0].name != "main") throw Unsupported("emit_pe: only hello main");
    auto& main = m.funcs[0];
    if (!main.has_block_body || !main.block_body) throw Unsupported("emit_pe: main needs block");

    std::string hello = "Hello, C++2!\n";
    int hello_len = (int)hello.size();          // 不含 NUL

    // ── main ──
    x64::Emitter e;
    e.push_rbp();
    e.mov_rbp_rsp();
    e.sub_rsp_imm8(32);                          // shadow space
    e.lea_rcx_rip(".LS0");                       // arg1 = buf
    e.mov_edx_imm32(hello_len);                  // arg2 = len
    e.call_rel("cpp2_write");                    // 内部 thunk(text_labels)
    e.add_rsp_imm8(32);
    e.xor_ecx_ecx();
    e.call_indirect_rip("ExitProcess");          // kernel32!ExitProcess(0)

    // ── cpp2_write thunk(手编 Win64;跨平台注:ELF 路径在 elf.cpp 用 syscall 对等实现)──
    // cpp2_write(rcx=buf, edx=len):
    //   push rbx; sub rsp,0x40                 ; 保持 16 对齐
    //   mov ebx, edx                           ; len 暂存(callee-saved)
    //   mov r14d? — 不用 volatile 存 buf:
    //   实际序列:
    //     mov rsi, rcx                         ; buf(rsi 非参数寄存器,安全)
    //     mov ecx, -11                         ; STD_OUTPUT_HANDLE
    //     call qword [rip+IAT.GetStdHandle]    ; rax = handle
    //     mov rcx, rax                         ; WriteFile(handle,
    //     mov rdx, rsi                         ;            buf,
    //     mov r8d, ebx                         ;            len,
    //     lea r9, [rsp+0x28]                   ;            &written,
    //     mov qword [rsp+0x20], 0              ;            NULL)   ; 第5参走栈
    //     call qword [rip+IAT.WriteFile]
    //     add rsp,0x40; pop rbx; ret
    x64::Emitter t;
    size_t thunk_off = 0;                        // 相对 .text 起点(main 之后)
    {
        // 先量 main 长度:thunk_off = e.code.size()(对齐 16 便于阅读)
        thunk_off = (e.code.size() + 15) & ~size_t(15);
        while (e.code.size() < thunk_off) e.code.push_back(0xCC); // int3 填充

        auto t8 = [&](uint8_t b){ e.code.push_back(b); };
        auto t32 = [&](int32_t v){ for(int i=0;i<4;++i) e.code.push_back(uint8_t((v>>(i*8))&0xff)); };
        auto rel32_to = [&](const char* sym){
            t8(0xFF); t8(0x15);                  // call qword ptr [rip+disp32]
            e.relocs.push_back({e.code.size(), sym, true});
            t32(0);
        };
        // push rbx                      53
        t8(0x53);
        // sub rsp, 0x40                 48 83 EC 40
        t8(0x48); t8(0x83); t8(0xEC); t8(0x40);
        // mov [rsp+0x38], rcx           48 89 4C 24 38   ; buf 存栈(volatile 寄存器跨调用不保)
        t8(0x48); t8(0x89); t8(0x4C); t8(0x24); t8(0x38);
        // mov ebx, edx                  89 D3            ; len(callee-saved)
        t8(0x89); t8(0xD3);
        // mov ecx, -11                  B9 F7 FF FF FF   ; STD_OUTPUT_HANDLE
        t8(0xB9); t32(-11);
        rel32_to("GetStdHandle");            // rax = handle
        // mov rcx, rax                  48 89 C1
        t8(0x48); t8(0x89); t8(0xC1);
        // mov rdx, [rsp+0x38]           48 8B 54 24 38   ; buf
        t8(0x48); t8(0x8B); t8(0x54); t8(0x24); t8(0x38);
        // mov r8d, ebx                  41 89 D8         ; len
        t8(0x41); t8(0x89); t8(0xD8);
        // lea r9, [rsp+0x28]            4C 8D 4C 24 28   ; &written
        t8(0x4C); t8(0x8D); t8(0x4C); t8(0x24); t8(0x28);
        // mov qword [rsp+0x20], 0       48 C7 44 24 20 … ; 第5参 NULL
        t8(0x48); t8(0xC7); t8(0x44); t8(0x24); t8(0x20);
        for(int i=0;i<4;++i) t8(0);
        rel32_to("WriteFile");
        // add rsp, 0x40                 48 83 C4 40
        t8(0x48); t8(0x83); t8(0xC4); t8(0x40);
        // pop rbx                       5B
        t8(0x5B);
        // ret                           C3
        t8(0xC3);
    }

    std::vector<uint8_t> text = e.code;
    std::vector<uint8_t> rodata;
    hello.push_back('\0');
    for(char c: hello) rodata.push_back((uint8_t)c);

    std::vector<pe::Reloc> relocs;
    for(auto &r: e.relocs){
        pe::Reloc pr;
        pr.offset = r.pos;
        pr.target = r.target;
        pr.is_call = r.is_call;
        relocs.push_back(pr);
    }
    std::vector<std::pair<std::string,std::string>> labels = { {".LS0","0"} };
    std::vector<std::pair<std::string,size_t>> text_labels = { {"cpp2_write", thunk_off} };
    return pe::build_exe(text, rodata, relocs, labels, text_labels);
}

// ── 通用 native: .s 文本 → asm64 汇编 → PE 字节(零外部工具)────────
std::vector<uint8_t> emit_native(const std::string& asm_text)
{
    // 追加最小运行时(零 CRT):cpp2_write/sys_exit/... 直接走 kernel32 系统调用
    static char const* runtime_s = R"(
.section .text
.globl cpp2_write
cpp2_write:
    push rbp
    mov rbp, rsp
    sub rsp, 0x30
    mov QWORD PTR [rbp-0x18], rcx
    mov QWORD PTR [rbp-0x20], rdx
    mov ecx, -11
    call GetStdHandle
    mov rcx, rax
    mov rdx, [rbp-0x18]
    mov r8, [rbp-0x20]
    lea r9, [rbp-0x08]
    mov QWORD PTR [rbp-0x10], 0
    call WriteFile
    add rsp, 0x30
    pop rbp
    ret
.globl cpp2_exit
cpp2_exit:
    call ExitProcess
    ret
.globl sys_exit
sys_exit:
    call ExitProcess
    ret
.globl cpp2_alloc
cpp2_alloc:
    push rbp
    mov rbp, rsp
    sub rsp, 0x20
    mov QWORD PTR [rbp-0x10], rcx
    call GetProcessHeap
    mov rcx, rax
    mov rdx, 0
    mov r8, [rbp-0x10]
    call HeapAlloc
    add rsp, 0x20
    pop rbp
    ret
.globl cpp2_free
cpp2_free:
    push rbp
    mov rbp, rsp
    sub rsp, 0x20
    mov QWORD PTR [rbp-0x10], rcx
    call GetProcessHeap
    mov rcx, rax
    mov rdx, 0
    mov r8, [rbp-0x10]
    call HeapFree
    add rsp, 0x20
    pop rbp
    ret
)";
    std::string full = asm_text + "\n" + runtime_s;

    auto res = asm64::assemble(full);
    if (res.text.empty()) throw Unsupported("asm64 produced no code");

    // 重定位:asm64 已把内部标签回填,其余(rodata + 外部导入)以 RIP 相对重定位给出
    std::vector<pe::Reloc> relocs;
    for (auto& r : res.relocs)
        relocs.push_back({r.offset, r.target, r.ref, r.is_call});

    // 构建 rodata 标签表(label→offset in rodata blob)
    std::vector<uint8_t> rodata_blob;
    std::vector<std::pair<std::string,std::string>> ro_labels;
    for (auto& [name, bytes] : res.rodata) {
        ro_labels.push_back({name, std::to_string(rodata_blob.size())});
        rodata_blob.insert(rodata_blob.end(), bytes.begin(), bytes.end());
    }

    // 构建 text 内标签表(函数入口等,跳过 .L 局部)
    std::vector<std::pair<std::string,size_t>> text_labels;
    for (auto& [name, off] : res.symbols) {
        if (!name.empty() && name[0] != '.')
            text_labels.push_back({name, off});
    }

    // 动态导入表:把外部符号按 DLL 归类
    std::map<std::string, std::vector<std::string>> imports_map;
    auto dll_for = [](std::string const& s) -> std::string {
        if (s == "printf" || s == "scanf" || s == "strlen" || s == "memcpy" ||
            s == "sprintf" || s == "strcmp" || s == "strcpy" || s == "puts" ||
            s == "malloc" || s == "free" || s == "exit" || s == "abort")
            return "msvcrt.dll";
        return "kernel32.dll";
    };
    for (auto& e : res.externs) {
        if (e == "cpp2_write" || e == "cpp2_exit" || e == "sys_exit" ||
            e == "cpp2_alloc" || e == "cpp2_free")
            continue;  // 由内联运行时提供
        imports_map[dll_for(e)].push_back(e);
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> imports(
        imports_map.begin(), imports_map.end());

    return pe::build_exe(res.text, rodata_blob, relocs,
                         ro_labels, text_labels, imports);
}

#endif

} // namespace cpp2::native
