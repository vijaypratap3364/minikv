# MiniKV Engineering Rules

These rules apply to all work in this repository.

## Purpose and scope

- MiniKV is a small persistent key-value storage engine for learning how storage
  systems implement persistence, indexing, crash recovery, concurrency,
  compaction, and performance.
- Keep the entire architecture understandable by one undergraduate student.
- Do not try to recreate PostgreSQL, RocksDB, Redis, or LevelDB.
- Add a mechanism only when it solves a concrete problem exposed by the previous
  design.
- Do not implement future roadmap stages unless the current task asks for them.

The intended architecture is:

```text
Client -> MiniKV API -> in-memory index -> append-only storage log -> disk
```

The eventual engine should cover binary records, restart recovery, tombstones,
checksums, crash and torn-write handling, durability, multithreaded access,
segment files, compaction, benchmarking, profiling, and CI sanitizers.

## Hardware and dependency constraints

- The primary Windows development laptop has about 5.8 GB of RAM.
- Keep local development native and lightweight.
- Do not install or require Docker Desktop, WSL, virtual machines, databases, or
  external services.
- Reserve heavy sanitizer/benchmark combinations for GitHub Actions when useful.
- Add dependencies only when they solve a real problem. Prefer the C++ standard
  library and small self-contained tests.

## Git workflow

- Use only `main`; do not create feature branches.
- Inspect repository status, the current branch, and remotes before making changes.
- Preserve an existing `origin`; never invent a GitHub URL.
- Never force-push or rewrite pushed history.
- Work directly on `main` and push meaningful passing commits to `origin/main`
  when that remote is configured.
- Commit coherent engineering milestones, not artificial stage markers. Essential
  implementation and tests belong in the same commit.
- Prefer concise Conventional Commit-style messages when appropriate.
- Before every commit, inspect the diff, build, run tests, and confirm that no
  generated files, temporary databases, or storage logs are included.

## C++ and design principles

- Use portable C++20 where supported.
- Prefer RAII, value semantics, standard library containers, const correctness,
  explicit error handling, deterministic cleanup, and small focused classes.
- Use smart pointers only when ownership requires them; never use raw owning
  pointers.
- Keep ownership and lifetimes obvious.
- Avoid unnecessary inheritance, excessive templates, custom allocators,
  lock-free programming, clever metaprogramming, premature optimization, and
  undefined behavior.
- Treat compiler warnings as defects, but do not blindly enable warnings-as-errors
  across every compiler when that would harm portability.

## Build, test, and repository hygiene

- Use CMake and keep GCC, Clang, and MSVC compatibility in mind.
- Typical local checks are:

  ```powershell
  cmake -S . -B build
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

- Keep generated build trees and test artifacts out of Git.
- Put public headers under `include/minikv`, implementation under `src`, tests
  under `tests`, command-line utilities under `tools`, benchmarks under
  `benchmarks`, and design material under `docs`.
- Tests should be deterministic and should clean up any files they create.

## Learning documentation

After every significant architectural feature, update `docs/learning-notes.md`
in plain English. Explain:

1. What problem existed?
2. What did we build?
3. How does it solve the problem?
4. What happens internally?
5. What can still go wrong?
6. What new problem does this design introduce?

Keep these notes architecture-focused rather than narrating C++ line by line.
Update `docs/architecture.md` when component boundaries or data flow change, and
update `docs/roadmap.md` when milestone scope changes.
