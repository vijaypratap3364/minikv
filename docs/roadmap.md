# Roadmap

The order is intentionally causal: each milestone exposes the problem addressed
by the next one.

## Bootstrap — build and test foundation (complete)

- CMake-based C++20 library
- linked demo executable
- dependency-free CTest smoke test
- warnings, documentation, and repository rules

## Stage 1 — API semantics and volatile index (complete)

- Define `PUT`, `GET`, and `DELETE` behavior through a small public API.
- Implement the behavior with an in-memory standard-library map.
- Test insertion, lookup, overwrite, deletion, missing keys, empty values, and
  multiple independent engine instances.
- Allow empty and binary-safe keys and values with explicit missing-key behavior.
- Keep this stage deliberately nonpersistent so process restart demonstrates the
  exact data-loss problem the append-only log must solve.

## Stage 2 — append-only persistence

- Introduce a documented binary record format.
- Append mutations to one storage log.
- Rebuild the in-memory index by replaying records at startup.
- Represent deletion with tombstones.

## Stage 3 — corruption boundaries and durability

- Add checksums and record validation.
- Detect and ignore an incomplete tail record after a torn write.
- Define flush/sync behavior and make durability guarantees explicit.

## Stage 4 — multithreaded access

- Protect engine state with straightforward standard-library synchronization.
- Test parallel readers and writers before considering finer-grained locking.

## Stage 5 — segments and compaction

- Rotate bounded segment files.
- Rewrite only live records during compaction.
- Make replacement crash-safe and recovery-aware.

## Stage 6 — measurement and automation

- Add representative read/write benchmarks.
- Profile before optimizing.
- Add CI builds and sanitizers, keeping heavy combinations off the constrained
  local laptop when appropriate.
