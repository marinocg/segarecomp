# Patch packages (`.segapatch`, schema 1 and 2)

A patch package is a small, portable file that describes one optional modification of one verified game. There are two kinds, and they have very different trust properties:

* **Schema 1: data only.** One BPS delta. Segarecomp never executes anything from it: no code, scripts, shared libraries or plugins, and no runtime memory injection. A verified package produces a *patched input
  image*; the normal static recompilation pipeline then processes that image into a separate generated-native program (Patched variant). Vanilla is never modified.
* **Schema 2: optional BPS plus trusted C11 source.** A schema 2 package may carry one `provider.c` (see "Schema 2" below). That source is **executable code**: the build compiles it into the generated-native Patched program,
  where it runs with that program's operating-system permissions. **It is not sandboxed.** The SHA-256 digests in the manifest establish *integrity* (the bytes are the ones the manifest names), not *trust* and not
  *author authenticity*: there are no signatures. Install such a package only if you trust whoever gave it to you.

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
entry order). Packages for commercial games are authored and maintained outside this repository; no ROM, patched ROM or reconstructable game data is ever committed here.

## Schema 2: trusted native-source packages (ADR 0267)

Schema 1 packages stay readable and unchanged. Schema 2 adds the typed payload `native`: ONE portable C11 source file, next to or instead of the BPS `rom` payload.

```text
my-game-widescreen.segapatch
├── manifest.json        (schema_version 2)
├── game.bps             (optional `rom` payload; absent in a native-only package)
└── provider.c           (the `native` payload: executable C11 source written against the public SDK)
```

```json
{"format": "segarecomp-patch", "schema_version": 2, "id": "...", "version": "1.0.0", "name": "...", "platform": "genesis", "purpose": "widescreen",
 "author": "Your Name", "license": "MIT", "source_sha256": "<exact original image>",
 "payloads": {"native": {"format": "c11-source", "file": "provider.c", "sha256": "<digest>", "abi": 1,
                         "capabilities": ["frame.work_ram", "frame.vdp", "scenery.words", "overlay.rgba"], "executable": "yes"}},
 "widescreen_width_max": 100, "presentation_defaults": {"widescreen": "enhanced_gameplay", "side_style": "raw"}}
```

* The manifest declares the package identity (author, id, name, version, optional license), the platform and original-image digest, the optional BPS with its hashes, the provider file (always `provider.c`) and
  its SHA-256, the generic native-patch **ABI version** it was written for, the generic **host capabilities** it requires (`frame.work_ram`: read-only work-RAM snapshot synchronized with the frame;
  `frame.vdp`: read-only per-line VDP registers/CRAM/VSRAM and the frame's VRAM; `scenery.words`: it contributes name-table words for off-screen cells; `overlay.rgba`: a bounded RGBA overlay), the supported
  presentation defaults and the explicit declaration `"executable": "yes"`: the package contains executable native source. A provider must contribute scenery and/or an overlay.
* There is **no** compiler argument, linker flag, header path, build script, prebuilt binary, fetch URL or other payload type in a package: those are refused. The product alone decides how the single
  declared source is compiled (strict C11, the bundled compiler, the public SDK include directory only).
* Refused at install and at build, with a readable error: an unknown capability or another ABI version, a wrong source/BPS hash, a source that is empty, larger than 256 KiB or contains NUL, a missing
  author, a missing or false executable declaration, any undeclared, duplicate, unsafe-path or extra archive entry, unknown manifest fields, `requires` entries, and the retired descriptor-style schema 2
  package (it named a capability built into one release; there is no migration of a built-in capability).
* A native-only package leaves the image bytes unchanged (the effective image is the original) and is still a separate variant. The package identity (digest of the manifest, which names the source digest) is
  part of the variant cache key together with the provider source digest, the ABI version and the capability set.

### Trust

A package with a `native` payload is a **trusted native-source patch**. Its source is compiled into the separate Patched program and runs with that program's full permissions. **This is not a sandbox**: bounded archive parsing, source hashes,
a restricted interface and compiler flags are NOT a security sandbox and must never be described as one. Only install such a patch from a source you trust. The launcher therefore shows an explicit sheet (author, license, package
identity, source digest, declared capabilities, "contains executable source, not sandboxed") and installs/builds it only after the user approves THAT exact package; a changed package has another identity and needs a new
approval; library scanning and updates never compile a new code-bearing patch; the build re-verifies the package and the approved identity before extracting anything. A future, separately designed sandboxed
format (for example WebAssembly ahead-of-time compiled) would be a different capability; none exists today.

**Approval boundary.** The launcher records an explicit user approval of one exact package identity (the sheet's "Approve and install" button). The CLI's `--patch-identity` is only a consistency check: the build verifies that the
supplied value equals the identity of the package it reads (so a package changed after it was shown or approved is refused). An identity typed or scripted by a CLI caller is **not** independently authenticated user consent;
whoever runs `segarecomp build` directly has already chosen to compile that package. There are no signatures, certificates or identity service.

### Authoring and CLI

`tools/segapatch.py create --source ROM [--target PATCHED] --id ... --name ... --version ... --author NAME [--license SPDX] --native-source provider.c --native-caps frame.work_ram,... --out FILE` writes a package deterministically
(see `docs/product/native-patch-sdk.md`). `segarecomp patch inspect|verify|apply` reports `schema_version`, `has_rom`, `has_native`, `executable_code`, `author`, `license`, `native_abi`, `native_capabilities` (bit mask) and `native_source_sha256`.
`segarecomp build --patch-package <file> --patch-identity <sha256>` (Genesis viewer builds) takes a REFERENCE to the package and the identity the user approved; the build re-reads and re-verifies the package, checks the identity and the image digest,
extracts `provider.c` into `<output>/native/`, re-hashes it, compiles it against the SDK headers only and links it. Nothing from a package is ever passed as a command-line argument or compiler option.
