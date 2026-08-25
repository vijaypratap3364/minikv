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

Stage 2 must preserve mutations across process restarts without losing the fast
in-memory lookup path. The next design will need an append-only binary log,
tombstones for deletion, and startup replay that reconstructs the same map. It
should first establish basic persistence; stronger torn-write and durability
guarantees remain later problems.
