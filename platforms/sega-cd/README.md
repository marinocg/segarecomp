# Sega CD

**Status:** synthetic discs and the authentic-BIOS profile are implemented; a generated-native Sega CD program can be played interactively in the
shared SDL3 viewer and from the launcher as an imported, verified prebuilt program. Contract:
[docs/architecture/sega-cd-machine-contract.md](../../docs/architecture/sega-cd-machine-contract.md). Decisions: ADR 0126-0133 (two processors,
overlays, scheduling, disc resources, BIOS profiles), 0142-0149 (BIOS-image profile, hand-off, input), 0176/0177 (Word RAM executable authority,
timing conventions and the Sonic CD checkpoint), 0179/0180 (epoch recipe, batched templates) and **0204 (the interactive viewer, live input
recording and the launcher's Sega CD game)**.

**Purpose:** Genesis plus a second MC68000 (the sub CPU), the Gate Array, Word RAM and the CD drive/CDC data path.

## What is supported

| | |
| --- | --- |
| **Synthetic profile** | Project-authored post-BIOS fixture discs only; no BIOS and no commercial software. Where the reference emulators disagree the runtime follows Genesis Plus GX (consulted, never copied; contract tag G). |
| **Authentic BIOS execution** | Your own BIOS image (verified by SHA-256) runs generated-native on the main CPU; the sub CPU, Gate Array, CDD/CDC, Word RAM and VDP follow the contract. Programs are compiled ahead of time; there is no interpreter, JIT or runtime opcode decoder. |
| **Sonic CD (USA)** | Checked from the BIOS through the title, the first START, the second START, the first level and a timed RIGHT input, against an independent reference at pre-registered checkpoints (ADR 0176, 0177). This is checkpoint evidence for one title, not a compatibility claim. |
| **A second title** | Experimental observations only (parts of its start-up run); no checkpoint and no claim. |
| **Interactive viewer** | `sega_cd_bios_build.py --viewer` links the same generated program with the shared viewer: every frame is presented at its vertical-blank onset, paced by the machine's own frame length (59.92 Hz), keyboard or gamepad on port 1 (the Genesis viewer's mapping and settings), Esc/Back pause menu, F6 hold and F7 single-step. |
| **Widescreen (Level 2)** | The viewer's Widescreen Mode *Extended Backgrounds* (shared `video.widescreen` setting, SEG-119, ADR 0262) shows real Plane A/B scenery left and right of the unchanged native picture in a 16:9 window. It is presentation only: the Sega CD frame path produces it with the Genesis producer right after the native frame is composed, from the VRAM and per-line state of that same frame, and stamps it with the frame number; the machine, scheduler, audio, input, Word RAM, DMA and the native frame digest are untouched (Original does no extension work at all). Background tiles that reach VRAM by stamp rendering into Word RAM and DMA are ordinary VRAM contents to it. **Always reported as Partial** (never Available): the Sega CD presentation path keeps no VRAM write journal, so the scenery is sampled from the frame's final VRAM and a mid-frame tile change is not replayed (per-line palette, scroll and registers are); a window plane over a picture edge or differing per-column vertical scroll are further typed caveats. *Enhanced Widescreen (Level 3, SEG-121, ADR 0264)* uses the same Genesis producer to also draw the sprites the frame's sprite table places over the extra columns (same hardware limits as the native line), stays Partial (same missing journal), and shows as Level 2 when the sprite layer cannot be produced. A frame that cannot be extended (no/invalid target, undersized surface, producer failure, stale or inconsistent result) is the Original picture for that frame and the next valid frame recovers. A program built before this support keeps playing in Original mode (it reports Level 2 Unavailable) and needs rebuilding to gain it. |
| **Input recording** | The player's port-1 input is recorded as an `segarecomp-input-script 1` file whose headless replay reproduces the run exactly (report, input counters and captured frames). |
| **Launcher** | Sega CD is a platform of the library: your BIOS and CUE are referenced (never copied); **Build** compiles a viewer program from them on this computer (SEG-078-T003, ADR 0205), or an imported viewer program is verified against them; Play, recording and replay are in the game's details page. |
| **Disc-extent discovery** | The build pass records every disc block the CDC delivered. At a stop in RAM code, `segarecomp sega-cd resolve-source` finds the track-1 span the live RAM window is a byte-exact copy of among the *delivered* blocks, and admits it as an ordered `extent:` admission (the same executable-image authority as `--disc-image`, without a human-supplied extent). Ambiguous, undelivered, generated or modified code is refused with a typed class (`ambiguous_source`, `no_source`, ...) and never guessed. No reference emulator, trace or title table is consulted. |
| **Not supported** | **Other audio** (the main-side Z80 sound driver executes generated-native and the shared YM2612 and PSG feed the mix as the Genesis devices do - SEG-115, ADR 0259 - but that output is neither reference-checked nor verified by listening, and its level against CD-DA/PCM is not established; CD-DA music from 2,352-byte audio tracks (SEG-104) and RF5C164 PCM sound effects (SEG-107) are mixed on one 44.1 kHz guest-time timeline and play in the viewer). **Saving** (the launcher passes a new console's formatted, empty backup RAM; nothing is written back). **Complete CUE-to-play compilation**: `segarecomp sega-cd build` and the launcher's Build button discover the disc extents a title loads without a reference (ADR 0205), but a title whose code is generated, transformed or reached only through computed destinations the build never observed is not fully covered; code a program's build did not admit stops with a typed stop (`entry_not_compiled`, ...). |

## Producing and importing a verified program (developer procedure)

A Sega CD program is generated from **your** BIOS and disc and is itself derived from commercial content: keep it, its work directory and the
inputs in ignored local storage; never commit, publish or bundle them.

1. Build the tools: `cmake --preset dev` (add `-DSEGARECOMP_ENABLE_SDL3_VIEWER=ON` and an SDL3 installation for the viewer) and build `segarecomp`.
2. Run the multi-pass build driver on your files, with the executable regions and admissions your title needs (the procedure of
   [the epoch recipe](../../docs/architecture/sega-cd-epoch-recipe.md) and ADR 0176/0180; the regions are title-specific and are not published):

       python3 tools/sega_cd_bios_build.py --segarecomp <segarecomp> --cc <compiler> --cue "<disc.cue>" --bios <bios.bin> --work <ignored dir> \
           [--disc-image ...] [--epoch-recipe ...] --viewer

   The summary JSON names `program` (headless) and `viewer.program` (`program-viewer`, the playable variant); the program identity is the same.
   `--viewer` needs SDL3 (`SDL3_PREFIX`); without it the build fails with `sdl3_unavailable`.
3. Check it: `<program-viewer> --describe` prints what the program claims (runtime contract, BIOS and disc files by size and SHA-256).
4. In the launcher: **Import ROMs > Add Sega CD game...**, choose your BIOS, the disc's CUE (its track files must be beside it) and, optionally, the
   `program-viewer`; or from a terminal: `Segarecomp --sega-cd --bios <bios> --cue <cue> --program <program-viewer>`. The program is accepted only if its
   statement matches the BIOS and every disc file (SHA-256) and it is a viewer build of the current runtime contract; otherwise the launcher says which
   check failed (`program_bios_mismatch`, `disc_file_missing`, `program_headless_only`, `program_abi_unsupported`, ...).

### Self-service build (no developer procedure)

    segarecomp sega-cd build --cue "<disc.cue>" --bios <bios.bin> --output <ignored dir> --runtime-dir <repository root> --cc <compiler> \
        [--viewer 1 --sdl3-include <dir> --sdl3-lib <dir>] [--cache-dir <dir>] [--backup-ram <formatted empty image>] [--budget <instructions>]

This is the same fixed point as `tools/sega_cd_bios_build.py --discover-extents`, in the product CLI: emit, compile (parallel, content-addressed object cache),
run a build pass from reset, admit the epoch image, jump-slot templates and discovered disc extents the pass proves, and confirm with a second identical
pass. It prints `@stage` / `@round` / `@result` progress lines and writes `status.json` (`extents`, the typed `frontier` refusal if any, rounds, result)
and `build.log`. The launcher's **Build** button runs exactly this command with the bundled compiler. A game that was added without a program is shown
as *No program yet (can be built)*.

**Static boot images (SEG-113, ADR 0257).** Before the first emission the build asks `segarecomp sega-cd boot-contract` which executable boot images the BIOS places in RAM (the main
boot area and the sub boot program). It answers only for a *verified profile* - an exact BIOS image and the header form its placement was observed with (`verified_authentic`; project fixture BIOS tags are honoured only with the test-only `--boot-fixture-profiles 1` and are labelled `test_assumed_fixture`) - and then the images are in the first generated program (`static-boot:` admissions, recomputed by the emitter, still byte-guarded at every instruction), so the two rounds that would
only discover them by running disappear. For any other BIOS or header form nothing is offered and the images are discovered by execution as before (`--static-boot 0` forces that).
`status.json` reports `static_boot` (state, `authority`, profile, per-image class `conditional`, `contradictions`, `internal_sources`, `later_overlays`, `unresolved_mismatches`).

The build does not invent controller input: it covers the path the console takes from reset with no button pressed. A title that waits for START at its
first menu is therefore built up to that menu; the program stops with a typed stop (`entry_not_compiled`) when play reaches code the build never ran.

### File-first broad AOT (experimental, opt-in; SEG-100, ADR 0221-0226)

    segarecomp sega-cd build ... --file-first structural      (or `all`; also `emit ... --partition-window 8192 --file-first ...`)
    segarecomp sega-cd file-survey --cue "<disc.cue>" [--structural]   (counts only: what a disc offers a file-first build)

Besides everything above, the build compiles the disc's ISO9660 files (at most 512 KiB each) into base-less, processor-neutral native bodies **before any run**.
When the original game later loads a module wherever its own code puts it, the program identifies the delivered file the live bytes are an exact copy of and
serves the precompiled bodies for it; every instruction still verifies its own bytes, and anything it cannot identify is the same typed stop as before. The
static and execution-derived producers keep precedence and remain the fallback; the Compatibility build is the unchanged reference. `status.json` gains
`file_first` (what was compiled and what was not, typed) and the run report gains a `dynamic` section (bindings, refusals by reason, search cost). The
launcher's build does not expose the option (adoption is not proposed); it can be passed through the documented `SEGARECOMP_SCD_BUILD_ARGS` seam.

## Code

`machine/` (build time: boot images from `libs/media` disc ingestion, two-processor program emission over the generic M68K lowering, digest-checked
module lists, `segarecomp sega-cd inspect|emit`); `runtime/` (strict C11: scheduler, buses, Gate Array, Word RAM, timer, CDD/CDC, disc reader, overlay
selection, driver, and `scd_live.c`, the interactive driver loop); `viewer/` (the SDL3 host of the live driver, opt-in). Fixtures:
`tools/sega_cd_fixture_disc.py`, `tools/sega_cd_bios_live_fixture.py`. Tests: `tests/sega_cd_*`, `apps/segarecomp-launcher/launcher_sega_cd_*`.

**Reuse:** `libs/cpu/m68k` and the generic M68K emitter instantiated twice (one `ExecutableImageSet` and one `M68kRuntimeCEmitter` per processor
instance, both `CpuVariant::mc68000`); the Genesis VDP device and frame export; the Genesis viewer's presenter, pacer and controller mapping.

**Milestones:** SEG-100 (file-first broad AOT and dynamic verified module mapping), SEG-062/063 (synthetic disc, dual M68K, modules/overlays), SEG-064..071 (authentic BIOS, checkpoints), SEG-078 (interactive play), SEG-097 (static discovery of modules no run has loaded, source-relative bodies, the experimental frozen-M3 policy; contract section 19).
