# Executable-image contract (SEG-027-T003, accepted by SEG-028-T001)

- Status: **Accepted (ADR 0077)**. Drafted by SEG-027-T003; implemented by SEG-028-T003/T004 and checked against the code by
  SEG-028-T001. Each answer below is marked **Confirmed** (the draft holds as written) or **Corrected** (the implementation differs; the
  correction is authoritative), with a code reference.
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

   **Corrected.** `cpu` is a set-level field, not a per-image field: `ExecutableImageSet {cpu, platform_selector, images}` and
   `ExecutableImage {id, bytes | source, mappings, provenance, verification}` (`executable_image.hpp`). `bytes` is owned or a source
   reference (question 3).

2. **Image identity: layered.**
   - **Generic layer:** a build-local `ImageId`. It is a dense ordinal assigned in deterministic producer order and is unique per
     `(cpu, build)`. It is what consumers key on.
   - **CPU/codegen layer:** the generated entry key. For M68K, the execution PC. For Z80,
     `(CodeImage::identity << 16) | (PC or window offset)`. The CPU emitter derives it from `ImageId` and the mapping, and it stays
     CPU-owned.
   - **Platform layer:** runtime selection keys. Examples are the Genesis activation signature, the SMS bank number, and ADR 0049's
     fixed execution base. These stay platform-owned and are never in the generic type.

   **Confirmed**, with precision: `ImageId` is a dense **1-based** ordinal (`0` is invalid; image `i` carries `i + 1`, else
   `id_not_dense`), unique per set. The Z80 projection sets `CodeImage::identity = ImageId` and rejects ids above `0xFFFF`
   (`executable_image.hpp` `ImageId`, `validate_executable_image_set`; `z80_executable_image.cpp` `project_executable_images`).
   Producers order images so that `ImageId` equals the historical identity (SMS `SMS_IMAGE_FIXED`/`SMS_IMAGE_BANK_FIRST`, Genesis
   registry ordinal).

3. **Byte exposure without assuming a 16-bit address space.** `bytes` is a `std::span<const std::uint8_t>` with a `std::size_t` length.
   Offsets are `std::uint32_t` image offsets. Nothing in the generic type knows the CPU's address width.
   - A Z80 consumer rejects an image whose mappings leave its 16-bit logical space (the existing `CodeWindow` validation).
   - An M68K consumer rejects anything outside 24 bits.
   - The span may be owned or borrowed. An ADR 0049 alias image borrows its source claim's bytes rather than copying them, exactly as
     ADR 0049 decodes the immutable source. The source image and offset range are recorded as producer evidence (question 6), not as
     a generic derivation field.

   **Corrected.** There is no borrowed `std::span` in the artifact. An image either owns `std::vector<std::uint8_t> bytes` or carries an
   `ImageSourceReference {source, offset, length}` to an *earlier* image of the same set that owns its bytes; exactly one is present.
   Sets are therefore copy-safe. `executable_image_bytes(set, image)` resolves a span on demand. The no-copy relationship is a validated
   generic field (`unresolved_source`, `source_out_of_range`), and the derivation meaning stays producer evidence
   (`executable_image.hpp` `ImageSourceReference`, `ExecutableImage`, `executable_image_bytes`). Image offsets, bases and lengths are
   `std::uint32_t`; CPU width limits stay in the consumer (Z80: `project_executable_images`; M68K: the work-RAM window check in
   `genesis_m68k_executable_images`).

4. **Execution mapping without SMS mapper semantics.** `mappings` is a non-empty list of
   `{execution_base: uint32, first_offset: uint32, length: uint32}`: image offset `o` executes at `execution_base + o`.
   - More than one mapping means the same bytes may execute at several bases. Examples are SMS slots 0-2 and Genesis Z80 RAM mirrors.
   - *Which* mapping is live at run time (the mapper register, the bank window, RAM mirroring) is platform semantics. It reaches the
     generated program only through the platform's existing tables (`sms_mapper_contract.h`, the Genesis Z80 window).
   - `ImageKind` (`invariant` vs `banked`) is a Z80 codegen property (direct binding allowed or not). The Z80 adapter derives it from
     the mapping count plus a platform flag. It is not generic.

   **Corrected** in two details. (a) The validator also requires each mapping to be non-empty (`empty_mapping`), inside the image bytes
   (`mapping_outside_bytes`), and its execution range, computed in 64 bits, not to exceed the 32-bit execution space (`mapping_wraps`).
   (b) `ImageKind` is not derived from the mapping count: the platform supplies it per image (`kinds` argument of
   `project_executable_images`; SMS `image_kinds`, Genesis all `banked`). The Genesis Z80 RAM mirror is **not** a second mapping: the
   image bytes are the 16 KiB window (the 8 KiB snapshot twice) with one mapping at `$0000`, which keeps the projected `CodeImage` and the
   generated output byte-identical; the mirror count is producer evidence `ram_mirrors` (`z80_images.cpp` `executable_images`).

5. **Architectural roots: a separate object.** Roots belong to the *discovery request* (SEG-029/030): reset and vector entries for
   M68K, platform entry points for Z80. They are not image fields.
   - Broad AOT needs no roots.
   - A RAM image's entry is reached by ordinary control flow.
   - Putting roots in the image would couple Problem A to Problem B.

   **Confirmed.** `ExecutableImage` has no root field (`executable_image.hpp`).

6. **Producer provenance.** `provenance = {authority, producer, evidence}`:
   - `authority` is closed, with three values (ADR 0076 "Image authority"). It says how the compiler establishes the bytes:
     - `immutable_input`: the validated input image;
     - `static_proof`: static reasoning over immutable input, with no guest execution;
     - `bounded_build_time_materialization`: observed while running the generated-native program under the bounded build-time
       execution rules.

     A future analysis-based producer uses `static_proof`. It is not a separate authority.
   - `producer` is a stable producer name (for example `genesis.cartridge`, `genesis.copy_alias`, `sms.cartridge_bank`,
     `genesis.z80_materializer`).
   - `evidence` is a producer-owned sanitized record. It carries the **derivation relationship**, which stays producer-owned. Examples:
     - ADR 0049: a verbatim source-image copy, with the source `ImageId`, source offset and length;
     - Genesis materialization: the runnable-epoch ordinal, content digest, signature facts and bounds used;
     - immutable input: the mapping claim.

     The generic artifact has **no closed derivation taxonomy** (copy, transform, decompression and so on). SEG-028 may promote a
     derivation field only when two real consumers need it.

   Evidence is build-artifact data. Durable reports keep only the authority, the producer name and counts.

   **Confirmed**, with precision: `ImageProvenance {authority, producer, evidence}`; evidence is an ordered list of
   `ImageEvidenceFact {name, value}` and always includes a `derivation` fact (`mapping_claim`, `verbatim_source_copy` or
   `runnable_epoch_snapshot`). Producer names must match `[a-z0-9_.]+` (`invalid_producer_name`). The four names above are the ones in
   use. Sanitized reporting is `count_image_provenance` / `format_image_provenance_json` (counts by authority and by producer only);
   the SMS emitter records it in `EmitOutcome::provenance` (`master_system/src/emit.cpp`).

7. **Distinguishing immutable input, guarded materialized snapshot and statically proven copy.** By `provenance.authority`, the
   producer evidence and `verification` together. The combinations that exist today:

   | authority | producer evidence | verification | example |
   | --- | --- | --- | --- |
   | `immutable_input` | mapping claim | `none` | cartridge claims, SMS banks |
   | `static_proof` | verbatim source-image copy | `byte_identity` | ADR 0049 |
   | `bounded_build_time_materialization` | runnable-epoch snapshot and signature facts | `structural` | Genesis Z80 |

   A future static producer replacing Genesis materialization would be `static_proof`, with evidence of the same platform image and
   verification unchanged. Authority and verification are independent axes. A consumer must not infer either from the address range.

   **Confirmed.** The three rows are exactly what the producers emit: `sms.cartridge_bank` and `genesis.cartridge` (`immutable_input`,
   `none`), `genesis.copy_alias` (`static_proof`, `byte_identity`), `genesis.z80_materializer` (`bounded_build_time_materialization`,
   `structural`) (`image_set.cpp` `executable_images`; `frontend.cpp` `genesis_m68k_executable_images`; `z80_images.cpp`
   `executable_images`).

8. **Verification policy.** The generic layer holds only a closed tag: `none` (bytes cannot change while mapped),
   `byte_identity` (every executed instruction's bytes must equal the image bytes), or `structural` (CPU-defined structural bytes
   must be equal; the CPU defines which bytes are live payload). The implementation is CPU/codegen-owned:
   - the M68K alias guard;
   - the Z80 `z80_live_guard` mask from `FormDescriptor`.

   The *consequence* of a mismatch is also CPU/platform policy: an M68K typed stop, or a Genesis sound-CPU fault.

   **Confirmed.** `ImageVerification` is closed with three values; out-of-range values fail validation (`unknown_verification`). The Z80
   projection maps `structural` to `CodeImage::live_bytes` and rejects `byte_identity` (`project_executable_images`).

9. **`CodeImage` / `ImageSet`: they stay Z80 codegen input, and an adapter projects onto them.**
   - `CodeImage` carries Z80-only facts: a 16-bit identity, `uint16` window bases, `ImageKind` direct-binding rules, and a 4-byte
     `live_bytes` guard. Promoting it to the generic type would export all of these.
   - The Z80 adapter is a pure function `executable images -> codegen::z80::ImageSet`. It must reproduce today's `ImageSet` exactly
     for SMS and Genesis.

   **Corrected** (placement only). The adapter is `codegen::z80::project_executable_images(set, kinds)` in its own target
   `segarecomp::codegen_c11_z80_image` (`z80_executable_image.hpp`/`.cpp`), so `segarecomp::codegen_c11_z80` keeps its pinned links.
   Each platform's `build_image_set` is now its producer followed by this projection, and is byte-identical to the historical builder
   (`image_set.cpp`, `z80_images.cpp` `build_image_set`, `emit_registry`).

10. **`PassRunner`: it stays Genesis-specific.** Its single consumer is `segarecomp build`'s Genesis route. The ADR 0049 preparation
    loop is structurally different: it runs in Python tooling, iterates on fail-closed frontiers and finds verbatim runs, while
    `PassRunner` iterates on hardware epochs. The generic part of materialization is only the `bounded_build_time_materialization`
    authority value, plus the bounded build-time execution rules: deterministic, bounded, confirmed, typed failures. Revisit if a second
    producer with that authority appears (for example, M68K decompressed code).

    **Confirmed.** `PassRunner` is untouched; the materializer is described only through `z80::ImageProducer` defaults
    (`z80_images.hpp`).

11. **How ADR 0049 consumes the seam without changing execution semantics.**
    - A descriptor becomes the output of a producer with `static_proof` authority: an M68K image that borrows the source claim's
      bytes, with one mapping at the work-RAM execution base, `verification = byte_identity`, and producer evidence = verbatim
      source-image copy (source image, offset, length). The authority is `static_proof` because the emitter re-validates every
      descriptor against the owned immutable claim. The *origin* of a descriptor is producer evidence and does not change the
      authority:
      - supplied directly;
      - proposed by the tooling preparation phase;
      - proved by a future analysis.
    - The frontend alias path (`apply_genesis_immutable_copy_alias`, `ImmutableRomAotEntry::execution_alias`) consumes that image
      instead of the raw `ImmutableCopyAlias` triple.
    - Decode, guard and provenance are unchanged, so generated C is **byte-identical** for the same descriptors. This adaptation is
      intentionally emission-neutral.
    - Giving `segarecomp build` an alias producer is a separate, later child. It is behaviour-adding, has its own acceptance, and is
      not part of the equivalence proof. If that producer proposes descriptors by executing generated code at build time, that
      execution must satisfy the bounded build-time execution rules. The images it yields still have `static_proof` authority.

    **Corrected** (mechanism). The alias image's bytes are an `ImageSourceReference` into a `genesis.cartridge` image of the same set,
    not a borrowed span. The producer is `genesis_m68k_executable_images(program)` in `platforms/genesis/machine` (`frontend.hpp`),
    returning the set plus each image's claim index; `populate_immutable_rom_aot_entries` (`frontend.cpp`) consumes its `static_proof`
    images. `apply_genesis_immutable_copy_alias` and `ImmutableCopyAlias` remain the descriptor input to that producer. Emission is
    byte-identical. The consumer-route producer is the T005 go/no-go in ADR 0077.

12. **How a future static analyzer replaces the SEG-032 producer.**
    - A replacement producer with `static_proof` authority must emit the same platform inputs the registry consumes today: per epoch, the 8 KiB content and the
      68K-written extents that define the activation signature.
    - The generic seam carries the image and its provenance. The Genesis registry keeps computing the content hash and signature from
      the platform record.
    - Replacement is proven by registry equality (the same ordered `(content, signature)` set) on the materialization test corpus.
      With an equal registry, AOT, runtime selection and devices are unchanged by construction (ADR 0073 decision 8).
    - The `PassRunner` loop then becomes one producer implementation among two. It does not need to be generic.

    **Confirmed.** `z80::registries_equal` (ordinal, content hash, signature, snapshot, in order) is the equality; a replacement fills a
    registry and declares itself through `z80::ImageProducer {authority = static_proof, name}` (`z80_images.hpp`). A test stand-in
    exercises the path; no real static producer exists.

13. **Multiple image variants.** Several images may map the same execution range only when the platform supplies a fail-closed
    selection mechanism. Today these are:
    - Genesis activation signatures (several Z80 images at `$0000`);
    - SMS banks (several images in one slot via the mapper).

    For M68K aliases, overlapping descriptors are rejected (ADR 0049), and the generic validator keeps that rule unless the platform
    declares a selector. Each variant has its own `ImageId`.

    **Corrected.** The selector is a **set-level** flag, `ExecutableImageSet::platform_selector`, not a per-platform or per-image
    declaration. Intra-image overlap is always rejected; inter-image overlap is allowed only when the flag is set
    (`overlapping_mappings`). SMS `sega` mapper sets and Genesis Z80 sets set it; the SMS `rom_only` set (one image) and the Genesis M68K
    set do not (`validate_executable_image_set`; `image_set.cpp`, `z80_images.cpp` `executable_images`).

14. **Keeping structural self-modification unsupported without polluting the generic model.** The generic model knows only "live bytes
    may differ, so verification is required". What counts as structural, and what happens on a mismatch, are CPU/platform facts
    (question 8). There is no `self_modifying` flag, no per-write image and no multi-form-per-PC concept in the generic type.

    **Confirmed** (`executable_image.hpp`; enforced by `tests/executable_image_forbidden_identifiers_test.py`).

15. **Deterministic identity and hash contract across CPUs.** Only two rules:
    - `ImageId` order is a pure function of producer order and producer output;
    - any content digest is SHA-256 over the image bytes and is computed by the producer that needs it.

    There is no cross-CPU hash requirement. The Genesis content hash and activation signature keep their ADR 0073 definitions.

    **Confirmed.** No digest is computed in the generic header. The Genesis digests are producer evidence computed by the registry
    (`z80_images.cpp`); provenance JSON contains counts only and is ordered deterministically.

16. **What must not be generalized yet.**
    - Genesis concepts: epochs, the hold window, activation signatures, the `Registry` bound, 8 KiB and the two-mirror window, and
      `PassRunner`.
    - Z80 concepts: 16-bit windows, `ImageKind`, the 4-byte live guard and structural mask, and direct binding rules.
    - SMS slot admissibility and mapper registers.
    - ADR 0049 discovery: the maximal verbatim run search and preparation rounds.
    - M68K cartridge mirroring (ADR 0049 decision 5).
    - Runtime entry tables beyond the already-neutral `emit_compiled_entry_table` (`compiled_entry_table.hpp`).
    - Any decompression or transform semantics.

    **Confirmed** (ADR 0077 decision 16). Added: no generic CPU address-width check and no derivation taxonomy.

## 3. Placement (final, ADR 0077)

- The generic artifact is the header-only `libs/recompiler/include/segarecomp/recompiler/executable_image.hpp`
  (`segarecomp::recompiler`, which links only `segarecomp::base`). It uses `CpuVariant` from `libs/core` (`segarecomp/core/address.hpp`).
- Producers stay in their platform libraries: `platforms/genesis/machine` for cartridge claims, aliases (`frontend.cpp`) and the Z80
  materializer (`z80_images.cpp`); `platforms/master-system/machine` for banks (`image_set.cpp`).
- The Z80 adapter is its own target, `segarecomp::codegen_c11_z80_image` in `libs/codegen/c11`, linking `codegen_c11_z80` and
  `recompiler`; `codegen_c11_z80` itself is unchanged.
- There is no separate M68K adapter: `populate_immutable_rom_aot_entries` in the Genesis frontend consumes the M68K images directly.

The dependency direction is unchanged: platforms and codegen depend on the generic header; it depends on no CPU, codegen or platform
library.

### Implementation map

| concept | file | symbol |
| --- | --- | --- |
| artifact, tags, validator, provenance counts | `libs/recompiler/include/segarecomp/recompiler/executable_image.hpp` | `ExecutableImageSet`, `ExecutableImage`, `ImageSourceReference`, `validate_executable_image_set`, `executable_image_bytes`, `count_image_provenance`, `format_image_provenance_json` |
| Z80 projection | `libs/codegen/c11/include/segarecomp/codegen/c11/z80_executable_image.hpp`, `src/z80_executable_image.cpp` | `codegen::z80::project_executable_images` |
| SMS producer (`sms.cartridge_bank`) | `platforms/master-system/machine/src/image_set.cpp` | `executable_images`, `image_kinds`, `build_image_set` |
| SMS sanitized provenance | `platforms/master-system/machine/src/emit.cpp` | `emit_cartridge` (`EmitOutcome::provenance`) |
| Genesis Z80 producer (`genesis.z80_materializer`) | `platforms/genesis/machine/src/z80_images.cpp` | `z80::executable_images`, `z80::ImageProducer`, `z80::registries_equal`, `build_image_set`, `emit_registry` |
| Genesis M68K producer (`genesis.cartridge`, `genesis.copy_alias`) | `platforms/genesis/machine/src/frontend.cpp` | `genesis_m68k_executable_images`, `GenesisM68kExecutableImages` |
| ADR 0049 consumer | `platforms/genesis/machine/src/frontend.cpp` | `populate_immutable_rom_aot_entries` (alias loop) |
| forbidden-identifier scan | `tests/executable_image_forbidden_identifiers_test.py` | |

## 4. Invariants every SEG-028 child must preserve

- The final executable stays fully static (ADR 0076). The build may execute generated code only under the bounded build-time execution rules (ADR 0076).
- Broad AOT over any image remains legal and is the default admission.
- Equivalence for adaptations follows the SEG-027-T003 no-regression rule:
  - byte-identical generated output where the adaptation is emission-neutral (both are: T003 for the SMS and Genesis Z80 producers,
    T004 for the M68K producer and the ADR 0049 frontend loop);
  - otherwise the full equivalence set: same admitted identities, same execution, same fail-closed behaviour, equivalent artifacts,
    deterministic output, no runtime decoder, no compatibility regression.
- No Genesis-epoch/signature, 8 KiB, 16-bit-window or 4-byte-guard concept in the generic part. This is enforced by a forbidden-identifier
  scan of the generic header, mirroring `genesis_z80_forbidden_identifiers_test`.
