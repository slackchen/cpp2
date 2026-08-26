// 其余命令:transpile / check / run / export-headers / audit / fuzz
#include "app.hpp"
#include "diagfilter.hpp"
#include "emit.hpp"
#include "fuzz.hpp"
#include "modules.hpp"
#include "native.hpp"
#include "sha256.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cpp2::app {
namespace fs = std::filesystem;

namespace ast   = cpp2::ast;
namespace emit  = cpp2::emit;
namespace mods  = cpp2::mods;
namespace sha256 = cpp2::sha256;
namespace tc    = cpp2::toolchain;
namespace audit = cpp2::audit;
namespace fuzz  = cpp2::fuzz;

using cpp2::app::find_compiler;
using cpp2::app::find_rt_dir;
using cpp2::app::ldflags_env;
using cpp2::app::native;
using cpp2::app::prepare;
using cpp2::app::print_diag;
using cpp2::app::quote;
using cpp2::app::read_file;
using cpp2::app::run_capture;
using cpp2::app::write_file;

// ── cpp2 transpile:摊平转译查看生成码 ──────────────────────────────
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

// ── cpp2 run:摊平转译 + 编译 + 执行(--backend=native 走汇编直译)──
int cmd_run(std::vector<std::string> const& args)
{
    bool release = false;
    bool want_native = false;
    std::string file;
    for (auto const& a : args) {
        if (a == "--backend=native") want_native = true;
        else if (a == "--release") release = true;
        else if (!a.empty() && a[0] != '-') file = a;
    }
    if (file.empty()) {
        std::cerr << "usage: cpp2 run <root.cpp2> [--backend=native] [--release]\n";
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

    // 原生路径:Win64 直接 PE（hello 去 g++），其余 Win64 asm+struct 经 g++ -c
    if (want_native && !in.parent_path().empty()) {
        try {
            auto& g0 = p->graph;
#ifdef _WIN32
            // Windows:先试 direct PE（hello），失败则 asm+struct via g++
            try {
                auto pe_bytes = cpp2::native::emit_pe(g0.units.at(g0.root_name).ast,
                                                      p->sema.at(g0.root_name));
                fs::path build_dir0 = in.parent_path() / ".cpp2build" / "native";
                fs::create_directories(build_dir0);
                fs::path exe = build_dir0 / (in.stem().string() + ".exe");
                std::ofstream out(native(exe), std::ios::binary);
                out.write(reinterpret_cast<char const*>(pe_bytes.data()), pe_bytes.size());
                out.close();
                std::cerr << "[cpp2] native backend: emitting x86-64 Win64 (direct PE, no g++)\n";
                return cpp2::app::sys_rc(std::system(quote(native(exe)).c_str()));
            } catch (...) {
                // 回退到 Win64 asm 路径
            }
#endif
            auto asm_text = cpp2::native::emit_asm(
                g0.units.at(g0.root_name).ast, p->sema.at(g0.root_name));
            fs::path build_dir0 = in.parent_path() / ".cpp2build" / "native";
            fs::create_directories(build_dir0);
            fs::path s_file = build_dir0 / (in.stem().string() + ".s");
            fs::path prog_o = build_dir0 / "prog.o";
            fs::path rt_o   = build_dir0 / "native_rt.o";
            fs::path rt_src = fs::path("tools") / "native_rt.c";
            if (!fs::exists(rt_src)) rt_src = fs::path("..") / "tools" / "native_rt.c";
            fs::path exe = build_dir0 / in.stem();
#ifdef _WIN32
            exe.replace_extension(".exe");
#endif
            write_file(s_file, asm_text);
            tc::Family fam = tc::detect(cxx);
            std::string shim = cxx + " -x c -c " + quote(native(rt_src))
                             + " -o " + quote(native(rt_o));
            std::string asmc = cxx + " -c " + quote(native(s_file))
                             + " -o " + quote(native(prog_o));
            std::string link = tc::link_command(cxx, fam,
                                  {native(prog_o), native(rt_o)},
                                  native(exe)) + ldflags_env();
#ifdef _WIN32
            link += " -mconsole";
            std::cerr << "[cpp2] native backend: emitting x86-64 Win64 (asm+struct, via g++)\n";
#else
            std::cerr << "[cpp2] native backend: emitting x86-64 SysV\n";
#endif
            std::cerr << "[cpp2] " << shim << "\n";
            if (!run_capture(shim).ok) throw std::runtime_error("shim build failed");
            std::cerr << "[cpp2] " << asmc << "\n";
            if (!run_capture(asmc).ok) throw std::runtime_error("assemble failed");
            std::cerr << "[cpp2] " << link << "\n";
            if (!run_capture(link).ok) throw std::runtime_error("link failed");
            return cpp2::app::sys_rc(std::system(quote(native(exe)).c_str()));
        } catch (std::exception const& e) {
            std::cerr << "[cpp2] native backend error: " << e.what() << "\n";
            std::cerr << "native compilation failed (no fallback)\n";
            return 1;
        }
    }

    auto code = emit::emit_flatten(p->entries(), release);
    fs::path build_dir = in.parent_path() / ".cpp2build";
    fs::path cpp = build_dir / (in.stem().string() + ".cpp");
    fs::path exe = build_dir / in.stem();
    write_file(cpp, code);

    std::string exe_s = native(exe);
    std::string cc = cxx + " -std=c++23 -O0 -g -I" + quote(native(*rt))
                   + " " + quote(native(cpp)) + " -o " + quote(exe_s)
                   + ldflags_env();
    std::cerr << "[cpp2] " << cc << "\n";
    auto r = run_capture(cc);
    if (!r.ok) {
        std::cerr << diagfilter::banner;
        std::string flt = diagfilter::filter(r.output, native(build_dir));
        std::cerr << (flt.empty() ? r.output : flt);
        std::cerr << "error: compilation failed\n";
        return 2;
    }
    return cpp2::app::sys_rc(std::system(quote(exe_s).c_str()));
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

} // namespace cpp2::app
