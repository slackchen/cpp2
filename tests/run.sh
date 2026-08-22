#!/usr/bin/env bash
# tests/run.sh — 全量回归
#   示例:期望退出 0 且输出含期望行
#   trap 用例:期望非零退出且 stderr 含 trap 消息(可选断言 #line 位置映射)
set -u
cd "$(dirname "$0")/.."
# 跨平台:Linux 构建产物无 .exe 后缀(与 build.sh 的命名规则一致)
CPP2="./.cpp2build/cpp2"
[[ -x "./.cpp2build/cpp2.exe" ]] && CPP2="./.cpp2build/cpp2.exe"
pass=0; fail=0

# ── 示例(期望输出)──────────────────────────────────────────────
run_case() { # $1=cpp2路径 $2=期望片段 $3=是否期望 trap [$4=期望位置片段]
    local out code
    out="$("$CPP2" run "$1" 2>&1)"; code=$?
    if [[ $3 == "trap" ]]; then
        if [[ $code -ne 0 && "$out" == *"$2"* && ( -z "${4:-}" || "$out" == *"$4"* ) ]]; then
            echo "PASS $1"; pass=$((pass+1))
        else
            echo "FAIL $1 (exit $code, want trap: $2 ${4:-})"
            printf '%s\n' "$out" | head -12
        fi
    else
        if [[ $code -eq 0 && "$out" == *"$2"* ]]; then
            echo "PASS $1"; pass=$((pass+1))
        else
            echo "FAIL $1 (exit $code, want output: $2)"
            printf '%s\n' "$out" | head -12
        fi
    fi
}

run_case examples/hello.cpp2  "Hello, C++2!"          ok
run_case examples/funcs.cpp2  "hypot2 = 25"           ok
run_case examples/funcs.cpp2  "a bumped = 4"          ok
run_case examples/loops.cpp2  "sum 1..=100 = 5050"    ok
run_case examples/point.cpp2  "len = 5"               ok
run_case examples/point.cpp2  "q.len = 0"             ok
run_case examples/colors.cpp2 "green as int = 1"      ok
run_case examples/safety.cpp2 "sum = 7"               ok
run_case examples/safety.cpp2 "total = 60"            ok
run_case examples/smart.cpp2  "norm2 = 25"            ok
run_case examples/smart.cpp2  "shared x = 1"          ok

# ── trap 用例(期望检查触发;断言消息与 .cpp2 源位置)─────────────
run_case tests/cases/overflow_trap.cpp2 "integer overflow"                    trap "overflow_trap.cpp2:6"
run_case tests/cases/bounds_trap.cpp2   "index out of bounds"                 trap "bounds_trap.cpp2:9"
run_case tests/cases/div_trap.cpp2      "division by zero"                    trap "div_trap.cpp2:7"
run_case tests/cases/null_trap.cpp2     "null dereference"                    trap "null_trap.cpp2:13"
run_case tests/cases/float_trap.cpp2    "float-to-integer conversion out of range" trap "float_trap.cpp2:7"

# @unsafe/@unchecked 块形式 + 逃逸后检查恢复
run_case tests/cases/optout.cpp2        "sum = 11"                            ok

# ── M3:多模块 ─────────────────────────────────────────────────
# 摊平模式 run(跨模块符号解析)
run_case examples/multimod/app.cpp2 "norm2 = 25"        ok
run_case examples/multimod/app.cpp2 "doubled_add(2,3) = 10" ok

# headers 后端(默认,不依赖 C++20 modules):build + 运行产物
if "$CPP2" build examples/multimod/app.cpp2 >/dev/null 2>&1 \
   && ./examples/multimod/.cpp2build/app 2>/dev/null | grep -q "norm2 = 25"; then
    echo "PASS m3/headers-build"; pass=$((pass+1))
else
    echo "FAIL m3/headers-build"; fail=$((fail+1))
fi

# no-op 增量:重建必须零转译零编译(内容寻址:生成码字节不变即免编译)
if "$CPP2" build examples/multimod/app.cpp2 2>&1 | grep -q "0 transpiled.*0 compiled"; then
    echo "PASS m3/incremental-noop"; pass=$((pass+1))
else
    echo "FAIL m3/incremental-noop"; fail=$((fail+1))
fi

# 装箱:--max-tu-size=1 强制每模块独立 TU(尽可能少文件 ↔ 预算约束的两端)
if "$CPP2" build examples/multimod/app.cpp2 --max-tu-size=1 >/dev/null 2>&1 \
   && ls examples/multimod/.cpp2build/hdr/c2_part0.cpp >/dev/null 2>&1 \
   && ls examples/multimod/.cpp2build/hdr/c2_part1.cpp >/dev/null 2>&1 \
   && ./examples/multimod/.cpp2build/app 2>/dev/null | grep -q "doubled_add(2,3) = 10"; then
    echo "PASS m3/headers-multipack"; pass=$((pass+1))
else
    echo "FAIL m3/headers-multipack"; fail=$((fail+1))
fi

# cxx20-modules 后端(opt-in):named module + BMI 路径保持可用
if "$CPP2" build examples/multimod/app.cpp2 --backend=cxx20-modules >/dev/null 2>&1 \
   && ./examples/multimod/.cpp2build/app 2>/dev/null | grep -q "norm2 = 25"; then
    echo "PASS m3/modules-build"; pass=$((pass+1))
else
    echo "FAIL m3/modules-build"; fail=$((fail+1))
fi

# export-headers:Cpp1 消费者互操作
if "$CPP2" export-headers tests/cases/exportlib.cpp2 -o tests/cases/bridge >/dev/null 2>&1 \
   && g++ -std=c++23 -I"$(pwd)/rt" -I tests/cases/bridge \
        tests/cases/bridge/app_lib.cpp tests/cases/consumer.cpp \
        -o tests/cases/bridge/consumer.exe 2>/dev/null \
   && ./tests/cases/bridge/consumer.exe 2>/dev/null | grep -q "42"; then
    echo "PASS m3/cpp1-interop"; pass=$((pass+1))
else
    echo "FAIL m3/cpp1-interop"; fail=$((fail+1))
fi

# ── M4 收口:invariant 类型不变量(DESIGN §6.5)──────────────────
run_case examples/invariant.cpp2 "balance = 100"            ok
run_case examples/invariant.cpp2 "after withdraw = 70"      ok
# 故障注入:出口违反不变量 → trap(带源位置)
run_case tests/cases/invariant_trap.cpp2 "invariant violated: Box" trap "invariant_trap.cpp2:7"
# audit 计数:Account 两个 mutates 方法受守卫
if "$CPP2" audit examples/invariant.cpp2 2>/dev/null | grep -q "invariant 2"; then
    echo "PASS m4/audit-invariant"; pass=$((pass+1))
else
    echo "FAIL m4/audit-invariant"; fail=$((fail+1))
fi

# ── 全特性展示(showcase):语言面冒烟 ───────────────────────────
run_case examples/showcase.cpp2 "A: a=42 b=84 c=7 norm2=25" ok
run_case examples/showcase.cpp2 "E: area = 12.56636"        ok
run_case examples/showcase.cpp2 "G: or=7/-1 must=5"         ok
run_case examples/showcase.cpp2 "I: clamp=5/2 max3=9 lambda=36" ok
run_case examples/showcase.cpp2 "K: v[2]=3 ptr=5/61 narrow=2000000000" ok
run_case examples/showcase.cpp2 "M: for-sum=6 while-w=3"    ok

# ── M5:生存期 Lite(L1/L2)悬垂捕获率(出口判据:报告)──────────
# 语料:tests/lifetime/*.cpp2,首行 // expect-error: <片段> 或 // expect-ok
total=0; caught=0
for f in tests/lifetime/*.cpp2; do
    total=$((total+1))
    out=$("$CPP2" check "$f" 2>&1)
    if grep -q "expect-ok" "$f"; then
        if ! grep -q "error" <<<"$out"; then caught=$((caught+1)); fi
    else
        msg=$(grep -o 'expect-error: .*' "$f" | head -1 | sed 's/^.*expect-error: //')
        if [[ -n "$msg" ]] && grep -qF -- "$msg" <<<"$out"; then caught=$((caught+1)); fi
    fi
done
echo "lifetime capture rate: $caught/$total"
if [[ $caught -eq $total && $total -gt 0 ]]; then
    echo "PASS m5/lifetime-capture"; pass=$((pass+1))
else
    echo "FAIL m5/lifetime-capture ($caught/$total)"; fail=$((fail+1))
fi

# ── M6:legacy 块 / @unsafe 指针(L5)/ arena(L6)────────────────
run_case examples/unsafe_demo.cpp2 "legacy = 5"                     ok
run_case examples/unsafe_demo.cpp2 "total=42 arena-point=145"       ok
run_case examples/unsafe_demo.cpp2 "after reset live=0"             ok
# audit:legacy 块白纸黑字
if "$CPP2" audit examples/unsafe_demo.cpp2 2>/dev/null | grep -q "legacy : 1 cxx_legacy block"; then
    echo "PASS m6/audit-legacy"; pass=$((pass+1))
else
    echo "FAIL m6/audit-legacy"; fail=$((fail+1))
fi

# ── M6 切片二:gc<T> 保守式收集器 + zlib 双向互操作 ──────────────
# gc:安全性断言(活对象必存活;回收率保守式非确定,不作断言)
run_case examples/gc_demo.cpp2 "kept.id=42 (survived)"      ok
run_case examples/gc_demo.cpp2 "churn-last=998"             ok
run_case examples/gc_demo.cpp2 "collections=1"              ok

# zlib:本机无 zlib.h 时跳过(CI ubuntu 自带 zlib1g-dev 实测)
if echo '#include <zlib.h>' | g++ -x c++ - -fsyntax-only >/dev/null 2>&1; then
    run_case examples/zlib_demo.cpp2 "compress rc=0"        ok
    run_case examples/zlib_demo.cpp2 "roundtrip-equal=true" ok
else
    echo "SKIP m6/zlib (zlib.h not found on this host; covered by CI)"
fi

# ── M4:检查器完备 / audit / fuzz ────────────────────────────────
# audit:检查注入点计数 + @unsafe/@unchecked 位置(白纸黑字)
audit_out="$("$CPP2" audit examples/smart.cpp2 2>/dev/null; "$CPP2" audit examples/safety.cpp2 2>/dev/null)"
if [[ "$audit_out" == *"deref 4"* && "$audit_out" == *"opt-out: @unchecked at line 20"* ]]; then
    echo "PASS m4/audit"; pass=$((pass+1))
else
    echo "FAIL m4/audit"; fail=$((fail+1))
fi
audit_out="$("$CPP2" audit tests/cases/optout.cpp2 2>/dev/null)"
if [[ "$audit_out" == *"opt-out: @unsafe at line 13"* && "$audit_out" == *"@unchecked x2, @unsafe x1"* \
      && "$audit_out" == *"index 1"* ]]; then    # 块外 v[0] 恢复检查
    echo "PASS m4/audit-optout"; pass=$((pass+1))
else
    echo "FAIL m4/audit-optout"; fail=$((fail+1))
fi

# fuzz:固定 seed 可复现,期望零崩溃
if "$CPP2" fuzz examples/*.cpp2 examples/multimod/*.cpp2 tests/cases/*.cpp2 \
        --seed 20260820 --iters 10000 2>/dev/null | grep -q "0 crashes"; then
    echo "PASS m4/fuzz"; pass=$((pass+1))
else
    echo "FAIL m4/fuzz"; fail=$((fail+1))
fi

# 深度防护:3000 层嵌套表达式 → 干净诊断而非进程崩溃
mkdir -p .cpp2build
perl -e 'print "main: () -> int = { x: int := ", "("x3000, "1", ")"x3000, "; return 0; }"' \
    > .cpp2build/deep.cpp2 2>/dev/null
if "$CPP2" transpile .cpp2build/deep.cpp2 2>&1 | grep -q "nesting too deep"; then
    echo "PASS m4/depth-guard"; pass=$((pass+1))
else
    echo "FAIL m4/depth-guard"; fail=$((fail+1))
fi

# ── M2c:错误通道 + 契约 ──────────────────────────────────────────
# 错误通道五件套:? 传播 / ! 断言 / or 默认 / match ok-err / if-let(DESIGN §8)
run_case examples/errors.cpp2 "a = 21"                                ok
run_case examples/errors.cpp2 "b = 7"                                 ok
run_case examples/errors.cpp2 "double = 40"                           ok
run_case examples/errors.cpp2 "failed: not a digit in '2x' (examples/errors.cpp2:12)" ok
run_case examples/errors.cpp2 "v = 99"                                ok
# 契约:pre/post/old()/result(DESIGN §6.5)
run_case examples/contract.cpp2 "w = 30, balance = 70"                ok
run_case examples/contract.cpp2 "int_sqrt(26) = 5"                    ok
run_case examples/contract.cpp2 "value = 5"                           ok
# throws + post 组合(错误通道上的出口检查)
run_case tests/cases/errpost.cpp2 "half = 5"                          ok
run_case tests/cases/errpost.cpp2 "failed: odd input (tests/cases/errpost.cpp2:9)" ok
# 故障注入:契约违反 / ! 断言失败 = bug → trap(带 .cpp2 源位置)
run_case tests/cases/pre_trap.cpp2  "precondition failed"             trap "pre_trap.cpp2:6"
run_case tests/cases/post_trap.cpp2 "postcondition failed"            trap "post_trap.cpp2:6"
run_case tests/cases/must_trap.cpp2 "error asserted impossible"       trap "must_trap.cpp2:10"

# audit:契约计数(contract.cpp2 = withdraw/int_sqrt/bump 各 pre+post = 6)
if "$CPP2" audit examples/contract.cpp2 2>/dev/null | grep -q "contract 6"; then
    echo "PASS m2c/audit-contract"; pass=$((pass+1))
else
    echo "FAIL m2c/audit-contract"; fail=$((fail+1))
fi

# 模块模式:expected 签名跨 BMI 正常(模块单元 + 链接)
if "$CPP2" build examples/errors.cpp2 >/dev/null 2>&1 \
   && ./examples/.cpp2build/errors 2>/dev/null | grep -q "double = 40"; then
    echo "PASS m2c/module-errors"; pass=$((pass+1))
else
    echo "FAIL m2c/module-errors"; fail=$((fail+1))
fi

# 负例:未处理错误通道调用 → 干净诊断(编译器强制处理,DESIGN §8.1)
cat > .cpp2build/unhandled.cpp2 <<'EOF'
module unhandled;
import std;
f: () -> int throws = { return err("boom"); }
main: () -> int = { f(); return 0; }
EOF
if "$CPP2" transpile .cpp2build/unhandled.cpp2 2>&1 | grep -q "unhandled error-channel"; then
    echo "PASS m2c/unhandled-call"; pass=$((pass+1))
else
    echo "FAIL m2c/unhandled-call"; fail=$((fail+1))
fi

# 负例:非 throws 函数内用 '?' → 干净诊断
cat > .cpp2build/nonthrows.cpp2 <<'EOF'
module nonthrows;
import std;
f: () -> int throws = { return 1; }
g: () -> int = { n: int := f()?; return n; }
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/nonthrows.cpp2 2>&1 | grep -q "enclosing function to be 'throws'"; then
    echo "PASS m2c/prop-requires-throws"; pass=$((pass+1))
else
    echo "FAIL m2c/prop-requires-throws"; fail=$((fail+1))
fi

# ── M2d:泛型/concept、variant、optional、模式匹配、UFCS ─────────
# 正向(DESIGN §4.5–§5.6 示例全绿)
run_case examples/shapes.cpp2   "circle area = 12.56636"   ok
run_case examples/shapes.cpp2   "rect area = 12"           ok
run_case examples/shapes.cpp2   "neg area = 0"             ok     # 守卫失败落入同模式后续臂
run_case examples/shapes.cpp2   "z is rectangle"           ok
run_case examples/shapes.cpp2   "go"                       ok
run_case examples/optional.cpp2 "found ada"                ok     # if-let 绑定解包值
run_case examples/optional.cpp2 "9 missing"                ok     # if-let else 分支
run_case examples/optional.cpp2 "hello, grace"             ok     # match some/none 表达式
run_case examples/generics.cpp2 "n to_string = 42"         ok     # UFCS 桥接 std::to_string
run_case examples/generics.cpp2 "clamp -3 = 0"             ok     # 泛型 + concept 约束
run_case examples/generics.cpp2 "clamp 2.5 = 2"            ok     # 同一泛型多类型实例化
run_case examples/generics.cpp2 "mid3 = 3"                 ok     # requires 子句
run_case examples/generics.cpp2 "squares sum = 30"         ok     # lambda 实参
run_case examples/types.cpp2    "rex says woof (2 tricks)" ok     # 继承:基类字段经派生访问
run_case examples/types.cpp2    "cleanup buddy"            ok     # 析构器:块出口确定性调用
run_case examples/types.cpp2    "cleanup rex"              ok     # 析构顺序与构造相反

# 负例:variant 非穷尽 → 干净诊断(match 是唯一合法访问,穷尽性编译器保证)
cat > .cpp2build/nonexhaustive.cpp2 <<'EOF'
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
if "$CPP2" transpile .cpp2build/nonexhaustive.cpp2 2>&1 | grep -q "match arms must be exhaustive"; then
    echo "PASS m2d/variant-exhaustive"; pass=$((pass+1))
else
    echo "FAIL m2d/variant-exhaustive"; fail=$((fail+1))
fi

# 负例:'_' 通配不在末尾
cat > .cpp2build/wildpos.cpp2 <<'EOF'
module wildpos;
import std;
Signal: enum = { red, green }
f: (s: Signal) -> int = match s {
    _      => 0;
    .red   => 1;
}
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/wildpos.cpp2 2>&1 | grep -q "'_' must be the last match arm"; then
    echo "PASS m2d/wildcard-last"; pass=$((pass+1))
else
    echo "FAIL m2d/wildcard-last"; fail=$((fail+1))
fi

# 负例:未声明的 concept 约束
cat > .cpp2build/badconcept.cpp2 <<'EOF'
module badconcept;
import std;
f: <T: NoSuch> (v: T) -> T = v;
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/badconcept.cpp2 2>&1 | grep -q "unknown concept 'NoSuch'"; then
    echo "PASS m2d/unknown-concept"; pass=$((pass+1))
else
    echo "FAIL m2d/unknown-concept"; fail=$((fail+1))
fi

# 负例:concept 用作值类型(concept 是约束,不是类型)
cat > .cpp2build/conceptval.cpp2 <<'EOF'
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
if "$CPP2" transpile .cpp2build/conceptval.cpp2 2>&1 | grep -q "is a constraint, not a value type"; then
    echo "PASS m2d/concept-not-type"; pass=$((pass+1))
else
    echo "FAIL m2d/concept-not-type"; fail=$((fail+1))
fi

# 负例:枚举成员模式用于非 enum scrutinee
cat > .cpp2build/enumpat.cpp2 <<'EOF'
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
if "$CPP2" transpile .cpp2build/enumpat.cpp2 2>&1 | grep -q "match scrutinee must be an error-channel value, enum, variant"; then
    echo "PASS m2d/enum-pat-scrutinee"; pass=$((pass+1))
else
    echo "FAIL m2d/enum-pat-scrutinee"; fail=$((fail+1))
fi

# 负例:解构非 struct 的 variant 候选(int/string 无字段可解构)
cat > .cpp2build/destructalt.cpp2 <<'EOF'
module destructalt;
import std;
Value: variant = { int, string }
describe: (v: Value) -> string = match v {
    int(x)    => "i";
    string(s) => "s";
}
main: () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/destructalt.cpp2 2>&1 | grep -q "only struct alternatives can be destructured"; then
    echo "PASS m2d/destruct-nonstruct"; pass=$((pass+1))
else
    echo "FAIL m2d/destruct-nonstruct"; fail=$((fail+1))
fi

# 负例:main 不能是泛型(入口单态化无意义)
cat > .cpp2build/genmain.cpp2 <<'EOF'
module genmain;
import std;
main: <T> () -> int = { return 0; }
EOF
if "$CPP2" transpile .cpp2build/genmain.cpp2 2>&1 | grep -q "main cannot be generic"; then
    echo "PASS m2d/main-nongeneric"; pass=$((pass+1))
else
    echo "FAIL m2d/main-nongeneric"; fail=$((fail+1))
fi

# 负例:调用点核对(sema 层拦截,不再漏到 C++ 编译期)
cat > .cpp2build/negarity.cppm <<'EOF'
module negarity;
import std;
f: (a: int, b: int, c: int) -> int = a + b + c;
main: () -> int = { return f(1, 2); }
EOF
if "$CPP2" transpile .cpp2build/negarity.cppm 2>&1 | grep -q "expects 3 argument(s), got 2"; then
    echo "PASS m3c/call-arity"; pass=$((pass+1))
else
    echo "FAIL m3c/call-arity"; fail=$((fail+1))
fi

cat > .cpp2build/negargtype.cppm <<'EOF'
module negargtype;
import std;
P: type = { x: int = 0; }
g: (s: string) -> int = 0;
main: () -> int = { return g(P{}); }
EOF
if "$CPP2" transpile .cpp2build/negargtype.cppm 2>&1 | grep -q "expects 'string', got 'P'"; then
    echo "PASS m3c/call-argtype"; pass=$((pass+1))
else
    echo "FAIL m3c/call-argtype"; fail=$((fail+1))
fi

cat > .cpp2build/negmetharity.cppm <<'EOF'
module negmetharity;
import std;
C: type = { v: int = 0; inc: (d: int) mutates = { v += d; } }
main: () -> int = { c: C := C{}; c.inc(); return 0; }
EOF
if "$CPP2" transpile .cpp2build/negmetharity.cppm 2>&1 | grep -q "expects 1 argument(s), got 0"; then
    echo "PASS m3c/method-arity"; pass=$((pass+1))
else
    echo "FAIL m3c/method-arity"; fail=$((fail+1))
fi

# 负例:方法不是值(赋值目标为方法 → 干净诊断而非 C++ 报错)
cat > .cpp2build/negmethval.cppm <<'EOF'
module negmethval;
import std;
A: type = { speak: () -> string = "x"; }
main: () -> int = {
    a: A := A{};
    a.speak = () -> string = "y";
    return 0;
}
EOF
if "$CPP2" transpile .cpp2build/negmethval.cppm 2>&1 | grep -q "'speak' is a method, not a value"; then
    echo "PASS m3c/method-not-value"; pass=$((pass+1))
else
    echo "FAIL m3c/method-not-value"; fail=$((fail+1))
fi

# 后端诊断过滤:concept 违反(设计上委托 C++ 判定)→ 漏到后端的错误
# 必须带横幅 + 位置映射回 .cpp2,且无生成码路径噪声
cat > .cpp2build/negconceptsat.cpp2 <<'EOF'
module negconceptsat;
import std;
Ordered: concept = { operator<: (that: self) -> bool; }
S: type = { x: int = 0; }
pick: <T: Ordered> (a: T, b: T) -> T = {
    if a < b { return a; }
    return b;
}
main: () -> int = {
    s := pick(S{}, S{});
    std::println("{0}", s.x);
    return 0;
}
EOF
out="$("$CPP2" run .cpp2build/negconceptsat.cpp2 2>&1)"
if [[ $? -ne 0 || "$out" == *"error"* ]] && [[ "$out" == *"backend (C++23)"* ]] \
   && [[ "$out" == *"negconceptsat.cpp2"* ]] && ! [[ "$out" == *"[generated]"* ]]; then
    echo "PASS m3c/backend-diag-filter"; pass=$((pass+1))
else
    echo "FAIL m3c/backend-diag-filter"; fail=$((fail+1))
fi

# ── M2e:cpp2 check + .c2i 格式 v1(冻结)─────────────────────────# check:快速语义检查,不生成代码
if "$CPP2" check examples/multimod/app.cpp2 2>/dev/null | grep -q "2 module(s) ok"; then
    echo "PASS m2e/check-ok"; pass=$((pass+1))
else
    echo "FAIL m2e/check-ok"; fail=$((fail+1))
fi
if "$CPP2" check .cpp2build/unhandled.cpp2 2>&1 | grep -q "unhandled error-channel"; then
    echo "PASS m2e/check-diag"; pass=$((pass+1))
else
    echo "FAIL m2e/check-diag"; fail=$((fail+1))
fi

# .c2i v1:二进制容器(magic C2IF + version 1)
c2i="examples/multimod/.cpp2build/hdr/cpp2cache/app.c2i"
if "$CPP2" build examples/multimod/app.cpp2 >/dev/null 2>&1 \
   && head -c 4 "$c2i" 2>/dev/null | grep -q "C2IF"; then
    echo "PASS m2e/c2if-magic"; pass=$((pass+1))
else
    echo "FAIL m2e/c2if-magic"; fail=$((fail+1))
fi

echo
echo "passed $pass, failed $fail"
[[ $fail -eq 0 ]]
