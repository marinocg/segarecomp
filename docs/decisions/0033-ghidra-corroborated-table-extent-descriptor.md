# ADR-0033: Ghidra-corroborated table-extent descriptor

- Status: Accepted
- Date: 2026-09-11
- Related: ADR-0023, ADR-0025 (§10), ADR-0032, SEG-007-T164, SEG-007-T203, SEG-007-T204

## Context

**2026-09-11 operator correction.** The original version of this ADR (and the
implementation it described) promoted a corroborated candidate directly into
`FrontendProgram::external_logical_table_descriptor_hints` -- ADR-0023's own trusted
finite-selector-domain vector. This was an architectural error, caught only after fresh
runtime ground truth on the authorized Sonic ROM showed generated-native execution was
completely unmoved by the change: ADR-0023's own production consumer,
`M68kGeneralStartupEnvironment::logical_table_descriptor_hint`, is called from exactly
one site (`src/cpu/m68k/static_discovery.cpp`, the `MOVE.W (d8,PC,Xn),Dn` finite-
selector-table consumer) and only ever looks up a WIDTH-2 record. This mechanism only
ever corroborates WIDTH-4 absolute-code-pointer tables (ADR-0032's shape), so every
entry it ever promoted into that vector was structurally unreachable by the one lookup
that vector exists to serve -- a test asserting only `external_logical_table_
descriptor_hints.size() == N` proved vector insertion, not any discovery consequence.
This revision corrects the design: **a corroborated candidate is re-homed under
ADR-0032-style code-pointer-table semantics** -- it is promoted only into ordinary
`external_code_entry_candidates` proposals, reusing the exact entry-extraction/
candidate-emission logic `apply_genesis_code_pointer_table_descriptors` already uses,
never into `external_logical_table_descriptor_hints`. Every other part of the design
below (the two-signal agreement rule, the export script, the JSON schema) is unchanged
by this correction; only the promotion TARGET changed.

ADR-0025 §10 explicitly anticipated and deferred a gap: Ghidra's own "Create Address
Tables" analyzer already runs on every `tools/ghidra.py analyze` invocation (enabled by
the pinned `PinAnalysisRecipe.java` recipe), but nothing ever exports its table/array
inference, and ADR-0025 §10 forbids letting that raw inference become an ADR-0023
`logical_table_descriptor` "without a separate generic validation contract."

T164's own record independently shows why a naive "trust the raw inference" shortcut is
unsound: a purely structural/mechanical validation attempt on the real Sonic table
extent previously produced a WRONG, too-large count, because a single per-entry
"looks like a plausible value" check has no independent signal for where the true
table actually ends — words immediately past the real boundary can still look
individually plausible.

This ADR designs the missing generic contract: a bounded, mechanical, two-independently-
sourced-signal corroboration check that promotes a raw Ghidra table proposal to a
trusted extent fact only when both signals agree on the exact same terminating
boundary, and fails closed otherwise.

## Decision

### 1. New additive, explicitly non-authoritative candidate kind

Add `tools/ghidra/scripts/ExportAddressTableCandidates.java` as a sibling headless
post-script (never a replacement for `ExportCodeEntryCandidates.java`), run in the same
pinned headless invocation, and emit one `address_table_candidate` record per table:
`base_address`, `entry_width_bytes = 4`, `stride_bytes = 4`, `entry_count`, and machine
provenance with `human_reviewed = false`. This is raw material only. `tools/ghidra.py
analyze` writes it to a sibling output path; `tools/ghidra.py compose-hints` recognizes
and unions it exactly like every other structured record kind (ADR-0025 §9: preserved,
never trusted). It is never directly consumed as ADR-0023 authority anywhere in the
codebase.

**Empirical correction during the Phase 1 probe.** The initial design assumed the
"Create Address Tables" analyzer materialises each table as a single `Array`-typed
`Data` item. Running the pinned recipe against the real, authorized, hash-pinned Sonic
image (via the project's own `tools/ghidra.py analyze`, plus a disposable diagnostic
post-script deleted immediately after use) showed this assumption was wrong: the
analyzer instead places one `Bookmark` (type `Analysis`, category `Address Table`,
comment `Address table[N] created`) at each detected table's base address, and
separately writes N consecutive INDIVIDUAL 4-byte pointer-typed `Data` items (never
wrapped in an `Array`) starting there. The export script was corrected accordingly: it
locates table bases from the `Analysis`/`Address Table` bookmark (the analyzer's own
genuine per-table detection signal), then independently re-measures the extent by
counting the contiguous run of pointer-typed `Data` items starting at that address
(never by parsing the bookmark's own free-form comment text). Against the authorized
Sonic image this produced 217 raw candidates, all with a 4-byte pointer width, entry
counts ranging from small to double digits — confirming the mechanism is real,
functioning, and not an assumption. This correction affected only the export script's
own internal detection logic; the exported `address_table_candidate` JSON schema, the
C++ corroboration algorithm, and every other part of this design were unaffected and
required no change.

### 2. Independent structural corroboration: two differently-sourced signals must agree

The C++ Genesis frontend (`apply_genesis_address_table_corroboration`,
`src/machine/genesis/frontend.cpp`) reads one `address_table_candidate`'s immutable
4-byte pointer entries and appends them as ordinary `code_entry_candidate` proposals
(via the SAME entry-extraction/candidate-emission helper `apply_genesis_code_pointer_
table_descriptors` already uses for ADR-0032's manually authored descriptors) only when
ALL of the following hold:

1. **Self-terminating prefix walk (signal B).** Starting at `base_address`, each
   successive 4-byte big-endian value is read and independently validated against the
   EXISTING, unweakened per-entry ADR-0025 call-target validation
   (`validate_m68k_static_call_target`: 24-bit-clean, even, unambiguously mapped) —
   never a new or weakened check. The walk stops at the first index that either fails
   that check or runs past the end of the single covering `raw_cartridge_rom` mapping
   claim. The stopping index is `self_terminating_count` — a boundary computed entirely
   independently of whatever Ghidra's analyzer claimed.
2. **Exact agreement (signal A vs. signal B).** Promotion requires
   `self_terminating_count == candidate.entry_count` exactly. This single equality
   check enforces both directions at once:
   - If Ghidra's raw analyzer over-approximated (the T164 failure mode — a proposed
     count too large), the self-terminating walk necessarily stops earlier, and the
     mismatch fails closed: no proposals are appended for a wrong, too-large count.
   - If Ghidra's raw analyzer under-approximated, the self-terminating walk keeps
     validating past Ghidra's claimed boundary, again producing a mismatch and failing
     closed — the two signals must land on the identical index, not merely both find
     *a* valid-looking prefix.

Signal A (Ghidra's own array/pointer-density heuristic, computed by a completely
different analyzer subsystem than any candidate validation) and signal B (this
project's own existing ADR-0025 per-entry call-target admission rule, applied
mechanically as a prefix scan) are genuinely independently sourced: neither is derived
from or parameterized by the other. Requiring their exact agreement is what T164's
single-source structural check lacked, and is the corroboration this ADR supplies.

**What exact agreement does NOT prove.** Two-signal agreement is corroboration, not a
universal proof of table identity, and must not be oversold as one. It establishes only
that Ghidra's own array/pointer-density heuristic and this project's own per-entry
absolute-pointer structural check (24-bit-clean, even, uniquely mapped -- NOT
decodability, NOT any semantic "is this really a jump table" proof) happen to land on
the identical boundary. Concretely:
- A neighboring, unrelated data value can coincidentally satisfy
  `validate_m68k_static_call_target`'s three structural conditions without being a real
  code pointer at all; offsets/data that merely look like plausible 24-bit-clean even
  addresses are not rare in a ROM image.
- A genuine table can sit immediately adjacent to a second, unrelated, independently
  valid-looking pointer region, in which case the self-terminating walk may not stop
  exactly where the true semantic table ends (it stops where PLAUSIBILITY ends, not
  where TYPE/OWNERSHIP changes) -- signal A and signal B could in principle both walk
  into that adjacent region and still agree with each other while being jointly wrong
  about the true semantic boundary.
- This is precisely why every promoted entry (§3 below) is downgraded to an ordinary,
  independently-re-validated ADR-0025 proposal rather than a trusted extent fact:
  the corroboration check bounds the RISK of promoting a structurally-plausible-but-
  wrong value (it can never grant it emitted-code authority on its own), it does not
  eliminate the possibility that this mechanism proposes a value that is not, in fact,
  part of any real code-pointer table.

### 3. Promotion target: ordinary ADR-0025 candidate proposals (re-homed 2026-09-11)

**This section was corrected by the 2026-09-11 operator correction; see "Context"
above.** A successfully corroborated candidate's `entry_count` immutable 4-byte
big-endian values are read from the single covering `raw_cartridge_rom` mapping claim
and appended to `FrontendProgram::external_code_entry_candidates` as ordinary
`GenesisCodeEntryCandidateHint` proposals, with `provenance_human_reviewed = false`
(explicitly machine-corroborated, matching the candidate's own honest provenance).
This reuses the EXACT SAME entry-extraction/candidate-emission helper
(`append_immutable_code_pointer_table_entries_as_candidates`,
`src/machine/genesis/frontend.cpp`) that `apply_genesis_code_pointer_table_descriptors`
(ADR-0032) already uses for its own manually authored, human-reviewed descriptors --
the two promotion paths differ ONLY in how they populate the boolean
`provenance_human_reviewed` argument and in which source vector supplies
`base_address`/`entry_count`. The corroborated candidate never becomes, resembles, or
is converted into an ADR-0023 `logical_table_descriptor`: `apply_genesis_address_table_
corroboration` never reads or writes `FrontendProgram::external_logical_table_
descriptor_hints`. This is a deliberate choice, not an oversight:

- The corroboration check (§2) proves a structural fact about a 4-byte
  absolute-code-pointer table shape (ADR-0032's territory) -- it says nothing about,
  and cannot be repurposed to prove, a 2-byte PC-relative offset-word finite-selector
  table (ADR-0023's territory, and the shape T204 was originally created to close).
  These are genuinely different table shapes with different consumers; conflating them
  was exactly the 2026-09-11 correction's finding.
- `provenance_human_reviewed = true` is meant to assert that a human actually reviewed
  the extent. A machine-corroborated result must never claim that provenance, even
  implicitly by landing in the same vector as records that do assert it.
- Every promoted proposal must still independently survive the complete, unweakened
  ADR-0025 admission walk (mapping, alignment, decode, discovery, retention, emission)
  before it can affect generated output -- exactly ADR-0032's own trust boundary, never
  a new or weaker one. §2's "what exact agreement does NOT prove" caveat is precisely
  why this re-validation is load-bearing, not redundant.

The raw JSON-interchange kind (`address_table_candidate`) IS a new additive kind (per
the SEG-007-T203/ADR-0032 precedent of adding a new kind rather than reinterpreting an
existing one).

### 4. Location of the corroboration algorithm

The corroboration algorithm lives in `src/machine/genesis/frontend.cpp`, immediately
beside `apply_genesis_code_pointer_table_descriptors` and the
`validate_m68k_static_call_target` primitive it reuses. An initial design considered a
separate adjacent module
(`src/machine/genesis/address_table_corroboration.{hpp,cpp}`); this was reconsidered
after inspecting the actual code: `validate_m68k_static_call_target` has internal
(anonymous-namespace, translation-unit-local) linkage inside `frontend.cpp`, and
duplicating it in a new translation unit would either require exposing it (risking an
inadvertently widened trust surface) or reimplementing the exact same per-entry
validation rule a second time (risking silent drift between the two copies — the same
failure class this ADR exists to prevent). Keeping the corroboration function in
`frontend.cpp`, clearly labeled and adjacent to its manual-descriptor sibling, reuses
the primitive exactly once and matches the file's own existing precedent (every other
descriptor-apply function already lives here). It is never placed in the CPU-generic
`src/cpu/m68k/static_discovery.cpp`.

## Trust boundary

Exactly ADR-0032's existing trust boundary (not ADR-0023's -- see the 2026-09-11
correction in "Context"): a corroborated candidate's entries are ordinary
`code_entry_candidate` proposals. Two-signal agreement (§2) raises confidence enough to
justify SUBMITTING the proposal at all, but it is NOT a trusted finite-selector-domain
claim and NOT a proof of semantic table identity. Every structural consequence
(mapping, alignment, decode legality, target admission, the 256-member cap) is still
independently re-derived, unchanged, by the existing ADR-0022/ADR-0009/ADR-0025
machinery, exactly as it is for any other candidate proposal regardless of source. This
mechanism adds no new trust primitive at all -- it automates producing MORE proposals
for the existing ADR-0025 candidate walk to independently accept or reject, nothing
more. A disagreement between the two signals is not a bug: it is the designed
fail-closed outcome, and yields no proposals at all for that candidate (the existing
frontier for that table is completely unaffected — no worse than not opting in). See §2
above ("What exact agreement does NOT prove") for the concrete limits of what this
corroboration check actually establishes.

## Consequences

- `ExportAddressTableCandidates.java` and its sibling wiring in `tools/ghidra.py` are
  purely additive at the trust-relevant seam: `compose-hints --address-table-candidates`
  is fully opt-in and unions nothing when the flag is omitted, so the composed hints
  artifact's trust content is byte-identical to prior behavior when unused. `analyze`
  itself now unconditionally runs the sibling post-script and always writes an
  `address_table_candidate` sibling output file alongside the primary `--output`
  artifact (default path derived from its stem when `--address-table-output` is not
  given); the primary `--output` artifact's own content is unaffected either way, so
  this extra file is a new observable side effect of `analyze`, not a change to any
  existing trusted artifact.
- `apply_genesis_address_table_corroboration` is additive and opt-in: a program with an
  empty `external_address_table_candidates` (every existing scenario, and every ordinary
  raw-ROM recompilation) is a complete no-op.
- ADR-0023's existing manually authored `logical_table_descriptor` path
  (`external_logical_table_descriptor_hints`, consumed by `M68kGeneralStartupEnvironment
  ::logical_table_descriptor_hint`) is completely unchanged and untouched by this
  mechanism for any currently-annotated table: `apply_genesis_address_table_
  corroboration` never reads or writes that vector at all (corrected 2026-09-11; see
  "Context" and §3 above).
- ADR-0032's existing manually authored `code_pointer_table_descriptor` path
  (`apply_genesis_code_pointer_table_descriptors`) is likewise completely unchanged in
  observable behavior; this mechanism only reuses its entry-extraction/candidate-
  emission helper, never its input vector or its `human_reviewed` gating.
- No Sonic-specific recognizer, hardcoded address, or hardcoded entry count is
  introduced; the mechanism is generic across ROMs and exercised by project-authored
  synthetic fixtures only.
- A `fail_closed_no_descriptor` outcome (the two signals disagree, or no covering
  mapping claim exists, or the base is odd/misaligned/out-of-ROM) means no
  `code_entry_candidate` proposals are appended for that candidate. It is an accepted,
  honest design outcome, not a defect -- and even a successful promotion is not itself
  a guarantee of admission: every appended proposal must still independently survive
  the ordinary ADR-0025 walk (see "Trust boundary" above).
