// cpp2 命令行工具(M2a:transpile / run)
#include "ast.hpp"
#include "emit.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char const* kVersion = "cpp2 0.1.0-m2b (transpiler, types + safety checks)";

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

// 转译单个文件;成功返回生成代码。
std::optional<std::string> transpile(fs::path const& input)
{
    auto src = read_file(input);
    if (!src) {
        std::cerr << "error: cannot read '" << input.string() << "'\n";
        return std::nullopt;
    }
    std::string in = input.string();

    std::vector<cpp2::lex::Token> toks;
    try {
        toks = cpp2::lex::lex(*src);
    } catch (cpp2::lex::LexError const& e) {
        print_diag(in, "error", e.line, e.col, e.msg);
        return std::nullopt;
    }

    cpp2::ast::Module mod;
    try {
        mod = cpp2::parse::parse(std::move(toks), in);
    } catch (cpp2::parse::ParseError const& e) {
        print_diag(in, "error", e.line, e.col, e.msg);
        return std::nullopt;
    }

    auto sema = cpp2::sema::check(mod);
    for (auto const& w : sema.warnings)
        print_diag(in, "warning", w.line, w.col > 0 ? w.col : 1, w.msg);
    if (!sema.ok()) {
        for (auto const& e : sema.errors)
            print_diag(in, "error", e.line, e.col > 0 ? e.col : 1, e.msg);
        return std::nullopt;
    }

    return cpp2::emit::emit_translation_unit(mod, sema, in);
}

// 查找 rt 目录(含 cpp2/support.hpp)
std::optional<fs::path> find_rt_dir(fs::path const& input)
{
    std::vector<fs::path> candidates;
    if (char const* env = std::getenv("CPP2_RT")) candidates.emplace_back(env);
    // 可执行文件位于 <repo>/.cpp2build/ → rt 在 <repo>/rt
    fs::path exe_dir = fs::absolute(fs::path(std::getenv("CPP2_EXE_DIR") ? std::getenv("CPP2_EXE_DIR") : "."));
    candidates.push_back(exe_dir / ".." / ".." / "rt");
    candidates.push_back(fs::current_path() / "rt");
    // 从输入文件向上层找
    for (fs::path d = fs::absolute(input).parent_path(); ; d = d.parent_path()) {
        candidates.push_back(d / "rt");
        if (!d.has_parent_path() || d == d.parent_path()) break;
    }
    for (auto const& c : candidates) {
        if (fs::exists(c / "cpp2" / "support.hpp"))
            return fs::weakly_canonical(c);
    }
    return std::nullopt;
}

std::string quote(std::string const& s) { return "\"" + s + "\""; }

// 选编译器:CPP2_CXX 环境变量,否则 g++ → clang++
std::string find_compiler()
{
    if (char const* env = std::getenv("CPP2_CXX")) return env;
    for (char const* cxx : {"g++", "clang++"}) {
        std::string cmd = std::string(cxx) + " --version > NUL 2>&1";
        if (std::system(cmd.c_str()) == 0) return cxx;
    }
    return "";
}

int cmd_transpile(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 transpile <in.cppm> [-o out.cpp]\n";
        return 1;
    }
    fs::path in = args[0];
    fs::path out;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "-o") out = args[i + 1];
    }
    if (out.empty()) out = in.parent_path() / ".cpp2build" / (in.stem().string() + ".cpp");

    auto code = transpile(in);
    if (!code) return 1;
    if (!write_file(out, *code)) {
        std::cerr << "error: cannot write '" << out.string() << "'\n";
        return 1;
    }
    std::cout << out.string() << "\n";
    return 0;
}

int cmd_run(std::vector<std::string> const& args)
{
    if (args.empty()) {
        std::cerr << "usage: cpp2 run <in.cppm>\n";
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

    auto code = transpile(in);
    if (!code) return 1;

    fs::path build_dir = in.parent_path() / ".cpp2build";
    fs::path cpp = build_dir / (in.stem().string() + ".cpp");
    fs::path exe = build_dir / in.stem();
    write_file(cpp, *code);

    // system() 走 cmd.exe:统一反斜杠,避免混合分隔符无法执行
    auto native = [](fs::path p) {
        std::string s = p.string();
        for (auto& c : s) if (c == '/') c = '\\';
        return s;
    };
    std::string exe_s = native(exe);

    std::string cc = cxx + " -std=c++23 -O0 -g -I" + quote(rt->string())
                   + " " + quote(native(cpp)) + " -o " + quote(exe_s);
    std::cerr << "[cpp2] " << cc << "\n";
    if (std::system(cc.c_str()) != 0) {
        std::cerr << "error: compilation failed\n";
        return 2;
    }
    return std::system(quote(exe_s).c_str());
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  cpp2 transpile <in.cppm> [-o out.cpp]   # .cppm -> C++23\n"
        << "  cpp2 run <in.cppm>                      # transpile + compile + execute\n"
        << "  cpp2 version\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "transpile") return cmd_transpile(args);
    if (cmd == "run")       return cmd_run(args);
    if (cmd == "version")   { std::cout << kVersion << "\n"; return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }

    std::cerr << "error: unknown command '" << cmd << "'\n";
    usage();
    return 1;
}
