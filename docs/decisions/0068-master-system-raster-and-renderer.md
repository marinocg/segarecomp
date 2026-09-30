# ADR 0068: Master System Raster Timing and Deterministic Renderer

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T005
- Related: ADR 0062 (independent references), ADR 0064 (artifacts), ADR 0066 (scheduler), ADR 0067 (VDP),
  `docs/architecture/master-system-machine-contract.md` sections 2, 9.8, 9.9, 14 (U2, U8).

## Decision

1. **One C11 renderer, driven by the VDP line hook** (`platforms/master-system/runtime/sms_render.{h,c}`; no SDL, no host
   dependency, linked into every generated program). Line L (0 .. active-1) is rendered at the scanline event of line L from
   the registers, VRAM and CRAM as that event sees them. With the U2 convention (events at offset 0 of their line) a register
   write executed during line L therefore first reaches line L + 1; a write stamped at instruction-start T reaches the line
   whose event time exceeds T. R9 (vertical scroll) and the frame height (mode) are latched at line 0 of each frame (a change
   during the active display takes effect at the next frame); every other register is read per line. Nothing depends on how a
   run is sliced.
2. **Frame boundary.** Frame *n* is the half-open T interval of the contract; its framebuffer is the active area rendered by the
   scanline events of that frame. A frame completes at its last active line; the completed frame is copied and hashed.
3. **Framebuffer artifact = contract 9.9**: 256 x 192 (or 224) bytes, row-major, each the 6-bit CRAM value. Canonical
   serialization = those bytes; frame hash = SHA-256 of them. RGB888 is the presentation mapping
   (`sms_vdp_cram_to_rgb888`), applied only by viewers (T009).
4. **Sprite flags** reach the status register through `sms_vdp_set_sprite_flags` from the same line; sprite evaluation is skipped
   (no flags) while the display is disabled. An unsupported mode renders nothing (T004 owns the typed stop).
5. **Rules fixed here where the contract left them open** (public-source readings, each covered by the model and tests):
   - 224-line name table base is `((R2 & $0C) << 10) | $700`, 32 rows; 192-line base is `(R2 & $0E) << 10`, 28 visible rows
     with the vertical wrap at 224 (MD-VDP section 8, SP-VREG);
   - the fine-scroll gap (`R8 & 7` leftmost pixels) shows the backdrop (U8: the alternative "sprite #0 pattern data" is not
     adopted; no source gives a rule);
   - sprite collision is evaluated on the drawn sprite pixels before the left-column blank and before the background priority
     mask; the left-column blank and the priority mask only affect the displayed colour;
   - a sprite row is `(line - (Y + 1)) mod 256`, so Y values near 255 wrap onto the top lines; no horizontal wrap.
6. **Validation.** (a) `tests/sms_render_test.py`: seeded and directed state-injection scenarios (per-line register events
   included) rendered by `tests/tools/sms_render_dump.c` against the independent Python model `tests/sms_render_model.py`,
   written from the contract with a different formulation; (b) `tests/sms_render_native_test.py`: four project-authored
   generated-native fixtures (three static scenes, one line-interrupt raster fixture) compared frame by frame using the register
   writes of the VDP trace; (c) nine deliberately wrong model rules (priority, both scroll locks, sprite limit 7/9, left blank,
   terminator, fine gap, live R9) must each be detected. No pinned machine reference renders frames in this task: the
   reference renderer is the independent model; the pinned machine references remain the T004 port/IRQ oracles.

## Consequences

The frame artifacts (`frames.txt`, `framebuffer.bin`, `frame.json`) are written by the headless device unit next to the T004
VDP artifacts. T008 consumes `SmsRenderer`/`SmsFrameRecord`; T009 presents `SmsRenderer::last`.
