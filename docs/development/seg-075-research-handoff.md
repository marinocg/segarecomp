# SEG-075 research handoff (cross-platform M68K corpus)

Non-sensitive guide for an agent continuing SEG-075 work **without** repeating source acquisition, toolchain builds, extraction,
training or inference. The private material (third-party sources, binaries, listings, labels, models) lives only in ignored storage
under the harness checkout and is never committed. The committed receipts contain aggregates and identities only.

## Private storage

| item | location (relative to the harness root) |
|---|---|
| T001 qualification cache (live working root) | `.cache/seg075/q/` (`src/`, `build/<candidate>/<recipe>/{a,b}`, `records/`, `toolchains/`, `xplat/`, `pilot/`, `toolchains.json`) |
| content-addressed handoff store | `.cache/seg075-handoff/` (`cas/sha256/<2>/<sha256>`, `manifests/<label>.json` + `.sha256`, `images/`) |
| toolchain image archive | `.cache/seg075-handoff/images/segarecomp-xplat-toolchains.tar` (also a store object) |
| SEG-072 bundle (consumed read-only) | `.cache/seg072-handoff/` (see `seg-072-research-handoff.md`) |

The store holds independent read-only copies (content-addressed, deduplicated), never hardlinks of the live cache. The T001 tool rewrites
records, helpers and logs in place, so a hardlinked store would lose its evidence on the next rebuild. Never open a store object for writing;
`verify` detects it, but cannot recover the original.

## Verify (fresh process, any worktree of the product repository)

```sh
python3 tools/segarecomp_xplat_handoff.py verify --fresh-process \
    --manifest <HARNESS>/.cache/seg075-handoff/manifests/<label>.json
```

Add `--receipt tools/segarecomp_xplat_handoff.<label>_receipt.json` to bind the manifest to the digest committed in the product. It
must print `"verdict": "PASS"`. It rehashes every store object (physical bytes, not saved JSON). It checks sizes, the manifest digest,
the image-archive identities, and that no entry depends on a task worktree. `--check-origin` additionally reports whether the live origin
files still match. The T001 recorded-evidence check is separate:

```sh
python3 tools/segarecomp_xplat_qualify.py verify --cache <HARNESS>/.cache/seg075/q          # records vs the committed manifest
python3 tools/segarecomp_xplat_qualify.py identity --cache <HARNESS>/.cache/seg075/q --check  # v2 recipe identity drift
```

## Restore

- Files: `python3 tools/segarecomp_xplat_handoff.py restore --manifest <...>.json --to <empty dir>`. This materializes the logical tree
  (paths relative to the harness `.cache/`) as independent copies; a restored `seg075/q` can be passed as `--cache` and rebuilt. Use
  `--hardlink` only for read-only inspection.
- Toolchain images: `docker load -i <HARNESS>/.cache/seg075-handoff/images/segarecomp-xplat-toolchains.tar` restores
  `segarecomp-xplat/tc-{gnu,elf,vbcc,a68k,mint}:1` with their recorded image ids.
- Rebuilding toolchains from the committed Dockerfiles is a fallback only. tc-gnu and tc-mint depend on floating package versions (ADR 0160,
  section 4).

## T002 corpus (no re-extraction needed)

`<HARNESS>/.cache/seg075/t002/corpus/` holds 54 canonical samples in the SEG-072 registry layout. Load them with the unchanged
`segarecomp_ml_v3_train.Image(<sample dir>)`; `tools/segarecomp_xplat_corpus.py load --corpus <dir>` checks all of them. To reproduce:
`segarecomp_xplat_corpus.py truth --cache <q> --out <truth>` (about 2 minutes, needs the toolchain images), then `extract --cache <q> --truth
<truth> --cli <segarecomp binary> --out <corpus>`. The result must reproduce the corpus manifest digest recorded in
`tools/segarecomp_xplat_corpus.coverage.json`.

## Manifests

| label | content |
|---|---|
| `c1-t001-audit` | C1 audit of the T001 cache: checkouts, both runs of every recipe, records, toolchain inputs, Dockerfiles, helpers, image archive |
| `c2-t002-corpus` | `c1-t001-audit` + T002 truth runs (`seg075/t002/truth`), canonical corpus (`seg075/t002/corpus`, 54 samples), decoder CLI binary |

Later checkpoints add labels (T002 corpus, T003 snapshot, T004/T005 models, final C7 handoff); see the receipts committed under `tools/`.
