// cpp2::rt — C++2 运行时支持库(M2a 骨架)
//
// 约定(IMPLEMENTATION.md §5):纯头文件、无外部依赖、禁用异常与 RTTI。
// 生成代码通过 #include "cpp2/support.hpp" 使用;工具链自身不依赖本文件。
#ifndef CPP2_SUPPORT_HPP
#define CPP2_SUPPORT_HPP

#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__cpp_lib_format)
    #include <format>
#endif

namespace cpp2 {

// ── trap:检查失败的统一终止行为(DESIGN §6)────────────────────────
[[noreturn]] inline void trap(char const* what, char const* file, int line)
{
    std::fprintf(stderr, "cpp2 trap: %s (%s:%d)\n", what, file, line);
    std::fflush(stderr);
    std::abort();
}

// ── in<T>:参数传递模式(DESIGN §4.3 / IMPL §4.2)────────────────────
// 可平凡拷贝且不超过两指针大小 → 按值;否则 const 引用。
template <class T>
concept in_by_value = std::is_trivially_copyable_v<T> && sizeof(T) <= 2 * sizeof(void*);

template <class T>
using in = std::conditional_t<in_by_value<T>, T, T const&>;

// ── 检查算术 / 收窄转换(DESIGN §6.3;M2b 起接入发射)────────────────
#if defined(__builtin_add_overflow)
    #define CPP2_HAS_BUILTIN_OVERFLOW 1
#endif

template <class T>
inline T checked_add(T a, T b, char const* file = "", int line = 0)
{
    T r{};
#if defined(CPP2_HAS_BUILTIN_OVERFLOW)
    if (__builtin_add_overflow(a, b, &r)) { trap("integer overflow", file, line); }
#else
    if (b > 0 && a > std::numeric_limits<T>::max() - b) { trap("integer overflow", file, line); }
    if (b < 0 && a < std::numeric_limits<T>::min() - b) { trap("integer overflow", file, line); }
    r = static_cast<T>(a + b);
#endif
    return r;
}

template <class T>
inline T checked_sub(T a, T b, char const* file = "", int line = 0)
{
    T r{};
#if defined(CPP2_HAS_BUILTIN_OVERFLOW)
    if (__builtin_sub_overflow(a, b, &r)) { trap("integer overflow", file, line); }
#else
    if (b < 0 && a > std::numeric_limits<T>::max() + b) { trap("integer overflow", file, line); }
    if (b > 0 && a < std::numeric_limits<T>::min() + b) { trap("integer overflow", file, line); }
    r = static_cast<T>(a - b);
#endif
    return r;
}

template <class T>
inline T checked_mul(T a, T b, char const* file = "", int line = 0)
{
    T r{};
#if defined(CPP2_HAS_BUILTIN_OVERFLOW)
    if (__builtin_mul_overflow(a, b, &r)) { trap("integer overflow", file, line); }
#else
    if (a > 0) {
        if (b > std::numeric_limits<T>::max() / a) { trap("integer overflow", file, line); }
    } else if (a < 0) {
        if (b < std::numeric_limits<T>::min() / a) { trap("integer overflow", file, line); }
    }
    r = static_cast<T>(a * b);
#endif
    return r;
}

template <class To, class From>
inline To narrow_cast(From v, char const* file = "", int line = 0)
{
    if (v < static_cast<From>(std::numeric_limits<To>::min())
     || v > static_cast<From>(std::numeric_limits<To>::max()))
    { trap("narrowing conversion out of range", file, line); }
    return static_cast<To>(v);
}

template <class T>
inline T checked_div(T a, T b, char const* file = "", int line = 0)
{
    if (b == 0) { trap("division by zero", file, line); }
    if constexpr (std::is_signed_v<T>) {
        if (a == std::numeric_limits<T>::min() && b == T(-1))
        { trap("integer overflow", file, line); }
    }
    return static_cast<T>(a / b);
}

template <class T>
inline T checked_mod(T a, T b, char const* file = "", int line = 0)
{
    if (b == 0) { trap("division by zero", file, line); }
    return static_cast<T>(a % b);
}

// ── 所有权类型别名与工厂(DESIGN §7.2)──────────────────────────────
template <class T> using unique = std::unique_ptr<T>;
template <class T> using shared = std::shared_ptr<T>;
template <class T> using weak   = std::weak_ptr<T>;

template <class T>
auto make_unique(T&& v)
{ return std::make_unique<std::decay_t<T>>(std::forward<T>(v)); }

template <class T>
auto make_shared(T&& v)
{ return std::make_shared<std::decay_t<T>>(std::forward<T>(v)); }

// ── 下标检查(DESIGN §6.2;M2b 起接入发射)──────────────────────────
template <class C, class I>
inline decltype(auto) index(C&& c, I i, char const* file = "", int line = 0)
{
    if (i < I{} || static_cast<decltype(c.size())>(i) >= c.size())
    { trap("index out of bounds", file, line); }
    return c[static_cast<decltype(c.size())>(i)];
}

// ── stdx:标准库名字桥(IMPLEMENTATION.md §4.7)─────────────────────
// std::print/println 经此转发。语义:
//   - 运行期字符串、数值 → 原样输出,不解释花括号
//   - 字面量 + 实参 → 走 vformat 格式化({0} 定位符可用)
// 注:字面量经转译后是 char const* 形参,vformat 为运行期格式化,
// 字面量中的孤立 '{' 会按 std::format 规则处理(与 std::print 一致)。
namespace stdx {

inline void print(std::string_view s)   { std::cout << s; }
inline void println(std::string_view s) { std::cout << s << '\n'; }

template <class T>
    requires std::integral<T> || std::floating_point<T>
void print(T v) { std::cout << v; }

template <class T>
    requires std::integral<T> || std::floating_point<T>
void println(T v) { std::cout << v << '\n'; }

#if defined(__cpp_lib_format)

template <class... A>
void print(char const* fmt, A const&... args)
{ std::cout << std::vformat(fmt, std::make_format_args(args...)); }

template <class... A>
void println(char const* fmt, A const&... args)
{ std::cout << std::vformat(fmt, std::make_format_args(args...)) << '\n'; }

#else

template <class... A>
void print(char const* fmt, A const&... args)
{
    static_assert(sizeof...(args) == 0,
                  "formatted print requires <format> support in the host compiler");
    std::cout << fmt;
}

template <class... A>
void println(char const* fmt, A const&... args)
{
    static_assert(sizeof...(args) == 0,
                  "formatted print requires <format> support in the host compiler");
    std::cout << fmt << '\n';
}

#endif

} // namespace stdx

} // namespace cpp2

#endif // CPP2_SUPPORT_HPP
