# tests/bench_sweep.ps1 - TU budget horizontal sweep (headers backend)
# For each budget: clean -> full build -> no-op rebuild -> single-module impl change.
# Probe = appending a NEW exported function (guaranteed generated-code change);
# asserts the incremental build really recompiled exactly 1 TU.
# ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less ps1 as ANSI.
param(
    [string]$Cpp2 = ".\.cpp2build\cpp2.exe",
    [string]$Target = "tests\bench\main.cpp2",
    [int[]]$Budgets = @(65536, 131072, 262144, 524288, 1048576, 2097152, 4194304)
)
$ErrorActionPreference = "Stop"

$probe = "tests\bench\m050.cpp2"
$backup = ".cpp2build\m050.orig.cpp2"
Copy-Item $probe $backup -Force

function Build([long]$budget) {
    $t = Measure-Command { cmd /c "$Cpp2 build $Target --max-tu-size=$budget 2>&1" | Out-Null }
    return $t.TotalSeconds
}

"budget`tfull_s`tparts`tnoop_ms`tincr_s`tincr_ok"
foreach ($b in $Budgets) {
    Remove-Item -Recurse -Force tests\bench\.cpp2build\hdr -ErrorAction SilentlyContinue

    $full = Build $b
    $parts = (Get-ChildItem tests\bench\.cpp2build\hdr\c2_part*.cpp).Count
    $avg = [int]((Get-ChildItem tests\bench\.cpp2build\hdr\c2_part*.cpp | Measure-Object Length -Average).Average)

    $t = Measure-Command { cmd /c "$Cpp2 build $Target --max-tu-size=$b 2>&1" | Out-Null }
    $noop = $t.TotalMilliseconds

    Copy-Item $backup $probe -Force
    Add-Content $probe ""
    Add-Content $probe "export probe_x: (v: int) -> int = v * 31 + 7;"
    $incr = 0.0
    $out = ""
    $t = Measure-Command { $script:out = cmd /c "$Cpp2 build $Target --max-tu-size=$b 2>&1" | Out-String }
    $incr = $t.TotalSeconds
    $ok = ($out -match "1 transpiled")

    Copy-Item $backup $probe -Force
    "$b`t$([math]::Round($full,1))`t$parts ($([math]::Round($avg/1024))KB)`t$([int]$noop)`t$([math]::Round($incr,1))`t$ok"
}
Copy-Item $backup $probe -Force
