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
  -> log file / OS page cache
  -> optional OS durable sync request
  -> in-memory key-to-record-location update

Client DELETE for an existing key
  -> Record encoder creates a tombstone
  -> StorageLog append + stream flush
  -> log file / OS page cache
  -> optional OS durable sync request
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
  -> incomplete EOF record truncates back to the last valid offset

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
nonexistent database files recover as empty databases. If EOF arrives partway
through the next record, recovery keeps every checksum-verified record and
truncates the incomplete tail. A complete checksum mismatch or invalid record
format remains fatal and is never truncated.

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

If PUT or DELETE encoding, append, stream flush, or durable sync fails, the
exception reaches the caller and the in-memory index is not changed. Because a
failed system call can have an ambiguous outcome, that `MiniKV` instance rejects
later appends. Restart validates the file: a partial final append is truncated,
while a complete valid record may be replayed even if its caller observed a sync
error.

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
against deliberate modification.

## Crash recovery policy

Recovery trusts only complete records whose format and CRC-32 validate. When a
sequential read begins at a verified record boundary but EOF arrives before the
next header, payload, or checksum is complete, MiniKV classifies all remaining
bytes as a torn final append. It resizes the file to that verified boundary and
continues with the recovered index. In Sync mode, it also syncs the truncation.

This rule is intentionally limited to EOF truncation. Invalid magic, version,
operation, bounds, tombstone shape, or a checksum mismatch in a complete record
aborts opening and leaves the file unchanged. MiniKV does not search for a later
magic sequence, because guessing a new boundary could silently discard or
misinterpret established data. A bounded length field corrupted so that it
extends beyond EOF is indistinguishable from a torn append and therefore follows
the documented tail-truncation policy.

## Durability modes

`DurabilityMode::Buffered` is the default. Each append is written and the C++
stream is flushed before the index changes, but the bytes may exist only in the
operating system's page cache. A process crash does not erase the OS page cache,
but an OS crash or power loss can lose those writes.

`DurabilityMode::Sync` performs the same stream flush and then calls the one
platform boundary in `platform_sync.cpp`: `FlushFileBuffers` on Windows and
`fsync` on POSIX. The index changes and the operation returns only after that
call succeeds. Under the operating system and storage device's documented
contract, this provides a power-loss durability request for the log file's data
and length. It does not sync the parent directory entry for a newly created log,
and hardware or virtualized storage can still violate flush guarantees.

Sync mode is slower because it reduces batching and waits for lower storage
layers. Neither mode provides transactions across multiple records, isolation,
atomic multi-key changes, authentication, or a general ACID guarantee.

## Boundaries

- `include/minikv`: public library interface
- `src/record.*`: private binary record model and codec
- `src/storage_log.*`: private append-only disk I/O
- `src/platform_sync.*`: one Windows/POSIX durable-flush boundary
- `src/minikv.cpp`: logical operation ordering and in-memory state
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
