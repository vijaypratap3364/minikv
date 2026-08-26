# Learning Notes

## Bootstrap: a buildable boundary

### 1. What problem existed?

There was no project yet, so any storage code would have arrived without a
repeatable way to compile it, link clients to it, or detect regressions.

### 2. What did we build?

We built a CMake project with a C++20 library, a tiny linked demo, and a
dependency-free smoke test. The public surface contains only version metadata;
it deliberately makes no storage promises.

### 3. How does it solve the problem?

The library target creates a stable boundary for future engine code. The demo
proves an ordinary client can link to that boundary, while CTest provides one
command that can later run every correctness test.

### 4. What happens internally?

CMake compiles `src/version.cpp` into the MiniKV library. It then compiles the
demo and test separately and links each against the library through its public
header path. The test process returns a nonzero status if the linked function
does not produce the expected metadata, and CTest reports that status.

### 5. What can still go wrong?

Nearly everything related to storage is still absent. MiniKV cannot store a key,
retrieve a value, delete data, survive a restart, or make any durability claim.
The smoke test proves target wiring, not engine correctness.

### 6. What new problem does this design introduce?

The next step needs a precise API and behavioral rules. Before persistence can
be discussed, `PUT`, `GET`, and `DELETE` need deterministic in-memory semantics,
including what a missing key means and whether overwriting a key is allowed.

## In-memory semantics: useful behavior without persistence

### 1. What problem existed?

The bootstrap proved that MiniKV could build and link, but it could not yet
define what `PUT`, `GET`, and `DELETE` mean. Starting in memory isolates those
user-visible rules from file formats, operating-system buffering, and recovery.
When a test fails, the likely cause is API behavior rather than disk machinery.

### 2. What did we build?

We built a `MiniKV` object that owns an in-memory hash index. The index is a
`std::unordered_map` from an owned `std::string` key to an owned `std::string`
value. `PUT` inserts or overwrites, `GET` returns an optional copied value, and
`DELETE` reports whether it removed an existing key.

### 3. How does it solve the problem?

The optional result distinguishes a missing key from a present empty value.
Allowing empty strings keeps the rules simple. Because `std::string` tracks its
length, keys and values can also contain NUL bytes; the engine never relies on a
C-style terminator to find their end.

### 4. What happens internally?

For `PUT`, the hash table hashes the key, finds its bucket, and either creates an
entry or replaces its value. `GET`, `CONTAINS`, and `DELETE` hash the same key to
find its bucket. Hash-table operations are expected to take constant time on
average, although hashing reads the key bytes and returned values are copied.
Heavy collision patterns can make a worst-case operation linear in the number of
stored keys.

### 5. What can still go wrong?

The map exists only in the process's RAM. Normal exit, a crash, or power loss
destroys every key and value. A single `MiniKV` instance is also not safe for
concurrent unsynchronized access yet, and memory usage grows with the live data.

### 6. What new problem does this design introduce?

Stage 2 must first preserve PUT bytes without losing the fast in-memory lookup
path. Restart replay is the next problem. Once replay exists, deletion will need
its own persistent representation; Stage 4 will add tombstones. Stronger
torn-write and durability guarantees also remain later problems.

## Append-only persistence: turning operations into bytes

### 1. What problem existed?

The hash table held useful state but only in RAM. When its process ended, there
was no lasting description of which puts had occurred. We needed to turn PUT
operations into bytes that could remain in a file.

### 2. What did we build?

We built a versioned binary record codec and an append-only storage log. This
conversion from logical fields such as operation, key, and value into a defined
byte sequence is called serialization. During Stage 2, the only valid encoded
operation is PUT. DELETE still changes only the current in-memory map.

We cannot safely dump a C++ `Record` object directly to disk. Its strings contain
pointers to separately allocated memory, object padding is compiler-dependent,
integer byte order varies by format choice, and the in-memory layout is not a
stable contract between builds. Explicit serialization writes only defined
fields with fixed widths, byte order, limits, and version rules.

### 3. How does it solve the problem?

Every successful PUT is appended to the end of the log before RAM changes.
Append-only writing is simple because old bytes never move: the next record goes
at the current end. Its offset is the zero-based byte position where that record
starts, so later code can locate it without scanning unrelated bytes.

Overwriting records in place would be harder. A replacement value can have a
different length, which might require shifting everything after it. A crash
during an in-place update can also destroy the old value before the new one is
complete. Appending preserves the earlier record and leaves a chronological
history.

### 4. What happens internally?

The encoder validates the operation and size limits, writes a 16-byte header in
the documented format, and then copies the exact key and value bytes. The
storage log writes that encoded record in binary append mode and flushes the C++
stream. Only after append succeeds does `MiniKV` insert or overwrite the
in-memory entry. DELETE bypasses the log and erases only the current map. The
decoder reverses the format explicitly and reports how many bytes it consumed so
another record can follow immediately.

### 5. What can still go wrong?

A stream flush is not the same as forcing the operating system or device to
persist bytes across power loss. A failed append may leave an incomplete tail,
and version 1 has no checksum to detect arbitrary corruption. Concurrent access
is still unsupported. Memory allocation could also fail after a successful log
append but before the in-memory update finishes.

Most importantly, MiniKV does not read the file at startup. PUT records survive
a normal exit, but a new instance starts with an empty hash table. Merely writing
records is insufficient for recovery because something must parse and apply PUTs
in order and decide what to do with invalid input. DELETE is not persistent at
all yet.

### 6. What new problem does this design introduce?

Stage 3 must replay PUT records to reconstruct values after a restart. Once that
works, an in-memory DELETE would otherwise be forgotten and an older PUT could
reappear. Stage 4 will solve that new problem by recording deletion as a
tombstone. Checksums, torn-write policy, and explicit power-loss durability also
remain follow-on problems.

## Persistent index: rebuilding locations after restart

### 1. What problem existed?

Stage 2 left valid PUT records on disk, but a newly constructed `MiniKV` did not
read them. Its fresh in-memory map was empty, so persisted bytes could not be
found through the API after closing and reopening the database.

### 2. What did we build?

We changed the hash index from key-to-value into key-to-record-location. A
location contains the record's zero-based file offset and encoded byte size.
Startup now scans the entire log in order, validates every record, and rebuilds
that index. `GET` looks up the location and reads the corresponding record from
disk. `PUT` still appends to disk first and changes the index only after the
append succeeds.

### 3. How does it solve the problem?

The log is now the persistent source from which a new process reconstructs its
fast lookup structure. The index lives in memory because hash lookup is fast and
because its contents are derived from the log, so no separate index file needs
to be kept consistent yet. Keeping only keys and small locations in the index
also avoids permanently retaining every value in RAM.

### 4. What happens internally?

Recovery begins at offset zero. It reads and validates one 16-byte header,
computes the bounded record size, reads exactly that record's remaining bytes,
and advances by the decoded size. Each key is assigned the location of the
record just read. If the key appeared earlier, this later assignment replaces
the old location, so the newest PUT wins. `GET` uses the indexed offset to seek
to the record, validates it again, and returns its value.

The index makes finding a location expected constant time on average, followed
by one file seek and one record read. Rebuilding it is linear in the number and
total encoded size of records.

### 5. What can still go wrong?

Stage 3 treats malformed, unsupported, or truncated content as a startup error;
it does not silently discard corruption or repair an incomplete tail. Records
still have no checksum, stream flush is not a power-loss sync guarantee, and
concurrent access is unsupported. DELETE only removes the current in-memory
index entry. Since it writes nothing to the log, recovery will make an erased
key reappear if a prior PUT exists.

### 6. What new problem does this design introduce?

Appending a new PUT changes only the index location; the older record remains on
disk. Repeated updates therefore make the file grow even when the number of live
keys stays constant. A later compaction stage must reclaim obsolete versions,
but Stage 3 deliberately does not implement it. Before that, Stage 4 will make
deletion persistent by appending tombstone records and replaying them during
recovery, while also hardening corruption and durability behavior.

## Tombstones and checksums: persistent absence with integrity checks

### 1. What problem existed?

Deleting a key only from the in-memory index worked until the database was
closed. The log still contained the key's older PUT, so startup recovery would
replay that record and make the deleted value reappear. The format also had no
way to detect a byte that changed while stored or copied.

### 2. What did we build?

We added DELETE records called tombstones and evolved the record format to
version 2. A tombstone stores the DELETE operation and key with no value. Every
version 2 PUT and DELETE also ends with a standard CRC-32/ISO-HDLC checksum.
Invalid format, incomplete input, and checksum mismatch have distinct error
types; a missing key remains normal API state represented by `std::nullopt`.

### 3. How does it solve the problem?

For an existing key, `erase` appends and flushes a tombstone before removing the
key from memory. If the append fails, the index still describes the last
persisted state. During recovery, a PUT assigns its location in the index and a
later tombstone removes it. Another PUT after that tombstone assigns a new
location, so delete followed by reinsertion behaves naturally.

The checksum summarizes the exact encoded header, key, and value bytes. A read
recomputes that summary and reports a mismatch when the stored result differs,
detecting many accidental bit flips and burst errors that structural validation
alone would miss.

### 4. What happens internally?

The recovery scan still moves from offset zero toward the end of the log. Each
version 2 header tells it the bounded record length. The decoder requires the
entire payload and four checksum bytes, validates the record shape, and verifies
CRC-32 before returning the operation. The scan applies operations in order, so
the newest PUT or DELETE determines whether the key is live.

Deleting a missing key returns `false` and appends nothing. Repeated deletion
therefore does not grow the log with redundant tombstones. Empty and binary-safe
keys remain valid, and a tombstone's value length must always be zero.

### 5. What can still go wrong?

CRC-32 is an error-detection code, not encryption, authentication, or a digital
signature. Someone who deliberately changes a record can calculate a matching
checksum. CRC-32 cannot repair damage, recover lost bytes, or prove which of two
different records is correct. It also does not guarantee that a stream flush
survived power loss.

MiniKV still rejects an incomplete tail instead of automatically truncating or
repairing it. It has no concurrent-access protection, and version 1 logs are
rejected because they do not carry the checksum required by version 2.

### 6. What new problem does this design introduce?

A tombstone makes absence durable, but it does not remove the older PUT bytes.
Updates and deletes therefore leave obsolete records behind and the file keeps
growing. Compaction must eventually rewrite only the state that still matters.
Before compaction, later work also needs a precise OS-level sync policy and a
safe policy for recovering or repairing an incomplete final record.

## Crash-safe tails and explicit durability

### 1. What problem existed?

An append can stop after only part of its header, key, value, or checksum has
been written. This is a torn write. Stage 4 correctly called the last record
incomplete, but it refused to open the database afterward, so one interrupted
append made every earlier valid record inaccessible until manual intervention.

There was also only one vaguely described persistence behavior. Calling
`write`, flushing a C++ stream, asking the operating system to sync a file, and
physically storing bits are different boundaries and must not be presented as
the same guarantee.

### 2. What did we build?

Recovery now truncates a clearly incomplete final record back to the offset
immediately after the last format-valid, checksum-valid record. It never
truncates a complete record with invalid structure or a checksum mismatch.

We also added two configurable modes. `DurabilityMode::Buffered` is the default.
`DurabilityMode::Sync` uses a small platform boundary that calls
`FlushFileBuffers` on Windows and `fsync` on POSIX. The append and any requested
sync finish before MiniKV changes the in-memory index.

### 3. How does it solve the problem?

The end of the last valid record is a safe append boundary. If EOF occurs before
the next record is complete, removing only those remaining bytes restores a log
that can be replayed and appended to normally. Earlier PUTs and tombstones keep
their original order and checksums.

The durability mode makes the tradeoff explicit. Buffered mode avoids waiting
for stable storage on every mutation. Sync mode waits for the operating system's
durable-file flush request before reporting success, giving applications a
stronger persistence boundary when they need one.

### 4. What happens internally?

A buffered `write` usually copies bytes into user-space or kernel buffers rather
than directly onto storage media. Flushing the C++ stream pushes its user-space
buffer toward the operating system. The OS normally keeps those bytes in its
page cache: memory used to combine, schedule, and accelerate file I/O. A process
crash leaves the OS and its cache alive, but a kernel crash or power loss does
not.

In Sync mode, MiniKV follows the stream flush with `FlushFileBuffers` or `fsync`.
Those calls ask the OS to send the file's data and length through the storage
stack before returning. On recovery, MiniKV reads sequentially. EOF during the
next record triggers a resize to the last verified offset; in Sync mode that
resize is synced too. Complete corruption throws and leaves the file untouched.

### 5. What can still go wrong?

Even after a successful sync call, physical persistence ultimately depends on
the OS, device firmware, controller, and storage hardware honoring their flush
contracts. MiniKV does not sync the parent directory entry when a brand-new log
file is created. A failed sync can also be ambiguous: the record may have reached
disk even though the caller received an exception. MiniKV leaves the index
unchanged, rejects later appends on that instance, and lets restart validation
decide what is present.

A bounded length field corrupted so it extends beyond EOF looks exactly like a
torn append and follows the truncation policy. CRC-32 still is not
authentication. There are no multi-record transactions, concurrency controls,
replicas, or general ACID guarantees.

### 6. What new problem does this design introduce?

Durable sync can reduce throughput because every mutation may wait for slower
storage layers instead of being batched in memory. Applications must choose
between that latency and the weaker buffered boundary. MiniKV also still assumes
one unsynchronized user at a time; Stage 6 must define safe multithreaded access
without weakening disk-first mutation ordering or recovery behavior.

## Multithreaded access: one clear critical section

### 1. What problem existed?

Stage 5 had shared mutable state but no synchronization. A race condition occurs
when threads access the same state at the same time, at least one access changes
it, and their ordering is not controlled. The result can depend on timing and,
for ordinary C++ containers, can be undefined behavior rather than merely an
unexpected winner.

The index can rehash while another thread reads it. The storage log has one
active append stream, one current end offset, one read stream with a mutable seek
position, and append-failure metadata. Without a lock, concurrent `PUT A=1` and
`PUT B=2` could interfere with file output, assign incorrect locations, race in
the hash table, or leave the disk record order inconsistent with the index.

### 2. What did we build?

Each `MiniKV` now owns one `std::mutex`. A mutex lets only one thread own a
protected region at a time. That protected region is called a critical section:
code that must execute as one coordinated unit with respect to other threads.

All API operations acquire the mutex. PUT and DELETE hold it across record
creation, disk append, optional durable sync, and the following index change.
GET holds it across index lookup and the complete seek/read/decode operation.
`contains`, `size`, and `empty` also lock before inspecting the index.

### 3. How does it solve the problem?

Only one operation at a time can touch an instance's index, streams, current
offset, or failure flag. A successful writer finishes its append before changing
the index, while a failed append leaves the index alone. Readers therefore see a
state before or after a mutation, never the half-finished relationship between
its disk record and memory entry.

Deterministic tests use barriers to release threads together. They cover many
GETs, PUTs to different and identical keys, GET racing with PUT or DELETE,
mixed PUT/DELETE contention, and a restart whose recovered state must match the
state observed after all concurrent calls finish.

### 4. What happens internally?

The current lock granularity is one coarse lock for an entire MiniKV instance.
It is an exclusive/write lock in effect: while one caller owns it, every other
caller waits, even when both callers only want to read. This makes the invariant
between the log and index easy to inspect and explain.

A shared/read lock is a mode that multiple readers may hold together. An
exclusive/write lock prevents both other writers and readers. C++ offers those
modes through `std::shared_mutex`, but MiniKV does not use it yet. The current
storage layer has a single read stream whose seek position and error flags are
mutable. Safe parallel GET I/O needs positional reads or independent handles,
not merely replacing the mutex type.

### 5. What can still go wrong?

Correct locking usually reduces concurrency because waiting threads cannot make
progress inside the protected region. MiniKV even holds its lock during disk
I/O and durable sync, so a slow mutation delays unrelated keys and all readers.
The design promises thread safety, not maximum parallel throughput or fairness.

A deadlock is a permanent wait cycle: for example, one thread holds lock A while
waiting for B and another holds B while waiting for A. Normal MiniKV calls take
one instance lock once, do not recursively call locking API methods, and never
upgrade a read lock, so they cannot create that cycle. Move assignment locks two
instances with `std::scoped_lock`, which provides deadlock avoidance.

The mutex protects only one object. Two separate MiniKV instances or processes
opening the same file can still race. The caller must also prevent an instance
from being moved or destroyed while another thread is using it.

### 6. What new problem does this design introduce?

The coarse mutex deliberately trades throughput for a small correctness model.
Measurements may later show that read parallelism or key-level concurrency is
valuable. Before introducing shared locking, MiniKV needs a storage-read design
whose file position is not shared, plus benchmarks proving the extra complexity
addresses a real bottleneck. File growth from obsolete PUTs and tombstones also
remains the next roadmap problem for segmentation and compaction.
