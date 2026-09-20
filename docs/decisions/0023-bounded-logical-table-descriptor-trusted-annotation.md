# ADR 0023: Bounded Logical-Table-Descriptor Trusted Annotation

- Status: Accepted
- Date: 2026-09-04
- Amends: ADR 0022 (adds one additive, explicitly opted-in alternate producer
  of the finite selector-value input ADR-0022's offset-table producer already
  consumes; changes nothing else in ADR-0022 or ADR-0009)
- Related: ADR 0009 (the `M68kFiniteIndexValueSet` producer contract this
  annotation feeds), SEG-007-T163 (research recommendation this ADR
  implements, with one corrected seam description)

## Context

SEG-007-T160/T161 (ADR-0022) proved that the runtime-selected Sonic Phase-B
frontier's blocking indirect JSR resolves its `MOVE.W (d8,PC,Dn),Dd` table
load through a real, mapped, immutable in-cartridge offset table -- but the
index register feeding that load is only bounded by a defensive `ANDI #mask`
over a runtime work-RAM byte, not by a proof of the real table's logical
entry count. ADR-0022's own "Observed boundary" section names this precisely:
the selector bound is a hardware/RAM-status mask, not evidence that every
masked value indexes a real table entry, so the whole-set target-admission
check correctly rejects the over-approximated candidate set and the site
stays an honest `reached_unresolved_direct_edge` / `unresolved_indirect_target`
terminal.

SEG-007-T163 researched whether any external, explicitly opted-in annotation
could legitimately supply the missing fact without reopening PR #275's
rejected writer/value-set closure analysis (Option 3). It recommended
adopting a narrowly bounded tier-2 mechanism restricted specifically to a
**logical-table-descriptor entry-count/extent** claim -- the real extent of
an *already-identified* immutable table, never a claim about which values a
register may ever hold -- and explicitly rejected extending this to any
fixed-address writer/value-set closure annotation, which has no bounded
downstream structural check to run it through.

SEG-007-T163's Evidence sketched an interchange format and fail-closed
behavior assuming an independent `entry_count` scalar existed inside
`m68k_fold_immutable_offset_table` to attach an alternate input to. Reading
the actual implementation (`src/cpu/m68k/static_discovery.cpp` lines ~172-210)
during SEG-007-T164's pre-merge review showed this is incorrect: the function
consumes the exact finite `selector.values` set already proven for the index
register directly -- there is no separate `entry_count`/condition-2/4 field.
This ADR corrects that seam description and implements the mechanism as
SEG-007-T164's task record requires: an alternate **producer of the same
finite selector-value input**, not a new parameter threaded into the existing
folding function.

## Decision

Add one **optional, explicitly opted-in, ROM-identity-bound logical-table-
descriptor trusted annotation** as an alternate producer of the finite
selector-value input `m68k_apply_finite_value_transfer` already builds for
`m68k_fold_immutable_offset_table`. Everything downstream of that finite
selector -- `m68k_fold_immutable_offset_table` itself (unchanged signature
and logic), per-candidate `M68kIndirectTargetEaSet` EA evaluation, target
admission/walk, `indirect_branch`/`indirect_call` edges, candidate-specific
JSR frames, C4 `indirect_targets_<source>[]` emission, and the
`m68k_indirect_target_member` runtime guard -- is reused **entirely
unchanged**. No ceiling is widened. No new dispatcher, no second target-set
representation, no new frontier class, and no C11 emitter change.

### Interchange format

A small, tool-agnostic JSON record (a single object, or a JSON array of
objects, for multiple sites):

```json
{
  "rom_sha256": "<64 lowercase hex characters>",
  "kind": "logical_table_descriptor",
  "base_address": "0x00000B10",
  "entry_width_bytes": 2,
  "stride_bytes": 2,
  "entry_count": 15,
  "provenance": {
    "tool": "ghidra",
    "tool_version": "11.x",
    "timestamp": "2026-09-04T00:00:00Z",
    "human_reviewed": true
  }
}
```

`base_address`/`entry_width_bytes`/`stride_bytes`/`entry_count` accept either
a JSON number or a `"0x..."`/decimal JSON string; a JSON number for any of
these fields must be mathematically integral (`15` is accepted, `15.5` is
not) -- a non-integral literal is dropped rather than silently truncated,
since these fields are byte counts/addresses/widths, never fractional
quantities. This is not a Ghidra project/database file: a different
analyzer, or a human-authored file with the same fields, is equally valid
input. No signature/trust-chain mechanism is required -- the record's truth
is never trusted beyond the one scalar domain it supplies (the logical entry
count/stride), and every structural consequence is independently re-checked
exactly as before this ADR.

The `provenance` object is **required**, not decorative: a record missing
`provenance` entirely, or missing any of its four fields with the wrong
type (`tool`/`tool_version`/`timestamp` as JSON strings, `human_reviewed` as
a JSON boolean), is dropped by the parser exactly like a missing/malformed
top-level scalar field -- never accepted with empty/default provenance
values. This closes a gap in the original SEG-007-T164 implementation, which
treated provenance as optional; "Provenance in output" below always intended
it to be a required part of the trusted record.

If more than one accepted (parsed, ROM-hash-verified) record shares the same
logical table identity `(base_address, entry_width_bytes)`, all matching
records must agree exactly on every semantic field (`stride_bytes`,
`entry_count`); provenance fields may differ freely, since provenance is
metadata, not semantic content. Records that disagree are a genuine
identity conflict: every record for that `(base_address, entry_width_bytes)`
is dropped (the table falls back to `std::nullopt`, exactly like the
not-opted-in/no-match case) rather than an arbitrary "first one wins"
resolution.

### Explicit opt-in

A required, per-invocation CLI flag, `--external-hints <path>`, on
`segarecomp emit-general-startup-bridge-c` (and forwarded verbatim by
`tools/genesis_startup_bridge.py --external-hints <path>`). Ordinary raw-ROM
recompilation that never passes this flag never populates
`FrontendProgram::external_logical_table_descriptor_hints`, so
`M68kGeneralStartupEnvironment::logical_table_descriptor_hint` -- and every
environment that does not override the new
`M68kStaticDiscoveryEnvironment::logical_table_descriptor_hint` default at
all -- always returns `std::nullopt`, and the existing producer/selector
proof path is completely unaffected. Passing a hints file without this flag
is not a supported input path (there is no other way to reach the parser).

### ROM identity precondition

`segarecomp`'s existing `--rom-sha256 <digest>` CLI input (already required
by `emit-general-startup-bridge-c` and already supplied by
`tools/genesis_startup_bridge.py` from its own independently computed
`hashlib.sha256`) is reused unchanged as the expected identity. Every hints
record's own `rom_sha256` field is compared for exact string equality against
that digest before the record is accepted at all; a mismatch drops the record
(never a hard build failure -- see "Fail-closed behavior" below).

### The alternate selector-position producer (the corrected seam)

Implemented in `analyze_finite_index_values`'s transfer function
(`m68k_apply_finite_value_transfer`, `src/cpu/m68k/static_discovery.cpp`) and
the `M68kStaticDiscoveryEnvironment` seam
(`include/segarecomp/cpu/m68k/static_discovery.hpp`,
`src/machine/genesis/frontend.cpp`):

1. A new narrow fact method,
   `logical_table_descriptor_hint(base_address, entry_width_bytes) ->
   optional<{stride_bytes, entry_count}>`, added to
   `M68kStaticDiscoveryEnvironment` with a default implementation returning
   `std::nullopt` (so every existing environment implementation is
   source-compatible and behaviorally unaffected without changes).
   `M68kGeneralStartupEnvironment` overrides it with a linear scan of the
   opted-in, ROM-hash-verified hint list for an exact
   `(base_address, entry_width_bytes)` match.
2. At the existing `MOVE.W (d8,PC,Xn),Dn` transfer-function site, before
   calling the unchanged `m68k_fold_immutable_offset_table`, the CPU-side
   code computes this instruction's own recognized table base
   (`source_ea.pc_base_address + source_ea.displacement`, ADR-0022's existing
   base derivation) and asks the environment for a matching hint with
   `entry_width_bytes = 2` (the only width ADR-0022 condition 1 ever
   recognizes for this producer). When the environment returns a match with
   `1 <= entry_count <= 256` and `stride_bytes > 0` (an `entry_count` of `0`
   or exceeding `256`, or a `stride_bytes` of `0`, is rejected upfront --
   never built into a candidate set at all; see "Fail-closed behavior"
   below for why `entry_count` is bounded explicitly here rather than left
   to emerge from `m68k_fold_immutable_offset_table`'s own cap check) AND
   whose largest implied position, `(entry_count-1)*stride_bytes`, computed
   with checked `std::uint64_t` arithmetic, does not exceed `0xFFFE` (see
   "Fail-closed behavior" for why this bound, not a bare
   "fits in 16 bits" `<= 0xFFFF` check, is required), the ordered synthetic
   position set `{0, stride_bytes, ..., (entry_count-1)*stride_bytes}`
   becomes the `selector` value fed into `m68k_fold_immutable_offset_table`
   **in place of** the register-proven `M68kDnValueState`. When the
   environment returns `std::nullopt` (no annotation opted in, or no
   annotation matches this exact base/width), or when either bound above is
   not satisfied, the pre-existing register-proven selector is used
   completely unchanged.
3. `m68k_fold_immutable_offset_table` itself is not modified in any way: its
   signature, its selector-consumption logic, and every one of its proof
   obligations (single covering `MappingClaim`, structurally valid affine
   mapping, bounded descriptor width `<= 0x10000`, per-entry address
   canonicalization/evenness/24-bit range, the 256-member cap,
   `read_immutable_cartridge_bytes` success) run identically regardless of
   whether the selector it receives came from the register-value analysis or
   from this annotation.

### Trust boundary (accurate, not overstated)

The annotated logical selector-position domain/count is the **one** semantic
assertion this mechanism accepts without independent re-derivation:
segarecomp cannot prove that `entry_count` is the true logical entry count of
the real table. The existing mapping/bounds/alignment/decode/target-admission
checks (ADR-0022/ADR-0009, unchanged) validate the *structural consequences*
of whatever domain the annotation implies -- every resulting candidate
address is still independently re-verified in-bounds, even, decodable, and
admitted -- but those checks do **not** prove the supplied count is correct:
a structurally valid but wrong count (too small, missing real entries; or too
large, admitting extra structurally-valid-but-not-actually-real candidates)
is **not detected by structural re-derivation alone**. This ADR does not
claim "every incorrect count is necessarily rejected by the next structural
check."

Safety is instead preserved by two independent mechanisms, neither of which
is a completeness proof of the annotation itself:

1. The explicit opt-in and provenance record (logged to stderr at generation
   time; see "Provenance in output" below) make the trust dependency visible
   and attributable -- a compilation that depended on a trusted annotation
   can never silently look identical to one that did not.
2. The existing, entirely unchanged runtime `m68k_indirect_target_member`
   membership guard: an actual runtime-computed EA that is not a member of
   the generated (possibly annotation-derived) candidate set fails closed
   exactly as it does today, never dispatching partially or to a near-match.
   This bounds an **under-count** to an honest runtime stop rather than
   silent corruption. It does **not**, by itself, prove an **over-count**
   cannot admit a structurally-valid spurious candidate -- which is exactly
   why the annotation remains an explicit, opted-in, provenance-recorded
   trust decision rather than an automatically-verified fact.

### Fail-closed behavior

Every one of the following falls back to the unmodified existing frontier
(`reached_unresolved_direct_edge` / `unresolved_indirect_target`), never a
partial or silently-adjusted admission, and never a hard CLI/build failure:

- ROM-hash mismatch on a hints record (`parse_genesis_external_hints` drops
  the record).
- A malformed hints file (JSON syntax error) or a record missing a required
  field, or with a zero `stride_bytes`/`entry_count` (dropped by the same
  parser; a top-level JSON syntax error drops the whole file).
- A hints file present without the explicit `--external-hints` opt-in (there
  is no code path that reads a hints file without this flag at all).
- An annotation whose `base_address`/`entry_width_bytes` does not match the
  reached site's own recognized table base/shape (the environment method
  returns `std::nullopt`; the CPU-side producer falls through to the
  register-proven selector unchanged).
- An annotated domain that produces an out-of-bounds, misaligned
  (odd-address), undecodable, or otherwise structurally invalid entry under
  the existing ADR-0022 checks -- identical treatment to an unannotated
  overrun.
- An `entry_count` of `0` or exceeding `256` (the existing unwidened
  256-member finite-value cap), or a `stride_bytes` of `0` -- rejected
  explicitly, upfront, by the selector-position producer itself before any
  candidate set is built, falling through to the register-proven selector
  exactly as a non-matching base/width would. This is a deliberate,
  explicit bound at construction time, not an emergent consequence of
  `m68k_fold_immutable_offset_table`'s own post-hoc cap check: for certain
  `stride_bytes` values (e.g. any multiple of 256), the raw arithmetic
  progression `{0, stride_bytes, ..., (entry_count-1)*stride_bytes} mod
  65536` collapses to at most 256 distinct positions regardless of how
  large `entry_count` actually is, so relying on post-construction
  deduplicated cardinality alone would not reliably catch every
  cap-exceeding `entry_count`. Bounding `entry_count` itself before
  construction is correct for every `stride_bytes`.
- A within-cap `entry_count` (`<= 256`) whose true, unmasked implied maximum
  position `(entry_count-1)*stride_bytes` (computed with checked
  `std::uint64_t` arithmetic, never relying on implicit 16-bit truncation)
  exceeds `0xFFFE` -- rejected explicitly, upfront, exactly like the
  cap-exceeding-`entry_count` case above, falling through to the
  register-proven selector. Without this check, a large `stride_bytes`
  combined with an `entry_count` up to `256` can make the true maximum
  offset exceed `65535`; masking it down with `& 0xFFFFU` (needed only to
  narrow the checked-arithmetic result into the `std::uint16_t` position
  type once it is already proven in-range) would otherwise silently wrap
  that value into a small, structurally-plausible-looking in-range position
  that does not correspond to the actual, unwrapped logical position the
  annotation describes -- for example `stride_bytes = 0x10002`,
  `entry_count = 2` implies a true maximum offset of `65538`, but
  `65538 & 0xFFFF == 2`, indistinguishable from a genuinely intended
  `stride_bytes = 2`. `0xFFFE`, not the simpler `0xFFFF` ("fits in 16
  bits"), is used because it is at least as strict as
  `m68k_fold_immutable_offset_table`'s own tighter `max_entry_addr -
  min_entry_addr + 2 <= 0x10000` descriptor-span bound (ADR-0022 Decision
  §4) applied to a synthetic position set that always starts at `0`.
- A hints-file JSON number for `base_address`/`entry_width_bytes`/
  `stride_bytes`/`entry_count` that is not mathematically integral (e.g.
  `15.5`) -- the record is dropped by `parse_genesis_external_hints` rather
  than silently truncated.
- A hints-file record missing the required `provenance` object entirely, or
  with any of its four required fields missing or of the wrong JSON type --
  the record is dropped by `parse_genesis_external_hints`.
- Two or more accepted hints-file records that share the same logical table
  identity `(base_address, entry_width_bytes)` but disagree on a semantic
  field (`stride_bytes` or `entry_count`) -- every record for that identity
  is dropped by `parse_genesis_external_hints`, so
  `logical_table_descriptor_hint` returns `std::nullopt` for that identity,
  exactly like the not-opted-in/no-match case, rather than picking an
  arbitrary one. Records that agree on every semantic field but differ only
  in provenance are not a conflict.
- A later write invalidating the proven index value, or a runtime-computed EA
  not in the statically proven candidate set (ADR-0009's existing
  merge/fixed-point and `m68k_indirect_target_member` non-member stop,
  unchanged).

A stale annotation (correct ROM hash but produced against a prior
compiler/discovery revision) is not separately detectable beyond the
ROM-hash + structural re-check above; this ADR does not claim to detect it
further, matching SEG-007-T163's Evidence.

### Provenance in output

Every hints record that passes parsing and ROM-hash verification is logged
to stderr (never stdout, which carries only the generated C) by
`segarecomp emit-general-startup-bridge-c`, one line per accepted record,
naming its `base_address`/`entry_width_bytes`/`stride_bytes`/`entry_count`/
`provenance_tool`. This is a deliberately lightweight mechanism (a
generation-session log line, not a structured field embedded in the existing
sanitized/full JSON report schemas `tools/genesis_startup_bridge.py`
produces) -- it makes a trusted annotation's use visible and attributable in
the generation transcript without changing any existing report's schema or
adding provenance to the generated C, which never gains any runtime
dependency on this mechanism.

### Ownership and seam

- `include/segarecomp/cpu/m68k/static_discovery.hpp`: one new struct
  (`M68kLogicalTableDescriptorHint { stride_bytes, entry_count }`) and one
  new virtual method with a `std::nullopt`-returning default implementation
  on `M68kStaticDiscoveryEnvironment`.
- `src/cpu/m68k/static_discovery.cpp`: the alternate selector-value producer
  inside `m68k_apply_finite_value_transfer`'s existing `MOVE.W`
  transfer-function branch. `m68k_fold_immutable_offset_table` itself is not
  touched.
- `include/segarecomp/machine/genesis/frontend.hpp` /
  `src/machine/genesis/frontend.cpp`: the `GenesisLogicalTableDescriptorHint`
  record type, `FrontendProgram::external_logical_table_descriptor_hints`
  (empty by default), the narrow schema-scoped JSON reader and
  `parse_genesis_external_hints`, and
  `M68kGeneralStartupEnvironment::logical_table_descriptor_hint`'s linear-scan
  override.
- `src/main.cpp`: the `--external-hints <path>` CLI opt-in on
  `emit-general-startup-bridge-c`, reusing the existing `--rom-sha256` value
  as the expected identity, and the stderr provenance log.
- `tools/genesis_startup_bridge.py`: the `--external-hints <path>` driver
  flag, forwarded verbatim to the emitter command (including inside the
  existing multi-round Phase B expansion loop, since it is part of the base
  emitter command every round derives from).
- Everything from the finite selector-value input onward is reused
  unchanged, per ADR-0022's own ownership list.

## Consequences

SEG-007-T164 implements and tests exactly this mechanism with focused
synthetic fixtures (`tests/m68k_pipeline_test.cpp`): a positive resolution
through a base-matching annotation of an otherwise-unprovable selector site;
a base-mismatch fall-through to the unannotated (here, unprovable) selector
proof; not-opted-in regression coverage; an annotated domain producing an
odd/invalid entry (fail-closed); a cap-exceeding `entry_count` (fail-closed);
a within-cap `entry_count` whose `stride_bytes` wraps its true maximum
implied position into a structurally-plausible collision with a genuinely
small stride (fail-closed, an adversarial-collision regression found during
review); and, at the interchange-file layer, a ROM-hash mismatch, a
malformed/missing-field record, a non-integral numeric field, a semantic
conflict between two records sharing the same table identity, a missing
`provenance` object, and a malformed provenance field type (all fail-closed),
plus two positive parser acceptance tests (one well-formed record, and two
semantically identical records differing only in provenance resolving to
that one shared content). Ordinary raw-ROM recompilation without
`--external-hints` is unaffected: every existing ADR-0009/ADR-0022 test in
the suite passes unmodified, and the default-empty hint list makes the new
environment method a universal no-op for every caller that does not opt in.

SEG-007-T164 also re-executed the authorized pinned Sonic Phase-B production
route with and without a base-matching annotation opted in for the specific
IRQ6/VBlank dispatch table, using a concrete `entry_count` domain claim
established during that task's own disposable feasibility preflight (Scope
item 0). See SEG-007-T164's Evidence for the normalized structural result of
that route (no raw address/opcode/byte content, per the project charter's
commercial-derived-evidence reduction rule).

### Reuse caveat (unchanged from ADR-0022)

This mechanism does not alter ADR-0022's own "Reuse caveat": at a site where
the annotation's implied domain happens to have every entry land on an even,
in-image, admissible target, the whole-set admission and cap checks provide
no additional protection against an over-counted `entry_count` beyond what
"Trust boundary" above already states. Any reuse of this annotation at a new
site should be undertaken with that limitation in mind, and does not by
itself establish an independent bound on the real table's entry count.
