// 小工具:128 位 FNV 组合哈希(缓存键;非密码学用途)
#pragma once

#include <cstdint>
#include <string>

namespace cpp2::util {

inline std::uint64_t fnv1a(std::uint64_t seed, std::string const& s)
{
    std::uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// 两路不同种子的 FNV → 32 位 hex 字符串,作缓存/接口哈希
inline std::string hash128(std::string const& s)
{
    std::uint64_t a = fnv1a(0xcbf29ce484222325ULL, s);
    std::uint64_t b = fnv1a(0x9e3779b97f4a7c15ULL, s);
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx",
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return buf;
}

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
