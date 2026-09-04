param(
    [string]$BuildDirectory = "build-profile",
    [string]$Output = "benchmarks/results/gprof.txt",
    [string]$CMake = "cmake",
    [string]$Gprof = "gprof"
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repository $BuildDirectory
$outputPath = Join-Path $repository $Output

& $CMake -S $repository -B $buildPath -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DMINIKV_BUILD_BENCHMARKS=ON -DMINIKV_ENABLE_GPROF=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMake --build $buildPath --parallel 2 --target minikv_benchmark
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$benchmark = Join-Path $buildPath "minikv_benchmark.exe"
if (-not (Test-Path -LiteralPath $benchmark)) {
    $benchmark = Join-Path $buildPath "RelWithDebInfo/minikv_benchmark.exe"
}
$benchmark = (Resolve-Path -LiteralPath $benchmark).Path
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) |
    Out-Null

Push-Location $buildPath
try {
    Remove-Item -LiteralPath "gmon.out" -ErrorAction SilentlyContinue
    $arguments = @(
        "--workload", "sequential-put",
        "--keys", "4000",
        "--value-size", "4096",
        "--operations", "4000",
        "--segment-size", "67108864",
        "--seed", "84954583903062",
        "--label", "gprof-focus"
    )
    & $benchmark @arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath "gmon.out")) {
        throw "gprof instrumentation did not create gmon.out"
    }

    $header = @(
        "MiniKV GNU gprof CPU profile",
        "Executable: $benchmark",
        "Arguments: $($arguments -join ' ')",
        "Generated UTC: $([DateTime]::UtcNow.ToString('o'))",
        ""
    )
    $profile = & $Gprof $benchmark "gmon.out"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    @($header + $profile) | Set-Content -LiteralPath $outputPath
}
finally {
    Pop-Location
}

Write-Host "Profile written to $outputPath"
