// C++2 模块图:import 解析、拓扑排序、环检测(M3)
#pragma once

#include "ast.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace cpp2::mods {

struct GraphError {
    std::string file;
    int line = 0;
    std::string msg;
};

struct ModuleUnit {
    fs::path file;
    std::string name;                        // 声明的模块名(缺省 = 文件名 stem)
    ast::Module ast;
    std::vector<std::string> imports;        // C++2 模块依赖(滤除 std)
    std::string source;                      // 原始文本(哈希/缓存用)
};

struct Graph {
    std::string root_name;
    std::vector<std::string> order;          // 拓扑序:依赖在前,root 最后
    std::unordered_map<std::string, ModuleUnit> units;

    ModuleUnit* unit(std::string const& n) {
        auto it = units.find(n);
        return it != units.end() ? &it->second : nullptr;
    }
    std::vector<std::string> const& deps_of(std::string const& n) {
        return units.at(n).imports;
    }
};

// 加载 root 及其传递依赖;失败抛 GraphError。
// 搜索路径:导入者所在目录 → root 所在目录 → CPP2_PATH(分隔符 ';' 或 ':')
Graph load(fs::path const& root);

// 导出接口的规范化文本(接口哈希来源,.c2i 种子)
std::string interface_text(ast::Module const& m);

} // namespace cpp2::mods
