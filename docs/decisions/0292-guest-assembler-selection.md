# ADR 0292: Guest assembler selection - a bounded in-tree assembler for M68K and Z80 (`guest-asm`)

- Status: accepted (combined delivery F05: SEG-136-T003 decision; implemented by SEG-136-T004).
- Builds on: ADR 0281 (schema 3; `guest-asm` was a reserved kind), ADR 0290 (edits, verified symbols), ADR 0280 (the ROM is immutable at runtime; edits are build-time), `mod-execution-phases.md`, `mod-platform-adapters.md`.

## Context

A mod needs to inject small hand-written guest routines (M68K for the Genesis main CPU, the Sega CD sub CPU, Z80 for the Master System and the Genesis sound CPU) into declared free ranges of an exact original image. The tool that turns the source into bytes must (a) be redistributable with the MPL-2.0 product, (b) run identically on Linux, macOS and Windows with nothing installed, (c) be byte-deterministic, (d) be bounded - no includes, macros or host paths, no clock - and (e) give diagnostics tied to source lines and a stable class. The result also has to be statically validated (forbidden instructions, branch targets, writes), which needs an instruction-level view that an opaque object file does not give.

## Evaluation (decision matrix)

Each candidate is judged against the named requirements R1 license compatible with MPL-2.0 redistribution, R2 no host dependency on the three CI hosts (pinned, built or vendored), R3 byte-deterministic, R4 bounded input (no host paths), R5 one tool for M68K and Z80, R6 instruction-level validation hook.

| Candidate | R1 | R2 | R3 | R4 | R5 | R6 | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GNU as (binutils) | fails: GPL-3.0-or-later (no vendoring into an MPL-2.0 distribution; invoking an installed copy is possible) | fails: the stock `as` of the hosts is for the host CPU - verified on the reference Linux host (`as` 2.44 rejects `-march=m68000`: "unknown architecture"); M68K and Z80 need separate cross builds (Z80 only in recent binutils) | ok | `.include` reads host paths unless disabled | needs two cross toolchains | ELF objects: needs `objcopy` and a decoder to validate | rejected on R1/R2/R5 |
| vasm (vasmm68k_mot, vasmz80_oldstyle) | fails: not an OSI license; the author's page offers binaries only ("Source Snapshot coming soon"), no pinned redistributable source; terms are the author's, not MPL-compatible without a separate grant | fails: no stable pinned source to build from; the released binaries are 2021 Windows/Linux x86-64/ARM only (no macOS) | ok | `include` and `incbin` read host paths | ok (one family) | raw output | rejected on R1/R2 |
| customasm 0.14 (Apache-2.0) | ok (Apache-2.0 verified from its `Cargo.toml`) | needs a Rust toolchain in the product build | ok | `#include` | has no M68K or Z80 definition: we would author both instruction sets in its rule language | raw output | rejected: the ISA tables would be written in a foreign language anyway, plus a new build toolchain |
| Host-provided arbitrary assembler (`--assembler <path>`) | n/a | fails: the image would depend on an unpinned host binary | fails: different versions, different bytes | fails | - | - | rejected: a mod would not reproduce |
| **New bounded in-tree assembler** | ok (our code) | ok (C++20 in `libs/media`, no dependency) | ok (pure function of the text) | ok (no include/macro/host path) | ok (two small tables behind one driver) | ok (the assembler emits instruction records that the validator checks) | **selected** |

No external candidate satisfies R1 together with R2, which is why a new assembler is chosen. The choice is sound only because the required surface is small and fixed: hook and utility routines, not whole programs.

## Decision

1. **A new in-tree assembler of a documented subset, kind `guest-asm`** (schema 3 payload kind version 1, field `cpu` = `m68k` | `z80`, ASCII text, at most one per package, counted with `guest-code` against the 1 MiB cap). It is a pure function of the text and the verified symbols of the revision: no file, include, macro, clock or host byte order.
2. **Source language.** Lower-case ASCII, one statement per line, `;` comments. `free <rev|*> <start> <len> <fill>` declares a free range and `org <rev|*> <offset> [at <addr>]` starts a block placed at an image offset; `label:`, the instruction subset below, `dc.b/dc.w/dc.l` (M68K) or `db/dw` (Z80), `even`, and `data` / `code` mode switches. Operands accept numbers, local labels and `@symbol[+N]` (verified symbols only, ADR 0290). Every revision is assembled independently from the same text, so one source serves several revisions whose symbols differ.
3. **M68K subset** (Genesis main CPU and the Sega CD sub CPU, which is an unmodified 68000): `move/movea`, `moveq`, `add/sub/and/or/cmp` (EA to Dn and Dn to EA), `addi/subi/andi/ori/eori/cmpi`, `addq/subq`, `clr/neg/not/tst`, `lea/pea/jmp/jsr`, `swap/ext`, `asl/asr/lsl/lsr/rol/ror`, `bra/bsr/bcc`, `dbra`, `nop/rts/rtr`, with the addressing modes Dn, An, (An), (An)+, -(An), d16(An), d16(PC), absolute long and immediate. **Z80 subset**: `nop ret ld inc dec add/adc/sub/sbc/and/xor/or/cp push pop jp jr djnz call ex de,hl rlca rrca cpl scf ccf`. Anything else is `E_GUEST_SYNTAX`. Instructions that exist in the architecture but are forbidden in injected code (see ADR 0293) are accepted by the assembler so that the validator, not a missing mnemonic, rejects them with a precise class; a mnemonic that is not in the subset is a syntax error, never a guess. The subset is extended by adding table rows with a hand-assembled test vector each (`tests/mod_guest_test.py`).
4. **Trust and bounds model.** The source is untrusted input: size-capped (1 MiB, 8192 statements, 64 blocks, 64 free ranges), ASCII only, upper case refused, no ambiguity tolerated (an unknown label, duplicate label, out-of-range value, odd M68K instruction address, numeric branch target or manual symbol is a stable rejection). No process is spawned and no file besides the payload is read, so assembling cannot touch the host.
5. **Sega CD.** The same driver and M68K table serve the Sega CD sub CPU when disc-image mods arrive (the instruction set is identical); disc payload kinds stay reserved and `guest-asm` is `E_PAYLOAD_PLATFORM` on `sega-cd` until SEG-148 defines placement on a disc. The decision therefore does not need revisiting for that platform.
6. **Reproducibility.** The reproducibility proof is `tests/mod_guest_test.py`: byte-exact hand-assembled expectations for both CPUs, three runs identical, and the CI matrix (Linux and macOS in the fast gate) producing the same SHA-256.

## T004 scope confirmation

SEG-136-T004's scope is confirmed unchanged and is delivered in the same PR (combined delivery F05): integrate the selected assembler with bounded input, deterministic output and line-mapped diagnostics; static validation (size inside the free range, forbidden instructions, branch targets, writes outside declared RAM); the injected image is what the generated-native recompile sees. T005 (collisions, relocation ranges, header repair) and T006 (BPS) follow ADR 0293.

## License review

The assembler is original project code under the product license (MPL-2.0); nothing is vendored. No candidate's code, tables or documentation was copied. External tools named above are cited only as evaluated alternatives; the GPL-3.0 and vasm terms are the reason they cannot be bundled, and the product has no runtime dependency on them. The vasm statement rests on the author's published page (no OSI license, binaries only, no stable source); if its author grants a redistributable license later this ADR can be revisited, but R2 would still need a pinned source build.

## Consequences

- Authors write routines in a small, documented dialect; large programs are out of scope (use `native-c11`).
- A mod's routine is bytes in the image, so the recompiler sees ordinary generated code (no interpreter, no JIT, no runtime decoding).
- Unsupported mnemonics are a table addition, not a toolchain change.
