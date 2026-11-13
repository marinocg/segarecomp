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

// ---- copy-in / copy-out records ----
struct FrameValue {
  std::string name;        // [a-z0-9_.], at most 32 bytes
  std::int64_t value = 0;  // |value| <= 2^53 (an integer a script can hold exactly)
};
struct FrameInput {
  std::uint64_t number = 0;
  std::vector<FrameValue> values;   // the read-only values the host publishes for this frame (at most 32)
};
struct OverlayOp {
  enum class Kind { rect, text } kind = Kind::rect;
  int x = 0, y = 0, w = 0, h = 0;   // already clamped to the overlay
  std::uint32_t rgba = 0;            // 0xRRGGBBAA
  std::string text;                  // printable ASCII, at most 64 bytes
};
// A non-fatal diagnostic of a callback (a clamp or a truncation). `cause` is a stable class: "log_truncated", "text_truncated", "overlay_clamped", "overlay_clipped".
struct Note {
  std::string cause, message;
};
// What one successful callback produced. A faulted callback produces nothing (staged output is discarded, phases section 6.1).
struct Output {
  std::vector<std::string> log;
  std::vector<OverlayOp> overlay;
  std::vector<Note> notes;
};

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

// ---- overlay rasterization (the fixed HUD overlay path) ----
struct OverlayImage {
  int width = kOverlayWidth, height = kOverlayHeight;
  std::vector<std::uint8_t> rgba;   // width * height * 4, tightly packed
};
// Source-over compositing of the operations in order, integer arithmetic only: identical on every host. Text uses the viewer's body bitmap font (sv_font.h), left aligned at (x, y).
[[nodiscard]] OverlayImage rasterize_overlay(std::span<const OverlayOp> ops);

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
