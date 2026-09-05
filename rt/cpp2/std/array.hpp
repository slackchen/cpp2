// cpp2/std/array.hpp — 原生固定长度数组(M10):T[N] 后缀 → cpp2::array<T, N>。
// 规则同 vector.hpp:无异常;受检走 at()(越界 trap),operator[] raw
// (受检路径 = emit 侧 cpp2::index)。长度是编译期常量;不衰减到指针,
// 赋值 = 整体拷贝;字面量数量不符 trap(sema 对定型声明先行静态拦截)。
#ifndef CPP2_STD_ARRAY_HPP
#define CPP2_STD_ARRAY_HPP

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <utility>

#include "cpp2/support.hpp"

namespace cpp2 {

template <class T, std::size_t N>
class array {
public:
    using size_type = std::size_t;

    array() : rep_{} {}
    array(std::initializer_list<T> il) : rep_{}
    {
        if (il.size() != N) trap("array literal size mismatch", "cpp2/std/array.hpp", 0);
        size_type i = 0;
        for (auto const& v : il) rep_[i++] = v;
    }
    array(std::array<T, N> a) : rep_(std::move(a)) {}   // 互操作:桥接头 / Cpp1 消费者

    operator std::array<T, N>&() & { return rep_; }
    operator std::array<T, N> const&() const & { return rep_; }

    // ── std 兼容层 ───────────────────────────────────────────
    auto size() const -> size_type { return N; }
    auto empty() const -> bool { return N == 0; }
    auto fill(T const& v) -> void { rep_.fill(v); }
    auto begin() { return rep_.begin(); }
    auto end() { return rep_.end(); }
    auto begin() const { return rep_.begin(); }
    auto end() const { return rep_.end(); }
    auto operator[](size_type i) -> T& { return rep_[i]; }
    auto operator[](size_type i) const -> T const& { return rep_[i]; }

    // ── cpp2 风格层 ──────────────────────────────────────────
    auto len() const -> size_type { return N; }
    // at/first/last 同 vector:越界/空以 trap/T? 取代 UB
    auto at(size_type i) const -> T const&
    {
        if (i >= N) trap("array index out of bounds", "cpp2/std/array.hpp", 0);
        return rep_[i];
    }
    auto at(size_type i) -> T&
    {
        if (i >= N) trap("array index out of bounds", "cpp2/std/array.hpp", 0);
        return rep_[i];
    }
    auto first() const -> std::optional<T>
    { return N == 0 ? std::nullopt : std::optional<T>(rep_.front()); }
    auto last() const -> std::optional<T>
    { return N == 0 ? std::nullopt : std::optional<T>(rep_.back()); }

    friend auto operator==(array const& a, array const& b) -> bool { return a.rep_ == b.rep_; }
    friend auto operator!=(array const& a, array const& b) -> bool { return a.rep_ != b.rep_; }

private:
    std::array<T, N> rep_;
};

// CTAD:std::array 互操作桥(cpp2 面用 T[N] 后缀,长度显式写出)
template <class T, std::size_t N> array(std::array<T, N>) -> array<T, N>;

} // namespace cpp2

#endif // CPP2_STD_ARRAY_HPP
