# ADR 0207: SEG-097-T002 - source-relative (position-independent) generated-native MC68000 bodies

- Status: Accepted (SEG-097-T002). Implements the T002 half of ADR 0206's roadmap; refines ADR 0132 decision 1 ("compilation unit = one mapping of one image").
- Related: ADR 0126 (images per processor instance), 0127/0132 (overlay dispatch, one unit per mapping), 0049 (Genesis copy alias; the second consumer, T004).

## Context

ADR 0132 compiles one unit per (image, mapping): the same module bytes at two bases emit the instruction bodies twice, because every absolute PC differs.
This is guest-code position independence, not host relocation. The CPU semantics must keep using the actual guest PC; only the *emission* may stop being
duplicated. The prompt asks to separate five things: source identity and source-relative offset; the guest mapping (processor, base, subrange); the shared
body; the actual guest PC with the runtime mapping that selected the instruction; byte verification and overlay selection.

## Decision

1. **What is shared.** One generated function per `(image, source offset)` for an image with two or more mappings (`scd_p<k>_m<image>_s<offset>`). It
   reads the actual guest PC from the CPU (`scd_pc0`), verifies the live bytes at that PC with the unchanged `scd_code_guard`, runs the CPU-owned
   lowering and retires with the unchanged timing. Everything else stays per mapping: the per-PC `case` labels (a thin `return shared(m)`), the overlay
   candidate tables (entries, granule digests, roots) and the selection by whole-range live bytes. A mapping's identity never enters a body.
2. **How the body is derived (no new CPU semantics, no table of forms).** `libs/codegen/c11/source_relative.hpp` takes the CPU-owned lowering of the
   same instruction at three distinct sentinel PCs and requires the three texts to be token-for-token identical except for numeric literals; every
   differing literal must move by exactly the PC difference (linear in the PC, same suffix). Each such literal (a branch target, a PC-relative effective
   address, a stacked continuation, the instruction's own address in a stop report) becomes `(uint32_t)(scd_pc0 + UINT32_C(delta))`; absolute addresses,
   immediates and cycle counts do not move and are left exactly as the lowering wrote them. Anything else is a typed refusal
   (`token_structure`: e.g. an identifier that embeds the PC; `literal_not_linear`: masked or hashed; `literal_suffix`; `constant_context`: a PC-derived
   `case` label) and the instruction **keeps the per-mapping emission**, which is reported (`fallback_instances`, `fallback_reasons`). The lowering is
   never edited to force reuse.
3. **Scope of reuse.** Within one processor's image. The same bytes in the main and the sub processor are not shared: each instance has its own generated
   state names and routed-access protocol (ADR 0126 decision 2), and sharing across them would need a runtime processor parameter in every access. A
   single-mapping image shares nothing and its C is identical to the reference. Boot images are never shared. Different images at one base (overlay
   alternatives) never share: they are different bytes.
4. **Option, identity, reference.** `emit --position-independent` (`SegaCdEmitOptions::position_independent`). Off by default: the default emission is the
   per-mapping emitter of ADR 0132, byte-identical to before, and remains the verified Compatibility reference. The option enters the program identity
   (`emitter position_independent.v1`) only when set, so a source-relative program and its reference can never share a cache entry or identity.
5. **Safety argument.** (a) The sentinel lowering is the CPU owner's own output, so the only claim made is "these literals are affine in the PC", and that
   is checked at three PCs rather than assumed. (b) Dispatch reaches a shared body only through a mapping's own `case` for a PC inside that mapping, so
   `scd_pc0` is always a PC the mapping admits. (c) Every execution still compares the live bytes at that PC; selection among overlay candidates is
   unchanged. (d) No form is trusted: a lowering that does not derive cleanly falls back.

## Audit of address-sensitive lowering paths (what the derivation decides, observed)

| form | result |
|------|--------|
| `LEA d16(PC)`, `MOVE d16(PC)`, `(d8,PC,Xn)` | PC-derived literal -> `pc0 + k`; equal to the per-mapping run at both bases |
| `BRA/Bcc/BSR` (byte and word), `DBcc`, `JSR d16(PC)`, computed `JMP (An)` | targets are `pc0 + k`; backward and forward branches, 2x4 DBRA iterations equal |
| `JSR abs.L`, absolute short/long loads and stores | do not move across the three lowerings: left absolute (the helper call ran twice at its fixed address) |
| `RTS`, `RTE`, `RTR`, exception vector fetch | read the stack / vector; the stacked PC is `pc0 + k` (TRAP and divide-by-zero handlers returned correctly twice) |
| instruction length, prefetch, timing | independent of the PC; the timing expression derives with zero relative literals |
| odd-address fault | the stop reports the actual guest PC (`$90016`), equal to the reference |
| a transfer outside the admitted images | `entry_not_compiled` at the target, equal |
| bytes changed after loading (second mapping) | `image_bytes_mismatch` at the actual PC, equal |
| overlapping / partially mapped images, source subranges | unchanged admission; subrange mappings share by source offset |
| identifiers that embed the PC (`m68k_indirect_targets_<pc>`), masked or hashed literals, PC-derived `case` labels | **fall back** to per-mapping emission and are counted (none occurred in the matrix; covered by `source_relative_test`) |

## Result

- `tests/source_relative_test.cpp` (10 checks), `tests/sega_cd_source_relative_test.py` (25 checks): on the address-sensitive module at two bases, on the
  base disc (A and B alternatives at one address, C at two bases, three routes), on three typed-stop variants and on all six SEG-063 loader cases, the
  **complete run reports of the two emitters are equal** (both CPUs' registers, retired instructions, cycles, instruction-trace hash, every memory digest,
  the device log, overlay selection history and typed stop), and hand-derived memory contents hold in both. 35 shared bodies served 70 mapped instructions
  in the matrix module with 0 fallbacks; 13 bodies in the SEG-063 fixture.
- Measured cost (`tools/sega_cd_pi_cost.py`, one local run, the matrix module mapped at k bases, strict C11 `-O1`, GCC on Linux; one translation unit in both
  cases; times are CPU seconds of one process and noisy at this scale; the run time is the median of three runs of the same 333k-instruction route):

| k | emitter | native bodies | C bytes | compile CPU s | compile peak KiB | object bytes | exe bytes | run s | added C bytes vs previous k |
|---|---------|--------------:|--------:|--------------:|-----------------:|-------------:|----------:|------:|----------------------------:|
| 1 | per-mapping / source-relative | 186 / 186 | 205,406 / 205,406 | 0.286 / 0.284 | 45,916 / 45,816 | 110,200 / 110,208 | 241,872 / 241,880 | 0.132 / 0.133 | - |
| 2 | per-mapping | 221 | 241,201 | 0.326 | 48,348 | 126,984 | 256,680 | 0.091 | +35,795 |
| 2 | source-relative | 186 | 214,462 | 0.316 | 46,652 | 121,720 | 251,816 | 0.091 | +9,056 |
| 4 | per-mapping | 291 | 310,917 | 0.425 | 53,816 | 160,048 | 273,584 | 0.091 | +69,716 |
| 4 | source-relative | 186 | 225,456 | 0.319 | 47,144 | 134,656 | 260,704 | 0.093 | +10,994 |
| 8 | per-mapping | 431 | 450,351 | 0.628 | 64,552 | 226,288 | 315,584 | 0.090 | +139,434 |
| 8 | source-relative | 186 | 247,446 | 0.333 | 48,400 | 160,576 | 270,288 | 0.090 | +21,990 |

  Native bodies grow with the number of mappings in the reference (+35 per added base) and are constant in the source-relative emitter; the residual per-mapping
  cost (about 5.5 KB of C and 6 KB of object per base) is dispatch cases, entry tables and overlay candidate tables. At two mappings the saving is small
  (11% of C, 7% of compile CPU); at eight it is 45% of C, 47% of compile CPU and 29% of the executable. The compile time of the whole translation unit is what an
  incremental rebuild costs (one unit), so adding a mapping costs roughly the reference's full-unit time in both emitters, but adds 4-6x fewer generated bytes
  in the source-relative one. Dispatch overhead is below the noise of this measurement (the route's run time is equal to within 3%); there is no evidence
  of a run-time cost and none is claimed. Emission CPU time is dominated by process start at this size and is not separable.
- Not claimed: faster compilation of a real game (these are synthetic modules of 28-140 bytes of code per mapping); a benefit for single-mapping images;
  integration into the BIOS-image profile's partitioned emission (`bios_profile.cpp`), which has no multi-mapping image in M1 (carried to SEG-098).

## Consequences

- The Genesis consumer (T004) reuses `derive_source_relative_text` from `codegen_c11`; nothing Sega CD specific lives in it.
- A future lowering that embeds the PC in an identifier or in a constant-expression position will fall back and be visible in the report rather than miscompile.
