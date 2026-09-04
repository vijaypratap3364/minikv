# MiniKV

MiniKV is a small persistent key-value storage engine written in modern C++.
Its purpose is to make the mechanics below a database API understandable:
persistence, indexing, recovery, concurrency, compaction, and performance.

The project writes `PUT` and `DELETE` operations to checksummed, versioned binary
records in numbered append-only segment files. It keeps an in-memory hash index
from each live key to its newest PUT record's segment and byte location. Opening
a database validates and replays segments in order. A clearly incomplete append
in the active segment is removed at its last verified boundary, while corruption
in an immutable segment remains fatal.
Public operations on one live instance are safe to call from multiple threads.

## Design direction

```text
Client
  -> MiniKV API
  -> in-memory key-to-segment-location index
  -> immutable segments + one active segment
  -> disk
```

Each layer will be introduced only when it solves a concrete limitation of the
previous design. MiniKV is an educational engine, not a replacement for
PostgreSQL, RocksDB, Redis, or LevelDB.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC, Clang, or MSVC)
- CTest, which is included with CMake

No database, service, VM, WSL installation, or Docker installation is required.

## API

```cpp
#include "minikv/minikv.hpp"

minikv::MiniKV store(
    "example.minikv",
    minikv::MiniKVOptions{
        minikv::DurabilityMode::Sync,
        64U * 1024U * 1024U});
store.put("course", "storage systems");

if (const auto value = store.get("course")) {
    // *value contains "storage systems"
}

const bool removed = store.erase("course");
store.compact();
```

`put` inserts or overwrites, `get` returns `std::nullopt` for a missing key, and
`erase` reports whether it removed an existing key. It appends a tombstone before
removing that key from the index; erasing a missing key returns `false` without
writing. PUTs and tombstones reach the log before memory changes. Empty strings
and embedded NUL bytes are valid in keys and PUT values, within the documented
size limits.

The path passed to `MiniKV` is a database directory, not a single log file. Its
`CURRENT` manifest selects one generation containing numbered segment files.
The final segment is active and receives appends; rollover makes prior segments
immutable. `maximum_segment_size` is a rollover target: a single valid record is
allowed to exceed it rather than becoming impossible to store. The default is
64 MiB. Stage 6 single-file databases are not migrated automatically.

`compact()` is a blocking operation that holds the instance mutex. It rewrites
only current live PUTs into a temporary generation, validates them, atomically
installs that generation and switches `CURRENT`, then removes the old generation.
Deleted keys need no tombstone in the new complete snapshot. If the process
stops before the manifest switch, restart uses the old generation; afterward it
uses the new one. Unselected temporary or obsolete generations are cleaned on
startup. Compaction can briefly require space for both generations.

MiniKV currently uses one mutex per instance, so concurrent calls are safe but
serialize at the API boundary, including GETs. Do not open the same database through
multiple `MiniKV` instances or processes concurrently; coordination is only
within one instance. The caller must also ensure the instance is not moved or
destroyed while another thread is using it.

See [the version 2 file format](docs/file-format.md) for the byte layout, CRC-32,
and validation rules. `DurabilityMode::Buffered` (the default) flushes C++ stream
buffers into the operating system's caching path. `DurabilityMode::Sync` also
calls `FlushFileBuffers` on Windows or `fsync` on POSIX before changing the
in-memory index. Compaction also uses same-filesystem atomic rename primitives
for its generation and manifest switch. Sync mode is stronger and slower, but it
is not an ACID claim or an absolute guarantee against hardware that does not
honor flush requests.

## Build and run

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a multi-configuration generator such as Visual Studio, add
`--config Debug` to the build and test commands.

ThreadSanitizer is prepared for supported GCC/Clang Linux builds and runs in its
own CI workflow:

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIKV_ENABLE_TSAN=ON
cmake --build build-tsan --parallel 2
ctest --test-dir build-tsan --output-on-failure
```

The sanitizer option intentionally rejects Windows and unsupported compilers.
It is off by default, so normal local builds do not pay the instrumentation cost.

Build and run the dependency-free performance workloads in Release mode:

```powershell
cmake -S . -B build-bench `
  -DCMAKE_BUILD_TYPE=Release `
  -DMINIKV_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel 2
./benchmarks/run_suite.ps1 -BuildDirectory build-bench
```

The runner measures sequential PUT, random GET, update-heavy, mixed,
delete-heavy, recovery, compaction, and configurable concurrent workloads. See
[the benchmark guide](benchmarks/README.md) for methodology, result fields, and
GNU `gprof` instructions. Recorded Stage 8 measurements are kept under
`benchmarks/results`; they describe their recorded machine and settings, not a
general performance guarantee.

Run the linked append-only demo from the generator-specific output directory:

```powershell
./build/minikv_demo.exe demo.minikv
```

See [the architecture](docs/architecture.md), [the learning notes](docs/learning-notes.md),
and [the roadmap](docs/roadmap.md) for the current boundaries and planned sequence.
