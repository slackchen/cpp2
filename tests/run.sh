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

# ── M3:多模块 ─────────────────────────────────────────────────
# 摊平模式 run(跨模块符号解析)
run_case examples/multimod/app.cppm "norm2 = 25"        ok
run_case examples/multimod/app.cppm "doubled_add(2,3) = 10" ok

# 模块模式 build + 运行产物
if "$CPP2" build examples/multimod/app.cppm >/dev/null 2>&1 \
   && ./examples/multimod/.cpp2build/app 2>/dev/null | grep -q "norm2 = 25"; then
    echo "PASS m3/module-build"; pass=$((pass+1))
else
    echo "FAIL m3/module-build"; fail=$((fail+1))
fi

# no-op 增量:重建必须零转译零编译
if "$CPP2" build examples/multimod/app.cppm 2>&1 | grep -q "0 transpiled.*0 compiled"; then
    echo "PASS m3/incremental-noop"; pass=$((pass+1))
else
    echo "FAIL m3/incremental-noop"; fail=$((fail+1))
fi

# export-headers:Cpp1 消费者互操作
if "$CPP2" export-headers tests/cases/exportlib.cppm -o tests/cases/bridge >/dev/null 2>&1 \
   && g++ -std=c++23 -I"$(pwd)/rt" -I tests/cases/bridge \
        tests/cases/bridge/app_lib.cpp tests/cases/consumer.cpp \
        -o tests/cases/bridge/consumer.exe 2>/dev/null \
   && ./tests/cases/bridge/consumer.exe 2>/dev/null | grep -q "42"; then
    echo "PASS m3/cpp1-interop"; pass=$((pass+1))
else
    echo "FAIL m3/cpp1-interop"; fail=$((fail+1))
fi

echo
echo "passed $pass, failed $fail"
[[ $fail -eq 0 ]]
