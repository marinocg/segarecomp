#include "sega_cd_aot.hpp"

#include <sstream>

#include "segarecomp/machine/genesis/hybrid_admission.hpp"
#include "segarecomp/machine/genesis/ml_region_gen2_producer.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp_app {

namespace scd = segarecomp::machine::sega_cd;

bool set_sega_cd_aot_option(SegaCdAotOptions &options, std::string_view key, std::string_view value) {
  if (key == "--aot-policy") {
    if (value != "compatibility" && value != "optimized") return false;
    options.policy = std::string(value);
    return true;
  }
  if (key == "--aot-scope") {
    if (value != "all-images") return false;
    options.all_images = true;
    return true;
  }
  if (key == "--aot-model") {
    if (value != "gen2" || options.model_given) return false;  // the Sega CD experiment reuses the frozen gen2 model only
    options.model_given = true;
    return true;
  }
  return false;
}

bool sega_cd_aot_options_valid(const SegaCdAotOptions &options) { return !((options.model_given || options.all_images) && !options.optimized()); }

std::string sega_cd_aot_producer_failure() { return segarecomp::genesis_ml2_model_identity_failure(); }

std::string sega_cd_aot_policy_identity(const SegaCdAotOptions &options) {
  // Compatibility keeps its original identity text (the v1 contract name): nothing about it changed. The experimental policy carries the v2 contract and its scope.
  std::string text = "segarecomp.sega_cd.aot_policy_identity.v1|contract=" +
                     std::string(options.optimized() ? scd::kSelectiveAdmissionContract : scd::kSelectiveAdmissionContractV1) + "|requested=" + options.policy;
  if (options.optimized()) {
    text += std::string("|scope=") + (options.all_images ? "all_images" : "game_modules");
    const auto &id = segarecomp::genesis_ml2_identity();
    text += "|family=gen2|producer=ml_region_gen2|model=" + std::string(id.model_sha256) + "|schema=" + std::string(id.representation_sha256) +
            "|tokens=" + std::string(id.token_schema_sha256) + "|rank=" + std::string(id.policy_id) + ":" + std::string(id.rank_plan_sha256) +
            "|weights=" + std::string(id.weight_table_sha256);
  }
  return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

std::optional<scd::SelectiveAdmissionRequest> make_sega_cd_aot_request(const SegaCdAotOptions &options) {
  if (!options.optimized()) return std::nullopt;
  scd::SelectiveAdmissionRequest request;
  request.policy_identity = sega_cd_aot_policy_identity(options);
  request.all_images = options.all_images;
  request.propose = [](std::span<const std::uint8_t> bytes, std::span<const std::uint32_t> seeds) {
    scd::RegionProposal out;
    // The scoring unit is the image's immutable source bytes (no address, mapping or base is an input): ADR 0102's evaluator, unchanged.
    const std::string sha = segarecomp::sha256_hex(bytes);
    auto result = segarecomp::propose_genesis_ml2_executable_regions(bytes, sha, seeds);
    out.units = static_cast<std::uint32_t>(result.stats.cells);
    out.selected = static_cast<std::uint32_t>(result.stats.ml_selected);
    out.seed_units = static_cast<std::uint32_t>(result.stats.seed_cells);
    if (!result.proposal) {
      out.failure = result.failure.empty() ? "empty_proposal" : result.failure;
      return out;
    }
    for (const auto &range : result.proposal->ranges) out.ranges.emplace_back(range.begin_address, range.end_address);
    out.ok = true;
    return out;
  };
  return request;
}

std::string sega_cd_aot_policy_json(const SegaCdAotOptions &options, const scd::SelectiveAdmissionOutcome &o) {
  const auto &id = segarecomp::genesis_ml2_identity();
  std::ostringstream out;
  out << "{\"requested\":\"" << options.policy << "\",\"experimental\":" << (options.optimized() ? "true" : "false") << ",\"effective\":\""
      << (o.admitted ? "selective" : "broad") << "\",\"fallback\":" << (options.optimized() && !o.admitted ? "true" : "false") << ",\"reason\":\""
      << (options.optimized() ? o.reason : std::string("none")) << "\",\"failed_image\":\"" << o.failed_image << "\",\"policy_identity\":\""
      << sega_cd_aot_policy_identity(options) << "\",\"contract\":\"" << (options.optimized() ? scd::kSelectiveAdmissionContract : scd::kSelectiveAdmissionContractV1) << "\"";
  if (options.optimized())
    out << ",\"model_family\":\"gen2\",\"model\":\"" << id.model_sha256 << "\",\"rank_policy\":\"" << id.policy_id << "\",\"rank_plan\":\""
        << id.rank_plan_sha256 << "\",\"weights\":\"" << id.weight_table_sha256 << "\"";
  out << ",\"k_sha256\":\"" << (o.admitted ? o.k_sha256 : std::string()) << "\",\"broad_entries\":" << o.broad_entries
      << ",\"admitted_entries\":" << o.admitted_entries;
  if (options.optimized())   // the Compatibility report keeps exactly its earlier keys
    out << ",\"scope\":\"" << (options.all_images ? "all_images" : "game_modules") << "\",\"m3_images\":" << o.m3_images << ",\"m3_broad_entries\":" << o.m3_broad_entries
        << ",\"m3_admitted_entries\":" << o.m3_admitted_entries;
  out << ",\"images\":[";
  for (std::size_t i = 0; i < o.images.size(); ++i) {
    const auto &r = o.images[i];
    out << (i ? "," : "") << "{\"processor\":\"" << scd::processor_name(r.processor) << "\",\"name\":\"" << r.name << "\",\"selection\":\"" << scd::image_selection_name(r.selection)
        << "\",\"selection_reason\":\"" << r.selection_reason << "\",\"source_bytes\":" << r.source_bytes
        << ",\"units\":" << r.units << ",\"selected\":" << r.selected << ",\"seed_units\":" << r.seed_units << ",\"proposed_bytes\":" << r.proposed_bytes
        << ",\"broad_entries\":" << r.broad_entries << ",\"admitted_entries\":" << r.admitted_entries << "}";
  }
  out << "]}";
  return out.str();
}

}  // namespace segarecomp_app
