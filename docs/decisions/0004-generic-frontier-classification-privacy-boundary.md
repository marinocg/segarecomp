# ADR 0004: Generic Frontier Classification Within the Runtime-Stop Privacy Boundary

- Status: Accepted
- Date: 2026-08-21
- Amends: The T028 generalized-startup partial-program/runtime-bridge architecture contract's §14
  ("Runtime-stop privacy boundary") and §7 diagnostic-category table
  (`docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md`), consumed by SEG-007-T029
  and generalized by SEG-007-T064.

## Context

§14 of the T028 contract requires that any output a commercial (authorized local ROM) run persists,
prints, or commits be reduced to a sanitized report carrying only `result`, `stop_class`/
`diagnostic_category`, safe CPU dimensions, the ROM SHA-256, and (for `--compare-runs`) `reports_match`
-- "never a raw address, offset, opcode/extension word, disassembly, RAM byte, or local path." This rule,
and this project's parallel `docs/testing/commercial-games.md` policy ("never print or paste ROM bytes"),
exist to keep this repository's own tooling from ever materializing, printing, logging, or committing
verbatim copyrighted commercial ROM content -- correctly and deliberately conservative, and unchanged by
this decision.

In practice this conflated two different things under one boundary: (1) content genuinely derived from
or identifying _this specific ROM's own bytes_ (a literal address, opcode word, extension word, or byte
sequence -- material that, combined with public disassembly knowledge, could reconstruct or closely
approximate copyrighted code); and (2) a coarse classification _about_ an already-decoded frontier that
is a property of the public MC68000 ISA encoding itself, or of this project's own already-cited,
already-implemented, already-public address-space region facts -- material that carries no ROM-specific
content at all and would be identical for any program in existence exhibiting the same instruction form
or memory-access shape.

Category (2) was being needlessly withheld under category (1)'s rule. The practical cost, observed
directly across this milestone's own real-Sonic-ROM validation history (SEG-007-T041/T054/T055/T056/
T057/T058/T059/T060/T061/T062/T063/T064): every real-ROM frontier is reported only as a bare
`DirectFlowDiagnostic` category name (for example `valid_but_unsupported_instruction`) and a
`GenesisFrontierClass` name (for example `GENESIS_STOP_UNSUPPORTED_CPU_FORM`). Neither says _which_ of
the dozens of currently-unimplemented MC68000 instruction families is responsible, nor _which_
already-mapped region a memory frontier's target falls near. Every one of the tasks above had to close
this gap either by process of elimination against previously-eliminated categories, by a bounded,
temporary, letter-coded discriminator built and deleted for one narrow question, or simply by choosing
the next most-plausible capability to implement and re-running the real ROM to see whether it helped --
an expensive, slow, guess-and-check loop this decision exists to shorten.

## Decision

The privacy boundary is _not_ removed. It is refined to distinguish the two categories above, and to
name exactly, narrowly, what may now be surfaced:

1. **Opcode-line classification**, for a frontier classified `unsupported_cpu_form`
   (`GENESIS_STOP_UNSUPPORTED_CPU_FORM`). The frontier's already-legitimately-retained primary
   instruction word (`UnresolvedFrontier.diagnostic.provenance->bytes[0..1]`, already held today, just
   never surfaced) is reduced to the standard MC68000 "opcode line" -- the top 4 bits of the instruction
   word, the exact 16-way grouping every public 68000 reference and disassembler already uses (lines
   0000 through 1111; see for example Motorola's own M68000 Programmer's Reference Manual opcode map).
   Only the derived line name may be surfaced (for example `"miscellaneous"`, the actual name
   `m68k_opcode_line_name` returns for line 0100); the instruction
   word's own 16 raw bits, in any encoding (hex, decimal, binary, base64, or otherwise), may never be
   surfaced, in any report, log, or committed artifact, under any circumstance.

2. **Region-class classification**, for a frontier classified `unsupported_device_access` or
   `unsupported_memory_region`. The frontier's already-legitimately-retained target address
   (`UnresolvedFrontier.access->address`, already held today for exactly these two classes, just never
   surfaced) is reduced to one of the region buckets this project's own tooling _already_ computes and
   _already_ surfaces unconditionally, with no ROM at all, through the pre-existing, unconditional
   `probe-genesis-startup-mapping` CLI command: `raw_cartridge_rom`, `synthetic_work_ram`, or
   `hardware_frontier`; plus one additional bucket for the already-cited GTO1 controller-I/O window
   (`m68k_controller_io_region_begin`..+0x20, already used elsewhere in this codebase). Only the derived
   bucket name may be surfaced; the target address's own value, in any encoding, may never be surfaced.
   No Z80-bus, VDP, or other address range not already independently cited by this project's own prior
   research may be used to derive this classification -- `docs/architecture/genesis-persistent-
   device-state-and-checkpoint-evidence-contract.md` already deliberately records that those ranges are
   not yet independently verified facts of this project, pending future `console-developer` research; this
   decision does not shortcut that requirement or introduce an uncited hardware claim through the back
   door of a "generic classification."

Both classifications are pure functions of already-held, already-legitimate fields; neither requires
reading any additional ROM content, and neither introduces a second control-transfer mechanism, a
runtime opcode decoder, or any capability beyond a coarse, static, discovery-time bucket name.

Everything else §14 and §7 already establish is unchanged: no raw address, byte, opcode/extension word,
offset, disassembly, RAM content, local path, or trace may ever be surfaced by any commercial-ROM run,
in any report, log, or committed artifact -- this decision adds exactly two new, narrow, well-bounded
derived-classification fields to the existing sanitized report shape, nothing more.

## Clarification: Existing Semantic Classifiers

A normalized semantic classification may be retained when it is produced by an already implemented project classifier and does not expose reconstructable commercial content.

For example, a durable handoff may identify an instruction family, operand size, and addressing-mode class without retaining its instruction word, raw bytes, address, image offset, or disassembly. The same rule applies to normalized device regions, access widths, directions, and stable diagnostic classes.

This does not authorize adding an ad hoc commercial-ROM decoder merely to enrich a report. New classifications must be justified by the implementation task, based on public architecture documentation or redistributable fixtures, and tested without requiring the commercial image.

This clarification governs agent handoffs and durable derived findings. It does not add fields to the inventory scanner's public report or widen the two-field scanner-report decision established above.

## Consequences

- Reviewers checking privacy discipline must confirm any new frontier-report field is either (a) already
  on the pre-existing sanitized allowlist, or (b) a bucket-name-only classification derived by one of the
  two pure functions this decision authorizes, from one of the two already-held fields this decision
  names, using only already-public ISA structure or already-cited project facts -- never a new raw value
  and never a new, uncited hardware-region claim.
- Future real-ROM validation and implementation-selection tasks in this milestone gain a materially more
  actionable signal (which opcode line, which region bucket) without any new privacy exposure, reducing
  the guess-and-check cost this decision's Context section describes.
- Extending this same narrow classification approach to further, more granular buckets (for example a
  full opcode-mnemonic classifier, or newly-cited Z80/VDP region names) requires either direct reuse of
  facts a `console-developer` task has independently cited, or a further, separately justified revision
  of this decision -- it is not implicitly authorized by this one.
