// 共享支撑实现:子进程/编译器发现/模块准备/缓存(.c2i v1)
#include "app.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
    #define CPP2_POPEN _popen
    #define CPP2_PCLOSE _pclose
#else
    #define CPP2_POPEN popen
    #define CPP2_PCLOSE pclose
    #include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace cpp2::app {


int sys_rc(int rc)
{
#ifdef _WIN32
    return rc;                                          // Windows:已是进程退出码
#else
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);     // trap(abort)= 134 等
    return rc;
#endif
}

CmdResult run_capture(std::string const& cmd)
{
    // 诊断走 stderr:合并后再捕获,否则过滤形同虚设
#if defined(_WIN32)
    FILE* p = CPP2_POPEN((cmd + " 2>&1").c_str(), "r");
#else
    FILE* p = CPP2_POPEN((cmd + " 2>&1").c_str(), "r");
#endif
    if (!p) return { false, "" };
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int rc = CPP2_PCLOSE(p);
    return { sys_rc(rc) == 0, std::move(out) };
}

std::string find_compiler()
{
    if (char const* env = std::getenv("CPP2_CXX")) return env;
    for (char const* cxx : {"g++", "clang++"}) {
        std::string cmd = std::string(cxx) + " --version > NUL 2>&1";
        if (std::system(cmd.c_str()) == 0) return cxx;
    }
    return "";
}

// ── native 混动 legacy DLL 的编译器选择(M11)────────────────────────
// 探测只看输出不挑 rc:-dumpmachine/--version 的非零退出不代表探测失败
namespace {

std::string probe_output(std::string const& cmd)
{
    if (FILE* p = CPP2_POPEN((cmd + " 2>&1").c_str(), "r")) {
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        CPP2_PCLOSE(p);
        return out;
    }
    return "";
}

std::string trim_copy(std::string s)
{
    auto not_space = [](unsigned char c) { return c != ' ' && c != '\t'
                                         && c != '\r' && c != '\n'; };
    while (!s.empty() && !not_space(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && !not_space(s[i])) ++i;
    return s.substr(i);
}

std::string lower_copy(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// -dumpmachine 自报目标三元组(编译器对自己目标的自述,跨机器通用)
std::string gcc_target(std::string const& cxx)
{
    return lower_copy(trim_copy(probe_output("\"" + cxx + "\" -dumpmachine")));
}

bool contains(std::string const& hay, char const* needle)
{
    return hay.find(needle) != std::string::npos;
}

// gcc/clang 家族可用性:三元组须为「原生 Windows x64 + GNU/MinGW ABI」。
// 三条拒绝各有明确的运行期故障形态,不是口味问题:
//   cygwin 宿主  → DLL 拖 cygwin1.dll,纯 native 进程内 SEH 展开必崩;
//   非 x86-64    → 32 位 DLL 无法装入 Win64 进程;
//   msvc 目标    → shared_link_command 发 gcc 式旗标(-static-libstdc++),
//                  msvc 目标驱动不识别,交由 MSVC 家族分支处理。
bool gnuish_x64_target(std::string const& t, std::string& why)
{
    if (t.empty())                { why = "未报目标三元组"; return false; }
    if (contains(t, "cygwin"))    { why = "Cygwin 宿主编译器(其 DLL 依赖 cygwin1.dll,"
                                          "纯 native 进程内异常展开不可用)"; return false; }
    if (!contains(t, "x86_64") && !contains(t, "amd64"))
                                  { why = "非 x86-64 目标 '" + t + "'(native exe 固定 Win64)"; return false; }
    if (!contains(t, "mingw") && !contains(t, "gnu"))
                                  { why = "MSVC 目标 '" + t + "'(gcc 式链接旗标不适用)"; return false; }
    return true;
}

// cmd.exe/cl 只认 Windows 路径,而工具的文件系统视角是 POSIX 宿主给的:
// cygwin 挂载 /cygdrive/e/...、MSYS 裸挂载 /e/...、WSL /mnt/e/...。
// 把命令串里这三种形式的绝对路径改写为「E:/...」;盘符后必须紧跟分隔符,
// 裸挂载形式另要求参数边界(串首/空白/引号/等号),避免误伤 /c、/Fo、
// /std: 这类旗标形参
std::string windows_paths(std::string s)
{
    auto is_drive = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    auto is_sep   = [](char c) { return c == '/' || c == '\\'; };
    auto upper    = [](char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; };

    for (char const* prefix : { "/cygdrive/", "/mnt/" }) {
        size_t plen = std::strlen(prefix);
        size_t at = 0;
        while ((at = s.find(prefix, at)) != std::string::npos) {
            if (at + plen + 2 < s.size() && is_drive(s[at + plen]) && is_sep(s[at + plen + 1])) {
                s.replace(at, plen + 1, std::string(1, upper(s[at + plen])) + ":");
                at += 2;
            } else {
                at += plen;
            }
        }
    }
    for (size_t at = 0; at + 2 < s.size(); ++at) {
        if (s[at] == '/' && is_drive(s[at + 1]) && is_sep(s[at + 2])
            && (at == 0 || s[at - 1] == ' ' || s[at - 1] == '\"' || s[at - 1] == '=')) {
            s.replace(at, 2, std::string(1, upper(s[at + 1])) + ":");
            ++at;
        }
    }
    return s;
}

} // namespace

NativeCxx find_native_compiler()
{
    NativeCxx r;
    auto note = [&r](std::string const& who, std::string const& why) {
        if (!r.rejected.empty()) r.rejected += "\n";
        r.rejected += "  " + who + ": " + why;
    };

    if (char const* env = std::getenv("CPP2_CXX")) {
        std::string cxx = env;
        if (tc::detect(cxx) == tc::Family::Msvc) return { cxx, "", std::move(r.rejected) };
        std::string why;
        if (gnuish_x64_target(gcc_target(cxx), why)) return { cxx, "", std::move(r.rejected) };
        note("CPP2_CXX=" + cxx, why);
    }
    for (char const* cxx : {"g++", "clang++"}) {
        std::string why;
        if (gnuish_x64_target(gcc_target(cxx), why)) return { cxx, "", std::move(r.rejected) };
        note(std::string(cxx) + "(PATH)", why);
    }

    // cl 已在 PATH(dev-prompt 环境):横幅自证身份(无 cl 时各 shell 的
    // "command not found" 文案均不含 "Microsoft")
    {
        std::string banner = probe_output("\"cl\" \"/?\"");
        if (contains(banner, "Microsoft")) return { "cl", "", std::move(r.rejected) };
        note("cl(PATH)", "未找到");
    }

    // vswhere 定位 VS/BuildTools(VS 官方发现接口;盘符/版本随机器自适应)
    if (char const* pf86 = std::getenv("ProgramFiles(x86)")) {
        fs::path vswhere = fs::path(pf86) / "Microsoft Visual Studio" / "Installer"
                                      / "vswhere.exe";
        if (fs::exists(vswhere)) {
            // `-products "*"` 必须带引号:popen 外壳可能是 POSIX sh,* 会被
            // 通配展开成工作目录文件列表,vswhere 收到垃圾参数后静默空返回
            std::string out = probe_output("\"" + native(vswhere) + "\" -latest -products \"*\""
                " -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                " -property installationPath");
            std::string install = trim_copy(out);
            size_t nl = install.find_first_of("\r\n");
            if (nl != std::string::npos) install.resize(nl);
            if (!install.empty()) {
                fs::path vcvars = fs::path(install) / "VC" / "Auxiliary" / "Build"
                                             / "vcvars64.bat";
                if (fs::exists(vcvars)) return { "cl", vcvars.string(), std::move(r.rejected) };
                note("vswhere " + install, "未找到 vcvars64.bat(缺 VC 工具集?)");
            } else {
                note("vswhere", "无启用 VC 工具集的 VS 实例");
            }
        } else {
            note("vswhere", "未安装(" + native(vswhere) + " 不存在)");
        }
    }
    return r;                                           // cxx 空 ⇒ 无可用原生编译器
}

std::string wrap_msvc_env(std::string const& vcvars, std::string const& cmd,
                          fs::path const& bat_out)
{
    if (vcvars.empty()) return cmd;
    // 命令与 bat 路径都改写为 cmd.exe 可消费的 Windows 路径(见 windows_paths);
    // 引号全落在文件内容里;bat 路径整体加引号,sh 与 cmd 两种 popen 外壳下
    // 均为单参数;/d 跳过 AutoRun,防用户脚本污染环境
    std::string bat = "@echo off\r\ncall \"" + vcvars + "\" >nul 2>&1\r\n"
                    + windows_paths(cmd) + "\r\n";
    write_file(bat_out, bat);
    std::string p = windows_paths(native(bat_out));
    for (auto& ch : p) if (ch == '/') ch = '\\';
    return "cmd.exe /d /c \"" + p + "\"";
}

std::optional<fs::path> find_rt_dir(fs::path const& input)
{
    std::vector<fs::path> candidates;
    if (char const* env = std::getenv("CPP2_RT")) candidates.emplace_back(env);
    fs::path exe_dir = fs::absolute(fs::path(std::getenv("CPP2_EXE_DIR")
                                                 ? std::getenv("CPP2_EXE_DIR") : "."));
    candidates.push_back(exe_dir / ".." / ".." / "rt");
    candidates.push_back(fs::current_path() / "rt");
    for (fs::path d = fs::absolute(input).parent_path(); ;
         d = d.parent_path()) {
        candidates.push_back(d / "rt");
        if (!d.has_parent_path() || d == d.parent_path()) break;
    }
    for (auto const& c : candidates)
        if (fs::exists(c / "cpp2" / "support.hpp"))
            return fs::weakly_canonical(c);
    return std::nullopt;
}

// ── 模块图 + 全模块语义检查 ─────────────────────────────────────────
std::vector<emit::ModuleEntry> Prepared::entries() const
{
    std::vector<emit::ModuleEntry> out;
    for (auto const& name : graph.order) {
        auto const& u = graph.units.at(name);
        emit::ModuleEntry e;
        e.m = const_cast<ast::Module*>(&u.ast);         // 发射只读使用
        e.r = &sema.at(name);
        e.src_name = u.file.string();
        e.imports = u.imports;
        e.is_root = (name == graph.root_name);
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<Prepared> prepare(fs::path const& root, bool quick)
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
        std::unordered_map<std::string, std::string> owner;
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
            for (auto& e : m.enums)    if (e.exported) claim(name, e.name, e.line);
            for (auto& s : m.structs)  if (s.exported) claim(name, s.name, s.line);
            for (auto& v : m.variants) if (v.exported) claim(name, v.name, v.line);
            for (auto& c : m.concepts) if (c.exported) claim(name, c.name, c.line);
            for (auto& g : m.globals)  if (g.exported) claim(name, g.name, g.line);
            for (auto& f : m.funcs)
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

// ── native 多模块整程序摊平(M8)─────────────────────────────────
std::optional<NativeMerged> merge_for_native(mods::Graph& g, fs::path const& root_file)
{
    if (g.order.size() <= 1) return std::nullopt;       // 单模块:走原路径

    // 重名检测:跨模块同名声明(含未导出的内部名)合并后无法区分
    std::unordered_map<std::string, std::string> owner;
    bool clash = false;
    auto claim = [&](std::string const& mod, std::string const& n) {
        if (n == "main") return;
        auto it = owner.find(n);
        if (it != owner.end()) {
            if (it->second != mod) {
                print_diag(root_file.string(), "error", 0, 1,
                           "native flatten: duplicate declaration '" + n
                               + "' in modules '" + it->second + "' and '" + mod + "'");
                clash = true;
            }
        } else {
            owner.emplace(n, mod);
        }
    };
    for (auto const& name : g.order) {
        auto const& m = g.units.at(name).ast;
        for (auto& s : m.structs)  claim(name, s.name);
        for (auto& e : m.enums)    claim(name, e.name);
        for (auto& v : m.variants) claim(name, v.name);
        for (auto& gl : m.globals) claim(name, gl.name);
        for (auto& f : m.funcs)    claim(name, f.name);
    }
    if (clash) return std::nullopt;

    NativeMerged out;
    out.mod.name = g.root_name;
    auto move_into = [](ast::Module& dst, ast::Module&& src) {
        for (auto& x : src.structs)       dst.structs.push_back(std::move(x));
        for (auto& x : src.enums)         dst.enums.push_back(std::move(x));
        for (auto& x : src.variants)      dst.variants.push_back(std::move(x));
        for (auto& x : src.concepts)      dst.concepts.push_back(std::move(x));
        for (auto& x : src.globals)       dst.globals.push_back(std::move(x));
        for (auto& x : src.funcs)         dst.funcs.push_back(std::move(x));
        for (auto& x : src.legacy_blocks) dst.legacy_blocks.push_back(std::move(x));
    };
    for (auto const& name : g.order) {
        if (name == g.root_name) continue;
        move_into(out.mod, std::move(g.units.at(name).ast));
    }
    auto& root_ast = g.units.at(g.root_name).ast;
    for (auto& im : root_ast.imports)
        if (!im.module_parts.empty() && im.module_parts[0] == "std")
            out.mod.imports.push_back(std::move(im));   // 仅保留 std;其余已摊平
    move_into(out.mod, std::move(root_ast));

    std::cerr << "[cpp2] native backend: flattened " << g.order.size()
              << " modules into one unit\n";
    out.sema = sema::check(out.mod, {});
    std::string file = root_file.string();
    for (auto const& e : out.sema.errors)
        print_diag(file, "error", e.line, e.col > 0 ? e.col : 1, e.msg);
    if (!out.sema.ok()) return std::nullopt;
    return out;
}

// ── .cpp2cache:.c2i 格式 v1(冻结,M2e)────────────────────────────
// kVersion 随发射语义变更递增(M10:T[N] 原生数组 → cpp2::array 载体,
// 旧缓存不识别 [N] 类型文本,全量失效,abi-freeze §6)
namespace {
constexpr char kMagic[4] = {'C', '2', 'I', 'F'};
constexpr std::uint32_t kVersion = 3;

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
    for (int i = 3; i >= 0; --i) v = (v << 8) | std::uint8_t(s[pos + i]);
    pos += 4;
    return true;
}

bool get_u64(std::string const& s, size_t& pos, std::uint64_t& v)
{
    if (pos + 8 > s.size()) return false;
    v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | std::uint8_t(s[pos + i]);
    pos += 8;
    return true;
}
} // namespace

std::string cache_path(fs::path const& build_dir, std::string const& mod)
{
    return (build_dir / "cpp2cache" / (util::safe_name(mod) + ".c2i")).string();
}

std::optional<CacheRec> read_cache(fs::path const& build_dir, std::string const& mod)
{
    auto blob = read_file(cache_path(build_dir, mod));
    if (!blob || blob->size() < 8) return std::nullopt;
    if (std::memcmp(blob->data(), "C2IF", 4) != 0) return std::nullopt;
    size_t pos = 4;
    std::uint32_t version = 0;
    if (!get_u32(*blob, pos, version) || version != 1) return std::nullopt;

    std::uint32_t name_len = 0;
    if (!get_u32(*blob, pos, name_len) || pos + name_len > blob->size()) return std::nullopt;
    std::string name = blob->substr(pos, name_len);
    pos += name_len;
    if (name != mod) return std::nullopt;

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

void write_cache(fs::path const& build_dir, std::string const& mod,
                 CacheRec const& r, std::string const& iface_text)
{
    std::string o;
    auto put_u32l = [](std::string& o, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) o.push_back(char((v >> (i * 8)) & 0xff));
    };
    auto put_u64l = [](std::string& o, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) o.push_back(char((v >> (i * 8)) & 0xff));
    };
    auto digest = [&](std::string const& hex) {
        auto d = sha256::digest_from_hex(hex);
        o.append(reinterpret_cast<char const*>(d.data()), d.size());
    };
    o.append("C2IF", 4);
    put_u32l(o, 1);                                     // version
    put_u32l(o, static_cast<std::uint32_t>(mod.size()));
    o += mod;
    digest(r.src_hash);
    digest(r.iface_hash);
    digest(r.gen_hash);
    digest(r.deps_hash);
    put_u64l(o, iface_text.size());
    o += iface_text;
    write_file(cache_path(build_dir, mod), o);
}

} // namespace cpp2::app
