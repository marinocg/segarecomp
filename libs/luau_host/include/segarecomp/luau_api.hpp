#pragma once

// The restricted Luau host API (ADR 0295; SEG-137-T004..T006). ONE table, `api_entries()`, is the machine-readable source of every host name a script can reach:
//  * the runtime installs exactly these bindings (luau_vm.cpp), each gated by its ABI 2 capability (native-sdk-abi-2.md section 4/7),
//  * `mod check`/`pack`/`build` run `analyze_mod`, a static check of the sources against this table (luau_analysis.cpp),
//  * `declarations()` and `api_json()` render the `.d.luau` declaration file and the JSON list from it (committed copies are compared by a test, so they cannot drift).
// Values cross the boundary by copy: numbers, strings and plain output records. No userdata, function or table of the host is ever handed to a script.

#include "segarecomp/luau_host.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::luau {

struct ApiEntry {
  std::string_view name;         // "sega.log"; a callback is a bare global ("render")
  std::string_view capability;   // catalogue name that must be declared; "" = the callback of the lifecycle (always available)
  std::string_view kind;         // "function" | "value" | "callback"
  std::string_view params;       // "name:type,..." with type number|string ("" = none); for a value, its type
  std::string_view returns;      // "" = none, else a Luau type
  std::string_view phases;       // "all" | "render" | "reset"
  std::string_view doc;
};
[[nodiscard]] std::span<const ApiEntry> api_entries();
[[nodiscard]] const ApiEntry *find_api(std::string_view name);
// The Luau type of one entry, e.g. "(x: number, y: number) -> ()".
[[nodiscard]] std::string api_signature(const ApiEntry &entry);
// The generated declaration file (`segarecomp.d.luau`) and the JSON list; both deterministic, both derived from `api_entries()` only.
[[nodiscard]] std::string declarations();
[[nodiscard]] std::string api_json();

// ---- limits of the API (policy sections 4 and 5) ----
inline constexpr std::size_t kLogLineBytes = 256, kLogLinesPerCallback = 16;
inline constexpr std::size_t kOverlayOpsPerCallback = 64, kOverlayTextBytes = 64, kFrameValueNameBytes = 32, kFrameValuesMax = 32, kNotesMax = 16;
// The fixed HUD overlay path (platforms/genesis/sdk/segarecomp_genesis_patch.h: GENESIS_COMPAT_OVERLAY_MAX_W/H): one RGBA8 image, fully transparent where nothing is drawn.
inline constexpr int kOverlayWidth = 128, kOverlayHeight = 256;
// SEG-140-T004: positioned layers: at most four, each at most 128 x 128 (four of them are exactly the host's 65,536-pixel overlay budget).
inline constexpr int kOverlayLayersMax = 4, kOverlayLayerSide = 128;
// SEG-152-T005 (ADR 0320 decision 6): the bounded RGBA bitmap feature of the native render API. These bounds belong to the bitmap feature ONLY (a provider/script that never uses it has the previous accounting):
// a bitmap layer is at most 256 x 256 within the shared 65,536-pixel transport, at most 8 bitmap requests and 393,216 submitted bytes (128 x 256 x 4 + 65,536 x 4) per callback.
inline constexpr int kBitmapLayerSide = 256;
inline constexpr std::size_t kLayerPixelsMax = 65536, kBitmapRequestsMax = 8, kBitmapBytesMax = 128U * 256U * 4U + 65536U * 4U;

// ---- copy-in / copy-out records ----
struct FrameValue {
  std::string name;        // [a-z0-9_.], at most 32 bytes
  std::int64_t value = 0;  // |value| <= 2^53 (an integer a script can hold exactly)
};
// The guest-facing services of the phase scheduler (SEG-138-T001/T002), as plain function pointers so this library and the generated-native program never depend on the native SDK header.
// The status codes are the SegaModStatus values (0 ok, 1 capability, 2 space, 3 range, 4 alignment, 5 readonly, 6 not_owner, 7 budget, 8 phase); the scheduler is the single authority for phase, range,
// alignment and budget rules, so a Luau script and a native provider get identical results. `space` is the SegaGenesisSpace id (0 = unknown name).
struct HostServices {
  void *ctx = nullptr;
  int (*read)(void *ctx, std::uint32_t space, std::uint32_t offset, std::uint32_t width, std::uint32_t *value) = nullptr;
  int (*write)(void *ctx, std::uint32_t space, std::uint32_t offset, std::uint32_t width, std::uint32_t value) = nullptr;
  int (*input_get)(void *ctx, std::uint32_t port, std::uint32_t *buttons) = nullptr;
  int (*input_set)(void *ctx, std::uint32_t port, std::uint32_t buttons) = nullptr;
  // SEG-145-T008: the declared audio interface (SegaModAudioApi). `op`: 0 play_sfx, 1 play_music, 2 stop_music (name ignored). `audio_event` writes at most 32 characters and a terminator ("" = no event).
  int (*audio_command)(void *ctx, int op, const char *name) = nullptr;
  int (*audio_event)(void *ctx, const char *mapping, char name_out[33]) = nullptr;
};
struct FrameInput {
  std::uint64_t number = 0;
  std::vector<FrameValue> values;   // the read-only values the host publishes for this frame (at most 32)
  const HostServices *services = nullptr;   // valid for the call only; null = the sega.mem / sega.input bindings fault with cause `phase`
};
struct OverlayOp {
  enum class Kind { rect, text, bitmap } kind = Kind::rect;
  int layer = 0;                     // 0 = the fixed HUD overlay; 1..kOverlayLayersMax = a positioned layer (SEG-140-T004)
  int x = 0, y = 0, w = 0, h = 0;   // already clamped to the overlay / layer
  std::uint32_t rgba = 0;            // 0xRRGGBBAA
  std::string text;                  // printable ASCII, at most 64 bytes
  std::vector<std::uint8_t> pixels;  // Kind::bitmap: the visible (clamped) w x h RGBA8 pixels, tightly packed, copied from the provider during the call (never a pointer into provider memory)
};
// A non-fatal diagnostic of a callback (a clamp or a truncation). `cause` is a stable class: "log_truncated", "text_truncated", "overlay_clamped", "overlay_clipped".
struct Note {
  std::string cause, message;
};
// What one successful callback produced. A faulted callback produces nothing (staged output is discarded, phases section 6.1).
// SEG-140-T004 (ADR 0303): a positioned, layered overlay declared with sega.overlay.layer. `anchor` and `reference` are the viewer's SvOvAnchor / SvOvReference values (0..8 / 0..1); the host validates
// them against the live geometry (out-of-bounds, budget) and reports a rejection, it never repairs one.
struct OverlayLayerDecl {
  int index = 0;                     // 1..kOverlayLayersMax
  int anchor = 0, reference = 0, dx = 0, dy = 0, w = 0, h = 0, z = 0;
  bool bitmap = false;               // SEG-152-T005: declared with the bitmap layer call (up to 256 x 256): accepts bitmap ops only
  bool has_clip = false;             // SEG-152-T005: GenesisCompatLayer::has_clip; only the clip rectangle (layer pixels) of the image is drawn
  int clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
};
// SEG-140-T008 (ADR 0306): the per-frame virtual-sprite submission of one render callback (platform-neutral names; the Genesis adapter maps it onto GenesisVirtualSpriteSet, the one transport every authoring
// route shares). Provenance 1 = game-state verified, 2 = mod authored; layer 0 behind_planes, 1 between, 2 above_all. Semantic validity (sizes, tile and palette ranges, positions, clips, the budget
// and the verified-state gate) is the HOST's whole-set validation with its stable reasons: the script only supplies commands and its OWN pool, and never reads guest memory or VDP state.
inline constexpr std::size_t kSpriteCommandsMax = 64, kSpriteCallsMax = 256;
inline constexpr std::size_t kSpritePoolVramBytes = 65536, kSpritePoolCramBytes = 128;
struct SpriteCommand {
  int provenance = 2, layer = 0, w_cells = 1, h_cells = 1, palette = 0, h_flip = 0, v_flip = 0, tile = 0, x = 0, y = 0;
  bool has_clip = false;
  int clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
};
struct SpriteSubmission {
  bool used = false;                      // some sega.sprite call ran in the callback
  int budget_mode = 0;                    // 0 = limit (the original per-line budgets), 1 = extended
  int space = 0;                          // SEG-140-T009: 0 = picture (draws inside the native picture only), 1 = presentation (may also draw into the presented widescreen columns; same origin)
  std::uint32_t total = 0;                // sega.sprite.add calls, saturating at 255; `commands` keeps the first kSpriteCommandsMax (a larger total is the host's whole-set rejection, never a truncation)
  std::uint32_t calls = 0;                // all sega.sprite calls (bounded by kSpriteCallsMax)
  std::vector<SpriteCommand> commands;
  std::vector<std::uint8_t> vram, cram;   // the authored pool in the VDP's own VRAM / CRAM layouts, allocated when first written (empty = all zero)
};
struct Output {
  SpriteSubmission sprites;
  std::string scene;
  std::vector<OverlayLayerDecl> layers;                  // sega.scene: the scene the frame belongs to (a validated token, "" = none; SEG-139-T004)
  std::vector<std::string> log;
  std::vector<OverlayOp> overlay;
  std::vector<Note> notes;
  // SEG-152-T005: the bitmap feature's own accounting (a bitmap does not consume the rect/text op budget) and the fixed overlay's ABI 1 fallback position.
  std::uint32_t bitmap_requests = 0;
  std::uint64_t bitmap_bytes = 0;
  int fallback_x = 0, fallback_y = 0;
  // SEG-152-T006: the non-fault decline of the native render API (components in the host's GENESIS_COMPAT_* bits and the diagnostic reason); 0 = nothing declined.
  std::uint32_t declined = 0, decline_reason = 0;
};

// SEG-138-T011 (ADR 0305): the ONE implementation of the overlay request rules (validation, clamping, notes, budgets), shared by the Luau `sega.overlay.*` bindings and the native ABI 2 render API so a
// native provider and a script that make the same calls stage identical output. `target` is the layer the following rect/text calls draw into (0 = the fixed HUD overlay).
struct OverlayCall {
  enum class Status { ok, range, budget } status = Status::ok;
  std::string message;   // empty when ok
};
[[nodiscard]] OverlayCall overlay_declare_layer(Output &out, const OverlayLayerDecl &layer);   // index 1..4, anchor 0..8, reference 0..1, |dx|,|dy| <= 2048, 1 <= w,h <= 128, z 0..3, not declared twice
// SEG-152-T005 (ADR 0320 decision 6). The bitmap-specific layer declaration: like overlay_declare_layer but 1 <= w,h <= 256, the declared layers share the 65,536-pixel transport (budget), and the layer accepts bitmap ops only.
[[nodiscard]] OverlayCall overlay_declare_bitmap_layer(Output &out, const OverlayLayerDecl &layer);
// A bitmap of w x h RGBA8 pixels (R,G,B,A, straight alpha, `stride_bytes` >= 4 * w, rows top to bottom) at (x, y) of the target; 1 <= w,h <= target size (else range), an origin that puts it partly outside is clamped with a note (never moved).
// The visible part is COPIED; the bitmap REPLACES the destination bytes when rasterized (rect/text blend). At most 8 requests and 393,216 submitted bytes per callback (budget fault); rect/text are refused on a bitmap layer (range).
[[nodiscard]] OverlayCall overlay_bitmap(Output &out, int target, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h, const std::uint8_t *rgba, std::uint64_t stride_bytes);
[[nodiscard]] OverlayCall overlay_layer_clip(Output &out, std::int64_t index, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h);   // a declared layer; the clip lies inside it; the last call wins
[[nodiscard]] OverlayCall overlay_fallback(Output &out, std::int64_t x, std::int64_t y);                                                         // 0..65535 picture pixels (GenesisCompatOverlay::fallback_x/y)
[[nodiscard]] OverlayCall overlay_select_target(Output &out, int &target, std::int64_t index);   // 0 or a layer declared earlier in the callback
[[nodiscard]] OverlayCall overlay_rect(Output &out, int target, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h, std::uint32_t rgba);
[[nodiscard]] OverlayCall overlay_text(Output &out, int target, std::int64_t x, std::int64_t y, std::string_view text, std::uint32_t rgba);
[[nodiscard]] OverlayCall overlay_scene(Output &out, std::string_view id);
// The virtual-sprite calls of the same render callback (same result type): shared by the Luau `sega.sprite.*` bindings and any host-side recorder.
[[nodiscard]] OverlayCall sprite_space(Output &out, std::string_view mode);                                    // "picture" (default) | "presentation"
[[nodiscard]] OverlayCall sprite_budget(Output &out, std::string_view mode);                                  // "limit" | "extended"
[[nodiscard]] OverlayCall sprite_tile(Output &out, std::int64_t index, std::string_view hex);                // 64 hex digits per 8x8 4bpp pattern, consecutive patterns from `index` (0..2047)
[[nodiscard]] OverlayCall sprite_palette(Output &out, std::int64_t line, std::string_view hex);              // 4 hex digits per CRAM word (0BGR), at most 16 colours, palette line 0..3
[[nodiscard]] OverlayCall sprite_add(Output &out, std::string_view provenance, std::string_view layer, std::int64_t x, std::int64_t y, std::int64_t w_cells, std::int64_t h_cells, std::int64_t tile,
                                     std::int64_t palette, std::int64_t flags);                              // provenance "authored" | "verified"; layer "behind_planes" | "between" | "above_all"; flags bit 0 h-flip, bit 1 v-flip
[[nodiscard]] OverlayCall sprite_clip(Output &out, std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h);   // clip rectangle (picture pixels) of the most recently added sprite   // a token [a-z0-9_-], at most 32 characters
// Printable ASCII only: control bytes and non-ASCII become '?'; `truncated` is set when `text` was longer than `limit`.
[[nodiscard]] std::string printable_ascii(std::string_view text, std::size_t limit, bool &truncated);

// The capabilities a VM grants (catalogue names). A binding whose capability is not here exists as a stub that faults with cause `capability`.
struct HostConfig {
  std::vector<std::string> capabilities;
};
// The capabilities the package declared that this platform actually grants: required plus the supported optional ones.
[[nodiscard]] HostConfig host_config_for(const mod::Manifest &manifest);

// ---- the live generated-native path (SEG-137-T007, ADR 0296) ----
// The minimal read-only seam of a played Genesis program grants exactly these capabilities; a package that REQUIRES anything else (or is not Luau-only, not Genesis, or carries a native ABI 2 provider)
// is refused by the build with the class below, never run with parts missing. An optional capability outside the list is simply not granted.
[[nodiscard]] std::vector<std::string> live_capabilities();
[[nodiscard]] std::optional<mod::Rejection> live_support(const mod::Package &package);   // E_LIVE_PLATFORM | E_LIVE_TRUST | E_LIVE_ABI | E_LIVE_CAPABILITY | E_LIVE_PAYLOAD
[[nodiscard]] HostConfig live_host_config(const mod::Manifest &manifest);
// SEG-138-T009 (ADR 0300): the headless scheduled path (the ONE mod scheduler, phases + bounded work-RAM/input services); same rejection classes as live_support.
[[nodiscard]] std::vector<std::string> scheduled_capabilities();
[[nodiscard]] std::optional<mod::Rejection> scheduled_support(const mod::Package &package);
[[nodiscard]] HostConfig scheduled_host_config(const mod::Manifest &manifest);
// SEG-138-T011 (ADR 0305): the same scheduled path for a NATIVE ABI 2 package (trust class native, `abi.native` 2, Genesis, native-c11 sources, no Luau; hooks allowed, SEG-138-T012); same rejection classes. A package with hooks and a Luau provider is E_HOOK_CALLBACK_UNSUPPORTED (live_support and scheduled_support).
[[nodiscard]] std::optional<mod::Rejection> native_scheduled_support(const mod::Package &package);
[[nodiscard]] std::vector<std::string> native_scheduled_capabilities();
[[nodiscard]] HostConfig native_scheduled_host_config(const mod::Manifest &manifest);

// ---- overlay rasterization (the fixed HUD overlay path) ----
struct OverlayImage {
  int width = kOverlayWidth, height = kOverlayHeight;
  std::vector<std::uint8_t> rgba;   // width * height * 4, tightly packed
};
// Source-over compositing of the operations in order, integer arithmetic only: identical on every host. Text uses the viewer's body bitmap font (sv_font.h), left aligned at (x, y).
[[nodiscard]] OverlayImage rasterize_overlay(std::span<const OverlayOp> ops);   // only the ops of layer 0 (the fixed overlay), exactly as before layers existed
// The ops of one declared layer, rasterized onto its own w x h image (SEG-140-T004).
[[nodiscard]] OverlayImage rasterize_layer(const OverlayLayerDecl &decl, std::span<const OverlayOp> ops);

// ---- static check against the API list (mod check / pack / build) ----
struct AnalysisIssue {
  std::string cls;   // E_LUAU_API (not in the API list) | E_LUAU_CAPABILITY (needs an undeclared capability) | E_LUAU_TYPE (arity or literal argument type)
  Diagnostic where;
};
// Best effort by design: it follows direct `sega.a.b` accesses and calls; a script that aliases `sega` is still held to the same rules at run time (capability stubs). A file that does not parse is skipped here;
// the trusted compile step reports it. Issues are ordered by path then line.
[[nodiscard]] std::vector<AnalysisIssue> analyze_mod(const mod::Package &package);

}  // namespace segarecomp::luau

namespace segarecomp::luau {

// ---- the deterministic fixture session (SEG-137-T006/T007) ----
// Runs a compiled Luau-only mod for `frames` frames against a synthetic, ROM-free frame source: a 320x224 RGB24 pattern seeded by the digest of the (fixture) image the package was installed for,
// a few published values (`score`, `lives`, `timer`), the script's overlay composited at the picture's top-left corner exactly as the viewer's fixed overlay seam does (RGBA8, straight alpha, picture
// rows 1:1, clipped), and a SHA-256 of every resulting frame. It does NOT run the Genesis machine: it proves the script -> overlay request -> raster -> composite chain, deterministically.
inline constexpr int kFixtureWidth = 320, kFixtureHeight = 224;
inline constexpr std::uint64_t kSessionFramesMax = 3600;

struct SessionResult {
  bool ok = false;                    // the session ran to its end without a refusal (a script FAULT is not a refusal: the host continues)
  std::string refusal;                // set when !ok
  std::uint64_t frames = 0;           // frames presented
  std::vector<std::string> frame_sha256;   // one per frame
  std::string session_sha256;         // SHA-256 over the frame digests in order
  std::vector<std::uint8_t> last_frame;    // RGB24 of the last frame (kFixtureWidth x kFixtureHeight)
  std::vector<std::string> log;       // sega.log lines (bounded)
  std::vector<Note> notes;            // non-fatal diagnostics (bounded)
  Fault fault = Fault::none;          // the mod's fault, if it faulted
  std::uint64_t fault_frame = 0;
  std::string diagnostic;             // `E_MOD_FAULT cause=... mod=... callback=... frame=...: ...` (phases section 6.1); empty without a fault
  bool nondeterministic = false;      // a watchdog fault: the run is not replay-verifiable
  std::uint64_t overlay_frames = 0;   // frames that carried at least one overlay request
  std::uint64_t leaked_bytes = 0;     // allocator bytes still held after teardown (0 = leak free)
};
// `package` is the verified package, `compiled` its trusted compile result (the only code the VM accepts).
[[nodiscard]] SessionResult run_fixture_session(const mod::Package &package, const CompiledMod &compiled, std::string_view image_sha256, std::uint64_t frames);

}  // namespace segarecomp::luau
