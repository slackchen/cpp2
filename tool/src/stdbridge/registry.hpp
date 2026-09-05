// stdbridge/registry.hpp — cpp2 标准库面的单表映射(IMPL §4.7)。
// 本表是语言面 ↔ 生成 C++ 类型拼串的唯一登记点:
//   - string/vector/list/map 落自研 std(rt/cpp2/std,内部包 std 容器;
//     abi-freeze v3 §1)。
//   - 其余条目为既有桥接(定宽整型/智能指针/arena 等)。
//   - 未注册的 std:: 限定名逐字透传 = C++ 互操作 escape hatch(DESIGN §9.1)。
// native 后端不读本表(按 sema Kind 分派);新增裸类型名需同步 sema 的 Kind 归类
// (sema.cpp type_from_use_core)与 emit.cpp 的 ListLit/sema_type_cpp 接线。
#pragma once

#include <string>
#include <unordered_map>

namespace cpp2::stdbridge {

// 裸类型名 → 生成 C++ 类型拼串。未命中返回 nullptr(调用方按原样透传)。
inline std::string const* map_type(std::string const& n)
{
    static std::unordered_map<std::string, std::string> const m{
        {"int", "int"}, {"double", "double"}, {"float", "float"},
        {"bool", "bool"}, {"void", "void"}, {"char", "char"},
        {"i8", "std::int8_t"}, {"i16", "std::int16_t"},
        {"i32", "std::int32_t"}, {"i64", "std::int64_t"},
        {"u8", "std::uint8_t"}, {"u16", "std::uint16_t"},
        {"u32", "std::uint32_t"}, {"u64", "std::uint64_t"},
        {"string", "cpp2::string"}, {"string_view", "std::string_view"},
        {"vector", "cpp2::vector"}, {"list", "cpp2::vector"},
        {"map", "cpp2::map"},
        {"byte", "std::byte"},
        {"unique", "std::unique_ptr"}, {"shared", "std::shared_ptr"},
        {"weak", "std::weak_ptr"},
        {"arena", "cpp2::arena"}, {"arena_ptr", "cpp2::arena_ptr"},
    };
    auto it = m.find(n);
    return it != m.end() ? &it->second : nullptr;
}

} // namespace cpp2::stdbridge
