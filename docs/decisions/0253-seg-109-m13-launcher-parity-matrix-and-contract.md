# ADR 0253: M13 launcher parity - audit matrix and acceptance contract (SEG-109-T001)

Status: accepted (T001 checkpoint, written before any UI change). Decision of the milestone: `M13_LAUNCHER_PARITY_GO | _PARTIAL | _BLOCKED` (recorded in T006).

## Goal

After import, a Sega CD game is a normal library game. Genesis, Master System and Sega CD share one details structure, one set of game actions,
one build lifecycle, one cover workflow and one storage/cleanup model. Deliberate differences: the **Import ROMs** flow (a Sega CD game is a BIOS
plus a disc, never a dropped file) and genuine hardware/product-specific *fields*. Non-goals: CPU/device/AOT behaviour, Sonic CD timing, a plugin or page
framework, a second build framework, a rewrite of the Sega CD builder (`ScdBuildJob`), new save features (M12 is consumed as is).

## Method

Read of `app_details.cpp`, `app_sega_cd.cpp`, `app_pages.cpp`, `app_screens.cpp`, `app.cpp` (statuses, build/run lifecycle, menus, scripts), `launcher_core.*`
(`BuildJob`, `ScdBuildJob`), `sega_cd.*`, `storage.*`, `storage_remover.hpp`, `covers.cpp`, `cover_download.cpp`, `library_catalog.*`. Every row names where the
behaviour lives. Class: **GAP** real missing feature; **PRES** equivalent function, different presentation (must converge); **HW** legitimate hardware/product-specific
difference (kept, but shown inside the shared structure); **OK** already shared.

## Parity matrix

| # | Area | Genesis / Master System | Sega CD today | Class | Contract |
|---|------|------------------------|---------------|-------|----------|
| 1 | Library cards, rows, badges, platform filter, search | `draw_card`/`draw_list_row`, `badge_for`, filter 0-3, title/platform search | same code; the *Building* badge honours the Sega CD job | OK | unchanged |
| 1 | Sort by size | uses `StorageReport` usage | always 0 (not measured) | GAP | fixed by storage integration (#8) |
| 1 | Region label | Genesis region / SMS | "Disc" (a disc makes no verified console-region claim) | HW | kept |
| 1 | Artwork placeholder | hand-drawn cartridge | "SEGA CD" variant | OK | unchanged |
| 2 | Details layout | identity -> BUILD CONFIGURATION card -> notices/log -> STORAGE rail -> BUILD PROVENANCE | identity -> one different "SEGA CD" panel (state, facts, actions, recordings, build) ; **no** storage rail, **no** provenance (`draw_scd_details`) | GAP/PRES | one structure: Sega CD fields become rows of the same cards; `draw_scd_details` is removed |
| 2 | ROM health pill | "ROM linked / missing" | "BIOS and disc linked / missing" | HW | kept (same pill) |
| 3 | Primary actions | Play + Build/Rebuild/Try again/Recompile + More at fixed places; Locate ROM replaces Play when the ROM is gone | Play same place; Build is a separate branch with its own labels (`Build`/`Rebuild`), own Cancel, tooltips | PRES | one code path: labels from `build_action_label`, `can_build`, `request_build`, Cancel, tooltips |
| 3 | Context menu / More | Details, Play, Build/Recompile, Unqueue, Cancel build, favorite, build log, open build folder, Manage storage, cover items, copy SHA, Locate ROM, Remove | Build, Cancel, Unqueue, Manage storage and Open build folder are **missing**; "run log" instead of build log; "Open recordings folder" | GAP | same items; Locate BIOS/disc where the ROM variant exists; recordings folder as an additional Sega-CD item |
| 4 | Play / stop / replay | Play starts the game (viewer or generated program) ; stop = close its window | same | OK | unchanged |
| 4 | Recording / replay | none | "Record this session", replay list (SEG-078) | HW | kept as an extra, platform-specific section (INPUT RECORDING), reusing the section-card presentation |
| 5 | Build / rebuild / cancel / progress | `BuildJob`, FIFO queue (one build at a time), 4 stage chips, Cancel/Unqueue, live log, sidebar "Building 1 game / n queued", quit waits | `ScdBuildJob` in a second slot (`scd_build_`): not queued, can run beside a Genesis build, text+checklist progress inside the SEGA CD panel, own Cancel, no live log, ignored by the sidebar activity line and the busy guard | GAP/PRES | the same flow drives both jobs: FIFO, stage chips (6 stages), Cancel/Unqueue, live log, sidebar, busy guard; the Sega CD job is adapted, not rewritten |
| 5 | Outcomes | cancelled keeps the prior program; failure keeps previous playable program; failed log in `.failed/` | same safety (publish by rename, program replaced only after verification) | OK | preserved (File-first status, safe replacement unchanged) |
| 6 | Build configuration | Build mode tiles (2), region/mapper, Advanced expander, state-vs-selection sentence | 3 tiles (Compatibility / File-first / Optimized exp.) , "selected vs built" sentences, separate "BUILDING" text | PRES | same card; the tile row has 3 options for Sega CD; the sentence uses the same slot |
| 6 | Provenance | rows: ROM SHA/file, last recompiled, last played, version, identity, executable, last run, diagnostics | none | GAP | same rows for Sega CD: BIOS, Disc, Program (+kind, verified), last built/played, build mode and inventory, identity, **Save data** |
| 6 | Program import / verify | n/a | Import/Replace program, Verify again, Locate BIOS/disc | HW | kept inside BUILD CONFIGURATION (Advanced) / provenance row actions |
| 7 | Errors, status, diagnostics, logs | failure banner + Dismiss, `diagnostics_` log, View build log | failure banner (shared), logs show `run.log` | PRES | build log (live while building, else last build.log / failed log); run log remains reachable as "View run log" |
| 8 | Storage metrics | per-entry usage in storage page, rail, sort | **absent**: the scan only knows `<cache>/games` | GAP | scan `library/sega-cd/<id>/` too; shared rail/table/clean actions |
| 8 | Cleanup / removal | intermediates, archive, superseded, failed/partial, zig cache, remove-from-library (+cache) | none | GAP | same plans over Sega CD build folders; shared object cache is a build cache; **user data protected** (below) |
| 9 | Covers | Libretro download (menu, Settings, auto on add), manual change/remove, find online | manual change/remove OK; Libretro **disabled** (no system directory), no auto lookup on add | GAP | Libretro system `Sega - Mega-CD - Sega CD`; auto lookup after add |
| 10 | Keyboard / mouse / controller | shared nav engine, D-pad zones, `take_details_focus` | shared, but focus-after-build logic skipped for the Sega CD branch | PRES | same buttons, same refocus |
| 11 | Responsive / small window | rail beside the cards above 700 px, below them otherwise; minimum window 720x520 | no rail; panel text wraps independently | PRES | same breakpoints (rail/stack) and tile-label shortening |
| 12 | Settings, notifications | global settings (cleanup, regions, viewer); toasts: queued, dequeued, stopping, cancelled, failed | cancel/fail toasts; success reported only by the program-binding toast | PRES | one set of toasts incl. a build-finished toast for both |

## Protected user data (contract)

* Per Sega CD game the folder `<library>/sega-cd/<identity16>/` holds **generated** files (`build/`, `build.partial/`, `build.failed/`, `build-backup-ram.bin`,
  `last-report.json`, `run.log`, `replay-backup-ram.bin`, `backup-ram.next`) and **user data** (`backup-ram.bin`, `recordings/`).
* The storage report shows user data in its own *protected* row. It is not part of any cleanup plan: plans are built only from generated build folders, and
  `execute_plan` independently refuses any Sega CD item that is not one of `build`, `build.partial`, `build.failed` (defence in depth).
* No cleanup policy (post-build, keep-last-N, soft limit), "Clean intermediates", "Delete build", cache eviction, rebuild or program replacement touches
  user data. Removing a game from the library never deletes it; its plan states what is removed and what is kept. Re-adding the same BIOS and disc finds the save again.
* Backup RAM and recordings are never copied into logs, evidence or the catalog.

## Acceptance contract

1. One details structure: `draw_scd_details` no longer exists; Sega CD uses `draw_build_config`, the storage rail and `draw_provenance`.
2. Play, Build/Rebuild, Cancel, Unqueue and More are drawn by the same code at the same places; the context menu offers the same supported items.
3. One build at a time across platforms in one FIFO; progress (stage chips, live log), sidebar activity, cancellation, notifications, quit behaviour and
   previous-program preservation are identical; File-first reporting and safe program replacement are unchanged.
4. Sega CD has Libretro cover download (menu, Settings, auto on add) and the manual cover workflow.
5. Sega CD build folders and caches appear in storage metrics, the details rail, sorting and cleanup; protected user data is reported but not removable.
6. Automated tests (project-authored fixtures only): a parity journey for the three platforms - Library -> Details -> Build -> Play -> Return, cancel and retry,
   failure keeping the previous program, mode change, cover change, storage inspection/cleanup, context menu, gamepad-only and keyboard/mouse paths,
   1080-wide and minimum-width layouts, relaunch with persisted settings and saves - plus unit tests for the storage classification and protection.
   M12 save-persistence tests and the existing Genesis/SMS launcher tests keep passing.
7. Screenshots of the three platforms at identical sizes (1080 and the 720 minimum) are compared structurally (section order, action placement) by the test.
8. No CPU/device/AOT, no builder rewrite, no new framework.

## Consequences

The parity decision (T006) reports GO only when every row above is OK/HW or fixed with evidence; remaining GAP rows are named in a PARTIAL decision.
