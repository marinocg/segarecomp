# Z80 to Master System integration contract (SEG-008-T010 handoff)

Audience: the SEG-009 (Master System) refiner and implementer. This is the complete boundary between the finished,
machine-neutral Z80 CPU support and the platform work. Nothing here reopens Z80 CPU scope: a gap found later is a
family/root-cause repair of the matrix (see *Frontier policy* at the end), never a platform workaround.

Normative sources this document summarizes (they win on any disagreement): `docs/architecture/z80-cpu-contract.md`,
`docs/architecture/z80-scheduling-contract.md`, ADR 0056-0060, `docs/testing/z80-conformance-harness.md`,
`docs/testing/z80-capability-coverage.md`, `docs/testing/z80-legal-forms.md`.

## 1. What exists

| piece | location | owns |
| --- | --- | --- |
| `cpu_z80` library | `libs/cpu/z80` (`decode.hpp`, `forms.hpp`) | legal-form dataset ids, logical-fetch decoder, prefix chains, provenance, T-states, M1 counts |
| `codegen_c11_z80` library | `libs/codegen/c11` (`z80.hpp`, `z80_lowering.hpp`) | one C owner per instruction start of every code image, sharded deterministic C11, exact entry table |
| runtime ABI | `libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h` | state, host callbacks, outcomes, interrupt/boundary logic; plain C11 |
| coverage / conformance tooling | `tools/z80_*.py`, `tests/z80_*` | legal-form matrix, ratchet, oracle differential, corpus, budgets |

Completion evidence (SEG-008-T010): all 261 legal forms (1,446 encodings) pass all 11 coverage stages at 100%, with zero
exclusions. No `libs/cpu/z80`, `codegen_c11_z80` or Z80 generated program depends on `cpu_m68k`, `codegen_c11_m68k`,
Genesis or platform code, and no generic library depends on Z80 (checked from the CMake target graph).

## 2. Public API

Generation time (C++, `segarecomp::cpu::z80` / `segarecomp::codegen::z80`):

- `decode_at(const LogicalFetch&, uint16_t)` and `classify_all(const LogicalFetch&)` classify each 16-bit start as
  `decoded`, `prefix_lock`, `unresolved_fetch_mapping`, `mutable_code` (or the reserved `excluded_form`). The caller
  supplies a *logical* fetch function (`address -> byte | unresolved | non-code`, plus the code-image identity and
  image offset of each byte). Fetch wraps 0xFFFF -> 0x0000; a storage-image edge is never architectural.
- `emit_image_set(const ImageSet&, const EmitOptions&)` writes the sharded C for an `ImageSet` and returns statistics
  (`EmitStats`) or an error string (nothing is left behind on failure). Output is byte-identical across runs.
- `t_states`, `m1_fetches`, `logical_length`, `displacement`, `immediate`, `replay_bytes` expose per-instruction facts and
  provenance (image id, address, image offset, byte count, prefix count) for diagnostics.

Run time (plain C11, generated image + `z80_runtime.h`):

- `Z80Outcome z80_run(Z80Runtime *rt, uint64_t deadline)` is defined by every generated image.
- The platform owns a `Z80Runtime { Z80State state; Z80Host host; Z80Outcome outcome; }`, calls `z80_reset(&state)`
  (contract reset state: PC=0, I=R=0, IFF=0, IM0, AF=SP=0xFFFF, other registers 0xFFFF) and then `z80_run`.

## 3. Runtime ABI (`Z80Host`)

| callback | contract |
| --- | --- |
| `read(ctx, addr16, cycles)` / `write(ctx, addr16, value, cycles)` | data memory. `cycles` = T-states at the start of the current instruction; intra-instruction offsets and wait states are outside the contract |
| `io_in(ctx, port16, cycles)` / `io_out(ctx, port16, value, cycles)` | see section 6 |
| `interrupt_acknowledge(ctx, cycles)` | the byte the device puts on the data bus at maskable-INT acceptance; called on every accepted INT (IM0/1/2) |
| `code_image(ctx, addr16, Z80CodeImage*)` | non-zero and fills `{identity, window_base}` if immutable code is mapped at `addr16`; zero means mutable/non-code (fail closed) |

Instruction fetch never goes through `read`: code is the compiled immutable image. `read` serves operand data only.
Reading ROM as data (tables, `LDIR` from ROM) is an ordinary `read` and is unrelated to code identity.

## 4. Image identity keying and mapping-sensitive dispatch (banked immutable ROM)

Owners are keyed by `(code-image identity, address)` (identity <= 0xFFFF, high half of a 32-bit entry key).

- `ImageKind::invariant`: exactly one window; absolute-PC owners; direct owner-to-owner binding is allowed between
  invariant-window instructions.
- `ImageKind::banked`: one or more admissible windows `{base, first_offset, length}` (windows of one image may expose
  different sub-ranges). An image admissible in exactly one window gets absolute-PC owners. An image admissible in several
  windows gets **window-relative** owners: they receive the run-time `window_base` and derive every PC-dependent value
  from it. An owner exposed by only some window
  bases fails closed for the others.
- Every runtime-selected control transfer (`JP (HL)/(IX)/(IY)`, `RET/RETI/RETN`, interrupt entry PCs, table dispatch, and
  resume after `deadline`/`halted`) returns to the generated dispatcher, which calls `host.code_image(PC)` and looks up
  `(identity, PC)` (or `(identity, PC - window_base)`) **exactly** in the compiled entry table. A miss is a typed stop, never
  a discovery cycle.
- **SEG-009 contract**: the platform reports the *current* bank mapping through `code_image`. Crossing a bank boundary is
  a mapper write followed by the next dispatch: nothing in `libs/cpu/z80` changes. Direct-bound successors exist only
  inside a statically invariant window; anything mapping-sensitive is dispatched.
- Reference shape (ADR 0058 addendum; guarded by `z80_static_budget_shape_test`): image 1 = invariant first 1 KiB
  (`0x0000-0x03FF`); images 2..N = 16 KiB ROM banks, each admissible in slot 0 (`0x0400-0x3FFF`, exposes offset 0x0400+),
  slot 1 (`0x4000-0x7FFF`) and slot 2 (`0x8000-0xBFFF`); nothing at `0xC000-0xFFFF` (RAM). Slot-window instructions that
  straddle a slot boundary classify as `unresolved_fetch_mapping` (fail closed) rather than guessing the next bank.
- SEG-009 owns the decision of *which* images/windows to declare for a given cartridge and mapper; the CPU side needs only
  the resulting `ImageSet` and the `code_image` answer.

Measured cost (SEG-008-T009 and re-measured in T010, budgets unchanged): full 64 KiB dense/random image 46-47 MiB C, 15 MiB
executable, 10.8-13.9 s at `-j8`, 231-244 MiB compiler RSS, exact lookup 37-40 ns; SMS-shaped 512 KiB map (dense/random/local
image) 340-368 MiB C, 116-123 MiB executable, 72-125 s at `-j8`, compiler RSS 868-921 MiB, lookup 65-66 ns. A 256 KiB
authorized local image: 170 MiB C, 57 MiB executable, 37 s at `-j8`, 439 MiB RSS, 36.5 ns. Generated C must be built with
the chunked table and TU sharding already applied by the emitter; there is no user knob to set.

## 5. Mutable code fails closed

Code that is not an immutable image is never executed. A transfer to an address for which `code_image` returns zero
stops with `Z80_ERROR_MUTABLE_CODE`; an instruction that would fetch a byte from non-code (including an image edge) is a
`mutable_code` stub owner. RAM-generated code, code modified after compilation and self-modifying code are all in this
class, and so is a ROM-to-RAM copy until a later architecture (SEG-032-style static materialization built on the same
identity key) gives it an image. The state is left exactly at the offending instruction start and is never resumed.
SEG-009 must treat this as a platform decision (declare the RAM image if it is provably static, or stop), not as a CPU gap.

## 6. I/O callback contract (16-bit port)

`IN A,(n)` / `OUT (n),A`: port = `(A << 8) | n`. `(C)` forms and block I/O: port = `BC`. `INI/IND/INIR/INDR` drive the port
with B *before* its decrement; `OUTI/OUTD/OTIR/OTDR` decrement B first and drive the *decremented* B (ADR 0057). `IN F,(C)`
and `OUT (C),0` are modelled. The callback receives the full 16-bit port, direction and value. **Port decoding is platform
policy**: the SMS ignores some address lines, and that mirroring, the VDP/PSG/controller/memory-control ports and their
side effects belong to SEG-009. Block-I/O repeats are one boundary per iteration (21 T repeating, 16 T final).

## 7. Interrupts, NMI and the data bus

Inputs are `Z80State` fields the platform drives between `z80_run` calls: `int_line` (level), `nmi_pending` (edge, latched).
- INT is accepted at a boundary when `int_line && iff1 && !int_deferral` (EI and IFF1-changing RETI/RETN defer one
  instruction); NMI has priority, clears IFF1 (keeps IFF2), pushes PC, jumps to `0x0066` (11 T).
- IM1: `0x0038`, 13 T; IM2: vector = `(I << 8) | data-bus byte` (16-bit wrap), 19 T; IM0: **RST-only static contract**
  (ADR 0056): the acknowledge byte must be one of the eight `RST` opcodes (13 T); any other byte stops with
  `Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE` before any state changes. There is no runtime opcode decoding.
- The acknowledge callback is invoked on every accepted maskable INT (its byte is ignored in IM1). A halted CPU leaves HALT
  on INT/NMI acceptance and pushes the address after HALT.
- Inputs raised while a deadline is already reached are seen at the next call. No INT/NMI is accepted while
  `in_prefix_run` is set (an endless DD/FD run is one indivisible boundary).
- **SEG-009 owns** wiring the VDP frame/line interrupt and pause-button NMI to these fields, deasserting `int_line`
  when the VDP status is read/acknowledged, and the interrupt schedule against the clock.

## 8. Clock and deadline contract

Time is `Z80State::cycles` (monotonic 64-bit T-states). The platform passes an **absolute** deadline; execution stops at
the first boundary with `cycles >= deadline`, so the last instruction may overshoot and no instruction is split. A block
iteration, an interrupt response and a halted M1 cycle are boundaries. A deadline already reached does no work. Wait
states, bus contention and bank latency are not modelled by the CPU: SEG-009 either adds them to the clock between calls or
accepts the documented nominal T-state counts. A run split at any deadline sequence ends in the same state as the
straight-through run (verified against the oracle in T010 on ~1.2 M random-program steps with 1/5/20 T budgets and
INT/NMI injection).

## 9. Outcome classes

| class | outcomes | platform action |
| --- | --- | --- |
| resumable | `deadline`, `halted`, `prefix_lock` | advance time, drive INT/NMI (not while `in_prefix_run`), call `z80_run` again |
| fail-closed | `no_owner`, `mutable_code`, `unresolved_fetch_mapping`, `unknown_image_identity`, `excluded_form`, `im0_unsupported_acknowledge_byte` | stop; never resume; diagnose the platform mapping/identity or interrupt device |

`z80_outcome_is_resumable` and `z80_outcome_is_error` are disjoint and exhaustive.
`unknown_image_identity` and `no_owner` indicate an inconsistent platform mapping, not a missing instruction: under broad
AOT every legal start of every declared window has an owner.

## 10. What SEG-009 owns (and the CPU never will)

Memory map (ROM/RAM/mirrors), mapper/banking registers and the image identities/windows they select, BIOS/boot-state
choice, ROM ingestion and metadata, VDP, PSG, controllers and other I/O port decoding and side effects, interrupt-source
wiring and acknowledgement, scheduling/clock of devices against Z80 time, Game Gear differences, and generated-runtime and
viewer integration. Everything above is a platform contract over the ABI in sections 3-9; none of it needs a Z80 change.

## 11. Known boundaries to design around

- No self-modifying/RAM-resident code (section 5). A Master System game that executes from RAM is a platform
  materialization problem (declare a static image) or a stop, never an interpreter fallback.
- IM0 is RST-only by design; a device that places another opcode on the data bus stops the machine.
- Wait states/contention, bus-cycle level timing and undocumented-behaviour beyond the NMOS model in the contract are out of scope.
- `zexdoc/zexall` were not run (no local image); the external falsification corpus is SingleStepTests/z80 at its pinned
  commit (190,430 cases sampled, zero unexplained disagreements), plus the pinned redcode oracle and the secondary
  kosarev oracle under a committed deviation mask.

## 12. Reproduce

```sh
cmake --preset dev && cmake --build --preset dev
SEGARECOMP_Z80_ORACLE_CHECKOUT=<z80 oracle dir> ctest --test-dir build/dev -R z80
python3 tools/z80_legal_forms.py --check
python3 tools/z80_capability_coverage.py --probe <z80_capability_probe> --lowering-probe <z80_lowering_probe> \
    --emitter <z80_image_emitter> --cc cc --check
python3 tools/z80_conformance.py --emitter <z80_image_emitter> --cc cc --secondary
python3 tools/z80_static_budget.py --emitter <z80_image_emitter> --shape dense64 --cc cc
```

## Frontier policy

A runtime frontier reached by later software never becomes "support opcode X". It names the incomplete architectural
family, static-code assumption or machine contract, repairs that class, and extends the matrix or harness. The Z80 matrix is
complete; a genuine disagreement with the pinned oracle or a public hardware reference is a defect in a continuation task
under SEG-008's owner libraries, recorded with a legal-form id.
