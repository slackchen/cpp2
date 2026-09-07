// hybrid.hpp — 混动转义连接(M11):native 后端 mini-C 解析不了的 cxx_legacy
// 卸载为"转译出 C++ TU → -shared 编成 <module>_legacy.dll → native PE 导入表
// 直连"。本头是 native 发射器(产出计划)与转译发射器(消费计划生成 DLL TU)
// 的共享描述;调用侧符号统一改名 cpp2leg_<name>,与 legacy 原名(可能 C++ 修饰)
// 解耦。边界 v1 = 按值整型标量(int/i8..u64/bool/char),其余签名干净拒绝并
// 指回转译模式。
#pragma once

#include "ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cpp2::hybrid {

// 一个被卸载的 legacy 函数 = 同名 cpp2 无体声明(签名/类型来源)
struct Export {
    std::string name;                    // 函数名(legacy C++ 侧同名定义)
    std::vector<std::string> param_c;    // 形参 C 类型(转发时按声明窄化)
    std::string ret_c;                   // 返回 C 类型;空 = 无返回
};

struct Plan {
    bool needed = false;
    std::string module_name;             // 所属模块(DLL TU 命名/审计)
    std::string dll_name;                // "<module>_legacy.dll"
    std::string src_path;                // #line 映射用(正斜杠归一)
    std::vector<std::pair<int, std::string>> blocks;   // legacy 块(起始行, 原文)
    std::vector<Export> exports;
};

// 调用侧导入符号(亦为 DLL 导出转发器符号)
inline std::string import_symbol(std::string const& name) { return "cpp2leg_" + name; }

// Win64 槽标量 ↔ C 转发型;不表内返回空串(v1 边界外)
inline std::string scalar_c_type(ast::TypeUse const& t)
{
    if (t.parts.size() != 1 || t.is_array || t.is_pointer || t.is_optional) return {};
    static std::pair<char const*, char const*> const tbl[] = {
        {"int", "int"},         {"i8", "signed char"},  {"i16", "short"},
        {"i32", "int"},         {"i64", "long long"},   {"u8", "unsigned char"},
        {"u16", "unsigned short"}, {"u32", "unsigned int"},
        {"u64", "unsigned long long"}, {"bool", "bool"}, {"char", "char"},
    };
    for (auto& [n, c] : tbl)
        if (t.parts[0] == n) return c;
    return {};
}

} // namespace cpp2::hybrid
