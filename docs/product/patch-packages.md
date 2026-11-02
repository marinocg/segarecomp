# Patch packages (`.segapatch`, schema 1 and 2)

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
  "presentation_defaults": { "widescreen": "enhanced_gameplay", "side_style": "raw" },
  "payloads": {
    "rom": { "format": "bps", "file": "game.bps", "sha256": "<SHA-256 of game.bps>", "target_sha256": "<SHA-256 of the patched image>" }
  }
}
```

The manifest is strict: a closed JSON subset (objects, ASCII strings, one small integer, one array), duplicate or unknown keys are errors, every string is bounded.
`requires` lists compatibility capabilities the package needs from this segarecomp (none exist in v1: any entry is refused, so a future package cannot be silently half-applied).
`widescreen_width_max` (0..100, optional) states the highest Widescreen Width the author validated; it is informational and never drives the capability report. `presentation_defaults` (optional; `widescreen`: `original` | `extended_backgrounds` | `enhanced_gameplay`, `side_style`: `comfort` | `borders` | `raw`; either one or both) are the starting
widescreen presentation of the Patched variant only: a default the user can change (saved per patched variant), never a lock and never provenance. `payloads` holds
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

`tools/segapatch.py create --source <original> --target <patched> --id ... --name ... --version ... [--width-max N] [--default-widescreen NAME] [--default-side-style NAME] --out <file>` writes a deterministic package (fixed timestamps and
entry order). Packages for commercial games are authored in the private harness; no ROM, patched ROM or reconstructable game data is ever committed here.

## Schema 2: native-only and combined packages (ADR 0267)

Schema 1 packages stay readable and unchanged. Schema 2 adds the typed payload `native`, next to or instead of the BPS `rom` payload:

```text
sonic1-native-widescreen.segapatch
├── manifest.json        (schema_version 2; "requires": ["genesis.sonic1.native-widescreen.v1"])
├── game.bps             (optional `rom` payload; absent in a native-only package)
└── widescreen.json      (the `native` payload: a closed descriptor, never code)
```

```json
{"format": "segarecomp-native-descriptor", "schema_version": 1, "capability": "genesis.sonic1.native-widescreen.v1",
 "source_sha256": "<exact original image>", "min_implementation": 1, "components": ["hud", "scenery"], "width_max": 100}
```

The descriptor only SELECTS an audited capability of this release: `capability` must be in the release's table (`segarecomp::patch::kNativeCapabilities`), `source_sha256` must be the one image
that capability is verified for and equal the manifest's, `min_implementation` may not exceed the implementation built in, `components` is a non-empty set of that capability's components,
`width_max` (optional, 0..100) must agree with the manifest's `widescreen_width_max`. The manifest must `require` exactly that capability, so a release without it refuses the package at
install and build instead of ignoring the payload. Unknown fields, malformed or oversized (> 4 KiB) descriptors, another revision, a repeated or unknown component and every payload type
other than `rom` and `native` are refused. A native-only package leaves the image bytes unchanged (the effective image is the original) and is still a separate variant: the package identity
(digest of the manifest, which names the descriptor digest) is part of the cache key, together with the capability id, its implementation version and the descriptor digest. A package may
carry both payloads (combined): the BPS is applied first, the native capability then serves the patched image.

`tools/segapatch.py create --source ROM [--target PATCHED] ... --native-capability ID --native-components hud,scenery` authors schema 2 packages deterministically.
`segarecomp patch inspect|verify|apply` reports `schema_version`, `has_rom`, `has_native`, `native_capability`, `native_components`, `native_implementation` and `native_descriptor_sha256`.
`segarecomp build` accepts `--native-capability --native-implementation --native-components --native-source-sha256 --native-descriptor-sha256` (the launcher passes the values of the verified
package; the build re-checks them against its own table and links the matching provider; any mismatch, a headless build or a test fixture capability is refused).
