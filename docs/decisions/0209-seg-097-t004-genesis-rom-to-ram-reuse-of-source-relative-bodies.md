# ADR 0209: SEG-097-T004 - does the source-relative mechanism serve the Genesis ROM-to-work-RAM copy (ADR 0049)?

- Status: Accepted. **Result: `GENESIS_REUSE_FEASIBLE_NOT_INTEGRATED`**: the shared mechanism reproduces the production Genesis lowering exactly and the shared bodies behave
  identically, with measured savings; the Genesis consumer path, its default output and its policies are unchanged, and the exact obstacles to a production integration are
  recorded as a follow-up.
- Related: ADR 0049 (immutable-copy alias), 0207 (source-relative Sega CD bodies), 0208, ADR 0077 (consumer-route alias producer), SEG-022-T011 (shared body helpers).
- Privacy: the fixture is the project-authored SEG-021-T041 image (a 40-byte routine); no commercial byte is used or recorded.

## Context

ADR 0049 compiles a ROM routine that the program copies verbatim into work RAM a second time, for the RAM execution base: "the SAME immutable source bytes decoded a second
time at the execution address", each alias body guarded by a comparison of its own bytes with the live RAM. That is exactly the situation ADR 0207 treats on the Sega CD (one
source image, several execution addresses). T004 asks whether the same mechanism applies, without a second implementation, and whether anything can be shared.

## Decision

1. **One shared concept, owned by the common code generator.** `libs/codegen/c11/source_relative.hpp` (`derive_source_relative_text`) is CPU- and platform-neutral text analysis
   over three lowerings of one instruction; the Sega CD emitter and this experiment are its two consumers. Nothing Genesis-specific lives in it and nothing was duplicated into
   the Sega CD platform.
2. **The production Genesis emitter is not modified.** `tests/genesis_source_relative_reuse_tool.cpp` runs the unchanged production emission
   (`emit_m68k_general_startup_runtime_c`) on the fixture with the routine's alias at 1..4 work-RAM bases and *derives from its output*. The default Genesis output, the
   consumer route and every Genesis policy identity are byte-for-byte what they were; the shared program exists only inside the test.
3. **What is derived.** Per instruction of the routine: the ROM identity's body (when it is a full function) and the alias identities' bodies are reduced to ONE function in which the
   PC-derived literals (the BSR continuation and branch targets, the PC-relative operand address, the provenance source address) are `genesis_pc0 + k`, the absolute addresses
   (`$00FF0800`, the stack range) stay absolute, and the alias byte-identity guard (`work_ram[pc0 - $FF0000 + i] != byte`) is conditional on `pc0 >= $FF0000`, so the same
   function serves the ROM address (no guard) and every alias address (guard). The compiled-entry rows of all those addresses name that function.
4. **Verification is exact and independent of the derivation**: every derived piece is instantiated at every real address of the *target* program (ROM and each alias, the
   fourth base never used to derive) and must reproduce that address's own production lowering text character for character; an instruction that does not reproduce keeps its own
   function (and is counted). The shared program is then compiled as strict C11 and compared with the production program by a **step-by-step machine-state trace** of the routine executed
   from ROM and from every RAM base (registers, status, stack, hash of work RAM after each instruction) and by every single-byte tamper of every executed alias instruction.
5. **Falsification control** (ADR 0049's own warning): a program that serves the alias addresses with the ROM-anchored bodies is built and must *differ* from the production
   trace. It does: the PC-relative read observes the ROM data word instead of the RAM one and every relative target is ROM-based.

## Result

`tests/genesis_source_relative_reuse_test.py` (29 checks, CI test `genesis_source_relative_reuse_test`) - fixture routine of 19 instructions with a full alias function:

| aliases K | functions production -> shared | C bytes | compile CPU s | compile peak KiB | object bytes | instructions kept per-identity |
|---|---|---|---|---|---|---|
| 1 | 70 -> 59 | 90,501 -> 80,933 | 0.171 -> 0.149 | 33,908 -> 33,268 | 40,208 -> 35,472 | 2 (RTS) |
| 2 | 89 -> 61 | 122,832 -> 87,103 | 0.223 -> 0.154 | 36,388 -> 33,872 | 51,992 -> 38,064 | 2 (RTS) |
| 3 | 108 -> 59 | 155,363 -> 84,193 | 0.272 -> 0.147 | 38,780 -> 33,488 | 63,840 -> 37,016 | 0 |
| 4 | 127 -> 59 | 188,097 -> 85,582 | 0.328 -> 0.148 | 41,212 -> 33,480 | 75,592 -> 37,696 | 0 |

- With three or more aliases every one of the 19 instructions (11 of them also serving the ROM identity) is one function; the number of functions and the generated size stop growing with
  the number of aliases (59 functions at any K >= 3), against +19 functions per alias in production. Compile CPU and object size fall accordingly (at K = 4: -55 % and -50 %); at K = 1
  (what a real single copy needs) the saving is small (-16 % functions, -11 % C bytes).
- The shared program's trace equals the production trace line for line at every K (42 to 141 lines: the ROM run, each RAM run, every tamper); all single-byte tampers of every alias stop at
  their instruction with nothing committed; the existing ADR 0049 harness (copy never happened, later change, mirror) passes unchanged on the shared program; the ROM-anchored control is
  detectably wrong at every K; the PC-relative read returns the ROM data word from ROM and the RAM data word from RAM (the two execution addresses are not interchangeable).

## What a production integration would still need (the follow-up, not done here)

1. **Program-wide authority lists are inlined in bodies.** The two RTS bodies embed the program's return-target list (one continuation per alias BSR), so their text depends on the number
   of aliases: derived from a different program they do not reproduce (K = 1 and 2 fail the exact check for exactly those two instructions and keep their own functions). A production
   share must read that authority through the existing compiled-entry lookup at run time (as the sharded emission already does) instead of a literal list.
2. **Existing byte-for-byte body sharing** (SEG-022-T011) turns eight ROM identities into one-line wrappers of `genesis_aot_shared_*` helpers; their alias identities are full bodies. The
   experiment shares those eight alias-only; a production design must decide whether the helper itself becomes source-relative.
3. **The emitter owns the function and table text**: the shared function needs a per-source-address name, the compiled-entry owner table must list it for every execution address, and the
   guard and the provenance literal must be emitted in their conditional / `pc0` form by the emitter, not by text rewriting.
4. **An explicit equivalence gate and a measured justification** before any default: the gate of this test (exact lowering equivalence plus the trace comparison) is the template.
   This task does not claim a saving for a real title (one copy, one alias: K = 1) and changes no Genesis output.

No extensive emitter redesign was started: the Genesis consumer path stays as it is and the follow-up above is recorded for the roadmap (SEG-097 candidate).
