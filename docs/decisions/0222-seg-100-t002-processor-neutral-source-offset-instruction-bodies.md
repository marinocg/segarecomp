# ADR 0222: SEG-100-T002 - processor-neutral source-offset instruction bodies (native correctness at a guest base unknown at compile time)

- Status: Accepted. Milestone SEG-100 (whole-game static AOT, M4). Refines ADR 0207 (source-relative bodies for *known* mappings) and ADR 0142 (BIOS-image profile variants). Related: ADR 0221 (architecture and feasibility), ADR 0223 (binding), ADR 0224 (inventory and build).
- Privacy: hand-encoded instructions and project-authored fixtures; for the authentic discs, classes and counts only.
- Gate: `DYNAMIC_BASE_AOT_GO` (native-code correctness only; it says nothing about which file is where - that is ADR 0223).

## Context

ADR 0207 shares one generated function per `(image, source offset)` among the *known* mappings of an image, but its dispatcher still has one `case` per known guest PC, and the BIOS-image profile (ADR 0142) emits a variant per `(PC, instruction bytes)`. Neither can serve an image the game loads at a base nobody told the emitter. The prompt's question for T002 is only whether a native body can be correct for an arbitrary legal guest base: PC-relative addressing, branches and calls, stacked continuations and exception frames, absolute addressing that must **not** move, byte verification and timing, computed JMP/JSR/RTS/RTE, the 24-bit bus.

## Decision

1. **A body is a function of the instruction's bytes alone.** `derive_dynamic_body` (`platforms/sega-cd/machine/src/dynamic_body.cpp`) decodes the instruction, lifts it, and takes the CPU-owned lowering (`emit_m68k_operation_c`, the timing expression and the refresh-skip statement - nothing is re-implemented) at **three** distinct sentinel PCs. `derive_source_relative_text` (ADR 0207) requires the three texts to be token-identical except numeric literals, each differing literal to move by exactly the PC difference with the same suffix; those literals become `(uint32_t)(scd_pc0 + UINT32_C(delta))`, every other literal (an absolute address, an immediate, a cycle count) is left exactly as the lowering wrote it. Any other shape is the typed refusal `source_relative_refused:<status>` (or `not_decoded`, `not_lifted`, `not_lowered`, `redecode` when the decoder reads the same bytes differently at another PC). **A PC-dependent instruction is never silently treated as position independent.** Because the sentinel offset is added to the PC only inside the lowering and the delta is relative, the text does not depend on where the bytes were found: the same bytes at any offset of any image give identical text (unit-tested). One body therefore serves every source offset, file and processor whose bytes are equal to it, and the emitter derives it once per *distinct instruction byte string*.
2. **Processor-neutral.** A source cannot be assumed to execute on one processor (Sonic CD's sub program loads code the *main* processor runs; the evidence is in ADR 0221). The Sega CD ABI text of the lowering is per processor instance only through the processor index and the computed-control authority, so the neutral emitter (`SegaCdM68kRuntimeCEmitter("scd_k", "scd_neutral_lookup")`) names them symbolically: the body starts `const uint32_t scd_k = m->dyn.cpu; ScdCpu *const cpu = &m->cpu[scd_k];` and the dispatcher sets `dyn.cpu` before the call. `scd_neutral_lookup` is a two-line function of the program unit that selects the processor's lookup. This halves the generated code against a per-processor body and removes the need to decide ownership at build time.
3. **The same execution protocol as every body.** `cpu->prefetch` (SEG-064-T003) is `0` or `scd_pc0 + length` exactly as `prefetch_position` decides (that helper moved to `emit_shared.cpp`, shared by both emitters); the main processor's refresh-skip statement (ADR 0177) runs only `if (scd_k == 0U)`; the application-entry `verify_only` early return is kept; `scd_code_guard(m, scd_k, scd_pc0, bytes, n)` compares the live bytes at the **actual** guest PC before anything else; retirement and timing are the unchanged `scd_retire`.
4. **Safety argument.** (a) The only claim made about the lowering is "these literals are affine in the PC", and it is checked at three PCs rather than assumed; (b) the semantics of one MC68000 instruction are a function of its bytes, the registers and the PC, so a body whose bytes the guard has just compared equal to the live bytes executes exactly what the hardware would at that PC - which is why a wrong *mapping* can only stop the program (ADR 0223), never change what an instruction computes; (c) a body is reached only through a verified binding (ADR 0223) or a static case; (d) nothing here runs at run time: the derivation is build time and the generated text contains no decoder.

## Audit of address-sensitive lowering (observed, not assumed)

| form | result |
|---|---|
| `BRA/Bcc/BSR/DBcc` (byte, word), `LEA/MOVE d16(PC)`, `(d8,PC,Xn)`, `JSR d16(PC)` | PC-derived literals -> `scd_pc0 + k` |
| `JSR/JMP abs.L/abs.W`, absolute loads and stores, immediates | not PC-derived: left absolute (unit-tested: the literal is the absolute value and no `scd_pc0` sum of it exists) |
| computed `JMP/JSR (An)`, `d16(An)`, `(d8,An,Xn)`; `RTS`, `RTE`, `RTR` | no PC literal; target authority is `scd_neutral_lookup` (ADR 0223); stacked continuation is `scd_pc0 + k` |
| `TRAP`, divide by zero, `ILLEGAL`, line A/F | the stacked PC and the faulting PC are `scd_pc0 + k` |
| identifier embedding the PC, masked or hashed literal, PC-derived constant expression | typed refusal; **0 occurrences in the 17.1 million compilable offsets of the two authorized discs** (10,554,591 + 6,552,309; ADR 0221) |
| odd source offset; an offset at or beyond the visible bytes; extension words that leave the visible bytes (a source-range boundary) | `not_decoded`: no body |
| address wrap: a target or continuation above bit 23 | the sums are unmasked 32-bit exactly as in the per-mapping lowering; the dispatchers and `scd_dynamic_dispatch` mask the PC to 24 bits, so both emitters agree (binding test: a PC with bits above bit 23 set is the same address) |
| live bytes changed after the load | `scd_code_guard` stops `code_bytes_mismatch` at the actual PC before the effect |
| a fetch outside executable memory | `scd_code_byte` is `NULL`: the guard fails, the binder never binds |

## Evidence

- `tests/sega_cd_dynamic_body_test.cpp` (15 forms x 4 checks + absolute-reference, location-independence, determinism and negative cases): all pass.
- **Differential against the per-mapping emitter, end to end** (`tests/sega_cd_file_first_test.py`, project-authored BIOS image and disc): the same fixture built with static admission (the per-mapping reference) and with `--static-discovery 0 --file-first structural|all` gives, for the three routes (module A at one base, B over A's address, C at two bases), **complete run reports that are equal** - both processors' registers, retired instructions, cycles, instruction-trace hash, every memory digest, device log and typed stop - except the program identity and the added `dynamic` section.
- Cost and sharing: ADR 0221 (distinct encodings are 3.5 % (Sonic CD) and 6.0 % (Earthworm Jim SE) of the compilable offsets; one body per offset would be 6.4 GB and 3.8 GB of C, the shared bodies are 316 MB and 319 MB: 20x and 12x less).

## Not claimed

That every lowering the CPU owner may add in the future derives cleanly (a new form that does not is a typed refusal); that PCs near the top of the 24-bit space were exercised by an authentic title; that the dynamic body is faster than a per-mapping body (the dispatch adds a table lookup and the guard is unchanged - no run-time difference was observed in the fixtures).

## Gate

`DYNAMIC_BASE_AOT_GO`: bodies are correct at every guest base the fixtures place a module at and at runtime-chosen bases; no PC-dependent form was found that does not derive; refusals are typed.
