# Mod projects and `.segapatch` schema 3 (developer preview)

`segarecomp mod` creates, checks and packs a **mod project**: a folder with a `mod.toml` and the source files it names. It never runs a mod, never downloads anything and never accepts a build script or compiler flag. Schema 1/2 patch packages keep working through `segarecomp patch` exactly as before.
The normative contracts are [schema 3](../architecture/segapatch-schema-3.md), [adapters](../architecture/mod-platform-adapters.md), [ABI 2](../architecture/native-sdk-abi-2.md) and [audio](../architecture/modding-audio-architecture.md); the decisions are in ADR 0288. What a platform supports today is in the [support matrix](mod-support-matrix.md).

```text
segarecomp mod init <dir> --platform <genesis|master-system|sega-cd> --id <id> [--template <c11-provider|luau|rom-edit>] [--rom-sha256 <hex>]
                                                                                   create mod.toml and an example Luau file (templates below)
segarecomp mod check <dir>                                                         validate the project exactly as a package would be
segarecomp mod pack <dir> --out <package.segapatch>                                the deterministic package, no compiler
segarecomp mod build <dir> --out <package.segapatch> [--cc <compiler> --sdk-dir <platforms-dir>]   the same package, after compiling the native units
segarecomp mod install <package.segapatch> --rom <original> --library <dir> [--approve <package-identity>]
segarecomp mod variant --library <dir> --identity <package-identity> --rom <original> [--cc <compiler> --sdk-dir <platforms-dir>]
segarecomp mod inspect [--json] [<dir|package.segapatch>]                          without an argument: every platform adapter and its capability support
segarecomp mod convert-audio <file.wav> --out <file.u8>                            the deterministic reference PCM conversion (u8-mono)
```

Every command prints one JSON object. A refusal is `{"ok":false,"class":"E_...","error":"..."}`; the **class** is stable, the text is not.

## Workflow: init, check, pack, install, approve, variant

```text
segarecomp mod init demo --platform genesis --id demo --template c11-provider --rom-sha256 <sha256 of your original image>
segarecomp mod check demo
segarecomp mod build demo --out demo.segapatch --cc cc --sdk-dir <platforms-dir>     (or `pack`, which does not compile)
segarecomp mod install demo.segapatch --rom <original image> --library <dir> --approve <package_identity>
segarecomp mod variant --library <dir> --identity <package_identity> --rom <original image> --cc cc --sdk-dir <platforms-dir>
```

* **Templates** (`init --template`): `c11-provider` (Genesis; a minimal ABI 2 provider that touches no guest state), `luau` (the default example script, any platform) and `rom-edit` (a placeholder: ROM edits are reserved, so it only keeps the revision declaration and a note). Without `--rom-sha256` the revision digest is zeros and must be replaced.
* **`pack` and `build`** produce byte-identical packages from the same project, independent of working directory or location; `build` additionally compiles the native units with the host's strict C11 flags.
* **`install`** verifies the package against the exact original image (one of the declared revisions) and copies the verified bytes to `<library>/mods/<identity>.segapatch`. `--approve` records the approval of that exact identity; it is required before a package with native code can be turned into a variant and never transfers to another identity (`E_APPROVAL_MISMATCH`, `E_NOT_APPROVED`).
* **`variant`** computes the single-mod variant key from the original image digest, the effective image digest (the `bps-image` target, else the original), the package identity, the digest of the native and Luau sources, the ABI and the capabilities, and prints those components. Equal inputs report `"cache":"hit"`; otherwise `"cache":"miss"` with `reasons` naming the components that changed against the previous build of that mod on that image (`no_previous_build` for the first). A build that fails (for example a broken C11 unit, `E_COMPILE_FAILED`) publishes nothing: earlier variants, the original image, Vanilla builds and saves are never written. Only `<library>/cache/variants` is ever created or changed.

This preview validates, compiles (compile-only) and caches a variant *record*; it builds no executable and does not use the launcher's game build cache; it does not link an ABI 2 provider into a playable Patched program (the runtime is a later milestone), and Schema 1/2 patches in the launcher keep their own identity and saves unchanged.

## `mod.toml`

A closed TOML subset: tables, arrays of tables, strings, non-negative integers and single-line string arrays. Unknown tables or keys are errors; there is no key for a command, URL, flag or absolute path.

| Table | Keys |
| --- | --- |
| `[mod]` | `id`, `version` (`MAJOR.MINOR.PATCH`), `name`, `author` (required with native sources), `license` (required with audio assets), `purpose` |
| `[target]` | `platform` (mandatory, never guessed), `kind` (optional, must match the platform), `disc_identity` (Sega CD; reserved) |
| `[[target.revision]]` | `label`, `sha256` (the exact original image), `mapper` (Master System: `sega` or `rom_only`, declared never inferred); repeat per supported revision (1 to 16) |
| `[capabilities]` | `required`, `optional` (names from `mod inspect --json`) |
| `[abi]` | `native = 2` (required with native sources) |
| `[state]`, `[limits]`, `[presentation]` | `schema`; `luau_memory_bytes`, `luau_budget_units`; `widescreen`, `side_style` |
| `[geometry]` | `scenes = []` only (reserved; any content is refused) |
| `[[source]]` | `kind` (`native-c11`, `luau-source`, `asset`, `bps-image`, `rom-edit-set`, `guest-code`), `path` (project-relative: lower-case, at most 3 segments, no `..`), plus kind fields (`target_sha256`, `cpu`, `asset_class`, audio fields, ...) |
| `[[audio.mapping]]`, `[[audio.mapping.value]]` | declared audio mappings: [audio architecture](../architecture/modding-audio-architecture.md) sections 5 and 9 |

The manifest is derived only from these fields and the bytes of the named files, so two loads of the same project on different machines, folders or working directories produce byte-identical packages.

## Native sources

`native-c11` sources (`.c` and `.h`, at most 64 files, 256 KiB each, 2 MiB total) may include only `<segarecomp_mod.h>`, the platform's own SDK header, `<stdint.h>`, `<stddef.h>` and package files by their package path. Anything else is refused. `mod build` compiles every `.c` unit in path order with
strict C11 flags the **host** owns (`-std=c11 -Wall -Wextra -pedantic -Werror`) and the SDK headers under `platforms/` (`--sdk-dir`). `mod build` also links all units into a throwaway probe to prove they resolve and export `segarecomp_mod_provider`. This preview checks and packages ABI 2 providers; running them is not part of this release. Luau sources and assets are stored as opaque, digest-listed payloads.

## Support today

Genesis is the supported adapter. Master System and Sega CD register `host.log` only; a package that needs any other capability, or a Sega CD disc source, is refused (`E_CAPABILITY_UNSUPPORTED`, `E_SOURCE_KIND_RESERVED`). Commercial ROMs, discs, BIOS images and game audio never belong in a project that is committed or shared.
