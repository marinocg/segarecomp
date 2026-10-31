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
#include <optional>
#include <span>
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

// SEG-115 (ADR 0259): a statically justified alternative value of one byte of a RAM-backed image (image offset, value).
struct CodeAlternative {
  std::uint32_t offset = 0;
  std::uint8_t value = 0;
};
// Alternative forms one entry may carry besides its snapshot form; an image whose alternatives would exceed it loses ALL of them (fail closed).
inline constexpr std::size_t kMaxAlternativeFormsPerEntry = 8;

struct CodeImage {
  std::uint32_t identity = 0;  // unique, <= 0xFFFF (the high half of the 32-bit entry key)
  ImageKind kind = ImageKind::banked;
  std::vector<std::uint8_t> bytes;
  std::vector<CodeWindow> windows;
  // RAM-backed image (SEG-032-T003, ADR 0073): the bytes are a snapshot of memory the platform may change after
  // compilation. Every entry (SEG-032-T012) snapshots its 1-4 live instruction bytes through the host `code_fetch` after the
  // boundary check and before any instruction-begin bookkeeping, verifies the statically defining bytes (prefixes, opcode, every
  // byte that is not a descriptor displacement/immediate payload) against the compiled ones and stops with
  // `Z80_ERROR_CODE_MISMATCH` on any difference; payload bytes are read from the snapshot. Requires a banked image with
  // exactly one window (the platform reports which image is bound, so no direct binding or in-group chaining exists), and
  // emits endless DD/FD runs as typed `mutable_code` stubs. Immutable images (false) emit exactly as before.
  bool live_bytes = false;
  // RAM-backed images only: finite alternative byte values (sorted by (offset, value), unique, offsets inside the bytes). Besides the form the
  // snapshot decodes to, every entry that covers an alternative byte also compiles the forms that the existing frontend decodes from each
  // combination of the alternatives, selecting among them by the live structural bytes before any effect. Mirrors are explicit (the producer lists
  // each offset). Empty = the plain guard, byte for byte.
  std::vector<CodeAlternative> alternatives;
};

struct ImageSet {
  std::vector<CodeImage> images;
};

// Selective admission (SEG-057, ADR 0118). An optional, explicitly supplied set of instruction starts per code image. The emitter
// emits exactly the owners of the admitted starts that have a lowering row; every other exposed start is rejected at run time with
// `Z80_ERROR_UNADMITTED_PC` (state.pc = that start, no instruction effect). Without a plan the emission is the broad immutable-image
// AOT, byte for byte. The plan is a structural subset claim: it never asserts that the omitted starts are unreachable (execution
// completeness) and a producer such as a learned ranker is not the correctness authority. The emitter validates the plan fail-closed:
//   * every image of the set appears exactly once, no plan entry names an unknown identity;
//   * `byte_count`/`content_digest` equal the image the plan is applied to (a plan made for other bytes is rejected);
//   * `offsets` are strictly increasing image offsets, each exposed by at least one window of the image.
struct ImageAdmission {
  std::uint32_t identity = 0;
  std::uint32_t byte_count = 0;
  std::uint64_t content_digest = 0;
  std::vector<std::uint32_t> offsets;  // admitted image offsets (the owner key space: base + offset is the logical address)
};

struct AdmissionPlan {
  std::vector<ImageAdmission> images;
};

// FNV-1a 64 over the image bytes: a binding checksum against applying a plan to other bytes, not a security primitive.
[[nodiscard]] std::uint64_t image_content_digest(std::span<const std::uint8_t> bytes);
// Binds `offsets` to `image` (identity, size, digest). Offsets are validated by the emitter, not here.
[[nodiscard]] ImageAdmission bind_admission(const CodeImage& image, std::vector<std::uint32_t> offsets);

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
  std::optional<AdmissionPlan> admission;  // selective emission (above); nullopt = broad AOT, the default
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
  // Selective emission only (zero when broad). `unlowered_starts` then counts admitted starts without a lowering row.
  std::size_t admitted_offsets = 0;  // offsets named by the plan
  std::size_t omitted_offsets = 0;   // exposed offsets the plan omitted
  std::size_t omitted_ranges = 0;    // maximal runs of omitted offsets (size of the generated rejection table)
  // SEG-115: entries with alternative forms, the forms they add, and images whose alternatives were dropped for exceeding the per-entry bound.
  std::size_t alternative_entries = 0;
  std::size_t alternative_forms = 0;
  std::size_t alternative_images_dropped = 0;
};

struct EmitResult {
  std::string error;  // empty on success; nothing is left behind on failure
  EmitStats stats;
  std::vector<OwnerRecord> owners;
};

EmitResult emit_image_set(const ImageSet& images, const EmitOptions& options);

}  // namespace segarecomp::codegen::z80
