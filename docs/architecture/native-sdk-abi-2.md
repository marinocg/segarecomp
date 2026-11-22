# Native SDK ABI 2

Status: accepted contract (SEG-133-T003, ADR [0283](../decisions/0283-native-sdk-abi-2.md)). **Headers, typed feature tables, negotiation and the multi-file host compile are implemented** (F02, ADR [0288](../decisions/0288-modding-platform-host-implementation.md)); the Genesis phase scheduler and the memory/input/log runtime services are implemented (F08, ADR [0297](../decisions/0297-genesis-mod-phase-scheduler-and-work-ram-services.md): `reset`, `frame-begin`, `input`, `post-input`, `frame-end`, `render`; `width` is in bytes, 1/2/4; ports are 1 and 2); hook dispatch is implemented for the Genesis M68K (F09, ADR [0298](../decisions/0298-genesis-static-function-hooks.md): `hook-entry`/`hook-exit`, the register-view space `SEGA_GEN_SPACE_M68K_HOOK_REGS`, bindings of manifest hook ids to `hook_id`); a Luau mod is linked into a headless built program (F11, ADR 0300); a native ABI 2 provider is linked into headless and played programs through the same scheduler, with a typed render feature (F28, ADR 0305); function hooks of a native provider are linked into those programs (SEG-138-T012, ADR [0308](../decisions/0308-function-hooks-in-built-genesis-programs.md)). The header skeleton below is the contract; the committed headers are `platforms/sdk/segarecomp_mod.h` (core, negotiation), `platforms/genesis/sdk/segarecomp_mod_genesis.h`, `platforms/master-system/sdk/segarecomp_mod_sms.h` and `platforms/sega-cd/sdk/segarecomp_mod_scd.h`. ABI 1 (`platforms/genesis/sdk/segarecomp_genesis_patch.h`) is untouched.
Inputs: [schema 3](segapatch-schema-3.md), [phases](mod-execution-phases.md), [adapters](mod-platform-adapters.md), [gap inventory](modding-platform-gap-inventory.md).
Named consumers: SEG-135-T003 (multi-file C11 packages), SEG-146-T002/T003 (adapter plumbing, typed feature tables), SEG-137-T004 (Luau bindings), SEG-138-T001/T002/T004 (phases, memory, hooks), SEG-145-T001/T008 (audio).

## 1. Model

ABI 2 is a versioned, capability-negotiated provider table that **coexists** with ABI 1. One capability model serves C11 and Luau: a capability name grants the same operation to a native provider (through the host service table) and to a Luau script (through a binding, section 7).
The host never exposes a raw pointer into live guest memory or runtime structures. Memory is accessed through host services that take a named space, an offset and a width; render views are host-owned **copies** valid only for the duration of the call (the ABI 1 model).
**This is not a sandbox.** A native provider is trusted native code compiled into the program ([Luau policy](luau-trust-policy.md) section 1); ABI 2 limits what the *interface* offers.

## 2. Header skeleton (documentation only)

```c
/* segarecomp_mod.h  --  ABI 2 skeleton text; <stdint.h> and <stddef.h> only. */
#define SEGA_MOD_ABI_MAJOR 2U
#define SEGA_MOD_ABI_MINOR 1U             /* append-only; a host with a larger minor accepts a provider requiring a smaller or equal minor */

typedef enum SegaModPlatform { SEGA_MOD_PLATFORM_GENESIS = 1, SEGA_MOD_PLATFORM_MASTER_SYSTEM = 2, SEGA_MOD_PLATFORM_SEGA_CD = 3 } SegaModPlatform;

typedef enum SegaModPhase { SEGA_MOD_PHASE_RESET = 1, SEGA_MOD_PHASE_FRAME_BEGIN, SEGA_MOD_PHASE_INPUT, SEGA_MOD_PHASE_POST_INPUT,
                            SEGA_MOD_PHASE_HOOK_ENTRY, SEGA_MOD_PHASE_HOOK_EXIT, SEGA_MOD_PHASE_FRAME_END, SEGA_MOD_PHASE_RENDER } SegaModPhase;

typedef enum SegaModStatus { SEGA_MOD_OK = 0, SEGA_MOD_E_CAPABILITY = 1, SEGA_MOD_E_SPACE = 2, SEGA_MOD_E_RANGE = 3, SEGA_MOD_E_ALIGNMENT = 4,
                             SEGA_MOD_E_READONLY = 5, SEGA_MOD_E_NOT_OWNER = 6, SEGA_MOD_E_BUDGET = 7, SEGA_MOD_E_PHASE = 8 } SegaModStatus;

/* Host services (the host fills it; `size` is the host's sizeof; a provider reads only fields below `size`; every pointer may be NULL when its capability was not granted). */
typedef struct SegaModHost {
  uint32_t size, abi_major, abi_minor, platform;       /* SegaModPlatform */
  uint64_t granted[2];                                  /* bitset over append-only capability ids (section 4); required capabilities are always set, optional ones only if granted */
  const void *platform_features;                        /* typed per-platform table (section 3); NULL never for a supported platform */
  uint32_t platform_features_size;
  uint32_t phase;                                       /* SegaModPhase of the current callback */
  uint64_t frame;                                       /* guest frame counter, deterministic */
  void (*log)(const char *ascii, uint32_t len);         /* host.log: <= 256 bytes per line, <= 16 lines per callback */
  int (*read)(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value);            /* memory.read.<space> */
  int (*write)(uint32_t space, uint32_t offset, uint32_t width, uint32_t value);            /* memory.write.<space>: staged (phases section 4) */
  int (*input_get)(uint32_t port, uint32_t *buttons);   /* input.read */
  int (*input_set)(uint32_t port, uint32_t buttons);    /* input.intercept (phase `input` only) */
  uint32_t reserved[16];                                /* zero; future append-only fields follow */
} SegaModHost;

typedef struct SegaModProvider {
  uint32_t size, abi_major, required_minor, platform;
  const char *name; uint32_t implementation_version;
  int  (*init)(void *state, const SegaModHost *host);                 /* once, before `reset`; returns 0 on success */
  void (*phase)(void *state, const SegaModHost *host);                /* one entry for every phase the provider declared (host->phase) */
  int  (*hook)(void *state, const SegaModHost *host, uint32_t hook_id); /* hook.function: `hook_id` is the id the host bound to a manifest `hooks` entry (ADR 0298); returns 0 = proceed (before/after, or a declined replacement) or 1 = replaced (replace sites only, with every declared result written); anything else is a fault */
  int  (*save_state)(void *state, uint8_t *out, uint32_t cap, uint32_t *len);   /* state.mod: <= 64 KiB (phases section 6.2) */
  int  (*load_state)(void *state, const uint8_t *in, uint32_t len);
  void *reserved[8];                                                  /* NULL */
} SegaModProvider;

const SegaModProvider *segarecomp_mod_provider(void **state_out);    /* the ABI 2 entry symbol (ABI 1 uses segarecomp_patch_provider) */
```

Platform-typed tables (section 3) and the render views are separate structures reached through `platform_features` and render-phase view structures; none is a union and none is universal.

## 3. Typed per-platform feature tables

The host passes exactly one table, selected by `platform`. A provider built for one platform is rejected on another (`E_ABI_PLATFORM`); there is no universal struct. Space identifiers are **named enumeration constants** of the platform's table (never a numeric literal shared across platforms),
append-only, mirroring [adapter](mod-platform-adapters.md) space names.

| Table | Contents (skeleton) |
|---|---|
| `SegaGenesisFeatures` (Genesis, and the main side of Sega CD reuses its VDP view only) | `SEGA_GEN_SPACE_M68K_WORK_RAM`, `SEGA_GEN_SPACE_M68K_ROM` (read at build only), `SEGA_GEN_SPACE_Z80_RAM` (limited read), `SEGA_GEN_SPACE_VDP_*`; sizes (work RAM 65536, VRAM 65536, 24 registers, CRAM 128, VSRAM 80, 224 lines) as ABI 1 |
| `SegaMasterSystemFeatures` | `SEGA_SMS_SPACE_Z80_WORK_RAM` (8 KiB, mirror is one space), `SEGA_SMS_SPACE_Z80_CART_RAM` (read), mapper family (declared), VDP view sizes |
| `SegaCdFeatures` | `SEGA_SCD_SPACE_MAIN_WORK_RAM`, `SEGA_SCD_SPACE_MAIN_WORD_RAM` (read, owner-gated), VDP view as Genesis, disc identity digest (read-only) |

### 3.1 The typed render feature (SEG-138-T011, SEG-140-T010; ADR 0305 and 0306)

`SegaGenesisFeatures::render` (appended; a provider reads it only when `features->size` covers it) is non-NULL exactly when `overlay.rgba` was granted and points to `SegaGenesisRenderApi`, valid in the `render` phase only: `overlay_layer`, `overlay_target`, `overlay_rect`, `overlay_text` and `scene` (the C counterparts of the Luau `sega.overlay.*` and `sega.scene`, with the same rules: they are one host implementation), and `virtual_sprites(const GenesisVirtualSpriteSet *)`, which submits the frame's virtual sprites as ONE set in the shared transport (`segarecomp_genesis_vsprite.h`), at most once per callback. Every call returns a `SegaModStatus`; a non-zero result is also a fault of the callback (`range`, `budget` or `phase`): its staged output is discarded and only that provider is disabled. The table size and layout of the other fields are unchanged (`virtual_sprites` took the first reserved slot).

`scenery_window(plane, cell_x0, cell_y0, cols, rows, words)` (SEG-140-T011, ADR [0309](../decisions/0309-abi-2-authored-scenery-window.md); the second reserved slot) submits a bounded name-table window of the authored offscreen scenery per VDP plane in unwrapped plane-cell coordinates (at most 128 x 64 cells and 4096 words, one window per plane per callback). It needs the `scenery.words` capability and, like every render output, is committed only when the callback did not fault and describes exactly one produced frame; a frame is served only when both planes have a window, and a cell outside a window withdraws that frame's authored-scenery claim (the host presents the generic plane and the coverage report credits no scenery). `scenery.words` publishes the render feature even without `overlay.rgba`.

`view_margin(&margin)` and `view_left(left_px, margin_px)` (ADR [0315](../decisions/0315-asymmetric-widescreen-viewport.md); the third and fourth reserved slots; render phase only, they need only `overlay.rgba`) are the asymmetric-viewport seam. `view_margin` writes the per-side margin (source pixels) of the host's most recent extension that is presented: 0 in Original (a verified-HUD package makes the producer run there, but nothing of it is shown), at 4:3, after a failed production and before the first one; it is read from the runtime's own per-frame publication, never cached. `view_left` declares, for the frame the callback describes, that `left_px` of the planned `2 * margin_px` extra columns lie left of the picture and the rest right of it (0 = picture at the left edge of the view, `margin_px` = centred); `margin_px` is the margin the placement and the scenery windows built for it were computed for. The provider supplies a placement, never dimensions: the host validates it against the plan the frame is produced with, serves it only for the frame it was declared for (the same N + 2 epoch as a scenery window; restore, rewind, load and a skipped callback discard it) and only if `margin_px` is the margin that frame is produced with; when it is not, the placement AND the scenery windows of that callback are both withheld (the scenery is the generic extension; HUD and sprite contributions are independent of the view and stay), and the centred layout is presented. A `left_px` above 1344 or a `margin_px` above 672 is a range fault of the callback. A provider that wants its scenery to cover the placed view sizes its windows from `left_px` and the margin it learnt.

## 4. Capability catalogue

Legality by phase uses the phase names of [mod-execution-phases.md](mod-execution-phases.md). **Mutation**: R read-only, W mutates guest-visible state, O output only (presentation), S host service. **Trust**: every capability requires the package to be at least the class shown
(`native` for a `native-c11` provider; `luau` also available through the Luau binding where the last column names one). Ids are append-only; **id 0 and unlisted ids are reserved**.

| Id | Capability | Phases | Mutation | Trust implication | Platforms | Luau binding |
|---|---|---|---|---|---|---|
| 1 | `host.log` | all | S | none beyond class | G, S, C | `sega.log` |
| 2 | `frame.work_ram` | `render` | R (copy) | none | G, S, C (copy of the platform's work RAM) | `sega.frame.work_ram` |
| 3 | `frame.vdp` | `render` (exact-frame, see section 11) | R (copy) | none; **required-only**: it selects the exact-frame callback timing, so it may not be declared optional (`E_CAPABILITY_INCONSISTENT`) | G, C | `sega.frame.vdp` |
| 4 | `frame.sms_vdp` | `render` | R (copy) | none | S | `sega.frame.sms_vdp` |
| 5 | `scenery.words` | `render` | O | none | G, C | not bound (native only) |
| 6 | `overlay.rgba` | `render` | O | none | G, S, C | `sega.overlay` |
| 7 | `memory.read.<space>` | `reset`, `frame-begin`, `input`, `post-input`, `hook-entry`, `hook-exit`, `frame-end` | R | none | per [adapters](mod-platform-adapters.md) | `sega.mem.read` |
| 8 | `memory.write.<space>` (work RAM spaces only) | `frame-begin`, `post-input`, `hook-entry`, `hook-exit`, `frame-end` | W | **changes guest behavior**; affects save-state eligibility (below) | G (`m68k.work_ram`), S (`z80.work_ram`), C (`main.work_ram`) | `sega.mem.write` |
| 9 | `input.read` | `frame-begin`, `input`, `post-input` | R | none | G, S, C | `sega.input.get` |
| 10 | `input.intercept` | `input` | W (input state) | changes what the guest reads | G, S, C | `sega.input.set` |
| 11 | `hook.function` | `hook-entry`, `hook-exit` | W | changes guest control flow; spike-gated | G (after SEG-138-T003), S (after SEG-147-T003), C (after SEG-148-T004) | `sega.hook` (registered by manifest `hooks`, never by script at runtime) |
| 12 | `state.mod` | save-state barrier (`save_state`/`load_state`) | S | required for any guest-affecting native mod to keep states and rewind | G, S, C | `sega.state` (one serializable value) |
| 13 | `audio.event.read` | `frame-end`, `render` | R | none | per [audio contract](modding-audio-architecture.md) | `sega.audio.events` |
| 14 | `audio.command.send` | `frame-begin`, `post-input`, `hook-entry`, `hook-exit`, `frame-end` | W (declared command mailbox in work RAM, a declared mapping) | changes guest behavior | per audio contract | `sega.audio.send` |
| 15 | `audio.host.play` | reserved | O | reserved | unsupported until SEG-145-T009 GO | none |

A platform that does not support a capability fails closed ([schema 3](segapatch-schema-3.md) section 6); a capability may be supported at different [support levels](mod-platform-adapters.md) per platform.

**Guest-affecting** = any of ids 8, 10, 11, 14. A native mod with any of them MUST declare `state.mod`, else the build disables save states and rewind for that program with a visible reason (`E_STATE_UNAVAILABLE`); a Luau mod's state is the VM's one serialized value by construction.
ABI 1 providers are read-only (ids 2, 3, 5, 6 only) and unaffected.

## 5. Negotiation

1. The manifest declares `abi.native` = 2, `capabilities` (the required set; absent means empty) and `capabilities_optional` (absent means empty) ([schema 3](segapatch-schema-3.md) section 3).
2. At build, the host checks, in order: package platform equals the target platform; `abi.native` = 2; the provider source compiles against this header with `SEGA_MOD_ABI_MAJOR` 2; the provider's `required_minor` <= host minor; every required capability is supported on the platform; every optional name is known.
3. At load, the host fills `granted` with all required capabilities plus the supported optional ones; the provider must tolerate absent optional capabilities.
4. Struct evolution: the host reads provider fields only below the provider's `size`; a provider reads host fields only below the host's `size`; new fields are appended using the `reserved` slots first; a host never reorders or repurposes a field; a field never changes meaning.
5. A required capability is **never** silently dropped.

### Stable diagnostic classes (negotiation and load, exhaustive)

| Class | Condition | When |
|---|---|---|
| `E_ABI_UNSUPPORTED` | `abi.native` not provided by this release (for example 3, or 0) | build |
| `E_ABI_PLATFORM` | provider/manifest platform differs from the target, or the provider table's `platform` differs | build |
| `E_ABI_MIXED` | the build contains an ABI 1 provider and an ABI 2 provider, or one source exports both entry symbols | build |
| `E_ABI_HEADER_MISMATCH` | the compiled `SEGA_MOD_ABI_MAJOR` differs from the manifest `abi.native` | build |
| `E_ABI_MINOR_TOO_NEW` | `required_minor` is greater than the host minor | build |
| `E_ABI_TABLE_SIZE` | provider `size` smaller than the minimum for its `required_minor`, or non-multiple of pointer alignment | build/load |
| `E_ABI_ENTRY_MISSING` | the ABI 2 entry symbol is absent or returns NULL | build/load |
| `E_CAPABILITY_UNKNOWN` | a declared name is not in the catalogue | build |
| `E_CAPABILITY_UNSUPPORTED` | a required capability is not supported on the platform (or has not passed its gate: `hook.function`, `audio.host.play`) | build |
| `E_CAPABILITY_INCONSISTENT` | declared capabilities and payloads disagree ([schema 3](segapatch-schema-3.md) section 4) | build |
| `E_STATE_UNAVAILABLE` | a guest-affecting native mod without `state.mod` (states and rewind disabled, not a build failure) | build (warning class) |
| `E_MOD_FAULT` (cause `capability`) | a service is called without its capability, outside its phase, or on a space not declared | run (fault: [phases](mod-execution-phases.md) section 6) |

## 6. Coexistence with ABI 1 (compatibility matrix)

A package declares exactly one ABI: schema 2 packages are ABI 1; schema 3 `native-c11` packages are ABI 2 (`abi.native` = 2; a schema 3 package declaring 1 is `E_FIELD_VALUE`).

| ABI 1 provider case on a host that supports ABI 1 and 2 | Result |
|---|---|
| Schema 2 native-only package, four read-only capabilities, Genesis | loads and runs through the ABI 1 path **unchanged** (entry `segarecomp_patch_provider`, header `segarecomp_genesis_patch.h`) |
| Schema 2 package with BPS `rom` and `native` | unchanged: image patched first, provider unchanged |
| Schema 2 package naming an unknown capability or another ABI | refused exactly as today |
| ABI 1 provider source exporting the ABI 2 symbol, or an ABI 2 source exporting the ABI 1 symbol | `E_ABI_MIXED` (one entry symbol per source) |
| ABI 1 package and an ABI 2 package in one build (mod set) | `E_ABI_MIXED`: rejected until a later contract explicitly allows it |
| ABI 1 provider on Master System or Sega CD | impossible: schema 2 requires `platform` `genesis` |
| Save state / rewind with an ABI 1 provider | unchanged: ABI 1 is read-only, states restore identically with or without it (Genesis contract section 3.5); the capability probe still reads `GENESIS_NATIVE_COMPAT_ABI` |
| Save state with an ABI 2 provider | allowed only if the program has `state.mod` support or the provider is not guest-affecting; otherwise disabled with `E_STATE_UNAVAILABLE` |

ABI 1 compatibility is a **testable statement**: every schema 2 fixture of `tests/segapatch_native_package_test.py` and the provider build of `tests/genesis_patch_provider_build_test.py` MUST still pass unchanged.

## 7. Luau binding gate (one capability model)

A Luau VM contains a binding only if its capability is granted; an ungranted binding is **absent** (a call raises `E_MOD_FAULT` cause `capability`). The binding names above are the only host surface ([Luau policy](luau-trust-policy.md) section 5). Values cross the boundary by copy; no binding returns a pointer or userdata that aliases runtime memory.
M04 and M05 MUST NOT define capabilities of their own: a new binding needs a new catalogue id in this document through a new ABI minor.

## 8. Header evolution policy

Append-only: new fields only at the end or in `reserved`; new enum values only at the end; new capability ids only at the end; no removal, no reorder, no change of width or meaning within a major. A change that breaks any rule is ABI 3. Reserved fields are zero/NULL.
A new capability or field increments `SEGA_MOD_ABI_MINOR`. The header is C11 plus `<stdint.h>` and `<stddef.h>` only.
No function exposes a raw runtime pointer; pointers in render views are valid only during the call and are `const`.

## 9. Named later consumers

SEG-135-T003 (build integration, multi-file sources), SEG-146-T002 and T003 (adapter plumbing and typed tables), SEG-137-T004 (bindings), SEG-138-T001/T002 (phases and memory services), SEG-138-T004 (hooks), SEG-145-T008 (audio capabilities 13/14).

## 10. Implementation notes (F02, ADR 0288)

* The core header adds the preprocessor ids `SEGA_MOD_PLATFORM_ID_*`, the capability id constants `SEGA_MOD_CAP_*`, and the host-side `sega_mod_negotiate(provider, host, required)` with `sega_mod_negotiation_class` (the stable class names of the table in section 5 for the load-time classes). Platform feature tables are `SegaGenesisFeatures`, `SegaMasterSystemFeatures` and `SegaCdFeatures` (lifecycle and capability reporting only for Master System and Sega CD until SEG-147-T004 / SEG-148-T005).
* Build-time rejection: the host compiles each unit with `-DSEGA_MOD_TARGET_PLATFORM=<id> -DSEGA_MOD_TARGET_ABI_MAJOR=2U`; a platform header for another platform fails with `E_ABI_PLATFORM` in its `#error` text, a mismatched ABI major with `E_ABI_HEADER_MISMATCH`. The include scan reports another platform's header as `E_ABI_PLATFORM` and the ABI 1 header as `E_ABI_MIXED` before compiling.
* Test: `tests/mod_sources_test.py` with the synthetic providers and `negotiate_harness.c` under `tests/fixtures/mod_sdk/`.

## 11. Exact-frame presentation and ABI 1 parity (decided, [ADR 0320](../decisions/0320-abi-2-exact-frame-presentation-and-parity-contract.md); implemented by SEG-152-T003 and T004 (F32: exact-frame render, frame view, helpers, minor 1); T005 to T007 pending)

The ABI 2 host already serves ABI 2 output through the ABI 1 producer; what ABI 2 lacks is the **exact frame**: `render(N)` runs before the raster of the frame it describes exists. The parity matrix is [abi2-abi1-parity.md](abi2-abi1-parity.md). Items marked *Implemented* are in the header and host from SEG-152-T003/T004; the others are decided and not yet available:

* **Exact-frame `render`.** *Implemented (T003).* A provider that **requires** `frame.vdp` has its `render` callback run at the producer seam, once per frame produced for presentation, instead of after the snapshot. `host->frame` is the produced frame; work RAM is the previous-onset copy (the bytes the ABI 1 `prepare` sees); everything it submits is consumed by the same producer invocation (no `N + 2` binding). It cannot also use the predicted `render`. Optional `frame.vdp` is refused; providers that do not require it keep every callback bit-for-bit. *Implemented by SEG-152-T003.*
* **Frame view.** *Implemented (T003).* `SegaGenesisFeatures::frame` (appended): a `const SegaGenesisFrameView *` valid only inside that callback: picture size, planned `side`, requested components, `work_ram` (with `frame.work_ram`), frame-start `vram` (always a host-owned copy; a `flags` bit says whether it is the frame-start state, a frame without a VRAM journal gets the producer's end-of-frame copy marked as such and is not declined) and per-line registers (24 x u16), CRAM (128 B), VSRAM (80 B) for up to 224 lines. VDP spaces stay `SEGA_MOD_E_SPACE`. *T003.*
* **Helpers.** `SegaGenesisFeatures::vdp` (appended, with `frame.vdp`): `read_hscroll`, `read_vscroll`, `vscroll_is_per_column`, `cram_rgb`, the host's own functions (the ABI 1 `GenesisCompatHost` function pointers themselves, not wrappers; NULL without `frame.vdp`). *Implemented (T004).*
* **Bitmap.** `overlay_bitmap`, `overlay_layer_bitmap`, `overlay_layer_clip`, `overlay_fallback` (appended to `SegaGenesisRenderApi`): copied RGBA8 pixels in the existing overlay op list, bitmap-specific limits (8 requests, 393216 bytes, layers up to 256 x 256 inside the 65536-pixel transport), shared constants unchanged. *T005.*
* **Refusal.** `decline(components, reason)` (appended): a non-fault call; provider-selectable reasons `UNSUPPORTED_SCENE`, `BAD_SCROLL`, `MISMATCH`, `BAD_STATE`, host-originated ones (`STALE_STATE`, `FAULT`) are never selectable (a range fault); faults stay for invalid calls. *T006.*
* **Scenery.** The window geometry is sufficient (two windows per plane, column-sliced where a plane needs more than one) but one plane coordinate cannot carry two answers: a per-row generic flag is required for the per-line fallback class ([widescreen-scenery-window-sufficiency.md](widescreen-scenery-window-sufficiency.md); SEG-152-T007, decided, not yet specified in the header).
* **Version.** `SEGA_MOD_ABI_MINOR` becomes 1; the manifest `abi` object gains an additive `minor` (default 0): an older product refuses it with `E_FIELD_UNKNOWN`, a newer one with `E_ABI_MINOR_TOO_NEW` when it exceeds its own, and the build compiles with `-DSEGA_MOD_TARGET_ABI_MINOR`, so a source using a member beyond its declared minor fails with a readable `#error`. Older providers are unaffected. *Implemented (T003).* As built: the header hides the two minor-1 members (they are renamed placeholders of identical size) when the source targets minor 0, so using one fails to compile with a missing-member error, not the `#error` sketched above (a struct member cannot carry a custom message); a minor above the header's is the `#error E_ABI_MINOR_TOO_NEW`. `segarecomp build` refuses a package that requires `frame.vdp` without `abi.minor` 1 (`E_CAPABILITY_INCONSISTENT`). The host scheduler never grants an optional `frame.vdp` (timing cannot change), and the package layer refuses the declaration. The exact-frame callback exists where a producer does: the played (viewer) native program; the headless capture has no producer seam, so such a provider's `render` does not run there.
