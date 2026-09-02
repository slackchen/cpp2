#!/usr/bin/env bash
# tools/native_cmp.sh — examples native 模式验收
#   基准 = 转译模式(默认 run)的 stdout;native 产物 stdout 必须一致(归一 \r)且退出 0
#   用法: bash tools/native_cmp.sh [-v] [name ...]   (name = examples/<name>.cpp2 的 <name>)
#
#   产物执行注意(本机安全软件行为):cpp2 进程树直写/直执行的 PE 会被拦(Access denied),
#   同字节文件经外部 shell 重新 cp 出的实例可执行。故此脚本经 `cpp2 build --backend=native`
#   取产物,rm+cp 重建文件实例后再执行。在无此类拦截的环境可直接 `cpp2 run --backend=native`。
set -u
cd "$(dirname "$0")/.."
CPP2="./.cpp2build/cpp2"
[[ -x "./.cpp2build/cpp2.exe" ]] && CPP2="./.cpp2build/cpp2.exe"
verbose=0; files=()
for a in "$@"; do
  [[ "$a" == "-v" ]] && verbose=1 || files+=("$a")
done
# 允许传裸名(如 errcat)或路径(如 examples/errcat.cpp2)
for i in "${!files[@]}"; do
  [[ "${files[$i]}" != */* ]] && files[$i]="examples/${files[$i]}.cpp2"
done
if [[ ${#files[@]} -eq 0 ]]; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      files=(examples/*.cpp2 examples/multimod/app.cpp2) ;;   # Win64 发射器:全量
    *)
      # SysV 发射器为整数核心原型(v0):仅 smoke 子集
      files=(examples/hello.cpp2 examples/loops.cpp2 examples/funcs.cpp2) ;;
  esac
fi
pass=0; fail=0; skip=0
tmp="examples/.cpp2build/native"
mkdir -p "$tmp"
for f in "${files[@]}"; do
  name=$(basename "$f" .cpp2)
  # 基线:先落盘取真实 rc(管道/命令替换会掩盖失败),再归一 \r
  "$CPP2" run "$f" > "$tmp/${name}.base" 2>/dev/null; brc=$?
  base=$(tr -d '\r' < "$tmp/${name}.base")
  if [[ $brc -ne 0 ]]; then
    echo "SKIP $f (转译基线失败 rc=$brc)"; skip=$((skip+1))
    continue
  fi
  blog=$("$CPP2" build "$f" --backend=native 2>&1); bld=$?
  exe=$(printf '%s\n' "$blog" | grep -E '\.exe$' | tail -1)
  if [[ $bld -ne 0 || -z "$exe" || ! -f "$exe" ]]; then
    echo "FAIL $f (native 构建失败)"
    fail=$((fail+1))
    [[ $verbose -eq 1 ]] && printf '%s\n' "$blog" | grep -v "^\[native\]" | head -6
    continue
  fi
  runexe="$tmp/${name}.acc.exe"
  rm -f "$runexe"; cp "$exe" "$runexe"
  # 先取真实退出码(管道会掩盖崩潰),再归一 \r
  "$runexe" > "$tmp/${name}.out" 2>/dev/null; nrc=$?
  nat=$(tr -d '\r' < "$tmp/${name}.out")
  if [[ $nrc -ne 0 ]]; then
    echo "FAIL $f (native 运行 rc=$nrc)"
    fail=$((fail+1)); continue
  fi
  # 本机安全软件偶发拦新落盘产物(静默空输出/截断,基线与 native 侧均可波及):
  # 输出不一致时重建 native 文件实例重试至多 2 次;仍不一致再重生成基线一次。
  # 真实回归不受影响(两侧都重取后仍不一致才判 FAIL)
  for retry in 1 2; do
    [[ "$nat" == "$base" ]] && break
    rm -f "$runexe"; cp "$exe" "$runexe"; sleep 1
    "$runexe" > "$tmp/${name}.out" 2>/dev/null; nrc=$?
    nat=$(tr -d '\r' < "$tmp/${name}.out")
    [[ $nrc -ne 0 ]] && break
  done
  if [[ "$nat" != "$base" ]]; then
    "$CPP2" run "$f" > "$tmp/${name}.base" 2>/dev/null; brc=$?
    if [[ $brc -eq 0 ]]; then base=$(tr -d '\r' < "$tmp/${name}.base"); fi
  fi
  if [[ "$nat" != "$base" ]]; then
    echo "FAIL $f (输出不一致)"
    fail=$((fail+1))
    if [[ $verbose -eq 1 ]]; then
      diff <(printf '%s\n' "$base") <(printf '%s\n' "$nat") | head -12
    fi
    continue
  fi
  echo "PASS $f"; pass=$((pass+1))
done
echo "== native examples: $pass pass, $fail fail, $skip skip =="
exit $((fail > 0))
