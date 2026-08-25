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
