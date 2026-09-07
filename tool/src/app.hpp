// cpp2 CLI 共享支撑(模块化拆分:M7c)
// 帮助函数 / 缓存(.c2i v1)/ Prepared(模块图+sema)/ 命令入口声明。
#pragma once

#include "ast.hpp"
#include "audit.hpp"
#include "diagfilter.hpp"
#include "emit.hpp"
#include "fuzz.hpp"
#include "modules.hpp"
#include "native.hpp"
#include "sema.hpp"
#include "sha256.hpp"
#include "toolchain.hpp"
#include "util.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace ast        = cpp2::ast;
namespace diagfilter = cpp2::diagfilter;
namespace emit       = cpp2::emit;
namespace fuzz       = cpp2::fuzz;
namespace mods       = cpp2::mods;
namespace sema       = cpp2::sema;
namespace sha256     = cpp2::sha256;
namespace tc         = cpp2::toolchain;
namespace util       = cpp2::util;






namespace cpp2::app {



// ── 小工具 ──────────────────────────────────────────────────────────
inline std::optional<std::string> read_file(fs::path const& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_file(fs::path const& p, std::string const& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

inline void print_diag(std::string const& file, char const* sev,
                       int line, int col, std::string const& msg)
{
    std::cerr << file << ":" << line << ":" << col << ": " << sev << ": " << msg << "\n";
}

inline std::string quote(std::string const& s) { return "\"" + s + "\""; }

// 额外链接旗标(CPP2_LDFLAGS,如 "-lz"):FFI 示例的系统库注入点(M6)
inline std::string ldflags_env()
{
    char const* l = std::getenv("CPP2_LDFLAGS");
    return (l && *l) ? std::string(" ") + l : "";
}

// 平台原生路径(Windows 反斜杠化;POSIX 原样)
inline std::string native(fs::path p)
{
#ifdef _WIN32
    std::string s = p.string();
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
#else
    return p.string();
#endif
}

// ── 子进程 ──────────────────────────────────────────────────────────
struct CmdResult { bool ok; std::string output; };

int sys_rc(int rc);                                     // POSIX waitstatus 解码
CmdResult run_capture(std::string const& cmd);          // 合并 stderr 捕获

std::string find_compiler();
std::optional<fs::path> find_rt_dir(fs::path const& input);

// ── native 混动 legacy DLL 的编译器选择(M11;跨机器环境兼容)─────
// 与 find_compiler(通用,转译路径)不同:legacy DLL 会被 LoadLibrary 进
// 纯 native PE 进程,只接受「原生 Windows x64 目标」编译器:
//   - gcc/clang 家族以 -dumpmachine 自报三元组为准 —— 拒 Cygwin 宿主
//     (其 DLL 依赖 cygwin1.dll,纯 native 进程内 SEH 异常展开必崩,
//     普通整型调用侥幸能过,属静默陷阱)、拒非 x86-64 目标(exe 固定
//     Win64)、拒 MSVC 目标的 clang(gcc 式链接旗标不适用);
//   - MSVC 家族原生即合规。
// 发现顺序:CPP2_CXX(显式指定;目标不合规则记原因顺延,不硬失败)→
// PATH g++ → PATH clang++ → PATH cl → vswhere 定位 VS 安装(读
// vcvars64.bat,随 VS 版本/盘符自适应,不硬编码路径)。
// cxx 为空 ⇒ 环境内无可用原生编译器;rejected 汇总各候选落选原因供诊断。
struct NativeCxx {
    std::string cxx;        // 编译器命令名(调用时按 PATH 解析)
    std::string vcvars;     // 非空 = 每条命令前需先 call 该 bat(vcvars64)
    std::string rejected;   // 候选落选原因(多行,诊断用)
};
NativeCxx find_native_compiler();

// MSVC 无 dev-prompt 环境时的命令包装:vcvars 非空时把真命令写进临时
// .bat(call vcvars64 后执行),以 cmd.exe /d /c 单引号参数执行 ——
// 内层引号只存在于文件内容,免疫外层 sh/cmd 引号规则差异;vcvars 为空
// 时原样返回 cmd(已配好环境的 dev-prompt 直接走 PATH)。
std::string wrap_msvc_env(std::string const& vcvars, std::string const& cmd,
                          fs::path const& bat_out);

// ── 模块图 + 全模块语义检查(--quick 仅根模块)───────────────────
struct Prepared {
    mods::Graph graph;
    std::unordered_map<std::string, sema::Result> sema;
    std::vector<emit::ModuleEntry> entries() const;
};

std::optional<Prepared> prepare(fs::path const& root, bool quick = false);

// ── native 多模块整程序摊平 ─────────────────────────────────────
// 依赖模块声明按拓扑序合并进单个模块并重跑一遍 sema(std 为虚拟模块不参与)。
// 单模块程序返回空 optional(调用方走原路径);重名/检查失败返回空并已打印诊断。
// 注意:成功时各模块 AST 声明已被搬空,graph/sema 仅作容器勿再读。
struct NativeMerged {
    ast::Module mod;
    sema::Result sema;
};
std::optional<NativeMerged> merge_for_native(mods::Graph& g, fs::path const& root_file);

// ── .cpp2cache:.c2i v1(冻结)────────────────────────────────────
struct CacheRec {
    std::string src_hash;
    std::string iface_hash;
    std::string gen_hash;
    std::string deps_hash;
};

std::string cache_path(fs::path const& build_dir, std::string const& mod);
std::optional<CacheRec> read_cache(fs::path const& build_dir, std::string const& mod);
void write_cache(fs::path const& build_dir, std::string const& mod,
                 CacheRec const& r, std::string const& iface_text);

// ── 命令入口 ────────────────────────────────────────────────────────
int cmd_transpile(std::vector<std::string> const& args);
int cmd_check(std::vector<std::string> const& args);
int cmd_run(std::vector<std::string> const& args);
int cmd_build(std::vector<std::string> const& args);
int cmd_export_headers(std::vector<std::string> const& args);
int cmd_audit(std::vector<std::string> const& args);
int cmd_fuzz(std::vector<std::string> const& args);

} // namespace cpp2::app
