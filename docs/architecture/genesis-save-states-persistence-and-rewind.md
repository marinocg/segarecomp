# Genesis save states, cartridge persistence and visual rewind: architecture contract

Status: frozen by Checkpoint A of SEG-131 (product PR task/seg-131-t001). Later checkpoints implement this contract; a change
to it is recorded in ADR 0278 rather than silently diverging. Scope: Genesis / Mega Drive only. Related: ADR 0098 (header-declared
SRAM), ADR 0136 (X24C01 serial EEPROM), ADR 0252 (Sega CD backup RAM persistence, unchanged), ADR 0072/0075 (Z80 machine, mixer).

## 1. Principles

- The program is AOT-generated native code. Restoring a state never decodes an instruction, never interprets ROM and never replays
  from power-on. A snapshot is plain data; the generated dispatcher continues from the restored PC.
- Explicit canonical fields. No `sizeof(struct)` is written; every field is serialized little-endian at a fixed width.
- Pointers, callbacks, ROM references, SDL objects and diagnostic observers are never serialized. They stay attached to the
  initialized machine; restore only rewrites the data they point at.
- Two-phase restore: parse + validate everything into temporary storage (nothing touched), then commit with plain copies that
  cannot fail. A failed load leaves the running machine byte-identical.
- No generic CPU serializer, no new threading, no database, no compression until a measurement demands it.

## 2. Synchronization barrier

Snapshots are captured and restored **only between two `genesis_runtime_run` slices**, i.e. in the host's once-per-slice window of
`genesis_viewer_run` (`viewer.c`): after `host->after_slice` has drained the mixer and presented frames, before `window_closed()`
samples input. At that point:

- the M68K is on a dispatch boundary: `runtime->pc` is the next dispatch target and no generated function is mid-flight (all
  M68K state lives in `GenesisRuntime`; generated code and `runtime.c` have **no** mutable statics);
- no host callback is executing; the pause overlay, rewind overlay and slot menus run inside `window_closed()` (`sv_host_service`
  blocks), so guest time does not advance while they are open;
- the Z80 and the audio devices may lag the master clock (`z80_synced_ticks <= scheduler.master_ticks`). This is part of the state and
  is captured as is: every interaction with Z80-domain state synchronizes first (ADR 0072 contract section 10), so the lag is
  deterministic and restoring it reproduces the future exactly. Capture performs **no** synchronization and mutates nothing
  (`const` inputs);
- a pending IRQ6/IRQ4 request, a DMA in progress, a stalled STOP and a nested exception frame are all fields of `GenesisRuntime`.

Restore additionally requires a **sink-side reset** (section 7) and is refused while a typed guest stop is latched.

## 3. Snapshot inventory

Classes: **R** restore (execution-affecting), **B** rebind (pointer/callback/immutable image; kept from the initialized machine),
**C** recreate (presentation), **X** exclude (diagnostic, cannot affect guest execution).

### 3.1 `GenesisRuntime` (`platforms/genesis/runtime/runtime.h`)

| Field(s) | Class | Notes |
| --- | --- | --- |
| `d[8]`, `a[8]`, `usp`, `sr`, `pc` | R | `a[7]` is the active SP, `usp` the inactive one (ADR 0043 section 6) |
| `pad1`, `pad2` | X (cleared) | host input latches; restored to 0 and re-sampled at the next slice; the held-input policy is section 7 |
| `region_profile`, `region_configured` | R (checked) | interpretation-affecting; part of the machine profile and verified equal, not overwritten |
| `work_ram[65536]` | R | |
| `owned_regions`, `owned_region_count`, `cartridge_empty_upper_slot` | B | immutable generated cartridge data; identity is the ROM digest |
| `cartridge_sram`, `cartridge_sram_storage` | B | pointer/config rebind; **bytes** of `cartridge_sram_storage[storage_bytes]` are R |
| `cartridge_sram_control` | R | mapper register `$A130F1` |
| `cartridge_eeprom` (9 bytes) | R | line + protocol phase, bit counter, address, buffer; the 128-byte array is in `cartridge_sram_storage` |
| `devices.z80_bus` (`bus_requested`, `bus_granted`, `reset_released`, `bank`, `z80_ram[8192]`) | R | |
| `devices.vdp`: `registers[]`, control-port latches, `addressed_pointer`, `auto_increment_value`, `status_register`, `data_port_transfer_code[_valid]`, `vram[65536]`, `cram`, `vsram`, `dma` (phase, kind, source, remaining, fill count, transfer count, target code) | R | DMA in progress is captured with its remaining length |
| `devices.psg` (`write_count`, `write_digest`) | R | evidence counters, kept so digests stay comparable |
| `devices.controller_io` (`data[3]`, `ctrl[3]`) | R | |
| `devices.interrupt` (`vblank_pending`, counters, checkpoint flags) | R | |
| `scheduler.master_ticks` | R | guest clock |
| `raster` (`initialized`, `last_line`, `next_line_tick`, `hint_counter`, `hint_pending`, `hint_strict`, `hint_undelivered`) | R | |
| `virtual_frame_count` | R | guest frame id; also the pacing authority |
| `exception_frame_irq6_origin[4096]` | R | |
| `irq6_*`, `divide_by_zero_*`, `privilege_violation_*`, `software_exception_handler_*`, `irq6_vector_in_work_ram` | B | build-resolved constants written by the generated `main` |
| `m68k_interrupt` (`SegarecompM68kInterruptState`) | R | sampled level, level-7 latch, stopped flag |
| `z80_epoch`: `epoch_count`, `executed`, `accumulate_until_epoch`, `written[1024]` | R | hold-window bitmap decides the next image activation; `on_epoch`/`context` are B |
| `z80_synced_ticks`, `z80_sync_quantum` | R | |
| `z80_hooks`, `audio_hooks` | B | |
| `live_frame_observer`, `live_raster`, `execution_coverage` | B/X | host observers; never serialized |
| `execution_history`, `m68k_checkpoint`, `device_checkpoint` | X | bounded diagnostics (ADR 0040/0042); not guest semantics. A restored session starts them from the live values |

### 3.2 Z80 machine (`z80_machine.h`, `Z80State`)

| Field(s) | Class | Notes |
| --- | --- | --- |
| `cpu.state` (all registers incl. shadow set, `ix/iy/sp/pc/wz`, `i/r/im/iff1/iff2/q`, `halted`, `int_deferral`, `ld_a_ir`, `in_prefix_run`, `int_line`, `nmi_pending`, `nmi_reject`, `cycles`) | R | `deadline` is transient (set by `z80_run`); stored as 0 and re-armed by the next `run_to` |
| `cpu.host`, `cpu.outcome`, `live_code`, `observe_pc/observer` | B/X | callbacks rebind; `live_code` is per-instruction scratch |
| `bound_ordinal`, `last_bound` | R by **identity** | serialized as the SHA-256 content hash of the image (`genesis_z80_image_content_hashes`), 0 = none; restore maps the hash back to the current ordinal and refuses when the registry no longer has the image. The mutable-code guard stays active: RAM-backed images are validated against the live RAM on every entry |
| `cycle_base_master_ticks` | R | |
| `view_stop` | R | must be 0 at capture (a latched stop refuses capture) |
| `sound_fault`, `sound_fault_epoch`, `sound_fault_master_ticks`, `sound_recoveries` | R | |
| `on_unknown_image`, `unknown_image_context`, `bank_view`, `quiesce_unknown_image`, `isolate_view_stops`, `rearm_faulted_on_epoch` | B | configuration of the initialized machine |
| `count_*` | X | opt-in aggregate evidence, not guest-visible |

### 3.3 Audio (`genesis_audio.h`, `genesis_mixer.h`, ym2612, sn76489)

| Field(s) | Class | Notes |
| --- | --- | --- |
| `Sn76489 psg`: periods, counters, outputs, attenuation, noise control/counter/flip, `lfsr`, `latch`, `ticks` | R | `config` (taps/width/divider/levels pointer) is B and verified |
| YM2612 internal state | R | `ymfm save_restore` blob (registers, phase, envelope, LFO, SSG-EG, DAC) |
| YM2612 wrapper: `now`, `next_sample_clock`, `timer_expiry[2]`, `busy_end`, `samples` | R | restored explicitly; chip phase is **never** derived by replaying register writes. `sink` is B |
| `ym_latched[2]` | R | the address latches of the two ports |
| `psg_writes`, `ym_writes`, `psg_data_before_latch`, `ym_class_*`, `ym_samples`, `ym_sample_fnv`, `trace_*`, `ym_trace_*` | R (counters) / X (traces) | the counters and the running FNV digest are restored so the audio determinism evidence of a restored run equals the straight run; the bounded traces are diagnostics and are cleared |
| `GenesisMixer`: both `GenesisMixerSource` records (window, window_end, watermark, `sum`, `count`, `last`, FIFO head/count/contents), `frames`, `clipped`, `changes`, `min/max/prev`, `have_prev`, `fault` | R | guest-time mixer state; the partially accumulated windows and the FIFOs must be restored or the stream diverges at the first frame |
| `mixer.sha`, `range_sha`, `range_*` | R | the stream digest is part of the contract (equal digest == equal samples) |
| `mixer.ring*` (consumer ring) | R as empty / host | the host-only playback ring is **cleared** on restore (`ring_read = ring_write`); frames already produced before the snapshot belong to the abandoned timeline and must not be heard again. The mixer's guest-time state is exact |
| `mixer.sink`, `sink_context`, `GenesisAudioPresenter` (host queue, underrun counters) | B / host | the presenter is reset by the viewer (section 7) |

### 3.4 Not in the snapshot

SDL window, renderer, textures, audio device, `GenesisFrameArtifact` published frames, `GenesisRasterCapture` (per-line capture,
VRAM journal), `GenesisFrameExtension` surface, the sdl viewer state and every `SvHost` field. The raster capture holds the lines
of the **current** frame observed before the barrier; after a restore it is reset (`captured[] = 0`, journal off) and the next frame
inherits per-line state exactly as the first frame of any session does. To avoid showing that partial frame, the viewer renders one
baseline frame from the restored VDP state with the single-snapshot producer and presents it immediately (section 7).

### 3.5 Native patch providers

The native provider ABI (`segarecomp_genesis_patch.h`, ADR 0267) is read-only by construction: it receives copies of work RAM, VRAM and
line state and returns presentation decisions; it has no API to write guest memory, control a device or reach the host. Its private
state therefore cannot affect future guest execution, and a state captured with a provider attached restores identically with or
without it. The *presentation* of the restored frame is recomputed: the provider is called on the first published frame after the
restore, and a thumbnail never claims that the enhanced extension is available. If a future ABI version gives a provider guest-visible
effect, the build must refuse state features (the viewer capability probe reads `GENESIS_NATIVE_COMPAT_ABI`). No incomplete composite is
ever restored silently.

## 4. Identity, versioning, integrity

### 4.1 Container (`SGSTATE`, all integers little-endian)

```
offset  size  field
0       8     magic "SGSTATE\0"
8       2     container_version = 1           (unknown -> refused)
10      2     state_schema = 1                (the semantic layout of the sections; a bump invalidates older states)
12      4     header_bytes
16      2     platform = 1 (Genesis)
18      2     machine_flags: bit0 sound present, bit1 SRAM present, bit2 EEPROM kind
20      4     region_profile
24      32    source_sha256   (effective cartridge/composite image digest, SEGARECOMP_ROM_SHA256)
56      32    variant_sha256  (patch identity; all zero = vanilla)
88      32    exec_fingerprint (section 4.2)
120     8     guest_frame     (virtual_frame_count)
128     8     guest_master_ticks
136     4     section_count
140     4     payload_bytes
144     2     thumb_w, 2 thumb_h, 4 thumb_bytes   (RGB565 LE, row-major)
...           section table: { u16 id, u16 flags (bit0 = required), u32 offset, u32 length } * section_count
...           thumbnail bytes, then section payloads
end-32  32    SHA-256 over every preceding byte
```

Sections: `CPU` (M68K regs, SR, PC, interrupt state), `RAM` (work RAM), `VDP`, `IO` (controller I/O, Z80 bus, interrupt, scheduler, raster,
epoch, IRQ6 origin bitmap), `CART` (SRAM bytes, mapper control, EEPROM protocol), `Z80`, `PSG`, `YM`, `MIXER`. Required sections are all of
them that the machine profile announces; an unknown *required* section, a missing required section, a duplicated section, a length that
exceeds its fixed maximum or leaves the file, a section overlap and a trailing byte are refusals.

### 4.2 Execution compatibility fingerprint

`SHA-256("segarecomp.genesis.sgstate.exec.v1" || state_schema || sizes of the serialized memories || sound flag || mixer constants)`.
It is independent of the build directory, compiler, compiler path and emitter version. A rebuild of an equivalent program therefore
keeps its states usable; a generated program whose PC is no longer covered stops fail-closed (a typed stop), never corrupts state. The
**Z80 image identity is not in the fingerprint**: the bound images are referenced by content hash (section 3.2) and a restore into a build
that lacks one of them is refused with a reason.

### 4.3 Limits

Total file <= 4 MiB; thumbnail <= 160x112 RGB565; per-section maxima are constants next to the parser (RAM 64 KiB, VDP < 80 KiB, Z80 <
16 KiB, YM2612 blob <= 16 KiB, MIXER < 128 KiB, CART <= 64 KiB + 16). Read through a bounded reader; allocation sizes come from constants,
never from file lengths.

### 4.4 Integrity is not authenticity

The digest detects corruption and truncation. It does not authenticate the file: a player can craft a valid file for their own machine,
exactly as they can edit a battery save. The parser therefore validates every value that indexes memory (DMA kinds, phases, EEPROM phase, ring
indices, FIFO counts, timer values, YM2612 blob size) before the commit phase. The ymfm blob is bounded by size; values inside it are
ymfm's own and cannot write outside the chip object.

## 5. Cartridge persistence

### 5.1 Formats

- `cartridge.srm`: exactly `storage_bytes` dense bytes of `cartridge_sram_storage` (the existing odd/even lane layout of the descriptor, no
  wrapper). Fresh: the deterministic `0xFF` fill from `genesis_cartridge_sram_install`.
- `cartridge.eep`: exactly the 128-byte X24C01 array (also `cartridge_sram_storage`). Protocol lines, phase and counter are **not** persisted:
  a new session starts idle. They are part of save states and rewind snapshots.
- Unsupported (`layout_supported == 0`) layouts have no file and no feature.

### 5.2 Lifecycle (ADR 0252 pattern, adapted to the viewer process)

1. The launcher prepares `<dir>/cartridge.srm|eep` (validated existing file or deterministic initial fill) and passes the directory in
   `SEGARECOMP_SAVE_DIR`. A stale `*.next` candidate is deleted.
2. The viewer hook (`viewer_main_hook.c`), after the generated `main` installed the cartridge memory and **before the first guest
   instruction**, loads the canonical file into `cartridge_sram_storage`. An invalid file (wrong length, unreadable, not a regular file,
   a symlink to something else) is a start failure: the process exits with a typed host status before any guest instruction and never
   touches the file.
3. On a **clean player end** (`GENESIS_VIEWER_WINDOW_CLOSED`: Quit, Return to Launcher, window close, scripted `Q`), the hook writes a candidate
   `<name>.next` (same directory, flushed, exact length). Nothing else writes a candidate: a typed guest stop, an exhausted runner budget,
   a host error, a crash and a kill leave no candidate.
4. The launcher validates the candidate (regular file, exact length), keeps `<name>.prev` (best effort copy of the old generation) and
   replaces the canonical file with one `rename`. A missing/invalid candidate or a failed rename keeps the previous save and tells the user.
   Durability: the candidate is flushed to the OS before the rename; `fsync`-level power-loss durability is **not** claimed.
5. Replays, recordings and the build/materialization pass never receive `SEGARECOMP_SAVE_DIR` (it is explicitly unset in their environment),
   so they run on the scratch fill and can neither import nor modify the player's data.

### 5.3 Exit distinction (finding)

Every Genesis end -- clean window close, exhausted budget, typed stop -- prints a report and exits **0** (`genesis_write_sanitized_report`
returns 0 on any well-formed report), and `WINDOW_CLOSED` and `RUNNER_EXHAUSTED` both return `GENESIS_RUNNER_RESOURCE_LIMIT`. The exit
status cannot distinguish them. Rather than change the report ABI, the clean-end signal is the **candidate file itself** written only by the
viewer on `WINDOW_CLOSED`, and `VIEWER_SUMMARY.outcome` already names the cause. A resource-limit exit is therefore never treated as a
successful save. Tests prove the three negative cases.

Generated programs need no new argv flags: the generated `main` argument parser (argc 1/3/5/7 pairs) is unchanged, so legacy Genesis
execution without `SEGARECOMP_SAVE_DIR` is byte-for-byte the previous behaviour.

## 6. Storage layout and manual slots

```
<library>/saves/genesis/<original-cartridge-sha256>/
   vanilla/                    cartridge.srm  cartridge.eep  states/{quick,slot-1..slot-5}.sgstate
   patches/<patch-identity>/   cartridge.srm  cartridge.eep  states/...
```

`<library>` is the existing library root (`library_root()`, `SEGARECOMP_LIBRARY_DIR`). Identities are full 64-hex digests, validated before
use; a malformed identity yields no path. The tree is outside `<cache>/games/<build hash>`, so build cleanup, re-import, cache eviction and
rebuilds never touch it. Storage accounting lists it as user data. Vanilla and patched variants are isolated by default.

State files are written to `<name>.tmp` in the same directory, validated by re-parsing, flushed, then renamed over the slot; the old valid
file survives any failure. A state is never auto-loaded at boot.

## 7. Rewind

- Rolling full snapshots (section 4 container without file IO) every 500 ms of **guest** time (`guest_master_ticks`), captured at the section 2
  barrier; 60 s target; **96 MiB** hard budget for snapshots + thumbnails; oldest-first eviction by age and by budget; no worker thread;
  no disk; allocations are bounded by the budget and made at capture time, failure marks rewind unavailable with a reason.
- Entering rewind freezes guest execution (it runs inside `window_closed()`), captures/references a current-state anchor, and previews
  thumbnails/frames only. Cancel discards the selection and resumes untouched.
- Confirm: validate the whole snapshot, restore atomically, then **sink reset**: clear host audio queue and reset the presenter, resync the
  pacer (`genesis_pacer_init`), reset the raster capture and present the baseline frame, release host input latches, drop every entry newer than the selected one,
  re-arm the capture cadence at the restored guest time. The resumed future is a new history branch.
- Failed restoration stays in the rewind UI and reports the reason; the machine is unchanged (two-phase restore).

## 8. Viewer capability interface

`libs/viewer` gets a small provider table (no machine state): availability + reason, history count/metadata/thumbnail, `restore_entry`,
slot `save/load/metadata`, `on_cancel`. A platform without whole-machine state (Master System, Sega CD) supplies none: the pause menu
shows disabled rows with an accurate reason. Reserved host shortcuts: `F3` performance overlay, `F11`/`Alt+Enter` fullscreen, `F12`
screenshot, `Esc`/menu key. New: `F5` quick save, `F8` quick load, `F7` rewind. Game input actions are never reused.

## 9. Checkpoint map

A contract (this file) - B `genesis_snapshot.[ch]`, YM2612/PSG/mixer save-load, tests - C `genesis_persistence`, launcher storage lifecycle -
D slots + launcher plumbing - E rewind engine - F viewer provider + UI - G integration + performance - H documentation, ADR 0278, review.
