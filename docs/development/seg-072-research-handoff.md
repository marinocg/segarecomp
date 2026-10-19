# SEG-072 research handoff (M68K ML v3)

Non-sensitive guide for an agent continuing M68K executable-region ML work (e.g. SEG-073) from the SEG-072 artifacts **without retraining**.
SEG-072's frozen results and classification (`RECALL_FAILED`, ADRs 0151-0154) are final; nothing below changes them. The sealed one-shot
evaluation has been run and must never be rerun (its marker is preserved); the five qualified sealed lineages are now exposed and cannot serve
as fresh blind evidence.

## The private bundle

The artifacts contain commercial-derived material (instruction labels, per-byte classes, scores). They live only in ignored private storage of
the harness checkout and are never committed:

| item | location (relative to the harness root) |
|---|---|
| bundle directory | `.cache/seg072-handoff/bundle-v1/` (`MANIFEST.json`, `MANIFEST.sha256`, `payload/`) |
| single-file archive | `.cache/seg072-handoff/seg072-handoff-bundle-v1.tar` + `.tar.sha256` |
| live working roots (same content, not modified) | `.cache/seg072/` (`SEG072_ROOT`), `.cache/seg049/` (`SEG049_ROOT`) |

`payload/` mirrors the harness `.cache/` layout, so `payload/seg072` can be used as `SEG072_ROOT` and `payload/seg049` as `SEG049_ROOT`.
Commercial images and the Sonic CD module files are **not copied**: `MANIFEST.json` `external_inputs` lists each by its expected local path and
SHA-256 (operator-held `games/` images; the SEG-063 Sonic CD disc-module cache). Verification fails if one is missing or different.

| `payload/` path | content |
|---|---|
| `seg072/data/<id>/` | development registry, 50 images: `meta.json` (identity, lineage, audit), `classes.bin` (per-byte I/c/D/U), `tokens.bin` (CPU decode tokens), `seeds.json`, `d.txt`/`u.txt` (seed and broad-universe reports; Genesis), Sonic CD `seed_starts.txt`, extraction copies |
| `seg072/truth/` | disassembly truth (start lists `*.u`, classes `*.cls`, two independent runs each) |
| `seg072/hb/<dNN>/` | SGDK builds: `inst/out/rom.bin`, `rom.map`, `classes.bin`, `build.json`, symbol tables, relist units |
| `seg072/scores/` | development out-of-fold scores: `M3R.<fold>`, `M1.<fold>`, `M4.<fold>.s<seed>` (`.npz` + `.json` with fold/train/held), `M3_FROZEN.production.npz` |
| `seg072/final/` | final weights: `M3R.pt` (+ `M3R.run1.pt`, identical), `M4.s1.pt`..`M4.s3.pt`, digest files |
| `seg072/results/` | development tables, selection, structural closure, diagnosis, economics, hardware benchmark, one-shot sealed results (`sealed.private.json` holds the frozen pair's sealed regions), post-hoc M4 aggregates |
| `seg072/sealed/` | the original `UNSEALED.marker.json`, binding, sealed truth (incl. the exposed Sonic Spinball classes and raw IDA classes), sealed tokens/seeds/universes, The Weave / Tank Battle builds |
| `seg049/` | gen2 historical outer-fold scores and results (the frozen-digest reproduction), gen2 token caches |
| `seg048/` | historical truth start lists and seed/universe reports used by the unchanged gen2 trainer |

## Environment (frozen for SEG-072 results)

Python 3.13.14 with numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1 (the `+cu130` wheel run on CPU only), float32,
`torch.use_deterministic_algorithms(True)`, threads M3-R 4 / M4-Dense 2. The private interpreter is `.cache/seg049/venv/bin/python`
(the exact package set is in `MANIFEST.json` `environment`). The ROCm environment was used only for the hardware benchmark. Truth builds used
Wine 9.0 with each SGDK release's bundled Windows toolchain and the digest-pinned SGDK image recorded in the manifest; sources are the pinned
revisions in `tools/segarecomp_ml_v3.plan.json`.

## Verify (fresh process, any worktree of this repository)

```sh
<HARNESS>/.cache/seg049/venv/bin/python tools/segarecomp_ml_v3_handoff.py verify --bundle <HARNESS>/.cache/seg072-handoff/bundle-v1
```

It must print `"verdict": "PASS"`. It re-hashes every bundled file and external input, checks the frozen record and the one-shot marker
binding, reloads the final weights and compares their state-dict digests with the recorded identities (M3-R `20bfa46f…`, M4-Dense seeds 1-3),
reproduces a reference inference sample bit for bit (final M3-R, final M4-Dense@64, frozen production M3), checks the five gen2 historical
folds against their frozen digests, checks every fold-score file against the corpus manifest and the lineage folds, and regenerates the frozen
pair's one-shot sealed regions for two sealed lineages from the preserved weights **without reading any sealed label**. To use the archive
instead, check `sha256sum -c seg072-handoff-bundle-v1.tar.sha256`, extract it anywhere and pass that directory to `--bundle`.

## Resume without retraining

- Set `SEG072_ROOT=<bundle>/payload/seg072` and `SEG049_ROOT=<bundle>/payload/seg049`.
- `tools/segarecomp_ml_v3_eval.py table|diagnose|select` recompute every development table from the stored fold scores (seconds); `kstage`
  and `econ` additionally need the release CLI and the external images.
- `segarecomp_ml_v3_train.final_scorer("M3_R" | "M4_DENSE@64" | "M4_DENSE@128" | "M3_FROZEN")` returns a scorer backed by the preserved weights;
  `ExternalImage(image, tokens, classes, seeds, id, lineage)` wraps any image for scoring.
- A new protocol must use fresh sealed lineages and pre-register before touching new truth (ADR 0151 pattern); E01/E03/E05/E06/E07 are exposed.

## Not preserved (and recomputation cost)

| artifact | status | recomputation |
|---|---|---|
| M1-HGB final model and per-fold models | never fitted / never saved (selection used out-of-fold scores, which are preserved) | final fit ~30 s; ~22 s per fold, deterministic |
| per-cell M4-Dense post-hoc sealed predictions | not saved; only aggregates (`results/posthoc.m4_sealed.json`) | ~1 min inference with `final/M4.s*.pt`, no training |
| per-cell one-shot sealed scores | not saved; the frozen regions are | seconds, label-free, with `final/M3R.pt` (the verifier does this) |
| toolchains, public sources, Wine prefix, JRE, Python 2.7, SGDK trees | public pinned inputs, not bundled | minutes to rebuild (ADR 0152 procedures) |
| sealed build trees (split/assembly working copies) | commercial-derived, not bundled; sealed truth and tokens are | ~15 min to rebuild from the pinned sources and operator images |
| research virtualenv | not bundled (5.5 GB); package set recorded | `pip install` of the recorded versions |

## SEG-073 diagnosis tooling

`tools/segarecomp_ml_v3_diagnose.py` (ADR 0155) consumes this bundle without retraining. Its subcommands are:

- `smoke`: a relocation smoke test. It resolves images through the manifest, can rebase external paths with `--rebase OLD=NEW`, and uses an
  audit hook to prove the original working roots are not opened.
- `infer`: regenerates the sealed per-cell scores by inference with the preserved weights.
- `analyze`: label credibility, model complementarity, training exposure, diagnostic metrics, and the rank-fusion / structural-expansion
  experiments.

Archived `meta.json` paths are never rewritten.
