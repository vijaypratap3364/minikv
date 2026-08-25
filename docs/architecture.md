# Architecture

## Current implementation

MiniKV currently owns an in-memory hash table and exposes the first useful
storage semantics through a small C++ API.

```text
Client
  -> minikv::MiniKV
  -> std::unordered_map<std::string, std::string>
  -> process memory
```

Each `MiniKV` object owns its map, so separate instances do not share state. The
map owns copies of keys and values. `std::string` is used as a length-aware byte
container, which permits empty data and embedded NUL bytes.

## API semantics

| Storage operation | C++ method | Behavior |
| --- | --- | --- |
| PUT | `put(key, value)` | Inserts a new key or overwrites its current value. |
| GET | `get(key)` | Returns a copied value in `std::optional`, or `std::nullopt` when missing. |
| DELETE | `erase(key)` | Removes a key and reports whether it existed. |
| CONTAINS | `contains(key)` | Reports whether a key currently exists. |

Empty keys and values are valid. A present empty value is distinguishable from a
missing key because only the latter returns `std::nullopt`. Keys and values may
contain arbitrary bytes, including NUL. This stage provides no synchronization;
concurrent access to the same instance is not supported.

The hash table gives expected average constant-time lookup, insertion, and
deletion. A pathological collision pattern can degrade an operation to linear
time. Hashing still examines the key bytes, and `put`/`get` copy or move owned
data, so byte lengths also affect real cost.

## Intended storage architecture

The engine will grow toward this data flow:

```text
Client
  -> MiniKV API (PUT, GET, DELETE)
  -> in-memory index (key to latest record location)
  -> append-only storage log (binary records and tombstones)
  -> disk
```

The API now defines observable behavior, and the in-memory map makes reads fast.
There is still no record format, file I/O, or durability claim. Stage 2 will add
an append-only log as the durable source of truth and rebuild the index from that
log after restart.

## Boundaries

- `include/minikv`: public library interface
- `src`: private engine implementation
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
