# ADR 0049: Immutable-Copy Alias for Work-RAM Execution (with static PEA return authority and cartridge mirroring)

- Status: Accepted
- Date: 2026-09-28
- Task: SEG-021-T041
- Related, unchanged: ADR 0006 (owned cartridge-data regions), ADR 0009 (computed indirect control flow),
  ADR 0011 (whole-program JSR-continuation set for RTS), ADR 0039 (compiled-entry precedence), ADR 0048
  (push-then-RTS computed jump).

## Context

A commercial title copies a routine verbatim from immutable cartridge ROM into work RAM at startup (a fixed
RAM address, the copy performed by ordinary generated code) and then calls it with `JSR abs`. The generated
program is fully ahead-of-time: every executable identity is a compiled body keyed by its execution PC. The RAM
target had no compiled body, so the run stopped fail-closed at the first call (`known_but_unemitted_target`).

Blindly dispatching the RAM address to the ROM-anchored body is unsafe: a ROM body bakes its own relative
control-transfer targets, fallthrough/continuations and PC-relative operand addresses as compile-time literals
anchored to the ROM source address. Running it while the CPU executes from RAM would silently compute every one
of them from the wrong base.

Public hardware background: the MC68000 computes relative branch targets, `(d16,PC)`/`(d8,PC,Xn)` operand
addresses and the pushed BSR/JSR continuation from the architectural program counter of the executing
instruction (Motorola M68000 Family Programmer's Reference Manual, Bcc/BSR/DBcc and PC-relative addressing
sections). Absolute addresses (`JSR abs`, `(xxx).W/L`) are independent of the executing address.

## Relocation audit (sanitized aggregates)

The copied region of the authorized image (one 432-byte verbatim, unique copy of an immutable ROM span) was
decoded ephemerally with the existing MC68000 decoder, once at the ROM source address and once at the work-RAM
execution address, and the two lifted operations were compared. Aggregate counts only (no bytes, addresses,
offsets or disassembly are retained):

| classification (linear sweep, one pass) | count |
| --- | --- |
| instructions decoded | 117 |
| relative control transfers (Bcc/BRA/BSR/DBcc) | 16 (8 Bcc/BRA, 7 DBcc, 1 BSR) |
| ... target inside the copied region / outside it | 13 / 3 |
| PC-relative or PC-indexed effective addresses | 0 |
| absolute call/jump targets (JSR/JMP abs) | 1 |
| instructions with an absolute effective-address operand | 20 |
| RTS/RTE/RTR | 8 |
| instructions whose lifted operation differs between the two bases | 16 (exactly the relative transfers) |
| position-independent instructions | 101 |

The region is therefore **not position-independent** (16 of 117 identities are execution-address dependent), so
the ROM-anchored bodies cannot be reused. Absolute operands and the one absolute call target must stay absolute.
Three relative targets leave the region: taken at run time they land on a RAM address that has no compiled body
and fail closed, exactly as a jump into unknown RAM must.

## Decision

1. **An alias is the same immutable bytes compiled a second time for an alternate execution base.** A generated
   data descriptor `(execution_base, source_base, length)` (work RAM, even, no wrap; the source span wholly inside
   exactly one structurally valid `raw_cartridge_rom` claim and the image) asks the analysis to decode every
   aligned start of the source span a second time **with the existing decoder and lifter**, passing the
   work-RAM execution address as the decode source address and keeping the image offset at the immutable source.
   There is no second decoder, no hand relocation and no rewriting of lifted operations: PC-relative operand
   addresses, relative branch/DBcc/BSR targets, fallthrough and call continuations are execution-relative
   because they are derived from the provenance address, and absolute targets/operands stay absolute. An alias
   identity is admitted only when its whole span lies inside its descriptor and the ordinary AOT-safety and
   complete-emission predicates hold. `ImmutableRomAotEntry` carries `execution_alias` and
   `alias_source_address`; the codegen validator selects the mapping at the source address and re-checks the
   work-RAM window, alignment and raw-byte presence. Architectural PC, exact-PC obligations, the compiled-entry
   table, JSR/BSR continuations and provenance all use the execution address. Descriptors are generated data
   derived from the current image and never committed.
2. **A runtime byte-identity guard is the sole authority.** Every alias body begins with a comparison of exactly
   its own instruction's bytes (from the immutable image) against the work-RAM bytes at its execution address,
   before any effect. A mismatch returns the existing typed `known_but_unemitted_target` stop with the
   instruction provenance and commits nothing. The comparison is identity only: nothing in RAM is ever decoded,
   compiled or cached. Because the guard runs at every fetch, bytes changed later (self-modification) fail
   closed at the changed instruction. A wrong or stale descriptor can therefore never execute wrong code.
3. **Descriptor discovery is tooling, not the runtime, and a separate optional preparation phase.**
   `--immutable-copy-alias <execution>:<source>:<length>` is the low-level emitter descriptor input (tests, future
   static analyzers, preparation artifacts); `tools/genesis_startup_bridge.py --discover-copy-aliases` is the normal
   high-level helper that produces such descriptors. The invocation lifecycle is:
   - an optional, bounded **preparation phase** (only with `--discover-copy-aliases`): up to 64 rounds (a
     test-only environment override lowers the bound), each round one generation + one compile + one run of the
     headless capture executable of the program built with the aliases found so far. When the guest stops
     fail-closed at a work-RAM PC, the capture hook writes a private ephemeral work-RAM dump; the tool finds the
     maximal verbatim run of the immutable image around that PC (both directions, constant delta, minimum 16
     bytes; identical ROM occurrences are interchangeable because the alias decodes the identical bytes at the
     execution address), merges same-delta neighbours, and iterates. The phase ends with exactly one aggregate
     `COPY_ALIAS_DISCOVERY` line (`rounds`, `generation_attempts`, `compile_attempts`, `run_attempts`,
     `alias_count`, `alias_total_bytes`, `termination_reason`), printed even for zero aliases; no addresses,
     offsets or dumps are reported. `termination_reason` is a closed vocabulary: `route_advanced` (a run ended
     without a guest stop), `runner_resource_limit` (host dispatch allowance reached without a stop),
     `no_work_ram_frontier`, `frontier_not_verbatim_copy`, `non_alias_frontier` (the stop cannot be explained by a
     copy alias), `repeated_alias_no_progress`, and the two incomplete outcomes `max_rounds` and `tool_failure`.
     Incomplete preparation fails closed: nonzero exit, no final program is generated or run. Every other outcome
     proceeds to the final phase, which then reports any remaining frontier honestly.
   - the canonical **final one-shot phase** (`--one-shot`): exactly one generation, one compile and one run of
     the final program, whose emitter command carries exactly the aliases found (or provided) before emission.
     `--one-shot` governs this final program and forbids runtime-confirmed code-discovery expansion of it.
     `ONE_SHOT_SUMMARY` describes the final phase only and is unchanged when no discovery flag is given; the whole
     invocation is therefore N preparation builds plus one final build, never "one generate/compile/run".
   Without `--discover-copy-aliases` there is no preparation: no extra execution, no work-RAM dump, no alias search,
   no extra compile round. The emitter re-validates every descriptor (`apply_genesis_immutable_copy_alias` fails
   closed on odd/zero/oversized/wrapping/overlapping/unowned input).
4. **Static PEA return authority (same generic family, found by the rerun).** A `PEA` with a statically
   foldable effective address (absolute or PC-relative; execution-relative for an alias identity) pushes a fixed
   code address (the classic manual call `PEA next; JMP/BRA callee`). Such an address that is itself a final
   emitted code address joins the whole-program RTS return set exactly like a call continuation (ADR 0011). The
   RTS still pops the real stack slot; unrelated compiled addresses and non-compiled values still fail closed.
5. **Cartridge mirroring for reads past a power-of-two ROM chip.** A raw cartridge claim mapped at the window base
   whose size is a power of two (at least 64 KiB) is mirrored across the rest of the 4 MiB cartridge window
   (the chip does not decode the upper address lines; Genesis Plus GX `core/cart_hw/md_cart.c` builds its map as
   `cart.rom + ((page << 16) & (size - 1))`, and the Genesis cartridge window is $000000-$3FFFFF per the Sega
   Genesis Software Manual memory map). The bridge emits one extra region row per mirror over the **same**
   embedded data array (no data duplication); the runtime's bounds-checked read path is unchanged. Any other
   shape (non-power-of-two, non-zero base, smaller than 64 KiB) gets no mirror and keeps failing closed. Padding
   behavior of non-power-of-two chips is not modeled (unsupported, documented).

## Non-goals and unsupported behavior (explicit)

- No interpreter, JIT, runtime opcode decode, generic code cache or taint/provenance engine. Only fixed,
  build-time-known verbatim copies of immutable ROM can be aliased; transformed copies, decompressed code and
  runtime-generated code remain fail-closed frontiers.
- Aliases are single-instruction guarded; prefetch-queue timing of an instruction modified two words ahead of
  the PC is not modeled (the project executes whole instructions).
- Work-RAM mirrors ($E00000-$FEFFFF) are not aliased: only the canonical `$FF0000-$FFFFFF` window (a PC in a
  mirror has no compiled body and fails closed).
- Alias descriptors are proposals derived from observed fail-closed stops; a route never observed reaching the
  copy has no alias and stops honestly.
- Discovery is workload/path dependent. It finds only ROM-to-RAM executable copies reached by the preparation
  runs: a boot, attract-mode copy and execute is discovered; a title that waits for START and only then loads a
  level and copies code is not discovered unless the preparation run exercises that path. The final executable
  contains only aliases discovered or provided before emission; a later, unseen executable RAM copy stays
  fail-closed. A title that runs without a stop is not proof of complete alias coverage. The helper is a temporary,
  explicit tool (no default, no persistence), and no game-specific descriptor is committed.

## Consequences

- Tests: `genesis_immutable_copy_alias_generated_test` (a project-authored fixture: generated ROM-to-RAM copy loop,
  transfer to the RAM copy, arithmetic, BSR call/return, fallthrough, relative branch, PC-relative data read that
  observes the RAM data word, absolute store, RTS; architectural PC and pushed continuation are RAM addresses;
  every byte of every executed instruction tampered individually fails closed before any effect; never-copied and
  later-changed bytes fail closed; ROM execution of the same routine is unchanged; the alias-less control still
  stops at the RAM target; no decoder in the generated program; deterministic; strict C11 with `-Werror`),
  descriptor validation checks in the same test, `genesis_pea_static_return_generated_test`,
  `genesis_cartridge_mirror_region_test`, `genesis_copy_alias_discovery_test`, and
  `genesis_copy_alias_lifecycle_test` (preparation/final lifecycle accounting, every termination reason,
  fail-closed round-bound exhaustion, and no preparation without the flag).
- The route reaches a stable generated-native presentation with no fail-closed frontier (sanitized frontier
  sequence recorded in the task evidence: alias, PEA return authority, cartridge mirror).

## Known limitation: cartridge SRAM

The project models no cartridge SRAM/backup RAM. A power-of-two image of 1 or 2 MiB
with SRAM at `$200000` would read mirrored ROM there under the mirroring rule above.
Modelling SRAM is out of scope for this decision, and nothing here claims that SRAM-bearing titles run correctly.
