# Changelog

Releases are published on the [Releases](../../releases) page. This file records what changed between them. Per-game and per-feature status is in the
[compatibility ledger](docs/product/compatibility-ledger.md); the operator checklist for cutting the next release is [docs/product/release-readiness.md](docs/product/release-readiness.md).

## Unreleased (on `main` since v0.7.0, DRAFT release notes)

> Draft for the next release. The version number and the publication are the operator's decision; nothing here has been tagged or published.

### Widescreen (new)

* A shared, presentation-only widescreen setting for Genesis, Master System and Sega CD: **Original**, **Extended Backgrounds (Level 2)** and **Enhanced Widescreen (Level 3)**, with a **Widescreen Width** slider from 4:3 to 16:9,
  **Comfort / Borders / Raw Extension** side treatments and per-game saved settings, all changeable live from the pause menu and the launcher settings.
* Honest capability reporting: Level 2/3 are often **Partial**. Master System's extra columns repeat scenery already on screen; Level 3 draws only sprites the game already submits and does not make a game create objects outside its original window.
* A program built with an earlier release keeps playing in Original and needs a rebuild to gain widescreen.

### Game patches and the native patch SDK (new, Genesis)

* `.segapatch` packages: **schema 1** (verified data-only BPS) and **schema 2** (a **trusted native C11 provider**, optionally with BPS), installed from a game's **Patches / Variants** card as a separate **Patched** variant beside **Vanilla**.
* A small public **provider SDK** (`segarecomp_genesis_patch.h`, ABI 1) lets a package reconstruct scenery from the game's own data and redraw its original HUD. The SDK header, its documents and a synthetic sample ship in the release package under `sdk/`.
* Packages are bound to the exact game image and the exact approved package identity. Native patches are **executable code, not sandboxed**; hashes prove integrity, not authorship or safety.
* Game-specific packages (Sonic 1, Sonic 2, Sonic 3 & Knuckles, Streets of Rage 2) are maintained outside this repository, are **not included in this release**, and have the limited validated scope listed in the ledger.

### Genesis

* Automatic recognition of the Sonic & Knuckles lock-on cartridge images (exact header contents only; manual override in Advanced build options; `--mapper lock_on` on the command line).
* Opt-in `--z80-static 1` for `segarecomp build`: reconstructs provable Genesis Z80 images before the first Z80 compilation (ADR 0277). Off by default, not exposed in the launcher, not a guarantee for any title.
* Optimized AOT evaluation for cartridges that run code from RAM thunks, and static RAM `JMP` thunks (ADR 0270, 0272). The changed interrupt-delivery behavior of the latter has no independent Genesis-level reference.

### Performance and diagnostics

* **F3** performance overlay (Off / Player / Developer), profiling reports and benchmark tools (ADR 0273).
* Throughput work (ADR 0275, 0276): measured on one development host, the software-rendered viewer ran about 20% faster unthrottled and builds were about 10-14% faster. Guest behavior and frames are unchanged. These are not guarantees on other machines.

### Documentation

* The README, platform pages and Sega CD notes now describe what is actually implemented (generated-native Genesis Z80, Master System, Sega CD audio and saves, key remapping) and how strong the evidence is.
  Human-reported completions of Sonic CD and Earthworm Jim Special Edition are recorded as **maintainer-reported**, not as independently validated runs.

### Known limitations

* Compatibility is not universal; unsupported behavior stops with a typed diagnostic.
* Sega CD: complete CUE-to-play compilation is not claimed; FM/PSG output is not reference-checked.
* Genesis is a three-button pad; PAL runs at 60 Hz.
* The full patch-to-executable build test runs on Linux CI only; Windows and macOS validate compilation of the SDK sample, packaging and the launcher.
* Packages are not signed or notarized. macOS needs right-click > Open on first launch; Linux needs glibc 2.35 or newer.

## v0.7.0 (2026-11-01)

Sega CD becomes an interactive platform in the launcher: authentic BIOS execution, Sonic CD playable, shared viewer with input recording and replay, self-service and File-first builds, CD-DA, RF5C164 PCM,
main-side Z80 with FM/PSG, persistent internal backup RAM and unified launcher integration. See the [release page](../../releases/tag/v0.7.0).
