# ADR 0126: Sega CD executable images: one `ExecutableImageSet` per processor instance

- Status: Accepted (SEG-062-T001). Implemented by SEG-062-T002 (producer) and T003 (consumer).
- Related: ADR 0077 (executable-image artifact), ADR 0044 (translation-unit sharding), ADR 0049/0122 (byte guards),
  `docs/architecture/sega-cd-machine-contract.md`.

## Context

The Sega CD runs two MC68000 processors. Both are the same instruction-set variant; they differ in bus view, interrupt
wiring and clock. Their programs arrive from a disc: the boot IP (main) and SP (sub) from the system area, and later
(SEG-063) file-loaded modules, possibly several at one address and one module at several addresses. The generic
`ExecutableImageSet` (ADR 0077) is CPU-neutral and its header is guarded against machine vocabulary.

## Decision

1. **No new ISA tag.** Both sets carry `CpuVariant::mc68000`. `CpuVariant` names an instruction set, not a socket.
2. **Processor instances are platform identity.** `platforms/sega-cd` defines `enum class SegaCdProcessor { main,
   sub }`. One `ExecutableImageSet` is produced per instance; the pairing (instance -> set) is held by the Sega CD
   producer/consumer, never by the generic header. Each instance has its own architectural state, bus view, interrupt
   state, timing state, generated execution context (state names, call frames) and compiled-entry selection.
3. **Five layers stay separate**, each with one owner:

   | layer | owner | example |
   | --- | --- | --- |
   | disc storage extents | `libs/media` (`DiscImage`) | "IP is bytes `[$200, $800)` of LBA 0..15 of track 1" |
   | executable images | generic (`ExecutableImageSet`) | bytes + mappings + provenance + verification |
   | generated code blocks | codegen (`M68kRuntimeCEmitter`, one instance per processor) | one function per compiled instruction |
   | host C translation units | ADR 0044 sharding (unchanged) | one file for the fixtures |
   | runtime activation identity | platform (`platforms/sega-cd/runtime`) | "sub PC `$6000` selects compiled entry N of the sub set" |

   Disc extents appear only as producer **evidence facts** (`ImageEvidenceFact{"disc.extent", "lba:..."}`), which are
   build-artifact data and never generated-code inputs.
4. **How each case is expressed with the unchanged header**:

   | case | expression | validator rule exercised |
   | --- | --- | --- |
   | IP/SP boot images | one image per set, `immutable_input`, producer `sega_cd.boot_ip` / `sega_cd.boot_sp`, mapping at the profile base, `byte_identity` (RAM-resident code) | dense ids, known tags |
   | disc-loaded module | one image, `immutable_input`, producer `sega_cd.disc_module`, evidence names the extent | same |
   | one module at several addresses | one image, several `ImageMapping`s | intra-image overlap rejected |
   | several modules at one PC | several images, `platform_selector = true` | overlap across images accepted only with the selector |
   | static-proof copy | `static_proof` image with `ImageSourceReference` to the module it copies | source resolution |
   | bounded materialized RAM code | `bounded_build_time_materialization`, `byte_identity` | authority tag |

5. **Exact gaps** (none is needed by SEG-062; all belong to SEG-063):
   - *Dispatch key beyond PC*: with `platform_selector`, the runtime needs a key (instance, live-byte identity) to pick
     among images at one PC. This is an **extension** of the platform consumer (ADR 0127), not of the header.
   - *Image-to-extent link at run time*: not needed; byte identity is checked against image bytes, not disc extents.
   - Nothing requires a **refactor** of the generic contract.

## Go/no-go criteria (falsifiable)

- GO for SEG-062-T003 if the M68K emitter can be instantiated twice with per-instance state names and a
  per-instance `M68kRuntimeCEmitter` without any change to `libs/cpu/m68k` semantics or the generic header (checked:
  the emitter's routed protocol is entirely caller text; `tests/m68k_reuse_boundary_test.py` proves state
  independence). NO-GO if a CPU-owned file must name a Sega CD concept.
- The model test `tests/sega_cd_executable_image_test.cpp` must accept every row of the table above and reject the
  mutated cases; a mutated validator that allows cross-image overlap without `platform_selector` must make it fail
  (`tests/sega_cd_executable_image_mutation_test.py`).

## Consequences

- No universal CPU interface, no new IR, no second CPU variant. The forbidden-identifier scan of the generic header
  is extended with disc and Sega CD vocabulary.
