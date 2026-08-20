#!/usr/bin/env bash
# tests/run.sh — 全量回归
#   示例:期望退出 0 且输出含期望行
#   trap 用例:期望非零退出且 stderr 含 trap 消息(可选断言 #line 位置映射)
set -u
cd "$(dirname "$0")/.."
CPP2="./.cpp2build/cpp2.exe"
pass=0; fail=0

# ── 示例(期望输出)──────────────────────────────────────────────
run_case() { # $1=cppm路径 $2=期望片段 $3=是否期望 trap [$4=期望位置片段]
    local out code
    out="$("$CPP2" run "$1" 2>&1)"; code=$?
    if [[ $3 == "trap" ]]; then
        if [[ $code -ne 0 && "$out" == *"$2"* && ( -z "${4:-}" || "$out" == *"$4"* ) ]]; then
            echo "PASS $1"; pass=$((pass+1))
        else
            echo "FAIL $1 (exit $code, want trap: $2 ${4:-})"; fail=$((fail+1))
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

# ── trap 用例(期望检查触发;断言消息与 .cppm 源位置)─────────────
run_case tests/cases/overflow_trap.cppm "integer overflow"                    trap "overflow_trap.cppm:6"
run_case tests/cases/bounds_trap.cppm   "index out of bounds"                 trap "bounds_trap.cppm:9"
run_case tests/cases/div_trap.cppm      "division by zero"                    trap "div_trap.cppm:7"
run_case tests/cases/null_trap.cppm     "null dereference"                    trap "null_trap.cppm:13"
run_case tests/cases/float_trap.cppm    "float-to-integer conversion out of range" trap "float_trap.cppm:7"

# @unsafe/@unchecked 块形式 + 逃逸后检查恢复
run_case tests/cases/optout.cppm        "sum = 11"                            ok

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

# ── M4:检查器完备 / audit / fuzz ────────────────────────────────
# audit:检查注入点计数 + @unsafe/@unchecked 位置(白纸黑字)
audit_out="$("$CPP2" audit examples/smart.cppm 2>/dev/null; "$CPP2" audit examples/safety.cppm 2>/dev/null)"
if [[ "$audit_out" == *"deref 4"* && "$audit_out" == *"opt-out: @unchecked at line 20"* ]]; then
    echo "PASS m4/audit"; pass=$((pass+1))
else
    echo "FAIL m4/audit"; fail=$((fail+1))
fi
audit_out="$("$CPP2" audit tests/cases/optout.cppm 2>/dev/null)"
if [[ "$audit_out" == *"opt-out: @unsafe at line 13"* && "$audit_out" == *"@unchecked x2, @unsafe x1"* \
      && "$audit_out" == *"index 1"* ]]; then    # 块外 v[0] 恢复检查
    echo "PASS m4/audit-optout"; pass=$((pass+1))
else
    echo "FAIL m4/audit-optout"; fail=$((fail+1))
fi

# fuzz:固定 seed 可复现,期望零崩溃
if "$CPP2" fuzz examples/*.cppm examples/multimod/*.cppm tests/cases/*.cppm \
        --seed 20260820 --iters 10000 2>/dev/null | grep -q "0 crashes"; then
    echo "PASS m4/fuzz"; pass=$((pass+1))
else
    echo "FAIL m4/fuzz"; fail=$((fail+1))
fi

# 深度防护:3000 层嵌套表达式 → 干净诊断而非进程崩溃
mkdir -p .cpp2build
perl -e 'print "main: () -> int = { x: int := ", "("x3000, "1", ")"x3000, "; return 0; }"' \
    > .cpp2build/deep.cppm 2>/dev/null
if "$CPP2" transpile .cpp2build/deep.cppm 2>&1 | grep -q "nesting too deep"; then
    echo "PASS m4/depth-guard"; pass=$((pass+1))
else
    echo "FAIL m4/depth-guard"; fail=$((fail+1))
fi

echo
echo "passed $pass, failed $fail"
[[ $fail -eq 0 ]]
