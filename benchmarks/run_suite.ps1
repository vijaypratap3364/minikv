param(
    [string]$BuildDirectory = "build-bench",
    [string]$Output = "benchmarks/results/local.csv",
    [string]$Label = "local",
    [int]$Keys = 2000,
    [int]$ValueSize = 256,
    [int]$Operations = 4000,
    [int64]$SegmentSize = 262144,
    [string]$CMake = "cmake",
    [string]$Generator = "",
    [string]$MakeProgram = "",
    [string]$CxxCompiler = ""
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repository $BuildDirectory
$outputPath = Join-Path $repository $Output

$configureArguments = @(
    "-S", $repository,
    "-B", $buildPath,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DMINIKV_BUILD_BENCHMARKS=ON"
)
if ($Generator) { $configureArguments += @("-G", $Generator) }
if ($MakeProgram) {
    $configureArguments += "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
}
if ($CxxCompiler) {
    $configureArguments += "-DCMAKE_CXX_COMPILER=$CxxCompiler"
}
& $CMake @configureArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMake --build $buildPath --parallel 2 --target minikv_benchmark
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$benchmark = Join-Path $buildPath "minikv_benchmark.exe"
if (-not (Test-Path -LiteralPath $benchmark)) {
    $benchmark = Join-Path $buildPath "Release/minikv_benchmark.exe"
}

$common = @(
    "--keys", $Keys,
    "--value-size", $ValueSize,
    "--operations", $Operations,
    "--segment-size", $SegmentSize,
    "--seed", 84954583903062,
    "--output", $outputPath,
    "--label", $Label
)

$workloads = @(
    "sequential-put",
    "random-get",
    "update-heavy",
    "mixed",
    "delete-heavy",
    "recovery",
    "compaction"
)
foreach ($workload in $workloads) {
    & $benchmark --workload $workload @common
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

foreach ($threadCount in @(1, 2, 4, 8)) {
    & $benchmark --workload concurrent @common `
        --readers $threadCount --writers 0
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $benchmark --workload concurrent @common --readers 3 --writers 1
exit $LASTEXITCODE
