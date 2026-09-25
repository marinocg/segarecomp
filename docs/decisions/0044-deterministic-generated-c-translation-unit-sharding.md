# ADR 0044: Deterministic Generated-C Translation-Unit Sharding

- Status: Accepted
- Date: 2026-09-25
- Task: SEG-022-T003
- Extends, without editing: ADR 0039 (monotonic AOT representation) and SEG-022-T002's streaming
  emitter contract (`emit_*_to(std::ostream &)`). Admission, the compiled-address set and generated
  guest semantics are untouched.

## Context

A Sonic immutable-ROM AOT build emits hundreds of MB of C in one translation unit. That single TU is
slow and memory-hungry to compile and, on the no-external-hints route, exceeds the compiler's
source-location limit. It also cannot be compiled in parallel.

## Decision

1. **Generic sharder, target-independent.** `segarecomp::TranslationUnitSharder`
   (`libs/codegen/c11/.../translation_units.hpp`, library `codegen_c11`, no CPU/platform knowledge) is a
   `std::ostream` sink. The emitter keeps writing one stream; it brackets each independently
   compilable top-level *unit* (one C function plus the file-scope objects private to it) with
   `ShardUnitScope(family, key, declaration)` and puts shared text between `shard_begin_header` /
   `shard_end_header`. On a plain stream every helper is a no-op, so the historical single-file output
   is byte-identical.
2. **Layout.** One shared header `<stem>.h`, one main TU (all unmarked "glue": bridge `main`,
   `genesis_dispatch`, owned-ROM literals), and per family a fixed number of shards. Genesis layout:
   `block` 8, `aot` 32, and single `stop` (frontier/tier-1/C4-lowering stop functions), `meta`
   (route-provenance lookup and `genesis_static_stop`), `entries` (sorted compiled-entry table and its
   binary-search lookup). TU count is therefore bounded by 1 + 8 + 32 + 3 = **44**, independent of ROM
   size. A unit is a whole function, so no guest instruction is ever split and shard boundaries have no
   guest-visible meaning.
3. **Deterministic, boring rule.** `shard = (key >> 10) % shards`, where `key` is the guest source
   address of the unit. No partition optimizer, no dependence on emission order, thread scheduling,
   host or path; the manifest `<stem>.units` lists main first, then sorted names. Files are written as
   `<name>.partial` and renamed only after every file succeeds (manifest last); any failure removes
   everything (fail closed).
4. **Linkage.** Every function that crosses a TU boundary is an ordinary external function declared
   once in the shared header (derived from the unit declaration, in emission order). The sharder
   rewrites the definition's leading `static ` off the exact declared signature. The two multiply-cycle
   helpers become `static inline` in the header. There are no cross-TU initializers, hence no
   static-initialization-order dependency. Each TU begins `#define _POSIX_C_SOURCE 200809L` then
   `#include "<stem>.h"`.
5. **When it applies.** `segarecomp emit-general-startup-bridge-c --generated-c-shard-dir <dir>`. Given
   together with `--generated-c-output`, the emitter shards only programs with at least 1024 compiled
   units (ordinary blocks + AOT entries) and otherwise writes the single file: a pure function of the
   accepted program size. The Python bridge passes both, compiles every TU (serially; parallel
   scheduling is SEG-022-T004) and links, and the single-file path is unchanged.

## Consequences

- No change to admission, the admitted/compiled address sets, retirement/cycle behaviour, exception
  handling, or membership semantics; the emitted function bodies are unchanged. Textual identity of the
  generated C is not required and is intentionally not preserved for sharded output.
- Symbols that were `static` are now external, so the compiler no longer diagnoses an *unused* such
  function; a *missing* definition still fails at link.
- The line-oriented attribution in `tools/generated_code_scalability_report.py` reports per-TU
  statistics for sharded output and keeps the region attribution for single-file output.
