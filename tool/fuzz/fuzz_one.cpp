// libFuzzer harness(M4):前端卫生检查,与内置 `cpp2 fuzz` 共用 run_one。
// 构建:tool/fuzz/build.sh(需要 clang -fsanitize=fuzzer)。
#include "fuzz.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::string src(reinterpret_cast<char const*>(data), size);
    cpp2::fuzz::run_one(src, "<libfuzzer>");
    return 0;
}
