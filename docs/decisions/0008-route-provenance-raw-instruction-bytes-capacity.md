# ADR 0008: Route-Provenance Raw-Instruction-Bytes Buffer Capacity

- Status: Accepted
- Date: 2026-08-29
- Amends: The generated-runtime provenance ABI physically owned by `runtime/genesis/runtime.h`
  (`GenesisBusAccess.raw_bytes` / `GENESIS_MAX_RAW_BYTES`) and enforced in `runtime/genesis/runtime.c`
  (`genesis_valid_provenance`), together with its two translation-time mirror constants
  (`genesis_frontier_max_raw_bytes` in `src/codegen/c11/frontend.cpp` and
  `src/machine/genesis/frontend.cpp`) and the sanitized/full-report validator bound in
  `tools/genesis_startup_bridge.py`. Does not amend, and does not relitigate, ADR 0002's
  static-dispatch boundary (no target-byte fetch, no decoder, no interpreter, no JIT, no second
  dispatcher), ADR 0003's shared-route ownership, ADR 0004/0005's privacy boundary, the wire report
  schema, or the stop/diagnostic enums.

## Context

The general-startup C4 route retains, for each accepted or frontier instruction, the verified raw
instruction bytes so a runtime routing failure can attach source provenance without ever re-reading
the image. That retained span is bounded by a single capacity that appears in five coupled places:

- `genesis_frontier_max_raw_bytes` in `src/codegen/c11/frontend.cpp` (the C11 emitter's fail-closed
  gate on `genesis_attach_route_provenance` and the frontier-stop builder);
- the same mirror constant in `src/machine/genesis/frontend.cpp` (the discovery-side frontier-shape
  eligibility gate);
- `GENESIS_MAX_RAW_BYTES` plus `uint8_t raw_bytes[GENESIS_MAX_RAW_BYTES]` in
  `runtime/genesis/runtime.h` — the generated-runtime provenance ABI the C11 backend's output
  `#include`s;
- the `instruction->length` / `access->raw_byte_count` bounds checks in
  `runtime/genesis/runtime.c` (`genesis_valid_provenance`), which consume `GENESIS_MAX_RAW_BYTES`;
- the equivalent `length` / `raw_byte_count` range checks in the canonical route driver
  `tools/genesis_startup_bridge.py` (`valid_provenance`), used for `--diagnose-frontier` and
  `--compare-runs`.

The value was `8`. The canonical authorized generated-native route reaches a `MOVE.L #imm32,(xxx).L`
store into the VDP control-port window whose full decoded instruction length is 10 bytes. Once that
store is routed to the runtime device gate (the write-direction mirror of the read arm), the 10-byte
instruction exceeds the capacity and emission fails closed at `genesis_attach_route_provenance`. The
capacity must be raised to the general-startup decoder's true maximum accepted instruction length,
consistently, with no static/runtime ABI skew.

### The decoder's true maximum accepted instruction length

The general-startup (`M68kDecodeProfile::general_startup`) decoder accepts MC68000 forms only. Its
effective-address layer (`src/cpu/m68k/decode.cpp`, `m68k_decode_one_ea`) accepts at most one
absolute-long (4-byte) or immediate-long (4-byte) extension field per operand and rejects the
indexed `d8(An,Xn)` mode outright. The longest accepted encoding is therefore a two-operand `MOVE`
whose source and destination are each an absolute-long address (or an immediate-long source plus an
absolute-long destination): one operation word + two source extension words + two destination
extension words = 10 bytes. This is the documented longest MC68000 instruction (Motorola *M68000
Family Programmer's Reference Manual*, `MOVE` instruction format and the absolute-long addressing
mode). No form the decoder accepts is longer.

## Decision

The capacity is `12`, expressed once as
`SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES` (`UINT32_C(12)`) in the already-shared
translation-time/runtime contract header
`include/segarecomp/machine/genesis/address_space_contract.h`.

- `runtime/genesis/runtime.h` now includes that contract header and defines
  `#define GENESIS_MAX_RAW_BYTES SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES`, so the emitted
  provenance-ABI array length, the runtime validator bound, and the ABI `#define` are the same token.
- Both translation-time mirror constants (`genesis_frontier_max_raw_bytes`) are defined as
  `SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES`.
- `tools/genesis_startup_bridge.py`'s `valid_provenance` range checks use `12` (the driver is not a
  C translation unit and cannot include the header; it is a hand-maintained mirror of the same ABI
  bound, updated in lockstep here).

**Chosen value rationale.** The true decoder maximum is 10 bytes. `12` rounds that up to the next
even byte count, giving exactly one 16-bit word of headroom against a future single-extension-word
addition to an accepted form, while keeping the four fixed-size provenance arrays small
(`raw_bytes[12]` vs `raw_bytes[8]` adds 4 bytes per `GenesisBusAccess`). It is an
engineering-headroom choice, not a hardware fact.

**Fail-closed guarantee is unchanged.** Every one of the five sites keeps its existing check; only
the bound is raised. Any decoded instruction longer than `12` bytes (not producible by the
general-startup decoder, but defended anyway) is still rejected before any state mutation: the C11
emitter returns a `/* translation rejected: ... */` string, the discovery-side eligibility gate
declines to promote the frontier, and `genesis_valid_provenance` /
`genesis_startup_bridge.py:valid_provenance` reject the report.

**Single-source-of-truth mechanism.** The C/C++ side has exactly one literal
(`SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES`); the static emitter constants, the emitted
runtime-header array length, and the runtime validator all expand from it, so a static/runtime ABI
skew is a compile error, not a silent divergence. `tests/genesis_startup_runtime_c2_test.py` keeps a
`_Static_assert(GENESIS_MAX_RAW_BYTES == 12U, ...)` pinning the ABI value.

## Consequences

- The route-provenance raw-instruction-bytes buffer carries the full bytes of any general-startup
  instruction, including the 10-byte `MOVE.L #imm32,(xxx).L` and `MOVE.L (xxx).L,(xxx).L` forms,
  end to end through the generated-runtime provenance ABI and its validators.
- No wire-schema redesign, provenance versioning scheme, or removal of the maximum-length check was
  required; the change is one coordinated constant/layout raise across the five coupled sites.
- Reviewers checking provenance-ABI consistency must confirm all five sites expand from (or, for the
  Python driver, mirror) the one contract-header constant, and that each site still fails closed
  above the bound.
- A future decoder change that admits a genuinely longer accepted encoding must raise this one
  constant (and the Python mirror and the static assert) together, and record why, rather than add a
  per-site override.
