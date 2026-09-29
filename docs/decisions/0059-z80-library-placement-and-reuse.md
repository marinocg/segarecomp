# ADR 0059: Z80 Library Placement, Runtime-ABI Location and Reuse Boundary

- Status: Accepted
- Date: 2026-09-29
- Task: SEG-008-T001
- Related: ADR 0056 (Z80 architectural contract), ADR 0057 (Z80 oracle and corpus), ADR 0058 (Z80 static-code
  strategy), `docs/architecture/z80-cpu-contract.md`, `docs/architecture/m68k-reuse-boundary.md`.

## Context

SEG-008 adds Z80 as the second CPU. The milestone fixes a three-way split: CPU semantics, a sibling C11
lowering, and a small machine-neutral generated-code runtime ABI. Machine policy stays outside all three. It
also lists what is deliberately reused and what is deliberately not reused. This ADR places the new
libraries, decides the additive `libs/core` change, and confirms both lists against current `main`
(`4f8244a`).

## Decision

### Placement

| component | location | CMake target | depends on |
| --- | --- | --- | --- |
| Z80 decode, legality, form identity, semantics/effects, timing metadata | `libs/cpu/z80/` (`include/segarecomp/cpu/z80/`, `src/`) | `segarecomp::cpu_z80` | `segarecomp::base` only |
| Z80 C11 lowering, image-level owner and entry emission | `libs/codegen/c11/` sibling sources (`src/z80*.cpp`, `include/segarecomp/codegen/c11/z80*.hpp`) | `segarecomp::codegen_c11_z80` | `segarecomp::codegen_c11`, `segarecomp::cpu_z80` |
| Z80 generated-code runtime ABI (C header consumed by generated C) | `libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h`, owned by `codegen_c11_z80` and installed with it | header of `segarecomp::codegen_c11_z80` | C11 standard headers only |

- `codegen_c11_z80` mirrors `codegen_c11_m68k`: a sibling target in the same directory. It must not link
  `cpu_m68k`, `codegen_c11_m68k`, `codegen_c11_genesis`, `machine_genesis` or any platform target.
- The runtime ABI is a plain C11 header. It declares the Z80 state structure, memory and I/O callbacks,
  interrupt inputs (INT line with the data-bus byte supplied through the acknowledge callback, NMI edge), the
  cycle deadline, the current-code-image-identity callback, and the result enum. That enum keeps resumable
  outcomes and fail-closed errors in distinct classes (ADR 0058). It contains no machine policy.
- Machine policy (memory maps, banking/mappers, port decoding, interrupt sources, device scheduling) stays in
  later platform code (SEG-009 Master System, SEG-032 Genesis Z80).

### Additive `libs/core` change

- Add `CpuVariant::z80` when T002 first needs it; no other value changes.
- Z80 provenance is **Z80-local** (`segarecomp::cpu::z80`). It holds the start's code-image identity, 16-bit
  logical address and image offset, plus the logical byte count. The instruction's bytes may continue across the
  0xFFFF -> 0x0000 fetch wrap or into an invariant window (ADR 0058 §5), so they are not assumed to be one
  contiguous storage slice. The length is not bounded by an assumed maximum, because
  DD/FD chains are unbounded. The existing `DecodeSource` and `InstructionProvenance` are M68K-shaped:
  `M68kProgramAddress` and fixed `std::array<uint8_t, 2> bytes`. They are neither reused nor widened. A neutral
  widening would change M68K types for no current second consumer. It can be reconsidered only when a
  CPU-neutral consumer (for example a shared diagnostics sink) needs both.
- `TargetAddressSpace` is not extended in T001. A Z80 program address is `(code-image identity, uint16)` and
  stays Z80-local.

### Reuse confirmed against current `main`

| item | status on `main` | Z80 use |
| --- | --- | --- |
| `codegen/c11/compiled_entry_table.hpp` | generic header; opaque 32-bit keys; names via `CompiledEntryTableNames` (Genesis defaults) | reuse with explicit Z80 names; key = code-image identity and address packed into 32 bits (ADR 0058) |
| `codegen/c11/translation_units.hpp` | generic, key-sharded, deterministic | reuse for owner sharding. The unit key is the dense position (image ordinal x image length + offset), not the lookup key (ADR 0058 §3) |
| `codegen_c11` common support (`emit_c_manifest`) | metadata-only manifest from `RomInfo` (media classification) | platform-level. A Z80 generated program may emit it through the platform (SEG-009); the CPU lowering does not depend on `RomInfo` |
| `libs/core` provenance/`CpuVariant` | M68K-shaped structures, one enum value | additive `CpuVariant::z80` only (above) |
| SEG-020 first-divergence (`tools/m68k_first_divergence.py`, ADR 0042) | M68K boundary schema (`d[8]`, `a[8]`, `sr`) | pattern reused: bounded sequential lockstep, first differing domain, write-ordering rule. See extraction below |
| SEG-021 tooling pattern (`m68k_legal_forms.py`, `m68k_capability_coverage.py`, `m68k_conformance.py`, validation manifest, ratchet) | M68K-specific (EA binding, CCR expectations) | pattern reused: the T001 Z80 dataset exists (`tools/z80_legal_forms.py`); T003 adds Z80-local coverage and conformance tools |
| `generated_code_scalability_report.py` | Genesis-symbol-shaped line classifier | pattern reused by the T001 experiment and later Z80 measurement; no shared code |

### Not reused (confirmed)

The M68K decoder, effective-address machinery, CCR/SR flag code, `M68kInstructionKind` and instruction
structures, M68K timing tables and descriptors, the exception/privilege model, and the C4 preflight/deferred-commit
machinery are not reused. Tier-1/Tier-2 discovery, continuation authorities, the ADR 0011/0039 RTS continuation
sets and Ghidra-assisted inventories/descriptors are not reused either. Nothing in the Z80 design wraps or
generalizes them. `recompiler/frontend.hpp` frontier projection is not used: broad AOT (ADR 0058) has no Z80
frontier set. There is no universal CPU IR or state.

### CPU-neutral extraction candidates for T003

Extraction requires Z80 as a real second consumer and no behavior change to the M68K tool (SEG-008 Scope).

1. **First-divergence comparison core.** `parse_stream` / `compare_streams` in `tools/m68k_first_divergence.py`
   contain a generic lockstep comparator. It takes a bounded limit, and write ordering differs by destination
   versus same destination. It is a candidate if T003 parameterizes it by a field list and effect kinds, with the
   M68K test suite unchanged and green. Otherwise T003 writes a Z80-local comparator following ADR 0042.
2. **Subprocess timing/RSS helper** (`timed()` in `tools/generated_code_scalability_report.py`). This is a
   trivial candidate; duplication is acceptable if extraction adds coupling.

No other M68K tool has a CPU-neutral core worth extracting. The coverage and conformance tools bind M68K
effective addresses and CCR semantics throughout.

## Consequences

- `cpu_z80` and `codegen_c11_z80` build and link without any M68K or Genesis target. T002 and T003 add dependency
  tests mirroring `cpu_m68k_no_genesis_dependency_test.py` and `architecture_dependency_test.py`. Generic
  libraries (`base`, `media`, `codegen_c11`, `recompiler`) gain no Z80 dependency.
- The generated-runtime ABI header is the Master System integration seam that T010 publishes.
