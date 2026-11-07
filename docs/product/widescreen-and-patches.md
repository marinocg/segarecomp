# Widescreen and game patches: what exists, what is generic and what is game-specific

This page is the entry point. Each section links the detailed document. Status of individual games and packages is in the
[compatibility ledger](compatibility-ledger.json) ([how to read it](compatibility-ledger.md)).

> **Availability.** Everything on this page was merged **after** the latest published release (v0.7.0). It is on `main` and will ship with the next release; it is not in the v0.7.0 archives.
> See [release readiness](release-readiness.md).

## 1. Generic widescreen (built in, no patch needed)

Widescreen is a **presentation** feature of the viewer. It never changes the game, its timing, its audio, a recording or the native picture; the console is still emulated at its own resolution.
The settings are in the pause menu (Quick Settings and Video & Display) and in the launcher's Settings > Video & Controls, and are saved in `viewer-settings.cfg`.

| Setting | Values | What it does |
| --- | --- | --- |
| **Widescreen Mode** | Original (default) / Extended Backgrounds (Level 2) / Enhanced Widescreen (Level 3) | Original is the unchanged 4:3 picture. **Level 2** draws the real background (plane) scenery left and right of the picture. **Level 3** additionally draws the sprites that the game's own sprite table already places over the extra columns. |
| **Widescreen Width** | a slider from 4:3 to 16:9 | The target aspect ratio. It never stretches or crops the game picture and keeps its value when Original is selected. |
| **Side Presentation** | Comfort (default) / Borders / Raw Extension | How the extra side area looks. *Comfort* is a soft ambient fill taken from the picture's edges: decorative, it hides distracting extra data and creates no gameplay visibility. *Borders* shows plain neutral panels. *Raw Extension* shows the extension exactly as produced, which is the honest way to judge it. |
| **Per-game settings** | Mode, Side Presentation and Width | Saved per game (platform + the game's SHA-256, in `viewer-widescreen/` next to the settings file). Everything else stays global. |

Every change applies to the next frame without a restart, reset or rebuild. A program built **before** widescreen support keeps playing in Original and has to be rebuilt to gain it.

### What the levels honestly are

* The viewer reports a **capability** for the current game: *Available*, *Partial* or *Not yet available for this game*. Genesis Level 2 is reported per frame (Available unless a caveat applies); Level 3 is always **Partial**; Master System and Sega CD are always **Partial**. A program that does not report support shows the Original picture.
* **Genesis:** Level 2 shows plane scenery from the VDP's own name tables. The VDP keeps only a bounded window of the world (the planes wrap), so beyond what the game has already written the extension is wrapped or stale scenery.
* **Master System:** the Mode 4 background name table is 32 columns wide and wraps, so the extra columns repeat scenery that is already inside the 256-pixel picture. The menu says so: *"Background repeats existing scenery; this does not reveal more of the game world."* Level 3 can only add the few sprite pixels that cross the picture edge (hardware sprite X geometry).
* **Sega CD:** Level 2 and Level 3 use the Genesis producer on the frame that was just composed. They are always Partial because the Sega CD presentation path keeps no VRAM write journal: the scenery is sampled from the frame's final VRAM, so a tile changed mid-frame is not replayed.
* **Level 3 does not make a game create objects.** It draws sprites the game already submitted. Whether a game loads, updates and submits objects outside its original 320-pixel window is the game's own logic, and most games do not. That is the reason the game-specific patches below exist.

Details: [viewer UI](viewer-ui.md#widescreen-side-presentation-seg-123), [launcher settings](launcher-library.md), ADR 0261 to 0265, [Master System](../../platforms/master-system/README.md), [Sega CD](../../platforms/sega-cd/README.md).

## 2. Game patches: the `.segapatch` package

A **patch package** is one portable file that describes an optional modification of one exact game image. Installing it adds a separate **Patched** variant next to **Vanilla**; Vanilla is never modified. Genesis only.

| Schema | Content | Runs code? |
| --- | --- | --- |
| **1** | one verified **BPS** delta: a data-only change to the game image | **No.** Nothing from the package executes. |
| **2** | a **native C11 provider** (`provider.c`), optionally combined with a BPS delta | **Yes.** The source is compiled into the Patched program and runs with its operating-system permissions. |

### Security: native patches are trusted code, not sandboxed

A schema 2 package contains **executable source code**. It is **not sandboxed**. The SHA-256 digests in the manifest prove **integrity** (the bytes are those the manifest names); they do **not** prove who wrote the package or that it is safe.
There are no signatures. Install a native patch only from a source you trust. The launcher shows an explicit approval sheet (author, license, package identity, source digest, declared capabilities, "contains executable source, not sandboxed")
and builds only the exact package identity you approved; a changed package has a new identity and needs a new approval. The command-line `--patch-identity` is a consistency check, not user consent.
Details: [patch packages](patch-packages.md#trust).

### What a package must satisfy

* **Exact image.** The platform and the full SHA-256 of the original image must equal the package's `source_sha256`. A different revision, region or dump is refused. Each supported revision needs its own package.
* **Identity-bound verification.** The package identity is the SHA-256 of its manifest. The launcher approval, the build and the cache key are all bound to it, together with the provider source digest, the ABI version and the capability set.
* **Separate variants.** The Patched program has its own cache entry, build record and provenance. Rebuilding, removing a patch (which selects Vanilla) and clearing a patched build never touch Vanilla, saves, recordings or widescreen preferences.
  A selected Patched variant that is not built is simply not playable; nothing falls back to Vanilla under a Patched label.
* **Defaults.** A package may declare presentation defaults (for example Level 3 + Raw Extension). They apply only while its Patched variant runs and never write a preference file.

### The public provider SDK (ABI 1)

One header, [`platforms/genesis/sdk/segarecomp_genesis_patch.h`](../../platforms/genesis/sdk/segarecomp_genesis_patch.h) (`<stdint.h>`/`<stddef.h>` only), is installed with every release package. A provider exports
`segarecomp_patch_provider()` and may serve:

* **`scenery.words`**: name-table words for the world cells under an off-screen column (the host still decodes patterns, palettes, flips, priority and sprites), so scenery comes from the **game's own level model** instead of wrapped VRAM;
* **`overlay.rgba`**: a bounded RGBA overlay, used to **redraw the game's original HUD** at the edge of the widened picture;
* read-only inputs `frame.work_ram` (a work-RAM snapshot) and `frame.vdp` (per-line VDP state and the frame's VRAM). There is no API to write guest memory or to reach the host.

Every frame is validated by the provider; a frame it declines is shown by the generic path and is never labelled authored. A complete synthetic example is [`sdk-sample/provider.c`](sdk-sample/provider.c).
Details: [native patch SDK](native-patch-sdk.md), [patch packages](patch-packages.md). The sample compiles as strict C11 against the SDK header on every CI platform; the full package-to-executable build test runs on Linux only.

### Generic Level 2/3 versus a game-authored patch

| | Generic Level 2 / 3 | Game-authored patch (schema 2 or 1) |
| --- | --- | --- |
| Scenery beyond the picture | what the VDP already holds; wrapped or stale beyond that | reconstructed from the game's own level data by the provider, where its frames are accepted |
| Original HUD | stays at the original place inside the 4:3 area | can be relocated to the widened picture's edge |
| Objects beyond the original window | only those the game already submits | can be made to exist by a BPS change to the game's own placement or culling code (this **changes gameplay**) |
| Needs | nothing | an exact image, a package, and for schema 2 a trusted source |
| Where the knowledge lives | in the public engine, with no game knowledge | in the package only; the product contains no game-specific provider, table or registry |

## 3. Game-specific packages that exist today

The product contains **no** game-specific widescreen code. The packages below are authored and maintained in the project's **private harness** and are **not distributed with any release**; their status is recorded in the ledger
(`patch_packages`). Each is bound to one exact image.

| Game | Package | Provides | Gameplay | Validated scope | Not validated |
| --- | --- | --- | --- | --- | --- |
| Sonic the Hedgehog (REV00) | `sonic1-native-widescreen` (schema 2) | authored scenery, relocated HUD | HUD only. A separate `-gameplay` package is **experimental**: it widens object activation and diverges from natural play | running levels of the main zones along scripted routes; special-stage background is the wrapped VDP plane | bosses and transitions outside the routes, ending, other revisions |
| Sonic the Hedgehog 2 (REV00 and REV01) | `sonic2-native-widescreen` (schema 2) + presentation-only variant | authored scenery, relocated HUD, actors across the widened picture | standard package changes gameplay slightly; presentation-only does not | single-player levels and demo scenes of the campaign inventory | later Wing Fortress, ending, final boss, special stages, two-player, PAL |
| Sonic 3 & Knuckles (4 MiB lock-on composite) | `sonic3k-native-widescreen` (schema 2) + presentation-only variant | authored foreground, background on some acts (the wrapped VDP plane elsewhere), relocated HUD, actors in the margins | standard package changes gameplay slightly | partial campaign: Sonic-route acts swept | bosses, act transitions, ending, Knuckles and Tails-alone routes |
| Streets of Rage 2 (USA) | `sor2-widescreen` (schema 1, data only) | additional sprite pieces and objects beside the picture, rendered by generic Level 3 | none intended | stage 1 opening screens and the attract demo | later stages, crowded scenes; provides no authored scenery or HUD |

Sonic 3 & Knuckles needs the lock-on cartridge model. The **launcher recognizes the exact supported images from their header contents and selects it automatically** (Advanced build options can override it);
command-line builds must pass `--mapper lock_on`. Modified, resized or unidentified images stay ordinary cartridges.

### Installing a verified patch

See [From install to a patched game](../../README.md#install-a-verified-patch) in the README for the short walk-through.

## 4. Writing your own provider

1. Read [native-patch-sdk.md](native-patch-sdk.md) and the header; copy [`sdk-sample/provider.c`](sdk-sample/provider.c).
2. Compile-check it exactly as the build does: `cc -std=c11 -Wall -Wextra -pedantic -I platforms/genesis/sdk -c provider.c`.
3. Package it with `tools/segapatch.py create ... --native-source provider.c --native-caps ...` and distribute the single file.

Nothing in the recompiler is modified for a new game. The release packages ship the SDK header, the sample and these documents under `sdk/` so a provider can be written from an installed copy.
