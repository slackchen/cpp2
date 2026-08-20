// C++2 → C++23 发射器(M3:摊平模式 / C++20 模块模式 / 桥接模式)
#pragma once

#include "ast.hpp"
#include "sema.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cpp2::emit {

struct ModuleEntry {
    ast::Module* m = nullptr;
    sema::Result const* r = nullptr;
    std::string src_name;                          // #line 映射用
    std::vector<std::string> imports;              // 本模块的 C++2 模块依赖名
    bool is_root = false;
};

// 整程序模式:全部模块摊平进一个翻译单元(每模块独立命名空间 + using 提升)
std::string emit_flatten(std::vector<ModuleEntry> const& units);

// C++20 模块模式:root 发射为普通 TU(import 依赖),其余为 named module
std::string emit_module_unit(ModuleEntry const& e);

// 桥接模式(export-headers):生成 .h / .cpp 对,供 Cpp1 消费者使用
std::pair<std::string, std::string> emit_bridge(std::vector<ModuleEntry> const& units,
                                                std::string const& libname);

} // namespace cpp2::emit
