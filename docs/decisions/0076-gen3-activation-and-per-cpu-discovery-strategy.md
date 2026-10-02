# ADR 0076: Gen-3 Activation and Per-CPU Discovery Strategy

- Status: Accepted (SEG-027-T006, independent gate PASS). Proposed by SEG-027-T002.
- Final decisions: SEG-028 ACTIVATE (`p1`); SEG-029 ACTIVATE as an incremental core (`p2`), no dependency edge between them;
  SEG-030 and SEG-031 DEFERRED; production unchanged (broad AOT on Z80 and M68K).
- Date: 2026-10-02
- Evidence: `docs/architecture/gen3-evidence-ledger.md` (SEG-027-T001). Citations of the form `[L 2.1]`, `[L C1]` and `[L S8]` point to
  ledger sections, contradiction rows and seam rows.
- Related, unchanged: ADR 0039, 0049, 0051-0055 (M68K Gen-2 and the discovery experiments), 0058/0071 (broad Z80 AOT), 0072/0073 (Genesis
  Z80 integration and build-time materialization).

## Context

"Gen-3" was postponed on 2026-09-29 until a second CPU and platform existed. That condition is now met:

- Z80 has a complete static path (SEG-008);
- the Master System platform exists (SEG-009);
- Z80 build scalability is solved (SEG-033);
- the Genesis Z80/audio integration, with build-time materialization, has been delivered (SEG-032).

The questions are now whether the Gen-3 milestones SEG-028..031 should start, and with what shape. The answers must come from current-main
evidence on both CPUs, not from M68K/Sonic evidence alone, and not from numbers that predate SEG-025, SEG-033 or SEG-032 [L 1].

Two problems are kept separate throughout:

- **Problem A (SEG-028):** where executable bytes come from, and under which identity, mapping and provenance they may be compiled
  statically.
- **Problem B (SEG-029 core, SEG-030 M68K instantiation, SEG-031 admission):** given an executable image, which instruction identities and
  indirect targets must be admitted.

## Per-CPU / per-image strategy

| CPU / image class | current producer | current admission | future candidate | why (evidence) |
| --- | --- | --- | --- | --- |
| **M68K immutable cartridge image** | Immutable input image: structurally valid `raw_cartridge_rom` claims [L S1]. | Broad immutable-ROM AOT over every aligned decodable start (`U` = 246,293 of 262,144 on the 512 KiB reference) plus Gen-2 Tier-1 discovery. | Gen-3: reachability plus abstract value/pointer analysis (SEG-029/030), reported as `precise set OR Unknown`. Production admission is changed only by SEG-031, using bounded fallback; broad AOT stays the fallback/reference. | Need demonstrated: sound reduction gives `L = U` on 6/6 titles [L 2.2]. The best local analysis recovers 42.7% of observed execution at 2.75% of `U`. 94.4% of the remainder sits behind width-only dispatch, and 84.2% has a register-relative field with a non-provable base on its chain; local store reasoning gained 0 PCs. Cost is linear in image size, not code size: 218 MB of C at 512 KiB, 1.72 GB at 4 MiB synthetic [L 2.1]. |
| **M68K statically proven RAM copy/alias (ADR 0049)** | Authority `static_proof`: the emitter validates the descriptor `(execution_base, source_base, length)` against an owned cartridge claim, so the image bytes are the immutable source span (producer evidence: verbatim source-image copy). Descriptors come from an optional bounded preparation phase in tooling [L S2]. | Broad AOT of the alias span at the execution base, with a per-instruction byte-identity guard. | The first M68K consumer of the SEG-028 image/producer seam, and the route by which `segarecomp build` gains alias images through a producer interface rather than bridge flags. A future static analysis (a SEG-030 copy-loop proof) can supply the descriptor instead of the preparation phase. The image authority stays `static_proof`. | `segarecomp build` consumes no alias today [L S2]. The mechanism exists only on tooling routes. Execution semantics (guard, execution-relative provenance) are proven and must be preserved byte for byte. |
| **future transformed/materialized M68K RAM image** (decompressed or generated code) | None. Fail-closed frontier (ADR 0049 non-goals). | None (fail closed). | Either a producer with `bounded_build_time_materialization` authority (as SEG-032), or one with `static_proof` authority (from SEG-030), behind the SEG-028 seam. Only when a workload requires it. | No measured workload requires it yet. Recorded as a SEG-028 revisit item, not a deliverable. |
| **Z80 immutable SMS cartridge image** | Immutable input image: an invariant image plus one banked image per 16 KiB bank [L S7]. | Broad Z80 AOT (ADR 0058/0071). | Broad AOT, retained. A future static analysis is not required to ship. | SEG-033: 512 KiB in 23-28 s, 14,999 host functions, 30.7 MiB executable, behaviour unchanged [L 2.3, C1]. No measured blocker. |
| **Z80 Genesis materialized RAM image** | Authority `bounded_build_time_materialization`: hardware-defined runnable epochs, content hash plus activation signature, registry with bound 8 [L S8]. | Broad Z80 AOT per 8 KiB image with a structural guard and live payload. Structural replacement isolates the sound CPU. | Broad AOT, retained. A future static producer may replace steps 1-4 of the materializer (ADR 0073 decision 8) when analysis can prove the image, without changing the registry, AOT, runtime selection or devices. | At most 2 images per workload. The materialize stage takes 8.5 s on the reference build, about 1 s of compile per image [L 2.4]. |

## Image authority

An image's **authority** is the mechanism by which the compiler establishes that these are the bytes to compile at these execution
addresses. There are exactly three values. Names are provisional; SEG-028 picks the repository vocabulary.

1. **`immutable_input`.** The bytes are the validated input image. Examples: Genesis cartridge claims, SMS banks.
2. **`static_proof`.** The bytes are established by static reasoning over immutable input, with no execution of guest code. Examples:
   - an ADR 0049 alias: the emitter validates the descriptor against an owned cartridge claim, so the bytes are the immutable source
     span;
   - any future analysis-based producer.
3. **`bounded_build_time_materialization`.** The bytes are observed while running the already-compiled generated-native program from
   deterministic reset with no input, under constant bounds. Only hardware-defined events select a snapshot. Example: Genesis Z80
   (ADR 0073).

A future static analyzer is **not** a fourth authority. It is a producer whose authority is `static_proof`.

**The derivation relationship stays producer-owned evidence, not a generic field.** Examples of derivation relationships:

- a verbatim copy of a source image;
- a Genesis runnable-epoch snapshot with its activation-signature facts;
- in future, whatever a static proof establishes.

No generic transform or decompression vocabulary is introduced. SEG-028 may promote a derivation field to the generic artifact only
when two real consumers need it.

**Verification is a separate axis.** It is required wherever live bytes may differ from the compiled image, and its implementation is
CPU/platform-owned:

- ADR 0049: byte identity;
- Genesis Z80: structural bytes plus live payload, and the signature lookup.

Worked examples:

| image | authority | producer evidence | verification |
| --- | --- | --- | --- |
| SMS bank, Genesis cartridge claim | `immutable_input` | mapping claim | none |
| ADR 0049 M68K alias | `static_proof` | verbatim source-image copy | byte identity |
| Genesis Z80 RAM image | `bounded_build_time_materialization` | runnable-epoch snapshot and signature facts | structural (CPU/platform rule) |
| future static producer replacing Genesis materialization | `static_proof` | whatever static proof establishes the same platform image | unchanged |

**The final executable is fully static. The build process need not be purely static.** The final executable contains only AOT code: no
discovery, materialization, learning, opcode decoding, JIT or interpreter. The build may execute its own generated-native code only
under the bounded build-time execution rules below.

The bounded build-time execution rules, which SEG-032 satisfies:

- the observed events are hardware-defined (`/RESET`, BUSREQ, Z80-RAM writes), not coverage;
- the run is deterministic and bounded, and a confirming run is required;
- the executed code is the same generated code the final program contains;
- every exhaustion is a typed build failure;
- the runtime still fails closed on any image or code it was not built for [L C2].

ADR 0049's preparation phase also executes generated code at build time, but it only proposes descriptors. The image authority stays
`static_proof`, because the emitter re-validates every descriptor against the immutable source and the runtime byte-identity guard
decides execution. The phase is workload- and path-dependent, stops at frontiers, and is tooling-only. Whether `segarecomp build`
gains it is a SEG-028 decision, not something adopted here. If it does, that build-time execution must satisfy the bounded
build-time execution rules above.

## Decisions

### SEG-028 (Problem A): ACTIVATE

Evidence [L 3]:

- three real, materially different producers already exist, one for each authority (`immutable_input`, `static_proof`,
  `bounded_build_time_materialization`);
- `codegen::z80::CodeImage` / `ImageSet` already serves two Z80 producers (SMS banks and Genesis materialization) [L S6];
- M68K has no image artifact; its identities are rows of `FrontendAnalysis` [L S1, S2];
- the consumer `segarecomp build` cannot use the proven ADR 0049 producer at all [L S2];
- ADR 0073 decision 8 promises a producer swap, which needs an explicit producer boundary to stay true.

The work is bounded and close to existing code. It needs no new analysis capability.

Shape: the smallest common artifact/producer seam, two adaptations proven equivalent (Z80; M68K ADR 0049), producer provenance, and
multi-CPU fail-closed validation. It is not a universal loader. Genesis epochs, activation signatures, 8 KiB/16-bit windows and
4-byte live guards stay platform/Z80-specific. The draft contract and refinement plan are SEG-027-T003's job (see Amendments).

### SEG-029 (Problem B core): ACTIVATE, as an incremental architecture/core milestone

Evidence:

- the M68K need is demonstrated and was diagnosed precisely by SEG-024/026 [L 2.2];
- two independent, ad-hoc, M68K-only finite-value analyses already exist (Gen-2 static discovery and the SEG-026 helper) [L S5], with
  no shared solver;
- the next precision step (pointer/alias/object/interprocedural) is beyond both, by measurement (ADR 0055);
- Z80 is a genuine second-CPU perspective. It has a different decoder, register file, addressing (HL/IX/IY), stack/return and banking.
  A bounded synthetic Z80 adapter can prove that the seam is not M68K-shaped without making production Z80 depend on it.

Shape: the smallest deterministic dataflow/fixed-point seam and the smallest domains justified by the first consumer, plus the Z80
adapter. Every further domain is staged and admitted only with a named consumer and precision problem. It is not a complete VSA
framework. The draft contract and refinement plan are SEG-027-T004's job.

### Scheduling and dependency

Both milestones are activated: SEG-028 at `p1`, then SEG-029 at `p2`, ordered by priority only. No architectural dependency exists
between them:

- SEG-029 analyses an executable-image *view*, which is an immutable cartridge image until SEG-028 lands;
- SEG-028 needs no analysis.

SEG-028 goes first because it is the smaller, more immediately useful seam: it brings proven aliases into the consumer route and keeps
ADR 0073's producer-swap promise explicit.

### SEG-030 and SEG-031: DEFERRED

- **SEG-030** (M68K value/pointer analysis) waits for delivered SEG-029 (and SEG-028 for RAM-executed images).
- **SEG-031** (production admission) waits until report-only SEG-030 analysis shows a substantial, falsified benefit with a safe fallback.

Their reconciled boundaries are recorded by SEG-027-T005 (see Amendments).

### Production strategy, unchanged by this ADR

- Z80: broad AOT on both platforms.
- M68K: broad AOT, until SEG-031 decides otherwise.
- Broad AOT is a successful Gen-2 production/reference strategy, not debt.

## Answers: the 16 ADR questions

1. **What is Gen-3 trying to improve?**
   - Problem A: an explicit, multi-producer executable-image boundary, so non-cartridge code (RAM copies, materialized images, later
     static producers) enters the build through one fail-closed artifact.
   - Problem B, for M68K: precision. The goal is to compile the program the architecture can select (`precise set OR Unknown` plus a
     bounded fallback) instead of every decodable word. Generated output, and its readability, would then scale with code size rather
     than image size [L 2.1: `U` is about 0.94 x aligned starts].
   - Neither problem is a performance emergency: Gen-2 cost is linear and affordable at 0.5-1 MiB.
2. **Which Gen-2 mechanisms remain production?** All of them:
   - M68K broad immutable-ROM AOT plus Tier-1 discovery;
   - ADR 0049 aliases (tooling routes);
   - broad Z80 AOT with bounded owners;
   - SMS image sets;
   - Genesis build-time materialization, registry and guards;
   - compiled-entry tables.
3. **Which CPUs need selective discovery?** M68K, by evidence. No other CPU, by current evidence.
4. **Why does Z80 not currently need it?** Broad Z80 AOT is measured as affordable on both platforms [L 2.3, 2.4]. Selective Z80
   admission would need new evidence:
   - a Z80 code space materially larger than 512 KiB of banked ROM;
   - a compile or RSS budget violation on a supported host;
   - materialized-image counts approaching the bound;
   - or a Z80 readability/size requirement that broad AOT cannot meet.
   Z80 still matters architecturally. It is a SEG-028 consumer and the SEG-029 second-CPU validator.
5. **What is an executable image?** Decision level: bytes plus the CPU that executes them, plus the address mappings at which it may
   execute them, plus an identity and producer provenance, with fail-closed verification wherever live bytes may differ. This is
   refined by T003 (contract questions 1-7).
6. **Who may produce one?** Any CPU/platform-owned producer behind the common artifact, provided its image has one of the three
   authorities above. All three are in use today. Future analysis-based producers use `static_proof`. Refined by T003.
7. **Can a build-time generated-native materializer be a production producer?** Yes, with `bounded_build_time_materialization` authority under the bounded build-time execution rules: hardware-defined events,
   determinism, constant bounds, a confirming run, typed failures, and a final executable that never learns.
8. **What does the final executable know at runtime?** Only:
   - the compiled identities, entry tables, registries and signatures fixed at build time;
   - the live machine state it is executing.
   It decodes nothing, learns nothing and compiles nothing. Unknown code means a typed stop. A Genesis structural Z80 mismatch means a
   sound-CPU fault.
9. **Who owns CPU abstract-transfer semantics?** The CPU libraries (`libs/cpu/m68k`, `libs/cpu/z80`), built on their existing
   decoders and effect owners. The generic core never interprets an instruction. Refined by T004.
10. **What is generic in the future analysis engine?** Decision level: a deterministic worklist/fixed-point solver, join, resource
    accounting and the `Unknown` contract, plus small baseline domains, all parameterized by a CPU adapter. Address regions, points-to,
    abstract memory, intervals, widening, contexts and summaries are staged, not baseline. Refined by T004.
11. **What happens on `Unknown`?** The site stays unresolved. Report-only stages count it. In production (SEG-031 only), it is covered
    by a bounded fallback island over proven image structure, or by broad AOT of the whole image. It is never dropped or guessed, and
    never covered by "operand-width region" expansion (ADR 0051).
12. **How does broad AOT coexist with selective analysis?** Broad AOT stays the reference, fallback and debugging mode, and the
    per-image default. Selective admission is a per-CPU/per-image choice that SEG-031 may adopt only where a multi-title comparison
    shows clear benefit with no compatibility regression.
13. **How are RAM/self-modifying cases guarded?**
    - M68K aliases: per-instruction byte identity.
    - Genesis Z80: structural bytes plus live payload, with structural replacement treated as unsupported (sound-CPU fault).
    - Any new producer must name its guard.
    Arbitrary self-modifying code is out of scope for the first Gen-3 version.
14. **How is validation/falsification performed?**
    - Independent oracles, synthetic/adversarial fixtures and mutation tests.
    - Exact generated-output comparison where behaviour must not change.
    - Complete M68K execution-PC coverage as a falsifier: `observed PC not in D` falsifies a claim, and an unobserved PC proves nothing.
    - A future Z80 observer only when a Z80 discovery or admission claim exists [L 5].
15. **What evidence must exist before production admission changes?** A SEG-031 multi-title, multi-platform comparison showing:
    - zero escapes;
    - no compatibility regression;
    - complete coverage of unresolved sites by bounded fallback;
    - deterministic output;
    - measured benefit on volume, compile time, size and readability, with runtime not worse.
    Nothing in SEG-028..030 changes admission.
16. **What are the STOP conditions for over-complex analysis?** Listed below; they carry into SEG-028..031.

## Answers: the original 12 Gen-3 quality questions

1. **Where do executable bytes come from?** From producers whose images carry one of the three authorities. The derivation is producer
   evidence (SEG-028).
2. **Is there a CPU-independent image representation?** Yes, at the artifact level only: CPU identity as a tag, not a type hierarchy.
   The minimal form is decided by T003. Z80-specific fields (16-bit windows, live guard) stay Z80-owned.
3. **Who owns CPU-specific abstract transfer?** The CPU libraries (answer 9 above).
4. **Where do VSA/points-to live?** In SEG-029, as *staged* domains admitted per consumer. The M68K instantiation lives in SEG-030.
5. **How are mutable object/state fields handled?** In SEG-030 (field transfer), using SEG-029 region/object domains when admitted. This
   is the measured SEG-026 blocker.
6. **How are unresolved targets represented conservatively?** As explicit `Unknown`. Fallback is SEG-031's job, and it never guesses.
7. **How does broad AOT coexist with selective analysis?** By per-CPU/per-image selection, with broad AOT first-class (answer 12 above).
8. **How is the SEG-024 snowball prevented?** There is no "unknown target -> operand-width region" rule. Fallback islands need proven
   executable-image or memory structure. Data-decode growth is reported (`D - O`, overlapping starts) and gated.
9. **Is runtime coverage a falsifier only?** Yes, always. It is never discovery input or admission authority.
10. **Why was Gen-3 postponed, and what changed?** It was postponed until a second CPU and platform existed. That condition is met
    (SEG-008/009/033/032). SEG-033 also removed the Z80 volume argument [L C1], and SEG-032 added a production
    `bounded_build_time_materialization` producer [L C2].
11. **Does Z80 keep broad AOT?** Yes, because of SEG-033/SEG-032 measurements, not image size alone [L C3].
12. **What evidence is needed before selective admission replaces broad AOT?** The SEG-031 comparison (answer 15 above).

## STOP conditions (carried into SEG-028/029/030/031)

Stop and re-refine if the design starts requiring any of the following:

- a universal CPU IR with no second real consumer;
- a generic hardware/memory emulator inside the analysis core;
- platform banking semantics inside generic value domains;
- Genesis epoch/signature concepts inside generic image types;
- arbitrary self-modifying code in the first Gen-3 version;
- whole-program path-sensitive symbolic execution;
- SMT as a mandatory baseline before simpler abstract interpretation is measured;
- per-game object schemas or target metadata;
- unbounded context sensitivity;
- all-or-nothing whole-program alias precision;
- deleting broad AOT before the replacement is independently validated.

Selective symbolic execution for isolated hard sites may be evaluated later. It is not an initial commitment.

## Standing constraints

These hold unless a later ADR explicitly argues otherwise:

- static final executables;
- generated-native execution;
- no interpreter, JIT, runtime opcode decoding or runtime target learning in the final executable;
- no title-specific target/address lists as production authority;
- deterministic bounded algorithms;
- fail closed;
- CPU semantics stay CPU-owned;
- no universal CPU IR for conceptual uniformity;
- external disassemblers are evidence, never execution authority;
- runtime coverage is a falsifier, never proof of unreachability.

## Consequences

- At SEG-027 closure, SEG-028 and SEG-029 move `deferred -> draft -> ready` (SEG-028 `p1`, SEG-029 `p2`). Each then receives its own full
  refinement from the Refinement plan in its record.
- SEG-030 and SEG-031 stay `deferred`.
- No product behaviour changes.

## Amendments

(Added by SEG-027-T003, T004 and T005, and finalized by T006.)

### T003 (SEG-028 plan)

- Draft contract: `docs/architecture/executable-image-contract.md`. It answers the 16 Problem-A questions.
- Accepted by ADR 0077 (SEG-028-T001), which fixes the final artifact, placement and producer boundary and marks each answer Confirmed
  or Corrected.
- The artifact is minimal and layered: `cpu` tag, build-local `ImageId`, byte span or source reference, execution mappings, producer
  identity, image authority, producer-owned evidence, and a verification tag. Roots are not part of the image.
- `CodeImage`/`ImageSet` stay Z80 codegen input behind a pure adapter. `PassRunner` stays Genesis-specific.
- ADR 0049 becomes a `static_proof` producer with byte-identical output.
- A `static_proof` replacement of the SEG-032 materializer is proven by registry equality.
- The SEG-028 milestone record carries the rewritten Outcome/Scope/Acceptance and a seven-child Refinement plan. Its
  fifth child is an evidence-gated go/no-go on giving `segarecomp build` an ADR 0049 alias producer (authority `static_proof`).

### T004 (SEG-029 plan)

- Draft contract: `docs/architecture/abstract-analysis-core-contract.md`. It covers the CPU-adapter / generic-solver boundary, the
  `Unknown` contract and its reason vocabulary, resource bounds, determinism, the forbidden-identifier rule and the image-view input.
- Accepted by ADR 0078 (SEG-029-T001), which fixes placement (generic `segarecomp::analysis`, CPU-owned adapter targets that no
  production target links), the closed `Unknown` vocabulary, the bounds and the exact baseline domain; no staged capability is admitted.
- The baseline is the smallest set of domains justified by the first consumer: exact finite register sets with immutable-byte reads,
  re-expressing an existing M68K finite-value analysis. Address region, points-to, abstract memory, intervals, widening, contexts and
  summaries are staged under a four-part admission rule. No complete VSA framework is pre-committed.
- The Z80 second-CPU proof is a bounded synthetic adapter. It needs a small CPU-owned Z80 effect/successor projection in `libs/cpu/z80`,
  because Z80 semantics currently reach code only through C11 lowering.
- SEG-029 does not depend on SEG-028.
- The SEG-029 milestone record carries the rewritten Outcome/Scope/Acceptance and a seven-child Refinement plan.

### T005 (future boundaries: SEG-030, SEG-031, cross-CPU validation)

- **SEG-030** stays `deferred`. Its real prerequisites:
  - SEG-029 is required.
  - SEG-028 is required only for RAM-executed M68K images. Over immutable cartridge images it could proceed with SEG-029 alone, and it
    may be re-pointed if SEG-028 is ever postponed.
- **SEG-030 targets the measured blocker** (width-only dispatch fed by object fields; `(An)` gates) through staged workstreams. It must
  first reproduce the SEG-026-T002 baseline exactly. It remains `precise set OR Unknown`, and report-only.
- **SEG-031** stays `deferred` until report-only SEG-030 shows a substantial benefit, falsified on more than one title, with a safe
  fallback design.
  - The production candidate is a precise graph plus bounded fallback; broad AOT is first-class.
  - The SEG-024 operand-width region rule is forbidden.
  - Fallback islands need proven image or memory structure.
  - Per-CPU asymmetry is intended: Z80 may stay broad AOT, and Z80 VSA is not required.
- **Cross-CPU execution-PC observation** (Z80/SMS and Genesis Z80) is planned in `docs/testing/execution-coverage.md`.
  - Owner: SEG-031.
  - It is a prerequisite for any Z80-facing discovery or admission claim, but not for SEG-029's synthetic Z80 adapter.
  - A neutral observation seam is extracted only when it has two consumers.

### T006 (independent gate)

An independent adversarial review (no authorship of T001-T005) checked the 14 T006 criteria and found no blocking or major defect. It
also falsified the code claims and judged both ACTIVATE decisions adequately evidenced. Seven minor wording and precision findings
were corrected in the same change:

- the SMS wall-time noise statement;
- the 94.4% / 84.2% distinction;
- the superseded Genesis Z80 C range;
- the entry-table identifier;
- the finite-value analysis description and ownership;
- the two-level SEG-026-T002 baseline;
- private-harness wording.

The fast gate passed. Status set to Accepted.

### Provenance correction (SEG-027, after the T006 gate)

The original four "producer classes" mixed two dimensions: how the bytes are established, and how they derive from another image. As a
result they were not mutually exclusive. For example, a static analyzer proving a ROM-to-RAM copy was both a "proven copy" and a
"static-analysis producer".

They are replaced by the three authorities in "Image authority". Derivation becomes producer-owned evidence.

No decision, execution semantics, guard or schedule changes.
