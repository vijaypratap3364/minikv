# Architecture

## Current implementation

MiniKV records PUTs and DELETE tombstones in checksummed, versioned append-only
segment files. Its in-memory hash index maps each live key to its newest PUT
record's segment identifier, segment-relative offset, and encoded size. Values
remain in the segments and are read from disk on demand. One `MiniKV` instance
can be used safely by multiple threads.

```text
Client PUT
  -> minikv::MiniKV logical operation
  -> acquire the instance mutex
  -> Record encoder
  -> SegmentedStorage selects or rolls the active segment
  -> StorageLog append + stream flush
  -> active segment / OS page cache
  -> optional OS durable sync request
  -> in-memory key-to-record-location update

Client DELETE for an existing key
  -> acquire the instance mutex
  -> Record encoder creates a tombstone
  -> StorageLog append + stream flush
  -> log file / OS page cache
  -> optional OS durable sync request
  -> remove key from the in-memory index

Client GET
  -> acquire the instance mutex
  -> in-memory index lookup
  -> StorageLog read at indexed offset
  -> Record decoder and key validation
  -> copied value

Client COMPACT
  -> acquire the instance mutex
  -> read the newest indexed PUT for every live key
  -> write and validate a temporary generation
  -> atomically install the generation and replace CURRENT
  -> switch index locations, then remove the obsolete generation

Startup
  -> read CURRENT to select a generation
  -> scan numbered segments and records in order
  -> validate format, length, and CRC-32 of each record
  -> PUT assigns its location to the key
  -> DELETE removes the key
  -> incomplete EOF record in the active segment truncates to its last valid offset

Client CONTAINS
  -> in-memory index
```

The responsibilities remain separate. `MiniKV` defines logical behavior, the
record codec defines bytes, `StorageLog` owns I/O within one file, and
`SegmentedStorage` owns the manifest, segment ordering, rollover, and complete
record locations. See [the file-format specification](file-format.md) for the
stable version 2 record and segment-container layouts.

Each `MiniKV` is constructed with a database-directory path. `CURRENT` selects a
numbered generation directory. Recovery scans its contiguous segment identifiers
in ascending order and scans records from offset zero within each segment.
Applying records in that total order makes the newest operation for a key win.
The highest numbered segment is active; prior segments are immutable. EOF during
a record is repaired only in that active segment. The same condition in an older
segment is established corruption, as are checksum and format failures.

## API semantics

| Storage operation | C++ method | Behavior |
| --- | --- | --- |
| PUT | `put(key, value)` | Appends a PUT, then inserts or overwrites its index location. |
| GET | `get(key)` | Uses the index to read the newest record and returns a copied value, or `std::nullopt` when missing. |
| DELETE | `erase(key)` | For an existing key, appends a tombstone and then removes the index entry. A missing key returns `false` without writing. |
| CONTAINS | `contains(key)` | Reports whether a key currently exists. |
| COMPACT | `compact()` | Rewrites only live PUTs into a safely installed replacement generation. |

Empty keys and values are valid. A present empty value is distinguishable from a
missing key because only the latter returns `std::nullopt`. Keys and values may
contain arbitrary bytes, including NUL. Calls through one live `MiniKV` instance
are synchronized. Independently opening the same database through multiple instances
or processes is not supported because the per-instance mutex cannot coordinate
their streams, offsets, or indexes.

If PUT or DELETE encoding, append, stream flush, or durable sync fails, the
exception reaches the caller and the in-memory index is not changed. Because a
failed system call can have an ambiguous outcome, that `MiniKV` instance rejects
later appends. Restart validates every segment: a partial final append is truncated,
while a complete valid record may be replayed even if its caller observed a sync
error.

The hash table gives expected average constant-time index lookup, insertion, and
deletion. A pathological collision pattern can degrade an operation to linear
time. `GET` additionally performs a seek and reads and decodes one record, while
startup recovery is linear in all current-generation records and bytes. Hashing still
examines the key bytes, so key and value lengths affect real cost.

## Concurrency model

The shared mutable state and its unsynchronized failure modes are:

| State | What can race without a lock |
| --- | --- |
| Hash index | Concurrent lookup, insertion, rehash, and erase would access `std::unordered_map` unsafely and can corrupt its internal structure. |
| Active segment, append stream, and file bytes | Two writers could race on rollover or stream state and produce records in an order neither caller can relate to its index update. |
| Segment list and next append offset | Writers could select conflicting active segments, calculate incorrect record locations, or lose offset updates. |
| Read stream | Concurrent seeks and reads would overwrite the stream's shared file position and status flags. |
| Append-failure metadata | One thread could miss another thread's failure and continue using an instance whose append outcome is ambiguous. |

For example, without synchronization, concurrent `PUT A=1` and `PUT B=2`
could both observe the same end offset, interfere with the append stream, and
race while changing the hash table. The resulting index might point at the
wrong record even if the file bytes happened to remain parseable.

MiniKV uses one `std::mutex` per instance. Every public operation that observes
or changes engine state acquires it. The critical section for PUT and DELETE
includes encoding, append, optional durable sync, and the index change. This
preserves disk-first ordering: no reader can observe a new index entry before
its record append succeeds. GET holds the same lock across index lookup, file
seek, record read, checksum validation, and value copy, so another operation
cannot change the stream position or indexed state partway through the read.
Recovery runs during construction before the instance can be shared.
Compaction holds the same mutex for its entire scan, rewrite, manifest switch,
index replacement, and cleanup, so concurrent operations cannot invalidate its
live-record snapshot.

This is deliberately coarse per-instance lock granularity. A mutex provides
exclusive ownership, so even unrelated keys and concurrent GETs are serialized.
A `std::shared_mutex` could instead grant shared/read locks to multiple GETs and
exclusive/write locks to mutations. It is not used yet because the current
storage layer has one mutable read stream; truly parallel reads first need an
appropriate positional-I/O or independent-handle design. The single mutex is a
smaller correctness boundary, at the cost of less concurrency.

A deadlock occurs when threads wait forever for locks held in a cycle. Ordinary
MiniKV calls acquire exactly one instance mutex once and never upgrade or nest
that lock, so they cannot form such a cycle. Move assignment is the sole
two-instance operation and uses `std::scoped_lock`, which acquires both mutexes
with deadlock avoidance. Moving, destroying, or otherwise ending the lifetime of
an instance while other threads use it still requires external coordination.

## Intended storage architecture

The current data flow is:

```text
Client
  -> MiniKV API (PUT, GET, DELETE)
  -> in-memory index (key to segment + latest record location)
  -> manifest-selected generation
  -> immutable segments + one append-only active segment
  -> disk
```

The API defines observable behavior, the index avoids scanning the whole log for
each read, and the ordered segments are the source used to rebuild locations
after restart. A configurable rollover target bounds ordinary segment growth.
If a record would exceed the target in a nonempty segment, MiniKV creates the
next numbered active segment and never appends to the previous one again. One
record may exceed the target when it occupies a segment alone.

Segmentation makes files manageable, while compaction reclaims stale record
bytes. MiniKV reads each indexed live PUT, sorts by binary key for deterministic
output, and writes only those records to a new generation. Deleted keys are
absent from the index, so their old PUTs and tombstones are both omitted. Every
version 2 record carries a CRC-32 that detects many accidental byte changes, but
it neither repairs data nor authenticates it.

## Compaction safety

Compaction treats `CURRENT` as the commit point for a whole generation:

```text
write generation-N.tmp
  -> close/sync segment files
  -> atomically rename to generation-N
  -> reopen and checksum-validate every compacted record
  -> write/sync CURRENT.tmp
  -> atomically replace CURRENT
  -> switch in-memory segments and index locations
  -> close and remove the old generation
```

Old segments are never deleted while temporary output is incomplete or before
`CURRENT` selects the new complete generation. A crash before the manifest
replacement leaves the old generation authoritative. A crash after replacement
leaves the new generation authoritative and the old directory as harmless extra
space. Startup first validates the selected generation and then removes
recognized temporary and unselected generation directories.

The directory rename and manifest replacement stay on the same filesystem.
POSIX uses `rename` plus directory `fsync` in Sync mode. Windows uses
`MoveFileExW`, adding `MOVEFILE_WRITE_THROUGH` in Sync mode. These are the
strongest practical boundaries used here, not a claim that arbitrary filesystems
or hardware make multi-file updates intrinsically atomic.

If deletion of the old generation fails after the committed switch, MiniKV keeps
the new view and treats cleanup as best effort. Turning that into an operation
failure would leave the caller with an error while its old index locations no
longer describe the selected generation. Restart retries obsolete cleanup.

If atomic manifest replacement reports a failure, its outcome can be ambiguous:
the new `CURRENT` bytes may already be visible even if a following durable
directory sync failed. MiniKV propagates that error, leaves its old index intact,
and rejects further appends or compaction on that instance. Reads of the old live
snapshot remain available. Restart is the boundary that reads `CURRENT`,
validates the selected generation, and permits writes again.

## Crash recovery policy

Recovery trusts only complete records whose format and CRC-32 validate. When a
sequential read in the active segment begins at a verified record boundary but
EOF arrives before the next header, payload, or checksum is complete, MiniKV
classifies all remaining bytes as a torn final append. It resizes that segment
to the verified boundary and continues. In Sync mode, it also syncs truncation.

This rule is intentionally limited to the final segment's EOF. An incomplete
record in an immutable segment, invalid magic, version, operation, bounds,
tombstone shape, or a checksum mismatch in a complete record aborts opening and
leaves the segment unchanged. MiniKV does not search for a later
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
contract, this provides a power-loss durability request for segment data and
length. `CURRENT` installation uses an atomic same-filesystem replace, with
`MOVEFILE_WRITE_THROUGH` on Windows or a parent-directory `fsync` after `rename`
on POSIX. Hardware or virtualized storage can still violate flush guarantees.

Sync mode is slower because it reduces batching and waits for lower storage
layers. Neither mode provides transactions across multiple records, isolation,
atomic multi-key changes, authentication, or a general ACID guarantee.

## Boundaries

- `include/minikv`: public library interface
- `src/record.*`: private binary record model and codec
- `src/storage_log.*`: private I/O for one segment file
- `src/segmented_storage.*`: manifest, segment ordering, rollover, and compaction
- `src/platform_sync.*`: Windows/POSIX durable-flush and atomic-rename boundary
- `src/minikv.cpp`: logical operation ordering and in-memory state
- `tools`: small programs that use the public interface
- `tests`: deterministic local checks
- `benchmarks`: deferred performance workloads
- `docs`: design decisions and the learning narrative
