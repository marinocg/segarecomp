# Patch packages (`.segapatch`, schema 1)

A patch package is a small, portable, **data-only** file that describes one optional modification of one verified game. Segarecomp never executes anything from a package: no code,
scripts, shared libraries or plugins, and no runtime memory injection. A verified package produces a *patched input image*; the normal static recompilation pipeline then processes
that image into a separate generated-native program (Patched variant). Vanilla is never modified.

Format ADR: `docs/decisions/0266-seg-122-portable-patch-packages-and-game-variants.md`. Implementation: `libs/media/src/patch_bps.cpp`, `patch_package.cpp`
(`segarecomp/patch.hpp`), the CLI seam `segarecomp patch ...` and the launcher (`apps/segarecomp-launcher/patch_library.*`) which calls the same functions.
`tools/segapatch.py` is an independent (Python) author tool and reference reader used by the tests.

## Layout

```text
sonic1-widescreen.segapatch      (ZIP; stored or deflate; classic ZIP, no encryption, no ZIP64)
├── manifest.json
└── game.bps                     (the `rom` payload named by the manifest)
```

## `manifest.json`

```json
{
  "format": "segarecomp-patch",
  "schema_version": 1,
  "id": "sonic1-widescreen",
  "version": "1.0.0",
  "name": "Sonic 1 Widescreen",
  "platform": "genesis",
  "purpose": "widescreen",
  "source_sha256": "<SHA-256 of the exact original image>",
  "requires": [],
  "widescreen_width_max": 100,
  "payloads": {
    "rom": { "format": "bps", "file": "game.bps", "sha256": "<SHA-256 of game.bps>", "target_sha256": "<SHA-256 of the patched image>" }
  }
}
```

The manifest is strict: a closed JSON subset (objects, ASCII strings, one small integer, one array), duplicate or unknown keys are errors, every string is bounded.
`requires` lists compatibility capabilities the package needs from this segarecomp (none exist in v1: any entry is refused, so a future package cannot be silently half-applied).
`widescreen_width_max` (0..100, optional) states the highest Widescreen Width the author validated; it is informational and never drives the capability report. `payloads` holds
the payload types this release understands (closed list; v1: `rom`). A package naming another payload type is refused, not ignored. The package **identity** is the SHA-256 of the manifest
bytes (it names every payload and target digest), independent of ZIP layout and timestamps.

## What is verified before a package is accepted

ZIP: entry count, names (no separators, traversal, absolute or drive paths, control or non-ASCII characters, leading dots), no duplicates, no links or directories, no encryption,
declared sizes bounded and enforced while inflating (a stream that expands past its declared size is refused), CRC-32, no overlapping entries or trailing data. Manifest: schema, digests,
bounds. Payload: SHA-256. Then the original image: platform and full SHA-256 must equal `source_sha256` (never a title or region label), the BPS source size and CRC-32 must match, every
command stays inside the source / output, copy offsets are range-checked, the output never exceeds 16 MiB, the command stream ends exactly at the footer, the target CRC-32 and patch CRC-32
match, and the patched image's SHA-256 equals `target_sha256`. Only then is anything published (temporary file + rename); a failure leaves no partial file.

## CLI

```text
segarecomp patch inspect <package>
segarecomp patch verify  <package> --rom <original image>
segarecomp patch apply   <package> --rom <original image> --out <patched image>
```

One JSON object on stdout (`{"ok":true,...}` or `{"ok":false,"error":"..."}`); exit 0 / 1 (refused) / 2 (usage). `apply` never overwrites its destination unless the whole verification succeeded.

## Authoring

`tools/segapatch.py create --source <original> --target <patched> --id ... --name ... --version ... [--width-max N] --out <file>` writes a deterministic package (fixed timestamps and
entry order). Packages for commercial games are authored in the private harness; no ROM, patched ROM or reconstructable game data is ever committed here.
