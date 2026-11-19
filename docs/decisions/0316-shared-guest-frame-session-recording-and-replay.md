# 0316 - Shared guest-frame session recording and replay (SEG-146-T006)

## Status

Accepted.

## Context

Only the Sega CD launcher records input (a tick-keyed text script, 4,096 events, written when the viewer closes, no identity). Genesis had two hand-written scripts
(`SEGARECOMP_PAD_SCRIPT`, headless, applied between slices; `SEGARECOMP_VIEWER_PAD_SCRIPT`, keyed by host poll). Neither can replay a human session: the headless script is
applied between runner slices, so its effect depends on the slice size, and the viewer samples the host pad once per slice, mid-frame.

## Decision

1. A platform-neutral, header-only recorder/replayer (`libs/viewer/include/sv_session.h`, format `segarecomp-session 1`): frame-keyed pad changes, an identity header
   (platform, effective image SHA-256, package identity), periodic checkpoint digests, an explicit end marker, streamed writes, explicit caps that end a recording with an
   error instead of truncating. No SDL and no machine type; an adapter supplies the frame, the pad and the digest.
2. The Genesis adapter (`genesis_session.h`) uses the runtime's existing host-owned `GenesisFrameHooks` (`start` for frame 0, `after_snapshot` for later frames) and the
   `mod_pad_*` guest-pad override to latch the pad for the whole frame. A mod scheduler already in the hooks is wrapped, never replaced.
3. Initial scope is power-on recording; quick-load/rewind/saves are disabled while a session is active (never silently reusing frame numbers). Header-only keeps the
   many explicit source lists (`segarecomp build`, the bridge, CMake) unchanged, following `sv_perf.h`.
4. The Sega CD recorder is **not** migrated in this change: its grammar is tick-keyed with its own limits and tests, which must stay valid unchanged. The shared code is
   deliberately independent of it so a later change can move Sega CD or Master System onto it with a platform adapter.

## Consequences

* One file replays identically in the viewer and headless, under any slicing; a divergence is located to a 600-frame checkpoint interval.
* Mod schedulers keep working; their input phase sees the recorded physical pad.
* Recording across a changed ROM, saves, audio and video are non-goals; recordings from save states are deferred.
