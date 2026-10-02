# ADR 0077: Executable-Image Artifact and Producer Boundary

- Status: Accepted (SEG-028-T001).
- Date: 2026-10-02
- Task: SEG-028-T001..T007 (T001 accepts the contract; later children append their records below).
- Contract: `docs/architecture/executable-image-contract.md` (Accepted by this ADR; each of its 16 answers is marked Confirmed or
  Corrected against the implementation).
- Related: ADR 0076 (SEG-028 ACTIVATE, "Image authority"), ADR 0049 (immutable-copy alias), ADR 0073 (Genesis Z80 build-time
  materialization), ADR 0065 (SMS mapper contract table), ADR 0058/0071 (broad Z80 AOT). Evidence: `docs/architecture/gen3-evidence-ledger.md`
  sections 3-4 (seams S1, S2, S6-S9).

## Context

ADR 0076 activated SEG-028 (Problem A: where executable bytes come from, and under which identity, mapping and provenance they may be
compiled statically). Three real producers already exist and share no type [ledger S2, S7, S8]:

- the ADR 0049 M68K alias (a statically proven verbatim copy, guarded per instruction, descriptor-based);
- the SMS cartridge banks (immutable, selected by the mapper);
- the Genesis Z80 registry (build-time materialized, guarded structurally, selected by activation signature).

`codegen::z80::CodeImage` already serves two Z80 producers, but it is Z80-shaped (16-bit identity and windows, `ImageKind`, a 4-byte
live guard). M68K has no artifact type: alias identities are rows of `FrontendAnalysis`. The draft contract proposed a minimal
CPU-neutral artifact between producers and consumers. This ADR fixes its final shape, placement and validation rules, and records how
each existing producer is re-expressed without changing generated output.

## Decision

1. **Placement and dependency direction.** The artifact is the header-only
   `libs/recompiler/include/segarecomp/recompiler/executable_image.hpp` (target `segarecomp::recompiler`, which links only
   `segarecomp::base`). Its CPU tag is the existing `CpuVariant` from `libs/core` (`segarecomp/core/address.hpp`). It names no CPU,
   console, device or platform concept. Platforms and codegen depend on it; it depends on no CPU, codegen or platform library.
   - The Z80 projection lives in its own target, `segarecomp::codegen_c11_z80_image`
     (`libs/codegen/c11/include/segarecomp/codegen/c11/z80_executable_image.hpp`), which links `codegen_c11_z80` and `recompiler`.
     `segarecomp::codegen_c11_z80` keeps its pinned links (`codegen_c11`, `cpu_z80`).
   - The M68K producer and its consumption live in `platforms/genesis/machine` (frontend). There is no separate M68K adapter target.
   - The SMS producer lives in `platforms/master-system/machine` (`image_set.cpp`); the Genesis Z80 producer in
     `platforms/genesis/machine` (`z80_images.cpp`).

2. **The artifact.** `ExecutableImageSet {cpu, platform_selector, images}`; `ExecutableImage {id, bytes | source, mappings, provenance,
   verification}`. Nothing else is generic: no roots, no CFG, no admission decision, no runtime selection key, no derivation taxonomy.

3. **Identity is layered.**
   - Generic: `ImageId`, a dense 1-based ordinal in deterministic producer order (`0` is never valid). Image `i` (0-based) carries
     `ImageId{i + 1}`; the validator rejects anything else (`id_not_dense`).
   - CPU/codegen: the generated entry key stays CPU-owned. For Z80, `CodeImage::identity = ImageId` (the projection rejects an id above
     `0xFFFF`), so the entry key remains `(identity << 16) | (PC or window offset)`. For M68K the key stays the execution PC.
   - Platform: runtime selection keys (SMS bank number, Genesis activation signature, the ADR 0049 work-RAM base) stay platform-owned and
     never enter the generic type. Producers choose their order so that `ImageId` equals the historical identity (SMS fixed image 1 and
     banks from `SMS_IMAGE_BANK_FIRST`; Genesis Z80 registry ordinal).

4. **Bytes are owned, or a source reference; never a borrowed pointer.** An image either owns `std::vector<std::uint8_t> bytes` or
   carries `ImageSourceReference {source, offset, length}` naming an *earlier* image of the same set that owns its bytes. Exactly one
   of the two is present. A set is therefore self-contained and safe to copy or move; `executable_image_bytes(set, image)` resolves a
   reference to a span on demand. Chained references are rejected (`unresolved_source`), and the referenced range must be non-empty and
   inside the source (`source_out_of_range`).

5. **Mappings.** A non-empty list of `ImageMapping {execution_base, first_offset, length}`: image offset `o` in
   `[first_offset, first_offset + length)` executes at `execution_base + o`. Each mapping must be non-empty, lie inside the image bytes,
   and its execution range, computed in 64 bits, must not exceed the 32-bit execution space (`mapping_wraps`). CPU address-width limits
   (16-bit Z80, 24-bit M68K) are checked by the CPU consumer, not by the generic validator. Which mapping is live at run time is
   platform semantics.

6. **Overlap rule.** Two mappings of the same image never share an execution address. Mappings of different images may overlap only
   when the set declares `platform_selector = true`, meaning the platform supplies a fail-closed run-time selection mechanism.
   - SMS `sega` mapper sets declare it (several banks map slots 0-2); the `rom_only` set has one image and does not.
   - Genesis Z80 sets declare it (every image maps `$0000`; the activation signature selects, ADR 0073).
   - The Genesis M68K set does not: overlapping alias descriptors stay rejected, as ADR 0049 requires.

7. **Closed tags.** `ImageAuthority` has exactly the three ADR 0076 values (`immutable_input`, `static_proof`,
   `bounded_build_time_materialization`); `ImageVerification` has exactly three (`none`, `byte_identity`, `structural`). Out-of-range
   values are rejected. Authority and verification are independent axes; neither is inferred from an address range.

8. **Producer provenance.** `ImageProvenance {authority, producer, evidence}`. The producer name is a stable identifier over
   `[a-z0-9_.]+` (`invalid_producer_name` otherwise). The names in use are `genesis.cartridge`, `genesis.copy_alias`,
   `sms.cartridge_bank` and `genesis.z80_materializer`. Evidence is an ordered list of producer-owned `{name, value}` facts, including
   the derivation relationship (`derivation = mapping_claim | verbatim_source_copy | runnable_epoch_snapshot`). Evidence is
   build-artifact data and is never reported.

9. **Sanitized reporting.** `count_image_provenance` and `format_image_provenance_json` produce only image counts by authority and by
   producer name (`{"images":N,"authority":{...},"producers":{...}}`, producers in sorted order). No address, byte or hash appears.

10. **Validation.** `validate_executable_image_set` is fail-closed and returns the first `ImageValidationError` with its image index.
    An empty set is valid. Every consumer projection validates before it uses a set.

11. **Z80 projection.** `codegen::z80::project_executable_images(set, kinds)` is a pure function onto `codegen::z80::ImageSet`:
    - `structural` verification <-> `CodeImage::live_bytes`; `none` -> immutable; `byte_identity` is rejected (the Z80 has no
      whole-instruction byte guard);
    - each mapping becomes a `CodeWindow` (bases above `0xFFFF` are rejected);
    - `ImageKind` is supplied per image by the platform (SMS: fixed image `invariant`, banks `banked`; Genesis: all `banked`) and is
      never inferred from the generic artifact;
    - a non-Z80 set or a kind count mismatch fails closed.
    `CodeImage`/`ImageSet` remain the Z80 emitter input; they are not promoted to the generic type.

12. **Genesis Z80 producer.** `z80::executable_images(registry, producer)` describes each registered snapshot as one owned 16 KiB image
    (the 8 KiB snapshot twice) with a single mapping at `$0000` and `structural` verification. The two-copy byte layout is kept so that
    projection reproduces the historical `CodeImage` exactly; the mirror count is producer evidence (`ram_mirrors`), not a second
    mapping. Ordinal, content digest and signature digest are producer evidence. The registry, content hash and activation signature
    keep their ADR 0073 definitions.

13. **M68K producer and the ADR 0049 consumer.** `genesis_m68k_executable_images(program)` yields, in order, one `immutable_input`
    image (`genesis.cartridge`, verification `none`) per structurally valid `raw_cartridge_rom` claim, owning the claim bytes at the
    claim base, then one `static_proof` image (`genesis.copy_alias`, verification `byte_identity`) per ADR 0049 descriptor, whose bytes
    are a source reference into its owning cartridge image and whose single mapping is the work-RAM execution base. Its evidence is a
    verbatim source-image copy (source image, offset, length). It fails closed on exactly the conditions the alias path always
    enforced. `populate_immutable_rom_aot_entries` derives the alias identities from these `static_proof` images; decode, guard and
    execution-relative provenance are unchanged.

14. **Replacement proof by registry equality.** A future static producer that replaces the Genesis materializer (ADR 0073 decision 8)
    declares `static_proof` through `z80::ImageProducer {authority, name}`. It is proven by `z80::registries_equal` (same ordered
    ordinal, content hash, signature and snapshot) on the materialization corpus; an equal registry yields the same projected
    `ImageSet`, so AOT, runtime selection and devices are unchanged by construction. A test stand-in exercises this path; no real
    static producer is added by SEG-028.

15. **Forbidden identifiers.** `tests/executable_image_forbidden_identifiers_test.py` scans the generic header for CPU, console, device
    and platform vocabulary (Genesis epoch/signature, 8 KiB, 16-bit window, 4-byte guard, mapper/bank/slot, M68K/Z80 names and
    similar), mirroring `genesis_z80_forbidden_identifiers_test`.

16. **Not generalized.** Genesis epochs, hold window, activation signatures, the registry bound, 8 KiB and the two-mirror window, and
    `PassRunner`; Z80 16-bit windows, `ImageKind`, the 4-byte live guard, structural mask and direct-binding rules; SMS slot
    admissibility and mapper registers; ADR 0049 discovery (maximal verbatim run search, preparation rounds) and M68K cartridge
    mirroring (ADR 0049 decision 5); runtime entry tables beyond `emit_compiled_entry_table`; any decompression or transform
    semantics; any derivation taxonomy. A field is promoted only when two real consumers need it.

17. **No-regression invariant per child.**
    - **T003 (Z80 producers: SMS and Genesis):** byte-identical generated output for every SMS and Genesis Z80 fixture and local image.
    - **T004 (M68K producer and the ADR 0049 frontend loop):** byte-identical generated output for the same descriptors.
    - Any later child that is not emission-neutral uses the full SEG-027-T003 equivalence set (same admitted identities, same
      execution, same fail-closed behaviour, equivalent artifacts, deterministic output, no runtime decoder, no compatibility
      regression).

## Consequences

- Every existing executable-image producer is expressed through one validated, copy-safe artifact, and generated output is unchanged.
- Consumers (broad AOT today; SEG-029/030 discovery and SEG-031 admission later) can key on `ImageId`, authority and verification
  without importing platform or Z80 codegen types.
- Durable reports gain only sanitized counts; producer evidence never leaves the build.
- The SMS and Genesis Z80 machine libraries now also link `segarecomp::codegen_c11_z80_image`; the SMS dependency test pins that link.
- `segarecomp build` consumes ADR 0049 aliases proposed by its own bounded build-time preparation (the T005 decision below).

## Alternatives rejected

- **Promote `CodeImage` to the generic type.** It would export the 16-bit identity, `uint16` window bases, `ImageKind` and the 4-byte
  live guard to every CPU.
- **Borrowed `std::span` bytes (the draft).** A borrowed pointer ties a set's lifetime to the producer's buffers and makes copies
  unsafe; a source reference to an owning image of the same set carries the same no-copy relationship and is validated.
- **A per-image overlap selector flag.** Selection is a property of the platform's run-time mechanism over the whole set, not of one
  image; a set-level flag keeps the rule simple and keeps M68K overlap rejection intact.
- **Validating CPU address width generically.** It would leak CPU facts into the neutral header; the consumer already enforces its
  width (`CodeWindow` validation, the M68K work-RAM window).
- **A generic derivation field (copy, transform, decompression).** Only one real derivation per producer exists today; it stays
  producer evidence.
- **Two Z80 mappings for the Genesis RAM mirror.** It changes the projected `CodeImage` and therefore generated output; the 16 KiB
  two-copy layout keeps emission byte-identical.
- **Putting the Z80 projection inside `codegen_c11_z80`.** It would add `recompiler` to that target's pinned links.
- **Roots or runtime selection keys in the image.** They belong to discovery requests and platform tables respectively.

## Consumer-route ADR 0049 alias producer (T005 go/no-go)

- Decision: **GO** (SEG-028-T005). `segarecomp build` runs a bounded build-time ADR 0049 alias preparation.
- Evidence before the decision:
  - The alias-less consumer-equivalent route (`--immutable-rom-aot`, no aliases) on one authorized title stops fail-closed with
    `known_but_unemitted_target` at a work-RAM execution target (sanitized classification only).
  - The project-authored fixture `genesis_immutable_copy_alias_generated_test` exists and exercises an alias image end to end.
- Mechanism:
  - Descriptor origin: a build-time preparation run on the headless materialization pass runner (ADR 0073), under the ADR 0076
    bounded build-time execution rules (finite instruction budget, wall timeout, constant round bound, typed failures, nothing
    executed in the final program). When the confirmed fixed point ends with an M68K guest stop, the pass hook writes a private stop
    record (stop PC, stop class, 64 KiB work RAM; the `SEGARECOMP_STOP_WORK_RAM_DUMP` layout) into the build's work directory.
  - A stop of class `known_but_unemitted_target` or `internal_dispatch_inconsistency` at an even work-RAM PC whose bytes are a
    verbatim cartridge run of at least 16 bytes yields one proposal (`m68k_alias::derive_copy_alias`, merged by
    `merge_copy_aliases`; a C++ port of the tooling-route functions, result-identical on synthetic inputs). The build re-emits the
    M68K C in-process with one `--immutable-copy-alias` per merged descriptor, recompiles only the M68K units whose content key
    changed, relinks the pass program and reruns the Z80 fixed point. The Z80 registry is kept across rounds: its images are
    activation-keyed facts already proven by observed execution, and every round's fixed point re-confirms the program it builds.
  - Authority stays `static_proof`: the emitter re-validates every descriptor against the image and every alias body keeps its
    per-instruction byte-identity guard, which remains the runtime authority. The final program links the last round's M68K units;
    the last fixed point (with the final alias set) is its confirming run.
- Bounds and terminations: at most 64 rounds (`m68k_alias::kMaxRounds`). `route_advanced`, `non_alias_frontier`,
  `no_work_ram_frontier`, `frontier_not_verbatim_copy` and `repeated_alias_no_progress` build the final program with the aliases
  found (a remaining frontier stays fail-closed). `max_rounds` and `tool_failure` fail the build with the typed diagnostic
  `alias_preparation_incomplete` (status `failed`, stage `m68k-alias-prepare`, no executable). A round whose Z80 fixed point fails
  fails the build with that typed `z80-materialize` failure. With no M68K guest stop there is no extra round, and generated M68K/Z80
  C is byte-identical to the previous route.
- Reporting (sanitized aggregates only): status.json `m68k_alias_preparation` `{rounds, aliases, alias_bytes, termination}` and the
  same build.log line; the aliases appear as `static_proof` / `genesis.copy_alias` images in `executable_images.m68k`, counted
  from `genesis_m68k_executable_images` of the program with the aliases.
- Evidence after the change (sanitized):
  - Hermetic: `genesis_m68k_alias_build_test` (project-authored ROM): 1 round, 1 alias, 1 `static_proof` image, `route_advanced`;
    the alias-less fixed point ended with a guest stop, the final program runs past the copied routine; deterministic across
    builds; a computed (non-verbatim) RAM routine terminates `frontier_not_verbatim_copy` and stays fail-closed.
    `genesis_m68k_copy_alias_test` covers every termination with scripted rounds; `genesis_m68k_copy_alias_parity_test` covers the
    port.
  - Authorized title A (the one that motivated GO): 1 round, 1 alias, `route_advanced`; the confirming fixed point observes the
    whole 600-frame window (previously it ended with a guest stop well inside it); Z80 audio `supported`. The final program, run
    headless with `--instruction-budget 400000000`, ends `runner_resource_limit` (no guest stop) with the Z80 generated-native and
    non-silent audio.
  - Authorized title B: 0 rounds, `route_advanced`; generated M68K/Z80 C and every other status member identical to the previous
    route.
- Known limits: discovery is path dependent (only copies reached by the headless no-input run inside the observation window are
  found); a copy executed later, or reached only through input, stays fail-closed at run time. Overlapping descriptors with
  different deltas are rejected by the emitter (`tool_failure`). Copies that are transformed rather than copied verbatim are never
  aliased.
