# Reproducible benchmarks

`minikv_benchmark` is a dependency-free C++ runner for the eight Stage 8
workloads. It records each logical operation with `std::chrono::steady_clock`
and reports throughput plus nearest-rank p50, p95, and p99 latency. Setup work,
including database creation and prefilling, is outside the timed interval.

Configure a Release build explicitly. Benchmarking a Debug build mostly
measures missing compiler optimizations.

```powershell
cmake -S . -B build-bench `
  -DCMAKE_BUILD_TYPE=Release `
  -DMINIKV_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel 2
./build-bench/minikv_benchmark.exe --workload random-get `
  --keys 10000 --value-size 256 --operations 20000
```

Run the standard workload suite with:

```powershell
./benchmarks/run_suite.ps1 -BuildDirectory build-bench `
  -Output benchmarks/results/local.csv -Label baseline
```

The suite covers sequential PUT, random GET, update-heavy, mixed GET/PUT,
delete-heavy (80% erase attempts and 20% PUTs), restart recovery, compaction,
and concurrent access. It also runs
read-only concurrency with 1, 2, 4, and 8 reader threads. Parameters are fixed
by the script but may be overridden. The runner also accepts `--distribution
hot`, where 90% of requests select the hottest 1% of keys, and configurable
reader/writer counts for focused concurrent runs.

Every CSV row contains the UTC time, result label, workload and settings,
compiler, build type, OS, configured commit SHA, logical CPU count, throughput,
latency percentiles, and segment-file bytes. Recovery and compaction have their
own duration columns. Portable process-memory measurement is deliberately not
reported: the C++ standard library offers no reliable cross-platform resident
set metric, and mixing incomparable platform counters would weaken the suite.

Each invocation requires a new database path. When none is supplied, the runner
uses and removes a unique temporary directory. `--database` refuses to reuse an
existing path; `--keep-database` retains the new path for inspection. This
prevents a benchmark command from accidentally deleting a real database.

## CPU profiling with GNU gprof

GCC users can build profiling instrumentation and run a focused workload:

```powershell
./benchmarks/profile_gprof.ps1 -BuildDirectory build-profile `
  -Output benchmarks/results/gprof.txt
```

The script profiles a sequential PUT workload with 4 KiB values. Its parameters
and command are written above the `gprof` report so a later build can repeat the
same experiment. Wall-clock benchmark results remain the source for user-visible
latency and throughput; instrumentation profiles identify where CPU time is
spent.
