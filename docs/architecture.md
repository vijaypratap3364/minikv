# Architecture

## Current implementation

MiniKV now records PUTs in a versioned append-only file and keeps live values in
an in-memory hash table. DELETE remains an in-memory operation during Stage 2.

```text
Client PUT
  -> minikv::MiniKV logical operation
  -> Record encoder
  -> StorageLog append + stream flush
  -> disk file
  -> in-memory std::unordered_map update

Client GET/CONTAINS/DELETE
  -> in-memory std::unordered_map
```

The three responsibilities are deliberately separate. `MiniKV` defines logical
behavior, the record codec defines bytes, and `StorageLog` owns file I/O and the
next append offset. See [the file-format specification](file-format.md) for the
stable version 1 layout.

Each `MiniKV` is constructed with a log path. Opening creates the file if needed
and discovers its current size so new records append after existing bytes. It
does not read those bytes or reconstruct the map yet.

## API semantics

| Storage operation | C++ method | Behavior |
| --- | --- | --- |
| PUT | `put(key, value)` | Appends a PUT, then inserts or overwrites in memory. |
| GET | `get(key)` | Returns a copied value in `std::optional`, or `std::nullopt` when missing. |
| DELETE | `erase(key)` | Removes an in-memory key and reports whether it existed. It does not append during Stage 2. |
| CONTAINS | `contains(key)` | Reports whether a key currently exists. |

Empty keys and values are valid. A present empty value is distinguishable from a
missing key because only the latter returns `std::nullopt`. Keys and values may
contain arbitrary bytes, including NUL. This stage provides no synchronization;
concurrent access to the same instance is not supported.

If PUT record encoding or append/flush fails, the exception reaches the caller
and the in-memory map is not changed. A partial failed write may still leave a
torn tail on disk; detecting and handling that condition is a later stage.

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
  -> append-only storage log (PUT records now, tombstones in Stage 4)
  -> disk
```

The API defines observable behavior, the map makes reads fast, and the log keeps
PUT bytes across normal process exit. DELETE is not persistent yet. The log is
not yet a usable source of truth after restart because startup replay is absent.
Stream flush is also not a power-loss durability guarantee; explicit OS sync
policy comes later.

## Boundaries

- `include/minikv`: public library interface
- `src/record.*`: private binary record model and codec
- `src/storage_log.*`: private append-only disk I/O
- `src/minikv.cpp`: logical operation ordering and in-memory state
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
