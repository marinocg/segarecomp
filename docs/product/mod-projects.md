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
segarecomp mod api [--declarations]                                                the Luau API list (JSON) or its generated `.d.luau` declarations
segarecomp mod diff-run <dir|package> --rom <original> --cc <compiler> --sdk-dir <platforms-dir> --frames <n> --budget <instructions> --timeout <seconds> [--input <pad-script>] [--ram <offset:length>]... [--code <offset:length|@symbol[+0xN]:length>]... [--first <frame>] [--aggregate]
                                                                                   Vanilla versus Modded, bounded: the first divergence, the changed guest bytes (gameplay example page)
segarecomp mod test <dir> --rom <original> --cc <compiler> --sdk-dir <platforms-dir> [--scenario <name>]   the project's tests.toml scenarios, each bounded by its own budget and timeout
segarecomp mod session <dir|package.segapatch> --rom <image> [--frames <n>] [--bmp <file>]   DIAGNOSTIC: run a Luau-only package in the synthetic fixture session (not the Genesis machine)
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

* **Templates** (`init --template`): `c11-provider` (Genesis; a minimal ABI 2 provider that touches no guest state), `luau` (a restricted-API starter: on Genesis the HUD example script plus `types/segarecomp.d.luau`, `.luaurc` and `.vscode/settings.json` for editors, on other platforms a log-only script; see [Luau scripting](luau-scripting.md)) and `rom-edit` (a commented `edits/main.edits` declared as a `rom-edit-set` source). Without `--rom-sha256` the revision digest is zeros and must be replaced.
* **`pack` and `build`** produce byte-identical packages from the same project, independent of working directory or location; `build` additionally compiles the native units with the host's strict C11 flags.
* **`install`** verifies the package against the exact original image (one of the declared revisions) and copies the verified bytes to `<library>/mods/<identity>.segapatch`. It prints `approval_kind` (`none` for data-only, `consent` for Luau-only, `native` for native and mixed packages) and the install sheet text of the package's trust class. `--approve` records the approval of that exact identity; it is required before a package with native code can be turned into a variant and never transfers to another identity (`E_APPROVAL_MISMATCH`, `E_NOT_APPROVED`).
* **`variant`** computes the single-mod variant key from the original image digest, the effective image digest (the `bps-image` target, else the original), the package identity, the digest of the native and Luau sources, the ABI, the capabilities and (for a package with Luau) the digest of the trusted compiler's bytecode, and prints those components. The compiled Luau is stored with the variant and verified against that digest before every reuse; a modified, truncated or missing artifact is rebuilt from source, never loaded. Equal inputs report `"cache":"hit"`; otherwise `"cache":"miss"` with `reasons` naming the components that changed against the previous build of that mod on that image (`no_previous_build` for the first). A build that fails (for example a broken C11 unit, `E_COMPILE_FAILED`) publishes nothing: earlier variants, the original image, Vanilla builds and saves are never written. Only `<library>/cache/variants` is ever created or changed.

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
| `[build]` | `header_repair` = `genesis-checksum` \| `none` (ADR 0293; absent = repair only a verifying original that the build changed) |
| `[geometry]` | `scenes = []` only (reserved; any content is refused) |
| `[[source]]` | `kind` (`native-c11`, `luau-source`, `asset`, `bps-image`, `rom-edit-set`, `guest-code`, `guest-asm`), `path` (project-relative: lower-case, at most 3 segments, no `..`), plus kind fields (`target_sha256`, `cpu`, `asset_class`, audio fields, ...) |
| `[[memory]]` | `revision` (a declared label; absent = every revision), `space` (`m68k.work_ram`), `offset`, `length` (decimal), `access` (`read`, `write` or `read-write`, backed by the matching `memory.*` capability). The ONLY guest memory a scheduled mod may touch; part of the package identity ([gameplay example](gameplay-example.md), ADR 0300) |
| `[[symbol]]` | `name` (token), `revision` (a declared label), `address` (string `"0x..."`, below `0x01000000`), `provenance` (`manual`, `verified`, `imported-hint`); one per (name, revision). Only `verified` entries may be an edit site |
| `[[audio.mapping]]`, `[[audio.mapping.value]]` | declared audio mappings: [audio architecture](../architecture/modding-audio-architecture.md) sections 5 and 9 |

The manifest is derived only from these fields and the bytes of the named files, so two loads of the same project on different machines, folders or working directories produce byte-identical packages.

## ROM edits and symbols

A `rom-edit-set` source is a text file, one edit per line (`#` comments and blank lines allowed):

```text
# <revision|*> <type> <site> <expected> <value>      (comments only on their own lines)
# 16-bit in the platform's byte order (Genesis: big-endian, Master System: little-endian)
rev-a u16 0x00000200 0x1234 0x5678
# explicit little-endian; the site is the verified symbol "title" plus 4
rev-a u32le @title+0x4 0x00000001 0x00000002
rev-b bytes 0x00000300 aabbcc ddeeff
# every declared revision, asserted against each
* u8 0x00000010 0x00 0x01
```

`expected` is checked against the exact original image before `value` replaces it; a mismatch, an overlap within one revision, an unverified symbol or an edit beyond the image refuses the build and writes nothing. Revisions resolve independently. Integers are `0x` plus exactly `2 x width` lower-case hex digits. Details and stable classes: ADR [0290](../decisions/0290-typed-rom-edits-and-revision-symbols.md).

```sh
segarecomp mod check <dir>                                            # syntax, count, symbols, overlap for every revision
segarecomp mod apply <dir|package.segapatch> --rom <original-image> --out <edited-image>   # one revision, selected by the image digest
segarecomp mod import-hints <dir> --revision <label> --from <symbols.csv>                 # imported-hint entries only
```

Symbols (`[[symbol]]`) name a location per revision: `@name` or `@name+0xN` in a site resolves to that revision's `verified` address. `manual` and `imported-hint` entries are recorded in the package and printed by `mod inspect` (`symbols` with `provenance`) but are refused as edit sites. Public symbol maps hold names only (lower-case tokens), never disassembly.

## Guest code injection, header repair and BPS (ADR 0292, ADR 0293)

A `guest-asm` source (`cpu = "m68k"` on Genesis, `"z80"` on Genesis or Master System) is assembled by a small in-tree assembler, lower-case, no includes or macros. Routines go into declared free ranges whose original bytes must equal the stated fill:

```
free * 0x00001000 0x00000100 0xff     ; revision (or *), start, length, expected original fill
org * 0x00001000                      ; a block at that image offset (Genesis Z80: `org * 0x1000 at 0x0100`)
entry:
  moveq #9,d1
  move.w d1,0xff0010                  ; absolute stores must be in work RAM
  jsr @helper                         ; a verified symbol of the revision
  rts
```

The emitted bytes are validated before an image exists: privileged/system instructions, `trap`, port I/O, `halt`, interrupt control, non-label branch targets, out-of-range branches and absolute stores outside RAM are refused with `E_GUEST_*` classes; overlapping blocks, free ranges and edits are `E_GUEST_COLLISION` (adjacent is fine); bank-switched mappers are `E_MAPPER_UNSUPPORTED`. For Genesis the header checksum is repaired per the `[build]` declaration.

```sh
segarecomp mod apply <dir|package> --rom <original> --out <target>        # edits + injected code + header repair for that revision
segarecomp mod bps <dir|package> --rom <original> --out <patch.bps>       # deterministic BPS, proved by applying it back
segarecomp mod verify-bps <dir|package> --rom <original> --bps <patch.bps>  # refuses a corrupted or stale BPS
segarecomp mod pack <dir> --out <package> --rom <image-a> --rom <image-b> # round-trip verifies every named image first
```

## Native sources

`native-c11` sources (`.c` and `.h`, at most 64 files, 256 KiB each, 2 MiB total) may include only `<segarecomp_mod.h>`, the platform's own SDK header, `<stdint.h>`, `<stddef.h>` and package files by their package path. Anything else is refused. `mod build` compiles every `.c` unit in path order with
strict C11 flags the **host** owns (`-std=c11 -Wall -Wextra -pedantic -Werror`) and the SDK headers under `platforms/` (`--sdk-dir`). `mod build` also links all units into a throwaway probe to prove they resolve and export `segarecomp_mod_provider`. This preview checks and packages ABI 2 providers; running them is not part of this release. Luau sources are packaged as source only; `mod check`, `pack` and `build` first check them against the Luau API list (`E_LUAU_API`, `E_LUAU_CAPABILITY`, `E_LUAU_TYPE`, each as `<path>:<line>: <message>`), then `mod build` and `mod variant` compile them with the pinned trusted compiler (Luau 0.746) and fail with `E_LUAU_SYNTAX` on a syntax error, and the build prints the compiler and the bytecode digest (`"luau":{...}`). The bytecode is never packaged, and a package that carries bytecode is rejected (`E_LUAU_BYTECODE`). A Luau-only package runs inside a played Genesis program: `segarecomp build --luau-package` links it (and the launcher's executable cache builds it the same way; [Luau scripting](luau-scripting.md), ADR 0296). `mod session` is a separate diagnostic: a synthetic fixture session, not the Genesis machine (ADR 0295). Note that `mod variant` writes only the F03 metadata record, which is never the executable. Assets are stored as opaque, digest-listed payloads.

## Support today

Genesis is the supported adapter. Master System and Sega CD register `host.log` only; a package that needs any other capability, or a Sega CD disc source, is refused (`E_CAPABILITY_UNSUPPORTED`, `E_SOURCE_KIND_RESERVED`). Commercial ROMs, discs, BIOS images and game audio never belong in a project that is committed or shared.
