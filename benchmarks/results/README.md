# Stage 8 measurements

These are raw measurements from one machine, not promises about other hardware:

- OS: Microsoft Windows NT 10.0.22631.0
- CPU: AMD Ryzen 5 8640HS with Radeon 760M Graphics
- logical CPUs visible to the process: 12
- compiler: MSYS2 GCC 15.2.0
- build: CMake Release for wall-clock results; RelWithDebInfo plus `-pg` for profiling
- durability: Buffered
- deterministic seed: 84954583903062

`stage8-before-after.csv` contains one pass of all workloads with 2,000 keys,
256-byte values, 4,000 timed operations, and a 262,144-byte segment target. It
also contains read-only concurrent runs with 1, 2, 4, and 8 threads and a mixed
3-reader/1-writer run. The exact invocation is the `run_suite.ps1` command in
the benchmark guide with those defaults.

The suite demonstrates workload coverage and preserves outliers honestly. It is
not a statistically powered claim that every row improved: Windows background
I/O caused noticeable run-to-run wall-clock variance. For example, the after
suite's 256-byte sequential PUT median was 9.1 microseconds while its total
elapsed time was unexpectedly 294.934 milliseconds. A small number of stalls
beyond p99 can therefore dominate throughput without appearing in p50/p95/p99.
Long-term regression gates should add repetitions and controlled-machine runs.

`stage8-focused-before-after.csv` is the exact comparison used to accept the
optimization. Both rows use sequential PUT, 4,000 keys and operations, 4,096-byte
values, a 67,108,864-byte segment target, and the same seed:

| Metric | Before (`054edd9`) | After (`a8bb9ab`) |
| --- | ---: | ---: |
| Operations/second | 17,992.4 | 31,519.7 |
| Elapsed time | 222.316 ms | 126.905 ms |
| p50 latency | 46.8 us | 23.0 us |
| p95 latency | 88.6 us | 64.2 us |
| p99 latency | 109.1 us | 79.6 us |
| Segment bytes | 16,528,000 | 16,528,000 |

That run improved throughput by 75.2% and reduced elapsed time by 42.9%. The
unchanged byte count is expected because the optimization does not alter the
record format.

`gprof-before.txt` and `gprof-after.txt` use the same focused workload. Before
the change, all 10 sampled CPU ticks were on the two source lines inside the
bit-at-a-time CRC loop. After the change, CRC work accounted for two sampled
ticks. A gprof tick is 10 milliseconds on this machine, so the small after count
is coarse; the independent Release wall-clock run above is the acceptance
measurement.

The 1/2/4/8-thread rows also show why more threads are not assumed to help.
Before optimization, read-only throughput was 147,418, 112,638, 107,940, and
112,082 operations/second respectively. MiniKV deliberately serializes GETs on
its coarse instance mutex and shared seekable stream, so extra callers add
contention instead of parallel read I/O.
