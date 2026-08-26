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

## Stage 2 — binary records and append-only persistence (complete)

- Introduce a documented binary record format.
- Append PUT records to one storage log before changing memory.
- Keep DELETE in-memory only and return record byte offsets from PUT appends.
- Validate versions, operations, limits, malformed headers, and truncated input.
- Deliberately leave restart recovery to Stage 3.

## Stage 3 — persistent index and restart recovery (complete)

- Scan and decode records from the start of the log.
- Rebuild an in-memory index from keys to their newest record locations.
- Read values from their indexed records instead of retaining all values in RAM.
- Reject malformed or truncated input explicitly during startup.
- Keep old record versions on disk and defer compaction.

## Stage 4 — persistent deletion and record integrity (complete)

- Add DELETE tombstone records and replay them during recovery.
- Add CRC-32 checksums and verify them on every decoded record.
- Distinguish invalid format, checksum mismatch, and incomplete records.
- Reject an incomplete tail explicitly; automatic repair remains Stage 5 work.
- Keep the current stream-flush behavior explicit without claiming power-loss
  durability.

## Stage 5 — crash safety and durability (complete)

- Truncate a clearly incomplete final append to the last verified record.
- Keep invalid format and complete checksum corruption fatal.
- Test partial headers, keys, values, checksums, and complete records with
  deterministic byte-prefix fault injection.
- Offer buffered and sync durability modes through one Windows/POSIX boundary.
- State the exact guarantee without claiming ACID or infallible hardware.

## Stage 6 — multithreaded access (complete)

- Protect one engine instance with a coarse standard-library mutex.
- Keep append, optional sync, and index mutation in one write critical section.
- Test concurrent reads, writes, deletes, contention, and restart recovery.
- Document the choice to defer finer-grained shared locking until storage reads
  have an appropriate parallel-I/O design.

## Stage 7 — segments and compaction

- Rotate bounded segment files.
- Rewrite only live records during compaction.
- Make replacement crash-safe and recovery-aware.

## Stage 8 — measurement and automation

- Add representative read/write benchmarks.
- Profile before optimizing.
- Add CI builds and sanitizers, keeping heavy combinations off the constrained
  local laptop when appropriate.
