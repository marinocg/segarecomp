# Widescreen geometry audit and planner model (SEG-139-T001)

Status: accepted input to SEG-139-T002/T003; implemented in the same delivery (F12). No private data: every number below
is derived from public code constants or from arithmetic on public Genesis geometry (no ROM, trace or asset content).

**Citation convention.** `path:line` refers to the tree at commit `ce49e73` (product `main` before SEG-139), i.e. the state this audit
describes. After the same delivery the lines move; the "Handling" column states what the delivery did with each item.

Scope: Genesis is the reference path. Master System (`SMS_EXT_MAX_SIDE`) and Sega CD (which reuses the Genesis producer) are
listed so that nothing implicit is shared silently; their coverage declarations belong to SEG-147-T009 / SEG-148-T010.

## 1. Data flow and buffer ownership

```text
settings (SvVideoSettings)  ->  geometry (sv_layout.c)  ->  producer (vdp_render.c, runtime)  ->  adapter surface (viewer_sdl3.c)
   -> host copy ext_raw (sv_host.c) -> side treatment buffers (side_buf/side_scratch) -> streaming texture -> render_game / sv_layout_extended_dest
```

| Buffer | Owner / lifetime | Size rule at `ce49e73` | Cite |
| --- | --- | --- | --- |
| Producer surface `g_extension_surface` | Genesis viewer adapter, static for the run | `(320 + 2*256) * 448 * 3` = 774,144 B (`GENESIS_EXT_SURFACE_BYTES`) | `platforms/genesis/viewer/viewer_sdl3.c:41`, `platforms/genesis/runtime/frame_extension.h:28-30` |
| Producer row scratch `sprite_row[]` | producer, stack, per call | `GENESIS_EXT_MAX_WIDTH` candidate pixels (4 B each) | `platforms/genesis/runtime/vdp_render.c:1265` |
| Host private copy `ext_raw` | `SvHost`, malloc at open | `(max_w + 2*SV_EXT_MAX_SIDE) * max_h * 3` | `libs/viewer/src/sv_host.c:922`, `libs/viewer/src/sv_internal.h:68` |
| Side buffers `side_buf` (left+right) | `SvHost`, malloc at open | `2 * SV_EXT_MAX_SIDE * max_h * 3` | `libs/viewer/src/sv_host.c:923-924` |
| Side scratch `side_scratch` | `SvHost`, malloc at open | `(h*12 + SV_SIDE_MAX_FADE) * 4` | `libs/viewer/src/sv_host.c:925`, `libs/viewer/src/sv_layout.c:171-175` |
| Streaming texture `frame_tex` | `SvHost`, recreated when `(w + 2*side) x h` changes | one RGB24 texture of the presented surface | `libs/viewer/src/sv_host.c:387-396` (`ensure_texture`) |
| Retained overlay `ovl_rgba` | `SvHost`, inline array | `SV_OVERLAY_MAX^2 * 4` (HUD, independent of side) | `libs/viewer/src/sv_internal.h:74` |

The producer never owns memory: the adapter hands a surface and a `capacity` and the producer checks `pitch * height <= capacity`
(`vdp_render.c:1286-1290`). The host copies the accepted surface row by row into `ext_raw` (`sv_host.c:1182`) after
`sv_extended_surface_check` validated size, pitch and side (`sv_layout.c:104-117`).

## 2. Assumption inventory

`H` = handling in this delivery. "Legacy" = the original 4:3..16:9 functions, kept bit-for-bit for existing settings.

### 2.1 Aspect assumptions (16:9 literals)

| # | Assumption | Cite | H |
| --- | --- | --- | --- |
| A1 | Production side is always for **16:9**: `total = src_h*16*src_w*ad / (9*an)` | `libs/viewer/src/sv_layout.c:40` | Legacy rule kept for unset ratio; new exact planner `sv_geom_plan` takes any ratio 4:3..32:9 |
| A2 | The user's "width" is a *linear* interpolation `(300+width)/225`, 0 = 4:3 .. 100 = 16:9; it can never name 21:9 or 32:9 | `libs/viewer/src/sv_settings.c:403-409`, `libs/viewer/include/sv_settings.h:51`, field `:78`, clamp `sv_settings.c:98` | Kept unchanged; new optional exact ratio `video.widescreen_ratio` (N:M) takes precedence when set |
| A3 | Ratio labels only recognise 4:3 and 16:9 by milli-ratio window | `libs/viewer/src/sv_settings.c:418-419` | Unchanged (labels for the legacy slider); exact ratios print as `N:M` (`sv_ratio_format`) |
| A4 | The 4:3-corrected aspect reports a fixed 1333 milli | `libs/viewer/src/sv_settings.c:412` | Unchanged (reporting only, no geometry decision) |
| A5 | Visible side uses the same 4:3..16:9 interpolation and the legacy clamp | `libs/viewer/src/sv_layout.c:47-61`, `libs/viewer/src/sv_host.c:1075` | Host calls `sv_geom_resolve_visible` (legacy function unless an exact ratio is set) |
| A6 | Menu and launcher offer only the 0..100 width slider | `libs/viewer/src/sv_pages.c:112-123,777`, `apps/segarecomp-launcher/app_pages.cpp:503-507` | **Not changed** (no UI in this delivery; the ratio is a settings-file value). Recorded as a limitation |
| A7 | Per-game profile stores only `widescreen`, `side_style`, `width` | `libs/viewer/src/sv_settings.c` (profile functions), `sv_settings.h:146-150` | Unchanged: the exact ratio is a global-file setting; a profile never carries it |
| A8 | The native display aspect model has exactly two pixel policies (square / 4:3-corrected) | `libs/viewer/include/sv_settings.h:32`, `sv_layout.c:28-33` | Unchanged; planner takes the same `SvAspect` |

### 2.2 Bounds on side extension, picture size and buffers

| # | Bound | Value | Cite | Consequence | H |
| --- | --- | --- | --- | --- | --- |
| B1 | `SV_EXT_MAX_SIDE` | 256 px/side | `libs/viewer/include/sv_layout.h:27` | Legacy functions **clamp silently** at 256 (`sv_layout.c:44,59`): a 256x448 square-pixel picture at 16:9 needs 270 but gets 256 | Hard bound raised to **672** (`SV_GEOM_MAX_SIDE`); legacy functions keep their 256 clamp under the new name `SV_EXT_LEGACY_SIDE`; the planner **rejects** instead of clamping |
| B2 | Genesis mirror `GENESIS_EXT_MAX_SIDE` | 256 | `platforms/genesis/runtime/frame_extension.h:28`; compile-time equality `viewer_sdl3.c:39` | Producer refuses larger sides (`vdp_render.c:1282`) | Raised to 672; equality assertion kept |
| B3 | 32:9 needs more than 256 | up to **668** (256x448 square pixels) | arithmetic (section 4) | Cannot be produced under B1/B2 | Covered by raising to 672 |
| B4 | Largest native picture | 4096 x 4096 | `sv_layout.c:37,51,64,76,110` | Prevents overflow; unchanged | `SV_GEOM_MAX_NATIVE` |
| B5 | Genesis frame | 320 x 448 | `platforms/genesis/runtime/checkpoint_evidence.h:17-18` | Defines the largest native picture; unchanged | Unchanged |
| B6 | Window size for placement | <= 2^20 | `sv_layout.c:77` | Presentation math uses doubles; no overflow | Unchanged |
| B7 | Side treatment rows | <= 480 | `libs/viewer/include/sv_layout.h:104`, `sv_layout.c:174,183`, `sv_host.c:1047` | 448-row Genesis fits | Unchanged |
| B8 | **Fade table `SV_SIDE_MAX_FADE` = 257** | 257 weights | `libs/viewer/src/sv_layout.c:171-175` | `sv_side_fill` writes `fade[0..side-1]` (`sv_layout.c:218-221`) and reads them back (`:227`); with side > 256 this would **overrun the scratch** (latent overflow once B1 is raised) | Defined as `SV_EXT_MAX_SIDE + 1`; test fills at side 672 with an exactly-sized scratch under AddressSanitizer |
| B9 | Overlay | 256 x 256 | `sv_layout.h:77`, `sv_host.c:1173` | Independent of side; HUD only | Unchanged |
| B10 | SMS bound | 80 px/side | `platforms/master-system/runtime/sms_render.h:63`, surface `sms_viewer_sdl3.c:39-40` | SMS cannot honour more than 80; requests come from the legacy rule (`sms_viewer_sdl3.c:131-132`) | Unchanged; the host shows `min(planned, produced)` so a wider request is honest about what SMS delivered. Coverage declaration: SEG-147-T009 |

### 2.3 Integer-overflow review

| Site | Expression | Worst case before | Verdict |
| --- | --- | --- | --- |
| Side planning | `(int64_t)src_h * 16 * src_w * ad` | 4096*16*4096*3 ~ 8e11 | Safe in `int64_t` (already) |
| Surface bytes (producer) | `((size_t)width + 2U*side) * 3U * height` | `(320+512)*3*448` | Safe; `size_t` |
| Surface bytes (host) | `(mw + 2u*SV_EXT_MAX_SIDE) * mh * 3u` | `size_t` arithmetic | Safe; at 672: 2,236,416 B |
| Pitch check | `(int64_t)pitch * (height-1) + row_bytes` | `int64_t` | Safe (`sv_layout.c:112-115`) |
| Texture width | `ensure_texture(h, w + 2*side, hgt)` | `int` | 320 + 1344 = 1664, far below `INT_MAX`; below SDL's renderer limits |
| Side buffers | `2u * vis * hgt * 3u` | `size_t` | Safe; capacity checked before every fill (`sv_host.c:1047`) |
| `uint16_t side` in the result | `GenesisExtensionResult.side` | 65535 | 672 fits (`frame_extension.h:70`) |
| Exact ratio terms | `num*9`, `den*32`, `native_w*3*num` | terms <= 4096 | Products < 2^40, `int64_t` (`sv_ratio_valid`, `sv_geom_plan`) |

## 3. Presentation modes

The host has **two scale modes** (`SV_SCALE_FIT`, `SV_SCALE_INTEGER`, `sv_settings.h:31`) and **two pixel policies**; there is no separate
"stretch" or "crop" mode. A wider-than-window surface is *letterboxed* (`sv_layout_extended_dest`, `sv_layout.c:69-102`); "crop" exists
only as the centred sub-rectangle of the maximum-size texture when the visible side is smaller than the produced side
(`sv_host.c:554-563`). The task wording "letterbox/stretch/crop" is therefore verified as: fit (letterbox) and integer (letterbox with whole
scale, falling back to fit below 1x), plus the visible-side crop. Nothing stretches the picture: the native rectangle always keeps its
pixel policy (`sv_layout.c:96-98`).

## 4. Geometry model (what T002 implements)

For a native picture `w x h` and pixel policy `P` the display width of the native picture is `w` (square pixels) or `4h/3` (4:3-corrected).
A target display aspect `n:d` (display width / display height) needs the surface to be `T` source pixels wide:

```text
square pixels   T = floor(h * n / d)
4:3-corrected   T = floor(3 * w * n / (4 * d))
extra = T - w ;  side = extra <= 1 ? 0 : floor(extra / 2)       (symmetric by construction; the native picture stays centred)
```

This is exactly the legacy formula with `n:d = 16:9` (verified by a recorded fixture hash, section 6). It is monotonic in `n/d` and uses only
`int64_t` products of values <= 4096.

Widest cases (source pixels per side, square pixels unless noted):

| Picture | 4:3 | 16:9 | 21:9 | 32:9 |
| --- | --- | --- | --- | --- |
| 320 x 224 | 0 | 39 | 101 | 238 |
| 320 x 224 (4:3-corrected) | 0 | 53 | 120 | 266 |
| 256 x 224 | 21 | 71 | 133 | 270 |
| 320 x 448 (interlaced) | 138 | 238 | 362 | 636 |
| 256 x 448 (interlaced H32) | 170 | 270 | 394 | **668** |

**Maximum supported surface.** `SV_GEOM_MAX_SIDE = 672` source pixels per side. The largest Genesis surface is therefore
`(320 + 2*672) x 448 = 1664 x 448` RGB24 = **2,236,416 bytes** per surface, and the host allocates at most one raw copy of it plus the
side buffers (`2*672*448*3` = 1,806,336 B). 672 is the smallest multiple of 16 that covers the widest need (668).

**Rejection rule.** The planner never clamps. It returns a typed status and a zero plan for: a native size outside `1..4096`
(`SV_GEOM_BAD_NATIVE`), a ratio that is unset, has a term outside `1..4096`, or lies outside `[4:3, 32:9]` (`SV_GEOM_BAD_ASPECT`),
and a required side above 672 (`SV_GEOM_TOO_WIDE`). Consumers fail closed on any non-OK plan: the Genesis producer reports
`NO_PLAN` (UNAVAILABLE), the host shows only the native picture; no buffer is ever sized from a reduced value.

**Settings model.** `video.widescreen_ratio=N:M` (global file only; `4:3`, `16:9`, `21:9`, `32:9` are the presets, any other `N:M`
inside the range is custom). Unset (the default) means the legacy width applies and the saved file stays byte-identical. An unreadable or
out-of-range stored value is dropped (the legacy width applies) rather than clamped. A previous release ignores the unknown key
(`apply_pair` returns 0 for unknown keys, `sv_settings.c` parse loop) and keeps using `video.widescreen_width`, which is preserved.

## 5. Planner API (T002)

```c
typedef struct SvDisplayRatio { int num, den; } SvDisplayRatio;                 /* {0,0} = unset */
SvGeomStatus sv_geom_plan(int native_w, int native_h, SvAspect aspect, SvDisplayRatio target, SvGeomPlan *out);
SvGeomStatus sv_geom_resolve_production(const SvVideoSettings *video, int native_w, int native_h, int *side_out);
int          sv_geom_resolve_visible(const SvVideoSettings *video, int native_w, int native_h, int produced_side);
```

Pure, SDL-free, allocation-free, in `libs/viewer/include/sv_layout.h` / `src/sv_layout.c` (the module every packaged viewer build already compiles, so no
build list changes).

## 6. Consumers and verification map (T003)

| Consumer | Change | Evidence |
| --- | --- | --- |
| Genesis producer plan callback (`viewer_sdl3.c`) | calls `sv_geom_resolve_production`; a rejected plan produces nothing | `genesis_extension_test` (pixel-exact references at 101, 238, 266, 636, 668 and 672), `sv_aspect_matrix_test` |
| Producer bound / row scratch | `GENESIS_EXT_MAX_SIDE` 672; scratch is 1664 candidate pixels (6.6 KB) | same tests; strict-C11 packaged build compiles |
| Host buffers and side fill | sized from `SV_EXT_MAX_SIDE`; fade table sized from the same constant | `sv_viewer_core_test geometry` under ASan/UBSan |
| Host visible side / presentation | `sv_geom_resolve_visible`; `sv_layout_extended_dest` unchanged | `sv_aspect_matrix_test` (4 presets + one custom ratio x 3 pictures x 4 windows, fit; integer scale in 1920x1080) |
| Legacy settings | production and visible side unchanged | fixture hash of 352 values recorded from the unmodified functions at `ce49e73` |

## 7. Out of scope (recorded, not hidden)

- No menu/launcher control for the exact ratio (A6); it is a settings-file value in this delivery.
- Master System production stays at its 80-pixel bound (B10); Sega CD uses the Genesis producer and inherits the new bound.
- A wide background is not proof of wide gameplay: declared per-mod and per-scene coverage is SEG-139-T004/T006, not this delivery.
