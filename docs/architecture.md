# Architecture

## Current implementation

The bootstrap milestone contains one small C++20 library target. Its only public
function reports version metadata. Both the demo executable and the smoke-test
executable link against that library, proving the intended target boundary works.

```text
minikv_demo ----\
                 -> MiniKV::minikv -> version metadata
minikv_tests ---/
```

There is currently no key-value API, in-memory index, record format, file I/O, or
durability claim. That absence is intentional: this milestone establishes a
repeatable build and test baseline before storage behavior makes failures more
interesting.

## Intended storage architecture

The engine will grow toward this data flow:

```text
Client
  -> MiniKV API (PUT, GET, DELETE)
  -> in-memory index (key to latest record location)
  -> append-only storage log (binary records and tombstones)
  -> disk
```

The API will define observable behavior. The in-memory index will make reads
fast. The append-only log will become the durable source of truth. Recovery will
replay that log to reconstruct the index after restart. Later stages will add
defenses and lifecycle mechanisms only after tests demonstrate why they are
needed.

## Boundaries

- `include/minikv`: public library interface
- `src`: private engine implementation
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
