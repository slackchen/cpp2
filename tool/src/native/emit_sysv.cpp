// emit_sysv.cpp — native 后端,x86-64 System V(Linux)汇编发射(自 native.cpp 拆出)
// 设计:栈机风格——表达式值经 rax 传递,中间量压栈;无寄存器分配。
// 槽位:参数与局部变量统一扁平分配 [rbp-8k];帧大小对齐 16。
// 平台:仅 SysV(Linux);Windows 构建的工具走 emit_win64.cpp(Win64 直出 PE)。
#include "emit_base.hpp"
#include "x64.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>

namespace cpp2::native {
#ifdef CPP2_NATIVE_HOST_OK
class NativeEmitter : public EmitterBase {
public:

private:

    // printf 格式符按静态类型选择(string 在 native v0 = NUL 结尾 char 指针)

    // 静态类型是否 string(字面量直接判定;其余查 sema)—— 用于 + 拼接判定

    // inout/out 参数集合(当前函数):这些名字的槽存实参地址,读写须间接


    // 字段偏移沿基类链累计(基类字段居低地址,与 C++ 布局同构);未找到返回 -1

    // 全字段视图(基类在前),StructLit 初始化用;second = 字节偏移
    std::vector<std::pair<ast::FieldDecl*, int>> fields_deep(ast::StructDecl* sd)
    {
        std::vector<std::pair<ast::FieldDecl*, int>> out;
        std::vector<ast::StructDecl*> chain;
        for (std::string bn = (sd->base && !sd->base->parts.empty()) ? sd->base->parts[0] : "";
             !bn.empty();) {
            ast::StructDecl* bs = find_sd(bn);
            if (!bs) break;
            chain.push_back(bs);
            bn = (bs->base && !bs->base->parts.empty()) ? bs->base->parts[0] : "";
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            for (auto& f : (*it)->fields)
                out.push_back({&f, (int)out.size() * 8});
        for (auto& f : sd->fields)
            out.push_back({&f, (int)out.size() * 8});
        return out;
    }

    void emit_rodata_strs()
    {
        if (!str_pool_.empty()) {
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
        // div0 trap 消息(供 checked div 使用)
        out_ << ".section .rodata\n";
        out_ << ".Lfmt_div0:\n";
        out_ << "    .string \"division by zero\\n\"\n";
        out_ << ".text\n";
        // 全局错误串槽(err() 写 / e.message() 读),始终存在
        out_ << ".data\n";
        out_ << ".Lerrmsg:\n";
        out_ << "    .quad 0\n";
        // 错误类别/因果链槽(M7):err(msg, cat) 写 .Lerrcat;err_caused 写 .Lerrcause
        out_ << ".Lerrcat:\n";
        out_ << "    .quad 0\n";
        out_ << ".Lerrcause:\n";
        out_ << "    .quad 0\n";
        out_ << ".text\n";
    }

    // ── 预检 ────────────────────────────────────────────────────

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
            // struct 变量:预留连续字段块(字段 i 位于 base + i*8,base = 块内最低槽)
            {
                std::string tn;
                if (v.has_type && v.type.parts.size() == 1) tn = v.type.parts[0];
                else if (v.init && v.init->kind() == ast::Expr::StructLit &&
                         !static_cast<ast::StructLitExpr&>(*v.init).type_parts.empty())
                    tn = static_cast<ast::StructLitExpr&>(*v.init).type_parts[0];
                ast::StructDecl* sd = tn.empty() ? nullptr : find_sd(tn);
                if (sd) {
                    int n = (int)fields_deep(sd).size();
                    for (int i = 1; i < n; ++i)
                        slot_or_new(v.name + "#f" + std::to_string(i));
                    slot_or_new(v.name);
                    break;
                }
            }
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
        cur_struct_ = nullptr;
        scope_stack_.clear();
        slots_.clear();
        push_depth_ = 0;
        record_inout_params(f.params);

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
        if (f.name == "main" && !(f.ret && !f.ret->empty()))
            ins("xor eax, eax");   // 隐式 return 0
        // SysV:正常 leave/ret,退出码经 rax 交还 CRT(含 stdio flush);ExitProcess 收尾是 Win64 直出 PE 专属
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
        cur_sym_name_ = mname;
        cur_struct_ = &s;
        scope_stack_.clear();
        slots_.clear();
        push_depth_ = 0;
        record_inout_params(m.params);
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
    // 作用域析构栈:每层 block 一个帧,记录 (变量名, 声明析构器的 struct)

    void emit_dtor_call(DtorEntry const& vd)
    {
        int off = slot_of(vd.first);
        ins("lea rax, QWORD PTR [rbp" + std::to_string(off) + "]");
        ins("push rax");
        ++push_depth_;
        ins("pop rdi");
        --push_depth_;
        if (push_depth_ % 2 == 1) ins("sub rsp, 8");
        ins("call " + vd.second->name + "_destructor");
        if (push_depth_ % 2 == 1) ins("add rsp, 8");
    }

    // return 离开全部作用域:自内向外调用所有未析构的 struct 局部
    // skip:被返回的局部(所有权移交给调用方),不在此处析构

    void emit_stmt(ast::Stmt* s)
    {
        switch (s->kind()) {
        case ast::Stmt::Block: {
            auto& blk = *static_cast<ast::BlockStmt*>(s);
            // 预扫描本块直接声明的、带析构器的 struct 局部变量(DESIGN §5.3)
            scope_stack_.emplace_back();
            for (auto& st : blk.stmts) {
                if (st->kind() != ast::Stmt::Var) continue;
                auto& v = *static_cast<ast::VarStmt*>(st.get());
                std::string tn;
                if (v.has_type && !v.type.parts.empty()) tn = v.type.parts[0];
                else if (R_ && v.init) {
                    auto vt = R_->type_of(*v.init);
                    if (vt.kind == sema::Type::Kind::NamedStruct) tn = vt.name;
                }
                if (tn.empty()) continue;
                ast::StructDecl* tsd = find_sd(tn);
                if (!tsd) continue;
                ast::StructDecl* dsd = nullptr;
                for (ast::StructDecl* s2 = tsd; s2 && !dsd;) {
                    for (auto& m2 : s2->methods) if (m2.name == "destructor") { dsd = s2; break; }
                    if (!dsd)
                        s2 = (s2->base && !s2->base->parts.empty()) ? find_sd(s2->base->parts[0]) : nullptr;
                }
                if (dsd) scope_stack_.back().push_back({v.name, dsd});
            }
            for (auto& st : blk.stmts) emit_stmt(st.get());
            {
                auto& fr = scope_stack_.back();
                for (auto it = fr.rbegin(); it != fr.rend(); ++it) emit_dtor_call(*it);
            }
            scope_stack_.pop_back();
            break;
        }
        case ast::Stmt::ExprStmt:
            eval(static_cast<ast::ExprStmt*>(s)->expr.get());
            break;
        case ast::Stmt::Var: {
            auto& v = static_cast<ast::VarStmt&>(*s);
            // 特殊处理 Point 等 struct 的 StructLit 初始化:字段 i 存入 base + i*8
            // (base 槽已由 scan_slots 预留为块内最低槽,与 field_offset_in 布局一致)
            if (v.init && v.init->kind() == ast::Expr::StructLit) {
                auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
                ast::StructDecl* sd = nullptr;
                for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
                if (sd) {
                    int base_off = slot_or_new(v.name);
                    auto fds = fields_deep(sd);
                    for (size_t i=0;i<fds.size();++i) {
                        bool found = false;
                        for (auto& pr : sl.fields) if (pr.first == fds[i].first->name) {
                            eval(pr.second.get());
                            found = true;
                            break;
                        }
                        if (!found) eval(fds[i].first->init.get());
                        ins("mov QWORD PTR [rbp" + std::to_string(base_off + fds[i].second) + "], rax");
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
                    {
                        auto fds = fields_deep(sd);
                        for (size_t i=0;i<fds.size();++i) {
                            bool found = false;
                            for (auto& pr : ((ast::StructLitExpr&)*v.init).fields)
                                if (pr.first == fds[i].first->name) {
                                    eval(pr.second.get());
                                    found = true;
                                    break;
                                }
                            if (!found) eval(fds[i].first->init.get());
                            ins("mov QWORD PTR [rbp" + std::to_string(data_off + (int)i*8) + "], rax");
                        }
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
            // 被返回的具名局部移交给调用方,返回路径跳过其析构
            std::string skip;
            if (r.value && r.value->kind() == ast::Expr::Name)
                skip = static_cast<ast::NameExpr&>(*r.value).parts[0];
            // 析构调用会破坏 rax(caller-saved),返回值先压栈
            bool has_dtors = false;
            for (auto& f : scope_stack_) if (!f.empty()) { has_dtors = true; break; }
            if (has_dtors) { ins("push rax"); ++push_depth_; }
            emit_all_dtors_for_return(skip);
            if (has_dtors) { ins("pop rax"); --push_depth_; }
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
                // 整数字面量列表迭代:for id in {a, b, c} → 元素存入连续临时槽,按索引循环
                if (fo.iterable && fo.iterable->kind() == ast::Expr::ListLit) {
                    auto& ll = static_cast<ast::ListLitExpr&>(*fo.iterable);
                    for (auto& el : ll.elements)
                        if (el->kind() != ast::Expr::Literal)
                            unsup("iterator for-loops: only literal int lists v0");
                    int n = (int)ll.elements.size();
                    std::string base_name = fo.var + "#lst";
                    for (int k = 1; k < n; ++k)            // 占位:保证元素槽连续
                        slot_or_new(base_name + std::to_string(k));
                    int base_off = slot_or_new(base_name); // 元素 k 位于 base + k*8
                    for (int k = 0; k < n; ++k) {
                        eval(ll.elements[k].get());
                        ins("mov QWORD PTR [rbp" + std::to_string(base_off + k * 8) + "], rax");
                    }
                    int ioff = slot_or_new(fo.var + "#i");
                    int voff = slot_or_new(fo.var);
                    ins("xor eax, eax");
                    ins("mov QWORD PTR [rbp" + std::to_string(ioff) + "], rax");
                    std::string top = lbl("fortop"), inc = lbl("forinc"), fend = lbl("fend");
                    label(top);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                    ins("cmp rax, " + std::to_string(n));
                    ins("jge " + fend);
                    break_labels_.push_back(fend);
                    continue_labels_.push_back(inc);
                    // var = lst[i]
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                    ins("shl rax, 3");
                    ins("lea rcx, QWORD PTR [rbp" + std::to_string(base_off) + "]");
                    ins("add rax, rcx");
                    ins("mov rax, QWORD PTR [rax]");
                    ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rax");
                    emit_stmt(fo.body.get());
                    label(inc);
                    ins("inc QWORD PTR [rbp" + std::to_string(ioff) + "]");
                    ins("jmp " + top);
                    label(fend);
                    break_labels_.pop_back();
                    continue_labels_.pop_back();
                    break;
                }
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
                if (cur_struct_) {
                    field_off = field_offset_in(cur_struct_, n.parts[0]);
                    is_field = field_off != -1;
                }
                if (is_field && cur_fn_ == nullptr) {
                    ins("mov rax, QWORD PTR [rbp-8]");
                    ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                } else {
                    int voff = slot_or_new(n.parts[0]);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(voff) + "]");
                    if (is_inout_param(n.parts[0])) ins("mov rax, QWORD PTR [rax]");
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
            else if (u.op == "move") { /* 所有权转移:值表示不变,恒等 */ }
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
            // 方法上下文(cur_fn_==nullptr)且名字是字段 → 经 this 指针读写
            bool fld = false;
            int fld_off = -1;
            if (cur_struct_) {
                fld_off = field_offset_in(cur_struct_, n.parts[0]);
                fld = fld_off != -1;
            }
            int off;
            bool via_this;
            if (fld && cur_fn_ == nullptr) {
                off = 0;                          // 占位:实际经 this
                via_this = true;
                ins("mov rax, QWORD PTR [rbp-8]");   // this
                ins("add rax, " + std::to_string(fld_off));
                ins("push rax");
                ++push_depth_;                    // 栈顶 = 字段地址
            } else {
                off = slot_or_new(n.parts[0]);
                via_this = false;
            }
            if (a.op == "=") {
                eval(a.value.get());
            } else {
                // compound assignment: desugar lhs op rhs -> rax
                std::string lop = a.op.substr(0, a.op.size() - 1); // "+=" -> "+"
                // lhs -> rcx, rhs -> rax, then compute
                if (via_this) {
                    // 字段复合赋值:栈顶已存字段地址;复制一份供读取
                    ins("mov rcx, QWORD PTR [rsp]");   // addr 副本(rcx 暂存)
                    ins("mov rax, QWORD PTR [rcx]");   // lhs = *addr
                } else {
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(off) + "]");
                    if (is_inout_param(n.parts[0])) ins("mov rax, QWORD PTR [rax]");
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
            } else if (is_inout_param(n.parts[0])) {
                ins("mov rcx, QWORD PTR [rbp" + std::to_string(off) + "]");   // 实参地址
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
            if (R_) {
                auto bt = R_->type_of(*m.base);
                if (bt.kind == sema::Type::Kind::NamedStruct) {
                    if (ast::StructDecl* rsd = find_sd(bt.name))
                        field_off = field_offset_in(rsd, m.name);
                }
            }
            if (field_off == -1) {
                for (auto& s : m_->structs) {
                    field_off = field_offset_in(&s, m.name);
                    if (field_off != -1) break;
                }
            }
            if (field_off == -1 && m.name == "category") {
                // err 绑定(哨兵 0)的类别:M7,经全局槽读取
                ins("lea rax, .Lerrcat[rip]");
                ins("mov rax, QWORD PTR [rax]");
                return;
            }
            if (field_off == -1) unsup("unknown field '" + m.name + "'");
            int base_off = slot_of(base.parts[0]);
            if (is_inout_param(base.parts[0])) {
                ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");   // 实参地址
                ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
            } else {
                ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off + field_off) + "]");
            }
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
            {
                auto fds = fields_deep(sd);
                for (size_t i=0;i<fds.size();++i) {
                    bool found = false;
                    for (auto& pr : sl.fields) if (pr.first == fds[i].first->name) {
                        eval(pr.second.get());
                        found = true;
                        break;
                    }
                    if (!found) eval(fds[i].first->init.get());
                    ins("mov QWORD PTR [rbp" + std::to_string(tmp_off + (int)fds[i].second) + "], rax");
                }
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
            // 接收者静态类型优先沿继承链解析(派生方法隐藏基类同名)
            if (R_) {
                auto bt = R_->type_of(*mem.base);
                if (bt.kind == sema::Type::Kind::NamedStruct) {
                    ast::StructDecl* s = find_sd(bt.name);
                    while (s && !md) {
                        for (auto& m : s->methods)
                            if (m.name == mem.name) { sd = s; md = &m; break; }
                        if (!md)
                            s = (s->base && !s->base->parts.empty()) ? find_sd(s->base->parts[0]) : nullptr;
                    }
                }
            }
            if (!sd || !md) {
                for (auto& s : m_->structs) {
                    for (auto& m : s.methods) if (m.name == mem.name) { sd = &s; md = &m; break; }
                    if (sd) break;
                }
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
                    // v1:错误串由 err() 写入全局槽,e.message() 读出
                    ins("lea rax, .Lerrmsg[rip]");
                    ins("mov rax, QWORD PTR [rax]");
                    return;
                }
                if (mem.name == "chain") {
                    // v1 因果链:message + (cause 非空 ? "\n  caused by: " + cause : "")
                    ins("lea rax, .Lerrcause[rip]");
                    ins("mov rax, QWORD PTR [rax]");
                    ins("cmp byte PTR [rax], 0");
                    std::string nc = lbl("nochain"), ce = lbl("chainend");
                    std::string midlbl = intern_string("\n  caused by: ");
                    ins("je " + nc);
                    ins("lea rax, .Lerrmsg[rip]");
                    ins("mov rax, QWORD PTR [rax]");
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
                    ins("lea rsi, " + midlbl + "[rip]");
#else
                    ins("mov rcx, rax");
                    ins("lea rdx, " + midlbl + "[rip]");
#endif
                    ins("call cpp2_strcat");
#ifdef CPP2_NATIVE_HOST_OK
                    ins("mov rdi, rax");
                    ins("lea rsi, .Lerrcause[rip]");
                    ins("mov rsi, QWORD PTR [rsi]");
#else
                    ins("mov rcx, rax");
                    ins("lea rdx, .Lerrcause[rip]");
                    ins("mov rdx, QWORD PTR [rdx]");
#endif
                    ins("call cpp2_strcat");
                    ins("jmp " + ce);
                    label(nc);
                    ins("lea rax, .Lerrmsg[rip]");
                    ins("mov rax, QWORD PTR [rax]");
                    label(ce);
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
            if (bn == "err") {
                // v1:错误串存入全局槽 .Lerrmsg;返回 0 哨兵。
                // 与参考实现一致,编译期拼接来源位置后缀" (路径:行号)"
                if (!c.args.empty()) {
                    eval(c.args[0].get());
                    if (!src_path_.empty()) {
                        std::string suffix = " (" + src_path_ + ":" + std::to_string(c.line) + ")";
                        std::string slbl = intern_string(suffix);
#ifdef CPP2_NATIVE_HOST_OK
                        ins("mov rdi, rax");
                        ins("lea rsi, " + slbl + "[rip]");
                        ins("call cpp2_strcat");
#else
                        ins("mov rcx, rax");
                        ins("lea rdx, " + slbl + "[rip]");
                        ins("call cpp2_strcat");
#endif
                    }
#ifdef CPP2_NATIVE_HOST_OK
                    ins("lea rdi, .Lerrmsg[rip]");
                    ins("mov QWORD PTR [rdi], rax");
#else
                    ins("lea rcx, .Lerrmsg[rip]");
                    ins("mov QWORD PTR [rcx], rax");
#endif
                }
                ins("xor eax, eax");  // 语言级哨兵
                return;
            }
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

        // inout 实参传地址(裸名字且槽存在);其余传值
        ast::FuncDecl* c2_callee = nullptr;
        for (auto& f : m_->funcs)
            if (f.name == nm.parts[0] && f.params.size() == c.args.size()) { c2_callee = &f; break; }
        for (size_t i = 0; i < c.args.size(); ++i) {
            auto& a = c.args[i];
            bool pass_addr = c2_callee && (c2_callee->params[i].mode == ast::ParamMode::Inout || c2_callee->params[i].mode == ast::ParamMode::Out)
                          && a->kind() == ast::Expr::Name;
            if (pass_addr) {
                auto& an = static_cast<ast::NameExpr&>(*a);
                int aoff = slot_of(an.parts[0]);
                ins("lea rax, QWORD PTR [rbp" + std::to_string(aoff) + "]");
            } else {
                eval(a.get());
            }
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
                int idx = text[pos + 1] - '0';
                if (idx + 1 >= (int)c.args.size())
                    unsup("println placeholder {" + std::string(1, text[pos + 1]) + "} out of range");
                cfmt += fmt_for(c.args[idx + 1].get());
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

        if (b.op == "+") {
            if (is_string_expr(b.lhs.get())) {
                ins("mov rdi, rcx");
                ins("mov rsi, rax");
                ins("call cpp2_strcat");
                return;
            }
            ins("add rax, rcx"); return;
        }
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

std::string emit_asm(ast::Module& m, sema::Result const& r, std::string const& src_path)
{
    NativeEmitter em;
    return em.emit(m, r, src_path);
}
#endif


} // namespace cpp2::native
