# MiniKV

MiniKV is a small persistent key-value storage engine written in modern C++.
Its purpose is to make the mechanics below a database API understandable:
persistence, indexing, recovery, concurrency, compaction, and performance.

The project currently writes `PUT` operations to a versioned binary append-only
log, then updates an in-memory hash table. `DELETE` remains in-memory only until
Stage 4. The log survives normal process exit, but restart recovery is
intentionally not implemented yet: a newly opened `MiniKV` does not replay
existing records.

## Design direction

```text
Client
  -> MiniKV API
  -> in-memory index
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

minikv::MiniKV store("example.minikv");
store.put("course", "storage systems");

if (const auto value = store.get("course")) {
    // *value contains "storage systems"
}

const bool removed = store.erase("course");
```

`put` inserts or overwrites, `get` returns `std::nullopt` for a missing key, and
`erase` reports whether it removed an existing key, but does not write to disk in
Stage 2. PUT records are appended before memory changes. Empty strings and
embedded NUL bytes are valid in both keys and values, within the documented size
limits.

See [the version 1 file format](docs/file-format.md) for the byte layout and
validation rules. This stage flushes the C++ file stream after each record but
does not yet promise power-loss durability or recover state after restart.

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
