#pragma once

// SEG-097-T003 (ADR 0208): the Sega CD seam for an experimental, model-PROPOSED selective admission.
//
// Broad AOT (the default, "Compatibility") compiles everything the executable-image authority justifies: statically reached instructions of
// the synthetic profile, every aligned start of the BIOS-image profile. A selective policy may OMIT instructions; it can never add one, widen an
// image, add a mapping or decide that bytes are executable. This header holds only the interface: a producer callback that, for ONE image's
// immutable source bytes, returns the source-relative ranges it proposes (the frozen gen2 M3 + B35 producer lives in apps/segarecomp, so this
// machine library depends on no other machine and on no model). The emitters apply the proposal, validate it, and on any failure fall back to
// the broad emission and say why; an experimental result is never claimed when the effective build is broad.
//
// The scoring unit is the image, not the mapping: one image mapped at several bases is scored once and its admitted source-relative offsets are
// shared by every mapping (and, with position-independent bodies, by every body).

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

// Bump on any change to how proposals are applied or validated (it enters the policy identity).
// v2 (SEG-097 follow-up, ADR 0208): the proposal applies ONLY to independently admitted disc-loaded game modules and extents; firmware, BIOS-derived images,
// the boot IP/SP and boot-area extents stay under broad AOT. v1 (every image) is the historical M1 reference, reachable only with `all_images`.
inline constexpr const char *kSelectiveAdmissionContract = "segarecomp.sega_cd.selective_admission.v2";
inline constexpr const char *kSelectiveAdmissionContractV1 = "segarecomp.sega_cd.selective_admission.v1";

struct RegionProposal {
  bool ok = false;
  std::string failure;  // stable sanitized reason when !ok: model_identity | rom_size | token_range | non_finite_score | empty_proposal
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // ascending, disjoint, even [begin, end) source offsets: the proposal R
  std::uint32_t units = 0;       // decision units of the image (cells)
  std::uint32_t selected = 0;    // units selected by the rank policy before the seed union
  std::uint32_t seed_units = 0;  // units holding a declared certain-code seed
};

// `seed_offsets` are source offsets of certain code (declared entry roots): the producer must include their units.
using RegionProposer = std::function<RegionProposal(std::span<const std::uint8_t> bytes, std::span<const std::uint32_t> seed_offsets)>;

struct SelectiveAdmissionRequest {
  bool all_images = false;  // HISTORICAL M1 scope (every image is scored); evidence tooling only, never the product default
  RegionProposer propose;
  std::string policy_identity;  // ROM-independent identity of the producer + analysis contract; enters the program identity when requested
};

// How one image was treated (typed): m3_selected = scored by the producer and filtered; broad_required = eligible for nothing but broad AOT by its role/provenance;
// broad_fallback = it was eligible but the producer/validator failed (the whole build then falls back, so this appears only in a fallback report).
enum class ImageSelection : std::uint8_t { broad_required, m3_selected, broad_fallback };
[[nodiscard]] const char *image_selection_name(ImageSelection selection) noexcept;

struct ImageAdmissionReport {
  ImageSelection selection{ImageSelection::broad_required};
  std::string selection_reason;         // the role / provenance that decided it, e.g. boot_ip, bios_firmware, disc_module
  Processor processor{Processor::main};
  std::string name;
  std::uint32_t image_id = 0;
  std::uint32_t source_bytes = 0;
  std::uint32_t units = 0, selected = 0, seed_units = 0;
  std::uint32_t proposed_bytes = 0;     // bytes of the proposal R
  std::uint32_t broad_entries = 0;      // instruction entries the broad emission compiles for this image (all mappings)
  std::uint32_t admitted_entries = 0;   // and the selective emission (0 when it fell back)
};

struct SelectiveAdmissionOutcome {
  bool requested = false;
  bool admitted = false;          // the effective emission is selective
  std::string reason = "none";    // none | a producer failure | validator_rejected:<detail>
  std::string failed_image;       // the image whose proposal failed or was rejected (a platform-assigned name, never content)
  std::string k_sha256;           // digest of the admitted (image, source-offset) set when admitted
  std::vector<ImageAdmissionReport> images;
  std::uint32_t broad_entries = 0;     // all images
  std::uint32_t admitted_entries = 0;  // all images (broad_required images count as broad-admitted here)
  std::uint32_t m3_images = 0;         // images actually scored
  std::uint32_t m3_broad_entries = 0;  // entries of the scored images only: what the optimization could touch
  std::uint32_t m3_admitted_entries = 0;
};

// Shared validation of one proposal against its image (build time, pure). Empty string = valid.
[[nodiscard]] std::string validate_region_proposal(const RegionProposal &proposal, std::uint32_t image_bytes, std::span<const std::uint32_t> seed_offsets);
[[nodiscard]] bool ranges_contain(const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges, std::uint32_t offset) noexcept;
// SHA-256 over a canonical text of (processor, image name, ascending admitted source offsets) rows.
[[nodiscard]] std::string admitted_set_digest(const std::vector<std::pair<std::string, std::vector<std::uint32_t>>> &rows);

}  // namespace segarecomp::machine::sega_cd
