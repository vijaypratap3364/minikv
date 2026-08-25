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
