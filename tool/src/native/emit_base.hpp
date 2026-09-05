// emit_base.hpp — native 后端发射器公共基类(OOP 重构,自 native.cpp 拆出):
//   SysV 与 Win64 两个发射器的共享状态与共享逻辑下沉至此;
//   ABI 差异(调用约定 / 寄存器分配 / printf fmt / 闭包队列)经虚钩子由子类实现。
//   模板方法 emit() 固定发射流程,子类经钩子注入平台行为,行为与重构前一致。
#pragma once
#include "../native.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace cpp2::native {

[[noreturn]] inline void unsup(std::string const& msg) { throw Unsupported(msg); }

// 源码级转义 → 真实字节(rodata 存真实字节;发射时 emit_rodata 再转义)
inline std::string unescape_str(std::string const& in)
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

bool is_int_kind(sema::Type::Kind k);

class EmitterBase {
public:
    virtual ~EmitterBase() = default;

    using DtorEntry = std::pair<std::string, ast::StructDecl*>;

    // 模板方法:发射流程固定,ABI 行为经钩子注入
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
            if (!f.type_params.empty()) continue;   // 泛型:不直接发射,调用点单态化时发射实例
            check_func(f);
            emit_func(f);
        }
        for (auto& s : m_->structs) {
            for (auto& md : s.methods) {
                emit_method(s, md);
            }
        }
        emit_pending_lambdas();
        emit_rodata_strs();
        return out_.str();
    }

protected:
    virtual void emit_globals() = 0;
    virtual void check_func(ast::FuncDecl& f) = 0;
    virtual void emit_func(ast::FuncDecl& f, std::string const& sym_override = {}) = 0;
    virtual void emit_method(ast::StructDecl& s, ast::MethodDecl& m) = 0;
    virtual void emit_rodata_strs() = 0;
    virtual void emit_dtor_call(DtorEntry const& vd) = 0;
    virtual void emit_pending_lambdas() {}                // Win64:闭包发射队列
    virtual char const* dbl_fmt() const { return "%f"; }  // printf double 格式符

    [[noreturn]] void unsup(std::string const& msg) { throw Unsupported(msg); }

    // 类型查询:泛型单态化感知。
    //   Generic("T") → mono_map_ 中实参具体类型(实例体内查询);
    //   Unknown 具名变量 → mono_var_types_ 中 ":= 泛型调用" 的推断结果类型
    sema::Type type_of(ast::Expr* e) const
    {
        if (!R_ || !e) return sema::Type{};
        auto t = R_->type_of(*e);
        if (t.kind == sema::Type::Kind::Generic) {
            auto it = mono_map_.find(t.name);
            if (it != mono_map_.end()) return it->second;
        }
        if (t.kind == sema::Type::Kind::Unknown) {
            if (auto* n = dynamic_cast<ast::NameExpr*>(e)) {
                auto it2 = mono_var_types_.find(n->parts[0]);
                if (it2 != mono_var_types_.end()) return it2->second;
            }
        }
        return t;
    }

    void ins(std::string const& s) { out_ << "    " << s << "\n"; }

    void put(std::string const& s) { out_ << s << "\n"; }

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

    bool is_inout_param(std::string const& n) const { return inout_params_.count(n) != 0; }

    bool is_string_expr(ast::Expr* e)
    {
        if (!e) return false;
        if (auto* lit = dynamic_cast<ast::LiteralExpr*>(e))
            if (lit->lit == ast::LitKind::String) return true;
        if (!R_) return false;
        auto t = type_of(e);
        using K = sema::Type::Kind;
        return t.kind == K::String || t.kind == K::StringView;
    }

    void record_inout_params(std::vector<ast::Param>& params)
    {
        inout_params_.clear();
        for (auto& p : params)
            if (p.mode == ast::ParamMode::Inout || p.mode == ast::ParamMode::Out) inout_params_.insert(p.name);
    }

    void check_global(ast::GlobalVar& g)
    {
        if (g.type.is_array)
            unsup("array globals are not supported on the native backend: '" + g.name + "'");
        auto k = scalar_kind(g.type);
        if (g.type.parts.size() != 1 || !is_int_kind(k))
            unsup("global '" + g.name + "' must be an explicitly-typed integer");
        if (!g.init || g.init->kind() != ast::Expr::Literal
            || static_cast<ast::LiteralExpr&>(*g.init).lit != ast::LitKind::Int)
            unsup("global '" + g.name + "' requires an integer literal initializer");
    }

    // M9:bare map 在 native 显式 unsup(无降级实现,转译回退兜底);
    // 扫描形参 + 函数体内的局部声明,凡 map<K,V> 即拒。
    void check_native_no_map(ast::FuncDecl& f)
    {
        auto is_map_type = [](ast::TypeUse const& t) {
            return t.parts.size() == 1 && t.parts[0] == "map";
        };
        for (auto& p : f.params)
            if (is_map_type(p.type))
                unsup("map is not supported on the native backend: '" + f.name + "'");
        std::function<void(ast::Stmt*)> walk = [&](ast::Stmt* s) {
            if (!s) return;
            switch (s->kind()) {
            case ast::Stmt::Block:
                for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) walk(st.get());
                break;
            case ast::Stmt::Var: {
                auto& v = static_cast<ast::VarStmt&>(*s);
                if (v.has_type && is_map_type(v.type))
                    unsup("map is not supported on the native backend: '" + f.name + "'");
                break;
            }
            case ast::Stmt::If: {
                auto& i = static_cast<ast::IfStmt&>(*s);
                walk(i.then_block.get());
                walk(i.else_block.get());
                break;
            }
            case ast::Stmt::While:
                walk(static_cast<ast::WhileStmt&>(*s).body.get());
                break;
            case ast::Stmt::For:
                walk(static_cast<ast::ForStmt&>(*s).body.get());
                break;
            case ast::Stmt::Match:
                for (auto& arm : static_cast<ast::MatchStmt&>(*s).arms) walk(arm.body.get());
                break;
            default: break;
            }
        };
        if (f.has_block_body) walk(f.block_body.get());
    }

    // M10:原生数组在 native 的落地面 = 函数体内局部声明/下标/遍历/整体拷贝/len/at;
    // 形参、返回类型、struct 字段、全局 —— 越过 8B 槽模型的部分显式 unsup。
    // 元素类型须为单 8B 槽标量;string 元素在 native 是 3 槽布局,不落槽模型 → 转译回退。
    bool native_array_elem_ok(ast::TypeUse const& t)
    {
        if (t.parts.size() != 1) return false;
        std::string const& n = t.parts[0];
        if (n == "double" || n == "char") return true;
        return is_int_kind(scalar_kind(t));
    }

    void check_native_no_array(ast::FuncDecl& f)
    {
        auto arr = [](ast::TypeUse const& t) { return t.is_array; };
        for (auto& p : f.params)
            if (arr(p.type))
                unsup("array parameters are not supported on the native backend: '" + f.name + "'");
        if (f.ret && arr(*f.ret))
            unsup("array return types are not supported on the native backend: '" + f.name + "'");
        std::function<void(ast::Stmt*)> walk = [&](ast::Stmt* s) {
            if (!s) return;
            switch (s->kind()) {
            case ast::Stmt::Block:
                for (auto& st : static_cast<ast::BlockStmt*>(s)->stmts) walk(st.get());
                break;
            case ast::Stmt::Var: {
                auto& v = static_cast<ast::VarStmt&>(*s);
                if (v.has_type && arr(v.type) && !native_array_elem_ok(v.type))
                    unsup("native array elements must be int-family/bool/double/char: '"
                          + v.name + "'");
                break;
            }
            case ast::Stmt::If: {
                auto& i = static_cast<ast::IfStmt&>(*s);
                walk(i.then_block.get());
                walk(i.else_block.get());
                break;
            }
            case ast::Stmt::While:
                walk(static_cast<ast::WhileStmt&>(*s).body.get());
                break;
            case ast::Stmt::For:
                walk(static_cast<ast::ForStmt&>(*s).body.get());
                break;
            case ast::Stmt::Match:
                for (auto& arm : static_cast<ast::MatchStmt&>(*s).arms) walk(arm.body.get());
                break;
            default: break;
            }
        };
        if (f.has_block_body) walk(f.block_body.get());
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
        check_legacy_blocks();               // 钩子:Win64 mini-C 解析;默认不支持
        for (auto& s : m_->structs) {
            for (auto& f : s.fields) {
                if (f.type.is_array)
                    unsup("struct field '" + f.name + "': array fields are not supported "
                          "on the native backend");
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
        // concept 允许：纯声明无代码形态，约束检查在 sema 完成，发射期直接略过
        for (auto& f : m_->funcs) { check_func(f); check_native_no_map(f); check_native_no_array(f); }
        for (auto& g : m_->globals) check_global(g);
    }

    ast::StructDecl* find_sd(std::string const& n) const
    {
        for (auto& s : m_->structs) if (s.name == n) return &s;
        return nullptr;
    }

    // 是否带虚表:自身或任一祖先声明了 virtual 方法(M7)。
    // 带虚表的对象布局 = [vptr][field0][field1]...,字段偏移整体 +8
    bool has_vtable(ast::StructDecl const* sd) const
    {
        while (sd) {
            for (auto& m : sd->methods) if (m.is_virtual) return true;
            sd = (sd->base && !sd->base->parts.empty()) ? find_sd(sd->base->parts[0]) : nullptr;
        }
        return false;
    }

    // vptr 垫片字节数(基类钩子:Win64 虚分发返回 8,SysV v0 不支持虚方法返回 0)
    virtual int vptr_pad(ast::StructDecl const* sd) const { (void)sd; return 0; }

    // cxx_legacy 块处理(M6):基类不支持;Win64 覆写为 mini-C 解析
    virtual void check_legacy_blocks() {
        if (!m_->legacy_blocks.empty()) unsup("cxx_legacy blocks");
    }

    // 全字段视图(基类在前),StructLit 初始化/对象拷贝用;second = 字节偏移
    // 带虚表的类型首槽为 vptr,字段偏移整体 +8(vptr_pad)
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
        int pad = vptr_pad(sd);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            for (auto& f : (*it)->fields)
                out.push_back({&f, pad + (int)out.size() * 8});
        for (auto& f : sd->fields)
            out.push_back({&f, pad + (int)out.size() * 8});
        return out;
    }

    int field_offset_in(ast::StructDecl* sd, std::string const& fname)
    {
        int off = vptr_pad(sd);                   // vptr 占据对象首槽
        std::string bn = (sd->base && !sd->base->parts.empty()) ? sd->base->parts[0] : "";
        while (!bn.empty()) {
            ast::StructDecl* bs = find_sd(bn);
            if (!bs) break;
            for (size_t i = 0; i < bs->fields.size(); ++i)
                if (bs->fields[i].name == fname) return off + (int)i * 8;
            off += (int)bs->fields.size() * 8;
            bn = (bs->base && !bs->base->parts.empty()) ? bs->base->parts[0] : "";
        }
        for (size_t i = 0; i < sd->fields.size(); ++i)
            if (sd->fields[i].name == fname) return off + (int)i * 8;
        return -1;
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

    int slot_or_new(std::string const& n)
    {
        auto it = slots_.find(n);
        if (it != slots_.end()) return it->second;
        int idx = (int)slots_.size();
        slots_[n] = -(8 * (idx + 1));
        return slots_[n];
    }

    void emit_all_dtors_for_return(std::string const& skip = {})
    {
        for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it)
            for (auto jt = it->rbegin(); jt != it->rend(); ++jt)
                if (skip.empty() || jt->first != skip) emit_dtor_call(*jt);
        for (auto& f : scope_stack_) f.clear();
    }

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
        case K::Double: case K::Float: return dbl_fmt();
        case K::Bool:   return "%d";
        default:        return "%lld";
        }
    }

    // ── 共享状态 ──
    ast::Module* m_ = nullptr;
    sema::Result const* R_ = nullptr;
    std::string src_path_;   // 源文件路径(err() 位置后缀用)
    std::ostringstream out_;
    int label_ = 0;
    int push_depth_ = 0;
    ast::FuncDecl* cur_fn_ = nullptr;
    std::string cur_sym_name_;      // 当前发射符号(func 或 Struct_method)
    std::unordered_map<std::string, int> slots_;
    std::vector<std::string> break_labels_;
    std::vector<std::string> continue_labels_;
    std::unordered_map<std::string, std::string> str_pool_;
    std::set<std::string> inout_params_;
    // 泛型单态化上下文(发射实例函数时由派生类填充/保存恢复)
    std::map<std::string, sema::Type> mono_map_;         // 类型参数名 → 实参具体类型
    std::map<std::string, sema::Type> mono_var_types_;   // ":= 泛型调用" 变量 → 结果类型
    ast::StructDecl* cur_struct_ = nullptr;   // 方法发射时的所属类型(经 this 字段访问)
    std::vector<std::vector<DtorEntry>> scope_stack_;
};

} // namespace cpp2::native
