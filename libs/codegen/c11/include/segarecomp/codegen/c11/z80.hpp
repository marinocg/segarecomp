#pragma once

// Z80 image-level C11 emission (SEG-008-T003; ADR 0058 sections 1-6, ADR 0071). Broad immutable-image AOT: one exact entry per
// instruction start of every code image, classified by the cpu_z80 logical-fetch decoder. Consecutive entries of one image share
// a bounded host function (an owner, at most kOwnerGroupEntries entries) that selects its entry from the PC or the window
// offset; PC-independent instruction effects are emitted once as shared functions (SEG-033).
//
//   full entry         a decoded start whose form has a lowering row (z80_lowering.hpp)
//   prefix_lock entry  a start of an endless DD/FD run
//   typed stub entry   `mutable_code`, `unresolved_fetch_mapping` (and the reserved `excluded_form`) starts
//   (no entry)         a decoded start whose form has no lowering row yet: dispatch fails closed with `no_owner`
//
// Absolute-PC owners serve statically invariant windows and banked images admissible in exactly one window.
// A banked image admissible in several windows gets one window-relative entry per offset that derives every
// PC-dependent value from the run-time window base. Direct owner-to-owner binding exists only from an invariant
// window owner to an invariant-window successor; everything else returns to the generated dispatcher `z80_run`,
// which asks the host for the current code-image identity and looks up (identity, PC) or (identity, PC - base)
// exactly through the generic compiled entry table.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "segarecomp/cpu/z80/decode.hpp"

namespace segarecomp::codegen::z80 {

enum class ImageKind : std::uint8_t {
  invariant,  // statically invariant window: exactly one window, absolute-PC owners, direct binding allowed
  banked,     // mapping-sensitive: one or more admissible windows of identical stride and exposed range
};

// Image offset `o` (first_offset <= o < first_offset + length) is exposed at logical address `base + o`.
struct CodeWindow {
  std::uint16_t base = 0;
  std::uint32_t first_offset = 0;
  std::uint32_t length = 0x10000;
};

struct CodeImage {
  std::uint32_t identity = 0;  // unique, <= 0xFFFF (the high half of the 32-bit entry key)
  ImageKind kind = ImageKind::banked;
  std::vector<std::uint8_t> bytes;
  std::vector<CodeWindow> windows;
  // RAM-backed image (SEG-032-T003, ADR 0073): the bytes are a snapshot of memory the platform may change after
  // compilation. Every entry verifies its exact 1-4 static instruction bytes against the live memory (host `code_matches`)
  // right after the boundary prologue and stops with `Z80_ERROR_CODE_MISMATCH` on any difference. Requires a banked image with
  // exactly one window (the platform reports which image is bound, so no direct binding or in-group chaining exists), and
  // emits endless DD/FD runs as typed `mutable_code` stubs. Immutable images (false) emit exactly as before.
  bool live_bytes = false;
};

struct ImageSet {
  std::vector<CodeImage> images;
};

enum class OwnerKind : std::uint8_t { full, prefix_lock, stub_mutable_code, stub_unresolved_fetch_mapping, stub_excluded_form };
const char* owner_kind_name(OwnerKind kind);

struct OwnerRecord {
  std::uint32_t identity = 0;
  std::uint16_t key = 0;             // logical address (absolute owner) or offset within the window (window-relative)
  bool window_relative = false;
  OwnerKind kind = OwnerKind::full;  // kind of the first variant
  std::uint32_t variants = 1;        // > 1: the owner selects among body variants from the run-time window base
  cpu::z80::FormId form = cpu::z80::kNoForm;
  bool bound_successor = false;      // fall-through is a direct owner-to-owner return
  std::uint32_t group_entries = 1;   // exact entries sharing this start's host owner function (1 = a function of its own)
};

// Entries per host owner function (SEG-033-T002). One exact start maps to its owner through the unchanged generic entry
// table; consecutive starts of one image share a bounded owner that selects its entry from the PC (absolute-PC owners) or the
// window offset (window-relative owners). 1 reproduces the historical one-function-per-start emission byte for byte.
// The production bound is 128 (SEG-033-T002 sweep over 16/32/64/128): host compile
// CPU and object code roughly halve versus one function per start and do not improve beyond it. It is a constant, not a user
// option.
inline constexpr std::size_t kOwnerGroupEntries = 128;

struct EmitOptions {
  std::filesystem::path directory;
  std::string stem = "z80_image";
  std::string runtime_include = "segarecomp/codegen/c11/runtime/z80_runtime.h";
  bool record_owners = false;  // fill EmitResult::owners (large for full images)
  // Entry-table chunk size: an image with more owners than this splits its exact-lookup table into per-TU chunks (bounded
  // TU size and compiler memory, ADR 0058). The default only matters for images above 64 Ki owners; tests lower it to exercise
  // the chunk path on a small image.
  std::size_t entry_chunk_entries = 65536;
  bool share_bodies = true;  // PC-independent instruction effects are emitted once and called (SEG-033-T005); false = reference
  std::size_t owner_group_entries = kOwnerGroupEntries;  // >= 1; tests and the differential gate pin 1 (reference) vs N
};

// full_/prefix_lock_/stub_owners, variant_owners and bound_successors are legacy names: they count exact entries. `entries` is the
// entry-table size and `owners` the number of host owner functions.
struct EmitStats {
  std::size_t shared_bodies = 0;     // distinct shared effect functions
  std::size_t entries = 0;           // exact starts with an owner binding (the entry-table size)
  std::size_t owners = 0;            // host owner functions
  std::size_t max_group_entries = 0; // largest group
  std::size_t full_owners = 0;
  std::size_t prefix_lock_owners = 0;
  std::size_t stub_owners = 0;
  std::size_t unlowered_starts = 0;  // decoded starts without a lowering row (no owner emitted)
  std::size_t variant_owners = 0;    // window-relative owners with a per-base switch
  std::size_t bound_successors = 0;
  std::size_t translation_units = 0;
};

struct EmitResult {
  std::string error;  // empty on success; nothing is left behind on failure
  EmitStats stats;
  std::vector<OwnerRecord> owners;
};

EmitResult emit_image_set(const ImageSet& images, const EmitOptions& options);

}  // namespace segarecomp::codegen::z80
