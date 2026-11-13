// Declared guest-memory ranges (SEG-138-T009, ADR 0300): the per-revision authority a scheduled mod's memory services run under. Pure declaration processing, no image bytes, no clock.
// The scheduler (platforms/genesis/runtime/mod_scheduler.c) denies every access outside a declared range, so a capability alone never authorizes an offset.
#include "segarecomp/mod.hpp"

#include <algorithm>

namespace segarecomp::mod {

namespace {
constexpr std::uint64_t kGenesisWorkRamBytes = 65536;

bool has_cap(const Manifest &m, const std::string &name) {
  return std::find(m.capabilities.begin(), m.capabilities.end(), name) != m.capabilities.end() || std::find(m.capabilities_optional.begin(), m.capabilities_optional.end(), name) != m.capabilities_optional.end();
}
}  // namespace

std::optional<Rejection> check_memory(const Manifest &m) {
  if (m.memory.empty()) return std::nullopt;
  if (m.memory.size() > kMaxMemoryRanges) return Rejection{"E_MEMORY_COUNT", "a package declares at most 16 memory ranges"};
  for (std::size_t i = 0; i < m.memory.size(); ++i) {
    const MemoryRange &r = m.memory[i];
    if (m.platform != "genesis") return Rejection{"E_MEMORY_SPACE", "memory ranges are implemented for the Genesis adapter only"};
    if (r.space != "m68k.work_ram") return Rejection{"E_MEMORY_SPACE", "the memory space \"" + r.space + "\" is not accessible to a mod on this platform (only m68k.work_ram)"};
    if (std::none_of(std::begin(kMemoryAccess), std::end(kMemoryAccess), [&](std::string_view a) { return a == r.access; })) return Rejection{"E_MEMORY_ACCESS", "memory access must be read, write or read-write"};
    if (r.length == 0 || static_cast<std::uint64_t>(r.offset) + r.length > kGenesisWorkRamBytes) return Rejection{"E_MEMORY_RANGE", "a memory range must be non-empty and lie inside m68k.work_ram (64 KiB)"};
    if (!r.revision.empty() && std::none_of(m.revisions.begin(), m.revisions.end(), [&](const Revision &v) { return v.label == r.revision; })) return Rejection{"E_MEMORY_REVISION", "a memory range names the undeclared revision \"" + r.revision + "\""};
    const bool read = r.access != "write", write = r.access != "read";
    if (read && !has_cap(m, "memory.read." + r.space)) return Rejection{"E_CAPABILITY_INCONSISTENT", "a readable memory range needs the memory.read." + r.space + " capability"};
    if (write && !has_cap(m, "memory.write." + r.space)) return Rejection{"E_CAPABILITY_INCONSISTENT", "a writable memory range needs the memory.write." + r.space + " capability"};
    for (std::size_t j = 0; j < i; ++j)
      if (m.memory[j] == r) return Rejection{"E_MEMORY_DUPLICATE", "a memory range is declared twice"};
    if (i != 0 && !(std::tie(m.memory[i - 1].revision, m.memory[i - 1].space, m.memory[i - 1].offset, m.memory[i - 1].length, m.memory[i - 1].access) <
                    std::tie(r.revision, r.space, r.offset, r.length, r.access)))
      return Rejection{"E_FIELD_VALUE", "memory ranges must be in canonical order"};
  }
  return std::nullopt;
}

std::vector<MemoryRange> memory_for_revision(const Manifest &m, std::string_view revision) {
  std::vector<MemoryRange> out;
  for (const MemoryRange &r : m.memory)
    if (r.revision.empty() || r.revision == revision) out.push_back(r);
  return out;
}

}  // namespace segarecomp::mod
