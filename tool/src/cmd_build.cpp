// cpp2 build:并行增量构建(M3b headers 默认 / M3 modules opt-in / M7 native 原型)
#include "app.hpp"
#include "diagfilter.hpp"
#include "emit.hpp"
#include "native.hpp"
#include "sha256.hpp"
#include "toolchain.hpp"
#include "native/asm64.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpp2::app {
namespace fs = std::filesystem;

namespace ast   = cpp2::ast;
namespace sema  = cpp2::sema;
namespace emit  = cpp2::emit;
namespace mods  = cpp2::mods;
namespace util  = cpp2::util;
namespace sha256 = cpp2::sha256;
namespace diagfilter = cpp2::diagfilter;
namespace tc    = cpp2::toolchain;

using cpp2::app::CacheRec;
using cpp2::app::read_cache;
using cpp2::app::read_file;
using cpp2::app::write_file;
using cpp2::app::ldflags_env;
using cpp2::app::run_capture;
using cpp2::app::quote;
using cpp2::app::native;

// ── cpp2 build:并行增量构建 ───────────────────────────────────────
// backend=headers(默认):每模块 .h 接口 + 实现片段按 TU 大小预算装箱,
//   普通 TU 并行编译,不依赖 C++20 modules;
// backend=cxx20-modules:每模块 named module + BMI(M3 原路径);
// backend=native(P1 原型):单模块整数子集 → x86-64 汇编直译,
//   子集外自动回退 headers。
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
    if (backend != "headers" && backend != "cxx20-modules" && backend != "native") {
        std::cerr << "error: unknown backend '" << backend
                  << "' (headers | cxx20-modules | native)\n";
        return 1;
    }
    if (in.empty()) {
        for (char const* cand : {"main.cpp2", "app.cpp2"}) {
            if (fs::exists(cand)) { in = cand; break; }
        }
    }
    if (in.empty()) {
        std::cerr << "usage: cpp2 build <root.cpp2> [--backend=headers|cxx20-modules]"
                     " [--max-jobs=N]\n";
        return 1;
    }

    std::string cxx = cpp2::app::find_compiler();
    if (cxx.empty()) {
        std::cerr << "error: no C++ compiler found (set CPP2_CXX)\n";
        return 2;
    }
    auto rt = cpp2::app::find_rt_dir(in);
    if (!rt) {
        std::cerr << "error: cannot locate rt/ directory (set CPP2_RT)\n";
        return 2;
    }
    auto p = cpp2::app::prepare(in);
    if (!p) return 1;

    // native 后端已模块化手写（x64 + ELF/PE），与转译双轨可切换；Windows 亦直发 x64 Win64 ABI

    // 转译(内容寻址落盘)+ 哈希缓存;各后端构建目录隔离,互不污染
    auto& g = p->graph;
    bool headers_backend = backend == "headers";
    bool native_backend  = backend == "native";
    fs::path build_dir = in.parent_path() / ".cpp2build" /
                         (headers_backend ? "hdr" : (native_backend ? "native" : "mods"));
    fs::path bmi_dir = build_dir / "bmi";
    fs::path obj_dir = build_dir / "obj";
    fs::create_directories(build_dir);
    fs::create_directories(obj_dir);
    if (!headers_backend && !native_backend) fs::create_directories(bmi_dir);

    // ── native 后端(P1 原型):单模块整数子集 → x86-64 汇编 ──
    if (native_backend) {
        if (g.order.size() != 1) {
            std::cerr << "[cpp2] native backend error: multi-module program not supported in native v0 (only single module)\n";
            std::cerr << "native compilation failed (no fallback)\n";
            return 1;
        } else {
            try {
#ifdef _WIN32
                // Windows: direct PE without g++ (B) for hello, else try asm+struct
                try {
                    auto pe_bytes = cpp2::native::emit_pe(g.units.at(g.root_name).ast,
                                                          p->sema.at(g.root_name));
                    fs::create_directories(build_dir);
                    fs::path exe = in.parent_path() / ".cpp2build" / (in.stem().string() + ".exe");
                    std::ofstream out(native(exe), std::ios::binary);
                    out.write(reinterpret_cast<char const*>(pe_bytes.data()), pe_bytes.size());
                    out.close();
                    std::cerr << "[cpp2] native backend: emitting x86-64 Win64 (direct PE, no g++)\n";
                    std::cout << exe.string() << "\n";
                    return 0;
                } catch (std::exception const& pe_e) {
                    // hello 以外走 asm+struct 路径（仍经 g++ -c，但已支持 struct）
                    std::string asm_text = cpp2::native::emit_asm(g.units.at(g.root_name).ast,
                                                                  p->sema.at(g.root_name));
                    fs::create_directories(build_dir);
                    fs::path s_file = build_dir / (in.stem().string() + ".s");
                    fs::path prog_o = build_dir / "prog.o";
                    fs::path rt_o   = build_dir / "native_rt.o";
                    fs::path rt_src = fs::path("tools") / "native_rt.c";
                    if (!fs::exists(rt_src)) rt_src = fs::path("..") / "tools" / "native_rt.c";
                    fs::path exe = in.parent_path() / ".cpp2build" / (in.stem().string() + ".exe");
                    tc::Family fam0 = tc::detect(cxx);
                    std::string shim = cxx + " -x c -c " + quote(native(rt_src)) + " -o " + quote(native(rt_o));
                    std::string asmc = cxx + " -c " + quote(native(s_file)) + " -o " + quote(native(prog_o));
                    write_file(s_file, asm_text);
                    std::cerr << "[cpp2] native backend: emitting x86-64 Win64 (asm+struct, via g++)\n";
                    std::cerr << "[cpp2] " << shim << "\n";
                    if (!run_capture(shim).ok) throw std::runtime_error(std::string("runtime shim build failed: ") + pe_e.what());
                    std::cerr << "[cpp2] " << asmc << "\n";
                    if (!run_capture(asmc).ok) throw std::runtime_error("assemble failed");
                    std::string link = tc::link_command(cxx, fam0, {native(prog_o), native(rt_o)}, native(exe)) + ldflags_env();
                    link += " -mconsole";
                    std::cerr << "[cpp2] " << link << "\n";
                    if (!run_capture(link).ok) throw std::runtime_error("link failed");
                    std::cout << exe.string() << "\n";
                    return 0;
                }
#else
                std::string asm_text =
                    cpp2::native::emit_asm(g.units.at(g.root_name).ast,
                                           p->sema.at(g.root_name));
                fs::create_directories(build_dir);
                fs::path s_file = build_dir / (in.stem().string() + ".s");
                fs::path prog_o = build_dir / "prog.o";
                fs::path rt_o   = build_dir / "native_rt.o";
                fs::path rt_src = fs::path("tools") / "native_rt.c";
                if (!fs::exists(rt_src)) rt_src = fs::path("..") / "tools" / "native_rt.c";
                fs::path exe = in.parent_path() / ".cpp2build" / in.stem();
                tc::Family fam0 = tc::detect(cxx);
                std::string shim = cxx + " -x c -c " + quote(native(rt_src))
                                 + " -o " + quote(native(rt_o));
                std::string asmc = cxx + " -c " + quote(native(s_file))
                                 + " -o " + quote(native(prog_o));
                write_file(s_file, asm_text);
                std::cerr << "[cpp2] native backend: emitting x86-64 SysV\n";
                std::cerr << "[cpp2] " << shim << "\n";
                if (!run_capture(shim).ok)
                    throw std::runtime_error("runtime shim build failed");
                std::cerr << "[cpp2] " << asmc << "\n";
                if (!run_capture(asmc).ok)
                    throw std::runtime_error("assemble failed");
                std::string link = tc::link_command(
                    cxx, fam0,
                    {native(prog_o), native(rt_o)},
                    native(exe)) + ldflags_env();
                std::cerr << "[cpp2] " << link << "\n";
                if (!run_capture(link).ok)
                    throw std::runtime_error("link failed");
                std::cout << exe.string() << "\n";
                return 0;
#endif
            } catch (std::exception const& e) {
                std::cerr << "[cpp2] native backend error: " << e.what() << "\n";
                std::cerr << "native compilation failed (no fallback; use --backend=headers for transpilation)\n";
                return 1;
            }
        }
    }

    // ── 转译(内容寻址落盘)+ 哈希缓存 ──────────────────────────────
    std::unordered_map<std::string, CacheRec> recs;
    std::unordered_map<std::string, bool> h_changed;      // 接口头(.h)是否重写
    std::unordered_map<std::string, bool> f_changed;      // 实现片段是否变化
    int transpiled = 0, cached = 0;
    std::unordered_map<std::string, std::string> iface;   // name → 接口哈希

    std::unordered_map<std::string, std::string> frags;   // headers:实现片段

    for (auto const& name : g.order) {
        auto const& u = g.units.at(name);
        // 源哈希混入工具版本:代码生成器变更后旧缓存自然失效,
        // 避免 emit 演进(如 M4 的空检查注入)被缓存掩盖
        std::string src_hash = sha256::hex_of(u.source + "|codegen:" + "cpp2 0.1.0-m7b (virtual dispatch, error categories, pipes, shifts, captures, if-bodies)");

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
            // include 平铺在 part 顶部:嵌套 include 经 #pragma once 短路——
            // 链式深图(m999→…→m0)否则产生千层 include 嵌套(超编译器上限)
            std::unordered_set<std::string> closure;
            bool any_h_changed = false;
            for (auto const& m : part.mods) collect(m, closure);
            std::vector<std::string> incs(closure.begin(), closure.end());
            std::sort(incs.begin(), incs.end());
            std::string text = "// Generated by cpp2c (headers backend). DO NOT EDIT.\n";
            for (auto const& m : incs)
                text += "#include \"" + util::safe_name(m) + ".h\"\n";
            for (auto const& m : closure) any_h_changed |= h_changed[m];
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

        // part 间无编译依赖(接口全在 .h)→ 并行编译(并发上限 = max_jobs)
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
                        std::string rargs = cc.substr(cxx.size() + 1);
                        for (auto& ch : rargs) if (ch == '\\') ch = '/';
                        write_file(rsp, rargs);
                        cc = cxx + " @" + quote(native(rsp));
                    }

                    std::cerr << "[cpp2] " << cc << "\n";
                    auto r = run_capture(cc);
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
    std::string link = tc::link_command(cxx, fam, obj_files, native(exe)) + ldflags_env();
    std::cerr << "[cpp2] " << link << "\n";
    auto lr = run_capture(link);
    if (!lr.ok) {
        std::cerr << diagfilter::banner;
        std::string flt = diagfilter::filter(lr.output, native(build_dir));
        std::cerr << (flt.empty() ? lr.output : flt);
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

} // namespace cpp2::app
