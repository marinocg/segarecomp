#pragma once

// Sega CD verified disc edits and main-M68K guest patching, host side (ADR 0287 contract, ADR 0328 implementation; SEG-148-T002/T003).
// A plan is a pure function of the verified original disc and the package: every assertion is made before one output byte exists. Materialization only READS the original files and writes a
// new, layout-identical image set into a new folder. Nothing here runs a guest, a compiler or a payload; code eligibility is judged against disc-source evidence the caller obtained from the
// product's own Sega CD build of the ORIGINAL disc. Every rejection is a stable class: `E_DISC_*`, `E_GUEST_*`, or `unsupported_code_class:*`.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/mod.hpp"

namespace segarecomp::mod {

inline constexpr std::uint64_t kDiscBootAreaBytes = 0x8000;   // the system area: the `boot` module context

// Replacement bytes at an offset of one CUE FILE.
struct DiscWrite {
  std::uint32_t file = 0;
  std::uint64_t offset = 0;
  std::vector<std::uint8_t> bytes;
};

// One contiguous piece of a write inside the logical user data of track 1 (the offset space of the build's disc extents). A `code` span is a declared main-M68K code patch: it is always the
// WHOLE write, and `address` is where the guest runs its first byte.
struct DiscSpan {
  std::uint64_t offset = 0, length = 0;
  std::uint32_t address = 0;
  bool code = false;
  std::string origin;   // "edit <payload path>" | "injected block (guest-asm line N)"
};

struct DiscPlan {
  bool ok = false;
  Rejection rejection;
  std::vector<DiscWrite> writes;   // ordered by (file, offset), never overlapping
  std::vector<DiscSpan> spans;
  std::vector<Symbol> symbol_uses;   // the symbols the injected code names (their module context is checked against the evidence)
  unsigned edits_applied = 0, blocks_injected = 0;
  std::uint64_t bytes_written = 0;
  [[nodiscard]] bool has_code() const;
};

// `files` are the verified payloads of `manifest`; `disc` is the user's original disc. Classes: E_DISC_IDENTITY, E_DISC_RANGE, E_DISC_PREGAP, E_DISC_AUDIO_TARGET, E_DISC_ISO_FILE,
// E_DISC_SECTOR_HEADER, E_DISC_ORIGINAL_MISMATCH, E_DISC_DELTA, E_DISC_PAYLOAD_SIZE, E_DISC_OVERLAP, E_GUEST_*, unsupported_code_class:layout_changing | not_disc_proven.
[[nodiscard]] DiscPlan plan_disc_patch(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files, const DiscImage &disc);

// Loads a CUE with the disc loader; `INDEX 02` and later are E_DISC_INDEX_UNSUPPORTED, every other loader failure E_DISC_LOAD.
struct DiscOpened {
  std::optional<DiscImage> disc;
  Rejection rejection;
};
[[nodiscard]] DiscOpened open_disc(const std::filesystem::path &cue);

// The layout rules of sega-cd-mod-sources.md section 4: E_DISC_FILE_SIZE, E_DISC_TRACK_COUNT, E_DISC_TRACK_ORDER, E_DISC_SECTOR_SIZE, E_DISC_TRACK_LENGTH, E_DISC_PREGAP.
[[nodiscard]] std::optional<Rejection> check_disc_layout(const DiscImage &original, const DiscImage &effective);

// `effective` is exactly `original` plus the planned writes: same layout, and every other byte unchanged (E_DISC_UNDECLARED_CHANGE; E_DISC_IDENTITY if a file changed while being read).
[[nodiscard]] std::optional<Rejection> verify_patched_disc(const DiscPlan &plan, const DiscImage &original, const DiscImage &effective);

// Writes the effective image set into `out` (which must not exist, or be an empty folder, and may not be the original's folder or lie inside it): the CUE copied byte for byte and one file per
// FILE, streamed. The result is re-loaded and verified before it is published under `out`; a failure leaves nothing there. E_DISC_OUTPUT and the verification classes.
struct DiscMaterialized {
  bool ok = false;
  Rejection rejection;
  std::filesystem::path cue;
  std::string effective_identity;
};
[[nodiscard]] DiscMaterialized materialize_disc(const DiscPlan &plan, const DiscImage &original, const std::filesystem::path &original_cue, const std::filesystem::path &out);

// ---- code eligibility (SEG-148-T003) ----
// What a Sega CD build proved: executable extents that are exact copies of track-1 disc bytes (`origin` boot | static | resolved), and the destinations of executable images that are NOT disc
// bytes (made or observed in RAM). Only the first kind makes a byte patchable.
struct DiscProof {
  // cpu "main" | "sub". An origin "boot_contradicted" (the build saw boot-area bytes the boot contract does not place there) never authorizes a patch but still counts for the refusals.
  std::string cpu, origin;
  std::uint64_t offset = 0, length = 0;
  std::uint32_t base = 0;
};
struct DiscDynamic {
  std::string cpu, kind;
  std::uint32_t base = 0;
  std::uint64_t length = 0;
};
struct DiscEvidence {
  std::vector<DiscProof> proofs;
  std::vector<DiscDynamic> dynamic;
};
// Every planned write against the evidence. A write overlapping a proven sub-CPU extent is `unsupported_code_class:sub_m68k`; an UNDECLARED write over a proven main extent is
// `:undeclared_code`; a declared main patch needs one proven main extent that holds all its bytes and places the first at its load address (`:dynamic` when that address is only reached by a
// non-disc image, else `:not_disc_proven`); an injected @symbol's module context must match the evidence (`:unrecognized_module`). `disc` resolves ISO9660 module paths.
[[nodiscard]] std::optional<Rejection> check_disc_eligibility(const DiscPlan &plan, const DiscImage &disc, const DiscEvidence &evidence);

}  // namespace segarecomp::mod
