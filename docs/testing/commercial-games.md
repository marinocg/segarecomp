# Local Commercial Game Corpus

The repository-root `games/` directory is reserved for local, user-provided commercial game images
used for exploratory compatibility and adversarial testing. It is ignored in its entirety and must
never contain a Git-tracked file. Documentation lives here rather than under `games/` so there is no
ignore exception that a ROM could exploit.

## Expected Layout

Developers may keep legally held local copies under platform directories such as:

```text
games/
  genesis/
  master-system/
  game-gear/
  saturn/
  dreamcast/
```

Genesis / Mega Drive images may use `.md`, not only `.bin` or `.rom`. The root `/games/` ignore rule
therefore applies to every file regardless of extension, name, or nesting. Do not add exceptions for
README files, `.gitkeep`, manifests, hashes, archives, patches, or generated output inside this tree.

## Developer Rules

- Treat every commercial image as untrusted and potentially copyrighted.
- Use a commercial image only when the user confirms they are legally entitled to analyze that local copy and the task defines a bounded compatibility question.
- Developers (and any local analysis tooling they run) may read the bounded portions required by the task and may run approved local analysis, discovery, recompilation, tracing, or diagnostic tools against the image.
- The developer may consume detailed command output—including addresses, instruction words, byte spans, or disassembly—inside a private, user-authorized development session when that detail is necessary to classify or diagnose the current frontier and the execution environment permits it.
- Raw commercial-derived information must remain ephemeral. Store it only in ignored temporary or cache paths when storage is necessary, remove it when the bounded diagnosis is complete, and never use it as a committed fixture.
- Never commit, push, publish, attach to a pull request, include in commit messages or CI output, or deliberately upload:
  - ROM data or extracted byte sequences;
  - disassembly or reconstructed source;
  - raw traces or complete diagnostic reports;
  - extracted assets;
  - generated commercial source, binaries, screenshots, framebuffers, or RAM dumps;
  - local paths identifying the user’s commercial corpus.

- Do not deliberately upload a ROM, a substantial extracted portion, or a bulk raw report to an external service. Bounded values consumed through an explicitly authorized local analysis workflow are not permission to disclose or redistribute the underlying image.
- Before recording or communicating a result outside the private diagnostic session, reduce it to the minimum actionable derived classification, such as:
  - instruction family, operand size, and addressing-mode class;
  - CPU or device frontier class;
  - normalized memory or device region;
  - access width and direction;
  - compiler stage and stable diagnostic category;
  - deterministic advancement or no-advancement result.

- Derived classifications that contain no reconstructable commercial content may be committed when necessary to define, plan, or validate a task. Raw addresses, offsets, instruction words, byte sequences, disassembly, and local paths remain excluded from committed evidence.
- **Narrow scalar compatibility-metadata carve-out (SEG-007-T206 / ADR-0034; record-count rationale corrected by SEG-007-T218 / ADR-0036).** `platforms/genesis/compat/<rom-sha256>.json` is the one committed (not ignored) exception to the "raw addresses... remain excluded from committed evidence" rule directly above, and it is narrow by construction: **not** by an implicit "handful of records" limit, but by field shape. Each record may carry only exactly `rom_sha256`, `kind` (currently only `"logical_table_descriptor"`), `base_address`, `entry_width_bytes`, `stride_bytes`, `entry_count`, and `provenance` — the same minimal scalar surface already required to reproduce a compatibility build via `--external-hints`, and nothing else. This carve-out exists because a `logical_table_descriptor`'s `entry_count` is, by ADR-0023's own design, a **trusted, non-structurally-provable semantic assertion**: without a committed home it would silently evaporate the moment a contributor regenerates `.tools/analysis-hints/` from scratch. A ROM-bound file may legitimately contain the **complete set** of bounded scalar `logical_table_descriptor` assertions required for that ROM — record count alone is not a semantic restriction, and a large but fully compliant per-ROM inventory (e.g. a whole-ROM harvest of one governed table shape) is exactly what this carve-out exists to hold; only a defensive corruption/runaway-generation ceiling (`tools/compat_genesis_schema.py`'s `MAX_RECORDS_PER_FILE`, currently `1024`) bounds it, never a "small curated set" design limit. The carve-out does **not** loosen any other prohibition in this document, and this correction does not loosen it either: `platforms/genesis/compat/` may never contain ROM bytes or extracted byte sequences; disassembly or reconstructed source; raw instruction words; extracted assets, screenshots, framebuffers, or RAM dumps; raw traces or complete diagnostic reports; or any bulk address inventory or raw Ghidra dump (the ignored `.tools/analysis-hints/` `code_entry_candidate` / `address_table_candidate` regenerable exports remain fully excluded from `compat/`, no matter how small or large a subset). `tests/compat_genesis_schema_test.py` enforces this exact narrow shape and fails the build on any future widening beyond it.
- Never modify, rename, delete, move, archive, or duplicate files under `games/`.
- Never force-add anything under `games/`, and never make CI or a required test depend on the local commercial corpus.
- Keep committed tests synthetic, homebrew, or otherwise explicitly redistributable.
- If a tool or hosting environment independently refuses access, stop and report that limitation. Repository policy does not override an external platform’s safety, privacy, or permission controls.

For this policy, a “private diagnostic session” means the user-authorized, non-public working context used to complete the bounded task. It distinguishes ephemeral analysis from commits, pull requests, CI, published logs, and other durable project evidence; it does not override the data-handling rules of any analysis-tool provider or execution environment.

For Ghidra, do not weaken the container mount policy or mount `games/` wholesale. A user must explicitly place an authorized analysis input in `.tools/ghidra/input/` under the security and evidence rules in `docs/testing/ghidra-analysis.md`.

## Efficient Frontier Diagnosis

Use the sanitized commercial route for durable evidence and the diagnostic route for private
attribution:

```sh
python3 tools/genesis_startup_bridge.py \
  --segarecomp build/dev/segarecomp --cc /usr/bin/cc \
  --rom "<authorized-local-rom>" --mode commercial \
  --expect-sha256 "<expected-sha256>" --diagnose-frontier
```

Stdout remains the canonical sanitized report. Stderr emits one bounded `EPHEMERAL_FRONTIER` record
with the reached instruction/access provenance and the ignored debug-binary path; it is private-session
diagnostic material and must not be copied into commits, PRs, CI, task evidence, or capability maps.
If device state rather than the access itself is ambiguous, run:

Ordinary `--diagnose-frontier` runs may use quick/optimized compilation (no debug info). For LLDB/GDB
local-variable inspection, rerun the same route with `--build-profile debug`, then pass that run's
`EPHEMERAL_FRONTIER.debug_binary`:

```sh
python3 tools/genesis_frontier_debug.py --binary "<debug_binary from EPHEMERAL_FRONTIER>"
```

The helper uses LLDB/GDB to stop at the generated runtime's access failure and print the failing access
plus the relevant VDP pointer/register/DMA state. Its output is equally ephemeral. Use one diagnostic
execution while investigating and reserve `--compare-runs` for the final determinism handoff.

## Enforcement

`tests/restricted_files_test.py` verifies that Git tracks no path under `games/` and that representative
Genesis `.md` and other filenames remain ignored. This catches accidental policy changes and forced
additions when the required gates run.
