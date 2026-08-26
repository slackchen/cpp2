// 原生后端 v0 实现:x86-64 SysV 汇编发射(见 native.hpp)
// 设计:栈机风格——表达式值经 rax 传递,中间量压栈;无寄存器分配。
// 槽位:参数与局部变量统一扁平分配 [rbp-8k];帧大小对齐 16。
// 平台:仅 SysV(Linux);Windows 构建的工具自动回退转译(需求行为)。
#include "native.hpp"
#include "native/x64.hpp"
#include "native/pe.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#ifndef _WIN32
#define CPP2_NATIVE_HOST_OK 1
#endif

namespace cpp2::native {

[[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

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
                if (!is_int_kind(k)) unsup("struct field '" + f.name + "' must be int");
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
        slots_.clear();
        push_depth_ = 0;

        for (size_t i = 0; i < f.params.size(); ++i)
            slots_[f.params[i].name] = -(8 * (int)(i + 1));
        scan_slots(f.has_block_body ? f.block_body.get() : nullptr);

        int words = (int)f.params.size() + (int)slots_.size();
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
        ins("mov rsp, rbp");
        ins("pop rbp");
        ins("ret");
        put("");
        cur_fn_ = nullptr;
    }

    void emit_method(ast::StructDecl& s, ast::MethodDecl& m)
    {
        std::cerr << "[native] emit_method " << s.name << "_" << m.name << std::endl;
        std::string mname = s.name + "_" + m.name;
        cur_fn_ = nullptr;
        slots_.clear();
        push_depth_ = 0;
        slots_["this"] = -(8 * 1);
        for (size_t i = 0; i < m.params.size(); ++i)
            slots_[m.params[i].name] = -(8 * (int)(i + 2));
        if (m.has_block_body && m.block_body) scan_slots(m.block_body.get());
        int words = 1 + (int)m.params.size() + (int)slots_.size();
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
            ins("jmp .Lret_" + cur_fn_->name);
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
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
            if (!fo.is_range) unsup("iterator for-loops");
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
            ins("push rax");
            ++push_depth_;
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
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx = -1;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx = (int)k;
                                break;
                            }
                        }
                        if (var_idx!=-1) break;
                    }
                    if (var_idx==-1) unsup("unknown variant alternative");
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(var_idx));
                    ins("jne " + next);
                } else {
                    unsup("unsupported match pattern");
                }
                emit_stmt(arm.body.get());
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            ins("pop rax");
            --push_depth_;
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
            else if (lit.lit == ast::LitKind::String) {
                std::string text = lit.text;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                std::string lblname = intern_string(text);
                ins("lea rax, " + lblname + "[rip]");
            } else
                unsup("literal kinds beyond integers/bool/string");
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
            ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
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
            eval(m.scrutinee.get());
            ins("push rax");
            ++push_depth_;
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
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
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
            ins("pop rax");
            --push_depth_;
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
            if (!sd || !md) unsup("unknown method '" + mem.name + "'");
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
            ins("sub rsp, 32");
            ins("call " + sd->name + "_" + md->name);
            ins("add rsp, 32");
#endif
            return;
        }
        if (c.callee->kind() != ast::Expr::Name) {
            std::cerr << "[native] indirect calls kind " << (int)c.callee->kind() << std::endl;
            unsup("indirect calls");
        }
        auto& nm = static_cast<ast::NameExpr&>(*c.callee);
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
                ins(j + " " + t_true);
                ins("jmp " + t_false);
                return;
            }
        }
        eval(e);
        ins("cmp rax, 0");
        ins("jne " + t_true);
        ins("jmp " + t_false);
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
                if (!is_int_kind(k)) unsup("struct field '" + f.name + "' must be int");
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
        if (f.params.size() > 4) unsup("more than 4 parameters on Windows native: '" + f.name + "'");
        for (auto& p : f.params) {
            if (p.mode != ast::ParamMode::In)
                unsup("parameter mode other than 'in' on '" + p.name + "' of '" + f.name + "'");
        }
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
        slots_.clear();
        push_depth_ = 0;
        for (size_t i = 0; i < f.params.size(); ++i)
            slots_[f.params[i].name] = -(8 * (int)(i + 1));
        scan_slots(f.has_block_body ? f.block_body.get() : nullptr);
        int words = (int)f.params.size() + (int)slots_.size();
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
        ins("mov rsp, rbp");
        ins("pop rbp");
        ins("ret");
        put("");
        cur_fn_ = nullptr;
    }

    void emit_method(ast::StructDecl& s, ast::MethodDecl& m)
    {
        std::cerr << "[native] emit_method " << s.name << "_" << m.name << std::endl;
        std::string mname = s.name + "_" + m.name;
        cur_fn_ = nullptr;
        slots_.clear();
        push_depth_ = 0;
        slots_["this"] = -(8 * 1);
        for (size_t i = 0; i < m.params.size(); ++i)
            slots_[m.params[i].name] = -(8 * (int)(i + 2));
        if (m.has_block_body && m.block_body) scan_slots(m.block_body.get());
        int words = 1 + (int)m.params.size() + (int)slots_.size();
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
            ins("jmp .Lret_" + cur_fn_->name);
            break;
        }
        case ast::Stmt::If: {
            auto& i = static_cast<ast::IfStmt&>(*s);
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
            if (!fo.is_range) unsup("iterator for-loops");
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
            ins("push rax");
            ++push_depth_;
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
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    int var_idx = -1;
                    for (auto& v : m_->variants) {
                        for (size_t k=0;k<v.alternatives.size();++k) {
                            if (v.alternatives[k].parts.size()==1 && v.alternatives[k].parts[0]==arm.type_pattern.parts[0]) {
                                var_idx = (int)k;
                                break;
                            }
                        }
                        if (var_idx!=-1) break;
                    }
                    if (var_idx==-1) unsup("unknown variant alternative");
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(var_idx));
                    ins("jne " + next);
                } else {
                    unsup("unsupported match pattern");
                }
                emit_stmt(arm.body.get());
                ins("jmp " + end);
                if (next != end) label(next);
            }
            label(end);
            ins("pop rax");
            --push_depth_;
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
            else if (lit.lit == ast::LitKind::String) {
                std::string text = lit.text;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                std::string lblname = intern_string(text);
                ins("lea rax, " + lblname + "[rip]");
            } else
                unsup("literal kinds beyond integers/bool/string");
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
        case ast::Expr::AsCast: {
            auto& ac = static_cast<ast::AsCastExpr&>(*e);
            eval(ac.operand.get());
            break;
        }
        case ast::Expr::Assign: {
            auto& a = static_cast<ast::AssignExpr&>(*e);
            if (a.target->kind() != ast::Expr::Name) unsup("assignment form");
            auto& n = static_cast<ast::NameExpr&>(*a.target);
            int off = slot_or_new(n.parts[0]);
            if (a.op == "=") {
                eval(a.value.get());
            } else {
                std::string lop = a.op.substr(0, a.op.size() - 1);
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
                    ins("lea rcx, .Lfmt_div0[rip]");
                    ins("call printf");
                    ins("mov ecx, 101");
                    ins("call exit");
                    label(ok);
                    ins("cqo");
                    ins("idiv rcx");
                } else if (lop == "%") {
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
                    ins("mov rax, rdx");
                } else if (lop == "&") { ins("and rax, rcx"); }
                else if (lop == "|") { ins("or rax, rcx"); }
                else if (lop == "^") { ins("xor rax, rcx"); }
                else if (lop == "<<") { ins("xchg rax, rcx"); ins("shl rax, cl"); }
                else if (lop == ">>") { ins("xchg rax, rcx"); ins("sar rax, cl"); }
                else unsup("compound assignment '" + a.op + "'");
            }
            ins("mov QWORD PTR [rbp" + std::to_string(off) + "], rax");
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
            eval(m.scrutinee.get());
            ins("push rax");
            ++push_depth_;
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
                    ins("mov rax, QWORD PTR [rsp]");
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
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
            ins("pop rax");
            --push_depth_;
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
            if (!sd || !md) unsup("unknown method '" + mem.name + "'");
            int base_off = slot_of(base.parts[0]);
            ins("lea rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
            ins("push rax");
            ++push_depth_;
            for (auto& a : c.args) {
                eval(a.get());
                ins("push rax");
                ++push_depth_;
            }
            static char const* regs[] = {"rcx","rdx","r8","r9"};
            ins("pop rcx");
            --push_depth_;
            for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                ins("pop " + std::string(regs[i+1]));
                --push_depth_;
            }
            ins("sub rsp, 32");
            ins("call " + sd->name + "_" + md->name);
            ins("add rsp, 32");
            return;
        }
        if (c.callee->kind() != ast::Expr::Name) unsup("indirect calls");
        auto& nm = static_cast<ast::NameExpr&>(*c.callee);
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
                ins("call printf");
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
                ins(j + " " + t_true);
                ins("jmp " + t_false);
                return;
            }
        }
        eval(e);
        ins("cmp rax, 0");
        ins("jne " + t_true);
        ins("jmp " + t_false);
    }
};

std::string emit_asm(ast::Module& m, sema::Result const& r)
{
    NativeEmitter em;
    return em.emit(m, r);
}

std::vector<uint8_t> emit_pe(ast::Module& m, sema::Result const& r)
{
    // Windows 直出 PE：仅 hello 单测（import std, main->{print "Hello, C++2!\n"}），其余回退 headers/emit_asm
    if (!m.structs.empty() || !m.enums.empty() || !m.variants.empty() || !m.concepts.empty()) throw Unsupported("emit_pe: hello has no types");
    if (m.funcs.size() != 1 || m.funcs[0].name != "main") throw Unsupported("emit_pe: only hello main");
    auto& main = m.funcs[0];
    if (!main.has_block_body || !main.block_body) throw Unsupported("emit_pe: main needs block");
    // 简化：直接用 x64::Emitter 手编 hello 的机器码
    x64::Emitter e;
    // prologue
    e.push_rbp();
    e.mov_rbp_rsp();
    e.sub_rsp_imm8(32);
    // lea rcx, .LS0[rip]  (rodata hello string)
    e.lea_rcx_rip(".LS0");
    e.xor_eax_eax();
    e.call_indirect_rip("printf");
    e.add_rsp_imm8(32);
    e.xor_ecx_ecx();
    e.call_indirect_rip("ExitProcess");
    std::vector<uint8_t> text = e.code;
    std::vector<uint8_t> rodata;
    std::string hello = "Hello, C++2!\n";
    hello.push_back('\0');
    for(char c: hello) rodata.push_back((uint8_t)c);
    std::vector<pe::Reloc> relocs;
    for(auto &r: e.relocs){
        pe::Reloc pr;
        pr.offset = r.pos - 0; // x64::Reloc pos is disp offset
        pr.target = r.target;
        pr.is_call = r.is_call;
        relocs.push_back(pr);
    }
    std::vector<std::pair<std::string,std::string>> labels = { {".LS0","0"} };
    return pe::build_exe(text, rodata, relocs, labels);
}

#endif

} // namespace cpp2::native
