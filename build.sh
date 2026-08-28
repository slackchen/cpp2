#!/usr/bin/env bash
# 构建 cpp2 工具链(M2a)
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
EXE=".cpp2build/cpp2"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE="$EXE.exe" ;;
esac

mkdir -p .cpp2build
# shellcheck disable=SC2086
"$CXX" -std=c++23 -O1 -Wall -Wextra tool/src/*.cpp tool/src/native/*.cpp -o "$EXE"
echo "built $EXE"
