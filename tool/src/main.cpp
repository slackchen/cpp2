// cpp2 命令行工具(M2e:build / run / check / transpile / export-headers / audit / fuzz)
#include "ast.hpp"
#include "audit.hpp"
#include "diagfilter.hpp"
#include "emit.hpp"
#include "fuzz.hpp"
#include "lexer.hpp"
#include "modules.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "sha256.hpp"
#include "toolchain.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace ast   = cpp2::ast;
namespace lex   = cpp2::lex;
namespace parse = cpp2::parse;
namespace sema  = cpp2::sema;
namespace emit  = cpp2::emit;
namespace mods  = cpp2::mods;
namespace util  = cpp2::util;
namespace sha256 = cpp2::sha256;
namespace diagfilter = cpp2::diagfilter;
namespace tc    = cpp2::toolchain;
namespace audit = cpp2::audit;
namespace fuzz  = cpp2::fuzz;

namespace {

constexpr char const* kVersion = "cpp2 0.1.0-m6 (M1-M6 complete: lifetime Lite L1-L6, arena/gc, legacy+zlib interop)";

std::optional<std::string> read_file(fs::path const& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(fs::path const& p, std::string const& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

void print_diag(std::string const& file, char const* sev, int line, int col, std::string const& msg)
{
    std::cerr << file << ":" << line << ":" << col << ": " << sev << ": " << msg << "\n";
}

std::string quote(std::string const& s) { return "\"" + s + "\""; }

// 运行命令并捕获输出(编译失败时做后端诊断过滤,替代直通 system)
struct CmdResult { bool ok; std::string output; };

#ifdef _WIN32
inline int sys_rc(int rc) { return rc; }            // Windows:已是进程退出码
#else
#include <sys/wait.h>
inline int sys_rc(int rc)                           // POSIX:16 位 waitstatus 解码
{
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc); // trap(abort)= 134 等
    return rc;
}
#endif

CmdResult run_capture(std::string const& cmd)
{
    // 诊断走 stderr:合并后再捕获,否则过滤形同虚设
#if defined(_WIN32)
    FILE* p = _popen((cmd + " 2>&1").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if (!p) return { false, "" };
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
#if defined(_WIN32)
    int rc = _pclose(p);
#else
    int rc = pclose(p);
#endif
    return { sys_rc(rc) == 0, std::move(out) };
}

std::string native(fs::path p)
{
#ifdef _WIN32
    std::string s = p.string();
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
#else
    return p.string();                      // POSIX:正斜杠原样
#endif
}

// ── 编译准备:模块图 + 逐模块语义检查 ─────────────────────────────
struct Prepared {
    mods::Graph graph;
    std::unordered_map<std::string, sema::Result> sema;   // 模块名 → 结果
    std::vector<emit::ModuleEntry> entries() const;
};

std::vector<emit::ModuleEntry> Prepared::entries() const
{
    std::vector<emit::ModuleEntry> out;
    for (auto const& name : graph.order) {
        auto const& u = graph.units.at(name);
        emit::ModuleEntry e;
        e.m = const_cast<ast::Module*>(&u.ast);           // 发射只读使用
        e.r = &sema.at(name);
        e.src_name = u.file.string();
        e.imports = u.imports;
        e.is_root = (name == graph.root_name);
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<Prepared> prepare(fs::path const& root, bool quick = false)
{
    Prepared p;
    try {
        p.graph = mods::load(root);
    } catch (mods::GraphError const& e) {
        print_diag(e.file, "error", e.line > 0 ? e.line : 1, 1, e.msg);
        return std::nullopt;
    }

    // 跨模块导出名冲突检测(headers 后端导出名落全局命名空间,冲突即链接错误)
    {
        std::unordered_map<std::string, std::string> owner;   // 导出名 → 模块
        bool clash = false;
        auto claim = [&](std::string const& mod, std::string const& n, int line) {
            auto it = owner.find(n);
            if (it != owner.end() && it->second != mod) {
                print_diag(p.graph.units.at(mod).file.string(), "error",
                           line > 0 ? line : 1, 1,
                           "exported name '" + n + "' is also exported by module '"
                           + it->second + "' (headers backend requires cross-module "
                           "uniqueness)");
                clash = true;
            } else {
                owner.emplace(n, mod);
            }
        };
        for (auto const& name : p.graph.order) {
            auto const& m = p.graph.units.at(name).ast;
            for (auto const& e : m.enums)    if (e.exported) claim(name, e.name, e.line);
            for (auto const& s : m.structs)  if (s.exported) claim(name, s.name, s.line);
            for (auto const& v : m.variants) if (v.exported) claim(name, v.name, v.line);
            for (auto const& c : m.concepts) if (c.exported) claim(name, c.name, c.line);
            for (auto const& g : m.globals)  if (g.exported) claim(name, g.name, g.line);
            for (auto const& f : m.funcs)
                if (f.exported && f.name != "main") claim(name, f.name, f.line);
        }
        if (clash) return std::nullopt;
    }

    for (auto const& name : p.graph.order) {
        if (quick && name != p.graph.root_name) continue;   // --quick:仅根模块检查
        auto const& u = p.graph.units.at(name);
        std::vector<ast::Module*> imported;
        for (auto const& dep : u.imports)
            imported.push_back(const_cast<ast::Module*>(&p.graph.units.at(dep).ast));
        auto r = sema::check(const_cast<ast::Module&>(u.ast), imported);
        std::string file = u.file.string();
        for (auto const& w : r.warnings)
            print_diag(file, "warning", w.line, w.col > 0 ? w.col : 1, w.msg);
        if (!r.ok()) {
            for (auto const& e : r.errors)
                print_diag(file, "error", e.line, e.col > 0 ? e.col : 1, e.msg);
            return std::nullopt;
        }
        p.sema.emplace(name, std::move(r));
    }
    return p;
}

// ── 工具链发现 ────────────────────────────────────────────────────
std::string find_compiler()
{
    if (char const* env = std::getenv("CPP2_CXX")) return env;
    for (char const* cxx : {"g++", "clang++"}) {
        std::string cmd = std::string(cxx) + " --version > NUL 2>&1";
        if (std::system(cmd.c_str()) == 0) return cxx;
    }
    return "";
}

std::optional<fs::path> find_rt_dir(fs::path const& input)
{
    std::vector<fs::path> candidates;
    if (char const* env = std::getenv("CPP2_RT")) candidates.emplace_back(env);
    fs::path exe_dir = fs::absolute(fs::path(std::getenv("CPP2_EXE_DIR") ? std::getenv("CPP2_EXE_DIR") : "."));
    candidates.push_back(exe_dir / ".." / ".." / "rt");
    candidates.push_back(fs::current_path() / "rt");
    for (fs::path d = fs::absolute(input).parent_path(); ; d = d.parent_path()) {
        candidates.push_back(d / "rt");
        if (!d.has_parent_path() || d == d.parent_path()) break;
    }
    for (auto const& c : candidates)
        if (fs::exists(c / "cpp2" / "support.hpp"))
            return fs::weakly_canonical(c);
    return std::nullopt;
}

// ── .cpp2cache:每模块缓存记录(.c2i 格式 v1,冻结)─────────────────
// 布局(小端):magic "C2IF" | u32 version=1 | u32 name_len + 模块名 |
//   4 × 32B SHA-256(src / iface / gen / deps)| u64 len + 接口规范化文本
// magic 或版本不符 → 视为无缓存(旧文本格式自然失效重建)。
struct CacheRec {
    std::string src_hash;       // 源文本哈希 → 是否需要重转译
    std::string iface_hash;     // 导出接口哈希 → 依赖者是否需要重编
    std::string gen_hash;       // 生成文件哈希
    std::string deps_hash;      // 直接依赖的 iface 组合
};

constexpr char kC2ifMagic[4] = {'C', '2', 'I', 'F'};
constexpr std::uint32_t kC2ifVersion = 1;

void put_u32(std::string& o, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) o.push_back(char((v >> (i * 8)) & 0xff));
}

void put_u64(std::string& o, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) o.push_back(char((v >> (i * 8)) & 0xff));
}

bool get_u32(std::string const& s, size_t& pos, std::uint32_t& v)
{
    if (pos + 4 > s.size()) return false;
    v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | std::uint8_t(s[pos + i]);
    pos += 4;
    return true;
}

bool get_u64(std::string const& s, size_t& pos, std::uint64_t& v)
{
    if (pos + 8 > s.size()) return false;
    v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | std::uint8_t(s[pos + i]);
    pos += 8;
    return true;
}

std::string cache_path(fs::path const& build_dir, std::string const& mod)
{
    return (build_dir / "cpp2cache" / (util::safe_name(mod) + ".c2i")).string();
}

std::optional<CacheRec> read_cache(fs::path const& build_dir, std::string const& mod)
{
    auto blob = read_file(cache_path(build_dir, mod));
    if (!blob || blob->size() < 8) return std::nullopt;
    if (std::memcmp(blob->data(), kC2ifMagic, 4) != 0) return std::nullopt;
    size_t pos = 4;
    std::uint32_t version = 0;
    if (!get_u32(*blob, pos, version) || version != kC2ifVersion) return std::nullopt;

    std::uint32_t name_len = 0;
    if (!get_u32(*blob, pos, name_len) || pos + name_len > blob->size()) return std::nullopt;
    std::string name = blob->substr(pos, name_len);
    pos += name_len;
    if (name != mod) return std::nullopt;               // 安全名碰撞防护

    auto take_hash = [&](std::string& out) -> bool {
        if (pos + 32 > blob->size()) return false;
        out = sha256::hex_of_bytes(blob->data() + pos);
        pos += 32;
        return true;
    };
    CacheRec r;
    if (!take_hash(r.src_hash) || !take_hash(r.iface_hash)
        || !take_hash(r.gen_hash) || !take_hash(r.deps_hash)) return std::nullopt;

    std::uint64_t text_len = 0;
    if (!get_u64(*blob, pos, text_len) || pos + text_len > blob->size()) return std::nullopt;
    pos += text_len;                                    // 接口文本:调试可读,哈希已含
    return r;
}

void write_cache(fs::path const& build_dir, std::string const& mod, CacheRec const& r,
                 std::string const& iface_text)
{
    auto put_hash = [](std::string& o, std::string const& hex32) {
        auto d = sha256::digest_from_hex(hex32);
        o.append(reinterpret_cast<char const*>(d.data()), d.size());
    };
    std::string o;
    o.append(kC2ifMagic, 4);
    put_u32(o, kC2ifVersion);
    put_u32(o, static_cast<std::uint32_t>(mod.size()));
    o += mod;
    put_hash(o, r.src_hash);
    put_hash(o, r.iface_hash);
    put_hash(o, r.gen_hash);
    put_hash(o, r.deps_hash);
    put_u64(o, iface_text.size());
    o += iface_text;
    write_file(cache_path(build_dir, mod), o);
}

// ── 子命令 ────────────────────────────────────────────────────────
int cmd_transpile(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 transpile <root.cpp2> [-o out.cpp] [--release]\n";
        return 1;
    }
    fs::path in = args[0];
    fs::path out;
    bool release = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--release") release = true;
    }
    if (out.empty()) out = in.parent_path() / ".cpp2build" / (in.stem().string() + ".cpp");

    auto p = prepare(in);
    if (!p) return 1;
    auto code = emit::emit_flatten(p->entries(), release);
    if (!write_file(out, code)) {
        std::cerr << "error: cannot write '" << out.string() << "'\n";
        return 1;
    }
    std::cout << out.string() << "\n";
    return 0;
}

// ── cpp2 check:快速语义检查,不生成代码(IMPLEMENTATION §2)────────
// --quick:只检查根模块(依赖仅作符号表,跳过其检查)——大图快速反馈
int cmd_check(std::vector<std::string> const& args)
{
    bool quick = false;
    std::string file;
    for (auto const& a : args) {
        if (a == "--quick") quick = true;
        else file = a;
    }
    if (file.empty()) {
        std::cerr << "usage: cpp2 check <root.cpp2> [--quick]\n";
        return 1;
    }
    fs::path in(file);
    auto p = quick ? prepare(in, /*quick*/true) : prepare(in);
    if (!p) return 1;
    std::cout << p->graph.order.size() << " module(s) ok"
              << (quick ? " (quick)" : "") << "\n";
    return 0;
}

int cmd_run(std::vector<std::string> const& args)
{    bool release = false;
    std::string file;
    for (auto const& a : args) {
        if (a == "--release") release = true;
        else file = a;
    }
    if (file.empty()) {
        std::cerr << "usage: cpp2 run <root.cpp2> [--release]\n";
        return 1;
    }
    fs::path in(file);
    std::string cxx = find_compiler();
    if (cxx.empty()) {
        std::cerr << "error: no C++ compiler found (set CPP2_CXX)\n";
        return 2;
    }
    auto rt = find_rt_dir(in);
    if (!rt) {
        std::cerr << "error: cannot locate rt/ directory (set CPP2_RT)\n";
        return 2;
    }
    auto p = prepare(in);
    if (!p) return 1;

    auto code = emit::emit_flatten(p->entries(), release);
    fs::path build_dir = in.parent_path() / ".cpp2build";
    fs::path cpp = build_dir / (in.stem().string() + ".cpp");
    fs::path exe = build_dir / in.stem();
    write_file(cpp, code);

    std::string exe_s = native(exe);
    std::string cc = cxx + " -std=c++23 -O0 -g -I" + quote(native(*rt))
                   + " " + quote(native(cpp)) + " -o " + quote(exe_s);
    std::cerr << "[cpp2] " << cc << "\n";
    auto r = run_capture(cc);
    if (!r.ok) {
        std::cerr << diagfilter::banner;
        std::string f = diagfilter::filter(r.output, native(build_dir));
        // 过滤器一无所获时回显原始输出(诊断不能被静默吞掉)
        std::cerr << (f.empty() ? r.output : f);
        std::cerr << "error: compilation failed\n";
        return 2;
    }
    return sys_rc(std::system(quote(exe_s).c_str()));
}

// ── cpp2 build:并行增量构建 ───────────────────────────────────────
// backend=headers(默认):每模块 .h 接口 + 实现片段按 TU 大小预算装箱,
//   普通 TU 并行编译,不依赖 C++20 modules;
// backend=cxx20-modules:每模块 named module + BMI(M3 原路径)。
int cmd_build(std::vector<std::string> const& args)
{
    std::string backend = "headers";
    long long max_tu = 1024 * 1024;   // 单 TU 生成码预算(字节);横向实测最优,见 IMPL M3b
    bool release = false;
    int max_jobs = (int)std::thread::hardware_concurrency();
    if (max_jobs < 1) max_jobs = 2;
    fs::path in;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rfind("--max-jobs=", 0) == 0) { max_jobs = std::max(1, std::stoi(args[i].substr(11))); continue; }
        if (args[i] == "--backend=headers")     { backend = "headers"; continue; }
        if (args[i].rfind("--backend=", 0) == 0){ backend = args[i].substr(10); continue; }
        if (args[i].rfind("--max-tu-size=", 0) == 0) { max_tu = std::stoll(args[i].substr(14)); continue; }
        if (args[i] == "--release")             { release = true; continue; }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "error: unknown option '" << args[i] << "'\n";
            return 1;
        }
        in = args[i];
    }
    if (backend != "headers" && backend != "cxx20-modules") {
        std::cerr << "error: unknown backend '" << backend
                  << "' (headers | cxx20-modules)\n";
        return 1;
    }
    if (in.empty()) {
        for (char const* cand : {"main.cpp2", "app.cpp2"}) {
            if (fs::exists(cand)) { in = cand; break; }
        }
    }
    if (in.empty()) {
        std::cerr << "usage: cpp2 build <root.cpp2> [--backend=headers|cxx20-modules]"
                     " [--max-tu-size=N]\n";
        return 1;
    }

    std::string cxx = find_compiler();
    if (cxx.empty()) {
        std::cerr << "error: no C++ compiler found (set CPP2_CXX)\n";
        return 2;
    }
    auto rt = find_rt_dir(in);
    if (!rt) {
        std::cerr << "error: cannot locate rt/ directory (set CPP2_RT)\n";
        return 2;
    }
    auto p = prepare(in);
    if (!p) return 1;

    // 转译(内容寻址落盘)+ 哈希缓存;两后端构建目录隔离,互不污染
    auto& g = p->graph;
    bool headers_backend = backend == "headers";
    fs::path build_dir = in.parent_path() / ".cpp2build" / (headers_backend ? "hdr" : "mods");
    fs::path bmi_dir = build_dir / "bmi";
    fs::path obj_dir = build_dir / "obj";
    fs::create_directories(build_dir);
    fs::create_directories(obj_dir);
    if (!headers_backend) fs::create_directories(bmi_dir);

    std::unordered_map<std::string, CacheRec> recs;
    std::unordered_map<std::string, bool> h_changed;      // 接口头(.h)是否重写
    std::unordered_map<std::string, bool> f_changed;      // 实现片段是否变化
    int transpiled = 0, cached = 0;
    std::unordered_map<std::string, std::string> iface;   // name → 接口哈希

    // headers 后端:模块名 → 实现片段(内存持有,装箱阶段使用)
    std::unordered_map<std::string, std::string> frags;

    for (auto const& name : g.order) {
        auto const& u = g.units.at(name);
        // 源哈希混入工具版本:代码生成器变更后旧缓存自然失效,
        // 避免 emit 演进(如 M4 的空检查注入)被缓存掩盖
        std::string src_hash = sha256::hex_of(u.source + "|codegen:" + kVersion);

        emit::ModuleEntry e;
        for (auto const& en : p->entries())
            if (en.m->name == name) { e = en; break; }

        auto old = read_cache(build_dir, name);

        if (headers_backend) {
            auto [h_str, frag] = emit::emit_headers(e, release);
            std::string h_hash = sha256::hex_of(h_str);
            std::string gen_hash = sha256::hex_of(frag);
            fs::path h_path = build_dir / (util::safe_name(name) + ".h");
            bool hc = !old || old->iface_hash != h_hash || !fs::exists(h_path);
            bool fc = !old || old->gen_hash != gen_hash;
            if (hc) write_file(h_path, h_str);            // 内容寻址:字节不变不落盘
            CacheRec r;
            r.src_hash = src_hash;
            r.iface_hash = h_hash;
            r.gen_hash = gen_hash;
            recs[name] = r;
            h_changed[name] = hc;
            f_changed[name] = fc;
            iface[name] = h_hash;
            frags[name] = std::move(frag);
        } else {
            fs::path gen = build_dir / (util::safe_name(name) + ".cpp");
            if (old && old->src_hash == src_hash && fs::exists(gen)) {
                recs[name] = *old;
                h_changed[name] = false;
                f_changed[name] = false;
                iface[name] = old->iface_hash;
                ++cached;
                continue;
            }
            auto code = emit::emit_module_unit(e, release);
            write_file(gen, code);
            CacheRec r;
            r.src_hash = src_hash;
            r.iface_hash = sha256::hex_of(mods::interface_text(u.ast));
            r.gen_hash = sha256::hex_of(code);
            recs[name] = r;
            h_changed[name] = true;
            f_changed[name] = true;
            iface[name] = r.iface_hash;
            ++transpiled;
            continue;
        }
        if (h_changed[name] || f_changed[name]) ++transpiled; else ++cached;
    }

    // 依赖组合哈希(.c2i 记录;headers 后端的编译判定用内容寻址,此为记录性)
    // 组合串先归约为 SHA-256 hex 再入缓存:.c2i 中哈希存原始字节,
    // 只有合法 64 位 hex 能无损往返(空串/任意文本会失真为全零摘要)
    for (auto const& name : g.order) {
        std::string joined = "deps1";
        for (auto const& dep : g.units.at(name).imports) joined += "|" + iface.at(dep);
        recs[name].deps_hash = sha256::hex_of(joined);
    }

    std::atomic<int> failures{0};
    std::atomic<int> compiled{0};

    // 编译器矩阵(M4):按家族分派编译参数
    tc::Family fam = tc::detect(cxx);
    std::string bmi_ext = tc::bmi_extension(fam);
    std::cerr << "[cpp2] compiler: " << cxx << " (" << tc::family_name(fam)
              << " family), backend: " << backend << "\n";

    std::vector<std::string> obj_files;                   // 链接输入(两后端各自填充)

    if (headers_backend) {
        // ── 装箱:实现片段按 TU 预算贪心合并(拓扑序,确定性划分)──
        // 尽可能少的 TU,但绝不成单个巨型文件;超大模块独占 TU。
        struct Part { std::vector<std::string> mods; size_t bytes = 0; };
        std::vector<Part> parts;
        for (auto const& name : g.order) {
            if (!parts.empty() && parts.back().bytes + frags[name].size() <= (size_t)max_tu) {
                parts.back().mods.push_back(name);
                parts.back().bytes += frags[name].size();
            } else {
                parts.push_back({{name}, frags[name].size()});
            }
        }

        // 头依赖闭包:part 依赖成员 .h 及其传递 import 的 .h
        std::function<void(std::string const&, std::unordered_set<std::string>&)> collect =
            [&](std::string const& m, std::unordered_set<std::string>& out) {
                if (!out.insert(m).second) return;
                for (auto const& dep : g.units.at(m).imports) collect(dep, out);
            };

        struct PartJob { std::string name; fs::path obj; };
        std::vector<PartJob> jobs;
        int idx = 0;
        for (auto const& part : parts) {
            // 头依赖闭包:成员 + 传递 import;include 平铺在 part 顶部,
            // 使 .h 内部的嵌套 include 命中 #pragma once 短路——
            // 链式深图(m999→…→m0)否则会产生千层 include 嵌套(超编译器上限)
            std::unordered_set<std::string> closure;
            bool any_h_changed = false;
            for (auto const& m : part.mods) collect(m, closure);
            std::vector<std::string> incs(closure.begin(), closure.end());
            std::sort(incs.begin(), incs.end());
            std::string text = "// Generated by cpp2c (headers backend). DO NOT EDIT.\n";
            for (auto const& m : incs)
                text += "#include \"" + util::safe_name(m) + ".h\"\n";
            for (auto const& m : incs) any_h_changed |= h_changed[m];
            text += "\n";
            for (auto const& m : part.mods) text += frags[m];

            std::string pname = "c2_part" + std::to_string(idx++);
            fs::path cpp = build_dir / (pname + ".cpp");
            fs::path obj = obj_dir / (pname + ".o");
            auto old_txt = read_file(cpp);
            bool text_changed = !old_txt || *old_txt != text;
            if (text_changed) write_file(cpp, text);

            bool needed = !fs::exists(obj) || text_changed || any_h_changed;
            if (needed) fs::remove(obj);
            jobs.push_back({pname, obj});
        }

        // part 间无编译依赖(接口全在 .h)→ 并行编译
        // 并发上限默认 = 硬件线程数(--max-jobs 覆盖):低配 runner 全量并发会 OOM
        std::mutex err_mtx;
        int live = 0;
        std::mutex live_mtx;
        std::condition_variable slot_cv;
        std::vector<std::thread> workers;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (fs::exists(jobs[i].obj)) continue;    // 未被判定为 needed
            {
                std::unique_lock<std::mutex> lk(live_mtx);
                slot_cv.wait(lk, [&] { return live < max_jobs; });
                ++live;
            }
            workers.emplace_back([&, i] {
                fs::path cpp = build_dir / (jobs[i].name + ".cpp");
                std::string cc = tc::plain_compile_command(
                    cxx, fam, native(*rt), native(cpp), native(jobs[i].obj));
                std::cerr << "[cpp2] " << cc << "\n";
                auto r = run_capture(cc);
                {
                    std::lock_guard<std::mutex> lk(live_mtx);
                    --live;
                }
                slot_cv.notify_all();
                if (!r.ok) {
                    std::lock_guard<std::mutex> lk(err_mtx);
                    std::cerr << diagfilter::banner;
                    std::string flt = diagfilter::filter(r.output, native(build_dir));
                    std::cerr << (flt.empty() ? r.output : flt);
                    ++failures;
                    return;
                }
                ++compiled;
            });
        }
        for (auto& w : workers) w.join();

        for (auto const& j : jobs) obj_files.push_back(native(j.obj));
        if (failures > 0) {
            std::cerr << "error: compilation failed\n";
            return 2;
        }
    } else {
        // ── C++20 模块后端(M3 原路径):分层并行 + BMI ──────────────
        std::unordered_map<std::string, bool> need_compile;
        for (auto const& name : g.order) {
            fs::path obj = obj_dir / (util::safe_name(name) + ".o");
            need_compile[name] = !fs::exists(obj) || f_changed[name] || h_changed[name];
            if (need_compile[name]) fs::remove(obj);
        }

        // 分层并行编译(Kahn:依赖已编译即可入层)
        std::unordered_map<std::string, int> indeg;
        for (auto const& name : g.order) {
            int n = 0;
            for (auto const& dep : g.units.at(name).imports)
                if (need_compile.at(dep)) ++n;
            indeg[name] = n;
        }

        std::mutex err_mtx;

        std::vector<std::string> remaining = g.order;
        while (!remaining.empty()) {
            std::vector<std::string> level;
            for (auto it = remaining.begin(); it != remaining.end();) {
                if (!need_compile.at(*it) || indeg.at(*it) == 0) {
                    level.push_back(*it);
                    it = remaining.erase(it);
                } else {
                    ++it;
                }
            }
            if (level.empty()) {
                std::cerr << "error: internal: compilation scheduling stalled\n";
                return 2;
            }

            std::vector<std::thread> workers;
            for (auto const& name : level) {
                if (!need_compile.at(name)) continue;
            workers.emplace_back([&, name] {
                fs::path gen = build_dir / (util::safe_name(name) + ".cpp");
                fs::path obj = obj_dir / (util::safe_name(name) + ".o");

                // BMI 映射需传递闭包:加载本模块 PCM 时,clang 还要解析其
                // 传递 import 的 PCM(深层链式图只给直接依赖会找不到模块)
                std::unordered_set<std::string> closure;
                std::function<void(std::string const&)> collect =
                    [&](std::string const& m) {
                        if (!closure.insert(m).second) return;
                        for (auto const& d : g.units.at(m).imports) collect(d);
                    };
                collect(name);
                std::vector<std::pair<std::string, std::string>> deps;
                for (auto const& dep : g.order)
                    if (dep != name && closure.count(dep))
                        deps.push_back({dep,
                            native(bmi_dir / (util::safe_name(dep) + bmi_ext))});

                std::string cc = tc::compile_command(
                    cxx, fam, native(*rt), deps, native(gen), native(obj),
                    /*is_interface*/ name != g.root_name,
                    native(bmi_dir / (util::safe_name(name) + bmi_ext)));

                // Windows 命令行长度上限:std::system 经 cmd.exe 仅 8191 字符,
                // BMI 闭包随模块数增长,超限改走响应文件(clang/gcc/msvc 均支持 @file;
                // 响应文件中反斜杠是转义符,路径一律用正斜杠)
                if (cc.size() > 7000) {
                    fs::path rsp = build_dir / (util::safe_name(name) + ".rsp");
                    std::string args = cc.substr(cxx.size() + 1);
                    for (auto& c : args) if (c == '\\') c = '/';
                    write_file(rsp, args);
                    cc = cxx + " @" + quote(native(rsp));
                }

                std::cerr << "[cpp2] " << cc << "\n";
                auto r = run_capture(cc);
                if (!r.ok) {
                    std::lock_guard<std::mutex> lk(err_mtx);
                    std::cerr << diagfilter::banner;
                    std::string flt2 = diagfilter::filter(r.output, native(build_dir));
                    std::cerr << (flt2.empty() ? r.output : flt2);
                    ++failures;
                    return;
                }
                ++compiled;
                });
            }
            for (auto& w : workers) w.join();
            if (failures > 0) {
                std::cerr << "error: compilation failed\n";
                return 2;
            }
            // 层完成:从剩余节点的入度中扣除本层
            for (auto const& done : level)
                for (auto const& [n, u] : g.units)
                    for (auto const& dep : u.imports)
                        if (dep == done && need_compile.at(n)) indeg[n] = std::max(0, indeg[n] - 1);
        }

        for (auto const& name : g.order)
            obj_files.push_back(native(obj_dir / (util::safe_name(name) + ".o")));
    }

    // 链接
    fs::path exe = in.parent_path() / ".cpp2build" / in.stem();
    std::string link = tc::link_command(cxx, fam, obj_files, native(exe));
    std::cerr << "[cpp2] " << link << "\n";
    if (std::system(link.c_str()) != 0) {
        std::cerr << "error: link failed\n";
        return 2;
    }

    // 写回缓存
    for (auto const& name : g.order) {
        auto const& u = g.units.at(name);
        write_cache(build_dir, name, recs[name], mods::interface_text(u.ast));
    }

    std::cerr << "[cpp2] build ok: " << transpiled << " transpiled, " << cached
              << " cached, " << compiled.load() << " compiled -> "
              << native(exe) << "\n";
    return 0;
}

// ── cpp2 export-headers:生成 Cpp1 桥接 .h/.cpp ───────────────────
int cmd_export_headers(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 export-headers <root.cpp2> [-o dir]\n";
        return 1;
    }
    fs::path in = args[0];
    fs::path out_dir;
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "-o") out_dir = args[i + 1];
    if (out_dir.empty()) out_dir = in.parent_path() / "cxx_bridge";

    auto p = prepare(in);
    if (!p) return 1;

    std::string libname = util::safe_name(p->graph.root_name);
    if (libname.empty()) libname = util::safe_name(in.stem().string());
    auto [h, cpp] = emit::emit_bridge(p->entries(), libname);

    fs::path hp = out_dir / (libname + ".h");
    fs::path cp = out_dir / (libname + ".cpp");
    if (!write_file(hp, h) || !write_file(cp, cpp)) {
        std::cerr << "error: cannot write bridge files into '" << out_dir.string() << "'\n";
        return 1;
    }
    std::cout << hp.string() << "\n" << cp.string() << "\n";
    return 0;
}

// ── cpp2 audit:安全审计报告(DESIGN §6.6 白纸黑字)───────────────
// 检查注入点计数 + 全部 @unsafe/@unchecked 位置;有诊断错误时退出非零
int cmd_audit(std::vector<std::string> const& args)
{
    if (args.empty() || (!args[0].empty() && args[0][0] == '-')) {
        std::cerr << "usage: cpp2 audit <root.cpp2>\n";
        return 1;
    }
    fs::path in = args[0];
    auto p = prepare(in);
    if (!p) return 1;

    int arith = 0, index = 0, deref = 0, narrow = 0, contract = 0, invariant = 0;
    int unchecked = 0, unsafe = 0;
    for (auto const& name : p->graph.order) {
        auto const& u = p->graph.units.at(name);
        auto rep = audit::report_for(const_cast<ast::Module&>(u.ast), p->sema.at(name));
        std::cout << audit::format_section(name, u.file.string(), rep);
        arith += rep.checked_arith; index += rep.checked_index;
        deref += rep.checked_deref; narrow += rep.checked_narrow;
        contract += rep.checked_contract;
        invariant += rep.checked_invariant;
        unchecked += rep.unchecked(); unsafe += rep.unsafe();
    }
    std::cout << "audit: " << p->graph.order.size() << " module(s), checks: arith "
              << arith << " / index " << index << " / deref " << deref
              << " / narrow " << narrow << " / contract " << contract
              << " / invariant " << invariant
              << "; opt-outs: @unchecked x" << unchecked
              << ", @unsafe x" << unsafe << "\n";
    return 0;
}

// ── cpp2 fuzz:词法/语法/语义模糊测试(固定 seed 可复现)───────────
int cmd_fuzz(std::vector<std::string> const& args)
{
    std::vector<std::string> files;
    unsigned seed = 1;
    int iters = 2000;
    std::string crash_dir = ".cpp2build/fuzz";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seed" && i + 1 < args.size()) {
            seed = static_cast<unsigned>(std::stoul(args[++i]));
        } else if (args[i] == "--iters" && i + 1 < args.size()) {
            iters = std::stoi(args[++i]);
        } else if (args[i] == "--crash-dir" && i + 1 < args.size()) {
            crash_dir = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "error: unknown option '" << args[i] << "'\n";
            return 1;
        } else {
            files.push_back(args[i]);
        }
    }
    if (files.empty()) {
        std::cerr << "usage: cpp2 fuzz <corpus.cpp2...> [--seed N] [--iters N]"
                     " [--crash-dir dir]\n";
        return 1;
    }
    auto out = fuzz::run(files, seed, iters, crash_dir);
    std::cout << "fuzz: " << out.iterations << " iterations (seed " << seed
              << "), " << out.crashes << " crashes\n";
    return out.crashes == 0 ? 0 : 1;
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  cpp2 run <root.cpp2>                        # 摊平转译 + 编译 + 执行\n"
        << "  cpp2 check <root.cpp2>                      # 快速语义检查,不生成代码\n"
        << "  cpp2 build [root.cpp2]                      # 并行增量构建(默认 headers 后端)\n"
        << "      [--backend=headers|cxx20-modules] [--max-tu-size=N]\n"
        << "  cpp2 transpile <root.cpp2> [-o out.cpp]     # 摊平转译查看生成码\n"
        << "  cpp2 export-headers <root.cpp2> [-o dir]    # 生成 Cpp1 消费者 .h/.cpp\n"
        << "  cpp2 audit <root.cpp2>                      # 安全审计:检查点 + 退出点\n"
        << "  cpp2 fuzz <corpus...> [--seed N --iters N]  # 前端模糊测试\n"
        << "  cpp2 version\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "transpile")       return cmd_transpile(args);
    if (cmd == "check")           return cmd_check(args);
    if (cmd == "run")             return cmd_run(args);
    if (cmd == "build")           return cmd_build(args);
    if (cmd == "export-headers")  return cmd_export_headers(args);
    if (cmd == "audit")           return cmd_audit(args);
    if (cmd == "fuzz")            return cmd_fuzz(args);
    if (cmd == "version")         { std::cout << kVersion << "\n"; return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }

    std::cerr << "error: unknown command '" << cmd << "'\n";
    usage();
    return 1;
}
