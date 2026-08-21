// C++2 工具链矩阵(M4):模块编译参数按编译器家族分派
// clang 路径经回归验证;gcc/msvc 为文档形态参数,未在无该编译器的环境实测
// (见 IMPLEMENTATION.md M4 完成记录的偏差表)。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cpp2::toolchain {

enum class Family { Clang, Gcc, Msvc };

// 探测编译器家族。优先解析 --version 输出:llvm-mingw 的 g++ 是别名,
// 仅看可执行名会误判;失败时退回名字启发式。
Family detect(std::string const& cxx);

char const* family_name(Family f);

// BMI 文件扩展名:clang .pcm / gcc .gcm / msvc .ifc
std::string bmi_extension(Family f);

// 模块 TU 编译命令。deps = 依赖(模块名 → BMI 文件路径);
// is_interface 时额外产出 BMI(bmi_out)。
std::string compile_command(std::string const& cxx, Family f,
                            std::string const& rt_include,
                            std::vector<std::pair<std::string, std::string>> const& deps,
                            std::string const& gen, std::string const& obj,
                            bool is_interface, std::string const& bmi_out);

// 普通 TU 编译命令(headers 后端):无任何模块/BMI 旗标
std::string plain_compile_command(std::string const& cxx, Family f,
                                  std::string const& rt_include,
                                  std::string const& gen, std::string const& obj);

// 链接命令(objs → exe)
std::string link_command(std::string const& cxx, Family f,
                         std::vector<std::string> const& objs, std::string const& exe);

} // namespace cpp2::toolchain
