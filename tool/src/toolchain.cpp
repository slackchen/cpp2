// C++2 工具链矩阵实现(M4)
#include "toolchain.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
    #define CPP2_POPEN _popen
    #define CPP2_PCLOSE _pclose
#else
    #define CPP2_POPEN popen
    #define CPP2_PCLOSE pclose
#endif

namespace cpp2::toolchain {

namespace {

std::string version_text(std::string const& cxx)
{
    std::string cmd = "\"" + cxx + "\" --version 2>&1";
    if (FILE* p = CPP2_POPEN(cmd.c_str(), "r")) {
        std::string out;
        char buf[256];
        while (out.size() < 1024 && std::fgets(buf, sizeof buf, p)) out += buf;
        CPP2_PCLOSE(p);
        return out;
    }
    return "";
}

bool contains(std::string const& hay, char const* needle)
{
    return hay.find(needle) != std::string::npos;
}

std::string to_lower(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

Family detect(std::string const& cxx)
{
    std::string ver = version_text(cxx);
    if (!ver.empty()) {
        if (contains(ver, "clang")) return Family::Clang;
        if (contains(ver, "Free Software Foundation")
         || contains(ver, "GCC") || contains(ver, "gcc")) return Family::Gcc;
        if (contains(ver, "Microsoft") || contains(ver, "MSVC")
         || contains(ver, "Optimizing Compiler")) return Family::Msvc;
    }
    std::string n = to_lower(cxx);
    if (contains(n, "clang"))                        return Family::Clang;
    if (contains(n, "g++") || contains(n, "gcc"))    return Family::Gcc;
    if (contains(n, "cl"))                           return Family::Msvc;
    return Family::Clang;                            // 未知:按最接近的默认
}

char const* family_name(Family f)
{
    switch (f) {
    case Family::Clang: return "clang";
    case Family::Gcc:   return "gcc";
    case Family::Msvc:  return "msvc";
    }
    return "?";
}

std::string bmi_extension(Family f)
{
    switch (f) {
    case Family::Clang: return ".pcm";
    case Family::Gcc:   return ".gcm";
    case Family::Msvc:  return ".ifc";
    }
    return ".pcm";
}

std::string compile_command(std::string const& cxx, Family f,
                            std::string const& rt_include,
                            std::vector<std::pair<std::string, std::string>> const& deps,
                            std::string const& gen, std::string const& obj,
                            bool is_interface, std::string const& bmi_out)
{
    auto q = [](std::string const& s) { return "\"" + s + "\""; };

    switch (f) {
    case Family::Clang: {
        // clang:named module BMI 显式映射(-fprebuilt-module-path 对其无效);
        // .cpp 文件需 -x c++-module 声明为接口单元(按扩展名判定 TU 类型)
        std::string cc = cxx + " -std=c++23 -O1 -I" + q(rt_include);
        for (auto const& [dep, bmi] : deps)
            cc += " -fmodule-file=" + dep + "=" + q(bmi);
        if (is_interface)
            cc += " -x c++-module -fmodule-output=" + q(bmi_out);
        return cc + " -c " + q(gen) + " -o " + q(obj);
    }
    case Family::Gcc: {
        // gcc:-fmodules-ts 开启;消费与接口同样映射 -fmodule-file=模块名=CMI
        std::string cc = cxx + " -std=c++23 -O1 -fmodules-ts -I" + q(rt_include);
        for (auto const& [dep, bmi] : deps)
            cc += " -fmodule-file=" + dep + "=" + q(bmi);
        if (is_interface)
            cc += " -fmodule-output=" + q(bmi_out);
        return cc + " -c " + q(gen) + " -o " + q(obj);
    }
    case Family::Msvc: {
        // msvc:/interface 产出 .ifc;消费者 /reference 名称=文件
        std::string cc = cxx + " /nologo /std:c++20 /EHsc /W3 /I" + rt_include;
        for (auto const& [dep, bmi] : deps)
            cc += " /reference " + dep + "=" + bmi;
        if (is_interface)
            cc += " /interface /ifcOutput:" + bmi_out;
        return cc + " /c " + q(gen) + " /Fo" + q(obj);
    }
    }
    return {};
}

std::string plain_compile_command(std::string const& cxx, Family f,
                                  std::string const& rt_include,
                                  std::string const& gen, std::string const& obj)
{
    auto q = [](std::string const& s) { return "\"" + s + "\""; };
    switch (f) {
    case Family::Msvc:
        return cxx + " /nologo /std:c++23 /EHsc /I" + q(rt_include)
             + " /c " + q(gen) + " /Fo" + q(obj);
    default:  // clang / gcc
        return cxx + " -std=c++23 -O1 -I" + q(rt_include)
             + " -c " + q(gen) + " -o " + q(obj);
    }
}

std::string link_command(std::string const& cxx, Family f,
                         std::vector<std::string> const& objs, std::string const& exe)
{
    std::string all;
    for (auto const& o : objs) all += " \"" + o + "\"";

    switch (f) {
    case Family::Clang:
    case Family::Gcc:
        return cxx + all + " -o \"" + exe + "\"";
    case Family::Msvc:
        return cxx + " /nologo" + all + " /Fe:\"" + exe + "\"";
    }
    return {};
}

} // namespace cpp2::toolchain
