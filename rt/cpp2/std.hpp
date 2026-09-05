// cpp2/std.hpp — 自研 std 伞头(M9):prelude 统一引入。
// 语言面:bare string/vector/list/map → 本目录类型(stdbridge 单表映射,
// tool/src/stdbridge/registry.hpp);std:: 限定名逐字透传 = 互操作 escape hatch
// (DESIGN §9.1)。
#ifndef CPP2_STD_HPP
#define CPP2_STD_HPP

#include "cpp2/std/string.hpp"
#include "cpp2/std/vector.hpp"
#include "cpp2/std/map.hpp"

#endif // CPP2_STD_HPP
