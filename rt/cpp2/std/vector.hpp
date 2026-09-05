// cpp2/std/vector.hpp — 自研 std 一期(M9):cpp2 风格 vector<T>,内部包 std::vector。
// 规则同 string.hpp:无异常;受检走 at()(越界 trap),operator[] raw
// (受检路径 = emit 侧 cpp2::index,@unchecked 语义与 std 一致)。
#ifndef CPP2_STD_VECTOR_HPP
#define CPP2_STD_VECTOR_HPP

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "cpp2/support.hpp"

namespace cpp2 {

template <class T>
class vector {
public:
    using size_type = std::size_t;

    vector() = default;
    vector(std::initializer_list<T> il) : rep_(il) {}
    vector(std::vector<T> v) : rep_(std::move(v)) {}   // 互操作:桥接头 / Cpp1 消费者

    operator std::vector<T>&() & { return rep_; }
    operator std::vector<T> const&() const & { return rep_; }

    // ── std 兼容层 ───────────────────────────────────────────
    auto size() const -> size_type { return rep_.size(); }
    auto empty() const -> bool { return rep_.empty(); }
    auto clear() -> void { rep_.clear(); }
    auto push_back(T const& v) -> void { rep_.push_back(v); }
    auto push_back(T&& v) -> void { rep_.push_back(std::move(v)); }
    auto resize(size_type n) -> void { rep_.resize(n); }
    auto resize(size_type n, T const& fill) -> void { rep_.resize(n, fill); }
    auto begin() { return rep_.begin(); }
    auto end() { return rep_.end(); }
    auto begin() const { return rep_.begin(); }
    auto end() const { return rep_.end(); }
    auto operator[](size_type i) -> T& { return rep_[i]; }
    auto operator[](size_type i) const -> T const& { return rep_[i]; }

    // ── cpp2 风格层 ──────────────────────────────────────────
    auto len() const -> size_type { return rep_.size(); }
    auto push(T const& v) -> void { rep_.push_back(v); }
    auto push(T&& v) -> void { rep_.push_back(std::move(v)); }
    // pop/first/last 以 T? 取代 UB:空容器是值不是未定义行为
    auto pop() -> std::optional<T>
    {
        if (rep_.empty()) return std::nullopt;
        T v = std::move(rep_.back());
        rep_.pop_back();
        return v;
    }
    auto at(size_type i) const -> T const&
    {
        if (i >= rep_.size()) trap("vector index out of bounds", "cpp2/std/vector.hpp", 0);
        return rep_[i];
    }
    auto at(size_type i) -> T&
    {
        if (i >= rep_.size()) trap("vector index out of bounds", "cpp2/std/vector.hpp", 0);
        return rep_[i];
    }
    auto first() const -> std::optional<T>
    { return rep_.empty() ? std::nullopt : std::optional<T>(rep_.front()); }
    auto last() const -> std::optional<T>
    { return rep_.empty() ? std::nullopt : std::optional<T>(rep_.back()); }

    friend auto operator==(vector const& a, vector const& b) -> bool { return a.rep_ == b.rep_; }
    friend auto operator!=(vector const& a, vector const& b) -> bool { return a.rep_ != b.rep_; }

private:
    std::vector<T> rep_;
};

// CTAD:ListLit 发射 cpp2::vector{a,b,c} 依赖此推导指引
template <class T> vector(std::initializer_list<T>) -> vector<T>;
template <class T> vector(std::vector<T>) -> vector<T>;

} // namespace cpp2

#endif // CPP2_STD_VECTOR_HPP
