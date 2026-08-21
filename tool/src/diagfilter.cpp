// 后端诊断过滤实现
#include "diagfilter.hpp"

#include <regex>
#include <sstream>

namespace cpp2::diagfilter {

namespace {

struct Frame {
    std::string file;
    int line = 0;
    int col = 0;
    std::string sev;
    std::string msg;
};

// clang/gcc: file:line:col: severity: msg
// msvc(文档形态): file(line): severity: msg
bool parse_frame(std::string const& line, Frame& f)
{
    static std::regex const clang_gcc(
        R"(^(.+?):(\d+):(\d+):\s*(fatal error|error|warning|note):\s*(.*)$)");
    static std::regex const msvc(
        R"(^(.+?)\((\d+)\)(?::(\d+))?:?\s*(error|warning)\s*\w*:\s*(.*)$)");
    std::smatch m;
    if (std::regex_match(line, m, clang_gcc)) {
        f.file = m[1]; f.line = std::stoi(m[2]); f.col = std::stoi(m[3]);
        f.sev = m[4]; f.msg = m[5];
        return true;
    }
    if (std::regex_match(line, m, msvc)) {
        f.file = m[1]; f.line = std::stoi(m[2]); f.col = m[3].matched ? std::stoi(m[3]) : 1;
        f.sev = m[4]; f.msg = m[5];
        return true;
    }
    return false;
}

bool is_noise(std::string const& line)
{
    // include 栈 / 模板实例化链引导句
    if (line.rfind("In file included from", 0) == 0) return true;
    if (line.find("required from here") != std::string::npos) return true;
    if (line.find("in instantiation of") != std::string::npos) return true;
    if (line.find("candidates are") != std::string::npos) return true;
    if (line.find("candidate is") != std::string::npos) return true;
    return false;
}

bool is_excerpt(std::string const& line)
{
    // clang/gcc 源码摘录与插入符: "  18 | ..." / "      |      ^~~"
    static std::regex const ex(R"(^\s*\d+\s*\||^\s*\|)");
    return std::regex_search(line, ex);
}

bool is_linker(std::string const& line)
{
    return line.find("undefined reference") != std::string::npos
        || line.find("multiple definition") != std::string::npos
        || line.find("cannot find") != std::string::npos
        || line.find("ld returned") != std::string::npos
        || line.find("unresolved external") != std::string::npos;
}

} // namespace

std::string filter(std::string const& raw, std::string const& build_dir)
{
    std::istringstream in(raw);
    std::string line;
    std::ostringstream out;

    bool last_generated = false;

    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;

        Frame f;
        if (parse_frame(line, f)) {
            last_generated = !build_dir.empty()
                             && f.file.rfind(build_dir, 0) == 0;
            out << (last_generated ? "[generated] " : "") << f.file << ":"
                << f.line << ":" << f.col << ": " << f.sev << ": "
                << f.msg << "\n";
            continue;
        }

        if (is_noise(line)) continue;
        if (is_excerpt(line)) {
            if (!last_generated) out << line << "\n";   // 只留映射回 .cpp2 的摘录
            continue;
        }
        if (is_linker(line)) {
            out << line << "\n";
            continue;
        }
        // 其余(编译器内部倾泻、响应文件回显等)一律丢弃
    }
    return out.str();
}

} // namespace cpp2::diagfilter
