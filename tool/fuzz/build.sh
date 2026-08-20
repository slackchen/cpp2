#!/usr/bin/env bash
# 构建 libFuzzer 版前端卫生检查目标(可选;内置 `cpp2 fuzz` 不依赖它)。
# main.cpp 被排除:libFuzzer 自带 main。
set -euo pipefail
cd "$(dirname "$0")/../.."

CXX="${CXX:-clang++}"
OUT=".cpp2build/fuzz_lexparse"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OUT="$OUT.exe" ;;
esac

mkdir -p .cpp2build
"$CXX" -std=c++23 -g -Wall -Wextra -fsanitize=fuzzer \
    tool/src/lexer.cpp tool/src/parser.cpp tool/src/sema.cpp tool/src/fuzz.cpp \
    tool/fuzz/fuzz_one.cpp -o "$OUT"
echo "built $OUT"
