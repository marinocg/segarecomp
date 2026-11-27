# Widescreen and game patches: what exists, what is generic and what is game-specific

This page is the entry point. Each section links the detailed document. Status of individual games and packages is in the
[compatibility ledger](compatibility-ledger.json) ([how to read it](compatibility-ledger.md)).

> **Availability.** Everything on this page was merged **after** the latest published release (v0.7.0). It is on `main` and will ship with the next release; it is not in the v0.7.0 archives.
> See [release readiness](release-readiness.md).

## 1. Generic widescreen (built in, no patch needed)

Widescreen is a **presentation** feature of the viewer. It never changes the game, its timing, its audio, a recording or the native picture; the console is still emulated at its own resolution.
The settings are in the pause menu (Quick Settings and Video & Display) and in the launcher's Settings > Appearance & Controls, and are saved in `viewer-settings.cfg`.

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

**Up to 32:9, and what that does and does not mean.** `video.widescreen_ratio=N:M` (4:3 through 32:9; the Video page and the launcher offer it as **Widescreen Aspect**) makes the Genesis producer and host show exactly that aspect; the legacy slider (4:3 to 16:9) is unchanged. 32:9 gives a wider *generic* extension (the VDP's own plane scenery and the sprites the game already submitted), which at that width is mostly wrapped or stale scenery and never new gameplay. A game patch that declares a supported aspect range (`geometry` in its manifest) is shown only the native picture, with a notice, outside that range; the viewer reports what each scene actually delivered. The measured table and the limits are in [the aspect matrix](../architecture/widescreen-aspect-matrix.md).

**Asymmetric view (ADR 0315).** The planned extra width at the chosen aspect is one total; a game package may decide how much of it lies left and how much right of the picture, frame by frame (`left_px`), for example to show only the world ahead at the start of a level. Without a package placement, and for every other game, the picture stays centred exactly as before. The viewer shows an asymmetric frame whole, at its own width; if the aspect is narrowed while it is on screen it shows the native picture for that frame rather than a cropped, misleading view. Side presentation (Comfort, Borders, Raw), the HUD and positioned overlays follow the picture.

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

### The public provider SDK (ABI 1, deprecated)

New providers use ABI 2; see the [migration guide](native-abi-migration.md). ABI 1 packages keep working.

One header, [`platforms/genesis/sdk/segarecomp_genesis_patch.h`](../../platforms/genesis/sdk/segarecomp_genesis_patch.h) (`<stdint.h>`/`<stddef.h>` only), is installed with every release package. A provider exports
`segarecomp_patch_provider()` and may serve:

* **`scenery.words`**: name-table words for the world cells under an off-screen column (the host still decodes patterns, palettes, flips, priority and sprites), so scenery comes from the **game's own level model** instead of wrapped VRAM;
* **`overlay.rgba`**: a bounded RGBA overlay, used to **redraw the game's original HUD** at the edge of the widened picture; it can also supply up to four positioned layers anchored to the picture or to the whole widened surface (ADR 0303);
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

The product contains **no** game-specific widescreen code. The packages below are authored and maintained in the separate [segarecomp-patches](https://github.com/marinocg/segarecomp-patches) repository (the destination for their source and releases) and are **not shipped with segarecomp releases**; their status is recorded in the ledger
(`patch_packages`). The extended scenery and widescreen of these packages were validated in all levels according to the maintainer; that statement is not backed by a published report or recording. Each is bound to one exact image.

| Game | Package | Provides | Gameplay | Validation (maintainer-stated) | Not covered |
| --- | --- | --- | --- | --- | --- |
| Sonic the Hedgehog (REV00) | `sonic1-native-widescreen` (schema 2) | authored scenery, relocated HUD, extended actor initialisation (the sprite builder's visibility test is widened by 64 px per side so objects do not pop in at the picture edge) | changes slightly: objects that wait for the on-screen flag act up to 64 px earlier, and a scripted natural-play route through Green Hill 1 did not reproduce identical play | all levels; the special-stage background is the wrapped VDP plane | other revisions |
| Sonic the Hedgehog 2 (REV00 and REV01) | `sonic2-native-widescreen` (schema 2) + presentation-only variant | authored scenery, relocated HUD, actors across the widened picture | standard package changes gameplay slightly; presentation-only does not | all levels | other revisions |
| Sonic 3 & Knuckles (4 MiB lock-on composite) | `sonic3k-native-widescreen` (schema 2) + presentation-only variant | authored foreground, background on some acts (the wrapped VDP plane elsewhere), relocated HUD, actors in the margins | standard package changes gameplay slightly | all levels | standalone Sonic 3 and Sonic & Knuckles images |
| Streets of Rage 2 (USA) | `sor2-widescreen` (schema 1, data only) | additional sprite pieces and objects beside the picture, rendered by generic Level 3 | none intended | all levels (extra sprites only) | other revisions; provides no authored scenery or HUD |

Sonic 3 & Knuckles needs the lock-on cartridge model. The **launcher recognizes the exact supported images from their header contents and selects it automatically** (Advanced build options can override it);
command-line builds must pass `--mapper lock_on`. Modified, resized or unidentified images stay ordinary cartridges.

### What 21:9 and 32:9 mean for these packages (F15 classification)

Four different statements must not be confused; the table in the [aspect matrix](../architecture/widescreen-aspect-matrix.md#5-real-game-classification-seg-140-t006--t007-aggregate-only) and [`widescreen-32x9-classification.json`](widescreen-32x9-classification.json) say which of them was actually shown, per scene class:

1. **Generic 32:9 rendering capability** exists and is tested (planner, producer, host; synthetic and real-game cost aggregates). It shows the VDP's own scenery and the sprites the game already submitted.
2. **Authored scenery at 32:9.** The existing Sonic 1 and Sonic 2 packages are validated for 16:9. Their providers still accept the same frames at 21:9 and 32:9 (observed), but no evidence shows the reconstructed columns are correct that far out: classified `authored-unverified`, not supported.
3. **Actual 32:9 gameplay visibility.** Not established for any title. In every studied running class (Sonic 1 act 1 of six zones, Sonic 2 Emerald Hill 1, Sonic 3 & Knuckles Angel Island 1), objects exist beyond the 64 px the package makes visible and are not submitted; Sonic 2 REV01 was **not run** (image not available).
4. **Gameplay-changing modifications.** The packages' 64 px actor visibility is itself a gameplay-changing modification (objects act earlier). Wider actor visibility would need a wider, equally gameplay-changing change in `segarecomp-patches`; this repository does not make it and does not claim it. A presentation-only statement requires submitted actors, resident art and a verified scenery source together, and no 21:9 or 32:9 class has them.

Virtual sprites (ADR 0303, 0306, 0307) do not change this. The viewer now presents them: a mod can author them through Luau (`sega.sprite.*`), an ABI 1 provider or a native ABI 2 provider, and with `presentation` space they can draw in the side columns the viewer presents (Raw side style; Comfort of generic scenery and Borders replace the columns, so nothing there counts). That is a **generic presentation capability proved with synthetic images**: no shipped Sonic package submits a virtual sprite, so for them a declared `actors` component still reports `missing`, and nothing here establishes that a game object exists or that Sonic is playable at 21:9 or 32:9. `actors` coverage is credited only for sprites that really reached the presented frame, by region (`pixels_picture`, `pixels_left`, `pixels_right`); layered overlays are not counted as HUD delivery; every refusal has a stable reason (ADR 0304).

### Installing a verified patch

See [From install to a patched game](../../README.md#install-a-verified-mod-or-patch-main-only-genesis) in the README for the short walk-through.

## 4. Writing your own provider

1. Read [native SDK ABI 2](../architecture/native-sdk-abi-2.md) and [mod projects](mod-projects.md); copy the [`sdk-sample/abi2`](sdk-sample/abi2/src/provider.c) project and set your image's SHA-256.
2. Compile-check and package it: `segarecomp mod build <project> --out <package> --cc cc --sdk-dir platforms`.
3. Distribute the single file.

Nothing in the recompiler is modified for a new game. An existing ABI 1 provider keeps working; to move it, follow the [migration guide](native-abi-migration.md).
