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

# ── M2c:错误通道 + 契约 ──────────────────────────────────────────
# 错误通道五件套:? 传播 / ! 断言 / or 默认 / match ok-err / if-let(DESIGN §8)
run_case examples/errors.cppm "a = 21"                                ok
run_case examples/errors.cppm "b = 7"                                 ok
run_case examples/errors.cppm "double = 40"                           ok
run_case examples/errors.cppm "failed: not a digit in '2x' (examples/errors.cppm:12)" ok
run_case examples/errors.cppm "v = 99"                                ok
# 契约:pre/post/old()/result(DESIGN §6.5)
run_case examples/contract.cppm "w = 30, balance = 70"                ok
run_case examples/contract.cppm "int_sqrt(26) = 5"                    ok
run_case examples/contract.cppm "value = 5"                           ok
# throws + post 组合(错误通道上的出口检查)
run_case tests/cases/errpost.cppm "half = 5"                          ok
run_case tests/cases/errpost.cppm "failed: odd input (tests/cases/errpost.cppm:9)" ok
# 故障注入:契约违反 / ! 断言失败 = bug → trap(带 .cppm 源位置)
run_case tests/cases/pre_trap.cppm  "precondition failed"             trap "pre_trap.cppm:6"
run_case tests/cases/post_trap.cppm "postcondition failed"            trap "post_trap.cppm:6"
run_case tests/cases/must_trap.cppm "error asserted impossible"       trap "must_trap.cppm:10"

# audit:契约计数(contract.cppm = withdraw/int_sqrt/bump 各 pre+post = 6)
if "$CPP2" audit examples/contract.cppm 2>/dev/null | grep -q "contract 6"; then
    echo "PASS m2c/audit-contract"; pass=$((pass+1))
else
    echo "FAIL m2c/audit-contract"; fail=$((fail+1))
fi

# 模块模式:expected 签名跨 BMI 正常(模块单元 + 链接)
if "$CPP2" build examples/errors.cppm >/dev/null 2>&1 \
   && ./examples/.cpp2build/errors 2>/dev/null | grep -q "double = 40"; then
    echo "PASS m2c/module-errors"; pass=$((pass+1))
else
    echo "FAIL m2c/module-errors"; fail=$((fail+1))
fi

# 负例:未处理错误通道调用 → 干净诊断(编译器强制处理,DESIGN §8.1)
cat > .cpp2build/unhandled.cppm <<'EOF'
module unhandled;
import std;
f: () -> int throws = { return err("boom"); }
main: () -> int = { f(); return 0; }
EOF
if "$CPP2" transpile .cpp2build/unhandled.cppm 2>&1 | grep -q "unhandled error-channel"; then
    echo "PASS m2c/unhandled-call"; pass=$((pass+1))
else
    echo "FAIL m2c/unhandled-call"; fail=$((fail+1))
fi

# 负例:非 throws 函数内用 '?' → 干净诊断
cat > .cpp2build/nonthrows.cppm <<'EOF'
module nonthrows;
import std;
f: () -> int throws = { return 1; }
g: () -> int = { n: int := f()?; return n; }
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/nonthrows.cppm 2>&1 | grep -q "enclosing function to be 'throws'"; then
    echo "PASS m2c/prop-requires-throws"; pass=$((pass+1))
else
    echo "FAIL m2c/prop-requires-throws"; fail=$((fail+1))
fi

# ── M2d:泛型/concept、variant、optional、模式匹配、UFCS ─────────
# 正向(DESIGN §4.5–§5.6 示例全绿)
run_case examples/shapes.cppm   "circle area = 12.56636"   ok
run_case examples/shapes.cppm   "rect area = 12"           ok
run_case examples/shapes.cppm   "neg area = 0"             ok     # 守卫失败落入同模式后续臂
run_case examples/shapes.cppm   "z is rectangle"           ok
run_case examples/shapes.cppm   "go"                       ok
run_case examples/optional.cppm "found ada"                ok     # if-let 绑定解包值
run_case examples/optional.cppm "9 missing"                ok     # if-let else 分支
run_case examples/optional.cppm "hello, grace"             ok     # match some/none 表达式
run_case examples/generics.cppm "n to_string = 42"         ok     # UFCS 桥接 std::to_string
run_case examples/generics.cppm "clamp -3 = 0"             ok     # 泛型 + concept 约束
run_case examples/generics.cppm "clamp 2.5 = 2"            ok     # 同一泛型多类型实例化
run_case examples/generics.cppm "mid3 = 3"                 ok     # requires 子句
run_case examples/generics.cppm "squares sum = 30"         ok     # lambda 实参
run_case examples/types.cppm    "rex says woof (2 tricks)" ok     # 继承:基类字段经派生访问
run_case examples/types.cppm    "cleanup buddy"            ok     # 析构器:块出口确定性调用
run_case examples/types.cppm    "cleanup rex"              ok     # 析构顺序与构造相反

# 负例:variant 非穷尽 → 干净诊断(match 是唯一合法访问,穷尽性编译器保证)
cat > .cpp2build/nonexhaustive.cppm <<'EOF'
module nonexhaustive;
import std;
Circle: type = { r: double = 0; }
Rect: type = { w: double = 0; h: double = 0; }
Shape: variant = { Circle, Rect }
area: (s: Shape) -> double = match s {
    Circle(r) => r * r;
}
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/nonexhaustive.cppm 2>&1 | grep -q "match arms must be exhaustive"; then
    echo "PASS m2d/variant-exhaustive"; pass=$((pass+1))
else
    echo "FAIL m2d/variant-exhaustive"; fail=$((fail+1))
fi

# 负例:'_' 通配不在末尾
cat > .cpp2build/wildpos.cppm <<'EOF'
module wildpos;
import std;
Signal: enum = { red, green }
f: (s: Signal) -> int = match s {
    _      => 0;
    .red   => 1;
}
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/wildpos.cppm 2>&1 | grep -q "'_' must be the last match arm"; then
    echo "PASS m2d/wildcard-last"; pass=$((pass+1))
else
    echo "FAIL m2d/wildcard-last"; fail=$((fail+1))
fi

# 负例:未声明的 concept 约束
cat > .cpp2build/badconcept.cppm <<'EOF'
module badconcept;
import std;
f: <T: NoSuch> (v: T) -> T = v;
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/badconcept.cppm 2>&1 | grep -q "unknown concept 'NoSuch'"; then
    echo "PASS m2d/unknown-concept"; pass=$((pass+1))
else
    echo "FAIL m2d/unknown-concept"; fail=$((fail+1))
fi

# 负例:concept 用作值类型(concept 是约束,不是类型)
cat > .cpp2build/conceptval.cppm <<'EOF'
module conceptval;
import std;
Ordered: concept = {
    operator<: (that: self) -> bool;
}
main: () -> int = {
    x: Ordered := 3;
    return 0;
}
EOF
if "$CPP2" transpile .cpp2build/conceptval.cppm 2>&1 | grep -q "is a constraint, not a value type"; then
    echo "PASS m2d/concept-not-type"; pass=$((pass+1))
else
    echo "FAIL m2d/concept-not-type"; fail=$((fail+1))
fi

# 负例:枚举成员模式用于非 enum scrutinee
cat > .cpp2build/enumpat.cppm <<'EOF'
module enumpat;
import std;
main: () -> int = {
    n: int := 5;
    match n {
        .red => std::println("x");
    }
    return 0;
}
EOF
if "$CPP2" transpile .cpp2build/enumpat.cppm 2>&1 | grep -q "match scrutinee must be an error-channel value, enum, variant"; then
    echo "PASS m2d/enum-pat-scrutinee"; pass=$((pass+1))
else
    echo "FAIL m2d/enum-pat-scrutinee"; fail=$((fail+1))
fi

# 负例:解构非 struct 的 variant 候选(int/string 无字段可解构)
cat > .cpp2build/destructalt.cppm <<'EOF'
module destructalt;
import std;
Value: variant = { int, string }
describe: (v: Value) -> string = match v {
    int(x)    => "i";
    string(s) => "s";
}
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/destructalt.cppm 2>&1 | grep -q "only struct alternatives can be destructured"; then
    echo "PASS m2d/destruct-nonstruct"; pass=$((pass+1))
else
    echo "FAIL m2d/destruct-nonstruct"; fail=$((fail+1))
fi

# 负例:main 不能是泛型(入口单态化无意义)
cat > .cpp2build/genmain.cppm <<'EOF'
module genmain;
import std;
main: <T> () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/genmain.cppm 2>&1 | grep -q "main cannot be generic"; then
    echo "PASS m2d/main-nongeneric"; pass=$((pass+1))
else
    echo "FAIL m2d/main-nongeneric"; fail=$((fail+1))
fi

# ── M2e:cpp2 check + .c2i 格式 v1(冻结)─────────────────────────
# check:快速语义检查,不生成代码
if "$CPP2" check examples/multimod/app.cppm 2>/dev/null | grep -q "2 module(s) ok"; then
    echo "PASS m2e/check-ok"; pass=$((pass+1))
else
    echo "FAIL m2e/check-ok"; fail=$((fail+1))
fi
if "$CPP2" check .cpp2build/unhandled.cppm 2>&1 | grep -q "unhandled error-channel"; then
    echo "PASS m2e/check-diag"; pass=$((pass+1))
else
    echo "FAIL m2e/check-diag"; fail=$((fail+1))
fi

# .c2i v1:二进制容器(magic C2IF + version 1)
c2i="examples/multimod/.cpp2build/mods/cpp2cache/app.c2i"
if "$CPP2" build examples/multimod/app.cppm >/dev/null 2>&1 \
   && head -c 4 "$c2i" 2>/dev/null | grep -q "C2IF"; then
    echo "PASS m2e/c2if-magic"; pass=$((pass+1))
else
    echo "FAIL m2e/c2if-magic"; fail=$((fail+1))
fi

echo
echo "passed $pass, failed $fail"
[[ $fail -eq 0 ]]
