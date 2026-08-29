// 原生后端公共实现(平台宏与发射器见 native/emit_base.hpp + emit_sysv.cpp / emit_win64.cpp)
#include "native.hpp"
#include "native/emit_base.hpp"

namespace cpp2::native {

bool is_int_kind(sema::Type::Kind k)
{
    switch (k) {
    case sema::Type::Int: case sema::Type::I8: case sema::Type::I16:
    case sema::Type::I32: case sema::Type::I64:
    case sema::Type::U8: case sema::Type::U16: case sema::Type::U32:
    case sema::Type::U64: case sema::Type::Bool:
        return true;
    default:
        return false;
    }
}

} // namespace cpp2::native
