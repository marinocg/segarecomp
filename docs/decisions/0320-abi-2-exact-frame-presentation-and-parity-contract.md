# ADR 0320: ABI 2 exact-frame presentation and the ABI 1 parity contract

- Status: accepted (combined delivery F31: SEG-152-T001, SEG-152-T002). Builds on [ADR 0305](0305-live-scheduled-mods-and-native-abi2-providers-in-built-programs.md) (the scheduled provider and its render feature), [0306](0306-virtual-sprite-authoring-transport-and-coverage.md), [0309](0309-abi-2-authored-scenery-window.md), [0313](0313-hardware-piece-commands-independent-provider-contributions-and-two-scenery-windows.md), [0314](0314-a-scenery-plane-left-to-the-generic-extension.md) and [0315](0315-asymmetric-widescreen-viewport.md). Documentation only: this task changes no header, runtime, adapter or test behaviour; the items below are implemented by SEG-152-T003 to T006 and verified by T008.
- Companion evidence: [ABI 2 / ABI 1 parity matrix](../architecture/abi2-abi1-parity.md) and [scenery window sufficiency](../architecture/widescreen-scenery-window-sufficiency.md).
- Scope: ABI 2 (`SEGA_MOD_ABI_MAJOR == 2`) stays. No ABI 3, no second scheduler, renderer, compositor or sprite transport, no interpreter/JIT/runtime decoding, no game-specific product code.

## Context

The three production widescreen providers (Sonic 1, Sonic 2, Sonic 3 & Knuckles) are ABI 1 providers: for the frame the host is producing they verify the VDP raster state of **that frame** against a work-RAM snapshot, decline on mismatch, and draw their HUD from that frame's VRAM and CRAM. An ABI 2 provider cannot, because its `render(N)` callback runs before the frame it would verify exists. The facts, all re-verified against current source (see the parity document for file and line references):

Runtime timeline at the onset that ends guest frame `N` (`runtime.c` ~3050-3070, `mod_scheduler.c` `hook_after_snapshot`, `mod_capture.cpp` `prepare`):

| Step | Event | Frame index | Work RAM visible | VDP data visible |
|---|---|---|---|---|
| 1 | `frame-end(N)` (guest-time phase, runs for every frame) | N | live, staged-write commit | n/a |
| 2 | `++virtual_frame_count` (becomes N + 1) | | | |
| 3 | `genesis_publish_live_frame`: the extension producer runs; ABI 1 `prepare/scenery_word/hud` | produces the raster of **N** | the copy taken at the previous onset (end of N - 1; `compat_ram_valid` needs `stamp + 1 == count`) | frame N's per-line raster state, frame-start VRAM |
| 4 | `genesis_compat_snapshot` | | copy of end of N | |
| 5 | `after_snapshot`: ABI 2 `render(N)` (`ended = count - 1`) | callback frame N | copy of end of N | **none** (`frame.vdp` not grantable; VDP spaces `E_SPACE`) |
| 6 | its output is served by `prepare` only when `visible_frame + 2 == count`, i.e. for the producer run of the **next** onset | serves the raster of **N + 1** | | |

So ABI 1 pairs "RAM of end of N - 1" with "raster N", while ABI 2 pairs "RAM of end of N" (render N) with "raster N + 1": the same one-onset lag, which is correct for state-derived data (camera, counters) and for virtual sprites (digest-bound), but gives `render(N)` nothing of raster N + 1 to verify or to draw from.

## Decision 1: frame timing is an opt-in exact-frame `render`, run at the producer seam (Option A)

A provider that **requires** the capability `frame.vdp` (ABI 2 catalogue id 3) is an *exact-frame provider*. For such a provider:

- the host runs its `render` callback **at the producer seam, once per frame that is produced for presentation**, instead of the ordinary after-snapshot `render`. There is exactly one `render` callback per produced frame and none on frames the producer does not run;
- `host->frame` is the index of the produced frame (`N` in the timeline: the guest frame whose raster the view describes), the same numbering `render` has today ("the frame that just ended");
- work RAM (`read(SEGA_GEN_SPACE_M68K_WORK_RAM, ...)` and `view->work_ram`) is the **copy taken at the previous onset** (end of frame N - 1): byte-identical to what the ABI 1 `prepare` receives for the same frame (the snapshot the game used to prepare picture N);
- an immutable `SegaGenesisFrameView` (Decision 4) carries raster N: frame-start VRAM, the per-line registers, CRAM and VSRAM, the picture size, the planned side;
- everything the callback submits (scenery windows, overlays, virtual sprites, `view_left`, the refusal call) is consumed by the **same producer invocation that called it**: there is no `+ 2` epoch binding and no stale-frame hazard to reason about, because there is no frame between submission and use.

**When the exact-frame callback runs (it is the producer seam, not the widescreen extension, that triggers it).** The trigger is "the extension producer is invoked for this frame and reaches the provider hook with a requested component and a valid RAM copy", exactly the condition under which an ABI 1 `prepare` is called today (`vdp_render.c`; ADR 0315 makes the producer run for a verified-HUD provider even when nothing of the extension is shown). The host's geometry ownership and every earlier behaviour are unchanged. Therefore:

| Situation | Callback runs? | What it sees / what is honoured |
|---|---|---|
| Widescreen with extension columns | yes, once per produced frame | `view->side` = the planned margin; HUD, sprites, scenery windows, `view_left` honoured as today |
| Original | yes, when the producer runs for a verified-HUD provider (`extension_shown` = 0) | `view->side` = 0; HUD and indicator content honoured; scenery windows and `view_left` are ignored (there is no extension to fill or place; the host's geometry is untouched) |
| Exact 4:3 | yes, as Original | the native 4:3 picture, `view->side` = 0, HUD honoured as ADR 0315 specifies; no placement |
| Failed extension production (the producer returns before the provider hook: target too small, invalid plan, native geometry) | no | the frame is generic, as for ABI 1 where `prepare` is not reached |
| Skipped presentation (the producer is not requested for the frame and no verified-HUD provider needs it) | no | nothing is submitted, nothing carries |
| First frame after restore or rewind (no valid previous-onset RAM copy: `compat_ram_valid` false) | no | generic frame, reason `STALE_STATE` (host-originated), as ABI 1 |

"No widescreen extension" is never taken to mean "no callback": the callback is skipped only where ABI 1 would not have called `prepare` either. Tests are in Decision 11 (T003).

Option B (rejected): an additive presentation phase, `SEGA_MOD_PHASE_PRESENT`, delivered through the existing `phase` callback to providers that opt in, with the ordinary `render` unchanged.

| Dimension | Option A: exact-frame `render` at the seam | Option B: additive `present` phase |
|---|---|---|
| Code complexity (files and symbols touched) | `mod_scheduler.c` (grantable mask; `hook_after_snapshot` skips `render` for an exact-frame provider; one new `genesis_mod_scheduler_present` entry that runs `render` with the previous-onset copy), `mod_capture.cpp` (`prepare` calls it, then reads the staged output as the current frame's), header (frame view pointer). No new enum value | all of A **plus** `SegaModPhase` value, `VALID_PHASES`, phase names, the registered-phase mask and its replay witness (`sha_u32(phase_mask)`), documentation of a ninth phase; the provider implements a second callback arm |
| Callback lifecycle compatibility | no new callback; `render` keeps its meaning for every provider that cannot be an exact-frame provider (no existing provider can be: a required `frame.vdp` is refused today) | `render` unchanged; the new phase must be withheld from existing native providers whose mask is fixed to all phases (`mod_capture.cpp` 309) or they would receive an unknown phase |
| Optional-capability behaviour | opt-in is the *required* capability; optional `frame.vdp` is a build error (below): nothing silently changes | the same opt-in rule is needed (a mask or capability must gate delivery); no gain |
| Frame numbering | produced frame N; same numbering as today | produced frame N for `present`, ended frame for `render`: two meanings in one provider |
| Snapshot ownership | host-owned previous-onset copy and the frame view, valid during the call | same |
| Provider state mutation | one callback: the state rules of Decision 3 apply to it | two callbacks, two timelines: the provider must reconcile state written in `render(N)` with `present(N)` |
| Guest-execution determinism | `render` is outside guest time and cannot write guest memory in either option | same |
| Save/load and rewind | nothing pending can survive: submission and consumption are one call; the provider's `state.mod` is the only cross-frame state | a `render`-staged window and a `present` call could straddle a restore |
| Skipped frames | the callback does not run when the producer does not (Original, a skipped frame); no output, generic picture | same |
| Presentation disabled or changed (Original, 4:3, aspect change) | the callback runs only for a produced extension; `view_margin`/`side` reflect that frame; nothing computed for another margin is ever served | same, plus a possible stale `render` window |
| Compatibility with existing ABI 2 providers | bit-for-bit: they are not exact-frame providers | bit-for-bit only if the phase is withheld (above) |
| Capability / ABI-minor negotiation | `frame.vdp` required + the minor negotiation of Decision 10 | the same, plus the phase number |
| Ability to use both ordinary scheduled render and exact-frame presentation in one provider | **no** (a provider is one or the other) | yes |

Why A: every named consumer (the three Sonic providers; the exact-frame HUD, scenery and placement of their adaptive-placement tasks) needs *only* the exact frame, and an exact-frame provider needs no prediction at all. The only thing B buys is mixing the two timelines in one provider, which no consumer needs and which creates the very two-timeline state problem Decision 3 exists to avoid. A new phase is not chosen merely because it is cleaner, and Option A is not chosen merely because the seam exists: it is chosen because it touches fewer symbols, introduces no new callback meaning for any existing provider, and removes the epoch binding for the providers that opt in. If a future consumer needs both timelines, an additive phase can be introduced then (append-only; ABI 2).

Falsification, one paragraph per hazard (each has a named test in Decision 11):

- *Stale output after a skipped callback.* The output is consumed inside the producer invocation that obtained it; if the producer does not run, nothing was submitted and nothing is carried. There is no committed-output slot that outlives the call, so there is nothing to go stale. Falsified by running with presentation toggled on frames: every presented frame's output equals a fresh computation from that frame's view.
- *Restore.* After a load the producer's first invocation sees the restored frame's own raster and RAM copy; the previous-onset copy is invalidated by the existing stamp rule (`compat_ram_valid`), so the callback is not called for a frame without a valid copy (the frame is generic, the same as ABI 1's `STALE_STATE`). Provider state comes only from `state.mod` (Decision 3).
- *Rewind.* Same as restore: the snapshot stamp and the provider state are rewound together; no presentation output exists to rewind.
- *Aspect change.* The view reports the side of *this* frame; a placement and windows are validated against the plan of the frame they are produced with (ADR 0315, unchanged). A provider that computed for another margin is told so by `view->side` and the host never serves a mismatched placement.

Ordering is deterministic: `frame-end(N)` always precedes the exact-frame `render(N)` (the timeline above); a provider that keeps frame-end state must remember that the exact-frame callback runs after `frame-end(N)` but reads the RAM of the end of N - 1 (see Decision 3 for how that state is kept consistent).

## Decision 2: opt-in and optional capabilities (answered explicitly)

1. **How a provider requests exact-frame observation:** by declaring `frame.vdp` in the manifest's required `capabilities`. No new manifest field, callback pointer or phase.
2. **`frame.vdp` declared optional:** rejected at pack and build time with `E_CAPABILITY_INCONSISTENT` ("`frame.vdp` selects the exact-frame callback timing and may only be required"). A provider therefore can never silently run under a different timing depending on a host's grants; an optional declaration never changes behaviour because it cannot exist. (The ABI 1 manifests that already list `frame.vdp` are schema 2 and unaffected.)
3. **Both ordinary scheduled render and exact-frame presentation in one provider:** no. An exact-frame provider's `render` is the exact-frame callback only. A provider that wants the predicted timing does not require `frame.vdp`.
4. **Called twice for one logical frame:** never. At most one `render` per produced frame, none otherwise.
5. **Frame number:** `host->frame` = the produced frame's index (the frame whose raster the view describes); see the timeline.
6. **Non-opting providers:** no existing callback changes meaning for any of them. Golden tests (Decision 11): an unchanged ABI 2 render provider and an unchanged Luau render script produce identical overlay, scenery and sprite digests and callback frame numbers before and after T003.

## Decision 3: state and restore semantics (enforceable)

Contract (each a testable requirement):

- **R1 presentation never affects the guest.** Guest memory, input, hooks, native-picture rendering and audio are identical (guest, native-picture and audio digests) with presentation on, off, skipped on some frames and at different aspects. The exact-frame callback is outside guest time and cannot write guest memory (unchanged).
- **R2 provider state classes.** (i) *Essential cross-frame state* is maintained only in guest-frame phases that run for every frame regardless of presentation (for example `frame-end`) or is a pure function of the exact-frame views of frames that were presented; it is serialized through `state.mod` and is identical after save/load and rewind. (ii) A *pure cache* (a search seed, memoization) may be updated at presentation time but must never change output: output with the cache cleared is byte-identical to output with it warm. (iii) A presentation-time write to essential state is a contract violation.
- **R3 serialization versus invalidation.** The host invalidates, never carries, anything it staged; a provider's own state is whatever `state.mod` saved. A cache is **invalidated** (cleared) on restore, rewind and `reset`, and rebuilt on demand; essential state is **serialized**. Existing `state.mod` providers stay compatible (the state schema is the provider's own).
- **R4 nothing previously supported disappears.** A frame that ABI 1 would present must still be presented after restore or rewind. If a cache is essential to exact output, the provider must specify its lifecycle: preserved through `state.mod`, or reconstructible from the exact-frame view and RAM copy alone.

**Two classes, two different tests (they are not interchangeable).**

- *Pure cache.* Test: output with the cache **cleared before every presented frame** is byte-identical to output with it warm, and clearing it on restore/rewind changes nothing. A cache that passes may be invalidated freely.
- *Essential cross-frame state* (state that decides what a later frame shows and that no single exact-frame view reconstructs). It **cannot** be discarded to "prove" ABI 1 equality: it must be maintained from a phase that runs for every frame (for example `frame-end`) or serialized through `state.mod`, and the test is **uninterrupted-run equality**: the frames presented after a skipped-presentation stretch, after a save/load and after a rewind are byte-identical to the frames of an uninterrupted run, while guest, native-picture and audio digests are identical with presentation on, off and toggled (R1).

Application to the named providers (read from the CURRENT provider code, not comments): Sonic 2 keeps `g_prev_level` and `g_hud_frame` (the HUD label frame held through the restart fade, a value the game last really built) and Sonic 3 & Knuckles keeps `g_prev_kx[l]/g_prev_ky[l]` (registration seeds that feed tie-break scores in `bg_register`). Today they are updated in `prepare`, only on frames where the producer ran, so they already violate R2 (iii) in ABI 1: a run with presentation and a run without can differ. SEG-152-T010 and T011 must **first classify** each such item with evidence (it is a pure cache only if clearing it before every presented frame reproduces the ABI 1 output; otherwise it is essential), then apply the matching test above. The ADR does not prescribe the implementation: derive from the view and RAM copy alone, maintain in `frame-end` from guest state, or serialize in `state.mod` are all acceptable. The acceptance goal stays exact ABI 1 parity; a state whose ABI 1 behaviour is itself presentation-dependent (above) is reported as such, with the chosen deterministic behaviour, for operator approval.

`frame-end(N)` runs before the exact-frame `render(N)` but the latter reads RAM of the end of N - 1: state a provider derives at `frame-end(N)` from the live RAM refers to frame N + 1's picture, not N. A provider keeps at most one extra frame of such state or reads everything from the RAM copy; the host documents this in `mod-execution-phases.md` section 2.

## Decision 4: frame data (the immutable frame view)

A new append-only member of `SegaGenesisFeatures` (the table's `size` grows; older providers read only below their own size): `const SegaGenesisFrameView *frame`, non-NULL **only** inside the exact-frame `render` callback of a provider granted `frame.vdp`, NULL in every other phase and for every other provider. Valid only during the callback; never retained.

```c
typedef struct SegaGenesisFrameLine { uint16_t registers[24]; uint8_t cram[128]; uint8_t vsram[80]; } SegaGenesisFrameLine; /* layout equal to GenesisCompatLine (asserted) */
typedef struct SegaGenesisFrameView {
  uint32_t size;                /* the host's sizeof; the provider reads fields only below it */
  uint32_t width, height;       /* the native picture */
  uint32_t side;                /* the per-side extension margin this frame is produced with; 0 = none (Original, 4:3) */
  uint32_t requested;           /* component bitmask the host asked for: HUD 1, SPRITES 2, SCENERY 4 */
  const uint8_t *work_ram;      /* 65536 bytes, NULL unless frame.work_ram is granted: the same copy read() serves */
  uint32_t flags;               /* SEGA_GEN_FRAME_VRAM_FRAME_START (1): `vram` is the frame-start state (journaled frame); 0: the producer's own end-of-frame VRAM, see below */
  const uint8_t *vram;          /* 65536 bytes, host-owned copy, never NULL when frame.vdp is granted */
  const SegaGenesisFrameLine *lines; /* per-line registers, CRAM, VSRAM */
  uint32_t line_count;          /* <= 224 */
  uint32_t reserved[7];
} SegaGenesisFrameView;
```

- Host-owned copy, **no pointer to live guest memory**: `vram` is a host-owned copy of the frame-start VRAM (`raster->vram_start`) for a journaled frame (see below for a non-journaled one), `lines` at the frame's captured raster lines, `work_ram` at the scheduler's previous-onset copy. Mutating the guest after the callback cannot change them (test in T003). **A frame without a VRAM journal is NOT declined.** Verified by `genesis_native_compat_tests` (`non_journaled_frame`): for such a frame ABI 1 still calls the provider, still delivers its HUD, hands it the producer's own end-of-frame VRAM (not `raster->vram_start`) and withholds only the authored-scenery credit (`GENESIS_EXT_FLAG_NO_VRAM_JOURNAL`, `vdp_render.c`: `cf.vram = journaled ? raster->vram_start : vram`). Declining the callback wholesale would drop HUD frames ABI 1 draws. The contract is therefore: the view's `vram` is always a **host-owned copy** (64 KiB copied once per such frame, never the producer's or the guest's live buffer), and `flags` says honestly which state it is: `SEGA_GEN_FRAME_VRAM_FRAME_START` set = the frame-start copy (journaled frame), clear = the producer's end-of-frame VRAM, **not** a frame-start snapshot (nothing is fabricated: no data exists that could establish one). A provider that needs frame-start state for an identity check declines (`decline`, `BAD_STATE`) when the flag is clear, exactly as the ABI 1 providers already do when their scroll check disagrees; a provider that only draws art from tiles (the HUD) may proceed, as it does under ABI 1. The producer's provenance accounting is unchanged: authored scenery credit and authored rows stay withheld for such a frame whatever the callback submits. `work_ram` and `vram`/`lines` are gated independently: `frame.work_ram` gates the former, `frame.vdp` the rest.
- `read()` in the exact-frame callback: `SEGA_GEN_SPACE_M68K_WORK_RAM` reads the previous-onset copy (the same bytes as `view->work_ram`; the declared-range rules are unchanged). The VDP spaces keep returning `SEGA_MOD_E_SPACE` in every phase: the view is the only VDP access (one access path, not two).
- Per-line data is registers 24 x u16, CRAM 128 B, VSRAM 80 B for up to 224 lines (the existing `GenesisCompatLine`).

## Decision 5: VDP helpers

Typed table members re-export the existing host functions (`genesis_native_compat_host()`): a `SegaGenesisVdpHelpers` pointer appended to `SegaGenesisFeatures` (non-NULL exactly when `frame.vdp` is granted) with `read_hscroll`, `read_vscroll`, `vscroll_is_per_column`, `cram_rgb`, ABI 1 semantics (`0` success). Rejected alternative: native re-decoding of the scroll tables or the CRAM in each provider (a second VDP interpretation that can drift from the host's). Implemented by SEG-152-T004.

## Decision 6: bounded RGBA bitmap

Implemented by SEG-152-T005; the design:

- **One mechanism, no second compositor.** A new `OverlayOp::Kind::bitmap` in the SAME staged `luau::Output::overlay` list, rasterized by the existing `raster_ops` in submission order together with rect/text, delivered through the existing `pack_hud` / `GenesisCompatOverlay` to the one compositor. No new queue, rasterizer or compositor.
- **Ownership.** The host copies the pixels during the call into the staged `Output`; the provider's buffer may be reused at once; nothing points into provider memory.
- **Write rule.** rect and text blend (`blend`, source-over with rounding), which is not byte-preserving for a raw image; a bitmap **replaces** the destination pixels of its clamped, clipped rectangle with its exact RGBA8 bytes (including the RGB of alpha-0 pixels), as the ABI 1 transport carries raw bytes. Later rect/text blend over it with the unchanged rule.
- **Format and call shape.** `overlay_bitmap(x, y, w, h, const uint8_t *rgba, uint32_t stride_bytes)` into the current target (appended to `SegaGenesisRenderApi`): RGBA8, byte order R,G,B,A, straight (non-premultiplied) alpha, `stride_bytes >= 4 * w`, rows top to bottom; arguments out of range, NULL or a short stride are a `range` fault with no partial staging.
- **Budgets (bitmap feature only).** At most 8 requests and 393216 bytes (128 x 256 x 4 for the fixed overlay + 65536 px x 4 for layers) per callback; a bitmap does not consume the 64 rect/text-op budget; a provider that never calls it has exactly the previous accounting.
- **Larger layers without changing shared constants.** ABI 1's layers (up to 256 x 256 inside the 65536-pixel transport) are reached through a **bitmap-specific layer declaration** (`overlay_layer_bitmap`) with its own validation (side <= 256, shared 65536-pixel budget, <= 4 layers); a bitmap layer accepts only bitmap ops and clip and rejects rect/text. **Rejected: a global change of `kOverlayLayerSide` or the op budgets**, which would silently accept previously rejected submissions (a 129-wide `overlay_layer`) and change existing Luau and ABI 2 providers' limits and goldens.
- **Clip and fallback.** `overlay_layer_clip(index, x, y, w, h)` reproduces `GenesisCompatLayer::has_clip`; `overlay_fallback(x, y)` sets the fixed overlay's `fallback_x/y` (today hard-wired to 0 by `pack_hud`); forwarded only when set.
- **Fault and rollback.** Staged with the callback, committed only if it did not fault; a fault discards bitmaps with the other output; the committed image belongs to exactly the frame of the callback (Decision 1: consumed in the same producer invocation).
- **Compatibility.** Luau `sega.overlay.*` unchanged; older ABI 2 providers do not see the appended member; the ABI 1 path is untouched.

## Decision 7: refusal versus fault

Implemented by SEG-152-T006.

- A new non-fault call `decline(uint32_t components, uint32_t reason)` on `SegaGenesisRenderApi` declines the named components (`HUD 1, SPRITES 2, SCENERY 4`). **Provider-selectable reasons** (the ABI 1 provider-owned vocabulary): `UNSUPPORTED_SCENE`, `BAD_SCROLL`, `MISMATCH`, `BAD_STATE`. **Host-originated reasons, never selectable by a provider:** `STALE_STATE` (no valid previous-onset RAM copy), `FAULT` (a callback fault or an unanswered authored cell) and the host-detected frame conditions (extension flags such as `GENESIS_EXT_FLAG_NO_VRAM_JOURNAL`). A provider that passes a host-originated or unknown reason, or an empty or unknown component mask, makes an invalid call: a `range` fault. (Intended narrowing of ABI 1, whose `prepare` could write any number: a provider must not impersonate a failure only the host can observe.) The call returns `SEGA_MOD_OK`, never faults for a valid argument, and is mapped by the adapter onto `GenesisCompatDecision.accepted / reason` and the viewer's extension diagnostic exactly as an ABI 1 decline. Declined components' staged output is discarded (never half-served); calling it twice in a callback is defined: the union of the declined components, the first reason wins. No component declined and no output is the legal "nothing to add".
- Already non-fault today (verified, only documented): no output means not served; a plane without a window is generic (ADR 0314); an unanswered cell inside a window plane is a generic cell plus a failed authored claim (ADR 0313).
- Faults remain: an invalid range, a wrong phase, a capability violation, an over-budget or malformed bitmap. A fault discards the callback's output and disables the mod (`E_MOD_FAULT`). An unsupported scene never disables the mod.

## Decision 8: scenery

Adopt the disposition recorded by SEG-152-T001 **with its scope**, and keep the three claims apart ([sufficiency](../architecture/widescreen-scenery-window-sufficiency.md)):

1. *Geometry sufficiency* holds: two bounded windows (with column slicing) cover every plane cell the measured scenes ask for at 16:9, 21:9 and 32:9, and by the 10-bit-scroll bound any scene up to 1024 px total width.
2. *Answer representability* does not hold in general: the real Sonic 2 provider (Emerald Hill Zone 1) asks the same plane coordinate with a fallback line and an authored line giving different answers in half of its accepted frames, and even an ideal window differs from the ABI 1 frame in up to two pixel rows. The disposition is **`T007_REQUIRED`**, limited to the **per-line fallback class**, with the minimal design **a per-row generic flag per plane**. Sonic 3 & Knuckles Angel Island Zone 1 has zero conflicts in 1032 accepted frames. The origin-multiple and raster-condition classes have synthetic failing fixtures only and are not implemented.
3. *Production coverage*: unresolved gates are recorded: Sonic 1 (the provider did not accept a scene of the available image), Sonic 2 revision 1, Sonic 3 & Knuckles zones other than Angel Island Zone 1. They can widen T007 only on a concrete failing real-provider comparison.

The canonical T007 resolution step (full refinement after F31 merges, **completed before F32 starts**) therefore: records the T001 evidence, promotes T007 from `draft` to `ready` with this measured scope only (the per-line fallback class, the smallest per-row generic mechanism; not implemented in F31), resolves the dependency edges, and runs the harness backlog and control validation. It does not cancel T007. The unresolved gates above are follow-up verification gates, not proof of compatibility.

## Decision 9: ADR 0315 preservation (asymmetric viewport)

The exact-frame mechanism preserves ADR 0315 and reuses `view_margin`, `view_left` and their host validation unchanged. For an exact-frame provider:

- it **learns the planned geometry** from `view->side` (the same per-side margin `view_margin` reports for this very frame, so the two agree by construction);
- it computes a placement for the produced frame and submits `view_left(left_px, margin_px)` and scenery windows built for the same `margin_px` and `left_px`;
- **after an aspect change** a placement or window set computed for another margin is never served: `margin_px` is compared with the margin of the frame being produced (as today); on mismatch the placement AND the windows are withheld and the centred layout is presented; because the callback runs inside the producer invocation that serves it, the margin it saw is the margin used;
- **stale N + 2 assumptions disappear for an exact-frame provider**: the binding of margin and frame is trivial (same call), where for an ordinary provider the existing epoch rule (`visible_frame + 2 == count`, restore/rewind discard) is untouched;
- the existing `render(N)` placement behaviour of a non-opting provider, including the Castle of Illusion package's learn-at-render / build-at-frame-end pattern, is unchanged (bit-for-bit golden: `mod_view_abi2_test.py` and `mod_view_transitions_abi2_test.py` run unmodified);
- no second geometry planner, no camera API, no change to the host's centred fallback.

## Decision 10: version and negotiation (validated against the current implementation)

Facts: the manifest `abi` object accepts only `native` (the major; `only(*a, {"native"}, "abi")`, `mod_package.cpp` 454); `SEGA_MOD_ABI_MINOR` is a header constant copied into `host.abi_minor` (`mod_scheduler.c` 465); `required_minor` is a runtime field of the provider table checked by `sega_mod_negotiate`; the product compiles the package's `native-c11` source against its OWN headers with `-DSEGA_MOD_TARGET_ABI_MAJOR=2`, so a provider that uses a field a given product's header does not declare fails to compile there, not with `E_ABI_MINOR_TOO_NEW`; `features.size` and `platform_features_size` are set by the host.

Decision: **an additive manifest field `abi.minor`** (integer, default 0), plus a header-level guard.

- A manifest that declares `abi.minor = 1` is refused by every product that does not know the field with `E_FIELD_UNKNOWN` (the existing strict `only(...)` check): a clear, stable failure on an older host, with no change to old readers.
- A product that knows the field checks `abi.minor <= SEGA_MOD_ABI_MINOR` (else `E_ABI_MINOR_TOO_NEW`, at pack and at build) and passes `-DSEGA_MOD_TARGET_ABI_MINOR=<minor>` to the compile. The new header exposes the new members (`frame`, `vdp`, bitmap, `decline`) only when `SEGA_MOD_TARGET_ABI_MINOR >= 1`; a source that uses one without declaring the minor fails with a readable `#error` instead of a missing-field error.
- The provider table's `required_minor` stays the runtime negotiation (`E_ABI_MINOR_TOO_NEW` at attach); the two must agree (checked at build).
- Older ABI 2 providers (`abi.minor` absent = 0, smaller `size`) keep working on the extended host; `SEGA_MOD_ABI_MINOR` becomes 1 with SEG-152-T003. `SEGA_MOD_ABI_MAJOR` stays 2; tables stay append-only (the first reserved slots / appended members); existing capability ids and the `E_ABI_MIXED` rules are unchanged; Genesis-only additions stay in the Genesis table; Master System and Sega CD are unaffected (their tables are unchanged; Sega CD reuses the Genesis VDP view only as already documented).
- **Amendment (F32, implemented by SEG-152-T003; for approval).** The sentence "a source that uses one without declaring the minor fails with a readable `#error` instead of a missing-member error" is not implementable as written: a use of a struct member cannot be turned into a preprocessor `#error` without a macro or accessor layer (rejected: a second header API only to change compiler wording). As built, the header omits the minor-1 members when the source targets minor 0, the compiler reports the missing member, and `segarecomp mod build/check` classifies exactly that failure as the stable class `E_ABI_MINOR_UNDECLARED` with a message naming `abi.minor`; the build additionally refuses a package that requires `frame.vdp` without `abi.minor` 1 (`E_CAPABILITY_INCONSISTENT`). The `#error E_ABI_MINOR_TOO_NEW` for a minor above the header's is unchanged.
- **Clarification (F32, Decision 4).** `line_count` counts captured raster/field lines (224 at most), not output pixel rows: in interlace mode 2 `height` is 448, output row `r` reads line `r / 2`, and 224 records lose no state.
- Rejected: relying on `required_minor` alone (does not protect the compile step); a header-only `#error` alone (not visible to the manifest reader or the launcher); a new ABI major (ABI 3 is a non-goal).

## Decision 11: tests that make these decisions enforceable

| Requirement | Named test | Task |
|---|---|---|
| Option A timeline: the provider sees, byte for byte, the RAM, VRAM, CRAM, VSRAM, registers, geometry and side the ABI 1 `prepare` receives for the same frames, including a mid-frame scroll/palette/register change | `genesis_mod_exact_frame_tests` (ABI 1 twin on the same synthetic ROM) | T003 |
| opt-in (`frame.vdp` required) works; optional `frame.vdp` is `E_CAPABILITY_INCONSISTENT`; never called twice per frame; frame number | `genesis_mod_exact_frame_tests`, `mod_exact_frame_abi2_test.py` | T003 |
| existing providers unchanged (non-opting ABI 2 provider and Luau script: identical overlay/scenery/sprite digests and callback frame numbers) | `genesis_mod_exact_frame_tests` golden cases | T003 |
| no-live-pointer rule: views are host-owned copies, mutated guest after the callback does not change them, accessor NULL outside the callback, invalid phase use is `SEGA_MOD_E_PHASE` and a fault | `genesis_mod_exact_frame_tests` immutability case | T003 |
| R1/R3: guest, native and audio digests identical with presentation on, off, skipped and at different aspects; a presentation-time write to essential state is detected | `genesis_mod_exact_frame_tests` | T003 |
| R2/R4, pure cache: cleared before every presented frame equals warm, invalidation on restore changes nothing | `abi_parity_state_test` (cache arm; extends `abi_parity_harness.hpp`) | T008, T010, T011 |
| R2/R4, essential state: frames after skipped presentation, save/load and rewind equal an uninterrupted run | `abi_parity_state_test` (essential arm) | T008, T010, T011 |
| frame exactness: skipped producer call, restore, rewind, aspect change, presentation toggled leave no stale pixels, scenery, placement or sprites | `mod_exact_frame_abi2_test.py` | T003 |
| ADR 0315 preservation (non-opting: identical; opting: placement bound to its frame, windows for another margin never reinterpreted, transitions) | `mod_view_abi2_test.py`, `mod_view_transitions_abi2_test.py` unmodified + `mod_exact_frame_abi2_test.py` | T003, T008 |
| a frame without a VRAM journal: the provider is still called, receives a host-owned copy with `flags` clear, HUD is delivered, authored credit withheld; byte equality with the ABI 1 `prepare` input including that frame | `genesis_mod_exact_frame_tests` (non-journaled case; the ABI 1 behaviour is pinned today by `genesis_native_compat_tests`) | T003 |
| the callback runs exactly where ABI 1 `prepare` would: Original and exact 4:3 with a verified-HUD provider (side 0, HUD honoured, windows and `view_left` ignored, geometry untouched), widescreen, and not on a failed production, a skipped presentation or the first frame after restore/rewind | `genesis_mod_exact_frame_tests` (run matrix), `mod_exact_frame_abi2_test.py` | T003 |
| helpers equal the ABI 1 `GenesisCompatHost` for every scroll mode, both planes, interlace, out of range, NULL | `genesis_mod_vdp_helper_tests` | T004 |
| bitmap pixel equality with ABI 1 `GenesisCompatOverlay` bytes, limits, fault, old providers unchanged | `genesis_mod_bitmap_tests`, `mod_bitmap_abi2_test.py` | T005 |
| refusal is not a fault; reason parity with ABI 1 for the provider-selectable reasons; a host-originated or unknown reason, or an empty component mask, is a `range` fault; genuine fault disables the mod | `genesis_mod_refusal_tests`, `mod_refusal_abi2_test.py` | T006 |
| the whole twin differential, including every scenario of the scenery sufficiency harness | `abi_parity_gate_tests` (extends `abi_parity_harness.hpp`, no second harness) | T008 |
| minor negotiation: `abi.minor = 1` refused by an older product (`E_FIELD_UNKNOWN`, simulated by the previous reader), `E_ABI_MINOR_TOO_NEW`, source/manifest/table agreement, an older provider on the extended host | `segapatch_abi_minor_tests` | T003 |

## Implementer-readable list of new symbols and capability behaviour

| Symbol | Where | Behaviour |
|---|---|---|
| `SEGA_MOD_ABI_MINOR` 1; manifest `abi.minor`; `SEGA_MOD_TARGET_ABI_MINOR` | `segarecomp_mod.h`, `mod_package.cpp`, the build command | Decision 10 |
| capability `frame.vdp` (id 3) grantable; required-only | `mod_scheduler.c` `genesis_mod_scheduler_supported`, manifest check | Decision 2 |
| `SegaGenesisFrameView` (incl. `flags`, `SEGA_GEN_FRAME_VRAM_FRAME_START`), `SegaGenesisFrameLine`, `SegaGenesisFeatures::frame` | `segarecomp_mod_genesis.h` | Decision 4; NULL outside the exact-frame callback |
| `genesis_mod_scheduler_present` (exact-frame `render` at the seam) | `mod_scheduler.c/.h` | Decision 1 |
| `SegaGenesisVdpHelpers`, `SegaGenesisFeatures::vdp` | `segarecomp_mod_genesis.h` | Decision 5 (T004) |
| `overlay_bitmap`, `overlay_layer_bitmap`, `overlay_layer_clip`, `overlay_fallback` | `SegaGenesisRenderApi` (appended) | Decision 6 (T005) |
| `decline` | `SegaGenesisRenderApi` (appended) | Decision 7 (T006) |

## Consequences

- A per-row generic flag is added to the scenery contract by SEG-152-T007 (promoted by the resolution step), before the twin differential gate.
- Stage B and C tasks implement a decided contract; the only implementation freedom is inside each decision's stated bounds.
- A provider that wants exact-frame data pays one callback per produced frame, and nothing for frames that are not produced; presentation can therefore never feed back into the guest (R1).
- The cost of the choice: an exact-frame provider cannot also use the predicted timing. Accepted (Decision 1); reversible by an additive phase.
- The Sonic 2 and Sonic 3 & Knuckles cross-frame state must be redesigned in their ports (R2/R4); this is a known cost recorded here, not discovered later.
