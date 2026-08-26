# Architecture

## Current implementation

MiniKV records PUTs and DELETE tombstones in a checksummed, versioned append-only
file. Its in-memory hash index maps each live key to its newest PUT record's
offset and encoded size. Values remain in the log and are read from disk on
demand.

```text
Client PUT
  -> minikv::MiniKV logical operation
  -> Record encoder
  -> StorageLog append + stream flush
  -> disk file
  -> in-memory key-to-record-location update

Client DELETE for an existing key
  -> Record encoder creates a tombstone
  -> StorageLog append + stream flush
  -> disk file
  -> remove key from the in-memory index

Client GET
  -> in-memory index lookup
  -> StorageLog read at indexed offset
  -> Record decoder and key validation
  -> copied value

Startup
  -> scan records from offset zero
  -> validate format, length, and CRC-32 of each record
  -> PUT assigns its location to the key
  -> DELETE removes the key

Client CONTAINS
  -> in-memory index
```

The three responsibilities are deliberately separate. `MiniKV` defines logical
behavior, the record codec defines bytes, and `StorageLog` owns file I/O and the
next append offset. See [the file-format specification](file-format.md) for the
stable version 2 layout.

Each `MiniKV` is constructed with a log path. Opening creates the file if needed,
discovers its current size, and scans records sequentially from offset zero.
Applying records in order makes the newest operation for a key win. A later PUT
replaces an earlier location, while a later DELETE removes it. Empty and
nonexistent database files recover as empty databases. Invalid format, checksum
mismatch, and incomplete content raise distinct record errors; Stage 4 does not
silently ignore or repair corruption.

## API semantics

| Storage operation | C++ method | Behavior |
| --- | --- | --- |
| PUT | `put(key, value)` | Appends a PUT, then inserts or overwrites its index location. |
| GET | `get(key)` | Uses the index to read the newest record and returns a copied value, or `std::nullopt` when missing. |
| DELETE | `erase(key)` | For an existing key, appends a tombstone and then removes the index entry. A missing key returns `false` without writing. |
| CONTAINS | `contains(key)` | Reports whether a key currently exists. |

Empty keys and values are valid. A present empty value is distinguishable from a
missing key because only the latter returns `std::nullopt`. Keys and values may
contain arbitrary bytes, including NUL. This stage provides no synchronization;
concurrent access to the same instance is not supported.

If PUT or DELETE encoding or append/flush fails, the exception reaches the
caller and the in-memory index is not changed. A partial failed write may still
leave a torn tail on disk; the next startup reports an incomplete record rather
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
  -> append-only storage log (PUT records and DELETE tombstones)
  -> disk
```

The API defines observable behavior, the index avoids scanning the whole log for
each read, and the log is the source used to rebuild locations after restart.
The append-only design deliberately leaves old versions on disk; they are no
longer indexed when superseded, but they make the file grow until a later
compaction stage rewrites only live data. Tombstones also remain because the log
is never rewritten in place. Every version 2 record carries a CRC-32 that detects
many accidental byte changes, but it neither repairs data nor authenticates it
against deliberate modification. Stream flush is also not a power-loss
durability guarantee; explicit OS sync policy comes later.

## Boundaries

- `include/minikv`: public library interface
- `src/record.*`: private binary record model and codec
- `src/storage_log.*`: private append-only disk I/O
- `src/minikv.cpp`: logical operation ordering and in-memory state
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
