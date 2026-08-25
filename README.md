# MiniKV

MiniKV is a small persistent key-value storage engine written in modern C++.
Its purpose is to make the mechanics below a database API understandable:
persistence, indexing, recovery, concurrency, compaction, and performance.

The project currently implements `PUT`, `GET`, `DELETE`, and `CONTAINS` semantics
with an in-memory hash table. It intentionally has no disk storage: every value
disappears when the process exits.

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

## In-memory API

```cpp
#include "minikv/minikv.hpp"

minikv::MiniKV store;
store.put("course", "storage systems");

if (const auto value = store.get("course")) {
    // *value contains "storage systems"
}

const bool removed = store.erase("course");
```

`put` inserts or overwrites, `get` returns `std::nullopt` for a missing key, and
`erase` reports whether it removed an existing key. Empty strings and embedded
NUL bytes are valid in both keys and values.

## Build and run

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a multi-configuration generator such as Visual Studio, add
`--config Debug` to the build and test commands.

Run the linked in-memory demo from the generator-specific output directory:

```powershell
./build/minikv_demo.exe
```

See [the architecture](docs/architecture.md), [the learning notes](docs/learning-notes.md),
and [the roadmap](docs/roadmap.md) for the current boundaries and planned sequence.
