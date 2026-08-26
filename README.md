# MiniKV

MiniKV is a small persistent key-value storage engine written in modern C++.
Its purpose is to make the mechanics below a database API understandable:
persistence, indexing, recovery, concurrency, compaction, and performance.

The project writes `PUT` and `DELETE` operations to a checksummed, versioned
binary append-only log. It keeps an in-memory hash index from each live key to
its newest PUT record location. Opening a database validates and replays PUTs
and DELETE tombstones. A clearly incomplete final append is removed at its last
verified boundary, while complete corruption remains a fatal error.

## Design direction

```text
Client
  -> MiniKV API
  -> in-memory key-to-record-location index
  -> append-only storage log
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
    minikv::DurabilityMode::Sync);
store.put("course", "storage systems");

if (const auto value = store.get("course")) {
    // *value contains "storage systems"
}

const bool removed = store.erase("course");
```

`put` inserts or overwrites, `get` returns `std::nullopt` for a missing key, and
`erase` reports whether it removed an existing key. It appends a tombstone before
removing that key from the index; erasing a missing key returns `false` without
writing. PUTs and tombstones reach the log before memory changes. Empty strings
and embedded NUL bytes are valid in keys and PUT values, within the documented
size limits.

See [the version 2 file format](docs/file-format.md) for the byte layout, CRC-32,
and validation rules. `DurabilityMode::Buffered` (the default) flushes C++ stream
buffers into the operating system's caching path. `DurabilityMode::Sync` also
calls `FlushFileBuffers` on Windows or `fsync` on POSIX before changing the
in-memory index. Sync mode is stronger and slower, but it is not an ACID claim or
an absolute guarantee against hardware that does not honor flush requests.

## Build and run

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a multi-configuration generator such as Visual Studio, add
`--config Debug` to the build and test commands.

Run the linked append-only demo from the generator-specific output directory:

```powershell
./build/minikv_demo.exe demo.minikv
```

See [the architecture](docs/architecture.md), [the learning notes](docs/learning-notes.md),
and [the roadmap](docs/roadmap.md) for the current boundaries and planned sequence.
