#!/usr/bin/env bash
# tests/run.sh — 全量回归
#   示例:期望退出 0 且输出含期望行
#   trap 用例:期望非零退出且 stderr 含 trap 消息
set -u
cd "$(dirname "$0")/.."
CPP2="./.cpp2build/cpp2.exe"
pass=0; fail=0

# ── 示例(期望输出)──────────────────────────────────────────────
run_case() { # $1=cppm路径 $2=期望片段 $3=是否期望 trap
    local out code
    out="$("$CPP2" run "$1" 2>&1)"; code=$?
    if [[ $3 == "trap" ]]; then
        if [[ $code -ne 0 && "$out" == *"$2"* ]]; then
            echo "PASS $1"; pass=$((pass+1))
        else
            echo "FAIL $1 (exit $code, want trap: $2)"; fail=$((fail+1))
        fi
    else
        if [[ $code -eq 0 && "$out" == *"$2"* ]]; then
            echo "PASS $1"; pass=$((pass+1))
        else
            echo "FAIL $1 (exit $code, want output: $2)"; fail=$((fail+1))
        fi
    fi
}

run_case examples/hello.cppm  "Hello, C++2!"          ok
run_case examples/funcs.cppm  "hypot2 = 25"           ok
run_case examples/funcs.cppm  "a bumped = 4"          ok
run_case examples/loops.cppm  "sum 1..=100 = 5050"    ok
run_case examples/point.cppm  "len = 5"               ok
run_case examples/point.cppm  "q.len = 0"             ok
run_case examples/colors.cppm "green as int = 1"      ok
run_case examples/safety.cppm "sum = 7"               ok
run_case examples/safety.cppm "total = 60"            ok
run_case examples/smart.cppm  "norm2 = 25"            ok
run_case examples/smart.cppm  "shared x = 1"          ok

# ── trap 用例(期望检查触发)─────────────────────────────────────
run_case tests/cases/overflow_trap.cppm "integer overflow"     trap
run_case tests/cases/bounds_trap.cppm   "index out of bounds"  trap
run_case tests/cases/div_trap.cppm      "division by zero"     trap

echo
echo "passed $pass, failed $fail"
[[ $fail -eq 0 ]]
