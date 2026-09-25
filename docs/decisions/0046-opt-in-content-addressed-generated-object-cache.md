# ADR 0046: Opt-In Content-Addressed Generated Object Cache

- Status: Accepted
- Date: 2026-09-26
- Task: SEG-022-T012
- Extends, without editing: ADR 0044 (deterministic translation-unit sharding) and SEG-022-T004's bounded
  parallel per-TU compile. Generated C, admission, the compiled-address sets, generated guest semantics
  and every correctness gate are untouched.

## Context

After SEG-022-T003/T004/T008 a large generated program is a stable, deterministic set of translation
units compiled independently and linked. Iterating on the bridge (rerunning the same ROM route, or changing
code that affects only some units) recompiled every unit although most were byte-identical to the
previous build. Compilation of the sharded units is the largest host build cost after generation.

## Decision

1. **Opt-in only.** `tools/genesis_startup_bridge.py --object-cache-dir <dir>` (or
   `SEGARECOMP_OBJECT_CACHE_DIR`) enables the cache for the sharded/viewer compile path
   (`compile_objects`). Without it nothing changes; CTest and CI never enable it, so no test outcome can
   depend on cache state.
2. **Key = every input that can affect the host object.** SHA-256 over length-prefixed fields: a schema
   string; the compiler identity (resolved driver path, `--version` output, `-dumpmachine` target triple
   and a fixed list of compile-affecting environment variables such as `SDKROOT`/`CPATH`); the exact compile
   argv (all flags, defines, include paths, optimization/debug profile); the working directory and the
   resolved source path (both reach debug info); and the digest of the complete preprocessed TU produced
   by the same argv plus `-E -dD`. The preprocessed stream keeps line markers (resolved header paths and
   system-header status) and macro definitions, so a change to the TU, to any included header's content or
   location, or to a macro that is never expanded still invalidates the entry. A driver that cannot report
   identity/target, or a TU that fails to preprocess, is compiled without the cache.
3. **Verified entries, fail safe.** An entry is one file `<key[:2]>/<key>.obj` holding a magic line,
   the SHA-256 of the object bytes, then the bytes; it is written to a temporary name and atomically
   renamed, and only after a successful compile. A hit is used only when the magic and digest verify;
   any unreadable, truncated, empty, foreign or mismatching entry is deleted and the unit is recompiled
   normally (then re-stored). Cache I/O failures (read-only cache, an entry held open by another process
   on Windows, a concurrent eviction) never fail the build: cache maintenance is best effort and the unit
   is simply compiled.
4. **Bounded disk use.** After each build the least-recently-used entries are evicted beyond
   `SEGARECOMP_OBJECT_CACHE_MAX_BYTES` (default 4 GiB); a hit refreshes recency. Temporary files left by an
   interrupted store are removed once older than an hour (younger ones may belong to a concurrent build).
5. **Privacy.** The cache holds host objects compiled from generated C and inherits the out-dir's status:
   for commercial inputs it lives only in an ignored local location (e.g. `.cache/`), is never committed,
   published or used as evidence, and is outside the compare-runs artifact surface.
6. One summary line `generated object cache: hits=… misses=… stores=…` goes to stderr when enabled.

## Consequences

- An unchanged rebuild reuses every object; the cost of a cold cache is one preprocessing pass per unit.
- The identity describes the driver named by argv[0]; a wrapper that changes behavior without changing
  its reported `--version`/target (or a launcher whose argv[0] is an interpreter) is outside this
  contract. The bridge always invokes the selected C compiler directly.
- The key includes the out-dir source path and cwd, so reuse happens within one out-dir/worktree (the
  iterative-rebuild case); a different out-dir is always a (correct) miss.
- Exporting `SEGARECOMP_OBJECT_CACHE_DIR` in a developer/CI environment also enables it under CTest; test
  outcomes remain correct because every hit is verified and every miss compiles normally.
- The non-sharded single-file path (small programs) is unchanged and uncached.
