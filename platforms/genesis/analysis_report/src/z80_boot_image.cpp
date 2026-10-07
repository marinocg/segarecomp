// SEG-040-T004 (ADR 0072 section 4; report-only). See z80_boot_image.hpp.

#include "segarecomp/genesis_analysis_report/z80_boot_image.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace segarecomp {
namespace {

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

// The Z80 control block (BUSREQ $A11100, RESET $A11200, with any partial-decode mirror; report.hpp's own
// `genesis_z80_control_first/last`, duplicated here exactly like z80_ram_write_proof.hpp already duplicates the geometry
// constants it needs rather than depending on the top-level report driver).
constexpr std::uint32_t z80_control_first = UINT32_C(0x00A11000);
constexpr std::uint32_t z80_control_last = UINT32_C(0x00A12000);

// Documented Genesis BUSREQ/RESET control register addresses (ADR 0072 section 4; genesis-z80-audio-contract.md section 4,
// frozen against Genesis Plus GX and ares). Only D8 of the word / D0 of the byte is architectural.
constexpr std::uint32_t busreq_register = UINT32_C(0x00A11100);
constexpr std::uint32_t reset_register = UINT32_C(0x00A11200);

// The Z80 RAM mirror the 68K sees directly ($A00000-$A03FFF, `address & $1FFF`; genesis-z80-audio-contract.md section 3). The
// broader Z80 bus area (genesis_z80_area_first..last, z80_ram_write_proof.hpp) also covers the YM2612 mirror, the bank register
// and the undefined upper half, none of which hold Z80 RAM content.
constexpr std::uint32_t z80_ram_mirror_first = UINT32_C(0x00A00000);
constexpr std::uint32_t z80_ram_mirror_last = UINT32_C(0x00A04000);
constexpr std::uint32_t z80_ram_mirror_mask = UINT32_C(0x1FFF);  // 8 KiB (genesis_z80_ram_write_proof.cpp ram_size)

// `/RESET` request bit extraction (byte/word): the same bit lane `genesis_z80_bus_write`/`genesis_z80_bus_shape_ok`
// (platforms/genesis/runtime/runtime.c) use -- D8 of a word access, D0 of a byte access.
bool control_bit(std::uint32_t span, std::uint64_t value) { return span == 2U ? ((value >> 8U) & 1U) != 0U : (value & 1U) != 0U; }

// True when a store of `span` bytes at `target` may touch [lo, hi): an Unknown target may be anywhere (the same conservative
// rule `may_release`/`observe_store` in finite_adapter.cpp already apply to this exact bus-geometry question).
bool touches_range(const M68kPointsTo &target, std::uint32_t span, std::uint32_t lo, std::uint32_t hi) {
  if (!target.is_known()) return true;
  for (const auto &[region, offsets] : target.pairs) {
    const std::uint64_t a = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.lo();
    const std::uint64_t b = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.hi() + span;
    if (a < hi && lo < b) return true;
  }
  return false;
}

// Clips a known store of `span` bytes at `target` to [lo, hi), appending every touched sub-range to `out`; false when `target`
// is Unknown (the caller counts it separately) or touches nothing.
bool clip_range(const M68kPointsTo &target, std::uint32_t span, std::uint32_t lo, std::uint32_t hi,
                std::vector<std::pair<std::uint32_t, std::uint32_t>> &out) {
  if (!target.is_known()) return false;
  bool touched = false;
  for (const auto &[region, offsets] : target.pairs) {
    const std::uint64_t a = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.lo();
    const std::uint64_t b = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.hi() + span;
    if (!(a < hi && lo < b)) continue;
    touched = true;
    out.emplace_back(static_cast<std::uint32_t>(std::max<std::uint64_t>(a, lo)), static_cast<std::uint32_t>(std::min<std::uint64_t>(b, hi)));
  }
  return touched;
}

// The exact single bus address `target` names, or nullopt (Unknown, more than one address, or a non-exact/strided set).
std::optional<std::uint32_t> exact_single_address(const M68kPointsTo &target) {
  if (!target.is_known() || !target.is_exact()) return std::nullopt;
  const auto values = target.values();
  if (!values || values->size() != 1U) return std::nullopt;
  return values->front() & bus_mask;
}

// The exact single numeric content of a resolved, non-pointer write value, or nullopt.
std::optional<std::uint64_t> exact_single_value(const std::optional<M68kCellValue> &value) {
  if (!value || value->is_pointer() || !value->data.is_precise() || value->data.values().size() != 1U) return std::nullopt;
  return value->data.values().front();
}

// SEG-040-T004: the proven BUSREQ/RESET fact at one program point (the Genesis "pristine" invariant, contract sections 4-5):
// `busreq`/`reset_released` are the exact 68K-written state of the two control-register latch bits when every path to this
// point agrees (nullopt: the paths disagree, or a write's value could not be read exactly); `pristine` is true only when the
// Z80 was never provably runnable on every such path. Both bits start Unknown and `pristine` starts false at every root except
// the one genuine Genesis power-on reset entry (contract section 4.1).
struct BootFact {
  std::optional<bool> busreq;
  std::optional<bool> reset_released;
  bool pristine{};
  friend bool operator==(const BootFact &, const BootFact &) = default;
};

BootFact power_on_fact() { return {false, false, true}; }  // contract 4.1: BUSREQ clear, /RESET asserted, never yet run
BootFact unknown_entry_fact() { return {std::nullopt, std::nullopt, false}; }

BootFact join(const BootFact &left, const BootFact &right) {
  BootFact out;
  out.busreq = (left.busreq && right.busreq && *left.busreq == *right.busreq) ? left.busreq : std::nullopt;
  out.reset_released =
      (left.reset_released && right.reset_released && *left.reset_released == *right.reset_released) ? left.reset_released : std::nullopt;
  out.pristine = left.pristine && right.pristine;
  return out;
}

}  // namespace

GenesisZ80BootDerivation derive_genesis_z80_boot_image(const M68kAnalysisImage &image, const M68kAnalysisConfig &config,
                                                        const M68kFiniteAnalysisResult &analysis,
                                                        std::optional<std::uint32_t> reset_entry_pc,
                                                        const std::vector<std::uint32_t> &roots) {
  GenesisZ80BootDerivation result;
  if (!analysis.complete) return result;
  M68kFiniteAdapter adapter{image, config};
  const auto &states = analysis.solution.in_states;

  std::map<std::uint64_t, BootFact> facts;
  std::set<std::uint64_t> worklist;
  const auto arrive = [&](std::uint64_t point, const BootFact &fact) {
    if (!states.contains(point) || m68k_point_tag(point) != 0U) return;  // tag 0 (main flow) only: SEG-040-T004 scope
    const auto [found, inserted] = facts.try_emplace(point, fact);
    if (!inserted) {
      auto joined = join(found->second, fact);
      if (joined == found->second) return;
      found->second = joined;
    }
    worklist.insert(point);
  };
  for (const auto root : roots) arrive(root, reset_entry_pc && root == *reset_entry_pc ? power_on_fact() : unknown_entry_fact());

  // Z80-area store accumulation, split by the proven boot window.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reset_ranges, running_ranges;
  std::size_t reset_unknown = 0U, running_unknown = 0U;
  std::map<std::uint32_t, std::uint8_t> image_bytes;
  bool image_exact = true;

  std::size_t iterations = 0U;
  bool exhausted = false;
  while (!worklist.empty()) {
    if (++iterations > genesis_z80_boot_window_iteration_bound) {
      exhausted = true;
      break;
    }
    const auto point = *worklist.begin();
    worklist.erase(worklist.begin());
    const auto &in = facts.at(point);
    const auto &state = states.at(point);
    auto out = in;
    const auto pc = m68k_point_pc(point);
    const auto decoded = adapter.decode(pc);
    if (decoded) {
      const auto &operation = decoded->operation;
      const auto next = pc + decoded->length;
      const auto status = adapter.effective_status(0U, state);
      const auto targets = adapter.memory_write_targets(operation, state, status);
      const auto values = adapter.memory_write_values(0U, operation, next, state, status);
      const bool boot_window_now = in.pristine && in.busreq.has_value() && *in.busreq && in.reset_released.has_value() && *in.reset_released;
      for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto &[target, span] = targets[i];
        const std::optional<M68kCellValue> value = i < values.size() ? values[i] : std::nullopt;
        // The two control registers: an exact single-address, byte/word write updates exactly its own latch bit; anything else
        // that may touch the control block (an Unknown target, a different in-region address, or a LONG/odd-width access the
        // documented shape never grants) collapses both bits to Unknown -- never assumed unchanged.
        if (touches_range(target, span, z80_control_first, z80_control_last)) {
          const auto address = exact_single_address(target);
          if (address && (span == 1U || span == 2U) && (*address == busreq_register || *address == reset_register)) {
            const auto numeric = exact_single_value(value);
            auto &bit = *address == busreq_register ? out.busreq : out.reset_released;
            bit = numeric ? std::optional<bool>(control_bit(span, *numeric)) : std::nullopt;
          } else {
            out.busreq.reset();
            out.reset_released.reset();
          }
        }
        // The Z80 bus area: classify into the proven boot-window group or the (today's pre-existing) running group, and --
        // boot-window writes into the Z80 RAM mirror only -- attempt the exact image byte.
        if (touches_range(target, span, genesis_z80_area_first, genesis_z80_area_last)) {
          auto &ranges = boot_window_now ? reset_ranges : running_ranges;
          auto &unknown = boot_window_now ? reset_unknown : running_unknown;
          if (!clip_range(target, span, genesis_z80_area_first, genesis_z80_area_last, ranges)) ++unknown;
          if (boot_window_now && touches_range(target, span, z80_ram_mirror_first, z80_ram_mirror_last)) {
            const auto address = exact_single_address(target);
            const auto numeric = exact_single_value(value);
            if (span == 1U && address && numeric) {
              const auto offset = *address & z80_ram_mirror_mask;
              const auto byte = static_cast<std::uint8_t>(*numeric);
              const auto [found, inserted] = image_bytes.try_emplace(offset, byte);
              if (!inserted && found->second != byte) image_exact = false;  // two different exact values at the same offset
            } else {
              image_exact = false;  // imprecise target/value, or a WORD/LONG store (the "high byte only" quirk is not modelled)
            }
          }
        }
      }
      // Pristine is lost for good once the Z80 is provably (or possibly) runnable: provably not-runnable requires BUSREQ known
      // asserted OR /RESET known still held; any other combination (including either bit now Unknown) fails closed.
      const bool provably_not_runnable =
          (out.busreq.has_value() && *out.busreq) || (out.reset_released.has_value() && !*out.reset_released);
      out.pristine = in.pristine && provably_not_runnable;
    }
    for (const auto &edge : adapter.transfer(point, state).edges) arrive(edge.target, out);
  }

  if (exhausted) return GenesisZ80BootDerivation{};  // fail closed: the caller keeps its own pre-T004 behaviour entirely

  if (!reset_ranges.empty()) result.area_stores.push_back(GenesisZ80AreaStores{reset_ranges, reset_unknown, true});
  else if (reset_unknown != 0U) result.area_stores.push_back(GenesisZ80AreaStores{{}, reset_unknown, true});
  result.area_stores.push_back(GenesisZ80AreaStores{running_ranges, running_unknown, false});

  if (image_exact && !image_bytes.empty()) {
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t offset = 0U;; ++offset) {
      const auto found = image_bytes.find(offset);
      if (found == image_bytes.end()) break;
      bytes.push_back(found->second);
    }
    if (!bytes.empty()) result.image = GenesisZ80Image{std::move(bytes), "m68k_boot_upload_derived"};
  }
  return result;
}

}  // namespace segarecomp
