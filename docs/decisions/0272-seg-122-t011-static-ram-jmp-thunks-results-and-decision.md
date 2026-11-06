# ADR 0272: SEG-122-T011 static work-RAM `JMP (xxx).L` thunks with a live operand - results and decision

- Status: Accepted. **Decision: `STATIC_THUNKS_ONLY`.** Static generation, the live-operand body, fail-closed behaviour and the one-shot build all pass their frozen gates
  (ADR 0271, applied unchanged). `ONE_SHOT_RAM_JMP_ACCEPTED` is not claimed because gate C6 is not met: there is no independent, pinned Genesis-level reference for H-interrupt
  delivery, so the correctness of the changed interrupt-delivery behaviour is not independently established. **Default-on promotion is withheld: the PR stays draft and must not be merged
  on this evidence alone** (operator decision, or a Genesis reference run on the same routes, is required).
- Limits: the routes below are evidence for those routes only. They show neither game compatibility nor that every dynamic H target is covered.

## 1. What was built

- Producer (`populate_immutable_rom_aot_entries`): bounded positive evidence from the discovered instruction graph (ADR 0271). Both investigated stubs were recognized on the
  authorized standalone and combined images with no execution of guest code; no address, signature or title appears in the code.
- Body: one precompiled `JMP (xxx).L` per admitted stub, guarded on the opcode word only, reading the operand live, masked to the 24-bit bus, checked against the one final compiled-entry
  table, 12 cycles. Reuses the computed-jump branch of the M68K emitter; no second target authority.
- Admission: the entry is a mandatory RAM-keyed identity; its representative operand yields no fixed successor in the validator, prune or closure. `genesis_admission_analysis_contract`
  is now `segarecomp.admission_contract.v3` (policy identity and the launcher cache key change: every Genesis Optimized entry rebuilds once, as in ADR 0270).
- Interrupt delivery is unchanged scheduler code: the existing wiring finds a compiled entry at the work-RAM vector address.

## 2. Gates (ADR 0271, thresholds unchanged)

| gate | result | evidence |
|---|---|---|
| C1 static | pass | T1 and T2: `ram_thunks=2`, `roots_added=2`, both stubs in the **first** emission (`m68k_alias_preparation rounds=0`); generated main installs the level-6 handler and vector 28 at the two stubs. Negative control (an unrelated RAM address with the same stores): no entry (synthetic fixture G) |
| C2 one shot | pass | T1/T2 `CAND`: 1 M68K emission, 0 preparation rounds, 0 observed thunks. `BASE`: 2 emissions, 1 round, 1 observed thunk (the other stub never found). Observing the same address later is subsumed (fixture H), never a conflict |
| C3 fail closed | pass | fixtures D-G (below): typed stop, no register, PC, SR, RAM or scheduler tick changed. Fixture H unchanged |
| C4 CPU semantics | pass | pinned Musashi 313ebf1b: PC, SR, D0-D7, A0-A7 and cycle count equal for 7 live operands (including 24-bit masking and a high-byte-garbage operand) |
| C5 interrupts | pass on measured routes | below |
| C6 independent reference | **not met** | no pinned Genesis-level emulator harness exists in the repository (the only independent semantics oracle is Musashi, CPU only). A Mednafen binary is installed locally, but it is not pinned, no state/RAM extraction harness exists for it in the repository and it was not evaluated or used as proof |
| C7 non-regression | pass | fast preset (209 tests) plus 13 focused suites at the validated head; strict-C11 compile of every synthetic generated program |

### Synthetic fixtures (`genesis_static_ram_jmp_thunk_generated_test`, strict C11, deterministic)

A both stubs in the first emission, opcode-only guard, no fixed target in the body. B real scheduler run (400,000-dispatch budget): the level-6 handler runs 36 times and the two level-4
handlers 4,143 and 4,142 times through the one retargeted body, no stop. C construction behind a branch the preparation never takes: entries still emitted. D uninstalled thunk: refused. E JSR,
JMP.W, NOP, JMP (A0), a different word over the opcode: refused. F odd, device-space, beyond-image, work-RAM and `$FFFFFFFF` operands: `unresolved_indirect_target`, nothing committed;
`$AB002000` runs as `$002000`. G conflicting store, long store spanning the opcode, a different opcode, no operand constructor, `CLR.W`, unreached construction, register-indirect writer,
unrelated address: no entry for the affected stub (the unaffected stub keeps its own). H subsumed materialization, refused different form, unrelated materialization works. Plus admission:
a first-emission selective plan holds both stubs and follows no live operand; a destination outside the plan stops with the typed stop.

### Real routes (R1, ADR 0271 addendum; headless, 7,504 virtual frames, finite budget, 1,800 s timeout)

| image | arm | stop | H-interrupts undelivered | frames / audio digest | against `BROAD-CAND` |
|---|---|---|---|---|---|
| T1 | `CAND`, `BASE`, `BROAD-CAND` | none | 0, 0, 0 | all equal | `CAND` equal on every field |
| T2 | `CAND` | none | 0 | equal to `BROAD-CAND` | equal on every field |
| T2 | `BASE` | none (default skip policy) | **2** | frames equal; audio digest and YM write count differ | explained by the two dropped level-4 interrupts |
| T2 | `BASE`, strict H-interrupt policy | `hint_handler_not_installed` at virtual frame 828 | - | - | `CAND` strict: runs all 7,504 frames, no stop |

R1 reaches gameplay on both (inspected captures: standalone in a level, combined in an in-game scene), but it exercises few H-interrupts: T1 never enables one, T2 two. So the route shows
that the previously dropped level-4 vector is now delivered and that `CAND` equals the broad program, not that H-interrupt effects (CRAM/VSRAM splits, water lines) are correct.
The 84-act coverage of ADR 0270 was **not** re-run: its validation image and route scripts were local, were not retained, and re-creating them would have meant a derived ROM.
`CAND` is also not independent of `BROAD-CAND` for the new behaviour (both use the new entries); only `BASE` is the independent older executable.

H-target presence: the authorized image has 11 immediate store sites writing 7 distinct handler addresses into the level-4 operand; all 7 are in the emitted compiled-entry table of the
selective `CAND` program (and of `BASE`), so a retarget to any of them dispatches. A retarget to an address outside the table stops with the typed stop; the destination set is not proven.

## 3. Cost (report only; one cold build per arm, not the median of three; `segarecomp build`, GCC 13.3 `-O1`, `--jobs 4`, one build at a time)

| image | arm | M68K emissions | CPU s (user+sys) | wall s | peak RSS MiB | final C MB | exe MB | K/U |
|---|---|---|---|---|---|---|---|---|
| T1 | `CAND` | 1 | 311 | 113 | 1,168 | 227.3 | 47.0 | 36.9 % |
| T1 | `BASE` | 2 | 576 | 211 | 1,197 | 227.3 | 47.0 | 36.9 % |
| T1 | `BROAD-CAND` | 1 | 704 | 223 | 1,999 | 515.1 | 110.1 | - |
| T2 | `CAND` | 1 | 574 | 217 | 2,332 | 405.2 | 83.0 | 37.0 % |
| T2 | `BASE` | 2 | 1,067 | 402 | 2,385 | 405.2 | 83.0 | 37.0 % |
| T2 | `BROAD-CAND` | 1 | 1,245 | 408 | 3,858 | 883.9 | 193.4 | - |

Identities: U 989,197 / K0 357,821 / K 365,269 (T1) and 1,981,349 / 711,028 / 733,828 (T2); `BASE` U is one lower and K one lower (the level-4 stub is absent). Thunks found statically 2/2, by
observation 0 (`BASE`: 0 statically, 1 by observation). No materialization-driven M68K recompilation. Cumulative compiled C is not reported: the CLI does not expose per-round unit counts;
`BASE` compiles its first emission and then the changed units, which is the CPU difference above (about -46 % on both images). Z80 materialization still runs (the build is not claimed one-shot overall).

## 4. Proof authority and fail-closed review (summary)

The producer proves only that the cartridge's reachable code contains the constant stores; it never proves installation order or the absence of other writers. The runtime opcode guard
therefore stays authoritative and any mismatch refuses with nothing committed (an interrupt's own entry effects are not undone and are not claimed to be). A static identity never
relaxes target authority: destinations are checked against the final compiled-entry table on every entry. Known limits: recognition is `JMP (xxx).L` from level-4/level-6 vector targets only;
an odd destination stops instead of raising an address error; the legacy C3 emission route has no shared compiled-entry lookup, so it cannot emit such an entry (it yields a translation rejection; not exercised, since only the general-startup route resolves the vectors).

## 5. Consequences and next steps

- Product defaults: `STATIC_THUNKS_ONLY` withholds default-on promotion of the changed level-4 delivery. Merge needs an operator decision or a Genesis-level reference run on the same routes
  (level-4 effects on CRAM/VSRAM, H-interrupt counts, RTE and timing) or on the retained all-acts coverage.
- Identity: contract v3; every Genesis Optimized library entry rebuilds once.
- Not done: `JMP (xxx).W`; other vectors; a Genesis-level reference.
