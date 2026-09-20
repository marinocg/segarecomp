# ADR 0042: Execution observability and first-divergence diagnosis contract

- Status: Accepted
- Date: 2026-09-20
- Task: SEG-020-T001 (research/contract only; no product implementation)

## Context

SEG-020 lets a developer find where generated-native execution first differed from a trusted
reference, with guest/generated provenance and the owning layer (CPU vs Genesis device). This ADR
audits the existing seams, freezes reuse decisions and the minimal neutral concepts, and states what
is deliberately not generalized. Code citations are to the tree at this commit.
Non-goals: GUI/GDB/LLDB replacement, reverse execution, persistent trace database, replay engine,
universal CPU state/IR/event bus, interpreter, JIT, runtime opcode decoding, Z80/SH-2 work.

## 1. Anchor audit

| Anchor | Code | Verdict |
| --- | --- | --- |
| `InstructionProvenance` / `DecodeSource` (cpu variant, program address, image offset, raw bytes, length) | `libs/core/include/segarecomp/core/provenance.hpp` | **Reuse as-is** as the guest-provenance record. |
| M68k static program records (`M68kStaticBlock` holds `InstructionProvenance` list, `M68kStaticEdge`, `M68kStaticCall`) | `libs/cpu/m68k/include/segarecomp/cpu/m68k/static_program.hpp` | **Extend** (T002): read-only projection for closure facts; no structural change. |
| `M68kOperationEffect` (register write masks, memory/stack/PC effect kinds) | `libs/cpu/m68k/.../effects.hpp` | **Reuse as-is** as the source of the bounded effect projection (Section 5). Its footprint-completeness flag gates what may be compared. |
| M68k cycle timing owner | `libs/cpu/m68k/.../timing.hpp`, ADR-0041 | **Reuse as-is**; not compared (Section 6). |
| `genesis_runtime_retire_m68k_instruction` | `platforms/genesis/runtime/runtime.h` | **Reuse as-is**: sole instruction-boundary seam; the checkpoint/history hook attaches here. |
| `recent_pc_history` circular buffer (capacity 64) + `genesis_write_ephemeral_pc_history` | `runtime.h` (`GenesisRuntime`, `GENESIS_RECENT_PC_HISTORY_CAPACITY`), ADR-0040 | **Extend** (T003): generalize the bounded ring into typed events; keep its exclusion from stable serializations and its ephemeral-channel-only transport. |
| `ephemeral_frontier` / `--diagnose-frontier` / `parse_ephemeral_pc_history` | `tools/genesis_startup_bridge.py` | **Extend** (T007): remains the single private diagnosis assembly; parse failure must never fail a run. |
| `checkpoint_evidence.h` (`GenesisCpuEvidence`, `GenesisRamEvidence`, `GenesisDeviceEvidence`, `GenesisTransactionEvidence`, digests) and the oracle `tests/oracle/genesis/checkpoint_oracle.*` | `platforms/genesis/runtime/checkpoint_evidence.h`, ADR-0012 | **Reuse as-is for Genesis device/RAM evidence** (T006); **extend** only via a minimal M68k-owned digest type for CPU-only checkpoints (T004). `GenesisCheckpointPcClass` is UNKNOWN-only today and is not relied on. |
| Musashi differential harnesses | `tests/m68k_*_musashi_differential_test.py`, pinned rev `313ebf1b...` | **Reuse as-is** as the trusted reference; new comparison code is test/tool-side only and skips gracefully when the pin is unset. |
| `tools/genesis_frontier_debug.py` (LLDB/GDB stop inspection) | `tools/genesis_frontier_debug.py` | **Reuse as-is; replace-not-allowed.** Stays a private-session tool; not a durable-evidence path. |

Any anchor not listed as extended is not modified by SEG-020.

## 2. Checkpoint boundary granularity

Decision: **instruction boundary** (one retired MC68000 instruction), matching ADR-0041 where
architectural effects commit before retirement. Block granularity is rejected: generated C is
block/PC-keyed but Musashi steps by instruction, so a block boundary could not name the first
differing instruction. Boundary index N is the count of retired instructions since reset. Blocks remain static/provenance context only; there is no second, block-level checkpoint system. Data lives
in the M68k-owned record; the Genesis retirement seam only forwards. Unresolved: whether an
exception-entry retirement counts as its own boundary is labeled **UNRESOLVED-T004** (default: the
faulting instruction's boundary reports the exception state).

## 3. Sequential compare vs bisection

Decision: **sequential lockstep compare** with a bounded window. The tool advances generated and
oracle in lockstep by boundary index and reports the last matching boundary N and first differing
boundary N+1. Bounded bisection is **deferred**: it requires re-execution from checkpoints/replay, which is
a non-goal. The compare stops at the divergence or at a caller-supplied boundary limit; the limit is
mandatory.

## 4. Bounded history event categories (justified now)

Only: (a) retired-instruction PC/provenance key, (b) control transfer taken (kind: direct, computed,
call, return, exception entry), (c) Genesis device-visible access (bus kind, width, direction,
device/region class — never values beyond what ADR-0012 already permits durably). Capacity is a
compile-time constant, ring semantics, oldest-overwritten, no allocation. Deferred: interrupt
timing events, DMA phase events, sound events, register-value histories.

## 5. Architectural effect comparison

Decision: compare a **minimal projection**, not a new universal state: `D0-D7`, `A0-A7`/USP-SSP as
the runtime holds them, `SR`, `PC`, a bounded list of memory writes (address, width, value), and a
trap/exception marker (vector number).
Ownership split: `M68kOperationEffect` supplies only the **static** effect classification and register
write footprint (memory/stack/PC effect kinds, `register_write_footprint_complete`); it is not an execution
result and must not become one. Concrete runtime-dependent addresses and values are observed at the
existing execution/write path by a **minimal diagnostics-only observation** captured there when
diagnostics are enabled (T004 chooses the seam). If an effect cannot be completely and reliably
observed (incomplete footprint, unobservable write), that boundary is
**"unsupported for comparison"** — never "equal". Writes into RAM are additionally
covered by a RAM digest at checkpoint granularity (T004 decides digest cadence). Field-level
differences name the field; first differing **domain** is `cpu` when any projection field differs,
else `device` when device evidence differs, else `none`.

## 6. Timing

Decision: **no cycle comparison**. Generated cycles come from the CPU timing owner but Musashi's
cycle accounting is not certified equal for every form; no canonical, already-correct,
Musashi-comparable value exists and none is necessary for first-divergence on state. Deferred until
a task needs timing-divergence diagnosis.

## 7. Image / module identity without module machinery

Decision: **image identity** is the existing platform-owned ROM identity — currently the ROM SHA-256
carried by `GenesisCheckpointIdentity` (`rom_sha256`). **Image offset** (`DecodeSource::image_offset`)
is a position within that image, not an identity. The guest diagnostic identity is
`(cpu variant, platform-owned image identity, guest address / image offset as needed)`, composed at the
reporting boundary. Multi-image safety (SEG-018) is preserved because two images never share an
identity. No module registry, loader graph, universal `ImageIdentity` type, or Sega-CD/Saturn concept is
introduced.

## 8. Static closure counts

Decision: **deferred unless trivially derivable.** Counts of blocks, instructions, edges and calls
are directly derivable from `M68kStaticBlock`/`M68kStaticEdge`/`M68kStaticCall`; T002 may report exactly
those. Unresolved-target, pruned, and immutable-AOT membership counts are **not** promised
(they depend on discovery internals, ADR-0010/0038/0039); no closure framework is created.

## 9. Opt-in mechanism

Decision: a **generation-time option** (CLI flag on the existing generator command family, like
`--immutable-rom-aot`) that compiles diagnostic hooks into generated C. With the option off, the
emitted C must be byte-identical to today's output; T004 adds a golden/identity test. Hooks only
observe: they never alter dispatch, guest state, timing, or become generation input. No runtime flag
can enable them in a diagnostics-off binary.

## 10. Ownership and privacy

- M68k diagnostic types/digests: `libs/cpu/m68k`; Genesis events/state: `platforms/genesis/runtime`
  and device code; neutral seam only in `libs/core` if two consumers exist (today none: none added).
  Guarded by the existing dependency tests (`architecture_dependency_test.py`,
  `cpu_m68k_no_genesis_dependency_test.py`).
- Commercial runs: addresses, bytes, disassembly, values and traces stay in the ephemeral channel;
  durable output (ADR-0004/0005/0012) is normalized classes only. Complete traces and bundles are never
  committed. Deterministic output: identical inputs/options give byte-identical reports.

## 11. Explicitly not generalized

Universal CPU state or `ICpu`; observable-device hierarchy; event bus; Z80/SH-2 observability
(Z80 is the first future consumer of any neutral seam — none is created now); rendering diagnosis;
persistent traces; bisection/replay; timing comparison; module machinery.

## 12. Dependencies of later tasks

- T002: sections 1, 7, 8. T003: sections 4, 9, 10. T004: sections 2, 5, 9.
- T005: sections 2, 3, 5 and the Musashi pin. T006: sections 1 (checkpoint_evidence), 5 (domain
  rule), 10. T007: sections 1 (bridge), 9, 10. T008: all, plus the unresolved items above.

## 13. Unresolved / deferred

- UNRESOLVED-T004: boundary numbering across exception entry.
- Deferred: bisection, timing, interrupt/DMA/sound events, closure counts beyond block/edge/call.

## 14. T002 implementation note

`segarecomp emit-general-startup-bridge-c ... --provenance-diagnostics` (opt-in, default off) appends a
deterministic C lookup (`segarecomp_provenance_diag_table`: guest PC, image offset, block entry,
M68k form id = `M68kInstructionKind` ordinal) plus the platform-supplied image SHA-256 and static
closure counts (blocks, instructions, edges, calls — the section 8 subset only). Owner:
`libs/codegen/c11/.../provenance_diagnostics.hpp` (M68k lowering layer, Genesis-free). Raw instruction
bytes are never projected and `InstructionProvenance`/`DecodeSource` are unchanged. With the flag off the
output is byte-identical (the diagnostic text is a pure suffix). Unresolved/pruned/AOT-membership counts
remain deferred.

## 15. T004 implementation note (M68k state checkpoints)

`GenesisM68kCheckpoint` (runtime, Genesis-local; enabled by the same generation-time
`--provenance-diagnostics` option, otherwise never read or written) finalizes one boundary at
`genesis_runtime_retire_m68k_instruction` (after IRQ admission, so an exception entry belongs to the
retiring boundary — resolves UNRESOLVED-T004 for the digest). It records M68k-owned state (D0-D7, A0-A7,
USP, SR, PC) plus effects observed at the existing `genesis_route_access_bus` write path (address, width,
value) and exception-frame entry (vector, handler). Digest is FNV-1a 64 over a fixed little-endian
serialization; RAM/device state and cycle counts are excluded. More than eight effects in one boundary,
or an invalid boundary, is `unsupported for comparison` and never compares equal.
`genesis_m68k_checkpoint_write_detail` expands the last boundary to field level on request. No effect
values are persisted; the digest is ephemeral diagnostic output.

The synchronous divide-by-zero route returns the handler transfer without calling the retire seam, so
`genesis_raise_divide_by_zero` completes the faulting DIV instruction's boundary itself (post-exception
state, two frame writes, vector-5 trap effect). It is the same boundary class as an ordinary retired
instruction: no second retirement, scheduler tick or IRQ admission occurs, and the handler's first
instruction begins the next boundary with an empty effect set.

## 16. T005 implementation note (M68k/Musashi first-divergence diagnosis)

`tools/m68k_first_divergence.py` is the tool-side workflow (no production, generation or runtime
change). `oracle` builds the pinned, unmodified Musashi core (pin and clean-tree checks as the other
differential tests) and emits one boundary record per retired instruction using `m68k_execute(1)`;
memory writes come from the write callbacks and an exception entry is recognized as an aligned
vector-table longword data read while stepping. `compare` runs the section 3 sequential lockstep
over the generated `genesis_m68k_checkpoint_write_detail` lines with a mandatory boundary limit and
reports last matching boundary N, first differing boundary N+1, the pc of the differing instruction
(the previous boundary's resulting pc, or `--initial-pc`), each differing field (register, SR, PC,
USP, `effect:write@addr/wN`, `effect:trap`), the caller-supplied image identity and domain `cpu`.
Because Musashi pushes an exception frame PC-first while generated code pushes SR-first, write order
inside one boundary is not compared (the write multiset is), so the field-level path is used rather
than the order-sensitive FNV digest. A boundary flagged unsupported is reported
`unsupported_for_comparison`, never equal. Test-only fault injection (a perturbed stacked-SR write and
a perturbed MOVEQ result) is applied only to temporary copies of the emitted C / runtime source inside
`tests/m68k_first_divergence_test.py`; no production flag or hook exists. Device-domain comparison is
T006.

Known limits (T005): (a) effects within one boundary are compared as a multiset, so a swap of two
writes to the same address and width with different values inside one instruction is not detected;
(b) the oracle's exception-entry recognition treats any aligned longword data read below 0x400 while
stepping as a vector fetch, so a genuine low-memory longword data read would be a false `effect:trap`
divergence — acceptable for the synthetic fixtures, to be revisited before real-ROM use; (c) more than
64 oracle effects in one boundary is flagged unsupported.
