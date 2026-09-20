# Persistent Genesis device-state and checkpoint-evidence/frame-oracle contract (SEG-007-T042)

> SEG-007-T252 / ADR-0040 note: this contract's device-state/VBlank-scheduler/IRQ6/checkpoint-evidence
> content remains current. Its few references to the ADR 0007 generated-runtime no-progress/watchdog
> termination check describe superseded termination/progress policy: ADR-0040 removes that watchdog
> entirely and replaces it with a runner-owned finite dispatch allowance
> (`genesis_runtime_run`/`GENESIS_RUNNER_RESOURCE_LIMIT`). This document is not rewritten in full; read
> any remaining watchdog-check reference below as historical.

## Purpose and boundary

This is a documentation-only architecture contract. It defines the persistent-device-state seam and
the separate checkpoint-evidence/frame-oracle seam that later implementation tasks (SEG-007-T043,
T045, T047, T049, T052, T053) must each build through, precisely enough that none of them needs a
further architecture decision of its own. It introduces no code, fixture, or test, and changes no
`src/`, `include/`, `tests/`, `tools/`, or build-system file.

It consumes only already-committed evidence:

- `tools/genesis_startup_bridge_runtime.h`'s current, already-implemented shape: `GenesisRuntime`
  today has exactly `d[8]`, `a[8]`, `sr`, `pc`, and `work_ram[65536]` — no device-state field of any
  kind. `GenesisAccessWidth`, `GenesisAccessDirection`, `GenesisStopClass` (8 explicit enumerators),
  `GenesisDiagnosticCategory` (37 explicit enumerators, most recently `GENESIS_DIAG_KNOWN_BUT_
  UNEMITTED_TARGET = 37`), `GenesisControlTransferKind` (`GENESIS_CONTINUE_AT_PC = 0`, `GENESIS_STOP =
  1`, `GENESIS_COMPLETE = 2`), `GenesisControlTransfer`, `GenesisRuntimeStop`, `GenesisProvenance`, and
  the sole runtime memory/device boundary `genesis_route_access(GenesisRuntime *runtime, uint32_t
  address, GenesisAccessWidth width, GenesisAccessDirection direction, uint32_t *value,
  GenesisRuntimeStop *stop_out)` are unchanged by this contract.
- `tools/genesis_startup_bridge_runtime.c`'s current, already-implemented `genesis_route_access`
  behavior: it recognizes synthetic work RAM (`0x00FF0000` through `0x01000000`, exclusive, width-
  aware), ROM (`< 0x00400000`, write-prohibited, `GENESIS_DIAG_ROM_WRITE_PROHIBITED`), and exactly one
  device region, `genesis_is_device` (`0x00A10000 <= address < 0x00A10020`), routed through
  `genesis_controller_io_access`, which recognizes exactly two folded-constant selectors — the SEG-007-
  T020/T021 CTRL1-then-CTRL2 LONG read at `0x00A10008` and the SEG-007-T038 CTRL3 WORD read at
  `0x00A1000C`, both fixed value `0`, both side-effect-free — and fails closed
  (`GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO`)
  for every other in-region access. Every address outside the recognized work-RAM/ROM/device regions
  fails closed with `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` / `GENESIS_DIAG_UNMAPPED_DATA_ACCESS`.
  Today, `genesis_is_device` recognizes no Z80-bus, VDP, or other system-I/O address range: an access
  to any of those ranges currently falls through to the final unconditional
  `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` fail-close, exactly like any other unmapped address.
- [The generalized-startup runtime bridge contract](genesis-generalized-startup-runtime-bridge-contract.md)
  (SEG-007-T028), the direct structural/citation template this document follows: same directory, same
  section-numbering and concrete struct/field-level naming convention, same "written by this task,
  implemented by a later task" split, and the same `GenesisControlTransfer`/typed-stop dispatcher
  architecture this contract's interrupt boundary (§7) reuses rather than replaces.
- [The Genesis controller-I/O startup-read compatibility policy](genesis-controller-io-startup-read-compatibility-policy.md)
  (SEG-007-T020/T038): CTRL1/CTRL2/CTRL3's selected value and side-effect-free behavior is a bounded
  **project compatibility policy**, never a claim of proven, universal Genesis-hardware truth. This
  contract reuses that framing verbatim (§1.5) and does not migrate those selectors into the model
  defined here.
- [The Genesis startup shared-route ownership contract](genesis-startup-shared-route-ownership-contract.md)
  (SEG-005-T004): controller (and, by the same reasoning, every other device) semantics have exactly
  one ownership seam — `statically lifted CPU memory operation` → `Genesis bus/address resolver` →
  `device read/write` → `typed device result or typed fail-closed result` — and never an instruction-
  specific special case. This contract extends that seam to new device subsystems; it never
  relitigates or bypasses it.
- the SEG-007-T042 backlog record's Outcome
  (sixteen numbered elements), Scope, Non-goals, and Acceptance, which this document satisfies in full.

This contract deliberately omits every concrete Z80-bus address range, VDP control/data-port address,
TMSS/system-control register address, and VDP register/VRAM/CRAM/VSRAM/Z80-RAM byte size. No prior
`docs/architecture/*.md`, `docs/decisions/*.md`, or `docs/references/*.md` file pins any of those
facts today — only controller-I/O (`$A10008`–`$A1000D`) and the Version register (`$A10001`) are
already established, and this contract does not extend that established set. §2.4 explains this gap
and names, precisely, which later task's own Scope already requires it to supply each missing
citation. Every concrete address, register meaning, byte size, or timing value this contract's structs
reference is therefore a named placeholder a consuming task pins with its own citation or explicit
project-policy label — never a fact this document asserts on its own authority.

## Revision history

- **Interrupt-progression amendment (SEG-007-T047, ADR 0020).** In-place edits to §§3, 4, 7, and
  13.2 only; no schema, serialization, digest, comparison, tolerance, stable-frame, or
  checkpoint-evidence-bundle change (ADR 0020 §10 explicitly keeps its new production state out of
  §§12–18). §3 gains a third mutation category (a program-independent per-dispatch-step scheduling
  tick performed by `genesis_runtime_step` itself (SEG-007-T252 / ADR-0040 correction: originally
  attributed to the pre-T252 combined drive function; the tick is performed inside the guest-owned
  single-step contract, `genesis_runtime_step`, not the runner-owned repetition loop,
  `genesis_runtime_run`), touching only `vblank_pending`/
  `vblank_transition_count` on rollover). §4.1 gains the dispatch-step scheduling-tick primitive and
  §4.4 a worked counterexample for it. §7 gains a request-side clause (the scheduling tick may set
  the pending flag; admission is evaluated before the ADR 0007 watchdog check) and an amended
  acknowledgment-side rule: successful eligible IRQ6 admission clears `vblank_pending`, explicitly
  superseding §7's prior "never an implicit clear on delivery" restriction for the IRQ6/VBlank
  mechanism only. §13.2's rising-edge clause is amended to include the scheduling tick; its
  edge-triggering arithmetic is unchanged. See
  `docs/decisions/0020-deterministic-vblank-irq6-interrupt-progression.md`.
- **Ownership amendment (SEG-007-T130, ADR 0012).** No schema, serialization, digest, comparison,
  tolerance, or stable-frame change. Records the production owner SEG-007-T129 found unassigned:
  §1.4 `GenesisInterruptState` (as `GenesisDeviceState.interrupt`), §12 `GenesisCheckpointPcClass`
  and the `GenesisCheckpointEvidenceBundle` family, the §13.1 checkpoint-entry transition, and §9/§12
  extraction and the sanitized checkpoint-evidence summary are all owned by the existing pure-C11
  runtime unit `platforms/genesis/runtime/runtime.{h,c}` and its existing routed device seam
  (`genesis_route_access`) and typed control-transfer dispatcher (`genesis_runtime_run`),
  implemented by SEG-007-T131. §16's oracle-construction ownership moves from the now-terminal
  SEG-007-T052 to SEG-007-T132, which builds the oracle in a non-production tree against a neutral,
  behavior-free schema header and the §17 sharing rule. See
  `docs/decisions/0012-checkpoint-evidence-runtime-and-independent-oracle-ownership.md` for the
  file/seam ownership map, independence-audit requirements, synthetic-fixture strategy, and the
  sanitized checkpoint-classification recording rule those two tasks consume.
- **Correction round.** Resolves six blocking gaps identified in independent adversarial review of
  the initial delivery, all in §§12–18 (the checkpoint-evidence half of this contract; §§1–11's
  persistent device-state model is unchanged): (a) the 256-entry transaction-list capacity versus a
  `uint8_t transaction_count` overflow, by widening `transaction_count` to `uint16_t` and adding an
  explicit field-wise canonical encoding for `GenesisBusAccess`, including the `raw_byte_count`-first
  reordering required to bound `raw_bytes`, and explicit full-list-versus-digest-only encoding modes
  for `GenesisTransactionEvidence` (§14); (b) replacing the prior "serialized as `int`, per C11 enum
  representation" enum rule, which is not portable across conforming C11 implementations, with a fixed
  one-byte wire code per enumerator's explicitly assigned value, named for every enum type appearing
  in the bundle (§14 rule 3); (c) pinning `GENESIS_FRAME_WIDTH`/`GENESIS_FRAME_HEIGHT` as immutable for
  every consuming task, removing the prior wording that let SEG-007-T049 adjust them (§12.5); (d)
  adding a complete, versioned `GenesisDeterministicOptions` schema, carried by value (not merely by
  digest) in `GenesisCheckpointIdentity.options`, so the independent oracle can reproduce a run rather
  than only detect that it differs (§12, §12.6, §16); (e) making the bounded stable-frame condition
  operationally complete: a named checkpoint-entry event and counter snapshot
  (`vblank_transition_count_at_checkpoint_entry`, §1.4), and a precise, program-independent,
  access-caused rising-edge rule for when `vblank_transition_count` itself advances (§13); and (f)
  narrowing §18's committed-evidence allowance so no per-category or bundle digest may be committed
  without an existing, separately established policy naming that exact field (`rom_sha256` remains the
  sole existing exception, for its own already-established ingestion-identity reason), leaving only
  already-approved identifiers/classifications, presence/provenance fields, per-category match
  booleans, and the overall verdict committable. A fresh independent `adversarial-validator` review of
  the corrected document (see this record's Evidence) found no remaining blocker.
- **Initial delivery.** Defines all sixteen Outcome elements: (1) a persistent device-state model
  (`GenesisDeviceState`, owned by one new `GenesisRuntime.devices` field) covering Z80-bus, VDP, and
  interrupt state; (2) a typed per-subsystem dispatch extension to `genesis_route_access` that
  authorizes no new device semantics by itself; (3) mutation/commit rules restricting persistent-state
  writes to the routed dispatch boundary or an explicitly named post-access side effect; (4)
  deterministic device advancement expressed only as address-based dispatch, access-caused state
  transition, or the named device-step counter — never CPU program-counter, opcode, or polling-loop
  recognition, with a concrete worked counterexample; (5)–(7) status-register, DMA, and interrupt
  semantics, the last reusing the existing `GenesisControlTransfer`/dispatcher architecture rather than
  a second control-transfer mechanism; (8) reset initialization; (9) snapshot/frame-extraction
  mechanics; (10) an unconditional fail-closed default for every unimplemented device behavior; (11)
  host-side synthetic test access that reuses the exact production model and dispatch boundary,
  never a duplicate semantic implementation; and, as a fully separate concern, (12)–(16) the complete
  checkpoint-evidence bundle schema (CPU, RAM, device, transaction, and nested frame-artifact evidence,
  checkpoint identity/options, and privacy fields), the bounded stable-frame condition, canonical
  serialization and deterministic digest rules for the complete bundle, comparison rules and
  tolerances per evidence category, and the independent-oracle method, construction-ownership
  assignment (SEG-007-T052), and oracle-independence permitted-versus-forbidden sharing rule. This is
  the first delivery of this document; no prior round exists to reconcile.

## 1. Persistent device-state model

### 1.1 `GenesisDeviceState` composition and the `GenesisRuntime` field addition

`GenesisRuntime` gains **exactly one** new field. It is not flattened into `GenesisRuntime` itself,
and no existing field's meaning, offset expectations, or initializer pattern changes:

```c
typedef struct GenesisRuntime {
  uint32_t d[8];
  uint32_t a[8];
  uint16_t sr;
  uint32_t pc;
  uint8_t  work_ram[65536];
  GenesisDeviceState devices;   /* new: SEG-007-T042. Zero-initialized identically to every other
                                    GenesisRuntime field (see §8). */
} GenesisRuntime;
```

`GenesisDeviceState` is a small, named composition of one nested struct per genuinely stateful
subsystem this milestone's remaining Scope needs — never one flat bag of unrelated fields:

```c
typedef struct GenesisDeviceState {
  GenesisZ80BusState    z80_bus;    /* SEG-007-T043 */
  GenesisVdpState        vdp;        /* SEG-007-T045 */
  GenesisInterruptState  interrupt;  /* SEG-007-T047 */
} GenesisDeviceState;
```

No other subsystem is added by this contract. A later task that needs to add a fourth genuinely
stateful subsystem this milestone did not anticipate must extend `GenesisDeviceState` with one more
named nested struct, following this same composition discipline, and must report the gap rather than
force its state into an existing nested struct that does not own it — matching this project's "add
abstractions only when a current target exercises them" rule (the project charter, Scope Discipline).

### 1.2 `GenesisZ80BusState`

```c
typedef struct GenesisZ80BusState {
  uint8_t  bus_requested;              /* 1 iff the 68000 has asserted BUSREQ and it has not yet been
                                           released; 0 otherwise. Boolean-valued (0/1), never a raw
                                           register bit pattern this contract has not cited. */
  uint8_t  bus_granted;                /* 1 iff the Z80 bus is currently granted to the 68000 side per
                                           whatever documented or policy-defined condition SEG-007-T043
                                           cites; 0 otherwise. */
  uint8_t  reset_asserted;             /* 1 iff the Z80 RESET line is currently held asserted by the
                                           68000 side; 0 otherwise. */
  uint32_t busreq_status_read_count;   /* Named device-step counter (§4.3): incremented by exactly one
                                           on every access to the documented BUSREQ/status register,
                                           keyed by that register address alone -- never by CPU PC,
                                           opcode, or instruction identity. */
  uint8_t  z80_ram[GENESIS_Z80_RAM_BYTES];   /* GENESIS_Z80_RAM_BYTES is a named placeholder; SEG-007-
                                                 T043's own Scope already requires it to cite the public
                                                 documented Z80 RAM window size before defining this
                                                 macro's value (see §2.4). This contract asserts no size
                                                 of its own. */
} GenesisZ80BusState;
```

### 1.3 `GenesisVdpState`

```c
typedef enum GenesisVdpDmaPhase {
  GENESIS_VDP_DMA_IDLE = 0,
  GENESIS_VDP_DMA_BUSY = 1,
} GenesisVdpDmaPhase;

typedef struct GenesisVdpDmaState {
  GenesisVdpDmaPhase phase;
  uint32_t source_address;          /* Meaning (VRAM offset, 68000 address, or other) is SEG-007-T045's
                                        own cited-or-policy-labeled decision; this field only reserves
                                        the seam. */
  uint32_t remaining_length;
  uint32_t transfer_access_count;   /* Named device-step counter (§4.3) for DMA-busy progression, used
                                        only if SEG-007-T045's own policy needs one (§6). */
} GenesisVdpDmaState;

typedef struct GenesisVdpState {
  uint16_t registers[GENESIS_VDP_REGISTER_COUNT];   /* GENESIS_VDP_REGISTER_COUNT is a named
                                                         placeholder; SEG-007-T045 cites the documented
                                                         register count before defining this macro's
                                                         value (§2.4). */
  uint8_t  control_port_awaiting_second_word;   /* 1 iff a first control-port word has been received
                                                    and this state is awaiting the second word of a
                                                    documented two-word command; 0 otherwise. */
  uint16_t control_port_first_word;
  uint32_t addressed_pointer;        /* Current VRAM/CRAM/VSRAM address the data port reads/writes,
                                         per the documented control-port command that set it. */
  uint16_t auto_increment_value;
  uint8_t  vram[GENESIS_VDP_VRAM_BYTES];     /* Named placeholder; SEG-007-T045 cites the documented
                                                 size (§2.4). */
  uint8_t  cram[GENESIS_VDP_CRAM_BYTES];     /* Named placeholder; SEG-007-T045 cites the documented
                                                 size (§2.4). */
  uint8_t  vsram[GENESIS_VDP_VSRAM_BYTES];   /* Named placeholder; SEG-007-T045 cites the documented
                                                 size (§2.4). */
  uint16_t status_register;          /* Observed per §5; mutated only per §5's rules. */
  GenesisVdpDmaState dma;
} GenesisVdpState;
```

### 1.4 `GenesisInterruptState`

```c
typedef struct GenesisInterruptState {
  uint8_t  vblank_pending;             /* 1 iff a VBlank interrupt request is currently pending
                                           delivery; cleared per whichever documented or policy-defined
                                           acknowledgment SEG-007-T047 implements (§7). */
  uint32_t vblank_status_read_count;   /* Named device-step counter (§4.3) for the VDP status
                                           register's VBlank-pending observation, used only if
                                           SEG-007-T047's own policy needs one. */
  uint32_t vblank_transition_count;    /* Named device-step counter (§4.3). Incremented by exactly one
                                           at each 0->1 rising edge of vblank_pending above -- never on
                                           a 1->1 no-op observation, a 1->0 acknowledgment, a raw CPU
                                           instruction count, a PC value, or an opcode. §13.2 makes this
                                           the complete, operational definition of "VBlank-onset state
                                           transition." §13's stable-frame condition is expressed
                                           entirely in terms of this field and the next one. */
  uint8_t  checkpoint_entered;         /* 0 until §13.1's checkpoint-entry event has occurred exactly
                                           once for this run; 1 thereafter (sticky -- never cleared
                                           again). Guards vblank_transition_count_at_checkpoint_entry
                                           below: that field is meaningful only once this is 1. */
  uint32_t vblank_transition_count_at_checkpoint_entry;   /* A one-time snapshot of
                                           vblank_transition_count, taken synchronously at the exact
                                           moment checkpoint_entered above transitions from 0 to 1
                                           (§13.1), and never written again afterward. 0 (its
                                           zero-initialized value, §8) is not itself meaningful while
                                           checkpoint_entered is still 0; a consumer reads this field
                                           only after observing checkpoint_entered == 1. */
} GenesisInterruptState;
```

### 1.5 CTRL1/CTRL2/CTRL3 explicit non-migration statement

**CTRL1, CTRL2, and CTRL3 are not migrated into this model.** They remain exactly the existing
folded-constant selectors `genesis_controller_io_access` already implements
(`tools/genesis_startup_bridge_runtime.c`), governed exactly as before by [the controller-I/O
compatibility policy](genesis-controller-io-startup-read-compatibility-policy.md) (SEG-007-T020/T038):
a deterministic value and side-effect-free behavior selected for those exact selectors as a bounded
**project compatibility policy**, never a claim of proven, universal Genesis-hardware truth. No struct
in §1.1–§1.4 represents controller state, and no dispatch rule in §2 routes CTRL1/CTRL2/CTRL3 through
`GenesisDeviceState`. This restates, and does not narrow or contradict, the same statement already
made in this document's Purpose section above.

## 2. Typed runtime bus/device dispatch extension

### 2.1 Per-subsystem routing-helper pattern extending `genesis_route_access`

`genesis_route_access`'s existing signature, boundary shape, and precedence order (24-bit-address
check, then alignment check, then work-RAM, then ROM, then device) do not change. Each new subsystem
is reached by exactly one new routing helper, kept in the same pure-C11 runtime translation unit,
mirroring `genesis_is_device`/`genesis_controller_io_access`'s existing shape:

```c
static int genesis_is_z80_bus_region(uint32_t address);
static int genesis_z80_bus_access(GenesisDeviceState *devices, uint32_t address,
                                   GenesisAccessWidth width, GenesisAccessDirection direction,
                                   uint32_t *value);

static int genesis_is_vdp_region(uint32_t address);
static int genesis_vdp_access(GenesisDeviceState *devices, uint32_t address,
                               GenesisAccessWidth width, GenesisAccessDirection direction,
                               uint32_t *value);

static int genesis_is_system_io_region(uint32_t address);   /* covers the Version register and any
                                                                 TMSS-style system-control register
                                                                 SEG-007-T043 implements */
static int genesis_system_io_access(GenesisDeviceState *devices, uint32_t address,
                                     GenesisAccessWidth width, GenesisAccessDirection direction,
                                     uint32_t *value);
```

`genesis_route_access` calls each `genesis_is_*_region` predicate in the same position
`genesis_is_device` occupies today, after the existing ROM check and before the final unconditional
`GENESIS_DIAG_UNMAPPED_DATA_ACCESS` fail-close; on a recognized region it calls the matching
`*_access` function, exactly mirroring `genesis_controller_io_access`'s existing "recognized region,
selector-level success-or-fail-closed" shape. Each predicate and access function takes
`GenesisDeviceState *`, not the whole `GenesisRuntime *`, so each subsystem's dispatch code can be
authored, reviewed, and tested (§11) without visibility into unrelated runtime fields. This contract
does not decide these predicates' or region ranges' concrete address bounds (§2.4); it decides only
this dispatch shape and ownership.

### 2.2 New `GenesisDiagnosticCategory` enumerators (naming pattern, not literal values)

Three new named categories are reserved by name, mirroring `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_
CONTROLLER_IO`'s existing pattern exactly, each pairing with the existing `GENESIS_STOP_UNSUPPORTED_
DEVICE_ACCESS` stop class:

```c
/* GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_SYSTEM_IO */
/* GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS */
/* GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP */
```

This contract does not itself assign these three names integer enumerator values. Per this project's
"add abstractions only when a current target exercises them" rule, the task that actually implements
each corresponding region (SEG-007-T043 for `SYSTEM_IO` and `Z80_BUS`, SEG-007-T045 for `VDP`) appends
its category as the next unused integer in `GenesisDiagnosticCategory`'s existing sequence (currently
ending at `GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET = 37`) at the point it is actually added, exactly as
every prior category in that enum was added by the task that first needed it.

### 2.3 "Defining this surface authorizes no new device semantics" statement

Defining §2.1's dispatch shape and §2.2's category names in this document does not, by itself,
authorize a single new device semantic, folded value, or side effect of any kind. Until a specific
later task actually implements a given `genesis_is_*_region`/`*_access` pair, every access an
in-scope region receives — indeed, every access to every address this contract names, since none of
these predicates exist in `tools/genesis_startup_bridge_runtime.c` until that task adds them — remains
exactly as unconditionally fail-closed as it is today: it falls through to the same final
`GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` / `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` result every unmapped
address already receives. Once a predicate exists, every access it recognizes but whose selector its
own `*_access` function does not implement remains fail-closed with that region's own
`GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_*` pairing — never
a silent success, folded default, or guessed value. §10 restates this as the general fail-closed
default.

### 2.4 The hardware-fact gap this contract deliberately leaves open

No committed `docs/architecture/*.md`, `docs/decisions/*.md`, or `docs/references/*.md` file pins the
Z80-bus address range, the VDP control/data-port addresses, the TMSS/system-control register address,
or the VDP register/VRAM/CRAM/VSRAM/Z80-RAM byte sizes referenced symbolically in §1.2–§1.3 and §2.1
above. Only controller-I/O (`$A10008`–`$A1000D`) and the Version register (`$A10001`) are already
established, by the controller-I/O compatibility policy and by `GTO1` p. 72 §1 as cited in SEG-007-
T043's own Scope. This contract deliberately states every other address, register meaning, and byte
size only as a named placeholder (`GENESIS_Z80_RAM_BYTES`, `GENESIS_VDP_REGISTER_COUNT`, `GENESIS_VDP_
VRAM_BYTES`, `GENESIS_VDP_CRAM_BYTES`, `GENESIS_VDP_VSRAM_BYTES`, and the unnamed region bounds
`genesis_is_z80_bus_region`/`genesis_is_vdp_region`/`genesis_is_system_io_region` test) rather than
asserting a concrete value under its own authority — exactly as `genesis_is_device`'s `0x00A10000`–
`0x00A10020` interval was pinned only once the controller-I/O compatibility policy already existed to
justify it, never invented ahead of that policy.

Each consuming task's own Scope already requires it to supply the citation this contract omits:
SEG-007-T043 (`GTO1` p. 72 §1 and p. 82) for the Version register, the TMSS-style system-control
register, and the Z80-bus/BUSREQ/RESET addresses and Z80 RAM window size; SEG-007-T045 for the VDP
control/data-port addresses and the VDP register/VRAM/CRAM/VSRAM sizes; SEG-007-T047 for the VBlank/
interrupt-status register address and bit meaning. This contract makes no claim, hint, or estimate
about any of these values; it only reserves the named seam each of those tasks fills with its own
independently cited fact or explicitly labeled project compatibility policy, per §2.3.

## 3. Mutation/commit rules

Persistent device state (`GenesisRuntime.devices`, every field in §1.2–§1.4) may be mutated **only**
by:

1. A routed access through §2.1's dispatch boundary — i.e., inside a `genesis_*_access` function
   called from `genesis_route_access`, in direct response to the one access it was called to service.
2. An explicit, deterministic post-access side effect this contract (or a later task's own contract-
   compliant extension of it, per §1.1's extension rule) names by field and by triggering condition —
   for example, §4.3's device-step counter increment, or a documented register write that also clears
   a status bit as part of its own write semantics (a fact SEG-007-T045 cites, not this contract).
3. **(SEG-007-T047 / ADR 0020; ownership corrected SEG-007-T252 / ADR-0040)** An explicit,
   deterministic, per-dispatch-step scheduling tick performed directly by `genesis_runtime_step`
   itself — never by a generated block, never by `genesis_route_access`, and never by
   `genesis_runtime_run` (the runner-owned finite dispatch allowance, which only repeats
   `genesis_runtime_step`) — independent of any particular bus access (it is driven by the
   dispatch-step count alone, and has no associated access). The counter this tick advances
   is `GenesisRuntime`-level production-runtime state declared **outside `GenesisDeviceState`
   entirely** (§1.1's `GenesisRuntime` composition is unchanged by this contract; ADR 0020 places the
   new `GenesisInterruptScheduler` field as a sibling of `devices`, mirroring
   `GenesisRuntime.loop_progress`/`data_progress`); it is not a `GenesisDeviceState`/evidence-bearing
   field and this rule does not make it one. The **only** thing this category may do to
   `GenesisDeviceState` proper is, on rollover of that `GenesisRuntime`-level counter (and only when
   §13.2's VDP interrupt-enable gating permits), set the two named interrupt-device fields
   `vblank_pending` (0→1) and `vblank_transition_count` (§4-amended, §13.2). It never generalizes to
   "the runtime may mutate device state for any reason" — only those two named fields, only via
   rollover of the `GenesisRuntime`-level counter, only at the named point (ADR 0020 §5).

No other code path may write `GenesisRuntime.devices`. In particular: no implicit mutation from an
access to an unrelated address or region; no mutation as a side effect of dispatch-loop bookkeeping
(**other than rule 3's explicit, named, program-independent scheduling tick, which touches only
`vblank_pending`/`vblank_transition_count` on rollover**), report generation, or diagnostics; no
mutation timed by elapsed CPU instruction count, PC value, or opcode (§4.2); and no mutation
performed directly by generated C outside the `genesis_*_access`
functions this dispatch boundary defines — generated C reaches device state exclusively through
`genesis_route_access`, exactly as it already does for work RAM and controller I/O today.

## 4. Deterministic device advancement/scheduling

### 4.1 Allowed

- Ordinary bus dispatch by documented device-register address — the same address-based routing
  `genesis_route_access`/`genesis_controller_io_access` already legitimately perform for every existing
  selector, extended verbatim to each new region's own selectors (§2.1).
- A state transition caused by a documented `BUSREQ`/`RESET`/VDP/status-register **access itself**: the
  state changes because that device address was accessed, per its own documented or policy-defined
  semantics — never because of who accessed it, from where, or why.
- The **named device-step counter**: a per-device-register `uint32_t` counter field, owned by the
  persistent device-state model (§1.2's `busreq_status_read_count`, §1.3's `transfer_access_count`,
  §1.4's `vblank_status_read_count`/`vblank_transition_count`, and any equivalent field a later task
  adds under §1.1's extension rule), incremented by exactly one, inside the routed `genesis_*_access`
  function, on every access (read or write, as the specific consuming policy names) to that one
  documented register — never on an access to any other address, and never conditioned on CPU PC,
  opcode, or the instruction that performed the access. This counter is the sole primitive a later task
  may use to express a bounded "after N accesses" progression rule (§4.3).
- **(SEG-007-T047 / ADR 0020) The dispatch-step scheduling tick**, distinct from the device-step
  counter above: a counter advanced by exactly one per `genesis_runtime_step` call (SEG-007-T252 /
  ADR-0040 correction: not `genesis_runtime_run`, which only repeats `genesis_runtime_step` up to its
  own finite allowance) — never per CPU instruction, never keyed to a PC value / opcode / instruction
  identity, and never counting a specific register's accesses — owned by `GenesisRuntime` itself rather than by
  `GenesisDeviceState` (per §3 rule 3). Its rollover at a fixed bounded period may set exactly the
  two named interrupt-device fields `vblank_pending` (0→1) and `vblank_transition_count`, and only
  when §13.2's VDP interrupt-enable gating condition permits. This is a uniform, program-independent
  scheduler applied identically to every generated program; it recognizes nothing about the running
  program.

### 4.2 Forbidden

- Recognizing a particular program-counter value as significant to device behavior.
- Recognizing a particular opcode or instruction sequence as significant to device behavior.
- Detecting a Sonic-specific, or any other program-specific, polling-loop shape.
- Skipping or forcing control flow based on program identity rather than on the bus access itself.

### 4.3 The named device-step counter primitive

A device-step counter is declared, per register, as a `uint32_t` field on the owning nested struct
(§1.2–§1.4). `genesis_route_access`'s routed `genesis_*_access` function increments it by exactly one,
unconditionally, on every access matching that exact documented register address and the specific
direction(s) the consuming policy names (read-only, write-only, or both) — the increment happens
purely because that address was accessed at all, with no inspection of `runtime->pc`, the calling
instruction's opcode, or any other CPU-identity fact. A later task's progression rule (for example,
"BUSACK becomes granted once `busreq_status_read_count` reaches N") reads this counter's current value
and compares it to a bounded constant that same task names and justifies (as a cited timing fact or an
explicitly labeled project compatibility policy, per §2.4) — it never reads or branches on CPU state to
decide when to advance.

### 4.4 Worked counterexample

- **Forbidden:** "Grant BUSACK only after the CPU executes a `TST.B` on the BUSREQ status address in a
  loop." This inspects the *instruction* performing the access (its opcode, `TST.B`) and its repetition
  pattern (a loop), not merely the fact that the address was accessed — exactly the polling-loop-shape
  and opcode recognition §4.2 forbids.
- **Allowed:** "Grant BUSACK after the Nth documented access to the BUSREQ status address, regardless
  of which instruction performed it." This uses only §4.3's device-step counter
  (`busreq_status_read_count`) reaching a bounded, named constant N — the same architecture guardrail
  this milestone already applies to controller semantics (no instruction-specific special case),
  extended here explicitly to timing/progression.
- **(SEG-007-T047 / ADR 0020) Forbidden:** "Raise `vblank_pending` after Sonic's wait loop has spun
  N times." This recognizes program repetition (a specific loop shape and its iteration count) —
  exactly the polling-loop-shape recognition §4.2 forbids. **Allowed:** "Raise `vblank_pending`
  after N `genesis_runtime_step` calls, unconditionally, identically for every generated program
  (SEG-007-T252 / ADR-0040 correction: the tick is performed inside `genesis_runtime_step`, not
  `genesis_runtime_run`)." This uses only §4.1's dispatch-step scheduling tick — a uniform,
  program-independent counter of the dispatcher's own iterations, with no inspection of the running
  program at all.

## 5. Status-register observation semantics

A status register (the VDP status register, §1.3's `status_register`; the Z80 BUSREQ/RESET status bits
implicit in §1.2's `bus_requested`/`bus_granted`/`reset_asserted`) is read through §2.1's dispatch
boundary exactly like any other device register: the routed `genesis_*_access` function computes the
current bit pattern from `GenesisDeviceState`'s current fields, returns it as `*value`, and — per §4.1
— increments that register's own device-step counter if the consuming task's policy names one for it.
A status-register **read** may clear a documented latched bit as part of the read's own documented
semantics (an explicit, named post-access side effect under §3 rule 2) only if the consuming task
cites that clear-on-read behavior as a public fact or labels it an explicit project compatibility
policy; absent such a citation or label, a status-register read is side-effect-free beyond its own
device-step-counter increment, mirroring the controller-I/O policy's existing "side-effect-free unless
otherwise established" default. A status register is never writable through this dispatch boundary
unless a consuming task's own cited or policy-labeled semantics say so explicitly; an unrecognized
write to a status-register address fails closed exactly as any other unsupported selector does (§2.3).

## 6. DMA state-transition semantics

DMA state (`GenesisVdpDmaState`, §1.3) transitions **only** through routed accesses to the documented
VDP registers/ports that control it (§2.1), never through an internal scheduler unrelated to bus
access:

- A documented control-port command or register write that starts a DMA transfer moves `phase` from
  `GENESIS_VDP_DMA_IDLE` to `GENESIS_VDP_DMA_BUSY` and sets `source_address`/`remaining_length` from
  the documented register fields that encode them (SEG-007-T045 cites which fields).
- While `phase == GENESIS_VDP_DMA_BUSY`, an access this contract's consuming task names as
  DMA-progressing (for example, a data-port access, or the device-step counter `transfer_access_count`
  reaching a named bounded value per §4.3) decrements `remaining_length` and, when it reaches zero,
  transitions `phase` back to `GENESIS_VDP_DMA_IDLE`. This transition is caused by the documented
  access(es) themselves or by the named device-step counter — never by elapsed CPU instructions, a PC
  value, or an opcode, matching §4.1/§4.2 exactly.
- A status-register read (§5) may observe `phase` (for example, as a documented "DMA busy" status bit)
  but never mutates it; only the rules above mutate `phase`.
- An access to a VDP register unrelated to the active DMA transfer while `phase == GENESIS_VDP_DMA_
  BUSY` is either explicitly permitted (and its own effect defined) or explicitly rejected by the
  consuming task's own cited-or-policy-labeled semantics — this contract does not decide that
  interaction itself; until a task decides it, such an access remains whatever §2.3's fail-closed
  default already produces for an unimplemented selector.

## 7. Interrupt request/delivery boundary

A device raises an interrupt request by setting a pending flag on `GenesisInterruptState` (§1.4;
`vblank_pending`, or an equivalent field a later task adds under §1.1's extension rule) through §2.1's
dispatch boundary, §4.1's access-caused-transition rule, or **(SEG-007-T047 / ADR 0020) the
§4-amended dispatch-step scheduling tick** — never by directly manipulating generated C's control
flow.

**(SEG-007-T047 / ADR 0020) Where admission is evaluated.** Interrupt admission is evaluated inside
`genesis_runtime_step` (SEG-007-T252 / ADR-0040 correction: not `genesis_runtime_run`, which only
repeats `genesis_runtime_step`), historically strictly before the now-removed ADR 0007 no-progress/
watchdog-termination check for that same dispatch step (ADR 0020 §5 gives the exact ordering and why
it was load-bearing; ADR-0040 retired the watchdog check itself, not this ordering). Delivery still
reuses `GenesisControlTransfer`/`genesis_runtime_step` with no second control-transfer mechanism.

Generated C observes and handles a pending interrupt request **exclusively** through the existing
`GenesisControlTransfer`/`genesis_dispatch`-style typed-stop/dispatcher architecture
(`tools/genesis_startup_bridge_runtime.h`'s `GenesisControlTransferKind`, `GenesisControlTransfer`,
and the generated `dispatch(...)` function this project's structured-C backend already emits, per [the
generalized-startup runtime bridge contract](genesis-generalized-startup-runtime-bridge-contract.md)).
A pending interrupt is delivered as an ordinary `GENESIS_CONTINUE_AT_PC` transfer to the documented
interrupt-handler entry point the consuming task cites or the finite dispatcher's own generated table
already resolves — reusing exactly the same `next_pc`-carrying transfer shape every other control
transfer already uses. This contract introduces **no second control-transfer mechanism**: no separate
interrupt-dispatch function, no parallel `next_pc` field, and no bypass of `genesis_runtime_run`'s
existing finite-dispatch loop.

Acknowledgment (clearing `vblank_pending`) — **amended by SEG-007-T047 / ADR 0020.** Successful
eligible IRQ6 admission (ADR 0020 §5 step 2) consumes the pending VBlank request and clears
`vblank_pending = 0` as part of delivery itself, as one of ADR 0020's own explicitly named
interrupt-state mutation sources (the §3-amended list). A masked/not-admitted interrupt (ADR 0020 §5
step 3) leaves `vblank_pending` set, and it is then unclearable by anything else. **This
admission-clears rule explicitly supersedes this section's prior "never an implicit clear on
delivery" restriction, for the IRQ6/VBlank mechanism only** — it does not reopen or apply to any
other, unrelated acknowledgment path this contract separately defines (for those paths, acknowledgment
still happens only as an explicit, named post-access side effect of a documented acknowledgment
register access, exactly as §3 rule 2 and §5 require). A future reader must not cite the superseded
prior wording against ADR 0020's design.

## 8. Reset initialization of persistent device state

`GenesisDeviceState` is zero-initialized at program start, mirroring the existing `d[]`/`a[]`/`pc`/
`work_ram` literal-initializer pattern already present in generated `main`
(`tools/genesis_startup_bridge_runtime.c`/the structured-C backend's existing zero-initialized
`GenesisRuntime runtime = {0};` pattern): every integer field is `0`, every enum field is its `0`-valued
enumerator (`GENESIS_VDP_DMA_IDLE = 0`), and every array is all-zero bytes. This is the sole reset rule
this contract defines. If a consuming task's cited public documentation establishes a non-zero
power-on/reset value for a specific field (for example, a documented non-zero Version-register reset
value), that task states the deviation explicitly, citing its source, and documents it as an addition
to — never a silent override of — this default; absent such a citation, zero-initialization is the
contract-mandated default for every field this document defines.

## 9. Snapshot/frame-extraction mechanics

A later task reads a point-in-time value from `GenesisDeviceState` by taking a **read-only copy** of
the exact fields it needs — never a second, parallel representation. Concretely: `GenesisVdpState.
vram`/`cram`/`vsram`/`registers` are read directly (a `memcpy` of the exact bytes `GenesisRuntime.
devices.vdp` currently holds) to build the frame artifact defined in §12.5; `GenesisCpuEvidence`,
`GenesisRamEvidence`, and `GenesisDeviceEvidence` (§12.1–§12.3) are populated the same way, directly
from `GenesisRuntime`'s own `d`/`a`/`sr`/`pc`/`work_ram`/`devices` fields. This extraction never
executes generated code, never mutates `GenesisRuntime`, and never re-derives a value some other means
already computed differently — it is a pure, deterministic read of the single production state. §12
below names this operation `genesis_extract_checkpoint_evidence` and specifies its exact output shape.

## 10. Fail-closed default for unimplemented device behavior

Every device behavior not explicitly implemented by a specific later task remains an unconditional
default identical in spirit to today's `genesis_route_access` fail-close: an unrecognized region falls
through to `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION`/`GENESIS_DIAG_UNMAPPED_DATA_ACCESS` exactly as
today; a recognized region's unimplemented selector fails with that region's own `GENESIS_STOP_
UNSUPPORTED_DEVICE_ACCESS`/`GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_*` pairing (§2.2–§2.3); no width,
direction, register shape, or access pattern this contract or a consuming task's policy does not
explicitly name ever silently succeeds, folds to a guessed value, or produces a side effect. This
applies identically to the new model defined here and to the existing work-RAM/ROM/controller-I/O
model it extends — one uniform fail-closed discipline, never a second, looser one for new subsystems.

## 11. Host-side synthetic test access without a second semantic implementation

Tests observe and drive persistent device state by calling `genesis_route_access` **directly**, against
a real `GenesisRuntime` instance, exactly as the existing controller-I/O regression already does
(`tests/genesis_startup_runtime_controller_io_test.py`, which compiles a small C harness `#include`ing
`genesis_startup_bridge_runtime.h` and asserts on `genesis_route_access`'s return value, `*value`, and
`stop` fields for each address/width/direction combination). A device-state test:

1. Constructs a `GenesisRuntime` (zero-initialized per §8, or with specific `devices` fields
   pre-populated to exercise a particular reachable state).
2. Calls `genesis_route_access` with the address/width/direction under test — the exact same function
   generated C calls, never a parallel test-only decode/dispatch path.
3. Asserts on the returned `GenesisAccessResultKind`, `*value`, `stop`, and the resulting
   `runtime.devices` fields (including device-step counters, per §4.3) directly.

No test may reimplement device semantics (a second `genesis_*_access`-equivalent function, a parallel
enum, or a hand-computed expected register transition) to compute its own expected value independently
of production code's *decision logic* — a test's expected value is a literal, hand-derived constant
(exactly as `tests/genesis_startup_runtime_controller_io_test.py` already asserts a literal `0U`), not
a second implementation of the semantics under test. This is distinct from and does not weaken §16's
independent-oracle requirement, which applies only to the separate checkpoint-evidence/frame-artifact
comparison defined in §12–§17, never to this project's own unit-level device-state tests.

## 12. Checkpoint-evidence bundle schema

This section, and §13–§18, define a **fully separate concern** from §1–§11's persistent device-state
model: the checkpoint-evidence bundle used to validate a checkpoint's reached state against an
independent oracle. `GenesisDeviceEvidence` (§12.3) reuses §1's production model directly; nothing
below duplicates its shape.

```c
#define GENESIS_MAX_NAME_LENGTH 64U   /* reused from tools/genesis_startup_bridge_runtime.h */

#define GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION 1U
typedef struct GenesisDeterministicOptions {
  uint32_t schema_version;              /* GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION for this
                                            contract's initial delivery, versioned independently of
                                            GenesisCheckpointEvidenceBundle.schema_version so this
                                            narrower options schema can be revised on its own numbering
                                            without forcing a bundle-schema revision. */
  uint32_t instruction_budget;          /* The bounded generated-native dispatch-step budget
                                            genesis_runtime_run is invoked with for this run, named in
                                            the same units GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED
                                            already uses (tools/genesis_startup_bridge_runtime.h); 0
                                            means this run named no bound. */
  uint32_t stable_frame_vblank_count;   /* Verbatim copy of the GENESIS_STABLE_FRAME_VBLANK_COUNT (§13)
                                            value actually used to decide extraction timing for this
                                            run, recorded as data -- not only relied upon as a
                                            compile-time constant -- so a bundle remains self-describing
                                            across a future amendment that changes the constant. */
} GenesisDeterministicOptions;   /* This is the complete set of deterministic options this contract's
                                    initial delivery defines. A later task names any additional
                                    determinism-affecting input this contract does not yet anticipate
                                    as an explicit, named field added to this struct -- never an
                                    unnamed, undocumented input supplied outside it. */

typedef struct GenesisCheckpointIdentity {
  char     checkpoint_id[GENESIS_MAX_NAME_LENGTH];
  uint8_t  checkpoint_id_length;
  char     rom_sha256[65];             /* 64 lowercase hex chars + NUL, matching the existing
                                           rom_sha256 convention already used by the sanitized/full
                                           report schema (genesis-generalized-startup-runtime-bridge-
                                           contract.md §13.3). */
  GenesisDeterministicOptions options;   /* The complete deterministic-options value that produced this
                                           bundle, carried by value, not merely by digest: a comparing
                                           party -- in particular the independent oracle (§16) -- needs
                                           the actual option values to reproduce the run; options_digest
                                           alone only detects a mismatch after the fact, it cannot drive
                                           a reproduction on its own (§12.6). */
  uint8_t  options_digest[32];         /* SHA-256 over the canonical serialization (§14) of `options`
                                           above -- so a byte-identical rerun is independently verifiable
                                           by digest comparison alone once both sides already hold the
                                           same options value; this field never substitutes for
                                           transmitting `options` itself. */
} GenesisCheckpointIdentity;

typedef enum GenesisCheckpointPcClass {
  GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN = 0,
  /* One enumerator per checkpoint category a later task names when it defines that checkpoint --
     for example a "bounded title phase entered" class -- never a raw address. This contract reserves
     only the UNKNOWN default; it names no checkpoint category of its own. */
} GenesisCheckpointPcClass;

typedef struct GenesisCpuEvidence {
  uint32_t d[8];
  uint32_t a[8];
  uint16_t sr;
  GenesisCheckpointPcClass pc_class;    /* Category/domain-level classification only -- never a
                                            private raw PC value, matching this milestone's existing
                                            privacy discipline (§18). */
} GenesisCpuEvidence;

typedef struct GenesisRamEvidence {
  uint8_t work_ram_digest[32];   /* SHA-256 over the complete 65536-byte GenesisRuntime.work_ram,
                                     matching this contract's exact-match RAM comparison rule (§15). A
                                     later task names any additionally required selected-range digest
                                     as an explicit extension of this field, not a replacement. */
} GenesisRamEvidence;

typedef struct GenesisDeviceEvidence {
  GenesisDeviceState devices;   /* Reuses §1's production model directly -- never a duplicate shape. */
} GenesisDeviceEvidence;

#define GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES 256U
typedef struct GenesisTransactionEvidence {
  uint8_t  has_full_transactions;   /* 0 = digest-only encoding (§14): only transaction_count and
                                        transaction_digest are meaningful, `transactions` is not
                                        serialized or committed; 1 = full-list encoding (§14):
                                        transaction_count, the first transaction_count entries of
                                        `transactions`, and transaction_digest are all meaningful.
                                        transaction_digest is always computed and always present in
                                        either encoding (below); it is the only field guaranteed
                                        meaningful regardless of this flag. */
  uint16_t transaction_count;   /* uint16_t, not uint8_t: GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES is
                                    256, one past uint8_t's inclusive maximum of 255, so a uint8_t count
                                    could never represent a fully-populated bundle. uint16_t (range
                                    0..65535) represents every value in transactions' actual populated
                                    range, 0..256, with no overflow, and is serialized as a 2-byte
                                    big-endian integer per §14 rule 2. */
  GenesisBusAccess transactions[GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES];   /* Reuses the existing
                                        GenesisBusAccess type (tools/genesis_startup_bridge_runtime.h)
                                        verbatim -- never a duplicate transaction-record shape. Only
                                        the first transaction_count entries are meaningful; trailing
                                        capacity is never serialized, digested, or committed (§14). See
                                        §14 for this array's own count-prefixed canonical encoding and
                                        for GenesisBusAccess's own field-wise canonical encoding. */
  uint8_t  transaction_digest[32];   /* SHA-256 over the canonical serialization (§14) of exactly the
                                         first transaction_count entries above, always present
                                         regardless of has_full_transactions. */
} GenesisTransactionEvidence;

#define GENESIS_FRAME_WIDTH  320U   /* Immutable. Pinned by this contract as a bounded project
                                        compatibility policy (§12.5) -- never altered by a consuming
                                        task. */
#define GENESIS_FRAME_HEIGHT 224U   /* Immutable. Pinned by this contract as a bounded project
                                        compatibility policy (§12.5) -- never altered by a consuming
                                        task. */
typedef struct GenesisFrameArtifact {
  uint8_t pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT];   /* Indexed-color per §12.5: one byte
                                                                     per pixel, a CRAM palette index,
                                                                     row-major, top-left origin. */
  uint8_t palette_snapshot[GENESIS_VDP_CRAM_BYTES];   /* Verbatim copy of GenesisVdpState.cram at
                                                           extraction time (§9); GENESIS_VDP_CRAM_BYTES
                                                           is the same named placeholder §1.3/§2.4
                                                           already reserve. */
  uint8_t frame_digest[32];   /* SHA-256 over the canonical serialization (§14) of pixels followed by
                                  palette_snapshot, in that field order. */
} GenesisFrameArtifact;

typedef struct GenesisCheckpointEvidenceBundle {
  uint32_t                     schema_version;   /* 1, for this contract's initial delivery. */
  GenesisCheckpointIdentity    identity;
  GenesisCpuEvidence           cpu;
  GenesisRamEvidence           ram;
  GenesisDeviceEvidence        device;
  GenesisTransactionEvidence   transaction;
  GenesisFrameArtifact         frame;
  uint8_t                      bundle_digest[32];   /* SHA-256 over the canonical serialization (§14)
                                                         of every field of this struct declared above
                                                         this one, in declared order, computed last, over
                                                         the already-final values of every other field.
                                                         bundle_digest explicitly excludes its own field
                                                         from its own input -- it is not, and cannot be,
                                                         computed over a serialization that includes
                                                         itself (§14 rule 8). */
} GenesisCheckpointEvidenceBundle;
```

`genesis_extract_checkpoint_evidence(const GenesisRuntime *runtime, const GenesisCheckpointIdentity
*identity, const GenesisTransactionEvidence *transaction, GenesisCheckpointEvidenceBundle *bundle_out)`
is the architectural name (never implemented by this task) for the pure, read-only extraction §9
describes: it never executes generated code, never mutates `runtime`, and populates every field above
directly from `runtime`'s own state plus the caller-supplied identity/transaction evidence (transaction
evidence is supplied, not derived by this function, because only the driver that actually issued the
bus accesses under test has that ordered sequence available).

### 12.1–12.3 CPU / RAM / device evidence

Defined above (`GenesisCpuEvidence`, `GenesisRamEvidence`, `GenesisDeviceEvidence`).

### 12.4 Transaction evidence

Defined above (`GenesisTransactionEvidence`). The bounded canonical transaction sequence this field
carries is "the accesses this contract deems relevant" per the task record's own Outcome wording:
every access routed through §2.1's dispatch boundary between the start of the bounded checkpoint window
and the moment this bundle is extracted, in issuance order, up to `GENESIS_MAX_TRANSACTION_EVIDENCE_
ENTRIES`. A consuming task that needs a narrower relevance filter (for example, device accesses only,
excluding ordinary work-RAM traffic) states that filter explicitly as its own scoped decision; this
contract's own default is "every routed access," the least surprising and most falsifiable choice
absent a narrower one.

**Overflow policy.** If the actual number of relevant accesses during a checkpoint window exceeds
`GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES`, only the first `GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES`
accesses, in issuance order, are retained in `transactions` and counted by `transaction_count`; every
access beyond that capacity is silently dropped **from transaction evidence only** — it is never
dropped from, and never affects, CPU, RAM, device, or frame evidence, and it never causes extraction
itself to fail or stall. This is the sole overflow rule for this field; a consuming task never invents
a different truncation, rejection, or resizing behavior for it.

### 12.5 Frame evidence / nested frame-artifact schema (immutable)

Defined above (`GenesisFrameArtifact`). A "frame" is exactly this typed value: a fixed `320 × 224`
indexed-color pixel buffer (`GENESIS_FRAME_WIDTH = 320`, `GENESIS_FRAME_HEIGHT = 224`) plus the palette
snapshot needed to interpret those indices as color, plus the frame's own digest — row-major, top-left
origin, one CRAM-palette-index byte per pixel.

`GENESIS_FRAME_WIDTH`/`GENESIS_FRAME_HEIGHT` are **immutably fixed by this contract**, as a bounded
**project compatibility policy** — the standard non-interlaced active-display pixel dimensions this
project adopts as its one reference resolution for every checkpoint this milestone's remaining scope
defines — not a claim of proven, universal hardware truth covering every possible Genesis display mode.
This contract fixes both the buffer's *shape* and its two dimension values now, for this contract's
initial delivery. **No later task may change `GENESIS_FRAME_WIDTH`, `GENESIS_FRAME_HEIGHT`, or any other
part of the `GenesisFrameArtifact` shape.** If a later task's own cited hardware evidence shows a
specific checkpoint genuinely requires a different resolution, that task returns to this document for an
explicit amendment recorded in this document's own Revision history (exactly the discipline this
document's own correction round, above, already followed) — it never adjusts these constants inline in
its own scope, and never invents a second frame-artifact shape. This is the exact, final shape T049/T052/
T053 build against without any further architecture decision of their own.

### 12.6 Checkpoint identity and deterministic options

Defined above (`GenesisCheckpointIdentity`, `GenesisDeterministicOptions`). `checkpoint_id` names which
checkpoint this bundle represents (a consuming task's own named checkpoint, for example a bounded
title-phase checkpoint); `rom_sha256` binds the bundle to the exact authorized ROM image, matching this
project's existing ROM-hash-binding convention.

`options` carries, **by value**, the complete `GenesisDeterministicOptions` (`schema_version`,
`instruction_budget`, `stable_frame_vblank_count`) that produced this bundle; `options_digest` is its
SHA-256 (§14). Both are always populated together: `options_digest` alone identifies whether two runs
used identical options (a comparison, after the fact), but only `options` itself lets a party that does
not already hold the matching value — in particular the independent oracle constructing its own bundle
under §16 — actually reproduce the run. A digest is therefore never a substitute for transmitting
`options`; per §18, `options` (like the rest of the full local bundle) may remain transient and is never
itself committed to a backlog record.

### 12.7 Privacy/presence fields

Defined and governed by §18 below, which specifies exactly which fields above may be committed
verbatim and which are reduced to presence/provenance-only summary fields before any evidence reaches
a backlog record.

## 13. Bounded stable-frame condition

**The stable-frame condition is:** `checkpoint_entered == 1` (§1.4, §13.1) **and**
`GenesisInterruptState.vblank_transition_count - GenesisInterruptState.
vblank_transition_count_at_checkpoint_entry >= GENESIS_STABLE_FRAME_VBLANK_COUNT`, where
`GENESIS_STABLE_FRAME_VBLANK_COUNT = 2`. This requires exactly two full, deterministically counted
VBlank-onset transitions (§13.2), counted strictly after checkpoint entry, to have occurred before a
bundle is extracted, ensuring the extracted frame reflects a fully composed, second-pass-rendered
buffer rather than a partially written first frame that may still be mid-draw when the checkpoint is
first reached. Both operands of the subtraction are `uint32_t` fields written only per §13.1/§13.2
below; the subtraction is never evaluated while `checkpoint_entered == 0`, since
`vblank_transition_count_at_checkpoint_entry` is not yet meaningful at that point (§1.4).

`GENESIS_STABLE_FRAME_VBLANK_COUNT`'s value of `2` is a bounded **project compatibility policy** — an
engineering choice about how many deterministic device-level transitions are sufficient for stability
— not a claim about real hardware VBlank timing content (cycle counts, NTSC/PAL frame duration, or any
other timing fact this contract's own Non-goals exclude). It is falsifiable exactly as stated: a
consuming task extracts a bundle only once this condition holds, never earlier and never by any other
trigger (a CPU instruction count, elapsed wall-clock time, or a named device-state predicate this
contract does not itself define).

### 13.1 Checkpoint-entry event

Checkpoint entry is the first `GenesisControlTransfer` (§7) whose resolved target this bundle's own
`GenesisCheckpointPcClass` classification (§12.1) assigns to the specific non-`GENESIS_CHECKPOINT_
PC_CLASS_UNKNOWN` enumerator a consuming task defines for its own named checkpoint
(`GenesisCheckpointPcClass`'s own comment, §12). This is a purely target-address-class-based,
program-independent classification, exactly like `pc_class` itself (§12.1): a consuming task names one
fixed enumerator value for one fixed, cited checkpoint-entry target class, added to
`GenesisCheckpointPcClass` the same way a later task appends a `GenesisDiagnosticCategory` value (§2.2)
— never a PC value recognized ad hoc inline in scheduling or device logic, and never the kind of
program-pattern recognition §4.2 forbids for *device* behavior (checkpoint-identity classification is a
CPU control-transfer concept, governed by this subsection, not a device-advancement concept governed by
§4).

At the exact moment this classification first occurs for a given run, and only then: `GenesisInterrupt
State.checkpoint_entered` is set from `0` to `1`, and `vblank_transition_count_at_checkpoint_entry` is
set, synchronously in the same step, to the current value of `vblank_transition_count` at that instant
(which may already be nonzero, if a VBlank-onset transition happened before checkpoint entry — that
prior count is deliberately excluded from §13's subtraction). Both writes happen exactly once per run;
after `checkpoint_entered == 1`, no later classification result writes either field again, even if a
later control transfer also matches the same `pc_class` category. A run that never reaches this
classification never sets `checkpoint_entered`, and therefore never satisfies §13's stable-frame
condition — no bundle is ever extracted for it under this contract.

### 13.2 VBlank-onset transition trigger (definition of "the counter advances")

`vblank_transition_count` advances by exactly one at each `0` -> `1` rising edge of `vblank_pending`
(§1.4) — and at no other time. `vblank_pending` itself is set to `1` only by a routed access, an
access-caused state transition, or **the §4-amended dispatch-step scheduling tick (SEG-007-T047 /
ADR 0020)** under §2.1/§4.1/§7's already-established rules (for the ADR 0020 mechanism: the
dispatch-step scheduler reaching its bounded period while VDP register #1 IE0 bit 5 (`0x20`) is set,
per ADR 0020 §3); it is cleared to `0` only by the explicit, named acknowledgment side effect §7
requires — **and §7's own acknowledgment side effect is itself amended by ADR 0020 to include
successful eligible IRQ6 admission** (see the §7 amendment above). This subsection remains textually
accurate only because it defers to §7 by reference; it does not separately assert that the
acknowledgment mechanism itself is unchanged. Consequently the rising-edge rule that drives `vblank_transition_count` is itself
already fully governed by §4.1's allowed/§4.2's forbidden rule set: it is caused exclusively by a
documented device access or the access-caused transition that access triggers, never by a CPU
instruction count, PC value, opcode, or polling-loop shape. This subsection fixes only the precise
edge-triggering arithmetic (rising edge of `vblank_pending`, not "every access" and not "every read of
a status bit"); SEG-007-T047 supplies the concrete register address and the specific documented or
policy-labeled condition that sets `vblank_pending` to `1` in the first place (§2.4) — it does not
invent a different triggering rule for `vblank_transition_count` itself, which this subsection already
fixes.

## 14. Canonical serialization and deterministic digest/hashing rule

The complete `GenesisCheckpointEvidenceBundle` — not merely the frame artifact — is canonically
serialized as follows. Every serialization decision below is fixed by this contract; no consuming
implementation task decides, chooses, or defers any part of it. The canonical serialization is a pure
function of a value's own already-final field contents: it never depends on the host compiler, ABI,
struct padding/alignment, endianness, `enum` underlying-type choice, or any other implementation-defined
or unspecified C11 behavior — two conforming C11 implementations serializing byte-identical field values
under these rules always produce byte-identical output.

1. **Field order.** Fields are serialized in exactly their C struct declaration order — the order each
   struct is declared in, whether that struct is declared in §12 (`GenesisCheckpointEvidenceBundle` and
   every type nested directly inside it) or, for `GenesisDeviceEvidence.devices`, in §1
   (`GenesisDeviceState` and its own nested `GenesisZ80BusState`/`GenesisVdpState`/
   `GenesisVdpDmaState`/`GenesisInterruptState`, serialized exactly per their own §1 declaration order,
   recursively) — no reordering, no name-keyed map, no locale- or platform-dependent ordering —
   **except** `GenesisBusAccess`, whose own field-wise canonical encoding (rule 5 below) explicitly
   reorders two of its fields for a stated reason. No other struct referenced from
   `GenesisCheckpointEvidenceBundle`, in either §1 or §12, has an exception to this rule.
2. **Integer byte order.** Every multi-byte integer field (`uint16_t`, `uint32_t`, `uint64_t`) is
   serialized big-endian, matching the byte order this project's runtime ABI already uses for MC68000
   addresses and immediates throughout `tools/genesis_startup_bridge_runtime.c`'s `work_ram`/bus-access
   handling. A `uint8_t` field serializes as exactly the one byte it already is.
3. **Enums: fixed one-byte wire codes, never the compiler's `int` representation.** C11 does not fix an
   `enum` type's underlying representation, width, or signedness — it is implementation-defined, and this
   contract makes no claim about it. Every enum field appearing anywhere inside
   `GenesisCheckpointEvidenceBundle` is instead serialized as a **fixed one-byte (`uint8_t`) wire code**,
   equal to that enumerator's explicitly assigned C integer literal in its own type definition, entirely
   independent of the enum's compiler-selected underlying type, size, alignment, or signedness. This is
   sound for exactly the enum types that appear inside the bundle, because every one of them has every
   enumerator value already fixed in the range `0..255`:
   - `GenesisCheckpointPcClass` (§12, `cpu.pc_class`): `GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN = 0`, and every
     enumerator a consuming task appends (§13.1), each a distinct value in `1..255`.
   - `GenesisVdpDmaPhase` (§1.3, `device.vdp.dma.phase`): `GENESIS_VDP_DMA_IDLE = 0`,
     `GENESIS_VDP_DMA_BUSY = 1`.
   - `GenesisBusKind` (`tools/genesis_startup_bridge_runtime.h`, `transaction.transactions[i].kind`):
     values `1..5`.
   - `GenesisBusRegion` (`tools/genesis_startup_bridge_runtime.h`, `transaction.transactions[i].region`):
     values `1..2`.
   No other enum type appears inside `GenesisCheckpointEvidenceBundle`. If a future amendment to this
   document adds a bundle field of an enum type whose assigned values could exceed `255`, that amendment
   must explicitly widen this rule to a named larger fixed width for that specific type; absent such an
   amendment, every enum field this contract's initial delivery defines uses exactly this one-byte rule.
4. **Fixed-size arrays.** Serialized in full, every byte, in array index order (`d[0..7]`, `a[0..7]`,
   `work_ram[0..65535]`, `pixels[0..GENESIS_FRAME_WIDTH*GENESIS_FRAME_HEIGHT-1]`,
   `palette_snapshot[0..GENESIS_VDP_CRAM_BYTES-1]`, etc.).
5. **`GenesisBusAccess` field-wise canonical encoding (exception to rule 1).** Each
   `GenesisBusAccess` entry — appearing only inside `GenesisTransactionEvidence.transactions` (rule 6) —
   serializes its fields in exactly this order, **not** its C struct declaration order, because
   `raw_byte_count` (declared *after* `raw_bytes` in `tools/genesis_startup_bridge_runtime.h`) must be
   known before `raw_bytes` can be bounded:
   1. `ordinal` — `uint64_t`, 8 bytes, rule 2.
   2. `kind` — `GenesisBusKind`, 1-byte enum wire code, rule 3.
   3. `address` — `uint32_t`, 4 bytes, rule 2.
   4. `raw_byte_count` — `uint8_t`, 1 byte.
    5. `raw_bytes[0 .. raw_byte_count - 1]` — exactly `raw_byte_count` bytes (never the full
       `GENESIS_MAX_RAW_BYTES = 12`-byte buffer): the unused trailing bytes of `raw_bytes` beyond
       `raw_byte_count` are **excluded** from serialization and from every digest, for the same reason
       rule 6 excludes unused trailing transaction-list capacity. `raw_byte_count` must be no greater
       than the shared capacity. A producer must reject an over-capacity record rather than truncate it,
       and a consumer/validator must reject it rather than serialize or digest it. This preserves the
       route-provenance ABI's fail-closed capacity rule while allowing the full verified span of every
       currently accepted general-startup instruction.
   6. `region` — `GenesisBusRegion`, 1-byte enum wire code, rule 3.
   Every other struct in this document serializes its fields in plain rule-1 declaration order; this is
   the sole named exception, and it applies only to `GenesisBusAccess`.
6. **Bounded, count-prefixed transaction list; full-list vs. digest-only encoding.**
   `GenesisTransactionEvidence` has exactly two canonical encodings, selected by `has_full_transactions`:
   - **Digest-only encoding** (`has_full_transactions == 0`): serializes `has_full_transactions` (1 byte),
     `transaction_count` (`uint16_t`, 2 bytes, rule 2), then `transaction_digest` (32 bytes, rule 4). The
     `transactions` array itself is **not** serialized in this encoding.
   - **Full-list encoding** (`has_full_transactions == 1`): serializes `has_full_transactions` (1 byte),
     `transaction_count` (2 bytes), then exactly `transaction_count` `GenesisBusAccess` entries in array
     index order (each per rule 5), then `transaction_digest` (32 bytes). The unused trailing capacity of
     the fixed `GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES = 256`-sized buffer beyond `transaction_count` is
     **never** serialized or included in any digest, so uninitialized or stale trailing bytes cannot
     influence determinism.
   In both encodings, `transaction_digest` is computed identically (rule 8, below) — over exactly the
   first `transaction_count` `GenesisBusAccess` entries' rule-5 encoding — regardless of whether those
   entries are also serialized alongside it; a producer using the digest-only encoding must still have
   held the full first-`transaction_count`-entries value in memory to compute `transaction_digest` at all,
   it simply does not also serialize/commit them.
7. **Strings.** `checkpoint_id` serializes its declared `checkpoint_id_length` bytes only (never the full
   fixed buffer, for the same trailing-capacity-exclusion reason as rules 5–6); `rom_sha256` serializes
   its 64 hex characters (never its trailing NUL).
8. **Digest computation order and self-exclusion.** `work_ram_digest`, `options_digest`,
   `transaction_digest`, and `frame_digest` are each computed first, over their own named byte ranges per
   rules 1–7 (`options_digest` over `GenesisCheckpointIdentity.options`'s own three fields —
   `schema_version`, `instruction_budget`, `stable_frame_vblank_count`, each per rule 2, in that
   declaration order — never over `options_digest` itself). `bundle_digest` is computed **last**, over the
   canonical serialization of the complete bundle in declared order (including the already-computed
   `options_digest`, `work_ram_digest`, `transaction_digest`, and `frame_digest`, each treated as an
   ordinary fixed 32-byte array per rule 4) — **excluding `bundle_digest`'s own field**, which is not, and
   structurally cannot be, part of its own input. `bundle_digest` therefore transitively covers every
   other field's content without needing to re-serialize their raw source bytes a second time within its
   own computation, and without ever depending on its own not-yet-computed value.

Every digest above uses **SHA-256**, matching this project's existing ROM-hash-binding convention
(`rom_sha256`).

Two bundles extracted from byte-identical `GenesisRuntime` states, byte-identical caller-supplied
identity/transaction evidence (including byte-identical `GenesisDeterministicOptions`), and the identical
`schema_version` produce byte-identical serialized output and identical digests at every level, by
construction (rules 1–8 admit no nondeterministic choice: no hash-map iteration order, no floating-point
rounding, no platform-dependent struct padding included in the serialization, no compiler-dependent enum
representation, no wall-clock or environment-dependent field).

## 15. Comparison rules and tolerances per evidence category

Every evidence category is compared **exactly, with zero tolerance** — matching this milestone's own
required byte-identical-across-repeats determinism (the governing Outcome's "reproduces... entry into
the bounded title phase... at least twice byte-identically"):

| Evidence category | Comparison rule | Tolerance |
| --- | --- | --- |
| CPU (`GenesisCpuEvidence`) | Exact equality of every `d`/`a` element, `sr`, and `pc_class`. | None. |
| RAM (`GenesisRamEvidence`) | Exact equality of `work_ram_digest`. | None. |
| Device (`GenesisDeviceEvidence`) | Exact equality of every field in `GenesisDeviceState` (§1), including every device-step counter. | None. |
| Transaction (`GenesisTransactionEvidence`) | Exact equality of `transaction_digest`; if `has_full_transactions` is set on both sides, additionally exact equality of `transaction_count` and every populated `GenesisBusAccess` entry. | None. |
| Frame (`GenesisFrameArtifact`) | Exact equality of `frame_digest` (equivalently, byte-exact equality of `pixels` and `palette_snapshot`). | None. |
| Whole bundle | Exact equality of `bundle_digest`, which by §14 rule 8 is equivalent to exact equality of every category above holding simultaneously. | None. |

No named bounded tolerance is defined for any category in this initial delivery. A later task may
introduce one only by amending this contract with an explicit, provenance-justified tolerance for a
specific field (for example, if a specific rendering step is shown to have a hardware-documented,
legitimately non-deterministic dimension this project cannot reproduce byte-for-byte); absent such an
amendment, comparison remains exact for every category, and any mismatch — in any single category — is
a comparison failure for the whole bundle.

## 16. Independent-oracle method and construction requirement

**Method.** SEG-007-T052 must select and use one of exactly two independent reference-computation
approaches, both satisfying §17's independence rule:

- **(a) An existing, publicly available, independently authored implementation** of the relevant
  Genesis subsystem semantics (for example, a mature, independently authored emulator), not written by
  or derived from this project's own decoder/lifter/analysis/runtime/rendering code, invoked as an
  unmodified external tool or process over the authorized ROM input and the identical
  `GenesisDeterministicOptions` value this bundle's `identity.options` carries (§12.6), with its own thin
  integration adapter (translating that tool's native output into this bundle's §12 schema) covered by
  its own independent tests; or
- **(b) New, project-authored reference-computation code**, implementing the target subsystem semantics
  from independently cited public documentation, entirely separate from and never calling into this
  project's own production CPU/device/rendering implementation, with its own independent tests.

**Ownership.** If constructing this oracle requires nontrivial new reference-computation code (option
(b), or a nontrivial integration adapter under option (a)), that construction is **its own required
task**, with its own independent tests — it is never folded into SEG-007-T053, the final
comparison task, which performs only the comparison this contract already specifies. Per ADR 0012
(`docs/decisions/0012-checkpoint-evidence-runtime-and-independent-oracle-ownership.md`) this
construction task is **SEG-007-T132** (replacing the now-terminal SEG-007-T052), which also performs
the one authorized checkpoint-identity retry; the oracle lives in a non-production tree, links no
production object, and shares only this section's neutral schema and §17's permitted items.

**Prohibition.** The oracle **must never** be constructed by merely re-rendering VRAM/CRAM/device state
the implementation under test itself produced. Doing so would validate composition math only (pixel
assembly from already-trusted inputs) and could silently reproduce an incorrect upstream CPU/device/
transaction state as if it were correct, defeating the purpose of an independent check. The oracle must
independently derive the **complete** bundle — CPU, RAM, device, transaction, and frame evidence, not
merely the frame — from the same authorized input (the ROM image and the identical deterministic
options), never from any intermediate state this project's own implementation under test computed.

**Input/output schema.** The oracle's input is exactly the authorized hash-pinned ROM image plus the
complete `GenesisDeterministicOptions` **value** carried by `GenesisCheckpointIdentity.options` (§12.6)
— the actual option values, not merely `options_digest`, since the oracle must *reproduce* the run
byte-for-byte, and a digest alone cannot drive a reproduction (only detect a mismatch once both bundles
already exist). Its output is exactly one `GenesisCheckpointEvidenceBundle` (§12), populated by its own
independent computation for every field, never merely copied or converted from this project's own
extracted bundle.

**Comparison algorithm.** Applies §15's exact-match rule per category, combined into one PASS/FAIL
verdict exactly as [the runtime bridge contract](genesis-generalized-startup-runtime-bridge-contract.md)
§13.3's existing `--compare-runs`/`reports_match` boolean already combines whole-report byte-identity —
the same "one overall boolean produced by combining several typed sub-comparisons" precedent, applied
here to §15's richer, multi-category comparison instead of a single whole-report digest.

## 17. Oracle-independence permitted-vs-forbidden sharing rule

**Permitted** to be shared between the implementation under test and the oracle:

1. The neutral, versioned checkpoint-evidence-bundle schema/wire-format definition itself (§12's struct
   shapes, field names, and `schema_version`). *Example:* both sides may serialize/deserialize the
   bundle using the identical JSON (or equivalent) field names §12 defines, and both may compile
   against the identical `GenesisCheckpointEvidenceBundle` C struct declaration.
2. Public hardware constants whose provenance is independently and separately documented — a citation,
   not a code import. *Example:* both sides may hard-code the same publicly documented VDP register bit
   layout from the same cited page number, each independently transcribing it into its own code, rather
   than one side importing the other's constant definitions.
3. Test-vector **file formats** (not test-vector **values** generated by one side and merely consumed
   by the other as if independently derived). *Example:* both sides may agree on a common on-disk
   fixture-file layout for a synthetic input ROM; neither side may generate the *expected* register/
   pixel values in that fixture and hand them to the other side to treat as its own independently
   derived expectation.

**Forbidden** to be shared, under any circumstance:

- CPU semantic implementation code (opcode decode, effect computation, control-transfer logic).
- Device semantic implementation code (VDP command/DMA/timing/status logic, Z80-bus arbitration logic,
  interrupt delivery logic).
- Rendering-composition code (tile/palette decode, plane/sprite compositing, pixel assembly).
- Expected-value generation of any kind (one side computing a value the other side is then told to
  treat as ground truth without independently deriving it).
- Any production state-transition or composition function — this project's own `genesis_route_access`,
  `genesis_*_access` functions, `GenesisVdpState` mutation logic, or generated-C output must never be
  called, linked against, `#include`d, or otherwise reused by the oracle, and the oracle's own
  equivalent functions must never be called, linked against, or reused by this project's production
  implementation.

Independence means a **separate semantic implementation** and a **separately derived expectation**,
never merely an incompatible wire format: sharing item 1's neutral schema definition does not by
itself violate independence, but computing a device register's resulting value using shared logic
would, even if the two sides otherwise serialize their results differently. *Concrete forbidden
example:* the oracle may not be built by taking this project's own generated-native execution's final
VRAM/CRAM contents and running only a separate palette-lookup/pixel-composition step on them — that
reuses this project's own upstream CPU/device state verbatim and violates §16's prohibition as well as
this rule's "no shared production state-transition or composition function" line.

## 18. Checkpoint-evidence privacy boundary (transient-local vs. sanitized-committed)

The **complete, local `GenesisCheckpointEvidenceBundle`** — every raw register value, RAM/VRAM/CRAM/
VSRAM byte, transaction address, frame pixel, deterministic-option value, and every digest field
(`options_digest`, `work_ram_digest`, `transaction_digest`, `frame_digest`, `bundle_digest`) — may remain
**transient and private**: produced, inspected, and compared entirely in local process memory or an
on-disk artifact under an already-gitignored path, and **never committed**, mirroring [the runtime
bridge contract](genesis-generalized-startup-runtime-bridge-contract.md)
§14's existing "full report... local inspection only" precedent.

Any evidence recorded in a **backlog record** (or any other committed artifact) is reduced to a
**sanitized summary**, matching this milestone's existing privacy discipline (SEG-007-T016 and later
validation tasks' established sanitization pattern, and [the runtime bridge contract](genesis-generalized-startup-runtime-bridge-contract.md)
§13.3's existing sanitized-wire-report precedent). Committed evidence contains **only**:

- Already-approved identifiers/classifications: `checkpoint_id`, `schema_version`, `rom_sha256` (already
  an approved, non-private identifier per this project's existing ROM-hash-binding convention), and
  `pc_class` (already a category-level, non-address value per §12.1). These may be recorded verbatim.
- Presence/provenance fields: for example, that a given evidence category was present/absent in a bundle,
  or which checkpoint a bundle targeted.
- Per-category match booleans (§15): whether CPU, RAM, device, transaction, and frame evidence each
  matched the independent oracle, individually.
- The overall PASS/FAIL verdict (§16).

**No digest of any kind — `options_digest`, `work_ram_digest`, `transaction_digest`, `frame_digest`, or
`bundle_digest` — may be committed**, and no raw register value (`GenesisCpuEvidence.d`/`a`/`sr`, any
`GenesisDeviceState` field), raw RAM/VRAM/CRAM/VSRAM byte, raw transaction address/value, raw frame
pixel, or raw `GenesisDeterministicOptions` value may be committed, **unless a separately, already
existing committed policy explicitly authorizes that exact field** — matching this project's existing
narrow-and-explicit privacy discipline rather than a blanket "a digest is safe because it does not reveal
its input" exception. `rom_sha256` remains the sole existing such exception, for its own already-
established reason (an already-approved ingestion-identity binding fact, not a digest *of private runtime
state*); no digest this contract itself newly defines (`options_digest`, `work_ram_digest`,
`transaction_digest`, `frame_digest`, `bundle_digest`) is retroactively granted that same exception by
merely existing in this schema. A future amendment to this document may name one of these digests
committable explicitly, with its own stated justification; absent such an amendment, only the four
bullet categories above may ever appear in committed evidence for this contract's checkpoint-evidence
bundle.

This privacy boundary applies uniformly to both the implementation-under-test's own extracted bundle
and the independent oracle's bundle (§16–§17): neither side's raw evidence, nor any digest of it, is
ever committed — only the four bullet categories above and the final comparison verdict.

## Non-goals

- Any implementation, fixture, or test. SEG-007-T043/T045/T047/T049 implement exactly this contract's
  persistent-device seam for every genuinely stateful behavior they add; SEG-007-T052 implements exactly
  this contract's independent-oracle method; SEG-007-T053 performs exactly this contract's already-
  specified reproduction and comparison. None of them invents a new architecture decision this contract
  should have made instead.
- Migrating CTRL1/CTRL2/CTRL3 off their existing folded-constant policy. That policy remains correct and
  unchanged (§1.5): SEG-007-T020/T038 already select a deterministic value and side-effect-free behavior
  for those exact selectors as bounded project compatibility policy, never a hardware-truth claim.
- Deciding TMSS, version-ID, Z80, VDP, DMA, VBlank, or rendering *content* semantics: register bit
  values, bit meanings, concrete addresses, byte sizes, or hardware timing counts. This contract defines
  only the architectural seam those later tasks build behavior through (§2.4), reusing already-cited
  public facts or explicit project-policy labeling at their own implementation time. §13's bounded
  `GENESIS_STABLE_FRAME_VBLANK_COUNT = 2` constant is a project-policy engineering threshold on already-
  defined device-step-counter progression, not a hardware timing-content claim. §12.5's immutable
  `GENESIS_FRAME_WIDTH = 320`/`GENESIS_FRAME_HEIGHT = 224` constants are the same kind of project-policy
  engineering choice, not a hardware-timing or register-content claim.
- Any claim that the bounded title-screen checkpoint, or any capability this contract merely enables, is
  reached. This document authorizes no new device semantics by itself (§2.3, §10).
- Commercial-ROM inspection or any private/commercial-derived address, byte, opcode, disassembly, path,
  or trace, in this document or any artifact it authorizes.
- No `src/`, `include/`, `tests/`, `tools/`, or build-system file is touched by this task's diff.
