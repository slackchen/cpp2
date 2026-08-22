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

#include "cpp2/arena.hpp"                   // M6:arena / arena_ptr
#include "cpp2/gc.hpp"                      // M6:可选保守式 GC

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

// 整型 → 整型:边界用 To 的 min/max 精确比较
template <class To, class From>
    requires std::integral<From>
inline To narrow_cast(From v, char const* file = "", int line = 0)
{
    if (v < static_cast<From>(std::numeric_limits<To>::min())
     || v > static_cast<From>(std::numeric_limits<To>::max()))
    { trap("narrowing conversion out of range", file, line); }
    return static_cast<To>(v);
}

// 浮点 → 整型(DESIGN §6.3"浮点到整型的越界转换"):边界取 2^N,
// 该值可被浮点精确表示,截断后落在 [min, max] 内;NaN 不满足任何比较 → trap。
template <class To, class From>
    requires std::floating_point<From>
inline To narrow_cast(From v, char const* file = "", int line = 0)
{
    long double lo;
    long double hi;
    if constexpr (std::is_signed_v<To>) {
        lo = static_cast<long double>(std::numeric_limits<To>::min());  // -2^(N-1)
        hi = -lo;                                                       //  2^(N-1)
    } else {
        lo = 0.0L;
        hi = static_cast<long double>(std::numeric_limits<To>::max()) + 1.0L;  // 2^N
    }
    if (!(v >= lo && v < hi))
    { trap("float-to-integer conversion out of range", file, line); }
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

// ── 错误通道:错误是值,bug 是 trap(DESIGN §8)─────────────────────
// throws 函数降为返回 cpp2::expected<R, cpp2::error>(IMPL §4.3)。
// v0.1 错误体 = 消息 + 源位置;类别/错误码体系留待 v0.3(DESIGN §8.4)。
struct error {
    std::string text;
    error() = default;
    explicit error(std::string s) : text(std::move(s)) {}
    std::string const& message() const { return text; }
};

// v1 决策(M6 收口):无条件使用自有 expected 实现。
// 曾按 __cpp_lib_expected 双轨,但 runner 环境组合(llvm-mingw / libstdc++12 /
// libc++ 版本差)证明宏探测不可靠且接口面完全可控;偏差表记录。
template <class E>
struct unexpected {
    E v_;
    explicit unexpected(E e) : v_(std::move(e)) {}
};

template <class T>
class [[nodiscard]] expected {
    std::variant<T, error> v_;
public:
    expected(T v) : v_(std::in_place_index<0>, std::move(v)) {}
    expected(unexpected<error> u) : v_(std::in_place_index<1>, std::move(u.v_)) {}
    bool has_value() const { return v_.index() == 0; }
    explicit operator bool() const { return has_value(); }
    T& operator*() & { return std::get<0>(v_); }
    T&& operator*() && { return std::get<0>(std::move(v_)); }
    T const& operator*() const & { return std::get<0>(v_); }
    T& value() & { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    ::cpp2::error& error() & { return std::get<1>(v_); }
    ::cpp2::error const& error() const & { return std::get<1>(v_); }
    // or 默认值(f() or d,DESIGN §8.3)
    T value_or(T def) const { return has_value() ? **this : std::move(def); }
};

// err():throws 函数体内构造失败值 → return err("not found");
// 位置并入消息,错误链可追溯(栈的确定性替代)。
inline unexpected<error> err(std::string msg, char const* file = "", int line = 0)
{
    return unexpected<error>(error(std::move(msg) + " (" + file + ":"
                                   + std::to_string(line) + ")"));
}

// must():f()! — 调用方确信不会失败,失败即 bug → trap(DESIGN §8.2)
template <class E>
inline decltype(auto) must(E&& e, char const* file = "", int line = 0)
{
    if (!e.has_value()) { trap("error asserted impossible", file, line); }
    return *std::forward<E>(e);
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

// ── 空检查解引用(DESIGN §6.4;M4 起接入发射)───────────────────────
// p.member 降为 deref(p)->member;空指针 → trap(可 @unchecked 退出)。
// 左值按引用透传(零开销);右值(v[i] 等)按移动返回,仅在调用点立即使用。
template <class P>
inline P& deref(P& p, char const* file = "", int line = 0)
{
    if (!p) { trap("null dereference", file, line); }
    return p;
}

template <class P>
inline P deref(P&& p, char const* file = "", int line = 0)
{
    if (!p) { trap("null dereference", file, line); }
    return std::move(p);
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

// 轻量回退(GCC13 libstdc++ 无 <format>):支持 {0} {1}… 占位替换,
// 参数类型限标量/bool/char*/string/string_view(std::to_string 可及者)
inline void fmt_replace(std::string& fmt, std::string const* args, size_t n)
{
    for (size_t idx = 0; idx < n; ++idx) {
        std::string tok = "{" + std::to_string(idx) + "}";
        size_t pos = fmt.find(tok);
        if (pos != std::string::npos) fmt.replace(pos, tok.size(), args[idx]);
    }
}

template <class T>
std::string fmt_to_string(T const& v)
{
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, std::string>)
        return v;
    else if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, char const*>)
        return std::string(v);
    else if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, bool>)
        return v ? "true" : "false";
    else
        return std::to_string(v);
}

template <class... A>
void print(char const* fmt, A const&... args)
{
    std::string out = fmt;
    std::string reps[] = { fmt_to_string(args)... };
    fmt_replace(out, reps, sizeof...(args));
    std::cout << out;
}

template <class... A>
void println(char const* fmt, A const&... args)
{
    std::string out = fmt;
    std::string reps[] = { fmt_to_string(args)... };
    fmt_replace(out, reps, sizeof...(args));
    std::cout << out << '\n';
}

#endif

} // namespace stdx

} // namespace cpp2

#endif // CPP2_SUPPORT_HPP
