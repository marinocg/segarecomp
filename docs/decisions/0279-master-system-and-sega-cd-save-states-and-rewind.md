# ADR 0279: Master System and Sega CD save states and visual rewind

- Status: accepted (SEG-132; one product PR, checkpoints A-H).
- Related: ADR 0278 (Genesis save states, persistence and rewind -- the contracts reused here), ADR 0252 (persistent Sega CD backup RAM), ADR 0204 (the Sega CD live viewer), ADR 0064 (the Master System machine).
- Inventory and restoration contracts (Checkpoint A, written before any codec): [docs/architecture/sms-sega-cd-save-states-inventory.md](../architecture/sms-sega-cd-save-states-inventory.md).

## Context

SEG-131 gave the Genesis a whole-machine snapshot, five numbered slots plus a quick slot, a bounded in-memory visual rewind, and the shared viewer UX (`SvStateProvider`, `sv_rewind`, F5/F7/F8, pause-menu rows, gamepad shortcut). The Master System and
the Sega CD showed the same rows disabled. Both consoles run on one guest timebase and advance in whole frames, so a frame boundary is a natural barrier; both have mutable state a naive struct dump would get wrong (host pointers, immutable media, observers).

## Decisions

1. **The shared viewer owns UX; each console owns a complete state codec.** No reflection framework, no universal machine object and no generic device serializer. `SmsSnapshot` (`SMSSTAT`) and `ScdSnapshot` (`SCDSTAT`) are field-by-field, explicit little-endian,
   bounded-section containers with an integrity digest, an identity, a compatibility fingerprint and an optional thumbnail -- the SEG-131 conventions -- and never `sizeof` a machine struct.
2. **Small neutral helpers are shared, nothing console-shaped is.** `libs/viewer` gained `sv_state_codec.h` (a bounded byte writer/reader), `sv_state_store` (directory creation, bounded regular-file read, atomic candidate-plus-rename replace) and
   `sv_state_service` (the generic `SvStateProvider`: six slot files, thumbnails, the bounded oldest-first rewind history with age and byte eviction, branch truncation, the retained-span note). A console supplies six callbacks. The Genesis adapter keeps its own earlier,
   equivalent copy untouched so that its accepted behaviour cannot regress; folding it into the shared service is possible later and not required.
3. **Barrier.** Master System: the top of an `sms_viewer_run` iteration (after the frame, its audio drain, pacing and presentation). Sega CD: `ScdLiveHost::poll`, the stop strictly before a vertical-blank onset, before that onset's input sample. The only adapter change in
   the Sega CD runtime is one optional `started(ctx, machine)` callback so the host can reach the machine.
4. **Two-phase restore.** The container is parsed, bounded and integrity-checked; identity and compatibility are verified; every section is decoded into a staged shallow clone of the live objects (so every host pointer, observer, sink and the instruction allowance keep their
   live values) and validated semantically, including cross-component invariants and bounds on every clock (a crafted far-behind clock could otherwise make the next catch-up loop run for minutes); only then are plain copies committed. A failure leaves the machine byte-identical.
5. **Identities.** *Master System:* source cartridge digest (the effective image) and variant, plus a fingerprint of the state schema, the mapper family, the ROM size, the RAM/VRAM/framebuffer geometry, the PSG variant and the PCM grid: a state for one mapping cannot load under another. *Sega CD:* the verified disc
   identity and the BIOS image digest, plus a fingerprint of the executable-image contract (materialized sub images, overlay manifest, dynamic images, processor program facts, the sound configuration). Build directories, the compiler, the generator version and compiled-entry counts are deliberately
   not part of it: rebuilding compatible code keeps states; a different image set is refused.
6. **Sega CD scope.** The snapshot covers both 68000s and their timing, the three RAMs, the Gate Array, CDC (including a transfer in flight), CDD, the delivery ledger, PCM, CD-DA and the mixer timeline (clock, hold value, unconsumed queue and output), the graphics ASIC, the live pad,
   overlay selection and dynamic bindings, backup RAM contents, the main-side Genesis VDP/I-O/Z80 bus and -- through the existing Genesis section codecs, exported as thin wrappers -- the Z80, YM2612, PSG and mixer. The Sega CD mix reads the FM/PSG output ring by absolute frame index, so the ring frames
   it has not consumed are guest-timeline state (the Genesis codec drops that ring because there it is host playback only); they travel in the Z80 section. This was found by the replay-equivalence test of the production configuration and is covered by it.
7. **Fail closed, with a reason.** Not capturable: a build pass or diagnostic observer, a scripted or recorded input, a latched stop, the main-side Z80 attached without its sound devices, a machine in the middle of an instruction. Recording sessions (`--viewer-record`, `--record-input`) and
   replays get no states and no rewind: a restore would rewrite the history the recording describes.
8. **Master System cartridge RAM stays non-persistent.** The mapper contract can say a RAM page is *mappable*, not that it is battery-backed, and the header and sidecar manifest carry no such flag. It is part of every state; it is not saved between sessions; the limitation is documented.
9. **Backup RAM lifecycle is unchanged.** A restore replaces only the in-memory bytes; the launcher's candidate/previous-generation commit at a clean exit is the only writer; browsing, cancelling, a refused state and a failed save never write it; the session exports the timeline it ended on. A state with backup RAM is refused by a session without it.
10. **Key conflict.** F7 is rewind on every console. The Sega CD frame step moved from F7 to F9 (F6 still holds). F9 is unreserved; neither action can trigger the other.
11. **Rewind budget unchanged, span reported honestly.** A Sega CD moment is about 1.1 MiB, so the 96 MiB default keeps 86 moments (about 43 s at 500 ms). The existing oldest-first eviction applies and the rewind screen reports the real figure. No compression, delta chain or background thread was added: the
    measured capture costs (below) do not justify one.
12. **Storage.** `<library>/saves/master-system/<cartridge sha256>/<mapper>/states/` and, for Sega CD, `<game folder>/states/` next to the backup RAM and recordings. Both are counted as protected user data by the Storage view, are never reachable from a cleanup plan, and are never deleted.

## Consequences

* Same player-facing workflow and defaults on all three consoles; the codecs are independent and individually tested.
* A Sega CD state is large (about 1.1 MiB); slots are cheap, the rewind history is the only memory consumer and is bounded.
* The Sega CD snapshot depends on the Genesis section codecs only through the exported part wrappers; a change to a Genesis section is a change to both containers' Z80 sections and is caught by the Sega CD z80+audio test.
* **Coverage gap, stated plainly:** no available fixture binds a *main*-processor dynamic image at run time (all bindings of the file-first fixtures are the sub CPU's). The codec validates and restores main-table bindings at work-RAM / Word-RAM addresses (checked with a forged state), but their *execution* after a restore is exercised only by the local, uncommitted Sonic CD smoke test. The Sega CD overlay-group section likewise has no fixture.
* Known limitations are listed in the user guide and the compatibility ledger: no Master System battery persistence, no states for recording/replay/diagnostic sessions, the first frame after a Master System load is shown in Original widescreen, a Sega CD program built with the Z80 but without sound devices is refused.

## Evidence

* `tests/sms_snapshot_test.py` (replay equivalence from every frame boundary, script and live input; refusals; crafted values; mutation controls), `tests/sms_viewer_states_test.py` (real viewer processes), `libs/viewer/test/sv_state_service_test.c` (slots, atomic files, bounded history).
* `tests/sega_cd_snapshot_test.py` (the same machine against itself across snapshot/restore windows, cross-process, a corrupted machine restored, backup RAM, refusals, the production Z80 configuration) and `tests/sega_cd_viewer_states_test.py` (real SDL3 viewer, F5/F7/F8/F6/F9).
* Measurements: [docs/product/save-states.md](../product/save-states.md).
