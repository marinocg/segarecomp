# Master System and Sega CD save states: machine inventory and restoration contracts (SEG-132, Checkpoint A)

Status: reviewed inventory and contract. Gate A: **no implementation of a console codec began before every execution-affecting state owner below had an explicit
treatment.** Companion to ADR 0278 (Genesis, SEG-131) and ADR 0279 (this work). Product-repository document; no commercial content.

Treatments used throughout:

* **R** restored (serialized in the console's own codec, validated, committed atomically);
* **B** rebound (a host pointer, callback, immutable table or media object that stays attached to the *running* machine; never serialized);
* **C** recreated (derived host data rebuilt from restored state, or reset to its deterministic initial value);
* **X** explicitly excluded, with the reason (diagnostic observer, host-only accounting, cache);
* **F** fail closed (the capability is refused for that configuration, with a player-readable reason).

## 1. Contracts shared by both consoles (inherited unchanged from SEG-131)

1. **Barrier.** Snapshots, slot loads and rewind restores run only at the adapter's once-per-slice service point (`sv_host_service`), while the guest is not
   executing and no host callback is in flight. SMS: the top of an `sms_viewer_run` iteration (after the previous frame was run, its audio drained, its picture
   presented, its pacing done). Sega CD: the top of an `scd_live_run` iteration (`ScdLiveHost::poll`), i.e. the stop boundary strictly before a vertical-blank
   onset, before that onset's input sample and device events.
2. **Staged restore.** Parse and bound-check the whole container, verify the integrity digest and identity, decode every section into *temporary* storage,
   run semantic validation, and only then commit with plain copies that cannot fail. Any failure leaves the running machine byte-identical.
3. **Container.** Explicit little-endian fields, bounded sections (fixed maxima, never sized by a file length), source identity, compatibility fingerprint,
   SHA-256 integrity digest, optional RGB565 thumbnail (<= 160x112) of the frame the guest itself published. Never `sizeof(struct)`; never a host address.
4. **Shared viewer** owns UX (`SvStateProvider`, `sv_rewind`, slot pages, F5/F8/F7, pause menu rows, gamepad shortcut). A console supplies a codec and a thin
   adapter; the viewer never reads a machine.
5. **Shared code** (small, platform-neutral, new in `libs/viewer`): `sv_state_store` (directory creation, bounded regular-file read, atomic candidate+rename
   replace, slot file names) and `sv_state_service` (the generic `SvStateProvider` implementation over four console callbacks: capture, inspect/validate,
   restore, after-restore; plus the bounded oldest-first rewind history with byte budget, age limit, branch truncation and retained-span reporting). Genesis keeps
   its own proven copy untouched (no regression risk to accepted behaviour); consolidating it is a possible later cleanup.
6. **Not added:** interpreter, JIT, runtime decoding, background snapshot thread, compression or delta chain, reflection framework.
7. **Availability.** States are offered only for an interactive launcher session that supplied `SEGARECOMP_SAVE_DIR` and a valid `SEGARECOMP_ROM_SHA256` (and,
   for Sega CD, the disc/BIOS identity). Replays, recordings (`--viewer-record`, `--record-input`), scripted-input sessions and build passes get none. This is
   the only rule that keeps an input recording reproducible: a restore would rewrite history the recording describes.
8. **Rewind policy** (defaults, env-tunable exactly like Genesis): one snapshot per 500 ms of *guest* time, target 60 s, 96 MiB budget, oldest-first eviction,
   real retained span reported when memory (not age) limits it. Rewind never touches the disk.

## 2. Master System

### 2.1 Barrier and timebase

`sms_viewer_run` advances exactly one guest frame per iteration (`sms_run_until_frame(frame + 1)`), then `sms_psg_sync`, audio drain, pacing, present. The single
timebase is the Z80 T-state counter (`rt.state.cycles`; frame = 59,736 T). The barrier is the `poll` call at the top of the next iteration. At that point the
line-0 event of the next frame has already been applied (events with `T <= cycles` are drained before a stop), so the renderer holds a *new frame in progress*
(`in_frame`, row 0 rendered) while `last` is the completed, presented frame. Both are part of the state. No other scheduler is created.

### 2.2 Inventory (`SmsViewerRig` and everything it reaches)

| Owner | Fields | Treatment |
|---|---|---|
| Z80 (`rt.state`) | AF BC DE HL and shadows, IX IY SP PC WZ, I R IM IFF1 IFF2 Q, halted, int_deferral, ld_a_ir, in_prefix_run, int_line, nmi_pending, nmi_reject, cycles | **R** (ABI field order is the digest's; `deadline` excluded: it is host scheduling, re-set by every run) |
| Z80 runtime | `rt.outcome` | **R** (must be `NONE`/resumable; any error outcome = **F** not capturable) |
| Z80 host callbacks | `rt.host.*` (context = machine) | **B** |
| Memory | `ram[8192]`, `cart_ram[32768]`, `mapper_regs[4]`, `memory_control` | **R** |
| Memory | `rom`, `rom_size`, `family` | **B** (immutable embedded ROM; family+size are part of the identity) |
| Memory error latch | `error`, `error_*` | must be `SMS_OK` at capture, else **F** (a latched stop is never snapshotted) |
| Machine | `io_control`, `next_line`, `irq_mask`, `dead`/`dead_stop` | **R** (`dead` must be 0, else **F**) |
| Machine | `input` (p1, p2, pause) | **R** |
| Machine | `input_events/count/next` (script cursor) | **R as cursor**: `input_next` plus a SHA-256 of the consumed prefix are saved and re-verified against the target's script; the viewer's live recording array is **X** and sessions that record are **F** (item 1.7) |
| Machine | `stop_on_halt_idle` | **B** (host option) |
| Machine | `irq_trace`, `mapper_trace` buffers, counts, dropped | **X** diagnostic observers (never read by guest semantics); counts keep running |
| VDP | `vram[16384]`, `cram[32]`, `reg[16]`, `address`, `code`, `latch_set`, `latch_byte`, `read_buffer`, `frame_pending`, `sprite_overflow`, `sprite_collision`, `line_pending`, `line_counter`, `hcounter_latch`, `hcounter_valid`, `line`, `frame`, `vram_writes`, `cram_writes` | **R** (`vram_writes`/`cram_writes` feed the trace summaries and the machine digest, so they are restored) |
| VDP | `error_sink`, `line_hook`, `line_hook_context` | **B** (re-pointed by `sms_vdp_install`/`sms_renderer_attach`; must still point at the running machine/renderer) |
| VDP | `trace`, `trace_count/dropped/capacity` | **X** observer |
| Renderer | `pixels` (frame in progress), `last` (published frame), `height`, `last_height`, `last_frame`, `vscroll`, `in_frame`, `frames_completed` | **R** (the visible frame and the in-progress row are execution-adjacent: `last` is what the thumbnail and the next present show, `frames_completed` is the presentation identity) |
| Renderer | `records`, `record_count/dropped` | **X** diagnostic frame hashes (counts keep running) |
| Renderer extension | `ext` (Level 2/3 widescreen target) | **C** on restore: `sms_extension_reset` (producer state and published result are presentation-only; the next frame regenerates them under the current request); never serialized |
| PSG chip | `Sn76489` tone periods/counters/outputs, attenuations, noise control/counter/flip, LFSR, latch, `ticks` | **R** through the existing `sn76489_state_bytes` / `sn76489_load_state_bytes` pair |
| PSG chip | `config` (taps, width, divider, levels) | **B** (chip's own; must equal the default contract config, else **F**) |
| PSG PCM stream | `Sn76489Pcm` `index`, `boundary`, `sum`, `count`, `last_mean`, `batch_first`, `batch_count`, `batch[64]` | **R** (guest-time sampling counters: the decimation accumulator and the not-yet-delivered batch are part of the future sample stream) — see 2.4 |
| PSG PCM stream | `config`, `sink`, `sink_context` | **B** |
| Pad | `io_control`, `h_latches` | **R** (`input`, `vdp` pointers **B**) |
| Ring (host audio FIFO) | `Sn76489Ring` data/indices/dropped | **C**: cleared on restore (abandoned audio of the old future is discarded); it is host presentation, not guest state |
| Viewer rig | `event_count`, `events[]`, `recorded` | `recorded` **R** (equals `machine.input` at the barrier); `events[]`/`event_count` **X** (see item 1.7); after restore `input_next = input_count` so no stale event is pending |
| Viewer pacing | `SmsPacer` | **C** (deadline rebased; counters continue) |
| Viewer present baseline | local `presented` in `sms_viewer_run` | **C**: moved into the rig and invalidated so the first frame after a restore is always presented |
| SDL | window, audio stream, GPU textures, held keys/pad latches | **C**/discarded: audio stream cleared, held inputs released, presentation settings untouched |

### 2.3 Cartridge RAM and persistence

`SmsMemory.cart_ram` is 32 KiB, zero at power-on, "never persisted". Whether a cartridge has *battery-backed* RAM is not knowable from the supported mapper
contract: the Sega mapper declaration (`$FFFC` bit 3/2 RAM select) says only that a RAM page *can* be mapped, not that it is non-volatile; the header and the
mapper manifest carry no battery flag, and a cartridge that maps RAM without a battery would otherwise be given a false save. **Decision: SMS cartridge RAM stays
non-persistent** (limitation documented in the product guide and ledger). It **is** part of every whole-machine snapshot.

### 2.4 SMS compatibility identity

A snapshot is bound to: (1) **source** = SHA-256 of the cartridge image the program was built from (`SEGARECOMP_ROM_SHA256`, the effective possibly-patched image);
(2) **mapper/profile declaration** = family, ROM size and the profile string (`sms2_ntsc_export`) hashed into the **execution fingerprint** together with the
state schema and every size/constant the sections depend on (RAM, cart RAM, VRAM, CRAM, register count, framebuffer geometry, PSG state size, PCM config
`rate_num/rate_den/full_scale`, cycles-per-frame). A state saved under one mapper declaration cannot load under another. Variant (patch) identity is the launcher's
`SEGARECOMP_SAVE_VARIANT` (all zero = vanilla) as in Genesis. Rebuilding the same program keeps the identity: nothing about the build directory or compiler enters it.

### 2.5 Recording implication

Sessions that record (`--viewer-record`) are refused (reason shown). The script describes the run since the last reset; a restore would break monotonic frame
order and reproducibility. A live, non-recording session still keeps appending to its in-memory event array (it already does so today, bounded by
`SMS_VIEWER_INPUT_CAPACITY`); restores do not rewind it and a full array keeps its existing `SMS_VIEWER_INPUT_FULL` outcome.

## 3. Sega CD

### 3.1 Barrier and the minimum adapter change

`scd_live_run` advances one video frame per loop iteration from "the stop boundary before one vertical-blank onset" to the next. `ScdLiveHost::poll(ctx,
boundary, out)` is called at the top of every iteration, before `scd_input_live_sample`, before any device event of the onset, with both processors stopped at
instruction boundaries (`scd_machine_run` returned `SCD_RUN_DEADLINE`; no host callback is mid-flight). **That is the barrier.** The poll is where the shared host
already services pause/menu, so provider calls arrive there.

The viewer's poll receives only its own `ctx`, so the machine is not reachable. **Minimum change:** one optional callback `ScdLiveHost::started(ctx, ScdMachine *m)`
called once by `scd_live_run` after the machine is set up and before the first poll (NULL = an older-style host; nothing else changes), plus the existing
`finished` callback keeps the machine for the stop screen. The program-level identities (disc identity, BIOS SHA-256, the `ScdAuthentic` image facts, overlay and
dynamic registries, Z80 registry) are reachable from the machine (`m->program`, `m->authentic`, `m->overlays`, the registered dynamic program) at `started` time.

### 3.2 Inventory (`ScdMachine` and everything it reaches)

| Owner | Fields | Treatment |
|---|---|---|
| Main and sub MC68000 | `cpu[2]`: `d[8]`, `a[8]`, `usp`, `pc`, `sr`, `irq` (`SegarecompM68kInterruptState`), `next`, `retired`, `cycles`, `trace`, `prefetch`; `cycle_ticks` | **R** (`cycle_ticks` re-derived from the profile and validated equal) |
| Machine time | `now`, `vdp_onset`, `vdp_frames`, `device_log` | **R** (`device_log` is the FNV chain that proves equality in tests) |
| Host-only allowances | `instruction_budget`, `ignored_writes`, `prg_write_protect_drops`, `sub_waits`, `sub_interrupts_taken[]`, `main_interrupts_taken[]`, `z80_area_ignored_writes`, `backup_ram_reads/writes`, and every other plain counter | **X** — the budget is *never* rolled back (a restore cannot extend the bounded run); counters are diagnostic. Scheduling bits that change behaviour (`sub_wait_request`, `main_refresh_*`, `main_irq_delay`, `main_extra_ticks`, `refresh_skip_cycles`) are **R** |
| Interrupts | `vint_pending`, `vint_at`, `hint_counter`, `hint_pending`, `hint_next`, `hint_at`, `hint_latched`, `hint_vector`, `ga.pending`, `ga.ifl2`, `ga.timer_*` | **R** |
| Main DMA / bus hold | `main_dma_until`, `main_dma_multi`, `main_dma_bank_until`, `vdp_dma_bank_count`, `wordram_mode_changes_in_dma`, `vdp_dma_count`, `vdp_dma_stall_ticks` | **R** (the first three change future timing; the counters are **X**) |
| RAM | `prg_ram` (512 KiB), `word_ram` (256 KiB), `work_ram` (64 KiB) | **R** |
| RAM ownership | `ga.mode` (RET/DMNA/1M), `ga.prg_bank`, `ga.write_protect`, `ga.prg_window`, `ga.sub_halted`, `ga.sub_wait`, `ga.ctl`, `ga.dmna_int` | **R** |
| Gate Array | `led`, `main_flag`, `sub_flag`, `comcmd[8]`, `comstat[8]`, `interrupt_enable`, `ien2_mirror`, `reg32_hi`, `timer_reload`, `cdd_ctrl_*`, `scratch[0x98]`, `stopwatch_origin/base` | **R** |
| CDC | all scalars (`address`, `destination`, `edt`, `dsr`, `dma_address`, `ifstat`, `ifctrl`, `ctrl0/1`, `irq`, `dbc`, `dac`, `pt`, `wa`, `head`, `stat*`, `decoder_end`, `host_word`, `dma_state`, `dma_target`, `dma_base`), `buffer[16384+2352]` | **R** (`dma_total`/`dma_done` are reports: **X**) — a CDC DMA in flight is restored with its base tick |
| CDD | `tx[10]`, `rx[10]`, `status`, `pending`, `latency`, `lba`, `index`, `anchor`, `grid_line0`, `grid_acc0`, `next_tick`, `delivered` | **R** (pending drive operations and latency included) |
| Delivery ledger | `delivered[]`, `delivered_count`, `delivered_overflow` | **R** (it is the evidence dynamic binding consults: execution-affecting) |
| Disc | `ScdDisc` (file handles, table), `identity` | **B**: the verified open disc stays attached; the saved *identity string* must equal it, else **F** (wrong disc). Track/LBA tables are immutable. |
| BIOS | `bios`, `bios_size`, `authentic` | **B**: SHA-256 of the verified BIOS image is part of the identity |
| Sub epoch bookkeeping | `sub_pristine`, `epochs`, `epoch_image`, `epoch_tick`, `materialize`, `capture_*`, `verify_only` | **R** for `sub_pristine`, `epochs`, `epoch_image`, `epoch_tick`; `materialize`/`verify_only`/`capture_*` non-zero (build pass) = **F** |
| PCM (RF5C164) | `env/pan/st/fd/ls[8]`, `index`, `bank`, `enabled`, `onoff`, `running`, `addr[8]`, `origin`, `samples`, `ram[64 KiB]` | **R** (`syncs`, `advanced`, `channel_steps`, `max_catchup` are cost counters: **X**) |
| CD-DA | `fader_register/target/volume`, `playing`, `next_lba`, `last_index` | **R**; `sink`, `sink_context`, `enabled`, `log` **B**; counters/`commands[]` **X** |
| Mixer timeline | `tick`, `rem`, `frames`, `held[2]`, `q_head`, `q_count`, `cd_active`, `cd_next`, `cd_end`, `fader_volume`, `queue[]`, and the `chunk[]`/`chunk_count` not-yet-delivered output | **R** (the sample clock, hold value and the unconsumed CD-DA queue *are* the audio future); `sink`/`sink_context`/`enabled` **B**; statistics **X**. Audio already handed to the host before the barrier is abandoned host audio (**C**) |
| Graphics ASIC | `busy`, `ctl`, `map_base`, `cell_v`, `buffer_origin`, `offset`, `h_dots`, `trace_base`, `trace_ptr`, `map_ptr`, `dot_mask`, `buffer_offset`, `buffer_start`, `stamp_shift`, `map_shift`, `lines_left`, `base`, `line_ticks` | **R** (an operation in progress restores with its progress); `operations`, `lines_rendered`, `vdot_reads` **X** |
| Backup RAM | `backup_ram_present`, `backup_ram[8192]` | **R as in-memory contents only**; the launcher file lifecycle is untouched (4.2) |
| Main-side VDP (Genesis device host `ScdVdpHost.host`) | `devices.vdp` (VRAM, CRAM, VSRAM, registers, control/data port state, status, DMA state), I/O and controller latches, Z80 bus-request/reset/bank latches, `pad1/pad2` | **R** through the Genesis section codecs (exposed, not reimplemented); host pointers (`owned_regions`, observers, hooks) **B**; `scheduler.master_ticks` **C** (derived from `now` by `scd_host()`) |
| `ScdVdpHost` | FIFO model `fifo_cycles[4]`, `fifo_idx`, `fill_end`, `dma_armed` | **R** |
| `ScdVdpHost` | `stage[]`, `stage_region` | **C** (a per-transfer staging copy; rebuilt by the next DMA, empty at the barrier) |
| `ScdVdpHost` | presentation: `frame`, `present*`, `raster`, `vram_line0` | **R** `raster`/`vram_line0` of the frame *in progress* (the per-line samples of lines already passed; execution-adjacent for the next published frame); `frame` artifact and `present_ready` **C** (a pending completed frame is presented before the barrier by `scd_live_present`); `extension` **B** |
| `ScdVdpHost` | capture request machinery (`capture_dir`, `captures[]`, `capture_line*`) | non-zero capture count = **F** (a diagnostic build pass feature) |
| Main-side Z80 | `ScdZ80.machine` (`GenesisZ80Machine`: CPU state, RAM view, BUSREQ/RESET, bank register, lazy-follower scheduling, epoch bookkeeping), `bank` view | **R** through the Genesis Z80 section codec (image-epoch registry check as in Genesis: a state needing an image this build lacks is refused); `owner`, `bank` view **B**; `unknown[]`, `log[]`, `pass`, `unmodelled`, `finish_*` — `pass`/`unmodelled`/`unknown_count != 0` = **F** (build-pass state); `log` **X** |
| YM2612 / PSG / Genesis mixer (`SCD_Z80_AUDIO`) | `GenesisAudio` | **R** through the Genesis YM2612 / PSG / mixer section codecs (including `audio_synced`, `audio_stopped`); `audio_stopped` set = **F** (latched stop) |
| Input | `ScdInput`: `pad[2]`, `live_pad[2]`, `live_last_tick`, `reads[2]`, `live`, `recording` | live, non-recording input: pads and `live_last_tick` **R**; `events`/`next`/`count` are empty at the barrier (every sampled event was applied during the frame) and are validated empty; `compacted`, `log*`, `script_bytes` **X**. A script-driven (`--input-script`) or recording (`--record-input`) session = **F** |
| Overlays | `overlay[16]` (`selected`, `stale_from`, `all_dirty`, `any_dirty`, `dirty_lo/hi`, `dirty[]` bitmap), `overlays` program pointer | **R** the six behavioural fields per active group; `selections`, `revalidations`, `invalidations`, `retired[]`, `overlay_log`, `overlay_events` **X**; the program table **B** and its `manifest_identity` is part of the identity |
| Dynamic images | `dyn.enabled`, `dyn.cpu`, `dyn.binding[2][16]` (`active`, `image`, `base`, `lo`, `hi`, `run_lo`, `run_hi`, `granule_first`, `order`, `retired`, `stale`, `dirty[]`, `mutated[]`), `dyn.order` | **R** for every active binding (the binding decides which precompiled body serves a PC); statistics, events, `bound_ever`, `exercised`, per-image accounting, `image_seen` **X**; the 4-sector comparison cache `cache*` **C** (invalidated); the dynamic *program* (image ids, first LBA, length, SHA-256, body tables) **B** and is part of the identity |
| Observers | `access_trace`, `write_log`, `pcm_trace`, `event_log`, `coverage`, `reloc` | **X** when absent; present (a diagnostic/build session) = **F** |
| Process-global mutable state | see 3.3 | |
| Host adapter | `ScdSdlHost` (window, `GenesisPacer`, audio presenter, `pending[]` CD-DA frames, key script), `GenesisViewerHost` | **C**: pending host audio discarded and the presenter restarted, pacer rebased, held inputs released, extension/presentation settings preserved |

### 3.3 Mutable process-global state (audited)

Searched every non-`const` static/global in `platforms/sega-cd/runtime/**`, `viewer/**` and the Genesis units it links:

* `scd_dyn_registered` (`const ScdDynamicProgram *`) and `scd_z80_ops_registered` (`const ScdZ80Ops *`): immutable registrations written once before the machine starts — **B**, and their content is in the identity.
* `scd_dyn_machine` (the machine the dynamic registry is attached to, read by `scd_dynamic_exec_cpu`): a pointer to the *running* machine — **B**; the single field it exposes (`dyn.cpu`) is restored with `dyn`.
* `g_cdda_tap` / `g_mix_tap` (`scd_main.c`): the host sinks' SHA-256 chain, the optional PCM file and the host forwarding — pure one-way output taps, **X** (a restore does not rewind a digest or file of a *run*; they are not enabled in interactive play, where the sinks forward to the viewer).
* `scd_coverage.c: scd_edges`: build-pass coverage, **X**; active coverage = **F**.
* Genesis-side per-process caches in the linked Genesis runtime (Z80 registry, YM2612 vendor state): the YM2612 *chip* state lives in `GenesisAudio` and is restored; the registry is immutable.

Nothing else mutable and execution-affecting was found. Any configuration in which a future finding contradicts this is **F** by the capture-time checks listed in the
table (each is a precise, named refusal, not a silent omission).

### 3.4 Sega CD compatibility identity

Bound to: (1) **disc identity** (`ScdProgram.disc.identity`, the verified identity string the program was built for — also checked against the opened disc); (2) **BIOS**
SHA-256 (`authentic->bios_sha256`, the verified image mapped at main `$000000`); (3) the **executable-image contract**: SHA-256 over {BIOS profile ip_entry; each
materialized sub image (`length`, `sha256`); overlay `manifest_identity`; every dynamic image (`id`, `first_lba`, `length`, `sha256`); the processors' `image_base`,
`image_disc_offset`, `image_length`, `entry_pc`; the Z80 image registry digest when a Z80 is attached; the sound-attached flag; the state schema and every size the
sections depend on}. Deliberately **not** included: the build directory, cache path, compiler, generator version string (`program_identity`), compiled entry counts.
Rebuilding compatible native code keeps the states; a genuinely different image set (different BIOS, disc, materialized or dynamic image, overlay manifest, Z80
image) is refused with a specific reason. The launcher's per-game identity (the game folder keyed by the library entry) selects the state directory.

## 4. Storage and the backup RAM lifecycle

### 4.1 Layout

`<library>/saves/<platform>/<identity>/<variant>/states/{quick.sgstate, slot-1..5.sgstate}` for SMS (`platform = master-system`, identity = SHA-256 of the cartridge as
for Genesis) and Sega CD (`platform = sega-cd`, identity = the library entry's disc identity digest; the BIOS profile is inside the container, so a BIOS change
shows as an incompatible, not-loadable slot rather than a different folder). Same file names as Genesis (shared viewer UX and tools). The launcher validates the
identity as lowercase hex, creates the folder, and passes `SEGARECOMP_SAVE_DIR` for an interactive launcher session only. A state never lives under the build
cache. Writes use the shared atomic candidate+rename helper.

### 4.2 Sega CD backup RAM is unchanged

* The launcher still passes `--backup-ram` (canonical) and `--backup-ram-out` (candidate); the program still exports the final 8 KiB only for a run that ends with
  status 0; the launcher still validates and installs it with one rename keeping `backup-ram.bin.prev`.
* A snapshot contains the in-memory backup RAM bytes; a **restore replaces only the in-memory bytes** (`m->backup_ram`). It never writes a file.
* Browsing the rewind surface, cancelling it, loading an invalid state and saving a slot never write backup RAM. The timeline the player ends the session on is
  the one a clean exit exports through the existing candidate/commit mechanism.
* A state is only restorable into a machine whose backup RAM is *present* iff the state's was (`backup_ram_present` equal), so a session without `--backup-ram`
  cannot receive RAM it could not export.
* Typed stops, crashes, build passes and replays acquire no new write path.

## 5. Sega CD input conflict: F6 / F7

SEG-131 reserved **F7** for rewind (a shared host key). Sega CD used F7 to step while held. Decision: **one consistent F7 = rewind on every console**; frame step
moves to **F9** (free; F3, F10, F11, F12 are already reserved host keys, F5/F7/F8 now states). F6 (hold/release) is unchanged. The Sega CD hold/step handler stops
peeking F7; F9 steps only while held; the shared host never sees F9. Neither action can trigger the other. UI help (`docs/product/sega-cd-play.md`, viewer help) and
tests are updated.

## 6. Memory budget (estimates, replaced by measurements in Checkpoints D/G)

SMS snapshot: ~8 KiB RAM + 32 KiB cart RAM + 16 KiB VRAM + frame buffers (2 x 57 KiB) + thumbnail <= 35 KiB + small scalars = about 200 KiB, i.e. ~120 entries
(60 s at 500 ms) is ~24 MiB, inside the 96 MiB budget. Sega CD: 512 + 256 + 64 KiB RAM + 64 KiB PCM RAM + 18 KiB CDC + 8 KiB backup + ~70 KiB main VDP + Z80/YM +
mixer queue + thumbnail = about 1.1 MiB, so 96 MiB holds ~85 entries (~42 s at 500 ms). The budget is **not** changed before measuring; if the cap shortens the span,
the oldest-first eviction applies and the real retained span is reported by the existing history note. No compression or delta chain without measured need.

## 7. Gate A result

Every execution-affecting owner above has a treatment; every **F** has a named refusal; the three questions the brief required answered are: *barrier* (2.1, 3.1),
*adapter change* (3.1: one optional `started` callback), *identities* (2.4, 3.4). Implementation may proceed: SMS first (Checkpoints B-D), Sega CD afterwards (E-F).
