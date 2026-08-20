// cpp2 命令行工具(M3:build / run / transpile / export-headers)
#include "ast.hpp"
#include "emit.hpp"
#include "lexer.hpp"
#include "modules.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "util.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

namespace {

constexpr char const* kVersion = "cpp2 0.1.0-m3 (modules + parallel build + bridge)";

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

std::string native(fs::path p)
{
    std::string s = p.string();
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
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

std::optional<Prepared> prepare(fs::path const& root)
{
    Prepared p;
    try {
        p.graph = mods::load(root);
    } catch (mods::GraphError const& e) {
        print_diag(e.file, "error", e.line > 0 ? e.line : 1, 1, e.msg);
        return std::nullopt;
    }
    for (auto const& name : p.graph.order) {
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

// ── .cpp2cache:每模块缓存记录(.c2i 文本种子)─────────────────────
struct CacheRec {
    std::string src_hash;       // 源文本哈希 → 是否需要重转译
    std::string iface_hash;     // 导出接口哈希 → 依赖者是否需要重编
    std::string gen_hash;       // 生成文件哈希
    std::string deps_hash;      // 直接依赖的 iface 组合
};

std::string cache_path(fs::path const& build_dir, std::string const& mod)
{
    return (build_dir / "cpp2cache" / (util::safe_name(mod) + ".c2i")).string();
}

std::optional<CacheRec> read_cache(fs::path const& build_dir, std::string const& mod)
{
    auto txt = read_file(cache_path(build_dir, mod));
    if (!txt) return std::nullopt;
    CacheRec r;
    std::istringstream in(*txt);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("--", 0) == 0) break;        // "--" 之后是接口文本,非键值
        std::istringstream ls(line);
        std::string key, val;
        if (ls >> key >> val) {
            if (key == "src")         r.src_hash = val;
            else if (key == "iface")  r.iface_hash = val;
            else if (key == "gen")    r.gen_hash = val;
            else if (key == "deps")   r.deps_hash = val;
        }
    }
    return r;
}

void write_cache(fs::path const& build_dir, std::string const& mod, CacheRec const& r,
                 std::string const& iface_text)
{
    std::ostringstream o;
    o << "module " << mod << "\n"
      << "src " << r.src_hash << "\n"
      << "iface " << r.iface_hash << "\n"
      << "gen " << r.gen_hash << "\n"
      << "deps " << r.deps_hash << "\n"
      << "--\n" << iface_text;
    write_file(cache_path(build_dir, mod), o.str());
}

// ── 子命令 ────────────────────────────────────────────────────────
int cmd_transpile(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 transpile <root.cppm> [-o out.cpp]\n";
        return 1;
    }
    fs::path in = args[0];
    fs::path out;
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "-o") out = args[i + 1];
    if (out.empty()) out = in.parent_path() / ".cpp2build" / (in.stem().string() + ".cpp");

    auto p = prepare(in);
    if (!p) return 1;
    auto code = emit::emit_flatten(p->entries());
    if (!write_file(out, code)) {
        std::cerr << "error: cannot write '" << out.string() << "'\n";
        return 1;
    }
    std::cout << out.string() << "\n";
    return 0;
}

int cmd_run(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 run <root.cppm>\n";
        return 1;
    }
    fs::path in = args[0];
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

    auto code = emit::emit_flatten(p->entries());
    fs::path build_dir = in.parent_path() / ".cpp2build";
    fs::path cpp = build_dir / (in.stem().string() + ".cpp");
    fs::path exe = build_dir / in.stem();
    write_file(cpp, code);

    std::string exe_s = native(exe);
    std::string cc = cxx + " -std=c++23 -O0 -g -I" + quote(native(*rt))
                   + " " + quote(native(cpp)) + " -o " + quote(exe_s);
    std::cerr << "[cpp2] " << cc << "\n";
    if (std::system(cc.c_str()) != 0) {
        std::cerr << "error: compilation failed\n";
        return 2;
    }
    return std::system(quote(exe_s).c_str());
}

// ── cpp2 build:C++20 模块模式 + 拓扑分层并行 + 增量 ────────────────
int cmd_build(std::vector<std::string> const& args)
{
    fs::path in;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "error: unknown option '" << args[i] << "'\n";
            return 1;
        }
        in = args[i];
    }
    if (in.empty()) {
        for (char const* cand : {"main.cppm", "app.cppm"}) {
            if (fs::exists(cand)) { in = cand; break; }
        }
    }
    if (in.empty()) {
        std::cerr << "usage: cpp2 build <root.cppm>\n";
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

    // 转译(带源哈希缓存)+ 接口哈希
    auto& g = p->graph;
    fs::path build_dir = in.parent_path() / ".cpp2build" / "mods";
    fs::path bmi_dir = build_dir / "bmi";
    fs::path obj_dir = build_dir / "obj";
    fs::create_directories(build_dir);
    fs::create_directories(bmi_dir);
    fs::create_directories(obj_dir);

    std::unordered_map<std::string, CacheRec> recs;
    std::unordered_map<std::string, bool> fresh;          // 本轮是否重转译
    int transpiled = 0, cached = 0;
    std::unordered_map<std::string, std::string> iface;   // name → 当前 iface hash

    for (auto const& name : g.order) {
        auto const& u = g.units.at(name);
        std::string src_hash = util::hash128(u.source);

        emit::ModuleEntry e;
        for (auto const& en : p->entries())
            if (en.m->name == name) { e = en; break; }

        auto old = read_cache(build_dir, name);
        fs::path gen = build_dir / (util::safe_name(name) + ".cpp");
        if (old && old->src_hash == src_hash && fs::exists(gen)) {
            recs[name] = *old;
            fresh[name] = false;
            iface[name] = old->iface_hash;
            ++cached;
        } else {
            auto code = emit::emit_module_unit(e);
            write_file(gen, code);
            CacheRec r;
            r.src_hash = src_hash;
            r.iface_hash = util::hash128(mods::interface_text(u.ast));
            r.gen_hash = util::hash128(code);
            recs[name] = r;
            fresh[name] = true;
            iface[name] = r.iface_hash;
            ++transpiled;
        }
    }

    // 依赖组合哈希 + 是否需要编译:
    //   obj 缺失 / 本轮源变化(重转译)/ 直接依赖的接口哈希变化
    std::unordered_map<std::string, bool> need_compile;
    for (auto const& name : g.order) {
        std::string dh;
        for (auto const& dep : g.units.at(name).imports) dh += iface.at(dep);
        auto& r = recs[name];
        bool dh_changed = r.deps_hash != dh;
        r.deps_hash = dh;

        fs::path obj = obj_dir / (util::safe_name(name) + ".o");
        need_compile[name] = !fs::exists(obj) || fresh[name] || dh_changed;
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

    std::atomic<int> failures{0};
    std::atomic<int> compiled{0};
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
                auto const& u = g.units.at(name);
                fs::path gen = build_dir / (util::safe_name(name) + ".cpp");
                fs::path obj = obj_dir / (util::safe_name(name) + ".o");

                std::string cc = cxx + " -std=c++23 -O1 -I" + quote(native(*rt));
                // C++20 named module:显式映射依赖 BMI(clang 形式 name=file)
                for (auto const& dep : u.imports)
                    cc += " -fmodule-file=" + dep + "="
                        + quote(native(bmi_dir / (util::safe_name(dep) + ".pcm")));
                if (name != g.root_name) {
                    // clang 按扩展名判定 TU 类型:.cpp 需显式声明为模块接口
                    cc += std::string(" -x c++-module -fmodule-output=")
                        + quote(native(bmi_dir / (util::safe_name(name) + ".pcm")));
                }
                cc += " -c " + quote(native(gen)) + " -o " + quote(native(obj));

                std::cerr << "[cpp2] " << cc << "\n";
                if (std::system(cc.c_str()) != 0) {
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

    // 链接
    std::string objs;
    for (auto const& name : g.order)
        objs += " " + quote(native(obj_dir / (util::safe_name(name) + ".o")));
    fs::path exe = in.parent_path() / ".cpp2build" / in.stem();
    std::string link = cxx + objs + " -o " + quote(native(exe));
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
        std::cerr << "usage: cpp2 export-headers <root.cppm> [-o dir]\n";
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

void usage()
{
    std::cerr
        << "usage:\n"
        << "  cpp2 run <root.cppm>                        # 摊平转译 + 编译 + 执行\n"
        << "  cpp2 build [root.cppm]                      # C++20 模块模式:并行增量构建\n"
        << "  cpp2 transpile <root.cppm> [-o out.cpp]     # 摊平转译查看生成码\n"
        << "  cpp2 export-headers <root.cppm> [-o dir]    # 生成 Cpp1 消费者 .h/.cpp\n"
        << "  cpp2 version\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "transpile")       return cmd_transpile(args);
    if (cmd == "run")             return cmd_run(args);
    if (cmd == "build")           return cmd_build(args);
    if (cmd == "export-headers")  return cmd_export_headers(args);
    if (cmd == "version")         { std::cout << kVersion << "\n"; return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }

    std::cerr << "error: unknown command '" << cmd << "'\n";
    usage();
    return 1;
}
