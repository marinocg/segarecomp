# Genesis raster state, display modes, H-interrupt and console region (SEG-055)

Status: implemented by SEG-055-T001 (one combined product PR). Decision record: `docs/decisions/0115-genesis-scanline-raster-display-modes-and-region.md`.

This contract describes the generic Mega Drive/Genesis video, interrupt and region behavior added on top of the existing T049 renderer
(`platforms/genesis/runtime/vdp_render.{h,c}`), the deterministic scheduler (`runtime.c`, ADR 0020/0041) and the viewer
(`platforms/genesis/viewer/`). Nothing here is specific to a game, title, address or ROM. There is still no interpreter, JIT or runtime
opcode decoding, and no second graphics engine: one renderer owns all composition.

## 1. Capability matrix (before / after this PR)

| Area | Before | After |
|---|---|---|
| Frame source | one snapshot of the *final* VRAM/CRAM/VSRAM/registers at the VBlank onset | per-field-line register file + CRAM + VSRAM (256 B/line) captured as each active line begins, plus VRAM at the frame boundary |
| Mid-frame palette / scroll / mode-register changes | lost (last value wins for the whole frame) | visible from the following line on; frames carry one palette slot per distinct line palette |
| VRAM written during the active display (split-screen sprite tables, HUD swaps) | lost (only the frame-end VRAM was rendered) | journaled per line (line-0 snapshot + bounded line-tagged byte writes) and replayed as rows advance; an overflowing journal falls back to the frame-end image for that frame (counted) |
| H-interrupt (IRQ4, vector 28) | never raised; register #0 bit 4 ignored; vector 28 handler never rooted | counter + latch + level-4 admission; vector 28 resolved/rooted like the other build-time vectors; STOP can wake on it |
| VBlank (IRQ6) | unchanged | unchanged (same boundary, same admission, same timing); level 6 outranks level 4 |
| Display width | H40 (320) only; everything else rejected | H40 (320) and H32 (256) |
| Interlace | rejected | interlace mode 2 (448 rows, 8x16 patterns); mode 1 / reserved stay rejected |
| Horizontal scroll modes | full, line (cell and `01` rejected) | full (`00`), cell (`10`), line (`11`); prohibited `01` stays rejected |
| Vertical scroll | full, per 2-cell column | unchanged; 11-bit values in interlace mode 2 |
| Window plane | not rendered | rendered (registers #3/#17/#18, A substitution, priority) |
| Sprite per-line limits | none | 20 sprites / 40 cells per line (H40), 16 / 32 (H32); 80 vs 64 table entries |
| Left-column mask (reg #0 bit 5) | ignored | backdrop color in the leftmost 8 pixels |
| Frame artifact | fixed 320x224, one palette | carries `width`, `height`, per-row palette slots; zero-initialized == the legacy artifact; legacy digest unchanged |
| SDL presentation / PPM export | fixed 320x224, final palette | geometry- and per-row-palette aware; both use one conversion (`genesis_vdp_frame_row_rgb888`) |
| Viewer pacing | coupled to published frames | per deterministic *virtual frame boundary* (`GenesisRuntime::virtual_frame_count`) |
| Controllers | Player 1 only; port 2 always released | independent Player 1 / Player 2 masks and keymaps |
| Version register ($A10001) | fixed constant 0xA0, folded at build time | the configured console region (default USA NTSC = 0xA0), read at run time |
| Region selection | none | `usa` / `japan` / `europe` profiles; `SEGARECOMP_GENESIS_REGION`, `genesis_runtime_set_region`, bridge `--region`; PAL representable but refused |

Still unsupported (fail closed or documented as ignored): shadow/highlight, the sprite X = 0 mask, display-disable, sprite collision/overflow
status flags, sub-line timing of VRAM/CRAM writes (a change is visible from the next line), 30-cell (V30) height, interlace mode 1,
the reserved `RS0/RS1` and `LSM` combinations, horizontal-scroll mode `01`, 50 Hz/PAL timing, the HV counter. (The VDP status register's HBlank flag, bit 2, and the interlace odd-field flag, bit 4, are modelled
as pure functions of guest time: HBlank = master ticks 228-872 (H40) / 280-860 (H32) into the line, odd field alternates per frame while `LSM0` is set.)

## 2. Raster model

The NTSC schedule is unchanged: a scanline is 3420 master ticks, a frame is 262 lines (`GENESIS_NTSC_MASTER_TICKS_PER_FRAME`), the active display is
lines 0-223 and the VBlank onset is the start of line 224. Guest time is the integer master clock advanced only by retired instructions and
exception processing; no host time is read.

`GenesisRasterScheduler` (runtime-private, never evidence) tracks the last scanline start the schedule has processed. At the first retirement boundary at or after a
line's start tick the runtime "enters" the line:

1. line 0 clears the per-frame capture flags;
2. an active line (0-223) captures the register file, CRAM and VSRAM into `raster.frame.lines[L]` (only when a host installed a scanline-aware
   producer, `GenesisRuntime::live_raster_producer`), then steps the H-interrupt counter;
3. line 224 latches the H-interrupt once more if the counter is zero;
4. the last line (261) reloads the counter from register #10.

VRAM writes made while the active display is in progress (data-port writes, memory-to-VRAM DMA, VRAM fill) are journaled the same way: line 0's entry snapshots the VRAM, and every byte
written during lines 0-223 is appended with the line it was written on (capacity 8192 bytes per frame; a larger burst abandons the journal for that frame -- the frame-end image is then used
and `GenesisRasterCapture::vram_journal_overflows` counts it). The renderer replays the journal as rows advance, so a handler that DMAs a different sprite attribute table in the middle of the screen
(two-player split screens) gives each half its own table. Writes during VBlank are not journaled: the next line-0 snapshot contains them.

Consequently a write made while line `L` is in progress (for example by an H-interrupt handler) is first visible on line `L + 1`, which is the
granularity at which the interrupt scheduler already samples every other source (the state after the instruction that straddles the boundary).
Interlace mode 2 does not change this: H-interrupts and scroll-table rows stay at field-line (low) resolution; output rows `2L` and `2L + 1` share line `L`'s state.

### 2.1 H-interrupt counter (level 4, vector 28)

For each active line: if the counter is zero it is reloaded from register #10 and the request is latched, otherwise it is decremented. With register #10 = `N` the request therefore
fires on lines `N, 2N + 1, 3N + 2, ...` of each frame (every `N + 1` lines); `N = 0` fires on every active line. The transition into line 224 latches a request when the
counter is zero (so `N = 0` yields 225 requests per frame; `N = 224` latches exactly at the VBlank onset). The request stays latched until the level-4 interrupt is taken, whether or not
it is enabled; it is delivered when register #0 bit 4 is set and the interrupt mask is below 4. VBlank (level 6) has priority; after the 68000 returns from the IRQ6 handler a pending IRQ4 is
taken at the next retirement boundary. Entry goes through the M68K-owned interrupt core exactly like IRQ6 (vector 28, six-byte frame, mask raised to 4, 44 cycles of exception processing).

The handler entry is resolved at build time from the cartridge vector table at offset `0x70`, in the same bounded static-discovery machinery as vectors 4-11 and 32-47
(`software_exception_handler_*[28]`), with three H-interrupt-specific rules that keep the root additive and never a new build failure:

* it is discovered with a **smaller bounded budget (96 instructions, call frames not followed)**: the handler's own straight-line body is compiled and a call or tail beyond the bound is a typed prefix-boundary frontier (fail closed if ever executed).
  Game-specific update routines called from a handler frequently reach code that no other root reaches, which the aggregated authoritative closure cannot represent (Sonic 1's HBlank tail is the measured example);
* a handler that is a single unresolved instruction (typically `JMP` through a RAM stub written at run time, e.g. Sonic 3) is not installed;
* the whole general-startup analysis is attempted with the root and, if that attempt is rejected for any reason, again without it (exactly the earlier analysis). Across the 17 locally available images no analysis was rejected with the root, and the root was
  installed for 11 of them (Sonic 1, Sonic 2 (two images), Golden Axe, Moonwalker, Cool Spot, Phantasy Star II, Land Stalker, Shining Force, Streets of Rage, Streets of Rage 2).

A vector slot that points into work RAM (the common RAM jump-table convention) retains the stub address (`hint_vector_in_work_ram`) so that the existing build-time preparation (`--discover-copy-aliases`: byte-guarded JMP thunks and verbatim
immutable-copy aliases, ADR 0049/0097) can turn it into an ordinary compiled handler entry guarded by the runtime byte-identity check. **RAM-resident code that the game constructs at run time (OutRun's H-interrupt routine is written into work RAM
instruction by instruction, it is not a copy of cartridge bytes) can never be compiled by this architecture** (no interpreter/JIT).

When an enabled H-interrupt fires with no resolved handler the default compatibility policy **counts and drops it** (`raster.hint_undelivered`; the capture/viewer hooks print a `RUNTIME_NOTES {...}` line when anything was dropped, a non-default region was used, frames were
rejected or a VRAM journal overflowed) -- precisely what every earlier build did with the interrupt, now observable -- so titles that never needed the handler keep running. `SEGARECOMP_GENESIS_HINT=strict` (or `genesis_runtime_set_hint_strict`) makes it stop fail-closed with
`unsupported_interrupt_or_scheduling_event` / `hint_handler_not_installed` instead. The choice is fixed before the first step.

A `STOP` whose loaded mask is below 4 can be woken by an H-interrupt that can actually be delivered (handler installed, or strict policy): virtual time then advances one scanline at a time (through the same scheduler) until a request is accepted. A `STOP` whose mask blocks level 4 keeps the single jump to the next VBlank onset with the exact pre-existing timing.

Evidence for the counter rules: Genesis Plus GX `core/system.c` `system_frame_gen` (public, used only as a behavioral reference; no code is copied): counter reload at the last line, per-line "zero → reload and request, else decrement" during the active display, and the request check at the start of the VBlank line. The IRQ4 priority and masking follow the MC68000 autovector rules already implemented by the CPU-owned interrupt core.
Limitation: real-hardware sub-line timing (the H counter within a line) is not modelled; the request is latched at line granularity.

## 3. Frame artifact (`GenesisFrameArtifact`)

`pixels` holds `width * height` CRAM indices (stride = width); `palette_snapshot` is palette slot 0; `row_palette[row]` selects a slot, slot `k >= 1` living in `extra_palettes[k - 1]`; `palette_count` counts the slots. A zero-initialized
artifact is the legacy 320x224 single-palette artifact. Slots are deduplicated by content, so an ordinary frame has exactly one.

`frame_digest` is `SHA-256(pixels[0, w*h) || palette_snapshot)` for a legacy-shaped artifact (320x224, one palette) -- exactly the pre-SEG-055 digest, so existing oracles, goldens and evidence remain valid -- and
`SHA-256(pixels || palette_snapshot || be16 w || be16 h || be16 palette_count || row_palette[h] || extra_palettes[(count-1) * 128])` for every other shape, so two frames that differ only in resolution or in
a mid-frame palette switch can never share a digest. The checkpoint-evidence bundle digest hashes `width * height` pixel bytes (identical to before for legacy frames); the independent checkpoint oracle uses the same rule.
The artifact is a pure function of its inputs: bytes beyond the active geometry are zeroed.

A render fails closed (nothing written, nothing published) for: Mode 4, V30, reserved RS0/RS1, interlace mode 1 or reserved LSM, a reserved plane size, scroll mode `01`, **or a geometry change between lines of the same frame** (a mid-frame H32/H40 or interlace
switch cannot be one rectangular picture). The runtime counts such rejections (`live_render_rejections`) and keeps advancing virtual frame time.

## 4. Renderer behavior (generic hardware rules)

Hardware references are collected in section 9. Every behavior below has synthetic fixtures with hand-derived expectations in `tests/genesis_raster_display_modes_test.cpp`.

* **Geometry.** H32: 256x224 (32 cells); H40: 320x224 (40 cells); interlace mode 2 (`LSM1:LSM0 = 11`): 448 rows, 8x16 patterns (64 bytes, rows consecutive; VRAM byte address = tile * 64, wrapping in 64 KiB), cell row = row / 16.
* **Horizontal scroll.** Table at register #13 bits 5-0 << 10, 4 bytes per entry (A word, B word), sign-extended 10 bits, subtracted from x, wrapping at the plane width. Mode `00`: entry 0; `10`: entry of the group's first line (`line & ~7`); `11`: the line's entry. The row index is the *field* line (`row / 2` in interlace mode 2). `01` ("8 values repeated", undocumented) stays rejected.
* **Vertical scroll.** One value per plane or per 2-cell column group; 10 bits (11 in interlace mode 2, in output rows), added to the row, wrapping at the plane height.
* **Window.** Replaces Plane A wherever it applies (a transparent window pixel shows Plane B/backdrop, not Plane A); never scrolls; nametable at register #3 << 10 (bit 1 ignored in H40: 4 KiB aligned), row stride = display width in cells (64/32). Register #17: horizontal boundary in 16-pixel units, bit 7 = window on the right of it; register #18: vertical boundary in cell rows (8 output rows, 16 in interlace mode 2), bit 7 = below it. A pixel is in the window if it is inside the vertical range **or** the horizontal range. Priority bits compete as Plane A's.
* **Sprites.** SAT at register #5 << 9 (H40: bit 0 ignored, 1 KiB aligned, 80 entries; H32: 512-byte aligned, 64 entries). Per line the first 20 (H40) / 16 (H32) sprites of the list that cover the line are used (the rest are dropped), and their cells draw until the 40 / 32 cell budget is exhausted (the sprite that crosses it is clipped on the right). First opaque pixel in list order wins and its priority bit is used. Interlace mode 2: the Y field is 10 bits with a +256 bias, cells are 16 rows tall, flips apply to the whole bounding box. Column-major multi-cell tile order and 11-bit tile-number wrapping are unchanged.
* **Composition.** Sprite-high, A/window-high, B-high, sprite-low, A/window-low, B-low, backdrop (register #7). Register #0 bit 5 replaces the leftmost 8 pixels with the backdrop.

## 5. Console region

`controller_io_contract.h` defines `SegarecompGenesisRegionProfile` and the version-register encodings (bit 7 overseas, bit 6 PAL, bit 5 no expansion unit):

| Profile | Version register | Operational |
|---|---|---|
| USA NTSC (overseas, 60 Hz) -- the default, id 0 | `0xA0` | yes |
| Japan NTSC (domestic, 60 Hz), id 1 | `0x20` | yes |
| Europe PAL (overseas, 50 Hz), id 2 | `0xE0` | **no**: representable, refused at run time (50 Hz timing is not implemented) |

The value is a property of the *configured console*: no cartridge byte is read or altered, no PC/fingerprint check exists, and the game's own compatibility logic is never bypassed. The static seam routes the `$A10001` BYTE read to the runtime device owner (like DATA1/DATA2),
so one generated program serves every profile and no region constant is baked into generated code (the existing host-side policy table keeps its USA value for analysis-only callers).

Selection is made once, before the first guest step, and is then immutable for the run: `genesis_runtime_set_region` (host API) outranks the `SEGARECOMP_GENESIS_REGION` environment variable read by `genesis_runtime_run`
(`usa`/`us`/`ntsc-u`, `japan`/`jp`/`ntsc-j`, `europe`/`eu`/`pal`, `auto` = the default). A zero-initialized runtime is the historical USA NTSC console, so every existing title keeps its previous behavior by default.
`tools/genesis_startup_bridge.py --region {usa,japan,europe,auto}` forwards the choice to the generated program's environment, records it in the capture/viewer summaries, and `auto` resolves a *hint* from the ROM header's region-support field
(prefer an overseas NTSC declaration, else a Japanese one; an explicit `--region` always wins). Header metadata is never authority for the emulated console. A PAL profile ends the run with `unsupported_region_timing` before any instruction executes. An unknown name fails closed the same way.

### Sonic 3 (Japan) -- investigation summary

The public `sonicretro/skdisasm` source (`s3.asm`) contains the standard Sega lockout at reset: it reads the hardware version register, takes bits 7-6 as a territory id (`0` = 'J' domestic NTSC, `2` = 'U' overseas NTSC, `3` = 'E' overseas PAL; `1` is "tampering") and
compares the resulting letter with the 16-byte country field of its own ROM header; on a mismatch it draws the "DEVELOPED FOR USE ONLY WITH NTSC MEGA DRIVE ... SYSTEMS" screen and loops forever. The local Japan/Korea image declares a domestic-only country field, so the hard-coded USA profile (`0xA0` => 'U') can never match it, while the Japan NTSC profile (`0x20` => 'J') does.
The cause is therefore the configured console region (version register), **not** 60 Hz video timing. No ROM edit, game fingerprint or lockout bypass is involved. (Hypothesis from the task confirmed from the public source and the local header; see the task evidence for the authorized local run outcome.)

## 6. Controllers

`runtime->pad1` / `runtime->pad2` are independent host input masks (`GENESIS_PAD_*`), read through the three-button TH protocol on DATA1 ($A10003) / DATA2 ($A10005). The SDL viewer maps Player 1 to the arrows + Z/X/C + Return and Player 2 to I/K/J/L + U/O/P + Right Shift
(`GenesisViewerHost::pad2`). Six-button controllers, multitaps and serial ports are out of scope.

## 7. Viewer pacing

`genesis_viewer_run` waits once per virtual frame boundary the guest crosses (`GenesisRuntime::virtual_frame_count`, advanced at every VBlank onset regardless of rendering) and presents a frame only when a new one was published. A frame the renderer rejects (unsupported display mode, mode switch mid-frame) therefore costs its period and can never let
the guest run ahead of real time; `--viewer-unthrottled` still skips only the sleeping; guest results are independent of host timing and slicing (tested).

## 8. Verification

* `tests/genesis_raster_display_modes_test.cpp`: scroll modes (full/cell/line, boundaries, negative/wrapping offsets, mixed modes within one frame), vertical modes, H32/H40 geometry and fail-closed combinations, sprite tables and limits, interlace mode 2, Window, left mask, per-line palettes/VSRAM/registers, digest rules, PPM export vs the shared conversion, runtime scanline capture and virtual-frame/rejection behavior.
* `tests/genesis_hint_region_controller_test.cpp`: H-interrupt schedule, gating, masking, IRQ6/IRQ4 priority, missing handler, STOP wake and timing preservation; region encodings, selection and PAL refusal, a guest region check taking different branches per profile; independent controller ports.
* `tests/genesis_viewer_test.cpp`: pacing decoupled from render success, unthrottled option, host-timing-independent guest state, Player 2 mask delivery.
* Existing renderer/export/evidence suites updated where a previously fail-closed shape became supported (H32, interlace mode 2, scroll mode `10`) and still assert fail-closed for the reserved shapes.

## 9. Hardware references

* Sega, *Genesis Technical Overview* v1.00 (GTO1): display modes, interrupts, sprites, scroll.
* Plutiedev: VDP registers <https://plutiedev.com/vdp-registers>, Scroll <https://plutiedev.com/scroll>, Sprite table <https://plutiedev.com/sprite-table>, Screen resolution (H32/H40/V28/V30, interlace doubles scroll and sprite Y, raster effects stay at low resolution) <https://plutiedev.com/screen-resolution>, Layer priority <https://plutiedev.com/layer-priority>, ROM header (region support) <https://plutiedev.com/rom-header>, Sonic water (mid-screen palette rewrite) <https://plutiedev.com/sonic-water>, FAQ (20 sprites / 320 px per line, 80 % in H32; the window overrides plane A even for blank tiles).
* Genesis Plus GX (`core/system.c`, `core/vdp_render.c`, `core/vdp_ctrl.c`, `core/io_ctrl.c`) as an independent behavioral reference only.
* `sonicretro/skdisasm` `s3.asm` (public): the lockout and version-register use described in section 5.
