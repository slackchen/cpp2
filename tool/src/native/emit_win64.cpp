// emit_win64.cpp — native 后端,x86-64 Win64 直出 PE(自 native.cpp 拆出)
// 设计:栈机风格同 SysV;调用约定按 Win64(整型 rcx/rdx/r8/r9,double xmm0-3,
//       varargs 走 GP 流 + AL=使用的 xmm 数,详见各 emit_call 注释)。
// 产物:asm64 汇编 → 内置汇编器 → PE(pe.hpp),零外部工具。
#include "emit_base.hpp"
#include "x64.hpp"
#include "asm64.hpp"
#include "pe.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>

namespace cpp2::native {
#ifdef CPP2_NATIVE_WIN
class NativeEmitter : public EmitterBase {
public:
public:
    void emit_pending_lambdas() override
    {
        while (!pending_lambdas_.empty()) {
            auto pl = pending_lambdas_.front();
            pending_lambdas_.erase(pending_lambdas_.begin());
            emit_lambda(pl.first, pl.second);
        }
    }

    char const* dbl_fmt() const override { return "%s"; }  // 位模式经 cpp2_dbl_str 转字符串

    std::string emit(ast::Module& m, sema::Result const& r, std::string const& src_path = {})
    {
        std::cerr << "[native] emit for module " << m.name << " funcs " << m.funcs.size() << " structs " << m.structs.size() << std::endl;
        m_ = &m;
        R_ = &r;
        src_path_ = src_path;
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
                emit_method(s, md);
            }
        }
        while (!pending_lambdas_.empty()) {
            auto pl = pending_lambdas_.front();
            pending_lambdas_.erase(pending_lambdas_.begin());
            emit_lambda(pl.first, pl.second);
        }
        emit_rodata_strs();
        return out_.str();
    }

private:
    // 闭包(λ)v0 状态:显式按值捕获,env = [fn_ptr][cap0][cap1]...
    struct LambdaInfo { std::string label; std::vector<std::string> caps; };
    std::map<std::string, LambdaInfo> lambda_vars_;             // 变量名 → 闭包
    std::vector<std::pair<std::string, ast::LambdaExpr*>> pending_lambdas_;
    LambdaInfo last_lambda_;                                    // 最近一次 eval(Lambda)
    bool in_lambda_ = false;                                    // 正在发射 lambda 体
    int lambda_ctx_env_ = 0;                                    // 体内 env 所在槽
    std::map<std::string, int> lambda_caps_;                    // 捕获名 → env 索引

    [[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

    sema::Type type_of(ast::Expr* e) const { return R_ ? R_->type_of(*e) : sema::Type{}; }

    // printf 格式符按静态类型选择(string 在 native v0 = NUL 结尾 char 指针)
    char const* fmt_for(ast::Expr* e)
    {
        if (!R_ || !e) return "%lld";
        // std 桥:e.message()/e.chain() 返回错误 C 串 → %s
        if (e->kind() == ast::Expr::Call) {
            auto& c = static_cast<ast::CallExpr&>(*e);
            if (c.callee->kind() == ast::Expr::Member) {
                auto const& mn = static_cast<ast::MemberExpr&>(*c.callee).name;
                if (mn == "message" || mn == "chain") return "%s";
            }
        }
        auto t = R_->type_of(*e);
        using K = sema::Type::Kind;
        switch (t.kind) {
        case K::String: case K::StringView: return "%s";
        case K::Char:   return "%c";
        case K::Double: case K::Float: return "%s";  // 位模式经 cpp2_dbl_str 转字符串
        case K::Bool:   return "%d";                 // cout << bool 语义(格式串路径另转 true/false)
        default:        return "%lld";
        }
    }

    // 静态类型是否 string(字面量直接判定;其余查 sema)—— 用于 + 拼接判定
    bool is_string_expr(ast::Expr* e)
    {
        if (!e) return false;
        if (auto* lit = dynamic_cast<ast::LiteralExpr*>(e))
            if (lit->lit == ast::LitKind::String) return true;
        if (!R_) return false;
        auto t = R_->type_of(*e);
        using K = sema::Type::Kind;
        return t.kind == K::String || t.kind == K::StringView;
    }

    // inout/out 参数集合(当前函数):这些名字的槽存实参地址,读写须间接
    bool is_inout_param(std::string const& n) const { return inout_params_.count(n) != 0; }
    void record_inout_params(std::vector<ast::Param>& params)
    {
        inout_params_.clear();
        for (auto& p : params)
            if (p.mode == ast::ParamMode::Inout || p.mode == ast::ParamMode::Out) inout_params_.insert(p.name);
    }

    // ── 虚分发(M7):对象首槽 = vptr,虚表槽序 = 根→派生声明序(同名覆盖)──
    int vptr_pad(ast::StructDecl const* sd) const override
    {
        return has_vtable(sd) ? 8 : 0;
    }

    // 虚表槽位表:(方法名, 实现函数名),按根→派生声明序;派生同名方法覆盖基类槽位
    std::vector<std::pair<std::string, std::string>> vtable_of(ast::StructDecl* sd)
    {
        std::vector<std::pair<std::string, std::string>> tbl;
        std::vector<ast::StructDecl*> chain;
        for (ast::StructDecl* s = sd; s;) {
            chain.push_back(s);
            s = (s->base && !s->base->parts.empty()) ? find_sd(s->base->parts[0]) : nullptr;
        }
        for (size_t ci = chain.size(); ci-- > 0;) {           // 根在前
            for (auto& m : chain[ci]->methods) {
                size_t k = 0;
                for (; k < tbl.size(); ++k)
                    if (tbl[k].first == m.name) break;
                if (k < tbl.size()) {                         // 覆盖基类槽位(once virtual, always virtual)
                    tbl[k].second = chain[ci]->name + "_" + m.name;
                    continue;
                }
                if (!m.is_virtual) continue;                  // 新增且非虚 → 不入表
                tbl.push_back({m.name, chain[ci]->name + "_" + m.name});
            }
        }
        return tbl;
    }

    // 静态类型的虚表槽位;非虚(或无虚表)返回 -1
    int vtable_slot(ast::StructDecl* sd, std::string const& mname)
    {
        if (!has_vtable(sd)) return -1;
        auto tbl = vtable_of(sd);
        for (size_t k = 0; k < tbl.size(); ++k)
            if (tbl[k].first == mname) return (int)k;
        return -1;
    }

    // 构造点写入 vptr:对象首槽 ← 全局表指针槽(.Lvt_<Type>,启动时填充)
    void store_vptr(int obj_off, ast::StructDecl* sd)
    {
        ins("lea r10, .Lvt_" + sd->name + "[rip]");
        ins("mov r10, QWORD PTR [r10]");
        ins("mov QWORD PTR [rbp" + std::to_string(obj_off) + "], r10");
    }

    // ── vector v0:堆块 [count][e0][e1]...,元素槽宽随元素类型 ──
    int elem_slots(sema::Type const& t)
    {
        using K = sema::Type::Kind;
        if (t.kind == K::Variant) return 1 + max_alt_fields(t.name);
        if (t.kind == K::NamedStruct)
            if (ast::StructDecl* sd = find_sd(t.name))
                return (int)fields_deep(sd).size() + (vptr_pad(sd) ? 1 : 0);
        return 1;
    }
    int elem_stride(sema::Type const& t) { return elem_slots(t) * 8; }

    bool is_bool_expr(ast::Expr* e) const
    {
        return R_ && e && R_->type_of(*e).kind == sema::Type::Kind::Bool;
    }

    bool has_any_vtables() const
    {
        for (auto& s : m_->structs)
            if (has_vtable(&s)) return true;
        return false;
    }

    // push_back:分配 (n+2) 槽新块,拷贝头+旧元素,追加,回写变量槽
    void emit_vector_push_back(std::string const& vec_name, ast::CallExpr& c)
    {
        int vec_off = slot_of(vec_name);
        int uid = label_++;
        int val_off = slot_or_new("$pbv" + std::to_string(uid));
        int n_off   = slot_or_new("$pbn" + std::to_string(uid));
        int new_off = slot_or_new("$pbw" + std::to_string(uid));
        if (c.args.size() != 1) unsup("push_back needs exactly 1 argument");
        eval(c.args[0].get());
        ins("mov QWORD PTR [rbp" + std::to_string(val_off) + "], rax");
        ins("mov rcx, QWORD PTR [rbp" + std::to_string(vec_off) + "]");
        ins("mov rdx, QWORD PTR [rcx]");                              // n
        ins("mov QWORD PTR [rbp" + std::to_string(n_off) + "], rdx");
        ins("add rdx, 2");
        ins("shl rdx, 3");                                            // (n+2)*8 字节
        ins("mov rcx, rdx");
        ins("call cpp2_alloc");
        ins("mov QWORD PTR [rbp" + std::to_string(new_off) + "], rax");
        ins("mov rcx, rax");                                          // dst
        ins("mov rdx, QWORD PTR [rbp" + std::to_string(vec_off) + "]"); // src
        ins("mov r8, QWORD PTR [rbp" + std::to_string(n_off) + "]");
        ins("add r8, 1");
        ins("shl r8, 3");                                             // (n+1)*8 = 头+旧元素
        ins("call cpp2_memcopy");
        ins("mov rcx, QWORD PTR [rbp" + std::to_string(new_off) + "]");
        ins("mov r8, QWORD PTR [rbp" + std::to_string(n_off) + "]");
        ins("add r8, 1");
        ins("mov QWORD PTR [rcx], r8");                               // 新 count
        ins("shl r8, 3");                                             // 追加位字节偏移
        ins("mov rax, QWORD PTR [rbp" + std::to_string(val_off) + "]");
        ins("mov QWORD PTR [rcx+r8], rax");
        ins("mov QWORD PTR [rbp" + std::to_string(vec_off) + "], rcx");
    }

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
        // double 字面量常量槽(.quad 位模式,按名解析)
        for (auto& dc : dbl_consts_) {
            out_ << dc.first << ":\n";
            out_ << "    .quad " << (long long)dc.second << "\n";
        }
        // cpp2_dbl_str 运行时数据:轮转计数 + 4×32B 缓冲(32 个 8B 标签按名序连续)
        out_ << ".Ldblcnt:\n";
        out_ << "    .quad 0\n";
        for (int i = 0; i < 32; ++i) {
            std::string bnm = ".Ldblbuf_" + std::string(i < 10 ? "0" : "") + std::to_string(i);
            out_ << bnm << ":\n";
            out_ << "    .quad 0\n";
        }
        // cpp2_int_str 运行时数据:轮转计数 + 4×32B 缓冲(同 dbl 布局)
        out_ << ".Lintcnt:\n";
        out_ << "    .quad 0\n";
        for (int i = 0; i < 32; ++i) {
            std::string bnm = ".Lintbuf_" + std::string(i < 10 ? "0" : "") + std::to_string(i);
            out_ << bnm << ":\n";
            out_ << "    .quad 0\n";
        }
        out_ << ".text\n";
    }

    void check_func(ast::FuncDecl& f)
    {
        // throws:expected<R> 语义 v0 简化为值返回(err() 调用点 unsup)
        if (!f.type_params.empty()) unsup("generic functions");
        int regslots = 0;
        for (auto& p : f.params) {
            if (is_variant_type(p.type)) regslots += 1 + max_alt_fields(p.type.parts[0]);
            else regslots += 1;
        }
        if (regslots > 4) unsup("more than 4 register slots on Windows native: '" + f.name + "'");
    }

    // variant 按值 = tag+data 两个寄存器槽(Win64 布局,与 emit_func/emit_call 一致)
    bool is_variant_type(ast::TypeUse const& t) const
    {
        if (t.parts.size() != 1) return false;
        for (auto& v : m_->variants) if (v.name == t.parts[0]) return true;
        return false;
    }

    // double 静态判定(位模式表示:槽/rax 恒存 IEEE754 位)
    bool is_double_expr(ast::Expr* e) const
    {
        return R_ && e && R_->type_of(*e).kind == sema::Type::Kind::Double;
    }
    bool is_double_field(ast::FieldDecl* fd) const
    {
        return fd && !fd->type.parts.empty() && fd->type.parts[0] == "double";
    }
    // 字段初始化求值:double 字段的整型初始值经 cvtsi2sd 转位模式
    void eval_field_init(ast::Expr* val, ast::FieldDecl* fd)
    {
        eval(val);
        if (is_double_field(fd) && val && !is_double_expr(val)) {
            ins("movq xmm0, rax");
            ins("cvtsi2sd xmm0, rax");
            ins("movq rax, xmm0");
        }
    }
    // double 常量入 .data(.quad 位模式),返回标签
    std::vector<std::pair<std::string, unsigned long long>> dbl_consts_;
    std::string intern_double_bits(unsigned long long bits)
    {
        std::string name = ".Ldbl_" + std::to_string(label_++);
        dbl_consts_.push_back({name, bits});
        return name;
    }
    // variant 数据区槽数 = 最大候选字段数(≥1;字段 i 地址 = 数据槽 + i*8)
    int max_alt_fields(std::string const& vname)
    {
        int K = 1;
        for (auto& vd : m_->variants) {
            if (vd.name != vname) continue;
            for (auto& alt : vd.alternatives) {
                if (alt.parts.size() != 1) continue;
                for (auto& s2 : m_->structs) {
                    if (s2.name != alt.parts[0]) continue;
                    int n = (int)fields_deep(&s2).size();
                    if (n > K) K = n;
                }
            }
        }
        return K;
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
        // 虚表(M7):每类型一个全局指针槽(.Lvt_X,启动经 cpp2_vtinit 填充);
        // 槽位表由发射期静态生成,表体运行期经 cpp2_alloc 分配后写入函数地址
        std::vector<ast::StructDecl*> vt;
        for (auto& s : m_->structs)
            if (has_vtable(&s)) vt.push_back(&s);
        if (!vt.empty()) {
            if (!any) { put(".data"); any = true; }
            for (auto* sd : vt) {
                put(".Lvt_" + sd->name + ":");
                put("    .quad 0");
            }
            put(".text");
            put(".globl cpp2_vtinit");
            put("cpp2_vtinit:");
            for (auto* sd : vt) {
                auto tbl = vtable_of(sd);
                ins("mov rcx, " + std::to_string(tbl.size() * 8));
                ins("call cpp2_alloc");
                for (size_t k = 0; k < tbl.size(); ++k) {
                    ins("lea rdx, " + tbl[k].second + "[rip]");
                    ins("mov QWORD PTR [rax+" + std::to_string(k * 8) + "], rdx");
                }
                ins("lea rdx, .Lvt_" + sd->name + "[rip]");
                ins("mov QWORD PTR [rdx], rax");
            }
            ins("ret");
        }
    }

    int slot_of(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it == slots_.end()) unsup("name '" + n + "' is not a local/parameter");
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
            // variant:数据字段槽(数据槽上方连续)必须先于数据槽分配
            {
                bool is_variant = false;
                if (v.has_type && v.type.parts.size()==1) {
                    for (auto& vd : m_->variants) {
                        if (vd.name != v.type.parts[0]) continue;
                        is_variant = true;
                        int K = max_alt_fields(v.type.parts[0]);
                        for (int d = K - 1; d >= 1; --d)
                            slot_or_new(v.name + "#d" + std::to_string(d));
                        break;
                    }
                }
                if (is_variant) { slot_or_new(v.name); break; }
            }
            // struct 变量:预留连续字段块(字段 i 位于 base + i*8,base = 块内最低槽)。
            // 占位槽仅保证连续性,字段实际地址一律按 base 偏移计算
            {
                std::string tn;
                if (v.has_type && v.type.parts.size() == 1) tn = v.type.parts[0];
                else if (v.init && v.init->kind() == ast::Expr::StructLit &&
                         !static_cast<ast::StructLitExpr&>(*v.init).type_parts.empty())
                    tn = static_cast<ast::StructLitExpr&>(*v.init).type_parts[0];
                ast::StructDecl* sd = nullptr;
                if (!tn.empty()) sd = find_sd(tn);
                if (sd) {
                    int n = (int)fields_deep(sd).size() + (vptr_pad(sd) ? 1 : 0);
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

    void emit_func(ast::FuncDecl& f)
    {
        cur_fn_ = &f;
        cur_sym_name_ = f.name;
        cur_struct_ = nullptr;
        scope_stack_.clear();
        slots_.clear();
        push_depth_ = 0;
        record_inout_params(f.params);
        // 参数槽位 + 寄存器布局:variant 参数占 2 寄存器(rcx=tag, rdx=data)
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        std::vector<std::string> param_stores;
        {
            int r = 0;
            for (size_t i = 0; i < f.params.size(); ++i) {
                // variant:数据字段 1..K-1 的槽(地址=数据槽+i*8)必须先于数据槽分配,tag 最后
                int vk = 1;
                if (is_variant_type(f.params[i].type)) {
                    vk = max_alt_fields(f.params[i].type.parts[0]);
                    for (int d = vk - 1; d >= 1; --d)
                        slot_or_new(f.params[i].name + "#d" + std::to_string(d));
                }
                slots_[f.params[i].name] = slot_or_new(f.params[i].name);
                if (is_variant_type(f.params[i].type)) {
                    slots_[f.params[i].name + "#tag"] = slot_or_new(f.params[i].name + "#tag");
                    param_stores.push_back(std::string("mov QWORD PTR [rbp") + std::to_string(slots_[f.params[i].name + "#tag"]) + "], " + regs[r]);
                    param_stores.push_back(std::string("mov QWORD PTR [rbp") + std::to_string(slots_[f.params[i].name]) + "], " + regs[r + 1]);
                    for (int d = 1; d < vk; ++d)
                        param_stores.push_back(std::string("mov QWORD PTR [rbp") + std::to_string(slots_[f.params[i].name + "#d" + std::to_string(d)]) + "], " + regs[r + 1 + d]);
                    r += 1 + vk;
                } else {
                    param_stores.push_back(std::string("mov QWORD PTR [rbp") + std::to_string(slots_[f.params[i].name]) + "], " + regs[r]);
                    r += 1;
                }
            }
        }
        scan_slots(f.has_block_body ? f.block_body.get() : nullptr);
        int words = (int)f.params.size() + (int)slots_.size() + 16; // +16 = temp slots + shadow
        int frame = ((words * 8 + 15) / 16) * 16;
        put(".globl " + f.name);
        put(f.name + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));
        for (auto& st : param_stores) ins(st);
        if (f.name == "main" && has_any_vtables())
            ins("call cpp2_vtinit");           // 虚表运行期初始化(先于任何对象构造)
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

    // 发射一个 lambda 函数:rcx = env,实参自 rdx 起;捕获经 env 加载
    void emit_lambda(std::string const& fn, ast::LambdaExpr* lx)
    {
        cur_fn_ = nullptr;
        cur_sym_name_ = fn;
        cur_struct_ = nullptr;
        scope_stack_.clear();
        slots_.clear();
        push_depth_ = 0;
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        std::vector<std::string> param_stores;
        int r = 1;                               // rcx = env,实参从 rdx 起
        for (size_t i = 0; i < lx->params.size(); ++i) {
            slots_[lx->params[i].name] = slot_or_new(lx->params[i].name);
            param_stores.push_back("mov QWORD PTR [rbp" + std::to_string(slots_[lx->params[i].name]) + "], " + regs[r]);
            ++r;
        }
        int env_off = slot_or_new("$lenv");
        scan_slots(lx->has_block_body ? lx->block_body.get() : nullptr);
        int words = (int)lx->params.size() + (int)slots_.size() + 16;
        int frame = ((words * 8 + 15) / 16) * 16;
        put(fn + ":");
        ins("push rbp");
        ins("mov rbp, rsp");
        if (frame > 0) ins("sub rsp, " + std::to_string(frame));
        ins("mov QWORD PTR [rbp" + std::to_string(env_off) + "], rcx");
        for (auto& st : param_stores) ins(st);
        std::map<std::string, int> caps;
        for (size_t k = 0; k < lx->captures.size(); ++k) {
            std::string c = lx->captures[k];
            if (!c.empty() && c[0] == '&') c = c.substr(1);
            caps[c] = (int)k;
        }
        bool saved_in = in_lambda_;
        int saved_env = lambda_ctx_env_;
        std::map<std::string, int> saved_caps = lambda_caps_;
        in_lambda_ = true;
        lambda_ctx_env_ = env_off;
        lambda_caps_ = caps;
        std::string ret = ".Lret_" + fn;
        if (lx->has_block_body && lx->block_body) emit_stmt(lx->block_body.get());
        else if (lx->expr_body) { eval(lx->expr_body.get()); ins("jmp " + ret); }
        label(ret);
        ins("mov rsp, rbp");
        ins("pop rbp");
        ins("ret");
        put("");
        in_lambda_ = saved_in;
        lambda_ctx_env_ = saved_env;
        lambda_caps_ = saved_caps;
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

    // 作用域析构栈:每层 block 一个帧,记录 (变量名, 声明析构器的 struct)

    void emit_dtor_call(DtorEntry const& vd)
    {
        int off = slot_of(vd.first);
        ins("lea rax, QWORD PTR [rbp" + std::to_string(off) + "]");
        ins("push rax");
        ++push_depth_;
        ins("pop rcx");
        --push_depth_;
        bool odd = (push_depth_ % 2 == 1);
        if (odd) ins("sub rsp, 8");
        ins("sub rsp, 32");
        ins("call " + vd.second->name + "_destructor");
        ins("add rsp, 32");
        if (odd) ins("add rsp, 8");
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
            // 特殊处理 Point 等 struct 的 StructLit 初始化：直接按字段存入 p 的连续槽
            if (v.init && v.init->kind() == ast::Expr::StructLit) {
                auto& sl = static_cast<ast::StructLitExpr&>(*v.init);
                std::string tname = sl.type_parts.empty() ? "" : sl.type_parts[0];
                ast::StructDecl* sd = nullptr;
                for (auto& s : m_->structs) if (s.name == tname) { sd = &s; break; }
                // variant 变量的候选字面量不走普通 struct 路径(需要 #tag 槽)
                if (sd && v.has_type && v.type.parts.size()==1)
                    for (auto& vd : m_->variants)
                        if (vd.name == v.type.parts[0]) { sd = nullptr; break; }
                if (sd) {
                    // 字段 i 存入 base + i*8:base 槽已由 scan_slots 预留为块内最低槽,
                    // 与 field_offset_in / 成员访问的 base+field_off 布局一致
                    int base_off = slot_or_new(v.name);
                    auto fds = fields_deep(sd);
                    for (size_t i=0;i<fds.size();++i) {
                        bool found = false;
                        for (auto& pr : sl.fields) if (pr.first == fds[i].first->name) {
                            eval_field_init(pr.second.get(), fds[i].first);
                            found = true;
                            break;
                        }
                        if (!found) eval_field_init(fds[i].first->init.get(), fds[i].first);
                        ins("mov QWORD PTR [rbp" + std::to_string(base_off + fds[i].second) + "], rax");
                    }
                    if (vptr_pad(sd)) store_vptr(base_off, sd);   // 首槽 vptr(构造自静态类型)
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
                int vk2 = max_alt_fields(v.type.parts[0]);
                for (int d = vk2 - 1; d >= 1; --d)
                    slot_or_new(v.name + "#d" + std::to_string(d));
                int data_off = slot_or_new(v.name);
                int tag_off = slot_or_new(v.name + "#tag");
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
                                    eval_field_init(pr.second.get(), fds[i].first);
                                    found = true;
                                    break;
                                }
                            if (!found) eval_field_init(fds[i].first->init.get(), fds[i].first);
                            ins("mov QWORD PTR [rbp" + std::to_string(data_off + (int)i*8) + "], rax");
                        }
                    }
                }
                break;
            }
            int off = slot_or_new(v.name);
            if (v.init) eval(v.init.get());
            else ins("xor eax, eax");
            if (v.init && v.init->kind() == ast::Expr::Lambda)
                lambda_vars_[v.name] = last_lambda_;    // 调用点按闭包间接调用
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
                // 容器迭代:for x in vec → 堆块 [count][e0]... 按元素步长扫描。
                // variant 元素复制到循环变量的 #tag/#d 槽布局(match 直读)
                if (fo.iterable && fo.iterable->kind() == ast::Expr::Name) {
                    auto& itn = static_cast<ast::NameExpr&>(*fo.iterable);
                    auto it_t = R_ ? R_->type_of(*fo.iterable) : sema::Type{};
                    if (it_t.kind == sema::Type::Kind::Container) {
                        auto et = it_t.elem();
                        int S = elem_stride(et);
                        int vec_off = slot_of(itn.parts[0]);
                        int voff = slot_or_new(fo.var);
                        int tag_off = 0, K = 0;
                        if (et.kind == sema::Type::Kind::Variant) {
                            K = max_alt_fields(et.name);
                            for (int d = K - 1; d >= 1; --d)
                                slot_or_new(fo.var + "#d" + std::to_string(d));
                            slot_or_new(fo.var);
                            tag_off = slot_or_new(fo.var + "#tag");
                        }
                        int ioff = slot_or_new(fo.var + "#i");
                        int noff = slot_or_new(fo.var + "#n");
                        ins("xor eax, eax");
                        ins("mov QWORD PTR [rbp" + std::to_string(ioff) + "], rax");
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(vec_off) + "]");
                        ins("mov rax, QWORD PTR [rax]");              // count
                        ins("mov QWORD PTR [rbp" + std::to_string(noff) + "], rax");
                        std::string top = lbl("fortop"), inc = lbl("forinc"), fend = lbl("fend");
                        label(top);
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                        ins("cmp rax, QWORD PTR [rbp" + std::to_string(noff) + "]");
                        ins("jge " + fend);
                        break_labels_.push_back(fend);
                        continue_labels_.push_back(inc);
                        // var = vec[i]:字节偏移 = 8 + i*S
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(vec_off) + "]");
                        ins("mov rcx, QWORD PTR [rbp" + std::to_string(ioff) + "]");
                        if (S != 8) {
                            ins("mov rdx, " + std::to_string(S));
                            ins("imul rcx, rdx");
                        } else {
                            ins("shl rcx, 3");
                        }
                        if (et.kind == sema::Type::Kind::Variant) {
                            ins("mov rdx, QWORD PTR [rax+rcx+8]");    // tag
                            ins("mov QWORD PTR [rbp" + std::to_string(tag_off) + "], rdx");
                            for (int j = 0; j < K; ++j) {             // 数据槽
                                ins("mov rdx, QWORD PTR [rax+rcx+" + std::to_string(16 + j * 8) + "]");
                                ins("mov QWORD PTR [rbp" + std::to_string(voff + j * 8) + "], rdx");
                            }
                        } else {
                            ins("mov rdx, QWORD PTR [rax+rcx+8]");
                            ins("mov QWORD PTR [rbp" + std::to_string(voff) + "], rdx");
                        }
                        emit_stmt(fo.body.get());
                        label(inc);
                        ins("inc QWORD PTR [rbp" + std::to_string(ioff) + "]");
                        ins("jmp " + top);
                        label(fend);
                        break_labels_.pop_back();
                        continue_labels_.pop_back();
                        break;
                    }
                }
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
            // scrutinee 两形态:variant 名字 → 直读 tag/data 槽;其余 → eval 进 $scr 槽
            std::string scr_name;
            if (m.scrutinee->kind() == ast::Expr::Name)
                scr_name = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
            bool scr_is_variant = !scr_name.empty() && slots_.count(scr_name + "#tag") > 0;
            int scr_off = -1;
            int tag_off = -1, data_off = -1;
            if (scr_is_variant) {
                tag_off = slot_of(scr_name + "#tag");
                data_off = slot_of(scr_name);
            } else {
                eval(m.scrutinee.get());
                scr_off = slot_or_new("$scr_" + std::to_string(label_++));
                ins("mov QWORD PTR [rbp" + std::to_string(scr_off) + "], rax");
            }
            auto read_scr = [&]() { ins("mov rax, QWORD PTR [rbp" + std::to_string(scr_off) + "]"); };
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
                    if (scr_is_variant) unsup("enum pattern on variant scrutinee");
                    read_scr();
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    if (!scr_is_variant) unsup("type pattern needs a variant variable scrutinee");
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
                    // ok/some:哨兵非 0;绑定 = scrutinee 值
                    if (scr_is_variant) unsup("ok/some pattern on variant scrutinee");
                    read_scr();
                    ins("test rax, rax");
                    ins("je " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        read_scr();
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Err || arm.pat == ast::MatchArm::Pat::None) {
                    // err/none:哨兵 0;e 绑定为 0(message/category 经全局槽读取)
                    if (scr_is_variant) unsup("err/none pattern on variant scrutinee");
                    read_scr();
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
                // 守卫:失败落入下一臂(绑定槽已就位)
                if (arm.guard) branch_bool(arm.guard.get(), "", next);
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
            } else if (lit.lit == ast::LitKind::Double) {
                double d = std::stod(lit.text);
                unsigned long long bits;
                std::memcpy(&bits, &d, 8);
                std::string lblname = intern_double_bits(bits);
                ins("lea rax, " + lblname + "[rip]");
                ins("mov rax, QWORD PTR [rax]");
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
                if (in_lambda_) {
                    // 闭包内:捕获名从 env 块加载(env = [fn_ptr][cap0]...)
                    auto ct = lambda_caps_.find(n.parts[0]);
                    if (ct != lambda_caps_.end()) {
                        ins("mov rcx, QWORD PTR [rbp" + std::to_string(lambda_ctx_env_) + "]");
                        ins("mov rax, QWORD PTR [rcx+" + std::to_string(8 + ct->second * 8) + "]");
                        goto name_done;
                    }
                }
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
        case ast::Expr::ListLit: {
            // list/vector 字面量 → 堆块 [count][e0][e1]...
            // 元素槽宽随类型:variant = 1+K 槽(tag+数据),struct = 字段数槽,标量 1 槽
            auto& ll = static_cast<ast::ListLitExpr&>(*e);
            long long n = (long long)ll.elements.size();
            int S = 8;
            if (!ll.elements.empty()) {
                auto et0 = R_ ? R_->type_of(*ll.elements[0]) : sema::Type{};
                S = elem_stride(et0);
            }
            int blk_off = slot_or_new("$list_ptr");
            ins("mov rcx, " + std::to_string(8 + n * S));
            ins("call cpp2_alloc");
            ins("mov QWORD PTR [rbp" + std::to_string(blk_off) + "], rax");
            ins("mov QWORD PTR [rax], " + std::to_string(n));
            int base = 8;
            for (size_t k = 0; k < ll.elements.size(); ++k) {
                auto et = R_ ? R_->type_of(*ll.elements[k]) : sema::Type{};
                if (et.kind == sema::Type::Kind::Variant) {
                    // variant 元素:须为具名变量(有 #tag/#d 槽),整块复制
                    if (ll.elements[k]->kind() != ast::Expr::Name)
                        unsup("list literal: variant element must be a variable");
                    auto& vn = static_cast<ast::NameExpr&>(*ll.elements[k]).parts[0];
                    int tg = slot_of(vn + "#tag");
                    int dt = slot_of(vn);
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(tg) + "]");
                    ins("mov rcx, QWORD PTR [rbp" + std::to_string(blk_off) + "]");
                    ins("mov QWORD PTR [rcx+" + std::to_string(base) + "], rax");
                    for (int j = 1; j * 8 < S; ++j) {
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(dt + (j - 1) * 8) + "]");
                        ins("mov QWORD PTR [rcx+" + std::to_string(base + j * 8) + "], rax");
                    }
                } else if (et.kind == sema::Type::Kind::NamedStruct) {
                    // struct 元素:求值得栈上临时块帧偏移,整块复制
                    eval(ll.elements[k].get());          // rax = 临时块帧偏移(负)
                    ins("mov rcx, QWORD PTR [rbp" + std::to_string(blk_off) + "]");
                    for (int q = 0; q * 8 < S; ++q) {
                        ins("mov rdx, QWORD PTR [rbp+rax+" + std::to_string(q * 8) + "]");
                        ins("mov QWORD PTR [rcx+" + std::to_string(base + q * 8) + "], rdx");
                    }
                } else {
                    eval(ll.elements[k].get());
                    ins("mov rcx, QWORD PTR [rbp" + std::to_string(blk_off) + "]");
                    ins("mov QWORD PTR [rcx+" + std::to_string(base) + "], rax");
                }
                base += S;
            }
            ins("mov rax, QWORD PTR [rbp" + std::to_string(blk_off) + "]");
            break;
        }
        case ast::Expr::Index: {
            // 下标读:容器 = 堆块 [count][e0]...;越界 trap(M2 安全默认)
            auto& ix = static_cast<ast::IndexExpr&>(*e);
            if (ix.base->kind() != ast::Expr::Name) unsup("index base must be name");
            int ptr_off = slot_or_new("$ix_ptr");
            std::string it_ = lbl("ixtrap"), id_ = lbl("ixdone");
            eval(ix.base.get());                    // rax = 块指针
            ins("mov QWORD PTR [rbp" + std::to_string(ptr_off) + "], rax");
            eval(ix.index.get());                   // rax = 下标
            ins("mov rcx, QWORD PTR [rbp" + std::to_string(ptr_off) + "]");
            ins("cmp rax, QWORD PTR [rcx]");
            ins("jae " + it_);
            ins("mov rdx, rax");
            ins("shl rdx, 3");
            ins("add rdx, rcx");
            ins("mov rax, QWORD PTR [rdx+8]");
            ins("jmp " + id_);
            label(it_);
            ins("lea rcx, .Lfmt_ix[rip]");
            ins("sub rsp, 32");
            ins("xor eax, eax");
            ins("call printf");
            ins("add rsp, 32");
            ins("mov rcx, 101");
            ins("call exit");
            label(id_);
            break;
        }
        case ast::Expr::Lambda: {
            // 闭包 v0:显式按值捕获;env = [fn_ptr][cap0][cap1]...(cpp2_alloc)
            auto& lx = static_cast<ast::LambdaExpr&>(*e);
            std::string fn = lbl("lambda");
            std::vector<std::string> caps;
            for (auto& cn : lx.captures)
                caps.push_back(!cn.empty() && cn[0] == '&' ? cn.substr(1) : cn);
            for (auto& c : caps) {                       // 捕获值 = 创建时快照
                int coff = slot_of(c);
                if (coff == 0) unsup("lambda capture '" + c + "' not found");
                ins("mov rax, QWORD PTR [rbp" + std::to_string(coff) + "]");
                ins("push rax");
                ++push_depth_;
            }
            ins("mov rcx, " + std::to_string((caps.size() + 1) * 8));
            ins("call cpp2_alloc");
            int env_off = slot_or_new("$lam_" + fn);
            ins("mov QWORD PTR [rbp" + std::to_string(env_off) + "], rax");
            ins("lea rdx, " + fn + "[rip]");
            ins("mov QWORD PTR [rax], rdx");
            for (size_t k = 0; k < caps.size(); ++k) {   // 倒序弹出存入 env
                ins("pop rcx");
                --push_depth_;
                ins("mov rdx, QWORD PTR [rbp" + std::to_string(env_off) + "]");
                ins("mov QWORD PTR [rdx+" + std::to_string(8 + k * 8) + "], rcx");
            }
            ins("mov rax, QWORD PTR [rbp" + std::to_string(env_off) + "]");
            last_lambda_ = LambdaInfo{fn, caps};
            pending_lambdas_.push_back({fn, &lx});
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
                if (field_off == -1) unsup("unknown field '" + m.name + "'");
                int base_off = slot_of(base.parts[0]);
                bool via_inout = is_inout_param(base.parts[0]);
                int tgt_off = base_off + field_off;
                if (a.op == "=") {
                    eval(a.value.get());
                } else {
                    std::string lop = a.op.substr(0, a.op.size() - 1);
                    if (via_inout) {
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");   // 实参地址
                        ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                    } else {
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(tgt_off) + "]");
                    }
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
                if (via_inout) {
                    ins("mov rcx, QWORD PTR [rbp" + std::to_string(base_off) + "]");
                    ins("mov QWORD PTR [rcx+" + std::to_string(field_off) + "], rax");
                } else {
                    ins("mov QWORD PTR [rbp" + std::to_string(tgt_off) + "], rax");
                }
                break;
            }
            if (a.target->kind() != ast::Expr::Name) unsup("assignment form");
            auto& n = static_cast<ast::NameExpr&>(*a.target);
            // 方法上下文(cur_fn_==nullptr)且名字是字段 → 经 this 指针读写
            bool fld = false;
            int fld_off = -1;
            if (cur_struct_) {
                fld_off = field_offset_in(cur_struct_, n.parts[0]);
                fld = fld_off != -1;
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
            // 解引用接收者:'(*p).f' — 任意指针形态(unique/裸指针/arena_ptr 同形)
            if (m.base->kind() == ast::Expr::Paren) {
                auto* pe = static_cast<ast::ParenExpr*>(m.base.get());
                if (pe->inner->kind() == ast::Expr::Unary
                    && static_cast<ast::UnaryExpr&>(*pe->inner).op == "*") {
                    auto& un = static_cast<ast::UnaryExpr&>(*pe->inner);
                    sema::Type pt = R_ ? R_->type_of(*un.operand) : sema::Type{};
                    if (pt.kind == sema::Type::Kind::SmartPtr) pt = pt.deref();
                    if (pt.kind != sema::Type::Kind::NamedStruct)
                        unsup("deref member: unknown pointee type");
                    ast::StructDecl* rsd = find_sd(pt.name);
                    if (!rsd) unsup("deref member: unknown struct '" + pt.name + "'");
                    int field_off = field_offset_in(rsd, m.name);
                    if (field_off == -1) unsup("unknown field '" + m.name + "'");
                    eval(un.operand.get());               // rax = 指针值
                    ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                    break;
                }
            }
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
            {
                // SmartPtr 基('.' 自动解引用):槽存堆指针,读 [ptr+field_off]
                auto bt2 = R_ ? R_->type_of(*m.base) : sema::Type{};
                if (bt2.kind == sema::Type::Kind::SmartPtr) {
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
                    ins("mov rax, QWORD PTR [rax+" + std::to_string(field_off) + "]");
                    break;
                }
            }
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
                        eval_field_init(pr.second.get(), fds[i].first);
                        found = true;
                        break;
                    }
                    if (!found) eval_field_init(fds[i].first->init.get(), fds[i].first);
                    ins("mov QWORD PTR [rbp" + std::to_string(tmp_off + (int)fds[i].second) + "], rax");
                }
                if (vptr_pad(sd)) store_vptr(tmp_off, sd);   // 临时对象同样带 vptr(make_unique 拷贝用)
            }
            ins("mov rax, " + std::to_string(tmp_off));
            break;
        }
        case ast::Expr::Call:
            emit_call(static_cast<ast::CallExpr&>(*e));
            break;
        case ast::Expr::Match: {
            auto& m = static_cast<ast::MatchExpr&>(*e);
            // scrutinee 两形态:variant 名字(有 #tag 槽)→ 直读;其余 → eval 进 $scr 槽
            std::string scr_name2;
            if (m.scrutinee->kind() == ast::Expr::Name)
                scr_name2 = static_cast<ast::NameExpr&>(*m.scrutinee).parts[0];
            bool scr2_is_variant = !scr_name2.empty() && slots_.count(scr_name2 + "#tag") > 0;
            int scr_tag_off = -1, scr_data_off = -1, scr2_off = -1;
            if (scr2_is_variant) {
                scr_tag_off = slot_of(scr_name2 + "#tag");
                scr_data_off = slot_of(scr_name2);
            } else {
                eval(m.scrutinee.get());
                scr2_off = slot_or_new("$scr_" + std::to_string(label_++));
                ins("mov QWORD PTR [rbp" + std::to_string(scr2_off) + "], rax");
            }
            auto read_scr2 = [&]() { ins("mov rax, QWORD PTR [rbp" + std::to_string(scr2_off) + "]"); };
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
                    if (scr2_is_variant) unsup("enum pattern on variant scrutinee");
                    read_scr2();
                    ins("cmp rax, " + std::to_string(enum_val));
                    ins("jne " + next);
                } else if (arm.pat == ast::MatchArm::Pat::TypePat) {
                    if (!scr2_is_variant) unsup("type pattern needs a variant variable scrutinee");
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
                } else if (arm.pat == ast::MatchArm::Pat::Ok || arm.pat == ast::MatchArm::Pat::Some) {
                    // some/ok:哨兵非 0;绑定 = scrutinee 值
                    if (scr2_is_variant) unsup("ok/some pattern on variant scrutinee");
                    read_scr2();
                    ins("test rax, rax");
                    ins("je " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        read_scr2();
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else if (arm.pat == ast::MatchArm::Pat::Err || arm.pat == ast::MatchArm::Pat::None) {
                    // none/err:哨兵 0;绑定 = 0(message/category 经全局槽)
                    if (scr2_is_variant) unsup("err/none pattern on variant scrutinee");
                    read_scr2();
                    ins("test rax, rax");
                    ins("jne " + next);
                    if (!arm.binding.empty() && arm.binding != "_") {
                        int boff = slot_or_new(arm.binding);
                        ins("xor eax, eax");
                        ins("mov QWORD PTR [rbp" + std::to_string(boff) + "], rax");
                    }
                } else {
                    unsup("unsupported match expr pattern");
                }
                // 守卫:失败落入下一臂
                if (arm.guard) branch_bool(arm.guard.get(), "", next);
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
            // 接收者形态:具名变量 / '(*p)' 解引用(虚分发 + 智能指针堆对象)
            ast::Expr* rbase = mem.base.get();
            bool rderef = false;
            if (rbase->kind() == ast::Expr::Paren) {
                auto* pe = static_cast<ast::ParenExpr*>(rbase);
                if (pe->inner->kind() == ast::Expr::Unary
                    && static_cast<ast::UnaryExpr&>(*pe->inner).op == "*") {
                    rderef = true;
                    rbase = static_cast<ast::UnaryExpr&>(*pe->inner).operand.get();
                }
            }
            std::string base_name;                     // 具名接收者名(解引用形态为空)
            if (!rderef) {
                if (rbase->kind() != ast::Expr::Name) unsup("method base must be name");
                base_name = static_cast<ast::NameExpr&>(*rbase).parts[0];
            }
            ast::StructDecl* sd = nullptr;
            ast::MethodDecl* md = nullptr;
            // 接收者静态类型优先沿继承链解析(派生方法隐藏基类同名)
            if (R_) {
                auto bt = R_->type_of(*mem.base);
                if (bt.kind == sema::Type::Kind::SmartPtr) bt = bt.deref();  // '.' 自动解引用
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
                // 容器方法桥:vector 增长(push_back)
                if (mem.name == "push_back") {
                    auto btt = R_ ? R_->type_of(*mem.base) : sema::Type{};
                    if (btt.kind == sema::Type::Kind::Container) {
                        emit_vector_push_back(base_name, c);
                        return;
                    }
                }
                // UFCS 桥:n.to_string() ≡ to_string(n)(int/double)
                if (mem.name == "to_string") {
                    eval(mem.base.get());
                    if (R_ && R_->type_of(*mem.base).kind == sema::Type::Kind::Double) {
                        ins("movq xmm0, rax");
                        ins("call cpp2_dbl_str");
                    } else {
#ifdef CPP2_NATIVE_HOST_OK
                        ins("mov rcx, rax");
#else
                        ins("mov rdi, rax");
#endif
                        ins("call cpp2_int_str");
                    }
                    return;
                }
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
            // 实参先求值压栈,this 最后压栈(rcx 最先弹出)
            for (auto& a : c.args) {
                eval(a.get());
                ins("push rax");
                ++push_depth_;
            }
            // this:解引用接收者 = 指针值;SmartPtr 名 = 堆指针;具名对象 = 帧地址
            if (rderef) {
                eval(rbase);                       // rax = 指针(unique/裸指针同形)
            } else {
                int base_off = slot_of(base_name);
                auto bt3 = R_ ? R_->type_of(*mem.base) : sema::Type{};
                if (bt3.kind == sema::Type::Kind::SmartPtr)   // '.' 自动解引用:this = 堆指针
                    ins("mov rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
                else
                    ins("lea rax, QWORD PTR [rbp" + std::to_string(base_off) + "]");
            }
            ins("push rax");
            ++push_depth_;
            static char const* regs[] = {"rcx","rdx","r8","r9"};
            ins("pop rcx");                       // this
            --push_depth_;
            for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                ins("pop " + std::string(regs[i+1]));
                --push_depth_;
            }
            // 虚分发(M7):静态类型定槽位,经 vptr 间接调用(once virtual, always virtual)
            int vt = vtable_slot(sd, mem.name);
            if (vt >= 0) {
                ins("mov r10, QWORD PTR [rcx]");              // vptr
                ins("mov rax, QWORD PTR [r10+" + std::to_string(vt * 8) + "]");
                ins("call rax");
            } else {
                ins("call " + sd->name + "_" + md->name);
            }
            return;
        }
        if (c.callee->kind() != ast::Expr::Name) unsup("indirect calls");
        auto& nm = static_cast<ast::NameExpr&>(*c.callee);
        if (!nm.qualified()) {
            std::string const& bn = nm.parts[0];
        // ── 智能指针工厂 v0:make_unique/make_shared → 堆块 + 整对象拷贝 ──
        // 实参:聚合字面量(栈上临时)或具名变量;含 vptr 的类型整槽拷贝后
        // 动态类型随对象保留(虚分发语义)。shared v0 与 unique 同构(无引用
        // 计数头,作用域结束不释放,进程退出回收);'.' 自动解引用与方法
        // this 解引用由 Member/emit_call 的 SmartPtr 分支承担
        if ((bn == "make_unique" || bn == "make_shared") && c.args.size() == 1) {
            ast::Expr* mkarg = c.args[0].get();
            std::string tn;
            if (mkarg->kind() == ast::Expr::StructLit) {
                tn = static_cast<ast::StructLitExpr&>(*mkarg).type_parts.empty()
                         ? "" : static_cast<ast::StructLitExpr&>(*mkarg).type_parts[0];
            } else if (mkarg->kind() == ast::Expr::Name) {
                auto at = R_ ? R_->type_of(*mkarg) : sema::Type{};
                if (at.kind == sema::Type::Kind::NamedStruct) tn = at.name;
            }
            ast::StructDecl* sd = nullptr;
            for (auto& s : m_->structs) if (s.name == tn) { sd = &s; break; }
            if (!sd) unsup("make_unique/make_shared: unknown struct '" + tn + "'");
            auto fds = fields_deep(sd);
            long long nbytes = fds.empty()
                ? 8                                          // 空对象:仅占位/vptr 槽
                : (long long)fds.back().second + 8;
            if (mkarg->kind() == ast::Expr::StructLit) {
                eval(mkarg);                    // StructLit → rax = 帧偏移(负)
                ins("mov rsi, rax");
                ins("add rsi, rbp");            // rsi = 源(栈上临时块)
            } else {
                int src_off = slot_of(static_cast<ast::NameExpr&>(*mkarg).parts[0]);
                ins("lea rsi, QWORD PTR [rbp" + std::to_string(src_off) + "]");
            }
            ins("mov rcx, " + std::to_string(nbytes));
            ins("call cpp2_alloc");             // rax = 堆块(cpp2_alloc 不动 rsi/rdi)
            ins("mov rdi, rax");
            for (long long q = 0; q < nbytes; q += 8) {   // 全槽拷贝(含 vptr 首槽)
                ins("mov rcx, QWORD PTR [rsi+" + std::to_string(q) + "]");
                ins("mov QWORD PTR [rdi+" + std::to_string(q) + "], rcx");
            }
            ins("mov rax, rdi");
            return;
        }
            if (bn == "err") {
                // v1:错误串存入全局槽 .Lerrmsg;返回 0 哨兵。
                // 与参考实现一致,编译期拼接来源位置后缀" (路径:行号)"
                // err(msg, cat) 二参形式:类别存 .Lerrcat(M7);同时清空因果链
                if (!c.args.empty()) {
                    if (c.args.size() >= 2) {
                        eval(c.args[0].get());
                        ins("push rax");
                        ++push_depth_;
                        eval(c.args[1].get());
                        ins("lea rcx, .Lerrcat[rip]");
                        ins("mov QWORD PTR [rcx], rax");
                        ins("pop rax");
                        --push_depth_;
                    } else {
                        eval(c.args[0].get());
                    }
                    if (!src_path_.empty()) {
                        std::string suffix = " (" + src_path_ + ":" + std::to_string(c.line) + ")";
                        std::string slbl = intern_string(suffix);
                        ins("mov rcx, rax");
                        ins("lea rdx, " + slbl + "[rip]");
                        ins("call cpp2_strcat");
                    }
                    ins("lea rcx, .Lerrmsg[rip]");
                    ins("mov QWORD PTR [rcx], rax");
                    ins("lea rcx, .Lerrcause[rip]");   // 新错误无因果链:指向空串
                    ins("lea rdx, " + intern_string("") + "[rip]");
                    ins("mov QWORD PTR [rcx], rdx");
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
        if (nm.qualified() && nm.parts[0] == "std" && nm.parts[1] == "sqrt" && c.args.size() == 1) {
            eval(c.args[0].get());
            ins("movq xmm0, rax");
            if (!is_double_expr(c.args[0].get())) ins("cvtsi2sd xmm0, rax");
            ins("sqrtsd xmm0, xmm0");
            ins("movq rax, xmm0");
            return;
        }
        if (nm.qualified() && nm.parts[0] == "std" && nm.parts[1] == "to_string" && c.args.size() == 1) {
            eval(c.args[0].get());
            if (is_double_expr(c.args[0].get())) {
                ins("movq xmm0, rax");
                ins("call cpp2_dbl_str");
            } else {
                ins("mov rcx, rax");
                ins("call cpp2_int_str");   // → "%lld" 文本(轮转缓冲)
            }
            return;
        }
        if (nm.qualified() && nm.parts[0] == "cpp2" && nm.parts.size() >= 2 && nm.parts[1] == "error") {
            // cpp2::error(msg):直接构造,message = msg(无位置后缀),category = 0,清链
            if (!c.args.empty()) {
                eval(c.args[0].get());
                ins("lea rcx, .Lerrmsg[rip]");
                ins("mov QWORD PTR [rcx], rax");
                ins("lea rcx, .Lerrcat[rip]");
                ins("mov QWORD PTR [rcx], 0");
                ins("lea rcx, .Lerrcause[rip]");
                ins("lea rdx, " + intern_string("") + "[rip]");
                ins("mov QWORD PTR [rcx], rdx");
            }
            ins("xor eax, eax");
            return;
        }
        if (nm.qualified() && nm.parts[0] == "cpp2" && nm.parts.size() >= 2 && nm.parts[1] == "err_caused" && c.args.size() == 2) {
            // err_caused(cause, msg):新 text = msg + " (:0)"(转译基线 file="",line=0);
            // category 沿用 cause(即当前 .Lerrcat);cause 文本入 .Lerrcause
            // v1 限制:cause 实参不再单独求值,链状态直接取全局槽(仅一层)
            eval(c.args[1].get());
            std::string slbl = intern_string(" (:0)");
            ins("mov rcx, rax");
            ins("lea rdx, " + slbl + "[rip]");
            ins("call cpp2_strcat");
            ins("push rax");
            ++push_depth_;
            ins("lea rcx, .Lerrmsg[rip]");
            ins("mov rcx, QWORD PTR [rcx]");   // cause 文本
            ins("lea rdx, " + intern_string("") + "[rip]");
            ins("call cpp2_strcat");            // 与空串拼接 = 堆拷贝
            ins("lea rcx, .Lerrcause[rip]");
            ins("mov QWORD PTR [rcx], rax");
            ins("pop rax");
            --push_depth_;
            ins("lea rcx, .Lerrmsg[rip]");
            ins("mov QWORD PTR [rcx], rax");
            ins("xor eax, eax");
            return;
        }
        if (!nm.qualified()) {
            auto lv = lambda_vars_.find(nm.parts[0]);
            if (lv != lambda_vars_.end()) {
                // 闭包调用:rcx = env,实参 rdx/r8/r9,经 env[0] 间接 call
                for (auto& a : c.args) {
                    eval(a.get());
                    ins("push rax");
                    ++push_depth_;
                }
                static char const* lregs[] = {"rdx","r8","r9"};
                for (int i = (int)c.args.size() - 1; i >= 0; --i) {
                    ins(std::string("pop ") + lregs[i]);
                    --push_depth_;
                }
                int eoff = slot_of(nm.parts[0]);
                ins("mov rcx, QWORD PTR [rbp" + std::to_string(eoff) + "]");
                ins("mov rax, QWORD PTR [rcx]");
                ins("call rax");
                return;
            }
        }
        if (nm.qualified()) unsup("qualified calls (std bridge)");
        // inout/out 实参传地址(裸名字且槽存在);其余传值
        ast::FuncDecl* c2_callee = nullptr;
        for (auto& f : m_->funcs)
            if (f.name == nm.parts[0] && f.params.size() == c.args.size()) { c2_callee = &f; break; }
        // 实参按"寄存器槽"序列压栈:variant 参数占 2 槽(tag, data),与 emit_func 布局一致
        std::vector<std::function<void()>> pushes;
        {
            int regslots = 0;
            for (size_t i = 0; i < c.args.size(); ++i) {
                auto& a = c.args[i];
                bool pass_addr = c2_callee && (c2_callee->params[i].mode == ast::ParamMode::Inout || c2_callee->params[i].mode == ast::ParamMode::Out)
                              && a->kind() == ast::Expr::Name;
                bool pass_variant = c2_callee && is_variant_type(c2_callee->params[i].type) && a->kind() == ast::Expr::Name;
                if (pass_addr) {
                    auto& an = static_cast<ast::NameExpr&>(*a);
                    int aoff = slot_of(an.parts[0]);
                    pushes.push_back([this, aoff]() {
                        ins("lea rax, QWORD PTR [rbp" + std::to_string(aoff) + "]");
                        ins("push rax");
                        ++push_depth_;
                    });
                    regslots += 1;
                } else if (pass_variant) {
                    auto& an = static_cast<ast::NameExpr&>(*a);
                    if (!slots_.count(an.parts[0] + "#tag")) unsup("variant arg must be a variable with tag: '" + an.parts[0] + "'");
                    int toff = slot_of(an.parts[0] + "#tag");
                    pushes.push_back([this, toff]() {
                        ins("mov rax, QWORD PTR [rbp" + std::to_string(toff) + "]");
                        ins("push rax");
                        ++push_depth_;
                    });
                    pushes.push_back([this, &a]() {
                        eval(a.get());
                        ins("push rax");
                        ++push_depth_;
                    });
                    int ak = 1;
                    for (auto& vd : m_->variants)
                        if (vd.name == c2_callee->params[i].type.parts[0]) { ak = max_alt_fields(vd.name); break; }
                    for (int d = 1; d < ak; ++d) {
                        int doff = slot_of(an.parts[0] + "#d" + std::to_string(d));
                        pushes.push_back([this, doff]() {
                            ins("mov rax, QWORD PTR [rbp" + std::to_string(doff) + "]");
                            ins("push rax");
                            ++push_depth_;
                        });
                    }
                    regslots += 1 + ak;
                } else {
                    pushes.push_back([this, &a]() {
                        eval(a.get());
                        ins("push rax");
                        ++push_depth_;
                    });
                    regslots += 1;
                }
            }
            if (regslots > 4) unsup("more than 4 register slots on Windows native call");
        }
        for (auto& pf : pushes) pf();
        static char const* regs[] = {"rcx","rdx","r8","r9"};
        for (int i = (int)pushes.size() - 1; i >= 0; --i) {
            ins("pop " + std::string(regs[i]));
            --push_depth_;
        }
        if (push_depth_ % 2 == 1) {              // call 前 rsp 16 对齐(奇数次 8B push)
            ins("sub rsp, 8");
            ins("sub rsp, 32");
            ins("call " + nm.parts[0]);
            ins("add rsp, 40");
        } else {
            ins("sub rsp, 32");
            ins("call " + nm.parts[0]);
            ins("add rsp, 32");
        }
    }

    // bool 实参 → "true"/"false" C 串(std::println 语义);rax = 0/1 入,rax = 串指针出
    void emit_bool_str()
    {
        std::string t0 = lbl("btt"), t1 = lbl("btf");
        ins("test rax, rax");
        ins("jne " + t0);
        ins("lea rax, " + intern_string("false") + "[rip]");
        ins("jmp " + t1);
        label(t0);
        ins("lea rax, " + intern_string("true") + "[rip]");
        label(t1);
    }

    void emit_printf(ast::CallExpr& c, bool newline)
    {
        if (c.args.empty()) unsup("println needs at least a format string");
        if (c.args.size() == 1) {
            auto* lit0 = dynamic_cast<ast::LiteralExpr*>(c.args[0].get());
            if (lit0 && lit0->lit == ast::LitKind::String) {
                // 单参字符串：走下方 cfmt 流程
            } else {
                // 单参非字符串:按静态类型选格式(int %lld / string %s / double %f / char %c)
                std::string cfmt = fmt_for(c.args[0].get());
                if (newline) cfmt += "\n";
                std::string fmt_lbl = intern_string(cfmt);
                eval(c.args[0].get());
                if (is_double_expr(c.args[0].get())) {   // 位模式 → 最短往返字符串(对齐 std::format 默认)
                    ins("movq xmm0, rax");
                    ins("call cpp2_dbl_str_rt");
                }
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
                int idx = text[pos + 1] - '0';
                if (idx + 1 >= (int)c.args.size())
                    unsup("println placeholder {" + std::string(1, text[pos + 1]) + "} out of range");
                // double 实参:{0} → %s(先经 cpp2_dbl_str_rt 转最短往返字符串,
                // 对齐转译基线 std::format 默认 = 最短表示;曾用 %f,输出 12.566360 与基线 12.56636 不一致)
                // bool 实参:{0} → %s("true"/"false",std::format 语义;cout << 路径仍为 1/0)
                if (is_double_expr(c.args[idx + 1].get())) cfmt += "%s";
                else if (is_bool_expr(c.args[idx + 1].get())) cfmt += "%s";
                else cfmt += fmt_for(c.args[idx + 1].get());
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
            if (is_double_expr(c.args[i].get())) {   // 位模式 → 最短往返字符串
                ins("movq xmm0, rax");
                ins("call cpp2_dbl_str_rt");
            } else if (is_bool_expr(c.args[i].get())) {
                emit_bool_str();
            }
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
        // double 路径:操作数按位模式进 rax/rcx,经 XMM 运算
        if (b.op != "&&" && b.op != "||" &&
            (is_double_expr(b.lhs.get()) || is_double_expr(b.rhs.get()))) {
            eval(b.lhs.get());
            ins("push rax");
            ++push_depth_;
            eval(b.rhs.get());
            ins("pop rcx");
            --push_depth_;
            // rcx = lhs 位模式(或整值),rax = rhs
            if (is_double_expr(b.lhs.get())) ins("movq xmm1, rcx");
            else { ins("movq xmm1, rcx"); ins("cvtsi2sd xmm1, rcx"); }
            if (is_double_expr(b.rhs.get())) ins("movq xmm0, rax");
            else { ins("movq xmm0, rax"); ins("cvtsi2sd xmm0, rax"); }
            if (b.op == "+") ins("addsd xmm1, xmm0");
            else if (b.op == "-") ins("subsd xmm1, xmm0");
            else if (b.op == "*") ins("mulsd xmm1, xmm0");
            else if (b.op == "/") ins("divsd xmm1, xmm0");
            else if (b.op == "<" || b.op == ">" || b.op == "<=" || b.op == ">=" ||
                     b.op == "==" || b.op == "!=") {
                ins("comisd xmm1, xmm0");
                std::string cc = (b.op=="<")?"setl":(b.op==">")?"setg":(b.op=="<=")?"setle":
                                 (b.op==">=")?"setge":(b.op=="==")?"sete":"setne";
                ins(cc + " al");
                ins("movzx rax, al");
                return;
            }
            else unsup("binary operator '" + b.op + "' on double");
            ins("movq rax, xmm1");
            return;
        }
        eval(b.lhs.get());
        ins("push rax");
        ++push_depth_;
        eval(b.rhs.get());
        ins("pop rcx");
        --push_depth_;
        if (b.op == "+") {
            if (is_string_expr(b.lhs.get())) {
                ins("mov rdx, rax");
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

std::string emit_asm(ast::Module& m, sema::Result const& r, std::string const& src_path)
{
    NativeEmitter em;
    return em.emit(m, r, src_path);
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
    // 注意: 本运行时导入的是 msvcrt.dll(legacy CRT)。其 printf 家族按"varargs 全部
    //       走 GP 位置流"读取: 第 1/2/3 个变参依次取 rdx/r8/r9 的 home 槽,之后取栈槽;
    //       xmm 寄存器与 AL 完全不参与(gdb 实测 sprintf 反汇编: va_list = rbp+0x28
    //       即 r8 home 起始的 GP 流)。因此浮点变参必须以位模式放入下一个 GP 位置,
    //       cpp2_dbl_str 调 sprintf 时 double 放 r9,而非 xmm3+AL。
    static char const* runtime_s = R"(
.section .text
.globl cpp2_write
cpp2_write:
    push rbp
    mov rbp, rsp
    sub rsp, 0x40
    mov QWORD PTR [rbp-0x08], rcx
    mov QWORD PTR [rbp-0x10], rdx
    mov ecx, -11
    call GetStdHandle
    mov rcx, rax
    mov rdx, [rbp-0x08]
    mov r8, [rbp-0x10]
    lea r9, [rbp-0x18]
    mov QWORD PTR [rbp-0x18], 0
    call WriteFile
    add rsp, 0x40
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
    sub rsp, 0x30
    mov QWORD PTR [rbp-0x08], rcx
    call GetProcessHeap
    mov rcx, rax
    mov rdx, 0
    mov r8, [rbp-0x08]
    call HeapAlloc
    add rsp, 0x30
    pop rbp
    ret

cpp2_memcopy:
    xor r9, r9
.Lmc_loop:
    cmp r9, r8
    jae .Lmc_done
    mov rax, QWORD PTR [rdx+r9]
    mov QWORD PTR [rcx+r9], rax
    add r9, 8
    jmp .Lmc_loop
.Lmc_done:
    ret
.globl cpp2_free
cpp2_free:
    push rbp
    mov rbp, rsp
    sub rsp, 0x30
    mov QWORD PTR [rbp-0x08], rcx
    call GetProcessHeap
    mov rcx, rax
    mov rdx, 0
    mov r8, [rbp-0x08]
    call HeapFree
    add rsp, 0x30
    pop rbp
    ret
.globl cpp2_strcat
cpp2_strcat:
    push rbp
    mov rbp, rsp
    sub rsp, 0x40
    mov QWORD PTR [rbp-0x08], rcx
    mov QWORD PTR [rbp-0x10], rdx
    mov rax, [rbp-0x08]
    xor r9, r9
.cpp2_strcat_la:
    cmp byte PTR [rax+r9], 0
    je .cpp2_strcat_lb
    inc r9
    jmp .cpp2_strcat_la
.cpp2_strcat_lb:
    mov QWORD PTR [rbp-0x20], r9
    mov rax, [rbp-0x10]
    xor r10, r10
.cpp2_strcat_lb2:
    cmp byte PTR [rax+r10], 0
    je .cpp2_strcat_alloc
    inc r10
    jmp .cpp2_strcat_lb2
.cpp2_strcat_alloc:
    mov rcx, r9
    add rcx, r10
    inc rcx
    call cpp2_alloc
    mov r9, QWORD PTR [rbp-0x20]
    mov QWORD PTR [rbp-0x18], rax
    mov rcx, [rbp-0x08]
    mov rdx, [rbp-0x18]
    xor r8, r8
.cpp2_strcat_cp1:
    cmp r8, r9
    jge .cpp2_strcat_cp2
    movzx r10, byte PTR [rcx+r8]
    mov ebx, r10d
    mov byte PTR [rdx+r8], bl
    inc r8
    jmp .cpp2_strcat_cp1
.cpp2_strcat_cp2:
    mov rcx, [rbp-0x10]
    mov rdx, [rbp-0x18]
    xor r8, r8
.cpp2_strcat_cp3:
    cmp byte PTR [rcx+r8], 0
    je .cpp2_strcat_done
    movzx r10, byte PTR [rcx+r8]
    mov ebx, r10d
    mov byte PTR [rdx+r9], bl
    inc r9
    inc r8
    jmp .cpp2_strcat_cp3
.cpp2_strcat_done:
    mov rax, [rbp-0x18]
    mov byte PTR [rax+r9], 0
    add rsp, 0x40
    pop rbp
    ret
.globl cpp2_dbl_str
cpp2_dbl_str:
    push rbp
    mov rbp, rsp
    sub rsp, 64
    movsd QWORD PTR [rbp-8], xmm0
    lea rax, .Ldblcnt[rip]
    mov rcx, QWORD PTR [rax]
    lea rdx, [rcx+1]
    mov QWORD PTR [rax], rdx
    and rcx, 3
    shl rcx, 5
    lea rax, .Ldblbuf_00[rip]
    add rax, rcx
    mov QWORD PTR [rbp-16], rax
    mov rcx, rax
    lea rdx, .Lfmt_g[rip]
    mov r8, QWORD PTR [rbp-8]
    call sprintf
    mov rax, QWORD PTR [rbp-16]
    add rsp, 64
    pop rbp
    ret
.globl cpp2_dbl_str_rt
cpp2_dbl_str_rt:
    push rbp
    mov rbp, rsp
    sub rsp, 64
    movsd QWORD PTR [rbp-8], xmm0
    mov DWORD PTR [rbp-24], 1
.cpp2_dblrt_loop:
    lea rax, [rbp-32]
    mov BYTE PTR [rax], 37
    mov BYTE PTR [rax+1], 46
    mov ecx, DWORD PTR [rbp-24]
    cmp ecx, 10
    jb .cpp2_dblrt_one
    mov BYTE PTR [rax+2], 49
    add ecx, 38
    mov BYTE PTR [rax+3], cl
    mov BYTE PTR [rax+4], 103
    mov BYTE PTR [rax+5], 0
    jmp .cpp2_dblrt_have
.cpp2_dblrt_one:
    add ecx, 48
    mov BYTE PTR [rax+2], cl
    mov BYTE PTR [rax+3], 103
    mov BYTE PTR [rax+4], 0
.cpp2_dblrt_have:
    lea rax, .Ldblcnt[rip]
    mov rcx, QWORD PTR [rax]
    lea rdx, [rcx+1]
    mov QWORD PTR [rax], rdx
    and rcx, 3
    shl rcx, 5
    lea rax, .Ldblbuf_00[rip]
    add rax, rcx
    mov QWORD PTR [rbp-16], rax
    mov rcx, rax
    lea rdx, [rbp-32]
    mov r8, QWORD PTR [rbp-8]
    call sprintf
    mov rcx, QWORD PTR [rbp-16]
    xor rdx, rdx
    call strtod
    comisd xmm0, QWORD PTR [rbp-8]
    je .cpp2_dblrt_done
    mov eax, DWORD PTR [rbp-24]
    cmp eax, 17
    jae .cpp2_dblrt_done
    add eax, 1
    mov DWORD PTR [rbp-24], eax
    jmp .cpp2_dblrt_loop
.cpp2_dblrt_done:
    mov rax, QWORD PTR [rbp-16]
    add rsp, 64
    pop rbp
    ret
.globl cpp2_int_str
cpp2_int_str:
    push rbp
    mov rbp, rsp
    sub rsp, 64
    mov QWORD PTR [rbp-8], rcx
    lea rax, .Lintcnt[rip]
    mov rcx, QWORD PTR [rax]
    lea rdx, [rcx+1]
    mov QWORD PTR [rax], rdx
    and rcx, 3
    shl rcx, 5
    lea rax, .Lintbuf_00[rip]
    add rax, rcx
    mov QWORD PTR [rbp-16], rax
    mov rcx, rax
    lea rdx, .Lfmt_int[rip]
    mov r8, QWORD PTR [rbp-8]
    call sprintf
    mov rax, QWORD PTR [rbp-16]
    add rsp, 64
    pop rbp
    ret

.section .rodata
.Lfmt_g:
    .string "%.6g"
.Lfmt_int:
    .string "%lld"
.Lfmt_ix:
    .string "cpp2 trap: index out of bounds\n"
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

    // .data 全局槽(如 .Lerrmsg 错误消息指针)
    std::vector<uint8_t> data_blob;
    std::vector<std::pair<std::string,std::string>> da_labels;
    for (auto& [name, qv] : res.data) {
        da_labels.push_back({name, std::to_string(data_blob.size())});
        for (int i = 0; i < 8; ++i) data_blob.push_back((uint8_t)((qv >> (i*8)) & 0xff));
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
            s == "strtod" ||
            s == "malloc" || s == "free" || s == "exit" || s == "abort")
            return "msvcrt.dll";
        return "kernel32.dll";
    };
    for (auto& e : res.externs) {
        if (e == "cpp2_write" || e == "cpp2_exit" || e == "sys_exit" ||
            e == "cpp2_alloc" || e == "cpp2_free" || e == "cpp2_strcat")
            continue;  // 由内联运行时提供
        imports_map[dll_for(e)].push_back(e);
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> imports(
        imports_map.begin(), imports_map.end());

    return pe::build_exe(res.text, rodata_blob, relocs,
                         ro_labels, text_labels, imports, data_blob, da_labels);
}
#endif


} // namespace cpp2::native
