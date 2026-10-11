# ADR 0105: SEG-049-T006 launcher policy parity (thin client of `segarecomp build`)

- Status: accepted (checkpoint C4). No default change: the launcher opens in Compatibility, Optimized is v1, the experimental model is an explicit secondary opt-in.
- Predecessors: ADR 0096 (launcher mode), ADR 0103 (authority/lifecycle), ADR 0104 (status contract).

## 1. Decisions

- **Thin client.** The launcher passes `--aot-policy optimized` and, only when the user opted in, `--aot-model gen2` to `segarecomp build`, then *reads* `status.json` `aot_policy`. It holds no
  producer, validator, threshold, schema or compatibility rule (`ml_producer_boundary_test` forbids producer headers/symbols and threshold/model-file terms in launcher sources).
- **Normal UI unchanged:** Mode = Compatibility | Optimized. One small secondary control, **Experimental**, appears only while Optimized is selected (the opt-in of ADR 0103; it is never
  selected by default or implicitly). The status line says "Optimized build", "Optimized build (experimental model)" or, on a fallback, "Optimized unavailable for this game: built
  Compatibility" (never "Optimized" for a broad program). A **Details** / **Why?** button opens the advanced diagnostics panel: requested/effective policy and model, fallback reason and class, model
  family and rank policy, model id, selected/admitted fractions, validator state, build and policy identities (sanitized product fields only).
- **Same semantics as the CLI.** Headless mode accepts `--aot-policy` / `--aot-model` (same validity rule) and prints the same product fields (`aot_policy_*`, including class, family,
  validator, fractions and both identities); the GUI and headless share the same core.
- **Cache identity from the product.** The entry key adds the model family and the *product-provided* ROM-independent `policy_identity` (`segarecomp aot-policy-identity`, which binds model,
  schema, token contract, rank policy and analysis contract), memoized per tool/policy/model; Compatibility keeps its historical key. In addition `entry_ready_for` refuses to reuse a finished
  entry whose recorded request, requested model or `policy_identity` differ from the current request/tool, so a stale or foreign entry is rebuilt, never reused, even if it somehow shares a
  directory. Mode and model switches recompute the entry and the Ready/RomSelected state; the old entry stays valid for its own request.

## 2. Evidence

- `launcher_policy_test` (CTest, built by the SDL3 CI job on three OSes; now also takes the real CLI): v1 / gen2 / Compatibility entries never alias; a model request never changes a Compatibility or Master System
  entry; product identities are distinct stable 64-hex digests; seven entry-reuse cases (matching gen2/Compatibility reuse; v1-vs-gen2, gen2-vs-v1, Compatibility-vs-Optimized, Optimized-vs-Compatibility and stale-identity
  all refused); report/summary/details wording including the gen2 fallback.
- Local headless end-to-end (fake package root with the real CLI, a gcc shim for the bundled compiler, locally built SDL3, the synthetic Genesis image): Compatibility, Optimized(v1) and Optimized+gen2 each
  miss on first build and hit on repeat with separate entries; the v1 build reports `ml_region`, the gen2 build `ml_region_gen2` (validator accepted, selected 35.2 %, admitted 5.5 % of this synthetic universe) and
  both `policy_identity` values equal the CLI's. The packaged-archive version of this run is part of the T007 smoke test.
- Review path: `SEGARECOMP_SHOT=<bmp> SEGARECOMP_SHOT_POLICY=optimized SEGARECOMP_SHOT_MODEL=gen2 SEGARECOMP_SHOT_ROM=<rom>` renders the Genesis panel without a display (dummy video driver); the layout was reviewed
  for the three states (the Experimental control fits inside the panel).

## 3. Limits

- The graphical flow was reviewed through rendered frames and the headless core, not by an interactive session. Fallback wording is covered by tests for the report text, not for pixels.
