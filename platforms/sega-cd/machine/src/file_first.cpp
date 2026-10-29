#include "segarecomp/machine/sega_cd/file_first.hpp"

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

#include "segarecomp/iso9660.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

const char *file_first_policy_name(FileFirstPolicy policy) noexcept { return policy == FileFirstPolicy::all ? "all" : "structural"; }

FileFirstSelection select_file_first_images(const DiscImage &disc, FileFirstPolicy policy, const FileFirstLimits &limits) {
  FileFirstSelection out;
  out.policy = std::string("file_first.v1:") + file_first_policy_name(policy) + ":max_bytes=" + std::to_string(limits.max_file_bytes) +
               ":max_offsets=" + std::to_string(limits.max_candidate_offsets);
  const auto list = enumerate_file_candidates(disc);
  out.enumeration = list.error;
  if (!list.complete()) {
    out.complete = false;
    return out;
  }
  struct Entry {
    std::size_t index{};
    std::vector<std::uint8_t> bytes, mask;
    std::uint64_t valid{};
    std::string sha256;
    bool admit{};
  };
  std::vector<Entry> entries(list.files.size());
  // The memory bound: candidate files are read in disc order until `max_total_bytes` is held; the rest are accounted `over_budget` without being read.
  std::vector<std::uint8_t> skipped(list.files.size(), 0U);
  {
    std::uint64_t held = 0;
    for (std::size_t i = 0; i < list.files.size(); ++i) {
      const auto size = list.files[i].extent.size;
      if (size > limits.max_file_bytes || size < 2U) continue;
      if (held + size > limits.max_total_bytes) skipped[i] = 1U;
      else held += size;
    }
  }
  std::atomic<std::size_t> next{0};
  const auto work = [&]() {
    for (std::size_t i = next++; i < list.files.size(); i = next++) {
      auto &e = entries[i];
      e.index = i;
      const auto &extent = list.files[i].extent;
      if (extent.size > limits.max_file_bytes || extent.size < 2U || skipped[i] != 0U) continue;
      e.bytes = read_file_candidate(disc, extent);
      if (e.bytes.size() != extent.size) {
        e.bytes.clear();
        continue;
      }
      e.sha256 = sha256_hex(e.bytes);
      if (policy != FileFirstPolicy::all) {
        auto closure = structural_closure(e.bytes);
        e.valid = closure.valid_offsets;
        e.mask = std::move(closure.valid);
      } else {
        e.valid = e.bytes.size() / 2U;
      }
    }
  };
  std::vector<std::thread> threads;
  for (unsigned t = 1; t < std::max(1U, limits.jobs); ++t) threads.emplace_back(work);
  work();
  for (auto &t : threads) t.join();
  // ---- accounting, in disc order ----
  std::set<std::string> seen;
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto &e = entries[i];
    const auto &extent = list.files[i].extent;
    ++out.files;
    out.file_bytes += extent.size;
    if (extent.size > limits.max_file_bytes) {
      ++out.oversize_files;
      out.oversize_bytes += extent.size;
      out.complete = false;
      continue;
    }
    if (extent.size < 2U) {
      ++out.empty_files;
      continue;
    }
    if (skipped[i] != 0U) {
      ++out.over_budget_files;
      out.complete = false;
      continue;
    }
    if (e.bytes.empty()) {
      ++out.unreadable_files;
      out.complete = false;
      continue;
    }
    if (!seen.insert(e.sha256).second) {
      ++out.duplicate_files;
      continue;
    }
    if (e.valid == 0U) {
      ++out.no_structure_files;
      continue;
    }
    order.push_back(i);
  }
  if (limits.max_candidate_offsets != 0U)
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      // density = valid / size, compared by cross-multiplication; ties keep disc order (stable)
      return entries[a].valid * entries[b].bytes.size() > entries[b].valid * entries[a].bytes.size();
    });
  std::uint64_t budget_used = 0;
  for (const std::size_t i : order) {
    auto &e = entries[i];
    if (limits.max_candidate_offsets != 0U && budget_used + e.valid > limits.max_candidate_offsets) {
      ++out.over_budget_files;
      out.complete = false;
      continue;
    }
    budget_used += e.valid;
    e.admit = true;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto &e = entries[i];
    if (!e.admit) continue;
    DynamicImageSpec spec;
    spec.track_offset = list.files[i].extent.block * kIso9660BlockBytes;
    spec.bytes = std::move(e.bytes);
    if (policy != FileFirstPolicy::all) spec.candidate = std::move(e.mask);
    out.candidate_offsets += e.valid;
    out.admitted_bytes += spec.bytes.size();
    out.images.push_back(std::move(spec));
  }
  return out;
}

}  // namespace segarecomp::machine::sega_cd
