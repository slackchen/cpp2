// cpp2/std/string.hpp — 自研 std 一期(M9):cpp2 风格 string,内部包 std::string。
// 契约(docs/abi-freeze.md v3 §1):转译模式 cpp2 `string` 的表示 = 本类型;
// rep_ 私有,内部实现可整体替换(native 侧为三槽自有表示,不经本头文件)。
// 规则:无异常;受检访问走 at()(越界 cpp2::trap),operator[] 保持 raw 语义
// ——emit 的受检下标路径是 cpp2::index,@unchecked 语义与 std 一致。
#ifndef CPP2_STD_STRING_HPP
#define CPP2_STD_STRING_HPP

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "cpp2/support.hpp"                    // cpp2::trap;stdx 声明域

namespace cpp2 {

class string {
public:
    using size_type = std::size_t;
    static constexpr size_type npos = std::string::npos;

    // ── 构造:字面量/互操作均为隐式(生成码大量依赖 char const* → string)──
    string() = default;
    string(char const* s) : rep_(s ? s : "") {}
    string(size_type n, char c) : rep_(n, c) {}
    string(std::string_view sv) : rep_(sv) {}
    string(std::string s) : rep_(std::move(s)) {}
    // 拷贝/移动/赋值:默认(rep_ 值语义)

    // ── 互操作转换:err() 通道 / string_view 形参 / filesystem ──
    operator std::string const&() const & { return rep_; }
    operator std::string_view() const { return rep_; }
    // path 直达转换:libc++(Win32 API 后端)path 的 value_type 是 wchar_t,
    // 其 __is_pathable 只认 basic_string/basic_string_view/字符数组本尊,
    // "先转 std::string 再入 path" 是两次用户转换 → 不可行;ifstream/ofstream/
    // directory_iterator 均取 const path&,必须一步到位。
    operator std::filesystem::path() const { return std::filesystem::path(rep_); }

    // ── std 兼容层(既有语料零迁移)────────────────────────────
    auto size() const -> size_type { return rep_.size(); }
    auto length() const -> size_type { return rep_.length(); }
    auto empty() const -> bool { return rep_.empty(); }
    auto data() const -> char const* { return rep_.data(); }
    auto data() -> char* { return rep_.data(); }
    auto c_str() const -> char const* { return rep_.c_str(); }
    auto push_back(char c) -> void { rep_.push_back(c); }
    auto resize(size_type n) -> void { rep_.resize(n); }
    auto resize(size_type n, char fill) -> void { rep_.resize(n, fill); }
    auto begin() -> std::string::iterator { return rep_.begin(); }
    auto end() -> std::string::iterator { return rep_.end(); }
    auto begin() const -> std::string::const_iterator { return rep_.begin(); }
    auto end() const -> std::string::const_iterator { return rep_.end(); }
    auto operator[](size_type i) -> char& { return rep_[i]; }
    auto operator[](size_type i) const -> char const& { return rep_[i]; }

    // ── cpp2 风格层 ──────────────────────────────────────────
    auto len() const -> size_type { return rep_.size(); }
    auto at(size_type i) const -> char
    {
        if (i >= rep_.size()) trap("string index out of bounds", "cpp2/std/string.hpp", 0);
        return rep_[i];
    }
    // find 以 T? 取代 npos:缺失是值不是哨兵
    auto find(std::string_view sub) const -> std::optional<size_type>
    {
        auto p = rep_.find(sub);
        if (p == npos) return std::nullopt;
        return p;
    }
    auto substr(size_type pos, size_type n = npos) const -> string
    { return string(rep_.substr(pos, n)); }
    auto starts_with(std::string_view p) const -> bool { return rep_.starts_with(p); }
    auto ends_with(std::string_view p) const -> bool { return rep_.ends_with(p); }
    auto to_string() const -> std::string const& { return rep_; }

    // ── 运算符(std 兼容语义;字面量一侧经隐式构造)────────────────
    auto operator+=(string const& r) -> string& { rep_ += r.rep_; return *this; }
    auto operator+=(std::string const& r) -> string& { rep_ += r; return *this; }
    auto operator+=(char const* r) -> string& { rep_ += r; return *this; }
    auto operator+=(char r) -> string& { rep_ += r; return *this; }

    friend auto operator+(string l, string const& r) -> string { l.rep_ += r.rep_; return l; }
    friend auto operator+(string l, char const* r) -> string { l.rep_ += r; return l; }
    friend auto operator+(char const* l, string const& r) -> string { return string(l) += r; }
    friend auto operator+(string l, char r) -> string { l.rep_ += r; return l; }
    friend auto operator+(char l, string const& r) -> string { return string(1, l) += r; }

    friend auto operator==(string const& a, string const& b) -> bool { return a.rep_ == b.rep_; }
    friend auto operator==(string const& a, char const* b) -> bool { return a.rep_ == b; }
    friend auto operator==(char const* a, string const& b) -> bool { return a == b.rep_; }
    // <=> 供有序容器(map 键)与 < <= > >= 的 C++20 改写使用
    friend auto operator<=>(string const& a, string const& b) { return a.rep_ <=> b.rep_; }

private:
    std::string rep_;
};

} // namespace cpp2

// ── 格式化接入:std::format 路径(有 <format> 的工具链)─────────────
#if defined(__cpp_lib_format)
namespace std {
template <> struct formatter<cpp2::string, char> : formatter<string_view, char> {
    template <class Ctx>
    auto format(cpp2::string const& s, Ctx& ctx) const
    { return formatter<string_view, char>::format(string_view(s), ctx); }
};
} // namespace std
#endif

namespace cpp2 {
// {N} 回退路径(GCC13 libstdc++ 无 <format>):经参数类型 ADL 找到本重载,
// 否则回退模板落到 std::to_string(wrapper 无匹配 → 编译错)。
inline auto fmt_to_string(string const& s) -> std::string { return s; }
} // namespace cpp2

#endif // CPP2_STD_STRING_HPP
