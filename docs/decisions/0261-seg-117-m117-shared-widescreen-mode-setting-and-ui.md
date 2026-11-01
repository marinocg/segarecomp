# ADR 0261: SEG-117 (M117) - the shared Widescreen Mode setting, capability reporting and UI

## Status

Accepted (SEG-117, milestone A of optional 16:9 support). Rendering follows in SEG-118 (shared presentation, Genesis Level 2), SEG-119 (Sega CD), SEG-120 (Master System) and SEG-121 (Level 3).

## Context

Optional widescreen is a presentation enhancement, not a change of the virtual console. Before any wider picture exists, the product needs one place where the user's choice lives, one way
for a platform to say what it can honestly show, and the UI that exposes both, without touching guest execution, frame artifacts, digests or reference checkpoints.

## Decision

1. **One enum, one owner.** `SvWidescreenMode { ORIGINAL (default), EXTENDED_BACKGROUNDS (Level 2), ENHANCED_GAMEPLAY (Level 3) }` is a field of `SvVideoSettings` in `libs/viewer`
   (`sv_settings`). The settings file stores a stable name (`video.widescreen=original|extended_backgrounds|enhanced_gameplay`); a missing key (older files), an unknown, numeric,
   empty or oversized value and an out-of-range in-memory value all become Original, and the other fields are untouched. `sv_video_defaults` restores Original. The mode is not a set of
   booleans, so contradictory combinations cannot exist. There is one video block for every `SvSystem`; Sega CD uses the Genesis adapter and therefore the same value.
2. **Requested, capability, effective.** *Requested* is the saved preference and is preserved even when it cannot be honoured. *Capability* is what the running platform viewer reports per
   level, `SvWidescreenSupport {UNAVAILABLE, PARTIAL, AVAILABLE}`, through one optional callback in `SvHostConfig` (`widescreen_caps`); reporting nothing means Unavailable. *Effective*
   (`sv_widescreen_effective`) is the fallback Level 3 -> Level 2 -> Original over the two (Level 2 never falls up). Capability describes observed support, not a guarantee for a game, and
   is not a registry or per-game database. In this milestone no platform reports support, so both enhanced levels are Unavailable and the effective mode is Original everywhere.
3. **Ownership and timing.** The preference is written only by the UI (pause menu, launcher). The platform adapter will sample it once per produced frame at a frame boundary (SEG-118 ADR contract);
   nothing in guest execution, scheduling, input sampling or recording reads requested or effective mode, so a switch cannot change the guest frame count, clocks or the input recording.
4. **UI only in settings.** The pause menu's Video & Display page (and Quick Settings) gain "Widescreen Mode" with the description of the selected mode as the focus hint, and the Video
   page a status note ("Not yet available for this game. Showing the original picture." while nothing is supported). A change applies immediately, is saved, and rebuilds the page text.
   The launcher's Settings > Video & Controls has the same selector with a short explanation. There is no widescreen toggle, indicator or message on the game picture. A bounded saturating
   counter (`sv_host_widescreen_fallbacks`) records selections that could not be honoured; there is no log growth.
5. **One shared file, no stale writes.** The launcher edits the same `viewer-settings.cfg` through `viewer_settings_` (no `LauncherSettings` field). Because the viewer is a separate process that
   writes the file, the launcher re-reads it at the two seams where an in-game change can have happened, game exit and entering Settings, and keeps its copy if the file is missing, unreadable or holds no recognised setting (a damaged file parses to defaults, so the content is checked, not the open result). No polling.
6. **Prebuilt programs.** Viewer code is statically linked into each generated program. A widescreen-capable build switches modes without rebuilding; an older prebuilt cannot gain extended
   rendering from a setting, stays usable in Original mode (it ignores the unknown key) and needs rebuilding. This applies to Sega CD prebuilts too and does not change their verification.
7. **Rendering is deferred on purpose.** Showing a selectable but non-functional mode as working would mislead, so the UI states the truth and the Original picture is untouched. The
   extended-surface producer/viewer contract is ADR 0262 (SEG-118); Genesis, Sega CD, Master System and Level 3 follow in SEG-118-121 without a universal VDP abstraction. Game Gear, 32X and
   Saturn only need to supply capability and a platform-owned extension later.

## Consequences

The original deterministic artifacts (frame artifact, digests, checkpoints) and execution are unchanged; the new field is a host preference. A viewer older than this ADR ignores the key; a
newer one reading an older file defaults to Original.

## Evidence

`tests/sv_viewer_core_test.cpp` (round trip, stable names, older/invalid/oversized values, defaults, fallback table, status text), `libs/viewer/test/sv_host_test.c` (menu with keyboard and
gamepad, descriptions, Unavailable message, provider fallback, persistence, no overlay, fake-guest frame/input invariance across a mid-run switch), `apps/segarecomp-launcher/launcher_widescreen_gui_test.py`
(selector, relaunch, viewer-written values, older files, defaults, game-return reload), and the Genesis and Sega CD viewer integration tests (a saved Level 3 request leaves the run identical).
