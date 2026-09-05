// cpp2/std/map.hpp — 自研 std 一期(M9):cpp2 风格 map<K,V>,内部包 std::map(有序)。
// 有序理由:迭代确定性 → 输出与后端对拍稳定;哈希版(hash_map)另行提供。
// 设计:不提供 m[k] —— 缺失键在 cpp2 里必须是值(get → V?)或 trap(at),
// 不允许 std 式静默插入;sema 的 Map kind 也不可下标(is_indexable 排除)。
#ifndef CPP2_STD_MAP_HPP
#define CPP2_STD_MAP_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <utility>

#include "cpp2/support.hpp"

namespace cpp2 {

template <class K, class V>
class map {
public:
    using size_type = std::size_t;

    map() = default;
    map(std::map<K, V> m) : rep_(std::move(m)) {}
    operator std::map<K, V> const&() const & { return rep_; }

    auto size() const -> size_type { return rep_.size(); }
    auto empty() const -> bool { return rep_.empty(); }
    auto clear() -> void { rep_.clear(); }
    auto len() const -> size_type { return rep_.size(); }

    auto insert(K const& k, V const& v) -> bool { return rep_.emplace(k, v).second; }
    auto get(K const& k) const -> std::optional<V>
    {
        auto it = rep_.find(k);
        if (it == rep_.end()) return std::nullopt;
        return it->second;
    }
    auto at(K const& k) const -> V const&
    {
        auto it = rep_.find(k);
        if (it == rep_.end()) trap("map key not found", "cpp2/std/map.hpp", 0);
        return it->second;
    }
    auto at(K const& k) -> V&
    {
        auto it = rep_.find(k);
        if (it == rep_.end()) trap("map key not found", "cpp2/std/map.hpp", 0);
        return it->second;
    }
    auto contains(K const& k) const -> bool { return rep_.find(k) != rep_.end(); }
    auto remove(K const& k) -> bool { return rep_.erase(k) != 0; }

    auto begin() { return rep_.begin(); }
    auto end() { return rep_.end(); }
    auto begin() const { return rep_.begin(); }
    auto end() const { return rep_.end(); }

private:
    std::map<K, V> rep_;
};

} // namespace cpp2

#endif // CPP2_STD_MAP_HPP
