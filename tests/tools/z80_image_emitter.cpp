// SEG-008-T003: test-side CLI over the public Z80 image emitter. It reads a small line-based image spec and emits the
// sharded generated C, so hermetic tests, the conformance harness and the coverage tool never link production
// internals.
//
//   z80_image_emitter <spec> <outdir> <stem> [--list]
//   spec verbs `group <n>`: entries per host owner function (1 = reference mode); `share <0|1>`: shared effect bodies
//   (0 = reference); `chunk <n>`: entry-table chunk size
//
// spec lines (`#` starts a comment):
//   image  <identity> invariant|banked
//   window <identity> <base-hex> <first-offset-hex> <length-hex>
//   bytes  <identity> <hex-bytes>          (appended; may repeat)
//   fill   <identity> <byte-hex> <count-hex>
//   live   <identity>                      (SEG-032-T003: mark a banked one-window image RAM-backed: every entry is byte-guarded)
//   alt    <identity> <offset-hex> <value-hex>  (SEG-115: an alternative byte value of a RAM-backed image; the mirror at offset + 0x2000 is added too)
//   selective 0                            (SEG-057: selective emission; an image without `admit` lines admits nothing)
//   admit  <identity> <lo-hex> [<hi-hex>]  (admit image offsets [lo, hi), or just lo; repeatable, merged, sorted)
//   admit_bad_digest <identity>            (test hook: corrupt the plan's content digest of that image)
//   admit_unsorted <identity> <hex>...     (test hook: replace that image's offsets by the literal, unsorted list)
//   admit_omit_image <identity>            (test hook: leave that image out of the plan)
//   admit_extra_image <identity>           (test hook: add a plan entry for an identity the image set does not contain)
//   admit_duplicate_image <identity>       (test hook: list that image's plan entry twice)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "segarecomp/codegen/c11/z80.hpp"

using namespace segarecomp::codegen::z80;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: z80_image_emitter <spec> <outdir> <stem> [--list]\n";
    return 2;
  }
  const bool list = argc > 4 && std::string(argv[4]) == "--list";
  std::ifstream spec(argv[1]);
  if (!spec) {
    std::cerr << "cannot read spec\n";
    return 2;
  }
  ImageSet set;
  std::size_t entry_chunk = 0;
  std::size_t owner_group = 0;
  int share_bodies = -1;  // -1: the emitter default
  bool selective = false;
  std::map<unsigned long, std::set<std::uint32_t>> admitted;
  std::set<unsigned long> bad_digest, omit_image, extra_image, duplicate_image;
  std::map<unsigned long, std::vector<std::uint32_t>> literal_offsets;
  std::map<unsigned long, std::size_t> index;
  std::map<unsigned long, std::set<std::pair<std::uint32_t, std::uint8_t>>> alternatives;  // SEG-115
  const auto image_of = [&](unsigned long identity) -> CodeImage& { return set.images.at(index.at(identity)); };
  for (std::string line; std::getline(spec, line);) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string verb;
    if (!(in >> verb)) continue;
    unsigned long identity = 0;
    in >> identity;
    if (verb == "image") {
      std::string kind;
      in >> kind;
      CodeImage image;
      image.identity = static_cast<std::uint32_t>(identity);
      image.kind = kind == "invariant" ? ImageKind::invariant : ImageKind::banked;
      index[identity] = set.images.size();
      set.images.push_back(std::move(image));
    } else if (verb == "window") {
      std::string base, first, length;
      in >> base >> first >> length;
      image_of(identity).windows.push_back({static_cast<std::uint16_t>(std::stoul(base, nullptr, 16)),
                                            static_cast<std::uint32_t>(std::stoul(first, nullptr, 16)),
                                            static_cast<std::uint32_t>(std::stoul(length, nullptr, 16))});
    } else if (verb == "bytes") {
      std::string hex;
      in >> hex;
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        image_of(identity).bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    } else if (verb == "chunk") {  // `chunk <entries>`: entry-table chunk size (test hook; the identity slot holds the count)
      entry_chunk = static_cast<std::size_t>(identity);
    } else if (verb == "group") {  // `group <entries>`: entries per host owner (1 = the reference one-function-per-start mode)
      owner_group = static_cast<std::size_t>(identity);
    } else if (verb == "share") {  // `share <0|1>`: shared PC-independent effect bodies (0 = the reference emission)
      share_bodies = static_cast<int>(identity);
    } else if (verb == "selective") {
      selective = true;
    } else if (verb == "admit") {
      std::string lo, hi;
      in >> lo >> hi;
      const auto first = static_cast<std::uint32_t>(std::stoul(lo, nullptr, 16));
      const auto last = hi.empty() ? first + 1 : static_cast<std::uint32_t>(std::stoul(hi, nullptr, 16));
      for (std::uint32_t o = first; o < last; ++o) admitted[identity].insert(o);
    } else if (verb == "admit_bad_digest") {
      bad_digest.insert(identity);
    } else if (verb == "admit_extra_image") {
      extra_image.insert(identity);
    } else if (verb == "admit_duplicate_image") {
      duplicate_image.insert(identity);
    } else if (verb == "admit_omit_image") {
      omit_image.insert(identity);
    } else if (verb == "admit_unsorted") {
      std::string value;
      while (in >> value) literal_offsets[identity].push_back(static_cast<std::uint32_t>(std::stoul(value, nullptr, 16)));
    } else if (verb == "live") {
      image_of(identity).live_bytes = true;
    } else if (verb == "alt") {  // `alt <identity> <offset> <value>`: both 8 KiB mirrors of a 16 KiB RAM window
      std::string offset, value;
      in >> offset >> value;
      const auto o = static_cast<std::uint32_t>(std::stoul(offset, nullptr, 16));
      const auto v = static_cast<std::uint8_t>(std::stoul(value, nullptr, 16));
      alternatives[identity].emplace(o, v);
      alternatives[identity].emplace(o + 0x2000U, v);
    } else if (verb == "fill") {
      std::string value, count;
      in >> value >> count;
      image_of(identity).bytes.insert(image_of(identity).bytes.end(), std::stoul(count, nullptr, 16),
                                      static_cast<std::uint8_t>(std::stoul(value, nullptr, 16)));
    } else {
      std::cerr << "unknown spec verb: " << verb << "\n";
      return 2;
    }
  }
  for (CodeImage& image : set.images)
    for (const auto& [offset, value] : alternatives[image.identity]) image.alternatives.push_back(CodeAlternative{offset, value});
  EmitOptions options;
  options.directory = argv[2];
  options.stem = argv[3];
  options.record_owners = list;
  if (entry_chunk != 0) options.entry_chunk_entries = entry_chunk;
  if (owner_group != 0) options.owner_group_entries = owner_group;
  if (share_bodies >= 0) options.share_bodies = share_bodies != 0;
  if (selective) {
    AdmissionPlan plan;
    for (const CodeImage& image : set.images) {
      if (omit_image.contains(image.identity)) continue;
      const auto& wanted = admitted[image.identity];
      std::vector<std::uint32_t> offsets = literal_offsets.contains(image.identity)
                                               ? literal_offsets[image.identity]
                                               : std::vector<std::uint32_t>(wanted.begin(), wanted.end());
      ImageAdmission entry = bind_admission(image, std::move(offsets));
      if (bad_digest.contains(image.identity)) entry.content_digest ^= 1;
      if (duplicate_image.contains(image.identity)) plan.images.push_back(entry);
      plan.images.push_back(std::move(entry));
    }
    for (const unsigned long identity : extra_image) {
      ImageAdmission entry;
      entry.identity = static_cast<std::uint32_t>(identity);
      plan.images.push_back(std::move(entry));
    }
    options.admission = std::move(plan);
  }
  const EmitResult result = emit_image_set(set, options);
  if (!result.error.empty()) {
    std::cout << "error " << result.error << "\n";
    return 1;
  }
  std::printf("stats full=%zu prefix_lock=%zu stubs=%zu unlowered=%zu variants=%zu bound=%zu units=%zu\n",
              result.stats.full_owners, result.stats.prefix_lock_owners, result.stats.stub_owners,
              result.stats.unlowered_starts, result.stats.variant_owners, result.stats.bound_successors,
              result.stats.translation_units);
  std::printf("alternatives entries=%zu forms=%zu dropped=%zu\n", result.stats.alternative_entries, result.stats.alternative_forms,
              result.stats.alternative_images_dropped);
  std::printf("shape entries=%zu owners=%zu max_group=%zu shared_bodies=%zu\n", result.stats.entries, result.stats.owners,
              result.stats.max_group_entries, result.stats.shared_bodies);
  if (selective)
    std::printf("admission admitted=%zu omitted=%zu runs=%zu\n", result.stats.admitted_offsets, result.stats.omitted_offsets,
                result.stats.omitted_ranges);
  for (const OwnerRecord& r : result.owners) {
    std::printf("owner %u %04X %s %u %s %d %d\n", r.identity, r.key, owner_kind_name(r.kind), r.variants,
                r.form == segarecomp::cpu::z80::kNoForm ? "-" : segarecomp::cpu::z80::form_name(r.form).c_str(),
                r.window_relative ? 1 : 0, r.bound_successor ? 1 : 0);
  }
  return 0;
}
