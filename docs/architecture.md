# Architecture

## Current implementation

MiniKV records PUTs in a versioned append-only file and keeps an in-memory hash
index from each live key to its newest record's offset and encoded size. Values
remain in the log and are read from disk on demand. DELETE remains an in-memory
operation through Stage 3.

```text
Client PUT
  -> minikv::MiniKV logical operation
  -> Record encoder
  -> StorageLog append + stream flush
  -> disk file
  -> in-memory key-to-record-location update

Client GET
  -> in-memory index lookup
  -> StorageLog read at indexed offset
  -> Record decoder and key validation
  -> copied value

Startup
  -> scan records from offset zero
  -> validate and decode each complete record
  -> assign its location to that key in the index

Client CONTAINS/DELETE
  -> in-memory index
```

The three responsibilities are deliberately separate. `MiniKV` defines logical
behavior, the record codec defines bytes, and `StorageLog` owns file I/O and the
next append offset. See [the file-format specification](file-format.md) for the
stable version 1 layout.

Each `MiniKV` is constructed with a log path. Opening creates the file if needed,
discovers its current size, and scans records sequentially from offset zero.
Assigning each decoded key's location replaces any earlier location, so the
newest record for that key wins. Empty and nonexistent database files recover as
empty databases. Malformed, unsupported, or truncated content raises an error;
Stage 3 does not silently ignore or repair corruption.

## API semantics

| Storage operation | C++ method | Behavior |
| --- | --- | --- |
| PUT | `put(key, value)` | Appends a PUT, then inserts or overwrites its index location. |
| GET | `get(key)` | Uses the index to read the newest record and returns a copied value, or `std::nullopt` when missing. |
| DELETE | `erase(key)` | Removes an in-memory index entry and reports whether it existed. It does not append through Stage 3. |
| CONTAINS | `contains(key)` | Reports whether a key currently exists. |

Empty keys and values are valid. A present empty value is distinguishable from a
missing key because only the latter returns `std::nullopt`. Keys and values may
contain arbitrary bytes, including NUL. This stage provides no synchronization;
concurrent access to the same instance is not supported.

If PUT record encoding or append/flush fails, the exception reaches the caller
and the in-memory index is not changed. A partial failed write may still leave a
torn tail on disk; the next startup reports that truncation as corruption rather
than repairing it.

The hash table gives expected average constant-time index lookup, insertion, and
deletion. A pathological collision pattern can degrade an operation to linear
time. `GET` additionally performs a seek and reads and decodes one record, while
startup recovery is linear in the file's records and bytes. Hashing still
examines the key bytes, so key and value lengths affect real cost.

## Intended storage architecture

The current data flow is:

```text
Client
  -> MiniKV API (PUT, GET, DELETE)
  -> in-memory index (key to latest record location)
  -> append-only storage log (PUT records now, tombstones in Stage 4)
  -> disk
```

The API defines observable behavior, the index avoids scanning the whole log for
each read, and the log is the source used to rebuild locations after restart.
The append-only design deliberately leaves old versions on disk; they are no
longer indexed when superseded, but they make the file grow until a later
compaction stage rewrites only live data. DELETE is not persistent yet, so an
older PUT for an erased key reappears after restart. Stream flush is also not a
power-loss durability guarantee; explicit OS sync policy comes later.

## Boundaries

- `include/minikv`: public library interface
- `src/record.*`: private binary record model and codec
- `src/storage_log.*`: private append-only disk I/O
- `src/minikv.cpp`: logical operation ordering and in-memory state
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
