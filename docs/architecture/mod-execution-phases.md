# Mod execution phases, guest writes, input interception and static hooks

Status: accepted contract (SEG-133-T004, ADR [0282](../decisions/0282-mod-execution-phases.md)). **Not implemented**: the scheduler and hooks are M05 (SEG-138); per-platform realizations are SEG-147/148.
Inputs: [adapters](mod-platform-adapters.md) (space and phase names), [gap inventory](modding-platform-gap-inventory.md) (the generated-native execution model and save-state barriers).
Consumers: SEG-133-T003 (ABI 2), SEG-133-T005 (Luau), SEG-137-T004, SEG-138-T001/T002/T003/T004/T005/T006/T007, SEG-145-T001, SEG-147-T004, SEG-148-T005, SEG-141-T003.

## 1. The execution model this contract is written against

The generated-native machine runs guest code as generated C functions dispatched by a runtime loop (`docs/architecture/pipeline.md`; Genesis `platforms/genesis/runtime/runtime.h`).
Between two dispatches the machine is on a **dispatch boundary**: the next target is known, no generated function is mid-flight, no host callback executes and all CPU state is in the runtime
structure (Genesis save-state contract section 2; Master System and Sega CD barriers: `docs/architecture/sms-sega-cd-save-states-inventory.md` section 1). There is no interpreter, JIT or runtime opcode
decoding, and mod code is never inserted into the guest instruction stream. **All mod callbacks run on dispatch boundaries, never inside an instruction, a DMA step or an interrupt acknowledgement.**
A mod callback takes **zero guest time**: it does not advance the guest clock.

A **frame boundary** is the platform's vertical-blank onset as delivered by the machine scheduler (the boundary at which the ABI 1 snapshot is taken and at which the Sega CD barrier stops). Guest time advances only in the machine.

## 2. Phases

Order within one guest frame `N`: `frame-begin(N)` -> `input(N)` -> `post-input(N)` -> guest execution of frame `N` (with `hook-entry`/`hook-exit` callbacks as hooked functions run) -> `frame-end(N)` -> **render snapshot of frame N is taken** -> `render(N)` (host side, outside guest time) -> `frame-begin(N+1)`.
`reset` runs once, before the first dispatch after machine reset (never after a state restore).

| Phase | When | May read | May mutate | Ordering | Timing |
|---|---|---|---|---|---|
| `reset` | once, before the first dispatch after reset | declared readable spaces (power-on contents), source identity metadata | mod-owned state only (guest memory: **no**) | mods in declared order | zero guest time |
| `frame-begin` | first dispatch boundary at the frame boundary, before the frame's input sample | declared readable spaces; previous frame's final input | declared work RAM (staged, committed at callback end); mod state | mods in order | zero guest time |
| `input` | after the platform samples the physical/scripted pad for frame `N`, before the guest can read it | the sampled input state (typed buttons) | **the input state the guest will see**, button bits only; mod state | mods in order; each sees the previous mod's result | zero guest time |
| `post-input` | after `input`, before guest frame execution | declared spaces; final input | declared work RAM; mod state | mods in order | zero guest time |
| `hook-entry` | at the dispatch boundary where a hooked function is about to run | declared spaces; hook-declared register/argument view | declared work RAM; mod state; **may replace** the function (section 5.3) | by hook site, then mod order | zero guest time except the declared replacement cost (section 5.3) |
| `hook-exit` | at the dispatch boundary where a hooked function has returned | declared spaces; declared result view | declared work RAM; mod state | by hook site, then mod order | zero guest time |
| `frame-end` | at the next frame boundary, after the guest finished frame `N`'s dispatches, before the render snapshot | declared spaces | declared work RAM; mod state | mods in order | zero guest time |
| `render` | after the snapshot of frame `N`, outside guest time | **copies only**: `frame.work_ram`, `frame.vdp` copies, mod state | **overlay output, authored presentation output, mod state** - never guest memory, never input | mods in order | outside guest time |

Rules that apply to every phase:

1. **Contradiction check.** Guest memory is writable only in `frame-begin`, `post-input`, `hook-entry`, `hook-exit`, `frame-end`, and only in a declared work-RAM space ([adapters](mod-platform-adapters.md) section 3). `reset` and `render` never mutate guest memory.
   `input` mutates only the input state. A mod that has not declared the matching capability cannot perform the operation in any phase (`E_CAPABILITY_UNSUPPORTED`; at run time a fault with cause `phase`).
2. **Render is read-only and presentation-only.** Its inputs are copies taken after `frame-end`; its outputs never feed back into the guest or into later guest-visible state other than mod-owned state. This preserves ABI 1 semantics: the snapshot a provider sees is the state the game used to prepare the frame.
3. **Mods in order** means the order declared by the mod set (single mod today; SEG-141 extends: stable, declared, no concurrency).
4. **No wall-clock, host RNG or host pointers** in any phase (section 7).
5. **Cycle and timing behavior:** callbacks are free of guest time. A function replacement charges a declared constant (section 5.3). A callback that exceeds its host-time or instruction budget is a fault (section 6), not a stall.
6. Input interception changes only what the guest reads; the original physical state is retained for recording (recordings store the pre-mod input).

## 3. Per-CPU and per-platform application

| Platform / CPU | Phase availability | Writes | Function hooks |
|---|---|---|---|
| Genesis M68K (reference) | all phases | `m68k.work_ram` | per spike SEG-138-T003; **no promise until GO** |
| Genesis Z80 (sound) | none of its own; frame phases are driven from the M68K timeline | none (`z80.ram` not writable) | none |
| Master System Z80 | all phases at frame boundaries (SEG-147-T004) | `z80.work_ram` (mirror is one space) | per spike SEG-147-T003 |
| Sega CD main M68K | all phases at the Sega CD barrier (stop strictly before VBlank onset, before the input sample) | `main.work_ram` | per spike SEG-148-T004 |
| Sega CD sub M68K, main-side Z80 | read-only observation at frame phases; Word RAM readable only when main is the owner at that boundary; comm registers and interrupt-sensitive state are **not** exposed | none | none until SEG-148-T013 and its spike |

Save states, whole-machine and rewind exist for all three platforms (SEG-131 Genesis, SEG-132 Master System and Sega CD); mod state rides those codecs (section 6.2).
An interrupt is serviced by the guest as ever; a mod callback never runs between interrupt acknowledgement and the handler's first dispatch; `frame-end`/`frame-begin` coincide with, but never reorder, the machine's interrupt delivery.

## 4. Guest memory writes

* **Spaces.** Declared work RAM only (`m68k.work_ram`, `z80.work_ram`, `main.work_ram`); ROM is never mutated at runtime; every other space is read-only or inaccessible ([adapters](mod-platform-adapters.md) section 3).
* **Bounded.** Per callback invocation: at most 1024 write operations and 16384 bytes total. Exceeding either is a fault `E_WRITE_BUDGET` (section 6).
* **Endian-aware and width-checked.** A write names the space, an offset, a typed width (`u8`, `u16`, `u32`) and a value; the host converts to guest byte order. `u16`/`u32` on an M68K space must be even-aligned (`E_ACCESS_ALIGNMENT`); Z80 spaces have no alignment rule. Out of range is `E_ACCESS_RANGE`.
* **Capability-scoped.** `memory.write.<space>` must be declared in the manifest; undeclared is `E_CAPABILITY_UNSUPPORTED` at build time when statically visible, a fault otherwise.
* **Staged and committed at declared boundaries.** Writes made in a callback are staged; reads inside the same callback observe the staged values; the commit happens atomically at the end of the callback if no fault occurred, else every staged write is discarded.

## 5. Static function hooks

### 5.1 Principle

A hook site is resolved at **build time** against the exact source-image digest ([schema 3](segapatch-schema-3.md) section 5) to a generated function entry. The generated program contains the hook dispatch; there is **no** runtime opcode
decoding, no patching of running code and no runtime site discovery. CPU-defined semantics and control flow are preserved: a hook with a no-op callback must produce guest state, frames and audio bit-identical to the unhooked build.

### 5.2 Supported site classes and exact build-time rejections

| Site class | Supported | Rejection for the unsupported variant (stable class) |
|---|---|---|
| Entry of a function that is a statically known call target (subroutine entry) in code the build compiled | yes (after SEG-138-T003 GO) | address mid-instruction: `E_HOOK_MID_INSTRUCTION`; address not a function entry the build knows: `E_HOOK_NOT_FUNCTION_ENTRY` |
| Exit: the return of such a function to its caller | yes | function with non-standard returns (computed return, stack manipulation, tail jumps the build cannot classify): `E_HOOK_UNCLASSIFIED_RETURN` |
| Replacement of such a function (entry hook with `replace`) | yes, with a declared contract (5.3) | contract absent or inconsistent: `E_HOOK_REPLACE_CONTRACT` |
| Computed/indirect target not statically resolved | no | `E_HOOK_COMPUTED_TARGET` |
| Overlapping hooks (same site claimed twice, or a site inside a replaced range) | no (SEG-141 defines ownership for sets) | `E_HOOK_OVERLAP` |
| Site in code that executes from RAM or is mutable | no | `E_HOOK_MUTABLE_CODE` |
| Site in code the build did not compile or discover | no | `E_HOOK_NOT_COMPILED` |
| Site in an interrupt handler or exception vector | no in v1 | `E_HOOK_INTERRUPT_CONTEXT` |
| Digest mismatch (image is not the declared revision) | no | `E_HOOK_DIGEST` |
| CPU without a succeeded feasibility spike (Z80, Sega CD sub, Sega CD Z80 until their spikes) | no | `E_HOOK_CPU_UNSUPPORTED` |
| Sega CD module/overlay site not verified by symbol per source identity | no | `E_HOOK_OVERLAY_UNRESOLVED` |

### 5.3 Replacement contract

A replacing hook declares: arguments it reads (register and stack slot names for the CPU), results it writes (register set and declared work-RAM effects), flags effect, and an integer **cost in guest cycles** charged for the replaced function.
The callback must produce the declared results; the declared cost keeps timing deterministic. Missing or undeclared effects are rejected at build time. Replacement and entry/exit hooks never run original code partially.

### 5.4 Callback ordering

Hooks at one site run in mod order (declared order today; SEG-141-T003 owns multi-mod ownership rules, including exclusive hooks). Within a boundary: hook sites in guest execution order; `hook-exit` callbacks in reverse of `hook-entry` order for nested functions.

## 6. Transactional failure and mod state

### 6.1 Faults

A callback **faults** when it exceeds its resource budget (instruction budget or per-callback time budget, [Luau policy](luau-trust-policy.md) section 4), violates a capability, space, range or alignment rule, exhausts the write budget, returns an invalid value, or (Luau) raises an error.
On a fault: (1) the callback's staged writes and input changes are discarded; (2) the mod is **disabled** for the rest of the session (no further callbacks of any phase); (3) a diagnostic with a stable class (`E_MOD_FAULT`, plus the cause class, the mod id, the phase and the guest frame) is surfaced through the host log and the viewer;
(4) the guest continues unmodified from the next boundary; a replacing hook that faulted executes the **original** function (its replacement never committed). The disabled flag is part of mod state (6.2). A native fault that crashes the process is not recoverable: native code is trusted, not sandboxed ([Luau policy](luau-trust-policy.md) section 1).

### 6.2 Mod state in save states and rewind

* A new section `MODSTATE` is added to each platform container ([Genesis contract section 4](genesis-save-states-persistence-and-rewind.md); the Master System and Sega CD inventory containers). It holds, in mod order: the 32-byte package identity, a `state_schema` u32, a `disabled` flag, and an opaque blob of at most 64 KiB per mod (256 KiB total); the guest total limits of the container still apply (Genesis 4 MiB).
* The execution compatibility fingerprint gains: the ordered package identities, the ABI major, the declared capability sets and the `state_schema` values. A state is **restored only into a build whose fingerprint matches**; otherwise it is refused with a reason (`E_STATE_MOD_MISMATCH`) and the machine is unchanged (the existing two-phase restore). A state without mods is refused by a modded build and vice versa (the variant identity already differs).
* Mods supply bounded `save_state`/`load_state` through `state.mod` (ABI 2) or, for Luau, one serializable value (nil excluded; numbers, strings, booleans, nested arrays/tables of those, depth <= 8, size <= the blob limit). A mod that has not declared `state.mod` has empty state; its `disabled` flag is still saved.
* Rewind snapshots carry the same section; a rewind restore resets host sinks as today, restores mod state, does **not** run `reset`, and the resumed future is a new history branch.
* A mod whose `load_state` fails causes the whole restore to be refused, never a half-restored machine.

## 7. Replay and determinism guarantee

The guarantee covers runs in which no host-timing fault occurred. A Luau **watchdog** fault ([Luau policy](luau-trust-policy.md) section 4) is host-timing dependent: it marks the session non-deterministic and not replay-verifiable, and no determinism test may count a run containing one as a success. Deterministic faults (budget units, memory, depth, capability, range) are reproducible and remain inside the guarantee.

For the same build (package identities and order, ABI major, capability sets), the same initial state and the same per-frame input sequence, every frame's guest state digest, framebuffer digest and audio digest are identical across runs and hosts.
Mods observe no wall-clock time, no host random source, no thread or timer, no filesystem or network, and no host-pointer values; `random` is deterministic (seeded from the package identity and the guest frame counter, [Luau policy](luau-trust-policy.md) section 3).

Tests (to be implemented by SEG-138-T007, SEG-138-T006 and SEG-144-T004, parameterized per platform by SEG-146-T004):

1. `replay-equivalence`: run N frames from reset with a scripted input twice; all digests equal.
2. `restore-equivalence`: save at frame k, run to N; restore and run to N; digests equal.
3. `rewind-equivalence`: as 2 through the rewind path.
4. `noop-hook-equivalence`: a hooked build with no-op callbacks equals the unhooked build.
5. `fault-isolation`: an induced fault disables the mod, the guest continues; the digests equal the unmodded build from the fault boundary only for guest state not written by the mod before the fault (documented), and the diagnostic is stable.
6. `incompatible-state`: a state from a different mod set, ABI or capability set is refused with `E_STATE_MOD_MISMATCH`, machine unchanged.

## 8. Evidence required before hooks are implemented (SEG-138-T003 must produce; SEG-138-T004 may not start without a GO or PARTIAL)

1. How the generated C represents function entry and exit boundaries: whether every statically known call target of the synthetic fixture has a dispatchable entry and a classifiable return.
2. A synthetic fixture (no commercial data) with entry, exit and replacement hooks proving `noop-hook-equivalence` bit-for-bit over at least 600 frames, including nested calls.
3. Rejection tests, one per code in section 5.2 that applies to the M68K path.
4. Measured overhead: GO requires the per-frame cost of ten active hooks with trivial callbacks to stay within 5% of the unhooked frame time on the fixture; larger is PARTIAL with the measured limit recorded.
5. That the hook table is part of the build identity ([schema 3](segapatch-schema-3.md) section 8) and the cache key.
6. The decision, exactly one of GO, PARTIAL or NO-GO. NO-GO leaves `hook.function` unsupported and `SEG-138-T004` cancelled by the disposition protocol; nothing else in this contract changes.
Equivalent spikes gate SEG-147-T003 (Z80) and SEG-148-T004 (Sega CD); no function-hook promise exists for any CPU until its spike succeeds.

## 9. What this contract does not define

Scheduler implementation, hook plumbing beyond the generated-native M68K path, a universal CPU interception API, multi-mod ordering and ownership (SEG-141), audio events (see [modding-audio-architecture.md](modding-audio-architecture.md) section 5).
