# ADR 0325: Master System mod lifecycle, mod state, presentation-only graphics, the gameplay example and PSG sound mods

- Status: accepted (combined delivery F18: SEG-147-T004, SEG-147-T005, SEG-147-T006, SEG-147-T011, SEG-145-T004).
- Builds on: ADR 0280 (adapters), 0282 (phases), 0283 (ABI 2), 0295 (Luau API), 0299 (mod state), 0279 (Master System save states), 0324 (Master System targets and injection), 0286 (audio).
- Synthetic evidence only. No commercial input was used.

## 1. One Master System scheduler, no shared core

`platforms/master-system/mod/` holds the Master System realization of the phase contract: `sms_mod_scheduler.c`. It is deliberately not a core shared with the Genesis scheduler: the adapter contract allows
sharing only behind its four-part gate (mod-platform-adapters.md section 4), and the machines differ in address space, byte order, alignment, mapper and VDP. The platform-neutral parts that already were
shared stay shared: the ABI 2 core header, the negotiation function, the Luau VM and phase adapter.

* **Boundary.** The machine gained three optional hooks (`SmsFrameHooks`) at the frame start, T = N x 59,736: `boundary` before the scripted input of frame N is applied, `sampled` right after it, `reset`.
  They run at guest-time points, so a run sliced any way gives the same phases. Order: frame-end(N-1), render(N-1), frame-begin(N), [sample], input(N), post-input(N), then the guest. `sms_run_until_frame(N)`
  returns after post-input(N): the save-state barrier. With no scheduler attached the machine is unchanged; a Vanilla program links none of the layer.
* **Input.** The six pad bits of ports 1 and 2. Interception points the pad device at the mod's final input; the machine's physical input (recorded, saved and digested) is untouched. The console **Pause**
  button (an NMI edge, not a pad bit) cannot be read or changed.
* **Memory.** `z80.work_ram` (8 KiB, the mirror is the same space) read/write and `z80.cart_ram` (32 KiB) read-only, in manifest-declared ranges, width 1 or 2, little-endian, no alignment rule. Cartridge
  RAM ranges exist only for `sega`-mapper revisions (`E_MEMORY_SPACE`; at attach `E_RANGE_DECLARATION`). The mapper is never decoded at run time. Budgets and fault semantics are the phase contract's.
* **No function hooks** (no Z80 spike), no raw ports, no VDP writes, no audio command mappings (SEG-145-T012).

## 2. Luau and C11 are one path

A Luau program defines the same `segarecomp_mod_provider` entry a native package exports (`sms_mod_luau.cpp`), so scripts reach the machine only through the `SegaModHost` services and the render table a C11
provider calls. Capability, phase, range and budget rules cannot differ. The one API list gained `api_on_platform` and `api_capabilities`: a name exists on a platform or is absent (`E_LUAU_API` statically),
and `sega.mem.read` / `write` are granted by the platform's own memory capability. New names: `sega.present.*`, `sega.frame.work_ram`, `sega.frame.sms_vdp`; `sega.overlay.rect` / `text` draw on the
256 x 224 presented picture. Positioned layers, Genesis virtual sprites, `sega.scene`, `sega.frame.value` and `sega.audio.*` are not on this platform.

## 3. Presentation only (SEG-147-T006)

ABI minor **5** appends `SegaMasterSystemFeatures::render` (`SegaSmsRenderApi`) and `::vdp` (`SegaSmsVdpView`, a copy; capability `frame.sms_vdp`), and capability id **16** `present.transform` (palette mapping
and picture offset). `overlay.rgba` covers the HUD (64 rect/text requests) and authored sprites (64, from the mod's own 256-pattern pool and 16 SMS colours).

* The compositor (`sms_mod_present.c`) takes a finished framebuffer and writes host RGB. It has no machine argument, so it cannot touch guest state, digests, states or replay.
* Order: picture shifted by the offset (vacated pixels black) with each 6-bit CRAM **value** mapped through the palette overrides; authored sprites; overlay. The picture size never changes.
* **Provenance** (the SEG-140-T001 rule): only *authored* sprite art exists here. No request reads guest VRAM into a sprite, and there is no *verified game art* mode because the platform has no verified,
  exactly current snapshot gate; adding one is a new decision.
* A render callback's requests are staged and committed only if it does not fault. Nothing carries to the next frame. A frame with no request, a faulted mod, and the first frame shown after a state restore
  present the Vanilla picture. In the played program a composed frame replaces the picture and the widescreen extension is not used for it.

## 4. Mod state (SEG-147-T005)

The `SMSSTAT` container gains an optional eighth section `MODSTATE` (identity, state schema, a compatibility digest of ABI major, granted capabilities, phases, provider version and ranges, and the codec's
body). Without a mod the container is byte for byte what it was (seven sections). A seam (`sms_mod_state.h`) keeps the snapshot module free of the scheduler. Restore stages and validates everything, then
runs the mod's own `load` (the only fallible step) **before** any guest state is replaced. Statuses: mismatch (`E_STATE_MOD_MISMATCH` class: another mod, schema, configuration, or a mod on one side only),
rejected (damaged body or refusing mod; the guest is unchanged), unavailable (a guest-affecting mod without `state.mod` never produces a state). A Vanilla and a modded program normally refuse each other
earlier, because the variant identity already differs. Rewind uses the same codec.

**Restore after a mod's own failure** (inherited from ADR 0299, and the one place Luau and C11 differ). A Luau VM fault is sticky. After a **Luau** mod has faulted, loading or rewinding to a state from
before the fault is refused (the mod cannot take its data back) and it stays disabled; a **C11** mod disabled by a scheduler-detected fault accepts that state and is enabled again. Likewise, a healthy Luau
mod whose `load()` itself fails latches its VM, so besides refusing the restore the scheduler disables it with cause `state`. The guest is unchanged by every refusal. The Luau trust policy's "VM is rebuilt on
restore" is not implemented on either platform.

**Machine reset.** It runs `reset` again, restarts the replay witness (so it describes the run since the reset, like the viewer's input recording) and keeps a fault's disabled flag. The Luau VM and a native
provider's memory are not recreated: a mod must initialize its own state in `reset`. A recording made after a reset reproduces the run in a fresh process only for a mod that does so and had not faulted
before the reset.

## 5. Build route and tooling

`segarecomp build --luau-package` / `--native-package` accept Master System packages; the verified package's platform selects the route and supplies the mapper (`MOD_MAPPER_MISMATCH` for a conflicting
declaration). Injected code passes the Z80 owner's admission. Headless and played programs link the scheduler. The headless driver of such a program adds `--mod-ram`, `--mod-checkpoint`,
`--mod-state-save` / `--mod-state-load` and writes `mod.json` and `presented.rgb`; its bounds are `--frames` and `--cycle-budget`. `segarecomp mod test` runs Master System scenarios (the scenario `budget` is
the cycle budget, frames are 0-based machine frames, pad masks are Master System bits); a data-only package supports `expect_state` and `expect_pcm`. `mod diff-run` stays Genesis only (`E_DIFF_PLATFORM`).

## 6. PSG sound mods (SEG-145-T004)

No new mechanism: verified sound data and command tables are typed edits, PSG parameter data is data, and a driver change is ADR 0324 static injection (port I/O, indirect jumps and fall-through are refused).
The synthetic example has its own invented PSG driver; no common Master System song format or driver is assumed. Changes are compared through the headless PCM artifact (`audio.sha256`). The YM2413 FM unit
stays unsupported; the example's probe shows no unit answers.

## Limits

The launcher does not install Master System mods. One mod per build. No Game Gear. No widescreen claim for composed frames. A native provider is trusted code, not a sandbox.

## Evidence

`tests/sms_mod_scheduler_test.cpp`, `tests/mod_platform_conformance_test.cpp` (Master System rows flip to pass), `tests/mod_sms_lifecycle_test.py` (developer workflow, hand-derived gameplay, Vanilla golden
recorded with the runtime of `main` before this change, Luau/C11 parity, replay, presentation bit-identity, state, fault parity, refusals), `tests/mod_sms_sound_test.py`, `tests/sms_viewer_mod_test.py`
(SDL3, Linux: played program save/load, rewind, recording replay).
