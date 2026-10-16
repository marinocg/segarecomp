# Sega CD

**Status:** SEG-062 implemented (synthetic profile); contract in [docs/architecture/sega-cd-machine-contract.md](../../docs/architecture/sega-cd-machine-contract.md)
(SEG-062). Decisions: ADR 0126 (executable images per processor instance), 0127 (overlay dispatch, design only), 0128
(dual-CPU execution and scheduling), 0129 (disc resources and media boundary), 0130 (BIOS profiles and gate), 0131 (ML reuse).

**Purpose:** Genesis plus a second MC68000 (the sub CPU), the Gate Array, Word RAM and the CD drive/CDC data path.

**Profile:** synthetic post-BIOS fixture profile only: project-authored discs, no BIOS executed or present, no
commercial software. Where the reference emulators disagree the runtime follows Genesis Plus GX (consulted, never copied;
contract tag G, ADR 0128).

**Code:** `machine/` (build time: boot images from `libs/media` disc ingestion, two-processor program emission over the
generic M68K lowering, `segarecomp sega-cd inspect|emit`); `runtime/` (strict C11: scheduler, buses, Gate Array, Word RAM,
timer, CDD/CDC, disc reader, driver). Fixtures: `tools/sega_cd_fixture_disc.py`. Tests: `tests/sega_cd_*`.

**Reuse:** `libs/cpu/m68k` and the generic M68K emitter instantiated twice (one `ExecutableImageSet` and one
`M68kRuntimeCEmitter` per processor instance, both `CpuVariant::mc68000`); the Genesis VDP device and frame export.

**Milestones:** SEG-062 (synthetic disc, dual generated-native M68K, CD data path); SEG-063 (modules and overlays);
SEG-064 (authentic BIOS, gated); SEG-065 (graphics ASIC, PCM, CD-DA, viewer).
