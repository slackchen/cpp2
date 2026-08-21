# tests/bench_gen.ps1 - large-scale benchmark generator (M3b compile-effect test)
# Generates N modules forming a dependency DAG (chain + cross links). Each module:
# exported struct/variant/match + internal helpers + exported functions
# (some cross-module calls, some bounds-checked indexing).
# Usage: powershell -File tests/bench_gen.ps1 -Count 120 -Funcs 80
# NOTE: ASCII-only on purpose - Windows PowerShell 5.1 reads BOM-less ps1 as ANSI.
param(
    [int]$Count = 120,
    [int]$Funcs = 80,
    [string]$Root = "tests/bench"
)
$ErrorActionPreference = "Stop"
$utf8 = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force -Path $Root | Out-Null

function ModName([int]$i) { "bench.m{0:D3}" -f $i }

for ($i = 0; $i -lt $Count; $i++) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("// bench module $i (generated; DO NOT EDIT)")
    [void]$sb.AppendLine(("module " + (ModName $i) + ";"))
    [void]$sb.AppendLine("import std;")
    if ($i -gt 0) { [void]$sb.AppendLine(("import " + (ModName ($i - 1)) + ";")) }
    if ($i -gt 3) { [void]$sb.AppendLine(("import " + (ModName ($i - 3)) + ";")) }
    [void]$sb.AppendLine("")

    [void]$sb.AppendLine("export P$i`: type = {")
    [void]$sb.AppendLine("    x: int = 0;")
    [void]$sb.AppendLine("    y: int = 0;")
    [void]$sb.AppendLine("    norm2: () -> int = x * x + y * y;")
    [void]$sb.AppendLine("    bump: (d: int) mutates = { x += d; }")
    [void]$sb.AppendLine("}")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("export V$i`: variant = { P$i, int }")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("export describe$i`: (s: V$i) -> int = match s {")
    [void]$sb.AppendLine("    P$i p => p.norm2();")
    [void]$sb.AppendLine("    int n => n;")
    [void]$sb.AppendLine("}")
    [void]$sb.AppendLine("")

    $half = [Math]::Floor($Funcs / 2)
    for ($k = 0; $k -lt $Funcs; $k++) {
        [void]$sb.AppendLine("g${i}_$k`: (v: int) -> int = v + $k;")
    }
    [void]$sb.AppendLine("")

    for ($k = 0; $k -lt $Funcs; $k++) {
        if ($k % 8 -eq 7) {
            [void]$sb.AppendLine("export f${i}_$k`: (a: int, b: int) -> int = {")
            [void]$sb.AppendLine("    v: vector<int> := {a, b, $k};")
            [void]$sb.AppendLine("    return v[1] + v[0] * 2 + v[2];")
            [void]$sb.AppendLine("}")
        } elseif ($i -gt 0 -and $k % 4 -eq 3) {
            [void]$sb.AppendLine("export f${i}_$k`: (a: int, b: int) -> int = {")
            [void]$sb.AppendLine("    s: int := a + b + $k;")
            [void]$sb.AppendLine("    if s > $($k + 10) { s -= b; }")
            [void]$sb.AppendLine("    return s + g${i}_$k(a) + f$($i - 1)_$k(b, a);")
            [void]$sb.AppendLine("}")
        } else {
            [void]$sb.AppendLine("export f${i}_$k`: (a: int, b: int) -> int = {")
            [void]$sb.AppendLine("    s: int := a * $($k + 1) + b;")
            [void]$sb.AppendLine("    if s > $k { s -= a; }")
            [void]$sb.AppendLine("    return s + g${i}_$k(b);")
            [void]$sb.AppendLine("}")
        }
    }
    $path = Join-Path $Root (("m{0:D3}" -f $i) + ".cpp2")
    [System.IO.File]::WriteAllText($path, $sb.ToString(), $utf8)
}

# Root module: import all, sample cross-module calls + variant match + indexing.
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// bench root (generated; DO NOT EDIT)")
[void]$sb.AppendLine("module bench.main;")
[void]$sb.AppendLine("import std;")
for ($i = 0; $i -lt $Count; $i++) { [void]$sb.AppendLine(("import " + (ModName $i) + ";")) }
[void]$sb.AppendLine("")
[void]$sb.AppendLine("main: () -> int = {")
[void]$sb.AppendLine("    total: int := 0;")
for ($i = 0; $i -lt $Count; $i += 10) {
    [void]$sb.AppendLine(("    total += f{0}_1({1}, 2);" -f $i, $i))
}
[void]$sb.AppendLine("    p0: P0 := P0{.x = 3, .y = 4};")
[void]$sb.AppendLine("    total += describe0(p0);")
[void]$sb.AppendLine(("    total += f{0}_7(5, 6);" -f ($Count - 1)))
[void]$sb.AppendLine("    std::println(`"bench ok: {0}`", total);")
[void]$sb.AppendLine("    return 0;")
[void]$sb.AppendLine("}")
[System.IO.File]::WriteAllText((Join-Path $Root "main.cpp2"), $sb.ToString(), $utf8)

Write-Output ("generated: $Count modules x $Funcs funcs -> " + (Join-Path $Root "main.cpp2"))
