# ADR 0298: Function-hook feasibility (GO), statically resolved M68K hooks, and single-mod callback ordering, cycle visibility and failure semantics

- Status: accepted (combined delivery F09: SEG-138-T003 spike, SEG-138-T004 hooks, SEG-138-T005 ordering and failure). Implements section 5 of [mod-execution-phases.md](../architecture/mod-execution-phases.md) (ADR [0282](0282-mod-execution-phases.md)) on top of the phase scheduler of ADR [0297](0297-genesis-mod-phase-scheduler-and-work-ram-services.md) and the symbol/provenance model of ADR [0290](0290-typed-rom-edits-and-revision-symbols.md).
- Not in scope (later tasks): linking an ABI 2 provider and the hook table into a CLI-built program (SEG-138-T009), save states / rewind of hook frames (`MODSTATE`, SEG-138-T006), multi-mod ordering and exclusive hooks (SEG-141), Luau hook callbacks, Master System / Sega CD hooks (their own spikes).

## Decision 1 (SEG-138-T003): the spike returns **GO** for the Genesis M68K

Static function hooks are feasible in generated-native C **without a recompiler architecture change**. The generated program already dispatches every guest instruction identity through one function (`genesis_dispatch`, keyed by `runtime->pc`) after each retired instruction. A hook site is a const table entry compared with the dispatch PC; the whole mechanism is one wrapper around that function, emitted only when a build declares sites. Evidence is executable, not argued:

| Question (mod-execution-phases.md section 8) | Result | Evidence |
|---|---|---|
| 1. Does every statically known call target have a dispatchable entry and a classifiable return? | Yes. A subroutine entry is a compiled-entry identity addressed by its image address (`genesis_block_<addr>` / the compiled-entry table); its return is the RTS that pops the pushed continuation. Classifiable = the body (instructions reachable without entering a callee) ends only in RTS. | `genesis_function_hook_generated_test`, `genesis_function_hook_reject_test` |
| 2. `noop-hook-equivalence` bit-for-bit over >= 600 frames incl. nested calls | Yes: registers, SR, PC, work RAM, master clock and frame count hash identically to the unhooked program for 600 frames, for before / after / before-after hooks with no-op callbacks, a hooked program with no host attached, and a declined replacement; ~1.08 M calls of A and ~0.54 M nested calls of B. Through the real scheduler too. | `genesis_function_hook_generated_test`, `genesis_mod_hook_scheduler_test` |
| 3. Rejection tests, one per section 5.2 code | 29 cases, every class (table below). | `genesis_function_hook_reject_test`, `mod_hooks_test` |
| 4. Overhead of ten active hooks with trivial callbacks within 5 % | **GO**: ten before-after sites, one call each per ~70-instruction pass, 600 frames, `-O2`, best of 15 interleaved runs: no host attached -0.1 %, host with no-op callbacks +3.7 % (earlier batch -1.8 %; run-to-run noise on the shared host is about +-4 %). Stress fixture (two sites, 3.2 M hook callbacks in 600 frames, a hook event every ~30 dispatches): +4.8 %, ~4.7 ns per hook event. | spike measurements recorded below |
| 5. Hook table in build identity and cache key | Yes: manifest `hooks` is in the package identity, in the content digest, and the resolved table (`hooks` component) keys the variant. | `mod_hooks_test` |
| 6. Decision | **GO.** | this ADR |

Mechanism (what the spike prototyped and T004 ships): `emit_m68k_function_hook_wrapper` renames the ordinary dispatch `genesis_dispatch_unhooked` and defines `genesis_dispatch` as `if (genesis_function_hook_step(...)) return hooked; return genesis_dispatch_unhooked(runtime);`. `genesis_function_hook_step` (header-only, `platforms/genesis/runtime/function_hooks.h`) binary-searches a const table sorted by entry and keeps a bounded (32) call-frame stack. Nothing decodes an opcode, patches code or looks up a name at run time. With no plan the emitted text is byte-for-byte what it was (tested by diffing a hooked against an unhooked emission: the only changed or removed plain line is the one dispatch header, everything else is insertion).

### Call and return forms (spike scope)

| Form | Treatment | Check / evidence |
|---|---|---|
| `JSR abs.L` / `BSR` to the site | **supported** call entry | a direct call edge in code reachable from the reset entry; its continuation is recorded in the site's accepted-continuation set |
| `JSR (An)`, `JSR d(An)`, `JSR (d,An,Xn)`, `JSR (d,PC,Xn)` reaching the site | **skipped at run time, never altered**: the pushed return address is not one of the site's proven continuations, so the hook does not fire, the guest runs unmodified and the event is counted (`unproven_callers`) and logged once (`E_HOOK_UNPROVEN_CALLER`). At build time a site that no direct call reaches is `E_HOOK_COMPUTED_TARGET` when the program has computed transfers, else `E_HOOK_NOT_FUNCTION_ENTRY`. | generated test `ind-hooked` |
| `JMP` / `Bcc` / fallthrough into the entry | rejected | `E_HOOK_NOT_FUNCTION_ENTRY` |
| `RTS` | **supported** return: the exit boundary is the dispatch where PC == the frame's return address and A7 == entry A7 + 4 | after-hook order test |
| `RTE`, `RTR`, `TRAP`, `JMP` in the body (tail jump, jump table), `PEA` of a code address (push-then-RTS), `MOVEA` into A7 | rejected | `E_HOOK_UNCLASSIFIED_RETURN` |
| stack unwound without the return (unbalanced `ADDQ`/`LEA` to A7, longjmp-like) | allowed at build (not decidable); detected at run time: the frame is **abandoned** (A7 above entry A7 + 4), counted, logged once (`E_HOOK_FRAME_ABANDONED`), no exit callback | step test |

State at the boundary: **entry** is the dispatch after the call instruction retired: D0-D7/A0-A6/CCR as the call left them, A7 points at the return address, stack slot k is the long at A7+4+4k. **Exit** is the dispatch after the RTS retired: registers and CCR are the function's results, A7 = entry A7 + 4. Callbacks take zero guest time.

### Supported / rejected site classes

Each rejection is raised at build time (`resolve_m68k_function_hooks` / the emitter; the bridge text becomes `/* translation rejected: E_HOOK_...: ... (hook <id>) */`) and fails the build. The exact check is the first failing one in this order.

| Class | Supported | Exact check | Negative build test (case) |
|---|---|---|---|
| Entry of a statically called subroutine in immutable ROM | yes | all the checks below pass | "supported: JSR-called", "BSR-called", "nested callee, after" |
| Exit (return to the caller) | yes | body returns only through RTS | after / before-after cases |
| Replacement | yes, with a contract | contract complete and consistent | "two sites ... replacement of the leaf" |
| `E_HOOK_COUNT` | - | more than 64 sites | "more than 64 sites" |
| `E_HOOK_ID` / `E_HOOK_MODE` / `E_HOOK_REGISTER` / `E_HOOK_REVISION` | - | id token (1-64 of `a-z 0-9 _ - .`), mode in {before, after, before-after, replace}, register names in the view, revision declared | `mod_hooks_test` |
| `E_HOOK_CPU_UNSUPPORTED` | no | `cpu` is not `m68k` on Genesis (Z80, Sega CD sub, Master System have no succeeded spike) | "the Z80 has no succeeded spike"; `mod_hooks_test` (Master System) |
| `E_HOOK_OVERLAY_UNRESOLVED` | no | Sega CD package: module/overlay code is not verified per source identity | `mod_hooks_test` |
| `E_HOOK_DIGEST` | no | `resolve_hooks_for_image`: the image's SHA-256 is none of the declared revisions | `mod_hooks_resolve_test` (`resolve_hooks_for_image`) |
| `E_HOOK_MUTABLE_CODE` | no | entry >= `$00FF0000` or an execution alias / RAM thunk identity | "work RAM is mutable code" |
| `E_HOOK_MID_INSTRUCTION` | no | the address lies strictly inside an instruction the program reaches (the build compiles an independent identity at every decodable image address, so "inside" is judged against reachable instructions), or is odd | "mid-instruction"; `mod_hooks_test` (odd address) |
| `E_HOOK_NOT_COMPILED` | no | no compiled identity at the address | "not compiled" |
| `E_HOOK_INTERRUPT_CONTEXT` | no | the entry is a vector target or reachable from any exception/interrupt vector target (including the level-6 root) | "called from an interrupt handler", "an interrupt vector target" |
| `E_HOOK_COMPUTED_TARGET` | no | no direct call reaches the entry and the reachable program has computed jumps/calls | "computed target" |
| `E_HOOK_NOT_FUNCTION_ENTRY` | no | no direct JSR/BSR in reachable code targets it, or it is also reached by a branch, fallthrough, return or the reset entry | "reset entry", "instruction after a call", "branch target" |
| `E_HOOK_UNCLASSIFIED_RETURN` | no | body contains JMP, RTE, RTR, TRAP, an always-exception instruction, PEA of a code address, MOVEA into A7, runs into uncompiled code, or exceeds 4096 instructions | tail jump, RTE, jump table, push-then-RTS, stack replacement |
| `E_HOOK_OVERLAP` | no | two hooks claim one site, or a site is reachable only inside the extent (body and callees) of a replaced function | "one site claimed twice", "reachable only through a replaced function" |
| `E_HOOK_REPLACE_CONTRACT` | no | a replace hook lacks reads/writes/flags/cost, cost is not 1..65535 cycles, writes outside D0-D7/A0-A6/CCR, the CCR bit of `writes` disagrees with the flags effect, or a non-replace hook declares writes/cost, or reads a slot that does not exist | five cases |

**Architecture change required: none.** No class needs a change to the generated-code contract, the IR, the compiled-entry table or the runtime dispatch; the wrapper is additive and emitted only for a hooked build. **Hidden dependencies: none.** The one new input is the exception-vector target list (`M68kFunctionHookPlan::vectors`), which the caller derives from the image header; wiring it from a real ROM in the CLI is part of SEG-138-T009, not assumed here.

## Decision 2 (SEG-138-T004): the hook model

1. **Declaration.** A project declares `[[hook]]` tables (JSON `hooks` array in the package, strictly sorted by `id`): `id`, `symbol` (a **verified** per-revision symbol, ADR 0290; `manual` and `imported-hint` entries never resolve: `E_SYMBOL_NOT_VERIFIED`), optional `revision` (empty = every declared revision, each of which must resolve), `cpu` (default `m68k`), `mode`, `reads`, and for `replace` the contract `writes` + `cost`. Register names (the *register view*, mod-execution-phases.md 5.3): `d0`..`d7`, `a0`..`a7`, `ccr`, `cycles` (read only), `stack0`..`stack7` (the longs at entry A7+4+4k, read only). Writable: `d0`..`d7`, `a0`..`a6`, `ccr`. A7 is never writable: the replacement's stack effect is fixed (it pops the return address). Lists are in view order without duplicates. A package with hooks requires `hook.function`.
2. **Resolution.** `check_hooks` (reader, `mod check`) validates every declared revision independently (platform/CPU, ids, modes, registers, contract, verified symbols, per-revision site overlap, even address). `resolve_hooks(_for_image)` yields addresses and masks; the code generator then classifies each address against the compiled program (`classify_m68k_function_hooks`, in `libs/cpu/m68k`, over lifted operations and `m68k_control_successors` only).
3. **Identity.** Hooks are in the package identity (they are in the manifest bytes), in the content digest, and the resolved table (ids, addresses, modes, masks, costs) is the variant component `hooks`, so a changed hook never hits another variant's cache. A package without hooks has unchanged digests. `hook.function` is `experimental` on Genesis in the adapter table; the user-facing support matrix stays `unsupported` until SEG-138-T009 links hooks into a built program (as for input and memory in ADR 0297).
4. **Modes.** `before`: callback at the entry boundary, then the original runs. `after`: a call frame is pushed; the callback runs at the return boundary. `before-after`: both. `replace`: the callback runs instead of the function; when it **commits** (returns 1 with every declared result written) the runtime sets PC to the return address, A7 += 4, applies the register results and retires the **declared cost** in M68K cycles through the one timing seam (`genesis_runtime_retire_m68k_instruction`); a decline (returns 0) or a fault runs the original function. Replacement never runs original code partially and a replace site has no other mode.
5. **Skips are loud and non-altering.** Unproven caller, a stack outside work RAM, an odd stack pointer, a full frame stack (32) and an abandoned frame never change guest state; they are counted (`unproven_callers`, `overflowed`, `abandoned`) and the scheduler logs the first occurrence of each class.
6. **Bounds.** At most 64 sites, 32 nested frames, 4096 body instructions per classification, cost <= 65535 cycles.

## Decision 3 (SEG-138-T005): ordering, cycle visibility, failure

1. **Order.** Within frame N: `frame-begin(N)` -> `input(N)` -> `post-input(N)` -> hook callbacks as the hooked functions run (entry callbacks in call order; exit callbacks in reverse order of the corresponding entries for nested calls; callbacks of one boundary in mod order, single mod today) -> `frame-end(N)` -> render snapshot -> `render(N)` -> `frame-begin(N+1)`. Exits at a boundary run before the entry of a function called at that same boundary.
2. **Phases.** `hook-entry` serves `before` and `replace` sites, `hook-exit` serves `after`; each needs the phase registered, a binding (`GenesisModHookBinding{id, hook_id}` naming the generated site) and the `hook.function` capability (attach refuses otherwise with `E_CAPABILITY_UNSUPPORTED` / `E_PHASE_UNSUPPORTED` / `E_HOOK_BINDING`). A site no binding names is not this mod's and is skipped. Work-RAM reads and (staged, declared-range, budgeted) writes are legal in both hook phases; register results only in `hook-entry` of a replace site and only for declared slots; reading the register view needs `hook.function`, only declared slots, only inside a hook phase.
3. **Time.** Callbacks are free of guest time. The only guest time a mod can observe is `host->frame` and the register-view slot `cycles` (`master_ticks / 7`, i.e. M68K cycles; deterministic, monotonic, equal to the guest clock at the boundary, readable in every live phase and never in `render`). No wall clock, RNG or host pointer exists in any phase.
4. **Failure is transactional.** A callback faults on a capability, phase, space, range, alignment, write-budget, undeclared-slot violation, an invalid return value (`hook-entry`: not 0/1; before/after returning 1; `hook-exit`: not 0), or a replacement missing a declared result (`cause=contract`). A fault: (a) discards the callback's staged RAM writes, register results, input and log lines; (b) disables the mod for the session - no further callback of any phase, the pad interception ends; (c) records `E_MOD_FAULT cause=<c> mod=<id> phase=<hook-entry|hook-exit> frame=<n> hook=<site id>` in the log sink and `fault`; (d) a faulting replacement executes the **original** function, so the guest continues unmodified from that boundary. Writes committed by earlier callbacks stay (they are the mod's legitimate effects); the replay digest never contains a faulted callback's writes. Register results are streamed into the same digest as space `m68k.hook_regs`.
5. **Disabled mod.** Inert and counted: hook sites keep firing the runtime step (frame stack maintenance continues) but no provider entry runs; `E_MOD_FAULT` is the single failure record (a native fault that crashes the process is not recoverable; native code is trusted).
6. **Determinism.** The same program and input give the same state hash and the same committed-write digest (tested twice per scenario, including every fault kind).

## Consequences

- New stable classes: `E_HOOK_COUNT`, `E_HOOK_ID`, `E_HOOK_MODE`, `E_HOOK_REGISTER`, `E_HOOK_REVISION`, `E_HOOK_CPU_UNSUPPORTED`, `E_HOOK_OVERLAY_UNRESOLVED`, `E_HOOK_DIGEST`, `E_HOOK_MUTABLE_CODE`, `E_HOOK_MID_INSTRUCTION`, `E_HOOK_NOT_COMPILED`, `E_HOOK_INTERRUPT_CONTEXT`, `E_HOOK_COMPUTED_TARGET`, `E_HOOK_NOT_FUNCTION_ENTRY`, `E_HOOK_UNCLASSIFIED_RETURN`, `E_HOOK_OVERLAP`, `E_HOOK_REPLACE_CONTRACT`, `E_HOOK_BINDING`; runtime diagnostics `E_HOOK_FRAME_ABANDONED`, `E_HOOK_UNPROVEN_CALLER`, `E_HOOK_FRAME_OVERFLOW`.
- ABI 2 gained an append-only space, `SEGA_GEN_SPACE_M68K_HOOK_REGS` (7); `SegaModHost` and `SegaModProvider` are unchanged (the provider's `hook` entry already existed).
- `GenesisRuntime` gained one pointer, `function_hooks` (NULL by default); a program without hook sites contains none of the hook code.
- Only the general-startup AOT emitter (`emit_m68k_general_startup_runtime_c_to`, the Genesis production route) takes a hook plan.

## Limitations

- Hooks are not yet reachable from `segarecomp build`: the ABI 2 provider link, the manifest-to-plan wiring (including the vector list from the ROM) and the per-revision binding of declared ranges are SEG-138-T009. The evidence here is generated-native programs built from project-authored synthetic images.
- Call frames are not part of save states: a state taken while a hooked function is running loses its pending exit callback on restore (SEG-138-T006 owns `MODSTATE` and the restore rule).
- Calls to a hooked function through computed `JSR` forms are not hooked (skipped and counted); a hooked function reached by `JMP` from unanalysed code is the same case.
- Unbalanced stack adjustment inside a hooked function abandons its frame (no exit callback).
- Luau has no hook callbacks yet; native ABI 2 only.
- Single mod: ordering across mods is SEG-141.

## Rejected alternatives

Rewriting the return address on the guest stack to intercept the exit (alters guest-visible state and breaks bit-identity); per-instruction hook checks in every generated block (cost and byte-identity for unhooked builds); runtime opcode decoding or patching (violates generated-native); a fail-closed stop on an unproven caller (would kill legitimate indirect callers; the skip is loud and guest-faithful); hook sites keyed by symbol name at run time.

## Verification (F09 final head)

`genesis_function_hook_generated_test`, `genesis_function_hook_reject_test`, `genesis_mod_hook_scheduler_test`, `mod_hooks_test`, `mod_hooks_resolve_test` (new); `genesis_mod_scheduler_tests`, `mod_*`, `luau_phase_scheduler_test` and the fast preset re-run; see the pull request for the exact head and CI.
