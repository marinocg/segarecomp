# Executable-image contract (draft, SEG-027-T003)

- Status: **Draft**. This contract is input to the SEG-028 full refinement. It is not implemented, and no product code follows it yet.
- Decision context: ADR 0076 (SEG-028 ACTIVATE); evidence in `gen3-evidence-ledger.md` sections 3-4 (seams S1, S2, S6-S9).
- Real consumers that justify the seam:
  - **M68K:** ADR 0049 statically proven RAM copies (S2).
  - **Z80:** SMS immutable banked images (S7) and Genesis materialized RAM images (S8). Both are already expressed as
    `codegen::z80::CodeImage`.

## 1. Purpose and boundary

An *executable image* is the build-time artifact that says: these bytes, executed by this CPU, at these execution addresses, were
produced by this producer, and live bytes must be verified this way. It is the boundary between **producers** (where bytes come from,
Problem A) and **consumers**:

- broad AOT today;
- discovery/analysis later (SEG-029/030);
- admission policy later (SEG-031).

It is **not**:

- a loader;
- a memory model;
- a banking/mapper model;
- a runtime object;
- a discovery result.

The final executable never sees it. Only generated tables derived from it reach run time.

## 2. Answers to the 16 questions

1. **Minimum CPU-neutral concept.** An image is the following tuple:
   - `cpu`: a tag, the existing `CpuVariant`;
   - `identity`: see question 2;
   - `bytes`: an owned or referenced immutable byte span;
   - `mappings`: see question 4;
   - `provenance`: see question 6;
   - `verification`: see question 8.

   Nothing else is generic. There are no roots, no CFG, no admission decision and no runtime selection key.

2. **Image identity: layered.**
   - **Generic layer:** a build-local `ImageId`. It is a dense ordinal assigned in deterministic producer order and is unique per
     `(cpu, build)`. It is what consumers key on.
   - **CPU/codegen layer:** the generated entry key. For M68K, the execution PC. For Z80,
     `(CodeImage::identity << 16) | (PC or window offset)`. The CPU emitter derives it from `ImageId` and the mapping, and it stays
     CPU-owned.
   - **Platform layer:** runtime selection keys. Examples are the Genesis activation signature, the SMS bank number, and ADR 0049's
     fixed execution base. These stay platform-owned and are never in the generic type.

3. **Byte exposure without assuming a 16-bit address space.** `bytes` is a `std::span<const std::uint8_t>` with a `std::size_t` length.
   Offsets are `std::uint32_t` image offsets. Nothing in the generic type knows the CPU's address width.
   - A Z80 consumer rejects an image whose mappings leave its 16-bit logical space (the existing `CodeWindow` validation).
   - An M68K consumer rejects anything outside 24 bits.
   - A class-2 image (proven copy) *references* its source image's bytes (source `ImageId` plus offset range) rather than copying them,
     exactly as ADR 0049 decodes the immutable source.

4. **Execution mapping without SMS mapper semantics.** `mappings` is a non-empty list of
   `{execution_base: uint32, first_offset: uint32, length: uint32}`: image offset `o` executes at `execution_base + o`.
   - More than one mapping means the same bytes may execute at several bases. Examples are SMS slots 0-2 and Genesis Z80 RAM mirrors.
   - *Which* mapping is live at run time (the mapper register, the bank window, RAM mirroring) is platform semantics. It reaches the
     generated program only through the platform's existing tables (`sms_mapper_contract.h`, the Genesis Z80 window).
   - `ImageKind` (`invariant` vs `banked`) is a Z80 codegen property (direct binding allowed or not). The Z80 adapter derives it from
     the mapping count plus a platform flag. It is not generic.

5. **Architectural roots: a separate object.** Roots belong to the *discovery request* (SEG-029/030): reset and vector entries for
   M68K, platform entry points for Z80. They are not image fields.
   - Broad AOT needs no roots.
   - A RAM image's entry is reached by ordinary control flow.
   - Putting roots in the image would couple Problem A to Problem B.

6. **Producer provenance.** `provenance = {class, producer, evidence}`:
   - `class` is closed: `immutable_input`, `proven_copy`, `build_time_materialization`, `static_analysis` (ADR 0076 classification).
   - `producer` is a stable producer name (for example `genesis.cartridge`, `genesis.copy_alias`, `sms.cartridge_bank`,
     `genesis.z80_materializer`).
   - `evidence` is a producer-owned sanitized record. Examples:
     - for a proven copy: the source `ImageId`, source offset and length;
     - for materialization: the epoch ordinal, content digest and bounds used;
     - for an immutable input: the mapping claim.

   Evidence is build-artifact data. Durable reports keep only the class and counts.

7. **Distinguishing immutable input, guarded materialized snapshot and statically proven copy.** By `provenance.class` together with
   `verification`. The three combinations that exist today:

   | class | verification | example |
   | --- | --- | --- |
   | `immutable_input` | `none` | cartridge claims, SMS banks |
   | `proven_copy` | `byte_identity` | ADR 0049 |
   | `build_time_materialization` | `structural` | Genesis Z80 |

   A consumer must not infer the class from the address range.

8. **Verification policy.** The generic layer holds only a closed tag: `none` (bytes cannot change while mapped),
   `byte_identity` (every executed instruction's bytes must equal the image bytes), or `structural` (CPU-defined structural bytes
   must be equal; the CPU defines which bytes are live payload). The implementation is CPU/codegen-owned:
   - the M68K alias guard;
   - the Z80 `z80_live_guard` mask from `FormDescriptor`.

   The *consequence* of a mismatch is also CPU/platform policy: an M68K typed stop, or a Genesis sound-CPU fault.

9. **`CodeImage` / `ImageSet`: they stay Z80 codegen input, and an adapter projects onto them.**
   - `CodeImage` carries Z80-only facts: a 16-bit identity, `uint16` window bases, `ImageKind` direct-binding rules, and a 4-byte
     `live_bytes` guard. Promoting it to the generic type would export all of these.
   - The Z80 adapter is a pure function `executable images -> codegen::z80::ImageSet`. It must reproduce today's `ImageSet` exactly
     for SMS and Genesis.

10. **`PassRunner`: it stays Genesis-specific.** Its single consumer is `segarecomp build`'s Genesis route. The ADR 0049 preparation
    loop is structurally different: it runs in Python tooling, iterates on fail-closed frontiers and finds verbatim runs, while
    `PassRunner` iterates on hardware epochs. The generic part of materialization is only the *provenance class* and the rule that a
    class-3 producer is deterministic, bounded, confirmed and fails with typed errors. Revisit if a second class-3 producer appears
    (for example, M68K decompressed code).

11. **How ADR 0049 consumes the seam without changing execution semantics.**
    - A descriptor becomes a class-2 producer output: an M68K image that references the source claim's bytes, with one mapping at
      the work-RAM execution base, `verification = byte_identity`, and evidence = (source image, offset, length).
    - The frontend alias path (`apply_genesis_immutable_copy_alias`, `ImmutableRomAotEntry::execution_alias`) consumes that image
      instead of the raw `ImmutableCopyAlias` triple.
    - Decode, guard and provenance are unchanged, so generated C is **byte-identical** for the same descriptors. This adaptation is
      intentionally emission-neutral.
    - Moving alias production into `segarecomp build` is a separate, later child. It is behaviour-adding, has its own acceptance, and
      is not part of the equivalence proof.

12. **How a future static analyzer replaces the SEG-032 producer.**
    - A class-4 producer must emit the same platform inputs the registry consumes today: per epoch, the 8 KiB content and the
      68K-written extents that define the activation signature.
    - The generic seam carries the image and its provenance. The Genesis registry keeps computing the content hash and signature from
      the platform record.
    - Replacement is proven by registry equality (the same ordered `(content, signature)` set) on the materialization test corpus.
      With an equal registry, AOT, runtime selection and devices are unchanged by construction (ADR 0073 decision 8).
    - The `PassRunner` loop then becomes one producer implementation among two. It does not need to be generic.

13. **Multiple image variants.** Several images may map the same execution range only when the platform supplies a fail-closed
    selection mechanism. Today these are:
    - Genesis activation signatures (several Z80 images at `$0000`);
    - SMS banks (several images in one slot via the mapper).

    For M68K aliases, overlapping descriptors are rejected (ADR 0049), and the generic validator keeps that rule unless the platform
    declares a selector. Each variant has its own `ImageId`.

14. **Keeping structural self-modification unsupported without polluting the generic model.** The generic model knows only "live bytes
    may differ, so verification is required". What counts as structural, and what happens on a mismatch, are CPU/platform facts
    (question 8). There is no `self_modifying` flag, no per-write image and no multi-form-per-PC concept in the generic type.

15. **Deterministic identity and hash contract across CPUs.** Only two rules:
    - `ImageId` order is a pure function of producer order and producer output;
    - any content digest is SHA-256 over the image bytes and is computed by the producer that needs it.

    There is no cross-CPU hash requirement. The Genesis content hash and activation signature keep their ADR 0073 definitions.

16. **What must not be generalized yet.**
    - Genesis concepts: epochs, the hold window, activation signatures, the `Registry` bound, 8 KiB and the two-mirror window, and
      `PassRunner`.
    - Z80 concepts: 16-bit windows, `ImageKind`, the 4-byte live guard and structural mask, and direct binding rules.
    - SMS slot admissibility and mapper registers.
    - ADR 0049 discovery: the maximal verbatim run search and preparation rounds.
    - M68K cartridge mirroring (ADR 0049 decision 5).
    - Runtime entry tables beyond the already-neutral `CompiledEntryTable`.
    - Any decompression or transform semantics.

## 3. Proposed placement (to be confirmed by SEG-028's first child)

- The generic artifact goes in a header in `libs/recompiler` (CPU-neutral plan contracts; no ISA types). It uses `CpuVariant` from
  `libs/core`.
- Producers stay in their platform libraries: `platforms/genesis/machine` for cartridge claims, aliases and the materializer;
  `platforms/master-system/machine` for banks.
- The Z80 adapter goes in `libs/codegen/c11` next to `z80.hpp`, or in each platform's `build_image_set`.
- The M68K adapter goes in `platforms/genesis/machine` (frontend).

The dependency direction is unchanged: platforms depend on generic; generic depends on no CPU or platform.

## 4. Invariants every SEG-028 child must preserve

- The final executable stays fully static (ADR 0076). The build may materialize only under the class-3 rules.
- Broad AOT over any image remains legal and is the default admission.
- Equivalence for adaptations follows the SEG-027-T003 no-regression rule:
  - byte-identical generated output where the adaptation is emission-neutral (both planned adaptations are);
  - otherwise the full equivalence set: same admitted identities, same execution, same fail-closed behaviour, equivalent artifacts,
    deterministic output, no runtime decoder, no compatibility regression.
- No Genesis-epoch/signature, 8 KiB, 16-bit-window or 4-byte-guard concept in the generic part. This is enforced by a forbidden-identifier
  scan of the generic header, mirroring `genesis_z80_forbidden_identifiers_test`.
