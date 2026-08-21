// 小工具
#pragma once

#include <string>

namespace cpp2::util {

// 模块名 → 文件系统安全名(app.util → app_util)
inline std::string safe_name(std::string const& mod)
{
    std::string out;
    for (char c : mod) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

} // namespace cpp2::util
