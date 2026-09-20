# ADR 0012: Checkpoint-Evidence Runtime Ownership and the Independent Title-Frame Oracle Boundary

- Status: Accepted
- Date: 2026-08-31
- Amends: the persistent-device-state and checkpoint-evidence/frame-oracle contract
  (`docs/architecture/genesis-persistent-device-state-and-checkpoint-evidence-contract.md`,
  SEG-007-T042). This ADR reassigns that contract's §16 oracle-construction ownership away from the
  now-terminal SEG-007-T052 and pins the production owner for its §1.4 `GenesisInterruptState`,
  §12 `GenesisCheckpointPcClass`/bundle schema, §13.1 checkpoint-entry transition, and §9/§12
  extraction. It changes **no** part of §12–§15 (bundle schema, canonical serialization, digest rule,
  per-category comparison, exact-match tolerances) or §13's stable-frame condition — only ownership,
  file/seam placement, and the oracle's independence boundary are decided here.
- Related: ADR 0002 (static translation / fail-closed execution), ADR 0003 (Genesis startup
  shared-route boundary — one semantic owner per fact), ADR 0004 (generic frontier-classification
  privacy boundary), ADR 0005 (CLI static report outside the wire-report privacy boundary),
  ADR 0011 (non-recursive static discovery — unchanged and not reopened by this ADR).
- Implemented by: SEG-007-T131 (production substrate) and SEG-007-T132 (independent oracle plus the
  authorized checkpoint-identity retry), not this ADR. This ADR changes no `src/`, `include/`,
  `runtime/`, `tools/`, `tests/`, or build-system file.

## Context

SEG-007-T129 was authorized to bind the real-ROM-derived Sonic title checkpoint identity and to
finish the independent title-frame oracle SEG-007-T042 §16 specified. It returned
`TASK_CONTRACT_CONTRADICTION` for a structural reason, recorded in its Evidence:

- **The production runtime does not expose the shapes T042's checkpoint-evidence half presumes.**
  `runtime/genesis/runtime.h` today owns `GenesisRuntime`, `GenesisDeviceState`
  (`z80_bus`, `vdp`, `psg`, `controller_io` — **no `interrupt` field**), `genesis_route_access`
  (the one routed device-state seam), `GenesisControlTransfer` / `GenesisDispatchFunction` /
  `genesis_runtime_drive` (the one typed control-transfer dispatcher), and
  `genesis_write_sanitized_report` / `genesis_write_full_report`. It does **not** define
  `GenesisInterruptState`, `GenesisCheckpointPcClass`, `checkpoint_entered` /
  `vblank_transition_count` / the §13.1 sticky transition, `GenesisCheckpointEvidenceBundle`, or
  `genesis_extract_checkpoint_evidence`. T042 originally assigned those to SEG-007-T047 (interrupt
  state) and SEG-007-T049/T052/T053 (extraction / oracle / comparison); the milestone was
  re-planned and those tasks did not land that work.
- **T129 could not own adding them.** Introducing interrupt state, a checkpoint-entry transition in
  the dispatcher, a new extraction API, and a new sanitized report surface is production-runtime
  ownership that was never assigned to T129, and doing it inside a checkpoint-binding task would have
  fused two unrelated concerns.
- **The independent oracle had no assignable owner.** T042 §16 named SEG-007-T052 as construction
  owner; T052 is `done`/terminal (its implementation spike was fully reverted). No independent
  adapter capable of deriving the **complete** bundle exists, and building one without a settled
  production schema and a settled independence boundary would risk violating §16's
  "complete independently-derived bundle" rule and §17's no-shared-semantics rule.

T129's disposition was `full_refinement_resolved` — "assign checkpoint-evidence/interrupt-state
runtime ownership and a contract-compliant independent-oracle boundary before retrying checkpoint
identity binding." This ADR is that assignment. **T129's ownership handoff is resolved here; the
checkpoint-identity binding itself is deferred to SEG-007-T132.**

## Decision

### 1. One production owner: the existing pure-C11 Genesis runtime translation unit

`runtime/genesis/runtime.h` + `runtime/genesis/runtime.c` — the unit that already owns
`GenesisRuntime`, `GenesisDeviceState`, `genesis_route_access`, `GenesisControlTransfer`,
`genesis_runtime_drive`, and the sanitized/full report functions — is the **sole** production owner
of the checkpoint-evidence runtime substrate. No new translation unit, no per-checkpoint module, and
no second dispatcher is created. SEG-007-T131 implements the following inside this one unit, exactly
per T042's already-committed schema:

1. **`GenesisInterruptState` as `GenesisDeviceState.interrupt`** (T042 §1.4 field list, §1.1
   extension discipline). Its fields are mutated **only** through the routed `genesis_*_access`
   dispatch boundary (T042 §2.1/§3) or a named post-access side effect / device-step-counter
   increment (T042 §4). No new mutation path, and no mutation timed by CPU instruction count, PC
   value, opcode, or polling-loop shape (T042 §4.2). An unsupported interrupt/status selector fails
   closed with the existing `GENESIS_STOP_UNSUPPORTED_*` pairing (T042 §10) — never a folded or
   guessed value.
2. **`GenesisCheckpointPcClass`** (T042 §12). Production defines **only**
   `GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN = 0`. T131 adds **no** title/Sonic enumerator. Exactly one
   non-`UNKNOWN` enumerator is appended later, by SEG-007-T132, and only if T132 can cite a
   non-reconstructable target class for it (Decision 6).
3. **The §13.1 checkpoint-entry transition mechanism**, evaluated **inside the existing typed
   control-transfer dispatcher path** (`genesis_runtime_drive` consuming `GenesisControlTransfer`).
   Concretely: one internal classification helper is invoked by the drive loop on each resolved
   control transfer's target; it performs a **pure target-address-class classification** of that
   target (the same static, compiled-in shape as `pc_class` itself), and on the first match to a
   non-`UNKNOWN` enumerator it sets `checkpoint_entered` `0 → 1` and snapshots
   `vblank_transition_count_at_checkpoint_entry` synchronously, once per run, sticky thereafter
   (T042 §13.1). This introduces **no second control-transfer mechanism** (T042 §7): it observes the
   target the existing dispatcher already resolved and never adds a parallel `next_pc`, a separate
   interrupt-dispatch function, or an ad hoc runtime PC/opcode/loop check. With only `UNKNOWN`
   defined, the helper is a no-op in production until T132 supplies the one class — that is the
   intended split (T131 builds the mechanism, T132 supplies the classification).
4. **`vblank_transition_count`** advances by exactly one at each `0 → 1` rising edge of
   `vblank_pending` (T042 §13.2), which is itself set only by a routed access / access-caused
   transition. T131 supplies the concrete VDP/interrupt-status register address and the bit meaning
   as a **bounded hardware fact** resolved from already-cited public documentation (GTO1), through
   the existing per-subsystem routed seam and the same explicitly-labelled project-compatibility-policy
   pattern SEG-007-T081/T102/T103/T109 already used for VDP/Z80/PSG state (T042 §2.4). If that fact
   is genuinely unresolvable from public evidence, T131 stops honestly without a speculative
   trigger; it does not invent a different rule for `vblank_transition_count` (T042 §13.2 fixes the
   edge arithmetic).
5. **`GenesisCheckpointEvidenceBundle` and `genesis_extract_checkpoint_evidence(...)`** (T042 §9,
   §12), verbatim to T042's schema and immutable frame shape (`GENESIS_FRAME_WIDTH = 320`,
   `GENESIS_FRAME_HEIGHT = 224`). Extraction is a pure, read-only copy of `GenesisRuntime`'s own
   fields plus the caller-supplied identity/transaction evidence — it executes no generated code,
   mutates nothing, and re-derives nothing (T042 §9). Extraction is attempted only once T042 §13's
   stable-frame condition holds; a run that never satisfies it yields no bundle and stops with its
   existing terminal `GenesisControlTransfer` — extraction is never forced.
6. **The canonical sanitized checkpoint-evidence report boundary.** A new function alongside the
   existing report functions (recommended name `genesis_write_checkpoint_evidence_summary`) emits
   **only** the T042 §18 committable summary: `checkpoint_id`, `schema_version`, `rom_sha256`,
   `pc_class`, per-category presence/provenance, per-category match booleans, and the overall
   PASS/FAIL verdict. **No digest** (`options_digest`, `work_ram_digest`, `transaction_digest`,
   `frame_digest`, `bundle_digest`) and **no raw** register / RAM / VRAM / CRAM / transaction /
   pixel / options value is ever written to this surface (T042 §18). The complete local bundle is
   written, if at all, only to an already-gitignored artifact for local inspection, mirroring
   `genesis_write_full_report`'s existing "local inspection only" precedent.

### 2. Preserved seams (restated so no successor re-litigates them)

- **Routed device-state seam.** `genesis_route_access` + the `genesis_is_*_region` /
  `genesis_*_access` per-subsystem helpers remain the one owner of device semantics (ADR 0003,
  T042 §2.1). Interrupt state is reached exclusively through this seam.
- **Typed control-transfer dispatcher.** `GenesisControlTransfer` / `GenesisDispatchFunction` /
  `genesis_runtime_drive` remain the one control-transfer mechanism (T042 §7). The §13.1 transition
  is an observation on top of it, not a new path.
- **Fail-closed default.** Every interrupt/VDP behavior T131 does not explicitly implement stays
  unconditionally fail-closed (ADR 0002, T042 §10).
- **No interpreter / JIT / runtime opcode decode / Sonic-specific branch.** The classification
  helper keys on a resolved target address class only; it never inspects an opcode, a poll shape, or
  a program-specific table.

### 3. What production does not own or decide

Device / CPU / VDP / DMA / timing / rendering-composition **content** semantics keep their existing
per-subsystem compatibility-policy owners, unchanged. This ADR adds no new device semantic. The
Sonic title-checkpoint target class is **not** decided here — it is SEG-007-T132's bounded,
authorized, sanitized job (Decision 6). ADR 0011's static-discovery ownership and SEG-007-T128's
discovery-prefix expansion are untouched.

### 4. One independent oracle owner: SEG-007-T132, in a non-production tree

T042 §16's oracle-construction ownership is reassigned from SEG-007-T052 to **SEG-007-T132**. The
oracle is a **separate host-side component that is never linked into, `#include`d by, or called from
any production target**. Recommended location: `tests/oracle/genesis/` (implementing task may choose
another path subject only to the "not under `src/`, `runtime/`, production `tools/` emitters, and not
in any production build target" constraint).

- **Single public entry point** (name normative in intent, exact signature T132's):
  `genesis_checkpoint_oracle_derive(const uint8_t *rom_image, size_t rom_len,
  GenesisDeterministicOptions options, GenesisCheckpointIdentity identity,
  GenesisCheckpointEvidenceBundle *bundle_out)`.
- **Input:** exactly the authorized hash-pinned ROM image bytes, plus the `GenesisDeterministicOptions`
  **value** (by value, not by digest — T042 §12.6/§16), plus the checkpoint identity. Nothing else.
- **Output:** exactly one complete `GenesisCheckpointEvidenceBundle` (CPU, RAM, device, transaction,
  frame), every field independently derived by the oracle's own computation — never copied,
  converted, or re-rendered from any state this project's own implementation under test produced
  (T042 §16 prohibition).
- **Construction approach.** Default is T042 §16(a): an existing, independently authored
  implementation of the relevant subsystem semantics invoked as an unmodified external tool, with a
  thin adapter into the §12 schema. The already-pinned local Musashi MC68000 oracle (operator
  context; `musashi-oracle-validation`) is the natural independent source for the CPU/RAM sub-bundle;
  device and frame sub-evidence require their own §16(a) source or, only with recorded justification
  in T132's Evidence, §16(b) new project-authored reference computation from independently cited
  public documentation. T132 states and justifies its choice per T042 §16.
- **Neutral-schema handoff (the only thing T131 and T132 share).** One pure-C11,
  types-and-constants-only header — recommended `runtime/genesis/checkpoint_evidence.h`, carved out
  of the runtime unit — containing the `GenesisCheckpointEvidenceBundle` struct family,
  `schema_version`, the fixed one-byte enum wire codes, and a pointer to T042 §14's canonical
  serialization/digest rules. It contains **no function definition with behavior**. Both the
  production extractor and the oracle compile against it. This is exactly T042 §17's permitted
  item 1 (neutral schema) and item 2 (independently cited public hardware constants) and item 3
  (test-vector file formats) — nothing else may be shared.

### 5. Independence audit (falsifiable requirements for SEG-007-T132)

T132's own tests must mechanically demonstrate all of:

1. **No shared translation unit / object.** The oracle build target links none of
   `runtime/genesis/runtime.o`, the MC68000 decode/lift/effects objects, the C11 codegen objects, or
   any generated-C object. Checked by build-graph / link-map inspection.
2. **No shared production symbol.** `nm` / link-map shows the oracle references no production
   `genesis_*` semantic function, no `m68k_*` semantic function, and no generated-C symbol.
3. **No production semantic `#include`.** A grep gate asserts the oracle sources include only the
   neutral schema header (Decision 4) from anywhere under `runtime/`, `src/`, or `include/`.
4. **No borrowed expected values.** Every expected value in an oracle test is a hand-derived literal
   or a value the oracle itself independently derived — never a value taken from a production run,
   from `genesis_extract_checkpoint_evidence`, or from generated-C output.
5. **The neutral schema header is inert.** A test asserts it declares only types, constants, and
   (optionally) `static inline` accessors with no subsystem semantics — no VDP/DMA/interrupt/CPU
   effect logic, no compositing, no expected-value generation.

The same audit obligation applies symmetrically to SEG-007-T131: its Evidence records that the
production runtime exports neutral schema data only and contains no oracle expected-value or
composition implementation.

### 6. Synthetic-fixture strategy (for SEG-007-T131 and SEG-007-T132)

- **Runtime side (T131).** Reuse T042 §11's host-test pattern: construct a `GenesisRuntime`, drive
  `genesis_route_access` / the dispatcher directly, assert on returned kind / `*value` / `stop` /
  resulting `devices` fields and the sticky transition fields with hand-derived literal
  expectations. Required cases: zero-initialization; exactly one valid `0 → 1` checkpoint transition
  plus snapshot; no snapshot overwrite on a later same-class transfer; complete-category extraction
  once the stable-frame condition holds; a fail-closed unsupported interrupt/status selector; and
  sanitized-summary output containing no digest and no raw value.
- **Oracle side (T132).** Project-authored, legally redistributable synthetic mini-ROM fixtures
  (hand-assembled 68000, synthetic tile/palette bytes for frame evidence) that exercise every bundle
  category deterministically, prove byte-identical output across repeated derivation, and include at
  least one adversarial-negative fixture (extraction/derivation must fail closed rather than emit a
  partial or guessed bundle; a deliberately divergent input must produce a per-category mismatch).
- **No commercial ROM, byte, word, address, offset, opcode, extension word, disassembly, access
  address, raw report, local path, trace, or framebuffer/pixel content** appears in any fixture,
  test assertion, commit message, PR description, or committed file (the project charter; ADR 0004; ADR 0005).

### 7. SEG-007-T132 checkpoint-classification recording rule

When T132 performs the authorized checkpoint-identity retry, its **durable** evidence may record
**only**: a project-chosen checkpoint ID (a short stable identifier, never a raw address); the one
non-`GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN` enumerator name it adds; a classification rule expressed
as an already-authorized generic structural / address-space-region bucket (the ADR 0004 precedent
this milestone already uses for opcode-line and region-class buckets); an explicit citation; and
confirmation that the entry event is exactly T042 §13.1's transition with no invented additional
trigger. It records **no** raw ROM-derived address, byte, opcode, offset, extension word,
disassembly, trace, local path, or frame pixel content anywhere — diff, commit message, PR
description, review comment, or committed file. If T132 cannot identify and safely sanitize a target
class after exhausting its authorized local diagnostic tooling, it stops without a weaker checkpoint
(e.g. the reset vector) and without any speculative production change, and records the precise
sanitized blocker (mirroring T129's honest stop).

### 8. Failure behavior (complete)

- Unsupported interrupt/VDP selector reached by the runtime → existing fail-closed
  `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` pairing.
- Stable-frame condition (T042 §13) never satisfied within the deterministic options' budget → no
  bundle; run ends with its existing terminal transfer; extraction never forced or faked.
- Over-capacity transaction list → T042 §12.4's truncation-from-transaction-evidence-only rule,
  unchanged.
- Oracle cannot derive a category → typed error return, never a partial or guessed bundle.
- T132's checkpoint-identity retry cannot cite a class → honest stop per Decision 7.
- Any independence-audit check (Decision 5) fails → a blocker for the owning task, never an
  acceptable shortcut (T042 §16/§17).

### 9. File / seam ownership map

| Artifact | Owner | Implemented by | Notes |
| --- | --- | --- | --- |
| `GenesisInterruptState` = `GenesisDeviceState.interrupt` | `runtime/genesis/runtime.h` | T131 | T042 §1.4 field list; mutated only via routed seam (§3/§4). |
| `GenesisCheckpointPcClass` (only `UNKNOWN`) | `runtime/genesis/runtime.h` (or the neutral header) | T131 | One non-`UNKNOWN` enumerator appended by T132 only (Decision 6/7). |
| §13.1 sticky checkpoint-entry transition + snapshot | `runtime/genesis/runtime.c`, inside `genesis_runtime_drive`'s dispatch loop | T131 | Pure target-address-class classification; no second control-transfer mechanism (§7). |
| `vblank_pending` / `vblank_transition_count` rising-edge rule | `runtime/genesis/runtime.c`, routed `genesis_*_access` | T131 | Concrete register address/bit is a bounded GTO1-cited fact (§2.4). |
| `GenesisCheckpointEvidenceBundle` family + `schema_version` + wire codes | `runtime/genesis/checkpoint_evidence.h` (new, inert) | T131 | The one artifact shared with the oracle (T042 §17 item 1). |
| `genesis_extract_checkpoint_evidence(...)` | `runtime/genesis/runtime.c` | T131 | Pure read-only extraction (T042 §9). |
| `genesis_write_checkpoint_evidence_summary(...)` (sanitized) + local full-bundle artifact | `runtime/genesis/runtime.c` | T131 | Committable summary only (T042 §18); full bundle to gitignored path. |
| Independent oracle (`genesis_checkpoint_oracle_derive` + adapter + tests) | `tests/oracle/genesis/` (non-production tree) | T132 | Links no production object; shares only the neutral header + independently cited constants (Decision 4/5). |
| One non-`UNKNOWN` checkpoint class + sanitized classification rule + §13.1 confirmation | T132 Evidence only | T132 | ADR 0004 generic-bucket recording discipline (Decision 7). |
| Final generated-native-vs-oracle comparison | — | SEG-007-T053 (unchanged) | Not T131 or T132. |

Must **not** be touched by T131/T132 per this ADR: MC68000 decode/lift/effects semantics or
addressing modes; ADR 0009's computed-indirect mechanism; ADR 0007's watchdog; ADR 0006's
cartridge-data read path; ADR 0011's static-discovery walker/expansion; the T042 §12–§15 bundle
schema, canonical serialization, digest rule, or exact-match comparison tolerances; the T042 §13
stable-frame condition; the immutable `GenesisFrameArtifact` shape; the existing wire-report schema
beyond the one additive sanitized checkpoint-evidence summary function.

## Consequences

- SEG-007-T131 becomes actionable: it implements Decision 1 against the seam map in Decision 9, with
  Decision 6's synthetic coverage and Decision 5's schema/ownership audit, and defers all
  checkpoint-identity and oracle work to T132.
- SEG-007-T132 becomes actionable once T131 lands: it builds the Decision 4 oracle against the
  neutral header, proves Decision 5 independence and Decision 6 fixtures, and performs the single
  authorized checkpoint-identity retry under Decision 7 — or stops honestly.
- SEG-007-T129's ownership handoff is resolved. The concrete checkpoint binding it could not make is
  now a well-owned, bounded task (T132), not an unresolved architecture fork.
- No production behavior changes as a result of this ADR; it is documentation only.

## Relationship to existing ADRs and contracts

- **T042 contract.** §1.4 / §12 / §13.1 / §9 ownership is pinned to `runtime/genesis/runtime.{h,c}`
  and its existing dispatcher; §16 construction ownership moves from SEG-007-T052 to SEG-007-T132;
  §17's permitted/forbidden sharing list is applied verbatim as the oracle boundary. Nothing else in
  T042 changes. A matching revision-history entry and a §16 pointer are added to that contract file.
- **ADR 0002 / ADR 0003.** Fail-closed execution and one-semantic-owner-per-fact are preserved: the
  substrate reuses the single routed device seam and the single typed dispatcher and adds no
  startup-only or checkpoint-only executor.
- **ADR 0004 / ADR 0005.** The sanitized checkpoint-evidence summary and T132's classification
  recording rule follow the established generic-bucket / non-reconstructable privacy discipline.
- **ADR 0011.** Not reopened. Static-discovery call/return ownership and checkpoint-directed
  expansion are unchanged; this ADR concerns only the runtime checkpoint-evidence substrate and the
  host-side oracle.

### Addendum (SEG-007-T050): post-T132 ownership handoff — historical record, not an amendment

This addendum records, as historical provenance only, the chain of ownership this ADR's Decisions 1–9
already established, now that SEG-007-T131 and SEG-007-T132 have both landed and SEG-007-T050 has
performed the next authorized step in that chain. It changes no Decision above; §12–§15's bundle
schema, §13's stable-frame condition, and Decision 4/5's oracle-independence boundary remain exactly
as decided.

- **SEG-007-T131** (PR #230, `done`) completed the **production substrate**: `GenesisInterruptState`,
  the always-`UNKNOWN`-only `GenesisCheckpointPcClass` mechanism, the §13.1 checkpoint-entry
  transition, `GenesisCheckpointEvidenceBundle` / `genesis_extract_checkpoint_evidence`, and the
  sanitized `genesis_write_checkpoint_evidence_summary` report boundary (Decision 1). At that point
  `GenesisFrameArtifact` was schema-complete but deliberately left inert (no renderer existed yet), and
  the summary function unconditionally forced `frame_present`/the overall verdict to reflect
  "not ready."
- **SEG-007-T132** (PR #232, `done`) completed the **independent oracle mechanism** (Decision 4/5) in
  `tests/oracle/genesis/`, plus attempted the Decision 6/7 authorized checkpoint-identity retry. It
  recorded its own honest stop: no non-`UNKNOWN` `GenesisCheckpointPcClass` enumerator could be safely
  bound and sanitized (Decision 7's "generic structural/address-space-region bucket, never a raw
  ROM-derived address" requirement was not met), so production's classifier remains `UNKNOWN`-only,
  exactly as T131 left it.
- **SEG-007-T050** (this task) owns the **production checkpoint-binding retry**: it wires T252's
  runner and T131's still-`UNKNOWN`-only classifier mechanism to T049's now-existing render/composition
  owner, so that once T042 §13's stable-frame condition holds, `genesis_extract_checkpoint_evidence`
  calls the real `genesis_vdp_produce_frame` and populates `bundle.frame` with a genuine artifact
  (previously permanently inert), and `genesis_write_checkpoint_evidence_summary` reports
  `frame_present`/`frame_state` from that bundle's own real population state instead of an always-false
  forced literal. This task also re-attempted Decision 6/7's checkpoint-class binding and records its
  own outcome (pass/stop) in its own Evidence — see SEG-007-T050's backlog record, not this ADR.
- **SEG-007-T053** (parent-milestone successor, not this task) remains the sole owner of the **final
  exact-subset independent completion/comparison** between a real generated-native run's bundle and
  T132's independent oracle's own bundle, per Decision 4/5's unchanged independence boundary.

No part of this addendum authorizes a different checkpoint-entry trigger, a different stable-frame
condition, a second renderer, or a new frame/comparison schema; each of those remains exactly as
Decision 1/4/5 and T042 §12–§15 already fixed.
