#include "segarecomp/machine/sega_cd/loader_scan.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <optional>
#include <span>
#include <tuple>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/modules.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

const char *loader_refusal_name(LoaderRefusalKind kind) noexcept {
  switch (kind) {
  case LoaderRefusalKind::lba_unknown: return "lba_unknown";
  case LoaderRefusalKind::destination_unknown: return "destination_unknown";
  case LoaderRefusalKind::destination_unsupported: return "destination_unsupported";
  case LoaderRefusalKind::length_unsupported: return "length_unsupported";
  case LoaderRefusalKind::cdd_command_invalid: return "cdd_command_invalid";
  case LoaderRefusalKind::cdc_state_unknown: return "cdc_state_unknown";
  case LoaderRefusalKind::loaded_not_executed: return "loaded_not_executed";
  case LoaderRefusalKind::entry_unknown: return "entry_unknown";
  case LoaderRefusalKind::entry_not_code: return "entry_not_code";
  case LoaderRefusalKind::source_outside_track: return "source_outside_track";
  case LoaderRefusalKind::too_many_operations: return "too_many_operations";
  case LoaderRefusalKind::too_many_modules: return "too_many_modules";
  case LoaderRefusalKind::scan_limit_exceeded: return "scan_limit_exceeded";
  case LoaderRefusalKind::computed_control_unresolved: return "computed_control_unresolved";
  case LoaderRefusalKind::lookup_key_unknown: return "lookup_key_unknown";
  case LoaderRefusalKind::loaded_bytes_unknown: return "loaded_bytes_unknown";
  }
  return "unknown";
}

namespace {

constexpr std::uint32_t kCdBiosVector = 0x5F22U;   // _CDBIOS: JSR with the function number in D0.W (public BIOS ABI, ADR 0215)
constexpr std::uint32_t kGateArraySub = 0x00FF8000U;
constexpr std::uint32_t kGateArraySubEnd = 0x00FF8200U;
constexpr std::uint32_t kWordRamSub = 0x00080000U;       // 2M mode view of the sub CPU
constexpr std::uint32_t kWordRamSubEnd = 0x000C0000U;
constexpr std::uint32_t kDmaAddressMask = 0x3FFFEU;      // runtime: ((dma_address << 3) & mask)

using Width = M68kMemoryAccessWidth;

// A 32-bit value with a mask of the bits the walk knows (a MOVE.W into a register defines only its low half).
struct Val {
  std::uint32_t v{};
  std::uint32_t mask{};
  bool disc{};   // the known bits derive from immutable disc content a documented service delivered (SEG-099: a comparison of such data with an unknown key is an unresolved lookup)
  [[nodiscard]] bool known() const { return mask == 0xFFFFFFFFU; }
};
Val K(std::uint32_t v) { return Val{v, 0xFFFFFFFFU}; }

std::uint32_t width_mask(Width w) { return w == Width::byte ? 0xFFU : w == Width::word ? 0xFFFFU : 0xFFFFFFFFU; }
std::uint32_t mask_of(Width w, std::uint32_t v) { return v & width_mask(w); }
// The value of `x` as a `w`-wide quantity, when every bit of that width is known (the result is then a fully known Val).
Val exact(const Val &x, Width w) {
  if ((x.mask & width_mask(w)) != width_mask(w)) return Val{};
  Val r = K(x.v & width_mask(w));
  r.disc = x.disc;
  return r;
}

// The machine's CD interface as the walk sees it (contract section 7). Unknown until a constant write defines it.
struct Hw {
  std::array<Val, 10> tx{};     // TX0..TX9 as written
  Val reg_ptr{K(0)};            // CDC register address ($FF8005)
  Val dest{};                   // CDC destination ($FF8004 & 7)
  Val dma_address{};            // $FF800A
  Val ifctrl{};                 // CDC register 1
  Val dbcl{}, dbch{};
  Val lba{};                    // the drive position after the latest play command
  bool play_invalid{};          // the latest play command could not be decoded
};

// SEG-099-T002: condition codes the walk knows (bit set in `mask` = known; bits N=8, Z=4, V=2, C=1 as in the status register).
struct Flags {
  std::uint8_t mask{};
  std::uint8_t val{};
  bool operator==(const Flags &) const = default;
};
Flags known_flags(bool n, bool z, bool v, bool c) {
  return Flags{0x0FU, static_cast<std::uint8_t>((n ? 8U : 0U) | (z ? 4U : 0U) | (v ? 2U : 0U) | (c ? 1U : 0U))};
}

constexpr std::uint32_t kMemTrackedEnd = 0x000C0000U;   // sub PRG RAM and the 2M Word RAM view

struct Page {
  std::array<std::uint8_t, 256> v{};
  std::array<std::uint64_t, 4> known{};
  std::array<std::uint64_t, 4> disc{};   // bytes whose value is immutable disc content
};

// Bounded abstract RAM: only bytes with an independently justified value are present. Copy-on-write pages keep a forked state cheap.
class Mem {
public:
  [[nodiscard]] std::optional<std::uint8_t> get(std::uint32_t a) const {
    const auto it = pages_.find(a >> 8);
    if (it == pages_.end()) return std::nullopt;
    const unsigned o = a & 0xFFU;
    if (((it->second->known[o >> 6U] >> (o & 63U)) & 1U) == 0U) return std::nullopt;
    return it->second->v[o];
  }
  [[nodiscard]] bool get_disc(std::uint32_t a) const {
    const auto it = pages_.find(a >> 8);
    if (it == pages_.end()) return false;
    const unsigned o = a & 0xFFU;
    return ((it->second->disc[o >> 6U] >> (o & 63U)) & 1U) != 0U;
  }
  void put(std::uint32_t a, std::optional<std::uint8_t> b, bool disc = false) {
    auto it = pages_.find(a >> 8);
    if (it == pages_.end()) {
      if (!b || pages_.size() >= kLoaderScanMaxTrackedPages) return;   // over the page bound: forget (never invent)
      it = pages_.emplace(a >> 8, std::make_shared<const Page>()).first;
    }
    auto page = std::make_shared<Page>(*it->second);
    const unsigned o = a & 0xFFU;
    if (b) {
      page->v[o] = *b;
      page->known[o >> 6U] |= std::uint64_t{1} << (o & 63U);
      if (disc) page->disc[o >> 6U] |= std::uint64_t{1} << (o & 63U);
      else page->disc[o >> 6U] &= ~(std::uint64_t{1} << (o & 63U));
    } else {
      page->v[o] = 0;
      page->known[o >> 6U] &= ~(std::uint64_t{1} << (o & 63U));
      page->disc[o >> 6U] &= ~(std::uint64_t{1} << (o & 63U));
    }
    if ((page->known[0] | page->known[1] | page->known[2] | page->known[3]) != 0U) it->second = std::move(page);
    else pages_.erase(it);
  }
  void put_bytes(std::uint32_t a, std::span<const std::uint8_t> data, bool disc = false) {
    for (std::size_t i = 0; i < data.size(); ++i) put(a + static_cast<std::uint32_t>(i), data[i], disc);
  }
  void clear_range(std::uint32_t lo, std::uint32_t hi) {   // [lo, hi)
    if (hi <= lo) return;
    for (auto it = pages_.lower_bound(lo >> 8); it != pages_.end() && (it->first << 8) < hi;) {
      const std::uint32_t pb = it->first << 8;
      if (pb >= lo && pb + 256U <= hi) { it = pages_.erase(it); continue; }
      auto page = std::make_shared<Page>(*it->second);
      for (unsigned o = 0; o < 256U; ++o)
        if (pb + o >= lo && pb + o < hi) { page->known[o >> 6U] &= ~(std::uint64_t{1} << (o & 63U)); page->disc[o >> 6U] &= ~(std::uint64_t{1} << (o & 63U)); }
      if ((page->known[0] | page->known[1] | page->known[2] | page->known[3]) != 0U) { it->second = std::move(page); ++it; }
      else it = pages_.erase(it);
    }
  }
  void clear() { pages_.clear(); }
  [[nodiscard]] bool empty() const { return pages_.empty(); }
  [[nodiscard]] std::size_t pages() const { return pages_.size(); }
  void serialize(std::string &out) const {
    const auto put32 = [&](std::uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
    put32(static_cast<std::uint32_t>(pages_.size()));
    for (const auto &[index, page] : pages_) {
      put32(index);
      for (const auto k : page->known) out.append(reinterpret_cast<const char *>(&k), 8);
      for (const auto k : page->disc) out.append(reinterpret_cast<const char *>(&k), 8);
      for (unsigned o = 0; o < 256U; ++o)
        if (((page->known[o >> 6U] >> (o & 63U)) & 1U) != 0U) out.push_back(static_cast<char>(page->v[o]));
    }
  }
  // The page-wise meet of two memories: a byte survives only when both know the same value. Returns true when `*this` lost knowledge.
  bool join(const Mem &other) {
    bool lost = false;
    for (auto it = pages_.begin(); it != pages_.end();) {
      const auto o = other.pages_.find(it->first);
      if (o == other.pages_.end()) { it = pages_.erase(it); lost = true; continue; }
      auto page = std::make_shared<Page>(*it->second);
      bool changed = false;
      for (unsigned b = 0; b < 256U; ++b) {
        const bool mine = ((page->known[b >> 6U] >> (b & 63U)) & 1U) != 0U;
        if (!mine) continue;
        const bool theirs = ((o->second->known[b >> 6U] >> (b & 63U)) & 1U) != 0U;
        if (!theirs || o->second->v[b] != page->v[b]) { page->known[b >> 6U] &= ~(std::uint64_t{1} << (b & 63U)); page->disc[b >> 6U] &= ~(std::uint64_t{1} << (b & 63U)); page->v[b] = 0; changed = true; }
        else if (((page->disc[b >> 6U] >> (b & 63U)) & 1U) != 0U && ((o->second->disc[b >> 6U] >> (b & 63U)) & 1U) == 0U) { page->disc[b >> 6U] &= ~(std::uint64_t{1} << (b & 63U)); changed = true; }
      }
      if (changed) {
        lost = true;
        if ((page->known[0] | page->known[1] | page->known[2] | page->known[3]) != 0U) it->second = std::move(page);
        else { it = pages_.erase(it); continue; }
      }
      ++it;
    }
    return lost;
  }
private:
  std::map<std::uint32_t, std::shared_ptr<const Page>> pages_;
};

// Memory an asynchronous entry (the SP's interrupt and user entries) may write while the init/main entries run: reads of it are unknown (ADR 0216).
struct Volatile {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;   // [lo, hi)
  bool all{};
  void add(std::uint32_t lo, std::uint32_t hi) {
    if (hi <= lo) return;
    ranges.emplace_back(lo, hi);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
    for (const auto &r : ranges) {
      if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
      else merged.push_back(r);
    }
    ranges = std::move(merged);
  }
  [[nodiscard]] bool overlaps(std::uint32_t lo, std::uint32_t hi) const {
    if (all) return true;
    return std::any_of(ranges.begin(), ranges.end(), [&](const auto &r) { return lo < r.second && r.first < hi; });
  }
  [[nodiscard]] bool covers(const Volatile &o) const {   // *this is at least as volatile as `o`
    if (all) return true;
    if (o.all) return false;
    return std::all_of(o.ranges.begin(), o.ranges.end(), [&](const auto &r) {
      return std::any_of(ranges.begin(), ranges.end(), [&](const auto &m) { return m.first <= r.first && r.second <= m.second; });
    });
  }
  void merge(const Volatile &o) {
    all = all || o.all;
    for (const auto &r : o.ranges) add(r.first, r.second);
  }
};

// The documented BIOS CDC sector-read state (ADR 0215): the next logical sector the CDC delivers and the sectors left of the latest ROMREADN.
struct Cd {
  Val pos{};
  Val left{};
  bool header_read{};   // CDCREAD has been called since the last CDCACK
};

struct Loaded {
  std::uint32_t op{};
  std::uint32_t base{};
  std::uint32_t bytes{};
  bool operator==(const Loaded &) const = default;
};

struct State {
  std::uint32_t pc{};
  std::array<Val, 8> d{};
  std::array<Val, 8> a{};
  Hw hw;
  std::vector<std::vector<std::uint32_t>> rets{std::vector<std::uint32_t>{}};   // the possible return stacks: exactly one until a join merges paths that came from different callers
  std::vector<Loaded> loaded;
  Flags fl;
  Mem mem;
  Cd cd;
  bool image_dirty{};     // the boot image's bytes may no longer be what the disc delivered (an unknown or forgotten write): non-PC-relative image reads are unknown
  std::uint8_t root{};    // the module-table slot this path started in (0 init, 1 main, 2 int2, 3 user)
  bool top_dirty{true};   // the top of `ret` may no longer be the word at the stack pointer (a push/pop the walk does not track happened)
};

std::string serialize(const State &s) {
  std::string out;
  const auto put = [&](std::uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
  const auto putv = [&](const Val &v) { put(v.mask); put(v.v & v.mask); out.push_back(v.disc ? 1 : 0); };
  put(s.pc);
  for (const auto &v : s.d) putv(v);
  for (const auto &v : s.a) putv(v);
  for (const auto &v : s.hw.tx) putv(v);
  putv(s.hw.reg_ptr); putv(s.hw.dest); putv(s.hw.dma_address); putv(s.hw.ifctrl); putv(s.hw.dbcl); putv(s.hw.dbch); putv(s.hw.lba);
  out.push_back(s.hw.play_invalid ? 1 : 0);
  put(static_cast<std::uint32_t>(s.rets.size()));
  for (const auto &stack : s.rets) { put(static_cast<std::uint32_t>(stack.size())); for (const auto r : stack) put(r); }
  put(static_cast<std::uint32_t>(s.loaded.size()));
  for (const auto &l : s.loaded) { put(l.op); put(l.base); put(l.bytes); }
  out.push_back(static_cast<char>(s.fl.mask));
  out.push_back(static_cast<char>(s.fl.val & s.fl.mask));
  putv(s.cd.pos); putv(s.cd.left);
  out.push_back(s.cd.header_read ? 1 : 0);
  out.push_back(s.top_dirty ? 1 : 0);
  out.push_back(s.image_dirty ? 1 : 0);
  out.push_back(static_cast<char>(s.root));
  s.mem.serialize(out);
  return out;
}

// ---- state convergence (SEG-099-T003, ADR 0217) --------------------------------------------------------------------------------------
// The join of two abstract values: identical knowledge is kept, any difference becomes unknown (all-or-nothing, so a location changes at most once). A join never chooses a constant.
Val join_val(const Val &a, const Val &b) {
  if (a.mask == b.mask && ((a.v ^ b.v) & a.mask) == 0U) { Val r = a; r.disc = a.disc && b.disc; return r; }   // identical knowledge is kept; any difference widens the whole value to unknown (a bounded number of changes per location)
  return Val{};
}

// Joins `b` into `a` (same pc, root and return stack). May-facts (loads seen, header read, dirtiness) are united, must-facts (values) are met.
State join_states(const State &a, const State &b) {
  State r = a;
  for (std::size_t i = 0; i < r.d.size(); ++i) r.d[i] = join_val(a.d[i], b.d[i]);
  for (std::size_t i = 0; i < r.a.size(); ++i) r.a[i] = join_val(a.a[i], b.a[i]);
  for (std::size_t i = 0; i < r.hw.tx.size(); ++i) r.hw.tx[i] = join_val(a.hw.tx[i], b.hw.tx[i]);
  r.hw.reg_ptr = join_val(a.hw.reg_ptr, b.hw.reg_ptr);
  r.hw.dest = join_val(a.hw.dest, b.hw.dest);
  r.hw.dma_address = join_val(a.hw.dma_address, b.hw.dma_address);
  r.hw.ifctrl = join_val(a.hw.ifctrl, b.hw.ifctrl);
  r.hw.dbcl = join_val(a.hw.dbcl, b.hw.dbcl);
  r.hw.dbch = join_val(a.hw.dbch, b.hw.dbch);
  r.hw.lba = join_val(a.hw.lba, b.hw.lba);
  r.hw.play_invalid = a.hw.play_invalid || b.hw.play_invalid;
  r.fl.mask = static_cast<std::uint8_t>(a.fl.mask & b.fl.mask & ~(a.fl.val ^ b.fl.val));
  r.fl.val = static_cast<std::uint8_t>(a.fl.val & r.fl.mask);
  r.rets.insert(r.rets.end(), b.rets.begin(), b.rets.end());
  std::sort(r.rets.begin(), r.rets.end());
  r.rets.erase(std::unique(r.rets.begin(), r.rets.end()), r.rets.end());
  for (const auto &l : b.loaded)
    if (std::none_of(r.loaded.begin(), r.loaded.end(), [&](const Loaded &x) { return x == l; })) r.loaded.push_back(l);
  std::sort(r.loaded.begin(), r.loaded.end(), [](const Loaded &x, const Loaded &y) { return std::tie(x.op, x.base, x.bytes) < std::tie(y.op, y.base, y.bytes); });
  r.mem.join(b.mem);
  r.cd.pos = join_val(a.cd.pos, b.cd.pos);
  r.cd.left = join_val(a.cd.left, b.cd.left);
  r.cd.header_read = a.cd.header_read || b.cd.header_read;
  r.top_dirty = a.top_dirty || b.top_dirty;
  r.image_dirty = a.image_dirty || b.image_dirty;
  return r;
}

struct Head {
  std::uint32_t count{};
  bool have{};
  State joined;
};

std::string head_key(const State &s) {
  std::string out;
  const auto put = [&](std::uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
  put(s.pc);
  put(s.root);
  // States are only joined with states of the same knowledge profile, so a path that has lost its work-area knowledge (an unknown alias, an unmodelled service) cannot degrade a path that has not.
  out.push_back(static_cast<char>((s.image_dirty ? 1 : 0) | (s.mem.empty() ? 2 : 0)));
  return out;
}

class Walker {
public:
  Walker(const DiscImage &disc, const ProcessorImages &sub, const LoaderScanOptions &options, const Volatile &assumed) : disc_(disc), sub_(sub), options_(options), assumed_(assumed) {
    const auto &image = sub.images.images.front();
    const auto &mapping = image.mappings.front();
    bytes_ = executable_image_bytes(sub.images, image);
    base_ = mapping.execution_base + mapping.first_offset;
    end_ = base_ + mapping.length;
    first_offset_ = mapping.first_offset;
    ws_ = options.work_state;
  }

  LoaderScan run() {
    // Registers other than the stack pointer are unknown at entry (the BIOS hands over nothing the walk may rely on).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> roots;   // (module-table slot, address)
    if (options_.module_table_roots) roots = module_table_roots();
    if (roots.empty()) roots.emplace_back(1U, sub_.entry_pc);
    scan_.survey.roots = static_cast<std::uint32_t>(roots.size());
    std::vector<State> stack;
    if (options_.order >= 2U) std::reverse(roots.begin(), roots.end());   // test-only variant: roots in the opposite order
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
      State start;
      start.pc = it->second;
      start.root = static_cast<std::uint8_t>(it->first);
      if (!options_.module_table_roots) { stack.push_back(std::move(start)); continue; }   // the legacy single root: states and pruning exactly as in M1
      if (seen_.insert(serialize(start)).second) { ++scan_.states; stack.push_back(std::move(start)); }
    }
    while (!stack.empty() && scan_.complete) {
      // Exploration order is deterministic (depth first). The test-only variants exist to check that the result does not depend on it.
      const bool fifo = options_.order == 1U || options_.order == 3U;
      State s = fifo ? std::move(stack.front()) : std::move(stack.back());
      if (fifo) stack.erase(stack.begin()); else stack.pop_back();
      walk(std::move(s), stack);
    }
    auto &sv = scan_.survey;
    sv.cdd_commands = static_cast<std::uint32_t>(cdd_sites_.size());
    sv.cdc_transfer_triggers = static_cast<std::uint32_t>(trigger_sites_.size());
    for (const auto &[site, fn] : bios_sites_) { (void)site; ++sv.bios_service_calls; if (fn == 0xFFFFFFFFU) ++sv.bios_service_unknown_function; }
    sv.foreign_calls_other = static_cast<std::uint32_t>(foreign_other_.size());
    sv.foreign_calls_unknown = static_cast<std::uint32_t>(foreign_unknown_.size());
    sv.copy_loop_candidates = static_cast<std::uint32_t>(copy_sites_.size());
    sv.counted_loops = static_cast<std::uint32_t>(loop_sites_.size());
    sv.dispatch_resolved = static_cast<std::uint32_t>(dispatch_sites_.size());
    sv.table_reads = static_cast<std::uint32_t>(table_read_sites_.size());
    sv.memory_carried_reads = static_cast<std::uint32_t>(memory_read_sites_.size());
    sv.bios_modelled_calls = static_cast<std::uint32_t>(modelled_sites_.size());
    sv.bios_unmodelled_calls = static_cast<std::uint32_t>(unmodelled_sites_.size());
    sv.sector_transfers = static_cast<std::uint32_t>(transfers_resolved_.size());
    sv.sector_transfers_unresolved = static_cast<std::uint32_t>(transfers_unresolved_.size());
    sv.memory_args_resolved = static_cast<std::uint32_t>(args_resolved_.size());
    sv.memory_args_unresolved = static_cast<std::uint32_t>(args_unresolved_.size());
    sv.branches_decided = static_cast<std::uint32_t>(decided_sites_.size());
    sv.module_entries_walked = static_cast<std::uint32_t>(entered_.size());
    sv.joins = joins_;
    sv.widenings = widenings_;
    sv.async_volatile_ranges = static_cast<std::uint32_t>(assumed_.ranges.size());
    sv.async_volatile_all = assumed_.all;
    return std::move(scan_);
  }

  [[nodiscard]] const Volatile &async_writes() const { return async_writes_; }

private:
  const DiscImage &disc_;
  const ProcessorImages &sub_;
  const LoaderScanOptions &options_;
  const Volatile &assumed_;      // what the asynchronous entries were found to write (a previous pass): unknown to the synchronous entries
  Volatile async_writes_;        // what the asynchronous entries write in THIS pass
  std::set<std::pair<std::string, std::uint32_t>> ends_;
  void end_at(const char *kind, std::uint32_t pc) { if (ends_.insert({kind, pc}).second) ++scan_.survey.path_ends[kind]; }
  std::set<std::uint32_t> table_read_sites_, dispatch_sites_, memory_read_sites_;
  std::set<std::uint32_t> cdd_sites_, trigger_sites_, copy_sites_, loop_sites_, foreign_other_, foreign_unknown_;
  std::set<std::pair<std::uint32_t, std::uint32_t>> bios_sites_;
  std::set<std::pair<std::uint32_t, std::uint32_t>> modelled_sites_;
  std::set<std::uint32_t> unmodelled_sites_, args_resolved_, args_unresolved_, transfers_resolved_, transfers_unresolved_, decided_sites_;
  std::uint32_t mem_resolved_reads_{};
  std::uint32_t cur_pc_{};            // the address of the instruction being executed
  std::set<std::uint32_t> entered_;   // distinct proven entries whose code was walked (recursive_modules)
  bool lookup_unknown_{};
  std::unordered_map<std::string, Head> heads_;
  std::uint32_t joins_{}, widenings_{};
  bool ws_{};
  std::span<const std::uint8_t> bytes_;
  std::uint32_t base_{}, end_{}, first_offset_{};
  LoaderScan scan_;
  std::unordered_set<std::string> seen_;
  std::set<std::pair<LoaderRefusalKind, std::uint32_t>> refused_;

  // The SP module jump table (the BIOS enters an SP module through it): header word $18 is the table offset from the module start; the table
  // is four word offsets (init, main, int2, user) from the table start, zero meaning none. Only offsets that land inside the image count.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> module_table_roots() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> roots;
    const std::uint32_t size = end_ - base_;
    const auto be32 = [&](std::uint32_t o) { return (std::uint32_t{bytes_[first_offset_ + o]} << 24) | (std::uint32_t{bytes_[first_offset_ + o + 1U]} << 16) | (std::uint32_t{bytes_[first_offset_ + o + 2U]} << 8) | bytes_[first_offset_ + o + 3U]; };
    const auto be16 = [&](std::uint32_t o) { return (std::uint32_t{bytes_[first_offset_ + o]} << 8) | bytes_[first_offset_ + o + 1U]; };
    if (size < 0x20U || bytes_.size() < first_offset_ + size) return roots;
    if (std::string_view(reinterpret_cast<const char *>(bytes_.data()) + first_offset_, 4U) != "MAIN") return roots;   // the module header's name field
    const std::uint32_t table = be32(0x18U);
    if ((table & 1U) != 0U || table < 0x20U || table + 8U > size) return roots;
    scan_.survey.module_table_found = true;
    for (std::uint32_t i = 0; i < 4U; ++i) {
      const std::uint32_t w = be16(table + 2U * i);
      const std::uint32_t at = table + w;
      if (w == 0U || (at & 1U) != 0U || at >= size) continue;
      if (std::none_of(roots.begin(), roots.end(), [&](const auto &r) { return r.second == base_ + at; })) roots.emplace_back(i, base_ + at);
    }
    return roots;
  }

  void refuse(LoaderRefusalKind kind, std::uint32_t pc) {
    if (refused_.insert({kind, pc}).second) scan_.refusals.push_back({kind, pc});
  }
  void exhausted(std::uint32_t pc) {
    refuse(LoaderRefusalKind::scan_limit_exceeded, pc);
    scan_.complete = false;
  }

  // ---- values ------------------------------------------------------------------------------------------------------------
  // A word/long at `address` inside the boot image (a constant when the table is not rewritten before the read; see LoaderScanOptions::constant_dispatch).
  Val image_read(std::uint32_t address, Width w) {
    const std::uint32_t n = w == Width::byte ? 1U : w == Width::word ? 2U : 4U;
    if (w == Width::byte || (address & 1U) != 0U || address < base_ || static_cast<std::uint64_t>(address) + n > end_) return {};
    const std::uint32_t off = first_offset_ + (address - base_);
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) v = (v << 8) | bytes_[off + i];
    table_read_sites_.insert(address);
    return K(v);
  }

  // The effective address of a (d8,PC|An,Xn) control/data operand when its base and index are known.
  std::optional<std::uint32_t> indexed_address(const State &s, const M68kEffectiveAddress &ea) const {
    Val base;
    if (ea.mode == M68kEaMode::pc_index8) base = K(ea.pc_base_address);
    else if (ea.mode == M68kEaMode::address_index8) base = s.a[ea.reg & 7U];
    else return std::nullopt;
    const Val index = ea.index_is_address ? s.a[ea.index_reg & 7U] : s.d[ea.index_reg & 7U];
    if (!base.known()) return std::nullopt;
    std::int64_t idx = 0;
    if (ea.index_is_long) {
      if (!index.known()) return std::nullopt;
      idx = static_cast<std::int32_t>(index.v);
    } else {
      const Val low = exact(index, Width::word);
      if (!low.known()) return std::nullopt;
      idx = static_cast<std::int16_t>(low.v);
    }
    return static_cast<std::uint32_t>((static_cast<std::int64_t>(base.v) + ea.displacement + idx) & 0x00FFFFFF);
  }

  Val read_ea(const State &s, const M68kEffectiveAddress &ea, Width w) {
    if (options_.constant_dispatch) {
      if (ea.mode == M68kEaMode::pc_index8 || ea.mode == M68kEaMode::address_index8) {
        const auto address = ea.mode == M68kEaMode::pc_index8 ? indexed_address(s, ea) : std::nullopt;   // only the immutable image is read
        return address ? image_read(*address, w) : Val{};
      }
      if (ea.mode == M68kEaMode::pc_disp16) return image_read(ea.absolute_address & 0x00FFFFFFU, w);
    }
    switch (ea.mode) {
    case M68kEaMode::immediate: return K(mask_of(w, ea.immediate_value));
    case M68kEaMode::data_register: return exact(s.d[ea.reg & 7U], w);
    case M68kEaMode::address_register: return exact(s.a[ea.reg & 7U], w);
    default: return {};
    }
  }

  static Val merge(const Val &old, const Val &val, Width w) {
    if (w == Width::long_word) return val;
    const std::uint32_t wm = width_mask(w);
    Val out;
    out.mask = (old.mask & ~wm) | (val.mask & wm);
    out.v = ((old.v & ~wm) | (val.v & wm)) & out.mask;
    out.disc = old.disc || val.disc;
    return out;
  }

  void taint_hw(State &s) { s.hw = Hw{}; }

  // A write to memory whose location the walk may not know could be a Gate Array register: the shadow is dropped (A7 is the stack,
  // which no boot profile places in the register block).
  void write_memory_ea(State &s, const M68kEffectiveAddress &ea, Width w, const Val &val) {
    switch (ea.mode) {
    case M68kEaMode::data_register: s.d[ea.reg & 7U] = merge(s.d[ea.reg & 7U], val, w); return;
    case M68kEaMode::absolute_word:
    case M68kEaMode::absolute_long: {
      const std::uint32_t address = ea.absolute_address & 0x00FFFFFFU;
      if (address >= kGateArraySub && address < kGateArraySubEnd) hw_write(s, address - kGateArraySub, w, val);
      return;
    }
    case M68kEaMode::address_indirect:
    case M68kEaMode::address_postinc:
    case M68kEaMode::address_predec:
    case M68kEaMode::address_disp16:
    case M68kEaMode::address_index8:
      if ((ea.reg & 7U) != 7U) taint_hw(s);
      return;
    default: return;
    }
  }

  // ---- the CD interface ------------------------------------------------------------------------------------------------
  void cdd_command(State &s, std::uint32_t pc) {
    auto &h = s.hw;
    const auto digit = [&](std::size_t i, std::uint32_t &out) {
      if (!h.tx[i].known() || h.tx[i].v > 9U) return false;
      out = h.tx[i].v;
      return true;
    };
    if (!h.tx[0].known()) {  // an unknown command may be a play: the position is lost
      h.lba = {};
      h.play_invalid = false;
      return;
    }
    if ((h.tx[0].v & 0xFU) != 3U) {  // status (0) leaves the position; stop, seek, pause, TOC... move or end it: the walk no longer knows it
      if ((h.tx[0].v & 0xFU) != 0U) h.lba = {};
      return;
    }
    std::uint32_t t[8];
    bool ok = true;
    for (std::size_t i = 2; i < 8; ++i) ok = ok && digit(i, t[i]);
    if (!ok) {
      h.lba = {};
      h.play_invalid = true;
      return;
    }
    // The checksum over TX0..TX8 (the drive's own rule); an invalid checksum is refused, never repaired.
    std::uint32_t sum = 0;
    bool known_all = true;
    for (std::size_t i = 0; i < 9; ++i) {
      if (!h.tx[i].known()) { known_all = false; break; }
      sum += h.tx[i].v & 0xFU;
    }
    if (!known_all || !h.tx[9].known() || ((~sum) & 0xFU) != (h.tx[9].v & 0xFU)) {
      h.lba = {};
      h.play_invalid = true;
      refuse(LoaderRefusalKind::cdd_command_invalid, pc);
      return;
    }
    const std::int64_t msf = ((static_cast<std::int64_t>(t[2]) * 10 + t[3]) * 60 + (t[4] * 10 + t[5])) * 75 + (t[6] * 10 + t[7]);
    const std::int64_t lba = msf - 150;
    if (lba < 0) {
      h.lba = {};
      h.play_invalid = true;
      refuse(LoaderRefusalKind::cdd_command_invalid, pc);
      return;
    }
    h.lba = K(static_cast<std::uint32_t>(lba));
    h.play_invalid = false;
  }

  void cdc_register_write(State &s, std::uint32_t value, std::uint32_t pc) {
    auto &h = s.hw;
    if (!h.reg_ptr.known()) {  // every register may have been written: nothing the walk knows about the CDC survives
      h.ifctrl = h.dbcl = h.dbch = {};
      return;
    }
    const std::uint32_t reg = h.reg_ptr.v & 0xFU;
    switch (reg) {
    case 0x01: h.ifctrl = K(value & 0xFFU); break;
    case 0x02: h.dbcl = K(value & 0xFFU); break;
    case 0x03: h.dbch = K(value & 0x0FU); break;
    case 0x06: transfer(s, pc); break;
    case 0x0F: h.ifctrl = h.dbcl = h.dbch = {}; h.reg_ptr = K(0); return;  // reset: the register address is 0, no increment
    default: break;
    }
    if (h.reg_ptr.v != 0U) h.reg_ptr = K((h.reg_ptr.v + 1U) & 0xFU);
  }

  // A hardware transfer the walk cannot resolve may still write memory (a DMA to an unknown or unsupported destination): in the work-variable model nothing tracked survives it.
  void unresolved_hw_transfer(State &s) { if (ws_) forget_all(s); }

  void transfer(State &s, std::uint32_t pc) {
    trigger_sites_.insert(pc);
    const auto &h = s.hw;
    // DOUTEN known clear: DTRG starts nothing. Unknown: the load is proposed anyway (a proposal can only fail closed at run time).
    if (h.ifctrl.known() && (h.ifctrl.v & 0x02U) == 0U) return;
    if (!h.lba.known()) {
      refuse(LoaderRefusalKind::lba_unknown, pc); unresolved_hw_transfer(s); return; }
    if (!h.dest.known() || !h.dma_address.known()) { refuse(LoaderRefusalKind::destination_unknown, pc); unresolved_hw_transfer(s); return; }
    if (h.dest.v != 7U) { refuse(LoaderRefusalKind::destination_unsupported, pc); unresolved_hw_transfer(s); return; }
    if (!h.dbcl.known() || !h.dbch.known()) { refuse(LoaderRefusalKind::length_unsupported, pc); unresolved_hw_transfer(s); return; }
    const std::uint32_t bytes = (((h.dbch.v & 0xFU) << 8) | h.dbcl.v) + 1U;
    if (bytes != kLoaderScanSectorBytes) { refuse(LoaderRefusalKind::length_unsupported, pc); unresolved_hw_transfer(s); return; }
    const std::uint32_t destination = kWordRamSub + ((h.dma_address.v << 3) & kDmaAddressMask);
    if (destination + bytes > kWordRamSubEnd) { refuse(LoaderRefusalKind::destination_unsupported, pc); unresolved_hw_transfer(s); return; }
    std::uint32_t op = 0;
    for (; op < scan_.operations.size(); ++op) {
      const auto &o = scan_.operations[op];
      if (o.trigger_pc == pc && o.lba == h.lba.v && o.destination == destination) break;
    }
    if (op == scan_.operations.size()) {
      if (scan_.operations.size() >= kLoaderScanMaxOperations) {
        refuse(LoaderRefusalKind::too_many_operations, pc);
        scan_.complete = false;
        return;
      }
      scan_.operations.push_back(LoadOperation{pc, h.lba.v, destination, bytes, {}});
    }
    auto &loaded = s.loaded;
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(), [&](const Loaded &l) { return l.base < destination + bytes && destination < l.base + l.bytes; }),
                 loaded.end());  // a newer load replaces what it overlaps
    loaded.push_back({op, destination, bytes});
    if (ws_) deposit_sector(s, h.lba.v, destination, bytes);
    s.hw.lba = {};  // the drive advances every 75 Hz tick: a further transfer needs a further play command
  }

  void hw_write(State &s, std::uint32_t off, Width w, const Val &val) {
    if (w == Width::long_word) {  // the runtime splits a long access into two word accesses the walk does not model: nothing known about the interface survives
      taint_hw(s);
      return;
    }
    auto &h = s.hw;
    const std::uint32_t pc = s.pc;
    if (off == 0x04U || off == 0x05U) {
      if (w == Width::word) {
        h.reg_ptr = val.known() ? K(val.v & 0xFU) : Val{};
        h.dest = val.known() ? K((val.v >> 8) & 7U) : Val{};
        h.dma_address = K(0);
      } else if (off == 0x04U) {
        h.dest = val.known() ? K(val.v & 7U) : Val{};
        h.dma_address = K(0);
      } else {
        h.reg_ptr = val.known() ? K(val.v & 0xFU) : Val{};
      }
    } else if ((off == 0x07U && w == Width::byte) || (off == 0x06U && w == Width::word)) {
      if (!val.known()) {  // an unknown data value: only the register it targets is lost
        const Val keep = h.reg_ptr;
        if (keep.known()) {
          switch (keep.v & 0xFU) {
          case 0x01: h.ifctrl = {}; break;
          case 0x02: h.dbcl = {}; break;
          case 0x03: h.dbch = {}; break;
          case 0x06: transfer(s, pc); break;
          case 0x0F: h.ifctrl = h.dbcl = h.dbch = {}; h.reg_ptr = K(0); return;
          default: break;
          }
          if (keep.v != 0U) h.reg_ptr = K((keep.v + 1U) & 0xFU);
        } else {
          h.ifctrl = h.dbcl = h.dbch = {};
        }
        return;
      }
      cdc_register_write(s, val.v, pc);
    } else if (off == 0x0AU || off == 0x0BU) {
      if (!val.known()) h.dma_address = {};
      else h.dma_address = K(w == Width::word ? (val.v & 0xFFFFU) : (((val.v & 0xFFU) << 8) | (val.v & 0xFFU)));
    } else if (off >= 0x42U && off < 0x4CU) {
      const std::size_t index = off - 0x42U;
      if (w == Width::word) {
        if (off == 0x4AU) {  // the word written to TX8/TX9 starts the command; the drive discards the pair after it checked the sum
          h.tx[8] = val.known() ? K((val.v >> 8) & 0xFFU) : Val{};
          h.tx[9] = val.known() ? K(val.v & 0xFFU) : Val{};
          cdd_sites_.insert(pc);
          cdd_command(s, pc);
          return;
        }
        h.tx[index] = val.known() ? K((val.v >> 8) & 0xFFU) : Val{};
        h.tx[index + 1U] = val.known() ? K(val.v & 0xFFU) : Val{};
      } else {
        h.tx[index] = val.known() ? K(val.v & 0xFFU) : Val{};
      }
    }
  }

  // ---- work-variable model (SEG-099-T002, ADR 0216): abstract memory, condition codes, documented BIOS CD services ---------------
  static std::uint32_t nbytes(Width w) { return w == Width::byte ? 1U : w == Width::word ? 2U : 4U; }

  static Val mem_read(const Mem &m, std::uint32_t a, Width w) {
    std::uint32_t v = 0;
    bool disc = true;
    for (std::uint32_t i = 0; i < nbytes(w); ++i) {
      const auto b = m.get(a + i);
      if (!b) return {};
      v = (v << 8) | *b;
      disc = disc && m.get_disc(a + i);
    }
    Val r = K(v);
    r.disc = disc;
    return r;
  }
  static void mem_write(Mem &m, std::uint32_t a, Width w, const Val &val) {
    const std::uint32_t n = nbytes(w);
    for (std::uint32_t i = 0; i < n; ++i) {
      const unsigned shift = 8U * (n - 1U - i);
      if (((val.mask >> shift) & 0xFFU) == 0xFFU) m.put(a + i, static_cast<std::uint8_t>((val.v >> shift) & 0xFFU), val.disc);
      else m.put(a + i, std::nullopt);
    }
  }

  struct Place {
    enum class Kind : std::uint8_t { none, dreg, areg, imm, mem, unknown_mem, stack } kind{Kind::none};
    unsigned reg{};
    std::uint32_t addr{};
    Val imm{};
    bool pc_rel{};
  };

  // Resolves one operand once (auto-increment / -decrement are applied here). A stack-pointer operand is the stack, which the model keeps disjoint from the tracked memory.
  Place place(State &s, const M68kEffectiveAddress &ea, Width w) {
    Place p;
    const unsigned r = ea.reg & 7U;
    const std::uint32_t n = nbytes(w);
    switch (ea.mode) {
    case M68kEaMode::data_register: p.kind = Place::Kind::dreg; p.reg = r; return p;
    case M68kEaMode::address_register: p.kind = Place::Kind::areg; p.reg = r; return p;
    case M68kEaMode::immediate: p.kind = Place::Kind::imm; p.imm = K(mask_of(w, ea.immediate_value)); return p;
    case M68kEaMode::address_indirect:
    case M68kEaMode::address_postinc:
    case M68kEaMode::address_predec:
    case M68kEaMode::address_disp16: {
      if (r == 7U) {
        p.kind = Place::Kind::stack;
        if (ea.mode == M68kEaMode::address_postinc || ea.mode == M68kEaMode::address_predec) s.top_dirty = true;
        return p;
      }
      if (ea.mode == M68kEaMode::address_predec && s.a[r].known()) s.a[r] = K(s.a[r].v - n);
      else if (ea.mode == M68kEaMode::address_predec) s.a[r] = {};
      Val at = s.a[r];
      if (ea.mode == M68kEaMode::address_disp16 && at.known()) at = K(at.v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
      else if (ea.mode == M68kEaMode::address_disp16) at = {};
      if (ea.mode == M68kEaMode::address_postinc) {
        s.a[r] = s.a[r].known() ? K(s.a[r].v + n) : Val{};
      }
      p.kind = at.known() ? Place::Kind::mem : Place::Kind::unknown_mem;
      p.addr = at.v & 0x00FFFFFFU;
      return p;
    }
    case M68kEaMode::absolute_word:
    case M68kEaMode::absolute_long: p.kind = Place::Kind::mem; p.addr = m68k_data_access_ea_address(ea) & 0x00FFFFFFU; return p;
    case M68kEaMode::pc_disp16: p.kind = Place::Kind::mem; p.addr = ea.absolute_address & 0x00FFFFFFU; p.pc_rel = true; return p;
    case M68kEaMode::address_index8:
    case M68kEaMode::pc_index8: {
      const auto a = indexed_address(s, ea);
      p.kind = a ? Place::Kind::mem : Place::Kind::unknown_mem;
      p.addr = a ? *a : 0U;
      p.pc_rel = ea.mode == M68kEaMode::pc_index8;
      return p;
    }
    default: return p;
    }
  }

  Val read_place(const State &s, const Place &p, Width w) {
    switch (p.kind) {
    case Place::Kind::dreg: return exact(s.d[p.reg], w);
    case Place::Kind::areg: return exact(s.a[p.reg], w);
    case Place::Kind::imm: return p.imm;
    case Place::Kind::mem: {
      if (p.addr >= kGateArraySub && p.addr < kGateArraySubEnd) return {};
      if (p.addr + nbytes(w) <= kMemTrackedEnd && !volatile_read(s, p.addr, p.addr + nbytes(w))) {
        std::uint32_t v = 0;
        bool all = true, disc = true;
        for (std::uint32_t i = 0; i < nbytes(w) && all; ++i) {
          auto b = s.mem.get(p.addr + i);
          if (b) disc = disc && s.mem.get_disc(p.addr + i); else disc = false;
          if (!b && !s.image_dirty && p.addr + i >= base_ && p.addr + i < end_) b = bytes_[first_offset_ + (p.addr + i - base_)];   // the admitted image, until something may have rewritten it
          if (!b) all = false; else v = (v << 8) | *b;
        }
        if (all) { if (!p.pc_rel) ++mem_resolved_reads_; Val r = K(v); r.disc = disc; return r; }
      }
      if (p.pc_rel && options_.constant_dispatch && !ws_) return image_read(p.addr, w);   // (in the work-variable model the generic path above already reads the image, until it may have changed)
      return {};
    }
    default: return {};
    }
  }

  void write_place(State &s, const Place &p, Width w, const Val &v) {
    switch (p.kind) {
    case Place::Kind::dreg: s.d[p.reg] = merge(s.d[p.reg], v, w); return;
    case Place::Kind::mem:
      if (p.addr >= kGateArraySub && p.addr < kGateArraySubEnd) { hw_write(s, p.addr - kGateArraySub, w, v); return; }
      if (p.addr + nbytes(w) <= kMemTrackedEnd) {
        if (!v.known() && p.addr < end_ && p.addr + nbytes(w) > base_) s.image_dirty = true;
        mem_write(s.mem, p.addr, w, v);
        note_write(s, p.addr, p.addr + nbytes(w));
      }
      return;
    case Place::Kind::unknown_mem: s.mem.clear(); s.image_dirty = true; note_write_unknown(s); taint_hw(s); return;
    case Place::Kind::stack: s.top_dirty = true; return;   // an unknown alias: nothing tracked survives
    default: return;
    }
  }

  static bool is_async(const State &s) { return s.root >= 2U; }
  void note_write(const State &s, std::uint32_t lo, std::uint32_t hi) { if (is_async(s)) async_writes_.add(lo, hi); }
  void note_write_unknown(const State &s) { if (is_async(s)) async_writes_.all = true; }
  [[nodiscard]] bool volatile_read(const State &s, std::uint32_t lo, std::uint32_t hi) const { return !is_async(s) && assumed_.overlaps(lo, hi); }

  void set_logic_flags(State &s, const Val &v, Width w) {
    const Val x = exact(v, w);
    if (!x.known()) { s.fl = {}; return; }
    const std::uint32_t sign = w == Width::byte ? 0x80U : w == Width::word ? 0x8000U : 0x80000000U;
    s.fl = known_flags((x.v & sign) != 0U, x.v == 0U, false, false);
  }

  // Whether the condition is decided by the tracked flags (every assignment of the unknown bits agrees), using the CPU-owned truth table.
  std::optional<bool> decide(const State &s, M68kCondition condition) const {
    std::optional<bool> result;
    for (unsigned a = 0; a < 16U; ++a) {
      if (((a ^ s.fl.val) & s.fl.mask) != 0U) continue;
      const bool r = m68k_evaluate_condition(condition, static_cast<std::uint16_t>(a));
      if (!result) result = r;
      else if (*result != r) return std::nullopt;
    }
    return result;
  }

  // Executes one instruction in the work-variable model. Returns false for an instruction it does not model (the footprint fallback then runs).
  bool exec_ws(State &s, const M68kIrOperation &op) {
    const Width w = op.size;
    switch (op.kind) {
    case M68kIrKind::write_moveq: {
      const Val v = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)));
      s.d[static_cast<unsigned>(op.destination) & 7U] = v;
      set_logic_flags(s, v, Width::long_word);
      return true;
    }
    case M68kIrKind::write_move: {
      const bool pop = op.source_ea.mode == M68kEaMode::address_postinc && (op.source_ea.reg & 7U) == 7U && w == Width::long_word;
      Val v;
      if (pop) {
        if (s.rets.size() == 1U && !s.rets[0].empty() && !s.top_dirty) { v = K(s.rets[0].back()); s.rets[0].pop_back(); }
        s.top_dirty = true;
      } else {
        const Place src = place(s, op.source_ea, w);
        v = read_place(s, src, w);
      }
      const Place dst = place(s, op.destination_ea, w);
      write_place(s, dst, w, v);
      if (dst.kind == Place::Kind::stack) s.top_dirty = true;
      set_logic_flags(s, v, w);
      return true;
    }
    case M68kIrKind::write_movea: {
      const Place src = place(s, op.source_ea, w);
      Val v = read_place(s, src, w);
      if (v.known() && w == Width::word) v = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.v))));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg == 7U) s.top_dirty = true;
      else s.a[reg] = v.known() || w == Width::long_word ? v : Val{};
      return true;
    }
    case M68kIrKind::write_clr: {
      const Place dst = place(s, op.destination_ea, w);
      write_place(s, dst, w, K(0));
      s.fl = known_flags(false, true, false, false);
      return true;
    }
    case M68kIrKind::load_effective_address: {
      const auto &ea = op.source_ea;
      Val v{};
      if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16) v = K(ea.absolute_address);
      else if (ea.mode == M68kEaMode::address_indirect) v = s.a[ea.reg & 7U];
      else if (ea.mode == M68kEaMode::address_disp16 && s.a[ea.reg & 7U].known()) v = K(s.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
      else if (ea.mode == M68kEaMode::address_index8 || ea.mode == M68kEaMode::pc_index8) { if (const auto a = indexed_address(s, ea)) v = K(*a); }
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg == 7U) s.top_dirty = true;
      else s.a[reg] = v;
      return true;
    }
    case M68kIrKind::add: case M68kIrKind::add_immediate: case M68kIrKind::add_quick:
    case M68kIrKind::subtract: case M68kIrKind::subtract_immediate: case M68kIrKind::subtract_quick: {
      const bool sub = op.kind == M68kIrKind::subtract || op.kind == M68kIrKind::subtract_immediate || op.kind == M68kIrKind::subtract_quick;
      const Place src = place(s, op.source_ea, w);
      const Place dst = place(s, op.destination_ea, w);
      if (dst.kind == Place::Kind::areg) {   // ADDQ/SUBQ to an address register: the whole register, no condition codes
        const Val sv = read_place(s, src, Width::long_word);
        const Val cur = s.a[dst.reg];
        if (dst.reg == 7U) { s.top_dirty = true; return true; }
        s.a[dst.reg] = cur.known() && sv.known() ? K(sub ? cur.v - sv.v : cur.v + sv.v) : Val{};
        return true;
      }
      const Val dv = read_place(s, dst, w), sv = read_place(s, src, w);
      if (dv.known() && sv.known()) {
        const std::uint32_t result = sub ? m68k_evaluate_subtraction(sv.v, dv.v, w).result : m68k_evaluate_addition(sv.v, dv.v, w).result;
        write_place(s, dst, w, K(result));
        if (sub) { const auto r = m68k_evaluate_subtraction(sv.v, dv.v, w); s.fl = known_flags(r.negative, r.zero, r.overflow, r.carry); }
        else { const auto r = m68k_evaluate_addition(sv.v, dv.v, w); s.fl = known_flags(r.negative, r.zero, r.overflow, r.carry); }
      } else {
        write_place(s, dst, w, Val{});
        s.fl = {};
      }
      return true;
    }
    case M68kIrKind::add_address: case M68kIrKind::subtract_address: {
      const Place src = place(s, op.source_ea, w);
      Val sv = read_place(s, src, w);
      if (sv.known() && w == Width::word) sv = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(sv.v))));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg == 7U) { s.top_dirty = true; return true; }
      const Val cur = s.a[reg];
      const bool sub = op.kind == M68kIrKind::subtract_address;
      s.a[reg] = cur.known() && sv.known() ? K(sub ? cur.v - sv.v : cur.v + sv.v) : Val{};
      return true;
    }
    case M68kIrKind::compare: case M68kIrKind::compare_immediate: case M68kIrKind::compare_address: case M68kIrKind::compare_memory: {
      const bool addr = op.kind == M68kIrKind::compare_address;
      const Width cw = addr ? Width::long_word : w;
      const Place src = place(s, op.source_ea, w);
      const Place dst = place(s, op.destination_ea, cw);
      Val sv = read_place(s, src, w);
      if (addr && sv.known() && w == Width::word) sv = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(sv.v))));
      const Val dv = read_place(s, dst, cw);
      if ((dv.known() && dv.disc && !sv.known()) || (sv.known() && sv.disc && !dv.known())) {   // a lookup: immutable disc data compared with a key the walk does not know
        refuse(LoaderRefusalKind::lookup_key_unknown, s.pc);
        lookup_unknown_ = true;
      }
      if (dv.known() && sv.known()) {
        const auto r = m68k_evaluate_subtraction(sv.v, dv.v, cw);
        s.fl = known_flags(r.negative, r.zero, r.overflow, r.carry);
      } else {
        s.fl = {};
      }
      return true;
    }
    case M68kIrKind::test_operand: {
      const Place src = place(s, op.source_ea, w);
      set_logic_flags(s, read_place(s, src, w), w);
      return true;
    }
    default: return false;
    }
  }

  struct Inval {
    enum class Kind : std::uint8_t { none, range, all } kind{Kind::none};
    std::uint32_t lo{}, hi{};
  };

  // The memory an unmodelled instruction may write, from the pre-instruction state (a stack-pointer destination is the stack: disjoint by the model's assumption).
  Inval invalidation_for(const State &s, const M68kIrOperation &op) const {
    Inval out;
    const auto &ea = op.destination_ea;
    const unsigned r = ea.reg & 7U;
    const std::uint32_t n = op.kind == M68kIrKind::movem_transfer ? 64U : op.kind == M68kIrKind::movep_transfer ? 2U * nbytes(op.size) - 1U : nbytes(op.size);
    switch (ea.mode) {
    case M68kEaMode::address_indirect: case M68kEaMode::address_postinc: case M68kEaMode::address_predec: case M68kEaMode::address_disp16: case M68kEaMode::address_index8: {
      if (op.kind == M68kIrKind::movem_transfer && op.movem_direction != M68kMovemDirection::registers_to_memory) return out;
      if (r == 7U && ea.mode != M68kEaMode::address_index8) return out;
      std::optional<std::uint32_t> at;
      if (ea.mode == M68kEaMode::address_index8) at = indexed_address(s, ea);
      else if (s.a[r].known()) {
        at = s.a[r].v - (ea.mode == M68kEaMode::address_predec ? n : 0U) + (ea.mode == M68kEaMode::address_disp16 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)) : 0U);
      }
      if (!at) { out.kind = Inval::Kind::all; return out; }
      out = {Inval::Kind::range, *at & 0x00FFFFFFU, (*at & 0x00FFFFFFU) + n};
      return out;
    }
    case M68kEaMode::absolute_word: case M68kEaMode::absolute_long: {
      const std::uint32_t a = m68k_data_access_ea_address(ea) & 0x00FFFFFFU;
      out = {Inval::Kind::range, a, a + n};
      return out;
    }
    default: return out;
    }
  }

  static bool preserves_condition_codes(M68kIrKind k) {
    switch (k) {
    case M68kIrKind::write_movea: case M68kIrKind::load_effective_address: case M68kIrKind::jump_general: case M68kIrKind::call_general:
    case M68kIrKind::bsr_call: case M68kIrKind::branch_ne_short: case M68kIrKind::branch_always_short: case M68kIrKind::general_branch:
    case M68kIrKind::return_from_subroutine: case M68kIrKind::no_operation: case M68kIrKind::movem_transfer: case M68kIrKind::dbcc_loop:
    case M68kIrKind::push_effective_address: case M68kIrKind::link_frame: case M68kIrKind::unlink_frame: case M68kIrKind::add_address:
    case M68kIrKind::subtract_address: case M68kIrKind::read_status_register: case M68kIrKind::exchange_registers:
      return true;
    default: return false;
    }
  }

  // ---- the documented BIOS CD services (ADR 0215) ------------------------------------------------------------------------
  void forget_all(State &s) {
    s.mem.clear();
    s.image_dirty = true;
    note_write_unknown(s);
    s.cd = Cd{};
    taint_hw(s);
    s.fl = {};
  }

  // Functions of the public _CDBIOS ABI that, per the two agreeing sources (ADR 0215), read registers and BIOS-owned tables and write no caller memory.
  static bool documented_without_caller_memory(std::uint32_t fn) {
    switch (fn) {
    case 0x02U: case 0x08U: case 0x09U: case 0x0AU: case 0x10U: case 0x11U: case 0x12U: case 0x13U: case 0x14U: case 0x15U: case 0x16U: case 0x18U: case 0x19U:
    case 0x80U: case 0x81U: case 0x82U: case 0x83U: case 0x84U: case 0x85U: case 0x86U: case 0x87U: case 0x96U: return true;
    default: return false;
    }
  }

  void bios_call(State &s, std::uint32_t here, const Val &fn) {
    const auto clobber = [&] { s.d[0] = {}; s.d[1] = {}; s.a[0] = {}; s.a[1] = {}; s.fl = {}; };
    const Cd before = s.cd;
    const Val dest = s.hw.dest;
    switch (fn.known() ? fn.v & 0xFFFFU : 0xFFFFFFFFU) {
    case 0x20U: {   // ROMREADN: A0 -> {first sector, count}, two big-endian longs in memory
      modelled_sites_.insert({here, 0x20U});
      const Val a0 = s.a[0];
      Val pos{}, left{};
      if (a0.known() && a0.v + 8U <= kMemTrackedEnd && !volatile_read(s, a0.v, a0.v + 8U)) { pos = mem_read(s.mem, a0.v, Width::long_word); left = mem_read(s.mem, a0.v + 4U, Width::long_word); }
      (pos.known() && left.known() ? args_resolved_ : args_unresolved_).insert(here);
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      s.cd = Cd{pos.known() && left.known() ? pos : Val{}, pos.known() && left.known() ? left : Val{}, false};
      break;
    }
    case 0x89U:   // CDCSTOP
      modelled_sites_.insert({here, 0x89U});
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      s.cd = Cd{};
      break;
    case 0x8AU:   // CDCSTAT (carry: a sector is ready; unknown to the walk, both arms are explored)
      modelled_sites_.insert({here, 0x8AU});
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      break;
    case 0x8BU:   // CDCREAD
      modelled_sites_.insert({here, 0x8BU});
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      s.cd.header_read = true;
      break;
    case 0x8DU:   // CDCACK
      modelled_sites_.insert({here, 0x8DU});
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      if (s.cd.pos.known()) s.cd.pos = K(s.cd.pos.v + 1U);
      if (s.cd.left.known()) s.cd.left = K(s.cd.left.v - 1U);
      s.cd.header_read = false;
      break;
    case 0x8CU:   // CDCTRN: A0 = data buffer, A1 = header buffer; sub-read device only
      modelled_sites_.insert({here, 0x8CU});
      cdc_transfer(s, here, before);
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      break;
    case 0x17U: {   // ROMREAD: A0 -> one long sector; reads on until stopped (the count is not known)
      modelled_sites_.insert({here, 0x17U});
      const Val a0 = s.a[0];
      Val pos{};
      if (a0.known() && a0.v + 4U <= kMemTrackedEnd && !volatile_read(s, a0.v, a0.v + 4U)) pos = mem_read(s.mem, a0.v, Width::long_word);
      (pos.known() ? args_resolved_ : args_unresolved_).insert(here);
      clobber();
      s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
      s.cd = Cd{pos, Val{}, false};
      break;
    }
    default:
      if (documented_without_caller_memory(fn.known() ? fn.v & 0xFFFFU : 0xFFFFFFFFU)) {   // ADR 0215: register in/out and BIOS-owned tables only; the read position is lost unless the call is a status/TOC/fader query
        modelled_sites_.insert({here, fn.v & 0xFFFFU});
        clobber();
        s.mem.clear_range(0U, kLoaderScanBiosAreaEnd);
        if ((fn.v & 0xFFFFU) < 0x80U || (fn.v & 0xFFFFU) == 0x87U || (fn.v & 0xFFFFU) == 0x96U) s.cd = Cd{};
        break;
      }
      unmodelled_sites_.insert(here);   // anything else (also a service number the walk does not know) has effects the walk cannot bound
      clobber();
      forget_all(s);
      return;
    }
    // A modelled service keeps the device byte the caller selected ($FF8004 is the caller's, ADR 0215 assumption); the rest of the CD interface shadow is lost.
    taint_hw(s);
    s.hw.dest = dest;
  }

  // The sector's bytes are immutable disc content: known concretely (so title code that parses them, e.g. an ISO9660 directory search, can be evaluated and a module can be walked).
  void deposit_sector(State &s, std::uint32_t lba, std::uint32_t destination, std::uint32_t forget_span) {
    note_write(s, destination, destination + forget_span);
    s.mem.clear_range(destination, destination + forget_span);
    bool read = false;
    if (!disc_.tracks().empty() && disc_.tracks().front().type != DiscTrackType::audio && lba < disc_.tracks().front().sector_count) {
      std::array<std::uint8_t, kDiscUserBytes> user{};
      if (disc_.read_user_data(disc_.tracks().front().first_lba + lba, std::span<std::uint8_t, kDiscUserBytes>(user.data(), kDiscUserBytes)) == DiscError::none) {
        s.mem.put_bytes(destination, std::span<const std::uint8_t>(user.data(), kLoaderScanSectorBytes), true);
        read = true;
      }
    }
    if (!read) refuse(LoaderRefusalKind::source_outside_track, s.pc);
  }

  void cdc_transfer(State &s, std::uint32_t here, const Cd &cd) {
    if (!s.hw.dest.known()) { refuse(LoaderRefusalKind::destination_unknown, here); transfers_unresolved_.insert(here); return; }
    if (s.hw.dest.v != 3U || !cd.header_read) return;   // CDCTRN requires the sub-read device and a CDCREAD'ed sector: otherwise it transfers nothing
    if (cd.left.known() && static_cast<std::int32_t>(cd.left.v) <= 0) return;   // the read already delivered every requested sector
    if (!cd.pos.known()) { refuse(LoaderRefusalKind::lba_unknown, here); transfers_unresolved_.insert(here); return; }
    const Val a0 = s.a[0];
    if (!a0.known()) { refuse(LoaderRefusalKind::destination_unknown, here); transfers_unresolved_.insert(here); return; }
    const std::uint32_t destination = a0.v & 0x00FFFFFFU;
    if (destination < kLoaderScanBiosAreaEnd || static_cast<std::uint64_t>(destination) + kLoaderScanSectorBytes > kMemTrackedEnd ||
        (destination < end_ && destination + kLoaderScanSectorBytes > base_)) {
      refuse(LoaderRefusalKind::destination_unsupported, here);
      transfers_unresolved_.insert(here);
      return;
    }
    transfers_resolved_.insert(here);
    std::uint32_t op = 0;
    for (; op < scan_.operations.size(); ++op) {
      const auto &o = scan_.operations[op];
      if (o.trigger_pc == here && o.lba == cd.pos.v && o.destination == destination) break;
    }
    if (op == scan_.operations.size()) {
      if (scan_.operations.size() >= kLoaderScanMaxOperations) {
        refuse(LoaderRefusalKind::too_many_operations, here);
        scan_.complete = false;
        return;
      }
      scan_.operations.push_back(LoadOperation{here, cd.pos.v, destination, kLoaderScanSectorBytes, {}});
    }
    auto &loaded = s.loaded;
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(), [&](const Loaded &l) { return l.base < destination + kLoaderScanSectorBytes && destination < l.base + l.bytes; }), loaded.end());
    loaded.push_back({op, destination, kLoaderScanSectorBytes});
    note_write(s, destination, destination + kLoaderScanCdcTrnSpan);
    deposit_sector(s, cd.pos.v, destination, kLoaderScanCdcTrnSpan);
    if (s.a[1].known()) { const std::uint32_t h = s.a[1].v & 0x00FFFFFFU; s.mem.clear_range(h, h + 4U); note_write(s, h, h + 4U); if (h < end_ && h + 4U > base_) s.image_dirty = true; }
    else { s.mem.clear(); note_write_unknown(s); }
  }

  // ---- decode ------------------------------------------------------------------------------------------------------------
  std::optional<M68kIrOperation> decode_at(std::uint32_t pc) const {
    if (pc < base_ || pc >= end_ || (pc & 1U) != 0U) return std::nullopt;
    const std::uint32_t offset = pc - base_ + first_offset_;
    const auto visible = bytes_.subspan(0, static_cast<std::size_t>(first_offset_) + (end_ - base_));
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(visible, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return std::nullopt;
    auto operation = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq) return std::nullopt;
    return operation;
  }

  // Under recursive_modules a proven-loaded range is executable by the walk too: its instructions are decoded from the abstract memory, whose bytes are the sector the walk proved (or unknown once
  // anything may have changed them). Only a transfer the entry proof accepted leads here; data is never walked because it happens to decode.
  std::optional<M68kIrOperation> decode_state(const State &s, std::uint32_t pc) const {
    if (pc >= base_ && pc < end_) return decode_at(pc);
    if (!ws_ || !options_.recursive_modules || (pc & 1U) != 0U || pc + 2U > kMemTrackedEnd) return std::nullopt;
    std::array<std::uint8_t, 10> buf{};
    std::size_t n = 0;
    for (; n < buf.size(); ++n) {
      const auto b = s.mem.get(pc + static_cast<std::uint32_t>(n));
      if (!b) break;
      buf[n] = *b;
    }
    if (n < 2U) return std::nullopt;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{0}};
    const auto result = decode_m68k_instruction(std::span<const std::uint8_t>(buf.data(), n), source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return std::nullopt;
    auto operation = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq) return std::nullopt;
    return operation;
  }

  // ---- data effect of one instruction --------------------------------------------------------------------------------
  void apply_data(State &s, const M68kIrOperation &op) {
    std::optional<State> pre;   // the state before the instruction, for the folded arithmetic and the table read below (the footprint clears the destination first)
    std::optional<State> pre_move;
    if (options_.constant_dispatch && op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::pc_index8) pre_move = s;
    if (options_.constant_dispatch && op.destination_ea.mode == M68kEaMode::data_register &&
        (op.kind == M68kIrKind::add || op.kind == M68kIrKind::add_immediate || op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::subtract ||
         op.kind == M68kIrKind::subtract_immediate || op.kind == M68kIrKind::subtract_quick))
      pre = s;
    const auto effect = m68k_operation_effect(op);
    const bool pure = op.kind == M68kIrKind::write_moveq || op.kind == M68kIrKind::write_move || op.kind == M68kIrKind::write_movea ||
                      op.kind == M68kIrKind::write_clr || op.kind == M68kIrKind::load_effective_address;
    // Kinds that write no data or address register other than the stack pointer (compares, tests, branches, calls, returns): arguments survive.
    const bool no_register_write =
        op.kind == M68kIrKind::compare || op.kind == M68kIrKind::compare_immediate || op.kind == M68kIrKind::compare_address ||
        op.kind == M68kIrKind::test_operand || op.kind == M68kIrKind::bit_test || op.kind == M68kIrKind::no_operation ||
        op.kind == M68kIrKind::general_branch || op.kind == M68kIrKind::branch_ne_short || op.kind == M68kIrKind::branch_always_short ||
        op.kind == M68kIrKind::bsr_call || op.kind == M68kIrKind::jump_general || op.kind == M68kIrKind::call_general ||
        op.kind == M68kIrKind::return_from_subroutine ||
        // written explicitly below / by the walker (the effect owner does not advertise a complete footprint for these)
        op.kind == M68kIrKind::write_moveq || op.kind == M68kIrKind::load_effective_address || op.kind == M68kIrKind::dbcc_loop;
    // The footprint first (what the walk does not model becomes Unknown); the explicit results below override it.
    // A MOVEM store (registers to memory) writes no data or address register except an auto-decremented A7 (modelled: the stack is never a register block).
    const bool movem_store = options_.constant_dispatch && op.kind == M68kIrKind::movem_transfer && op.movem_direction == M68kMovemDirection::registers_to_memory &&
                             (op.destination_ea.mode == M68kEaMode::address_predec || op.destination_ea.mode == M68kEaMode::address_indirect) && (op.destination_ea.reg & 7U) == 7U;
    if (no_register_write || movem_store) {
      // nothing is written
    } else if (!effect.register_write_footprint_complete) {
      for (auto &v : s.d) v = {};
      for (std::size_t i = 0; i < 7; ++i) s.a[i] = {};
    } else {
      for (unsigned r = 0; r < 8; ++r) {
        if ((effect.data_register_write_mask >> r) & 1U) s.d[r] = {};
        if (r < 7 && ((effect.address_register_write_mask >> r) & 1U)) s.a[r] = {};
      }
    }
    const Width w = op.size;
    if (pre) {   // ADD/SUB into a data register with known operands: the result is known (the condition codes are not modelled)
      const unsigned r = op.destination_ea.reg & 7U;
      const Val before = exact(pre->d[r], w);
      const Val src = read_ea(*pre, op.source_ea, w);
      if (before.known() && src.known()) {
        const bool sub = op.kind == M68kIrKind::subtract || op.kind == M68kIrKind::subtract_immediate || op.kind == M68kIrKind::subtract_quick;
        const std::uint32_t result = mask_of(w, sub ? before.v - src.v : before.v + src.v);
        s.d[r] = merge(pre->d[r], K(result), w);
      }
      return;
    }
    switch (op.kind) {
    case M68kIrKind::write_moveq:
      s.d[static_cast<unsigned>(op.destination) & 7U] = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)));
      return;
    case M68kIrKind::write_move:
      write_memory_ea(s, op.destination_ea, w, read_ea(pre_move ? *pre_move : s, op.source_ea, w));
      return;
    case M68kIrKind::write_clr:
      write_memory_ea(s, op.destination_ea, w, K(0));
      return;
    case M68kIrKind::write_movea: {
      Val v = read_ea(s, op.source_ea, w);
      if (v.known() && w == Width::word) v = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v.v))));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg != 7U) s.a[reg] = v;
      return;
    }
    case M68kIrKind::load_effective_address: {
      const auto &ea = op.source_ea;
      Val v{};
      if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16) v = K(ea.absolute_address);
      else if (ea.mode == M68kEaMode::address_indirect) v = s.a[ea.reg & 7U];
      else if (ea.mode == M68kEaMode::address_disp16 && s.a[ea.reg & 7U].known())
        v = K(s.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
      const unsigned reg = op.destination_ea.reg & 7U;
      if (reg != 7U) s.a[reg] = v;
      return;
    }
    default: break;
    }
    if (!pure) {
      // An instruction the walk does not model that stores to memory may store to a Gate Array register.
      const auto mode = op.destination_ea.mode;
      const bool stores = mode != M68kEaMode::unused && mode != M68kEaMode::data_register && mode != M68kEaMode::address_register;
      if (stores && op.kind != M68kIrKind::compare && op.kind != M68kIrKind::compare_immediate && op.kind != M68kIrKind::compare_address &&
          op.kind != M68kIrKind::test_operand && op.kind != M68kIrKind::bit_test) {
        if (mode == M68kEaMode::absolute_word || mode == M68kEaMode::absolute_long) {
          const std::uint32_t address = op.destination_ea.absolute_address & 0x00FFFFFFU;
          if (address >= kGateArraySub && address < kGateArraySubEnd) taint_hw(s);
        } else if (!((mode == M68kEaMode::address_predec || mode == M68kEaMode::address_postinc || mode == M68kEaMode::address_indirect) &&
                     (op.destination_ea.reg & 7U) == 7U)) {
          taint_hw(s);
        }
      }
      if (op.kind == M68kIrKind::movem_transfer) taint_hw(s);
    }
  }

  // ---- control -----------------------------------------------------------------------------------------------------------
  // The taken arm of a conditional: a target inside a loaded range is an entry proof (when it comes from outside that range) and, under recursion, is still walked.
  bool queue_taken(State &taken, std::uint32_t target) {
    const bool landed = note_entry(taken, target, true);
    return !landed || (ws_ && options_.recursive_modules && decode_state(taken, target));
  }

  // A transfer to `target`: records the entry when it lands in a load of this path. Returns true when it did.
  // `direct`: a statically encoded branch or call. Inside a proven module such a transfer from the module's own code is not a new entry (the traversal follows it); computed ones are.
  bool note_entry(State &s, std::uint32_t target, bool direct = false) {
    bool landed = false;
    for (const auto &l : s.loaded) {
      if (target >= l.base && target < l.base + l.bytes) {
        // the same module = the same run of consecutive sectors (disc and memory advance together)
        const bool from_inside = ws_ && options_.recursive_modules && std::any_of(s.loaded.begin(), s.loaded.end(), [&](const Loaded &o) {
          return cur_pc_ >= o.base && cur_pc_ < o.base + o.bytes &&
                 static_cast<std::int64_t>(scan_.operations[o.op].lba) - static_cast<std::int64_t>(scan_.operations[l.op].lba) ==
                     (static_cast<std::int64_t>(o.base) - static_cast<std::int64_t>(l.base)) / static_cast<std::int64_t>(kLoaderScanSectorBytes);
        });
        if (ws_ && !(s.mem.get_disc(target) && s.mem.get_disc(target + 1U))) {   // the bytes at the target are no longer known to be the disc's (decoded in place, overwritten, forgotten): no source claim
          refuse(LoaderRefusalKind::loaded_bytes_unknown, s.pc);
          continue;
        }
        if (direct && from_inside) { landed = true; continue; }
        auto &entries = scan_.operations[l.op].entries;
        if (std::find(entries.begin(), entries.end(), target) == entries.end()) entries.push_back(target);
        landed = true;
      }
    }
    return landed;
  }

  void havoc_after_foreign_call(State &s) {
    for (auto &v : s.d) v = {};
    for (std::size_t i = 0; i < 7; ++i) s.a[i] = {};
    taint_hw(s);
    if (ws_) forget_all(s);   // an unknown callee may write any tracked byte
  }

  // After `widen_after` distinct states at one (pc, root, call stack) the next ones are joined into one state that only ever loses knowledge, so a loop whose state keeps changing
  // reaches a fixed point; a state already covered by the joined one ends its path. Within the threshold exploration is exact.
  bool admit(State &s) {
    if (!options_.state_convergence) return true;
    auto &h = heads_[head_key(s)];
    if (++h.count <= options_.widen_after) return true;
    if (!h.have) { h.have = true; h.joined = s; return true; }
    State j = join_states(h.joined, s);
    if (j.rets.size() > kLoaderScanMaxReturnContexts) { exhausted(s.pc); return false; }   // more callers than the bound: not summarised, reported incomplete
    const std::string before = serialize(h.joined);
    if (serialize(j) == before) return false;   // covered: the joined state was (or will be) explored
    ++joins_;
    if (serialize(j) != serialize(s)) ++widenings_;
    h.joined = j;
    s = std::move(j);
    seen_.insert(serialize(s));
    return true;
  }

  void push(std::vector<State> &stack, State &&next) {
    if (scan_.states >= kLoaderScanMaxStates) { exhausted(next.pc); return; }
    if (!seen_.insert(serialize(next)).second) return;
    if (!admit(next)) return;
    ++scan_.states;
    stack.push_back(std::move(next));
  }

  void walk(State s, std::vector<State> &stack) {
    for (;;) {
      if (!scan_.complete) return;
      if (++scan_.steps > kLoaderScanMaxSteps) { exhausted(s.pc); return; }
      const auto decoded = decode_state(s, s.pc);
      if (!decoded) {
        if (ws_ && options_.recursive_modules && (s.pc < base_ || s.pc >= end_) &&
            std::any_of(s.loaded.begin(), s.loaded.end(), [&](const Loaded &l) { return s.pc >= l.base && s.pc < l.base + l.bytes; }))
          refuse(LoaderRefusalKind::loaded_bytes_unknown, s.pc);   // proven module code that cannot be decoded from known bytes: not silently dropped
        end_at("undecodable_or_outside", s.pc);
        return;
      }  // outside the boot image (a loaded module's code is not walked) or not an instruction
      const M68kIrOperation &op = *decoded;
      const std::uint32_t here = s.pc;
      cur_pc_ = here;
      const std::uint32_t next = here + op.provenance.length.value;
      if (op.kind == M68kIrKind::stop_until_interrupt) { end_at("stop", here); return; }
      if (op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::address_postinc && op.destination_ea.mode == M68kEaMode::address_postinc) copy_sites_.insert(here);
      if (op.kind == M68kIrKind::dbcc_loop) loop_sites_.insert(here);
      if (options_.constant_dispatch && op.kind == M68kIrKind::write_move && op.destination_ea.mode == M68kEaMode::data_register && (op.source_ea.reg & 7U) != 7U &&
          (op.source_ea.mode == M68kEaMode::address_indirect || op.source_ea.mode == M68kEaMode::address_postinc || op.source_ea.mode == M68kEaMode::address_disp16))
        memory_read_sites_.insert(here);
      if (!ws_) apply_data(s, op);
      else if (!exec_ws(s, op)) {
        const Inval inval = invalidation_for(s, op);
        const bool touches_stack = op.kind == M68kIrKind::link_frame || op.kind == M68kIrKind::unlink_frame || op.kind == M68kIrKind::push_effective_address ||
                                   op.kind == M68kIrKind::movem_transfer || op.destination_ea.mode == M68kEaMode::address_predec || op.destination_ea.mode == M68kEaMode::address_postinc ||
                                   ((op.destination_ea.mode == M68kEaMode::address_indirect || op.destination_ea.mode == M68kEaMode::address_disp16) && (op.destination_ea.reg & 7U) == 7U) ||
                                   ((op.destination_ea.mode == M68kEaMode::address_indirect || op.destination_ea.mode == M68kEaMode::address_disp16) && (op.destination_ea.reg & 7U) == 7U) ||
                                   op.source_ea.mode == M68kEaMode::address_predec || op.source_ea.mode == M68kEaMode::address_postinc ||
                                   (op.destination_ea.mode == M68kEaMode::address_register && (op.destination_ea.reg & 7U) == 7U);
        // An unmodelled instruction still advances a known auto-incremented / -decremented address register by the access size.
        std::array<std::optional<Val>, 2> stepped{};
        const M68kEffectiveAddress *eas[2] = {&op.source_ea, &op.destination_ea};
        for (int k = 0; k < 2; ++k) {
          const auto &e = *eas[k];
          const unsigned r = e.reg & 7U;
          if ((e.mode == M68kEaMode::address_postinc || e.mode == M68kEaMode::address_predec) && r != 7U && s.a[r].known() && op.destination_ea.mode != M68kEaMode::address_register && op.kind != M68kIrKind::movem_transfer)
            stepped[static_cast<std::size_t>(k)] = K(e.mode == M68kEaMode::address_postinc ? s.a[r].v + nbytes(op.size) : s.a[r].v - nbytes(op.size));
        }
        apply_data(s, op);
        for (int k = 0; k < 2; ++k) if (stepped[static_cast<std::size_t>(k)]) s.a[eas[k]->reg & 7U] = *stepped[static_cast<std::size_t>(k)];
        if (inval.kind == Inval::Kind::all) { s.mem.clear(); s.image_dirty = true; note_write_unknown(s); }
        else if (inval.kind == Inval::Kind::range) {
          s.mem.clear_range(inval.lo, inval.hi);
          note_write(s, inval.lo, inval.hi);
          if (inval.lo < end_ && inval.hi > base_) s.image_dirty = true;
        }
        if (!preserves_condition_codes(op.kind)) s.fl = {};
        if (touches_stack) s.top_dirty = true;
      }
      if (lookup_unknown_) { lookup_unknown_ = false; end_at("lookup_key_unknown", here); return; }   // no source is proposed behind a search with an unknown key
      const auto control = m68k_control_successors(op);
      if (control.always_raises_exception) { end_at("exception", here); return; }
      if (control.dynamic != M68kDynamicControlFamily::none) {
        if (control.dynamic == M68kDynamicControlFamily::return_from_subroutine) {
          // One continuation per possible return stack (a joined state may have several callers); the first continues this walk, the others are queued.
          bool continued = false;
          const auto stacks = s.rets;
          for (const auto &rs : stacks) {
            if (rs.empty()) { end_at("return_to_caller", here); continue; }
            State t = s;
            t.rets = {std::vector<std::uint32_t>(rs.begin(), rs.end() - 1)};
            t.pc = rs.back();
            t.top_dirty = true;
            if (!continued) { s = std::move(t); continued = true; }
            else push(stack, std::move(t));
          }
          if (!continued) return;
        } else if (control.dynamic == M68kDynamicControlFamily::jump_address_indirect || control.dynamic == M68kDynamicControlFamily::call_address_indirect ||
                   control.dynamic == M68kDynamicControlFamily::jump_address_disp16 || control.dynamic == M68kDynamicControlFamily::call_address_disp16) {
          const bool call = control.dynamic == M68kDynamicControlFamily::call_address_indirect || control.dynamic == M68kDynamicControlFamily::call_address_disp16;
          Val target = s.a[op.source_ea.reg & 7U];
          if (target.known() && op.source_ea.mode == M68kEaMode::address_disp16)
            target = K(target.v + static_cast<std::uint32_t>(static_cast<std::int32_t>(op.source_ea.displacement)));
          if (!transfer_to(s, target, call, next, here, stack)) { end_at("computed_jump", here); return; }
        } else if (options_.constant_dispatch && (control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::jump_pc_index ||
                                                  control.dynamic == M68kDynamicControlFamily::call_address_index || control.dynamic == M68kDynamicControlFamily::jump_address_index) &&
                   indexed_address(s, op.source_ea)) {
          const bool call = control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::call_address_index;
          dispatch_sites_.insert(here);
          if (!transfer_to(s, K(*indexed_address(s, op.source_ea)), call, next, here, stack)) { end_at("computed_jump", here); return; }
        } else {
          const bool indexed_call = control.dynamic == M68kDynamicControlFamily::call_pc_index || control.dynamic == M68kDynamicControlFamily::call_address_index;
          end_at(control.dynamic == M68kDynamicControlFamily::call_pc_index ? "computed_call_pc_index" : control.dynamic == M68kDynamicControlFamily::call_address_index ? "computed_call_address_index"
                 : control.dynamic == M68kDynamicControlFamily::jump_pc_index ? "computed_jump_pc_index" : control.dynamic == M68kDynamicControlFamily::jump_address_index ? "computed_jump_address_index" : "computed_other", here);
          if (!s.loaded.empty()) refuse(LoaderRefusalKind::entry_unknown, here);
          if (options_.constant_dispatch) refuse(LoaderRefusalKind::computed_control_unresolved, here);
          if (!(indexed_call && options_.continue_after_computed_call)) return;
          havoc_after_foreign_call(s);
          s.pc = next;
        }
        if (!continue_or_fork(s, stack)) return;
        continue;
      }
      std::vector<M68kControlSuccessor> successors = control.successors;
      if (successors.empty()) return;
      // Direct calls: BSR/JSR to a constant.
      if (successors.size() == 1U && successors.front().kind == M68kControlSuccessorKind::call_target) {
        if (!transfer_to(s, K(successors.front().target), true, next, here, stack, true)) return;
        if (!continue_or_fork(s, stack)) return;
        continue;
      }
      if (successors.size() == 1U) {
        const auto &only = successors.front();
        if (only.kind == M68kControlSuccessorKind::branch_target) {
          if (!transfer_to(s, K(only.target), false, next, here, stack, true)) return;
          if (!continue_or_fork(s, stack)) return;
        } else {
          s.pc = only.target;  // straight-line flow is not a join point: no visited-set entry
        }
        continue;
      }
      if (ws_ && successors.size() == 2U && (op.kind == M68kIrKind::general_branch || op.kind == M68kIrKind::branch_ne_short || op.kind == M68kIrKind::dbcc_loop)) {
        const M68kControlSuccessor *taken_succ = nullptr, *fall_succ = nullptr;
        for (const auto &successor : successors) (successor.kind == M68kControlSuccessorKind::conditional_target ? taken_succ : fall_succ) = &successor;
        if (taken_succ != nullptr && fall_succ != nullptr) {
          const M68kCondition condition = op.kind == M68kIrKind::branch_ne_short ? M68kCondition::ne : op.condition;
          const unsigned counter = op.destination_ea.reg & 7U;
          std::optional<bool> go;   // nullopt: both arms
          if (op.kind == M68kIrKind::dbcc_loop) {
            const auto cond = decide(s, condition);
            const Val low = exact(s.d[counter], Width::word);
            if (cond && *cond) { go = false; decided_sites_.insert(here); }              // condition true: fall through, counter untouched
            else if (cond && !*cond && low.known()) {                                         // condition false: decrement the low word
              const auto r = m68k_evaluate_dbcc_decrement(low.v);
              s.d[counter] = merge(s.d[counter], K(r.updated_register & 0xFFFFU), Width::word);
              go = !r.expired;
              decided_sites_.insert(here);
            } else if (cond && !*cond) {
              s.d[counter] = merge(s.d[counter], Val{}, Width::word);
            }
            if (!go) {   // both arms: the taken arm decrements, the fall-through arm is a true condition or an expired counter
              State taken = s;
              taken.pc = taken_succ->target;
              if (low.known() && low.v == 0U && !cond) { /* an expired counter cannot take the loop unless the condition was true: the arm below covers it */ }
              else { taken.d[counter] = merge(taken.d[counter], low.known() ? K((low.v - 1U) & 0xFFFFU) : Val{}, Width::word); if (queue_taken(taken, taken_succ->target)) push(stack, std::move(taken)); }
              s.d[counter] = merge(s.d[counter], Val{}, Width::word);
              s.pc = fall_succ->target;
              if (!continue_or_fork(s, stack)) return;
              continue;
            }
          } else {
            go = decide(s, condition);
            if (go) decided_sites_.insert(here);
          }
          if (go) {
            if (*go) {
              if (!transfer_to(s, K(taken_succ->target), false, next, here, stack, true)) return;
            } else {
              s.pc = fall_succ->target;
            }
            if (!continue_or_fork(s, stack)) return;
            continue;
          }
        }
      }
      // A conditional: both arms. The not-taken arm continues here; the taken arm is queued.
      for (const auto &successor : successors) {
        if (successor.kind == M68kControlSuccessorKind::conditional_target) {
          State taken = s;
          taken.pc = successor.target;
          if (op.kind == M68kIrKind::dbcc_loop) taken.d[op.destination_ea.reg & 7U] = {};
          if (queue_taken(taken, successor.target)) push(stack, std::move(taken));
        } else {
          s.pc = successor.target;
        }
      }
      if (op.kind == M68kIrKind::dbcc_loop) s.d[op.destination_ea.reg & 7U] = {};
      if (!continue_or_fork(s, stack)) return;
    }
  }

  // Admits the state at its (already set) pc into the visited set; false ends this path.
  bool continue_or_fork(State &s, std::vector<State> &) {
    if (scan_.states >= kLoaderScanMaxStates) { exhausted(s.pc); return false; }
    if (!seen_.insert(serialize(s)).second) return false;
    if (!admit(s)) return false;
    ++scan_.states;
    return true;
  }

  // Control transfer to `target` (a call when `call`, with continuation `next`). Returns false when the path ends here.
  bool transfer_to(State &s, const Val &target, bool call, std::uint32_t next, std::uint32_t here, std::vector<State> &, bool direct = false) {
    if (!target.known()) {
      foreign_unknown_.insert(here);
      if (options_.constant_dispatch) refuse(LoaderRefusalKind::computed_control_unresolved, here);
      if (!s.loaded.empty()) refuse(LoaderRefusalKind::entry_unknown, here);
      if (call) { havoc_after_foreign_call(s); s.pc = next; return true; }
      return false;
    }
    if (note_entry(s, target.v, direct)) {  // a proven entry into a loaded range
      if (ws_ && options_.recursive_modules && decode_state(s, target.v)) {   // recursive discovery: walk the module's own code from the proven entry
        if (entered_.insert(target.v).second && entered_.size() > kLoaderScanMaxModuleEntries) { exhausted(here); return false; }
        if (call) {
          for (const auto &rs : s.rets) if (rs.size() >= kLoaderScanMaxCallDepth) { exhausted(here); return false; }
          for (auto &rs : s.rets) rs.push_back(next);
          s.top_dirty = false;
        }
        s.pc = target.v;
        return true;
      }
      if (!call) return false;
      havoc_after_foreign_call(s);
      s.pc = next;
      return true;
    }
    const bool inside = target.v >= base_ && target.v < end_;
    if (!inside) {
      if (target.v < base_) {  // a call/jump into the BIOS area below the SP image: the BIOS service interface, function number in D0.w
        const Val fn = exact(s.d[0], Width::word);
        if (fn.known()) { if (bios_sites_.insert({here, fn.v}).second) ++scan_.survey.bios_functions[fn.v]; }
        else bios_sites_.insert({here, 0xFFFFFFFFU});
        if (ws_) {   // only the public _CDBIOS vector takes a function number; its documented services have modelled effects, everything else forgets the abstract state
          if (target.v == kCdBiosVector) bios_call(s, here, fn);
          else { unmodelled_sites_.insert(here); s.d[0] = {}; s.d[1] = {}; s.a[0] = {}; s.a[1] = {}; forget_all(s); }
          if (call) { s.pc = next; return true; }
          if (s.rets.size() == 1U && !s.rets[0].empty()) {   // a tail jump to the vector: the service returns to the caller's continuation
            s.pc = s.rets[0].back();
            s.rets[0].pop_back();
            s.top_dirty = true;
            return true;
          }
          return false;
        }
      } else {
        foreign_other_.insert(here);
      }
      if (call) { havoc_after_foreign_call(s); s.pc = next; return true; }
      return false;
    }
    if (call) {
      for (const auto &rs : s.rets) if (rs.size() >= kLoaderScanMaxCallDepth) { exhausted(here); return false; }
      for (auto &rs : s.rets) rs.push_back(next);
      s.top_dirty = false;
    }
    s.pc = target.v;
    return true;
  }
};

// ---- modules -----------------------------------------------------------------------------------------------------------------

struct Hull {
  bool ok{};
  std::uint32_t lo{}, hi{};  // image offsets, [lo, hi)
};

// The executable subrange: the hull of the instructions reachable by the emitter's own traversal rules from the proven entries.
Hull closure_hull(std::span<const std::uint8_t> sector, std::uint32_t base, const std::vector<std::uint32_t> &entry_offsets) {
  Hull hull;
  std::set<std::uint32_t> seen;
  std::vector<std::uint32_t> work;
  for (const auto e : entry_offsets) work.push_back(base + e);
  while (!work.empty()) {
    const std::uint32_t pc = work.back();
    work.pop_back();
    if (!seen.insert(pc).second) continue;
    if (pc < base || pc >= base + sector.size() || (pc & 1U) != 0U) continue;
    const std::uint32_t offset = pc - base;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(sector, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) continue;
    const auto op = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && op.kind == M68kIrKind::write_moveq) continue;
    const std::uint32_t length = op.provenance.length.value;
    if (!hull.ok) { hull.ok = true; hull.lo = offset; hull.hi = offset + length; }
    hull.lo = std::min(hull.lo, offset);
    hull.hi = std::max(hull.hi, offset + length);
    const auto control = m68k_control_successors(op);
    for (const auto &s : control.successors) work.push_back(s.target);
    if (control.stacked == M68kStackedContinuationKind::call_continuation || control.stacked == M68kStackedContinuationKind::exception_continuation)
      work.push_back(control.stacked_address);
  }
  return hull;
}

std::string hex32(std::uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

}  // namespace

LoaderScan scan_sega_cd_loaders(const DiscImage &disc, const BootImages &images, const LoaderScanOptions &options) {
  // The asynchronous entries (interrupt, user) are walked as roots of their own; whatever they write is unknown to the init and main entries. The walk is repeated until the
  // set the synchronous entries assumed covers what the asynchronous ones were found to write (monotone, at most four passes; otherwise the result is incomplete).
  // Convergence is a fallback: the exact walk runs first (so every earlier result is reproduced) and only an exhausted walk is repeated with joins.
  LoaderScan scan;
  for (int attempt = options.converge_always ? 1 : 0; attempt < 2; ++attempt) {
    LoaderScanOptions attempt_options = options;
    attempt_options.state_convergence = attempt == 1;
    Volatile assumed;
    for (unsigned pass = 0;; ++pass) {
      Walker walker(disc, images.sub, attempt_options, assumed);
      scan = walker.run();
      if (!scan.complete || assumed.covers(walker.async_writes())) break;
      if (pass >= 3U) { scan.refusals.push_back({LoaderRefusalKind::scan_limit_exceeded, 0U}); scan.complete = false; break; }
      assumed.merge(walker.async_writes());
    }
    scan.survey.convergence_used = attempt == 1;
    if (scan.complete || !options.state_convergence) break;
  }
  if (!scan.complete) return scan;  // an exhausted walk proposes nothing: the inventory is not labelled complete

  // Group the proven loads. A run is a maximal chain of sector loads that are consecutive on the disc AND in memory ((lba, destination) -> (lba + 1, destination + 2,048));
  // the run is one image, whatever loop loaded it. A module is one run with its proven entry set; several destinations of one group are several mappings.
  struct Group {
    std::uint32_t lba{};
    std::uint32_t sectors{};
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> bases;
    std::vector<std::uint32_t> ops;
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::set<std::uint32_t>, std::vector<std::uint32_t>>> merged;   // (lba, destination) -> entries, operation indices
  std::vector<Group> groups;
  if (!options.work_state) {   // the M1/M2 grouping, unchanged: one module per (sector, proven entry set)
    for (std::uint32_t i = 0; i < scan.operations.size(); ++i) {
      const auto &op = scan.operations[i];
      if (op.entries.empty()) { scan.refusals.push_back({LoaderRefusalKind::loaded_not_executed, op.trigger_pc}); continue; }
      std::vector<std::uint32_t> offsets;
      for (const auto e : op.entries) offsets.push_back(e - op.destination);
      std::sort(offsets.begin(), offsets.end());
      auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.lba == op.lba && g.entry_offsets == offsets; });
      if (it == groups.end()) { groups.push_back(Group{op.lba, 1U, offsets, {}, {}}); it = groups.end() - 1; }
      if (std::find(it->bases.begin(), it->bases.end(), op.destination) == it->bases.end()) it->bases.push_back(op.destination);
      it->ops.push_back(i);
    }
  } else {
  for (std::uint32_t i = 0; i < scan.operations.size(); ++i) {
    const auto &op = scan.operations[i];
    auto &slot = merged[{op.lba, op.destination}];
    slot.first.insert(op.entries.begin(), op.entries.end());
    slot.second.push_back(i);
  }
  for (const auto &[key, slot] : merged) {
    if (key.first >= 1U && key.second >= kLoaderScanSectorBytes && merged.count({key.first - 1U, key.second - kLoaderScanSectorBytes}) != 0U) continue;   // a continuation of an earlier sector
    std::uint32_t n = 1;
    std::set<std::uint32_t> entries = slot.first;
    while (n < kModuleMaxBytes / kLoaderScanSectorBytes) {   // a run is one module image: at most the module list's own size bound
      const auto next = merged.find({key.first + n, key.second + n * kLoaderScanSectorBytes});
      if (next == merged.end()) break;
      entries.insert(next->second.first.begin(), next->second.first.end());
      ++n;
    }
    for (auto e = entries.begin(); e != entries.end();) {   // an entry must be an even address inside the image
      if (((*e) & 1U) != 0U || *e < key.second || *e - key.second >= n * kLoaderScanSectorBytes) { scan.refusals.push_back({LoaderRefusalKind::entry_not_code, *e}); e = entries.erase(e); }
      else ++e;
    }
    if (entries.empty()) {
      scan.refusals.push_back({LoaderRefusalKind::loaded_not_executed, scan.operations[slot.second.front()].trigger_pc});
      continue;
    }
    std::vector<std::uint32_t> offsets;
    for (const auto e : entries) offsets.push_back(e - key.second);
    std::sort(offsets.begin(), offsets.end());
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.lba == key.first && g.sectors == n && g.entry_offsets == offsets; });
    if (it == groups.end()) {
      groups.push_back(Group{key.first, n, offsets, {}, {}});
      it = groups.end() - 1;
    }
    if (std::find(it->bases.begin(), it->bases.end(), key.second) == it->bases.end()) it->bases.push_back(key.second);
    it->ops.push_back(slot.second.front());
  }
  }
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return scan;
  const auto &track = disc.tracks().front();
  std::map<std::uint32_t, unsigned> per_lba;
  for (auto &g : groups) {
    if (scan.modules.size() >= kLoaderScanMaxModules) {
      scan.refusals.push_back({LoaderRefusalKind::too_many_modules, 0U});
      scan.complete = false;
      scan.modules.clear();  // an exhausted inventory proposes nothing
      return scan;
    }
    std::vector<std::uint8_t> sector(static_cast<std::size_t>(g.sectors) * kLoaderScanSectorBytes);
    bool readable = g.lba >= kIso9660FirstDescriptor && static_cast<std::uint64_t>(g.lba) + g.sectors <= track.sector_count;
    for (std::uint32_t k = 0; readable && k < g.sectors; ++k)
      readable = disc.read_user_data(track.first_lba + g.lba + k, std::span<std::uint8_t, kDiscUserBytes>(sector.data() + static_cast<std::size_t>(k) * kLoaderScanSectorBytes, kDiscUserBytes)) == DiscError::none;
    if (!readable) {
      scan.refusals.push_back({LoaderRefusalKind::source_outside_track, scan.operations[g.ops.front()].trigger_pc});
      continue;
    }
    std::sort(g.bases.begin(), g.bases.end());
    // The hull at each base (an absolute transfer inside the module follows the base it was linked for); the union is the subrange.
    Hull hull;
    for (const auto base : g.bases) {
      const auto h = closure_hull(sector, base, g.entry_offsets);
      if (!h.ok) continue;
      if (!hull.ok) hull = h;
      hull.lo = std::min(hull.lo, h.lo);
      hull.hi = std::max(hull.hi, h.hi);
    }
    if (!hull.ok) {
      scan.refusals.push_back({LoaderRefusalKind::entry_not_code, scan.operations[g.ops.front()].trigger_pc});
      continue;
    }
    DiscoveredModule m;
    const unsigned k = per_lba[g.lba]++;
    m.name = "lba" + std::to_string(g.lba) + (k == 0U ? "" : "_" + std::to_string(k));
    m.block = g.lba;
    m.size = g.sectors * kLoaderScanSectorBytes;
    m.sha256 = sha256_hex(sector);
    m.bases = g.bases;
    m.entries = g.entry_offsets;
    m.range_offset = hull.lo & ~1U;
    m.range_length = ((hull.hi - m.range_offset) + 1U) & ~1U;
    scan.modules.push_back(std::move(m));
  }
  std::sort(scan.modules.begin(), scan.modules.end(), [](const DiscoveredModule &a, const DiscoveredModule &b) { return a.name < b.name; });
  std::sort(scan.refusals.begin(), scan.refusals.end(), [](const LoaderRefusal &a, const LoaderRefusal &b) {
    return std::tie(a.kind, a.pc) < std::tie(b.kind, b.pc);
  });
  if (!scan.modules.empty() && scan.modules.size() <= kModuleMaxCount) {   // the module-list text is only valid within the list's own bound
    std::ostringstream text;
    text << kModuleManifestVersion << "\n# discovered by the static loader scan (SEG-097-T001, ADR 0206); admitted unchanged by the module list\n"
         << "disc " << disc.identity() << "\n";
    for (const auto &m : scan.modules) {
      text << "module " << m.name << "\nprocessor sub\nextent " << m.block << " " << m.size << "\nsha256 " << m.sha256 << "\n";
      for (const auto base : m.bases) text << "map " << hex32(base) << "\n";
      text << "range " << hex32(m.range_offset) << " " << hex32(m.range_length) << "\n";
      for (const auto entry : m.entries) text << "entry " << hex32(entry) << "\n";
      text << "verify byte_identity\nend\n";
    }
    scan.manifest = text.str();
    scan.identity = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(scan.manifest.data()), scan.manifest.size()));
  }
  return scan;
}

}  // namespace segarecomp::machine::sega_cd
