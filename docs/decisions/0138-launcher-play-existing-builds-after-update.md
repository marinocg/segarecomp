# ADR 0138: Playing a verifiably compatible build from an earlier release

- Status: accepted (SEG-069-T005).
- Context: the launcher's cache identity (`entry_dir`) includes the product tool's size and timestamp, the launcher version, the policy identity, the host and the
  install path. After an update every existing build looked stale and had to be recompiled before it could be played, although the executable is self-contained
  apart from SDL3.
- Decision: keep `entry_dir()`/`entry_ready_for()` exactly as they are for generated-output reuse and new builds, and add a separate fail-closed predicate
  `find_playable_older_build` (launcher `older_build.*`) used for status and for Play only. It considers only builds recorded in the library catalog, never a
  scanned or foreign directory, and requires: a plain (non-symlink) 32-hex folder under the cache `games/` directory; executable + successful `status.json`;
  `metadata.json` matching the library ROM hash, platform and the host OS/CPU; for Master System the same declared mapper (option declaration) and the baseline
  profile; a complete recorded `aot_policy` for the same requested mode as now (the model family is reported as recorded); a matching H-interrupt discovery
  setting; the installed SDL3 library; and the host-interface marker. The marker is `runtime_contract` (currently 1), written into `metadata.json` by every
  new build together with `hint_alias_discovery`. Builds from before the marker are accepted only from the listed release `0.5.0`, whose launcher/viewer
  interface is the current one; no other release is declared compatible, and a version number is never proof by itself.
- Running an older build keeps its recorded mode and model (including the unadmitted_pc -> Compatibility restart, which is decided from the build that actually
  ran). To make the program find SDL3 after an install-path change, an older build is started with the installed SDL3 folder prepended to the loader path
  (`LD_LIBRARY_PATH` / `DYLD_FALLBACK_LIBRARY_PATH`; Windows already prepends it to `PATH`).
- Consequences: bumping `runtime_contract` (or removing `0.5.0` from the legacy list) is the one switch that withdraws old builds when a future change breaks
  the program/launcher interface. No second cache, index or relinking step exists; recompiling stays an explicit user action.
