// ADR 0277: see z80_static_producer.hpp.
#include "segarecomp/machine/genesis/z80_static_producer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "segarecomp/core/kosinski.hpp"
#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::genesis::z80 {

const char* static_outcome_name(StaticOutcome outcome) noexcept {
  switch (outcome) {
    case StaticOutcome::complete: return "complete";
    case StaticOutcome::partial: return "partial";
    case StaticOutcome::none: return "none";
    case StaticOutcome::unsupported: return "unsupported";
    case StaticOutcome::resource_bound: return "resource_bound";
  }
  return "unsupported";
}

const char* static_refusal_name(StaticRefusal refusal) noexcept {
  switch (refusal) {
    case StaticRefusal::entry_unreadable: return "entry_unreadable";
    case StaticRefusal::bus_write_value_unknown: return "bus_write_value_unknown";
    case StaticRefusal::z80_store_value_unknown: return "z80_store_value_unknown";
    case StaticRefusal::z80_store_unknown_address: return "z80_store_unknown_address";
    case StaticRefusal::z80_store_form_unsupported: return "z80_store_form_unsupported";
    case StaticRefusal::length_unknown: return "length_unknown";
    case StaticRefusal::source_not_immutable: return "source_not_immutable";
    case StaticRefusal::destination_out_of_ram: return "destination_out_of_ram";
    case StaticRefusal::loop_unproven: return "loop_unproven";
    case StaticRefusal::decompression_unproven: return "decompression_unproven";
    case StaticRefusal::decode_failed: return "decode_failed";
    case StaticRefusal::window_control_unknown: return "window_control_unknown";
    case StaticRefusal::ambiguous_epoch_order: return "ambiguous_epoch_order";
    case StaticRefusal::image_bound: return "image_bound";
    case StaticRefusal::call_depth: return "call_depth";
    case StaticRefusal::state_bound: return "state_bound";
    case StaticRefusal::step_bound: return "step_bound";
  }
  return "unknown";
}

bool static_refusal_affects_epochs(StaticRefusal refusal) noexcept {
  switch (refusal) {
    case StaticRefusal::call_depth:
    case StaticRefusal::state_bound:
    case StaticRefusal::step_bound:
    case StaticRefusal::entry_unreadable:
      return false;  // resource / entry classes: ruled by the outcome rule, not by a refused Z80 construct
    default:
      return true;
  }
}

const char* static_proof_name(StaticProof proof) noexcept {
  switch (proof) {
    case StaticProof::rom_stores: return "rom_stores";
    case StaticProof::rom_copy: return "rom_copy";
    case StaticProof::kosinski: return "kosinski";
  }
  return "rom_stores";
}

namespace {

using Width = M68kMemoryAccessWidth;

constexpr std::uint32_t kZ80Ram = 0x00A00000U;
constexpr std::uint32_t kZ80RamEnd = 0x00A02000U;      // the RAM proper (an upload may not run into the mirror)
constexpr std::uint32_t kZ80Window = 0x00A04000U;      // RAM + its mirror
constexpr std::uint32_t kBusReq = 0x00A11100U;
constexpr std::uint32_t kReset = 0x00A11200U;
constexpr std::uint32_t kBusRegionBegin = 0x00A11000U;
constexpr std::uint32_t kBusRegionEnd = 0x00A12000U;
constexpr std::uint32_t kIoBegin = 0x00A10000U;
constexpr std::uint32_t kIoEnd = 0x00A10020U;
constexpr std::uint32_t kRomEnd = 0x00400000U;
constexpr std::uint32_t kWorkRam = 0x00FF0000U;
constexpr std::uint32_t kWorkRamEnd = 0x01000000U;
constexpr std::size_t kMaxStageBytes = 0x4000;  // one staged (decoded) work-RAM buffer, see Context::stage
constexpr std::size_t kMaxRoutineBytes = 384;  // the decoder routine scan: first RTS within this many bytes
constexpr std::size_t kMaxDecompressedBytes = kRamBytes;
constexpr std::uint16_t kMaxLooseStores = 48;  // see Flow store()

// ---- recognised Kosinski routine forms -------------------------------------------------------------------------------------
// SHA-256 digests of the routine's code bytes (position independent: short branches only, no absolute operand). A form is a routine
// whose behaviour on (A0 = stream, A1 = destination) is the public Kosinski decode, producing the bytes at (A1) and returning by RTS;
// it clobbers D0-D6 and A2 (everything is treated as clobbered by the walk). Form 1 is the project-authored reference routine
// (tests/fixtures; docs/architecture/genesis-z80-static-first.md); form 2 is the routine form that qualified titles share (identified
// by the digest only - no code is stored in this repository).
struct RoutineForm {
  const char* name;
  const char* digest_hex;
};
constexpr RoutineForm kKosinskiForms[] = {
    {"reference", "3384a2ab77f8025f288c72999a117da9ec2e53a561e2f8d974c182195a8062df"},
    {"library", "e8028514cfa2b24f49cd07ee523af573b7cb404b62cf45ff9484a69090b26f90"},
};

// ---- machine state -----------------------------------------------------------------------------------------------------
struct Reg {
  std::uint32_t v = 0;
  std::uint32_t mask = 0;
  [[nodiscard]] bool full() const noexcept { return mask == 0xFFFFFFFFU; }
  [[nodiscard]] bool has(std::uint32_t bits) const noexcept { return (mask & bits) == bits; }
  bool operator==(const Reg&) const = default;
};
Reg K(std::uint32_t v) { return Reg{v, 0xFFFFFFFFU}; }
Reg meet_reg(const Reg& x, const Reg& y) {
  const std::uint32_t m = x.mask & y.mask & ~(x.v ^ y.v);
  return Reg{x.v & m, m};
}

struct Flags {
  std::int8_t z = -1, n = -1, c = -1, v = -1;  // -1 unknown
  bool operator==(const Flags&) const = default;
};

struct Regs {
  std::array<Reg, 8> d{}, a{};
  Flags f{};
  bool operator==(const Regs&) const = default;
};

Regs meet(const Regs& x, const Regs& y) {
  Regs out;
  for (std::size_t i = 0; i < 8; ++i) {
    out.d[i] = meet_reg(x.d[i], y.d[i]);
    out.a[i] = meet_reg(x.a[i], y.a[i]);
  }
  out.f.z = x.f.z == y.f.z ? x.f.z : static_cast<std::int8_t>(-1);
  out.f.n = x.f.n == y.f.n ? x.f.n : static_cast<std::int8_t>(-1);
  out.f.c = x.f.c == y.f.c ? x.f.c : static_cast<std::int8_t>(-1);
  out.f.v = x.f.v == y.f.v ? x.f.v : static_cast<std::int8_t>(-1);
  return out;
}

// The Z80-side context: everything that must be EQUAL for two walk states to merge (registers are met, this is not).
struct Context {
  std::vector<std::uint8_t> blob;          // kRamBytes of RAM, then kBitmapBytes of the hold-window bitmap
  std::uint8_t busreq = 0, released = 0, executed = 0;  // the runtime's latches; power-on: /RESET asserted, no request, pristine
  std::uint8_t io_written = 0;             // an I/O-area store was seen: control-register power-on values no longer hold
  std::uint8_t proof = 0;                  // StaticProof of the current window
  std::uint16_t loose_stores = 0;          // non-idiom, non-constant stores applied in the current window (bounded)
  // A STAGED buffer: the output of a recognised decompressor call whose destination is constant work RAM. It is a function of immutable ROM bytes
  // only, so a later counted copy out of exactly that range is a proven source - as long as nothing may have written the range since: any store
  // through an unknown pointer or into the range, and any call of an unrecognised routine, discards it (Walker::touch).
  std::uint32_t stage_base = 0;
  std::vector<std::uint8_t> stage;
  std::vector<std::uint32_t> window_stores;  // pcs of non-constant stores in the current window (loop check)
  std::vector<std::uint32_t> epochs;       // epoch ids, activation order
  std::vector<std::uint32_t> calls;        // shadow return stack
  auto key() const { return std::tie(blob, busreq, released, executed, io_written, proof, loose_stores, stage_base, stage, window_stores, epochs, calls); }
  bool operator<(const Context& o) const { return key() < o.key(); }
};

Context power_on() {
  Context c;
  c.blob.assign(kRamBytes + kBitmapBytes, 0);
  return c;
}

[[nodiscard]] bool granted(const Context& c) { return c.busreq == 1U && c.released == 1U; }

struct Decoded {
  M68kIrOperation op;
  M68kControlSuccessors control;
  std::uint32_t length = 0;
};

struct CandidateEpoch {
  Epoch epoch;
  StaticProof proof = StaticProof::rom_stores;
  std::vector<std::uint32_t> store_pcs;
};

class Walker {
 public:
  Walker(std::span<const std::uint8_t> rom, StaticProduction& out) : rom_(rom), out_(out) {}

  void run() {
    const auto entry = rom_long(4);
    if (!entry || *entry >= rom_.size() || (*entry & 1U) != 0U) {
      ++out_.refusals[StaticRefusal::entry_unreadable];
      out_.outcome = StaticOutcome::unsupported;
      return;
    }
    Regs regs;
    seed(*entry, intern(power_on()), regs);
    bool bounded = false;
    while (!work_.empty()) {
      if (steps_ >= kStaticMaxSteps) { ++out_.refusals[StaticRefusal::step_bound]; bounded = true; break; }
      if (nodes_.size() > kStaticMaxNodes) { ++out_.refusals[StaticRefusal::state_bound]; bounded = true; break; }
      WorkItem item = std::move(work_.front());
      work_.pop_front();
      ++steps_;
      step(item.pc, item.ctx, item.regs);
      if (contexts_.size() > kStaticMaxStates) { ++out_.refusals[StaticRefusal::state_bound]; bounded = true; break; }
    }
    out_.steps = steps_;
    out_.nodes = static_cast<std::uint32_t>(nodes_.size());
    out_.contexts = static_cast<std::uint32_t>(contexts_.size());
    finish(bounded);
  }

 private:
  // ---- cartridge image ----------------------------------------------------------------------------------------------
  std::optional<std::uint32_t> rom_value(std::uint32_t address, Width w) const {
    const std::uint32_t n = w == Width::byte ? 1U : w == Width::word ? 2U : 4U;
    if (address >= kRomEnd || static_cast<std::uint64_t>(address) + n > rom_.size()) return std::nullopt;
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) v = (v << 8U) | rom_[address + i];
    return v;
  }
  std::optional<std::uint32_t> rom_long(std::uint32_t address) const { return rom_value(address, Width::long_word); }

  std::optional<Decoded*> decode(std::uint32_t pc) {
    const auto hit = decoded_.find(pc);
    if (hit != decoded_.end()) return hit->second ? std::optional<Decoded*>(&*hit->second) : std::nullopt;
    auto& slot = decoded_[pc];
    if ((pc & 1U) != 0U || pc >= rom_.size() || pc >= kRomEnd) return std::nullopt;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{0}};
    const auto result = decode_m68k_instruction(rom_.subspan(pc), source, M68kDecodeProfile::general_startup);
    const auto* decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return std::nullopt;
    Decoded entry;
    entry.op = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && entry.op.kind == M68kIrKind::write_moveq) return std::nullopt;
    entry.control = m68k_control_successors(entry.op);
    entry.length = static_cast<std::uint32_t>(entry.op.raw_bytes.size());
    slot = std::move(entry);
    return &*slot;
  }

  // ---- node bookkeeping -----------------------------------------------------------------------------------------------
  std::uint32_t intern(const Context& c) {
    const auto it = context_ids_.find(c);
    if (it != context_ids_.end()) return it->second;
    const auto id = static_cast<std::uint32_t>(contexts_.size());
    contexts_.push_back(c);
    context_ids_.emplace(c, id);
    return id;
  }

  // A node is (pc, context). Register states are explored EXACTLY (a counted loop with constant operands unrolls concretely) until a node has
  // seen kExactVisits distinct states; after that the node widens: further states are met into one, losing the constants that differ.
  static constexpr std::uint32_t kExactVisits = 96;

  static std::string regs_key(const Regs& r) {
    std::string key;
    key.reserve(16 * 8 + 4);
    const auto put = [&](std::uint32_t v) { key.append(reinterpret_cast<const char*>(&v), sizeof v); };
    for (const Reg& x : r.d) { put(x.v & x.mask); put(x.mask); }
    for (const Reg& x : r.a) { put(x.v & x.mask); put(x.mask); }
    key.push_back(static_cast<char>(r.f.z)); key.push_back(static_cast<char>(r.f.n)); key.push_back(static_cast<char>(r.f.c)); key.push_back(static_cast<char>(r.f.v));
    return key;
  }

  void seed(std::uint32_t pc, std::uint32_t ctx, const Regs& regs) {
    NodeInfo& info = nodes_[{pc, ctx}];
    if (!info.widened) {
      if (!info.seen.insert(regs_key(regs)).second) return;
      info.accum = info.exact == 0 ? regs : meet(info.accum, regs);
      if (++info.exact > kExactVisits) {
        info.widened = true;
        info.seen.clear();
        work_.push_back({pc, ctx, info.accum});
        return;
      }
      work_.push_back({pc, ctx, regs});
      return;
    }
    const Regs joined = meet(info.accum, regs);
    if (joined == info.accum) return;
    info.accum = joined;
    work_.push_back({pc, ctx, joined});
  }

  // ---- operands ---------------------------------------------------------------------------------------------------------
  static std::uint32_t nbytes(Width w) { return w == Width::byte ? 1U : w == Width::word ? 2U : 4U; }
  static std::uint32_t wmask(Width w) { return w == Width::byte ? 0xFFU : w == Width::word ? 0xFFFFU : 0xFFFFFFFFU; }
  static std::uint32_t sext(std::uint32_t v, Width w) {
    if (w == Width::byte) return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(v)));
    if (w == Width::word) return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v)));
    return v;
  }
  static bool is_memory(const M68kEffectiveAddress& ea) {
    return ea.mode != M68kEaMode::unused && ea.mode != M68kEaMode::data_register && ea.mode != M68kEaMode::address_register &&
           ea.mode != M68kEaMode::immediate;
  }
  static std::uint32_t step_size(const M68kEffectiveAddress& ea, Width w) { return (ea.reg & 7U) == 7U && w == Width::byte ? 2U : nbytes(w); }

  // The address a memory operand denotes (24-bit); applies the auto-increment/decrement to a known register.
  std::optional<std::uint32_t> address_of(Regs& r, const M68kEffectiveAddress& ea, Width w) const {
    const unsigned reg = ea.reg & 7U;
    switch (ea.mode) {
      case M68kEaMode::absolute_word:
      case M68kEaMode::absolute_long: return m68k_data_access_ea_address(ea) & 0x00FFFFFFU;
      case M68kEaMode::pc_disp16: return ea.absolute_address & 0x00FFFFFFU;
      case M68kEaMode::address_indirect: return r.a[reg].full() ? std::optional<std::uint32_t>(r.a[reg].v & 0x00FFFFFFU) : std::nullopt;
      case M68kEaMode::address_postinc: {
        std::optional<std::uint32_t> at;
        if (r.a[reg].full()) {
          at = r.a[reg].v & 0x00FFFFFFU;
          r.a[reg] = K(r.a[reg].v + step_size(ea, w));
        }
        return at;
      }
      case M68kEaMode::address_predec: {
        std::optional<std::uint32_t> at;
        if (r.a[reg].full()) {
          r.a[reg] = K(r.a[reg].v - step_size(ea, w));
          at = r.a[reg].v & 0x00FFFFFFU;
        }
        return at;
      }
      case M68kEaMode::address_disp16:
        return r.a[reg].full() ? std::optional<std::uint32_t>((r.a[reg].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement))) & 0x00FFFFFFU)
                               : std::nullopt;
      default: return std::nullopt;
    }
  }

  // A value read. Applies address side effects. ROM is immutable; the BUSACK latch and the power-on control registers are modelled;
  // everything else (work RAM, VDP, controllers, Z80 RAM which the Z80 may have changed) is unknown.
  std::optional<std::uint32_t> read_value(Regs& r, const Context& c, const M68kEffectiveAddress& ea, Width w) const {
    switch (ea.mode) {
      case M68kEaMode::immediate: return ea.immediate_value & wmask(w);
      case M68kEaMode::data_register: return r.d[ea.reg & 7U].has(wmask(w)) ? std::optional<std::uint32_t>(r.d[ea.reg & 7U].v & wmask(w)) : std::nullopt;
      case M68kEaMode::address_register: return r.a[ea.reg & 7U].has(wmask(w)) ? std::optional<std::uint32_t>(r.a[ea.reg & 7U].v & wmask(w)) : std::nullopt;
      default: break;
    }
    if (!is_memory(ea)) return std::nullopt;
    const auto at = address_of(r, ea, w);
    if (!at) return std::nullopt;
    if (*at < kRomEnd) return rom_value(*at, w);
    if (*at == kBusReq && (w == Width::word || w == Width::byte)) {  // BUSACK: bit clear iff granted (the runtime's read rule)
      if (c.busreq > 1U || c.released > 1U) return std::nullopt;
      return granted(c) ? 0U : (w == Width::word ? 0x0100U : 0x0001U);
    }
    if (c.io_written == 0U && ((*at == 0x00A10008U && w == Width::long_word) || (*at == 0x00A1000CU && w == Width::word))) return 0U;  // power-on control registers
    return std::nullopt;
  }

  void clobber(Regs& r, const M68kIrOperation& op) {
    const auto effect = m68k_operation_effect(op);
    if (!effect.register_write_footprint_complete) {
      r = Regs{};
      return;
    }
    for (unsigned i = 0; i < 8; ++i) {
      if ((effect.data_register_write_mask >> i) & 1U) r.d[i] = Reg{};
      if (i < 7 && ((effect.address_register_write_mask >> i) & 1U)) r.a[i] = Reg{};
    }
    r.a[7] = Reg{};
    r.f = Flags{};
  }

  void set_nz(Regs& r, std::uint32_t value, Width w) {
    r.f.z = (value & wmask(w)) == 0U ? 1 : 0;
    r.f.n = (value & (w == Width::byte ? 0x80U : w == Width::word ? 0x8000U : 0x80000000U)) != 0U ? 1 : 0;
    r.f.c = 0;
    r.f.v = 0;
  }

  // ---- the Z80 side: bus latches and the epoch tracker (runtime.c genesis_z80_bus_write, rule for rule) ----------------------
  void bus_write(Context& c, bool reset_register, bool set) {
    const bool was_runnable = c.released == 1U && c.busreq == 0U;
    if (!reset_register) {
      if (static_cast<std::uint8_t>(set) == c.busreq) return;  // no edge
      c.busreq = static_cast<std::uint8_t>(set);
    } else {
      if (static_cast<std::uint8_t>(set) == c.released) return;  // no edge
      c.released = static_cast<std::uint8_t>(set);
      if (set) c.executed = 0;  // /RESET release: the Z80 is pristine again
    }
    const bool runnable = c.released == 1U && c.busreq == 0U;
    const bool transition = runnable && !was_runnable;
    const bool epoch = transition && c.executed == 0U;
    if (epoch) {
      CandidateEpoch candidate;
      std::memcpy(candidate.epoch.ram.data(), c.blob.data(), kRamBytes);
      std::memcpy(candidate.epoch.written.data(), c.blob.data() + kRamBytes, kBitmapBytes);
      candidate.proof = static_cast<StaticProof>(c.proof);
      candidate.store_pcs = c.window_stores;
      c.epochs.push_back(static_cast<std::uint32_t>(candidates_.size()));
      candidates_.push_back(std::move(candidate));
      c.executed = 1;
    }
    if (transition) {  // the hold window ends at every runnable transition
      std::fill(c.blob.begin() + kRamBytes, c.blob.end(), 0);
      c.window_stores.clear();
      c.proof = 0;
      c.loose_stores = 0;
    }
  }

  static void note_ram_write(Context& c, std::uint32_t offset, std::uint8_t value) {
    offset &= static_cast<std::uint32_t>(kRamBytes - 1U);
    c.blob[offset] = value;
    c.blob[kRamBytes + (offset >> 3U)] = static_cast<std::uint8_t>(c.blob[kRamBytes + (offset >> 3U)] | (1U << (offset & 7U)));
  }

  // A write of `bytes` bytes to work RAM (or anywhere): discards the staged buffer when it may overlap it. A stack-based store with an unknown
  // address is assumed to be the 68K stack, which does not overlap a staging buffer (a wrong assumption surfaces as a different image: the
  // verification run then contradicts the prediction).
  static void touch(Context& c, std::optional<std::uint32_t> at, std::uint32_t bytes, bool stack_based) {
    if (c.stage.empty()) return;
    if (!at) {
      if (!stack_based) c.stage.clear();
      return;
    }
    if (*at < c.stage_base + c.stage.size() && static_cast<std::uint64_t>(*at) + bytes > c.stage_base) c.stage.clear();
  }

  enum class Flow : std::uint8_t { go, dead };

  // One store of `w` bytes of `value` at `address` (runtime route rules). `kind_plain`: a plain MOVE/CLR (the only recognised store);
  // `constant_form`: an immediate stored to an absolute address (idempotent in any loop); `pc`: for the loop check.
  Flow store(Context& c, std::optional<std::uint32_t> address, Width w, std::optional<std::uint32_t> value, bool plain, bool constant_form,
             std::uint32_t pc, bool stack_based) {
    touch(c, address, nbytes(w), stack_based);
    if (!address) {
      if (stack_based) return Flow::go;
      if (c.busreq == 0U) return Flow::go;  // the 68K does not own the bus: a Z80-area store would stop the runtime, never alter the window
      refuse(StaticRefusal::z80_store_unknown_address);
      return Flow::dead;
    }
    const std::uint32_t at = *address;
    if (at >= kIoBegin && at < kIoEnd) c.io_written = 1;
    if (at >= kBusRegionBegin && at < kBusRegionEnd) {
      if ((at != kBusReq && at != kReset) || (w != Width::word && w != Width::byte) || !plain) return Flow::dead;  // unsupported shape: the runtime stops typed
      if (!value) { refuse(StaticRefusal::bus_write_value_unknown); return Flow::dead; }
      bus_write(c, at == kReset, ((*value) & (w == Width::word ? 0x0100U : 0x0001U)) != 0U);
      return Flow::go;
    }
    if (at >= kZ80Ram && at < kZ80Window) {
      if (c.busreq > 1U || c.released > 1U) return Flow::dead;
      if (!granted(c)) return Flow::dead;  // 68K_Z80_AREA_WITHOUT_BUS: the runtime stops typed
      if (w == Width::long_word && at + 3U >= kZ80Window) return Flow::dead;
      if (!plain) { refuse(StaticRefusal::z80_store_form_unsupported); return Flow::dead; }
      if (!value) { refuse(StaticRefusal::z80_store_value_unknown); return Flow::dead; }
      if (w == Width::byte) {
        note_ram_write(c, at - kZ80Ram, static_cast<std::uint8_t>(*value));
      } else if (w == Width::word) {
        note_ram_write(c, at - kZ80Ram, static_cast<std::uint8_t>(*value >> 8U));  // a WORD store keeps its high byte
      } else {
        note_ram_write(c, at - kZ80Ram, static_cast<std::uint8_t>(*value >> 24U));
        note_ram_write(c, at + 2U - kZ80Ram, static_cast<std::uint8_t>(*value >> 8U));
      }
      if (!constant_form) {
        c.window_stores.push_back(pc);
        // A bounded number of individually executed non-constant stores: a longer sequence is a loop the walk would unroll store by store
        // (state explosion over an unrecognised mechanism).
        if (++c.loose_stores > kMaxLooseStores) {
          refuse(StaticRefusal::loop_unproven);
          return Flow::dead;
        }
      }
      return Flow::go;
    }
    return Flow::go;
  }

  void refuse(StaticRefusal refusal) { ++out_.refusals[refusal]; }

  // ---- the counted copy idiom -------------------------------------------------------------------------------------------
  // MOVE.B (As)+,(Ad)+ ; DBRA Dn,<back to the MOVE>: constant operands, ROM source, RAM destination.
  struct CopyIdiom {
    unsigned src = 0, dst = 0, counter = 0;
    std::uint32_t after = 0;
  };
  std::optional<CopyIdiom> copy_idiom(std::uint32_t pc, const Decoded& first) {
    const M68kIrOperation& mv = first.op;
    if (mv.kind != M68kIrKind::write_move || mv.size != Width::byte || mv.source_ea.mode != M68kEaMode::address_postinc ||
        mv.destination_ea.mode != M68kEaMode::address_postinc || (mv.source_ea.reg & 7U) == 7U || (mv.destination_ea.reg & 7U) == 7U)
      return std::nullopt;
    const std::uint32_t next = pc + first.length;
    const auto second = decode(next);
    if (!second || (*second)->op.kind != M68kIrKind::dbcc_loop || (*second)->op.condition != M68kCondition::never) return std::nullopt;
    const bool back = std::any_of((*second)->control.successors.begin(), (*second)->control.successors.end(), [&](const M68kControlSuccessor& s) {
      return s.kind == M68kControlSuccessorKind::conditional_target && (s.target & 0x00FFFFFFU) == pc;
    });
    if (!back) return std::nullopt;
    return CopyIdiom{mv.source_ea.reg & 7U, mv.destination_ea.reg & 7U, (*second)->op.destination_ea.reg & 7U, next + (*second)->length};
  }

  // Applies the idiom: `applied` (the loop's effect is in the state), `generic` (not a Z80 copy: walk the instructions), `dead` (the path ends;
  // the refusal is counted where the construct is Z80-affecting).
  enum class Copy : std::uint8_t { applied, generic, dead };
  Copy apply_copy(Context& c, Regs& r, const CopyIdiom& idiom) {
    if (!r.a[idiom.dst].full()) return Copy::generic;  // an unknown destination: the generic path decides (store with unknown address)
    const std::uint32_t dst = r.a[idiom.dst].v & 0x00FFFFFFU;
    if (dst < kZ80Ram || dst >= kZ80Window) return Copy::generic;  // not a Z80 destination
    if (c.busreq > 1U || c.released > 1U || !granted(c)) return Copy::dead;  // no bus (typed runtime stop) or unknown latches
    if (!r.d[idiom.counter].has(0xFFFFU)) { refuse(StaticRefusal::length_unknown); return Copy::dead; }
    const std::uint32_t count = (r.d[idiom.counter].v & 0xFFFFU) + 1U;
    if (dst + count > kZ80RamEnd) { refuse(StaticRefusal::destination_out_of_ram); return Copy::dead; }
    const bool src_known = r.a[idiom.src].full();
    const std::uint32_t src = r.a[idiom.src].v & 0x00FFFFFFU;
    const bool from_rom = src_known && src < kRomEnd && static_cast<std::uint64_t>(src) + count <= rom_.size();
    const bool from_stage = src_known && !from_rom && !c.stage.empty() && src >= c.stage_base &&
                            static_cast<std::uint64_t>(src) + count <= static_cast<std::uint64_t>(c.stage_base) + c.stage.size();
    if (!from_rom && !from_stage) {
      refuse(StaticRefusal::source_not_immutable);
      return Copy::dead;
    }
    for (std::uint32_t i = 0; i < count; ++i) note_ram_write(c, dst - kZ80Ram + i, from_rom ? rom_[src + i] : c.stage[src - c.stage_base + i]);
    c.proof = std::max<std::uint8_t>(c.proof, static_cast<std::uint8_t>(from_stage ? StaticProof::kosinski : StaticProof::rom_copy));
    ++out_.copy_loops;
    r.a[idiom.src] = K(src + count);
    r.a[idiom.dst] = K(dst + count);
    r.d[idiom.counter] = Reg{(r.d[idiom.counter].v & 0xFFFF0000U) | 0xFFFFU, r.d[idiom.counter].mask | 0xFFFFU};
    r.f = Flags{};
    return Copy::applied;
  }

  // ---- the Kosinski decompressor call ----------------------------------------------------------------------------------
  [[nodiscard]] bool recognised_routine(std::uint32_t target) {
    const auto cached = routines_.find(target);
    if (cached != routines_.end()) return cached->second;
    bool ok = false;
    std::uint32_t at = target;
    for (; at < target + kMaxRoutineBytes && at < rom_.size(); ) {
      const auto instruction = decode(at);
      if (!instruction) break;
      at += (*instruction)->length;
      if ((*instruction)->op.kind == M68kIrKind::return_from_subroutine) {
        const std::string digest = sha256_hex(std::span<const std::uint8_t>(rom_.data() + target, at - target));
        for (const RoutineForm& form : kKosinskiForms)
          if (digest == form.digest_hex) ok = true;
        break;
      }
    }
    routines_[target] = ok;
    return ok;
  }

  // A call with a recognised-routine target. Returns true if the call was consumed (the caller continues at the next instruction).
  // `consumed` false: not a decompressor call, handle as an ordinary call.
  bool decompressor_call(Context& c, Regs& r, std::uint32_t target, bool& consumed, bool& live) {
    consumed = false;
    live = true;
    if (!recognised_routine(target)) return true;
    consumed = true;
    if (!r.a[0].full() || !r.a[1].full()) { refuse(StaticRefusal::decompression_unproven); live = false; return true; }
    const std::uint32_t src = r.a[0].v & 0x00FFFFFFU, dst = r.a[1].v & 0x00FFFFFFU;
    if (dst < kZ80Ram || dst >= kZ80Window) {
      // Decompresses elsewhere: no Z80 effect now (registers are clobbered by the caller). A constant work-RAM destination of a constant ROM
      // stream is remembered as the staged buffer; anything else the routine writes discards the previous one.
      c.stage.clear();
      if (dst >= kWorkRam && dst < kWorkRamEnd && src < kRomEnd && src < rom_.size()) {
        KosinskiResult staged = kosinski_decode(rom_, src, std::min<std::size_t>(kMaxStageBytes, kWorkRamEnd - dst));
        if (staged.error == KosinskiError::none && !staged.output.empty()) {
          c.stage_base = dst;
          c.stage = std::move(staged.output);
          ++out_.staged_decodes;
        }
      }
      return true;
    }
    if (c.busreq > 1U || c.released > 1U || !granted(c)) { live = false; return true; }
    if (src >= kRomEnd || src >= rom_.size()) { refuse(StaticRefusal::source_not_immutable); live = false; return true; }
    if (dst >= kZ80RamEnd) { refuse(StaticRefusal::destination_out_of_ram); live = false; return true; }
    const KosinskiResult decoded = kosinski_decode(rom_, src, kZ80RamEnd - dst);
    if (decoded.error != KosinskiError::none || decoded.output.empty()) {
      refuse(decoded.error == KosinskiError::decoded_length_exceeded ? StaticRefusal::destination_out_of_ram : StaticRefusal::decode_failed);
      live = false;
      return true;
    }
    if (decoded.end > rom_.size() || decoded.output.size() > kMaxDecompressedBytes) { refuse(StaticRefusal::decode_failed); live = false; return true; }
    for (std::size_t i = 0; i < decoded.output.size(); ++i) note_ram_write(c, dst - kZ80Ram + static_cast<std::uint32_t>(i), decoded.output[i]);
    c.proof = static_cast<std::uint8_t>(StaticProof::kosinski);
    ++out_.decoder_calls;
    return true;
  }

  // ---- one instruction ---------------------------------------------------------------------------------------------------
  bool condition_known(const Regs& r, M68kCondition cc, bool& result) const {
    const Flags& f = r.f;
    const auto need = [&](std::initializer_list<std::int8_t> flags) {
      for (const std::int8_t x : flags) if (x < 0) return false;
      return true;
    };
    switch (cc) {
      case M68kCondition::always: result = true; return true;
      case M68kCondition::never: result = false; return true;
      case M68kCondition::eq: if (!need({f.z})) return false; result = f.z == 1; return true;
      case M68kCondition::ne: if (!need({f.z})) return false; result = f.z == 0; return true;
      case M68kCondition::cs: if (!need({f.c})) return false; result = f.c == 1; return true;
      case M68kCondition::cc: if (!need({f.c})) return false; result = f.c == 0; return true;
      case M68kCondition::mi: if (!need({f.n})) return false; result = f.n == 1; return true;
      case M68kCondition::pl: if (!need({f.n})) return false; result = f.n == 0; return true;
      case M68kCondition::hi: if (!need({f.c, f.z})) return false; result = f.c == 0 && f.z == 0; return true;
      case M68kCondition::ls: if (!need({f.c, f.z})) return false; result = f.c == 1 || f.z == 1; return true;
      case M68kCondition::ge: if (!need({f.n, f.v})) return false; result = f.n == f.v; return true;
      case M68kCondition::lt: if (!need({f.n, f.v})) return false; result = f.n != f.v; return true;
      case M68kCondition::gt: if (!need({f.z, f.n, f.v})) return false; result = f.z == 0 && f.n == f.v; return true;
      case M68kCondition::le: if (!need({f.z, f.n, f.v})) return false; result = f.z == 1 || f.n != f.v; return true;
      default: return false;
    }
  }

  void end_path() { ++out_.horizon_paths; }

  void step(std::uint32_t pc, std::uint32_t ctx_id, const Regs& entry) {
    Context c = contexts_[ctx_id];
    Regs r = entry;
    const auto decoded = decode(pc);
    if (!decoded) { end_path(); return; }
    const Decoded& d = **decoded;
    const M68kIrOperation& op = d.op;
    const std::uint32_t next = pc + d.length;
    const Width w = op.size;

    if (const auto idiom = copy_idiom(pc, d)) {
      const Copy applied = apply_copy(c, r, *idiom);
      if (applied == Copy::dead) return;
      if (applied == Copy::applied) {
        seed(idiom->after, intern(c), r);
        return;
      }
    }

    switch (op.kind) {
      case M68kIrKind::write_moveq: {
        r.d[static_cast<unsigned>(op.destination)] = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)));
        set_nz(r, static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)), Width::long_word);
        break;
      }
      case M68kIrKind::write_move: {
        const auto value = read_value(r, c, op.source_ea, w);
        if (op.destination_ea.mode == M68kEaMode::data_register) {
          const unsigned reg = op.destination_ea.reg & 7U;
          if (value) {
            r.d[reg] = Reg{(r.d[reg].v & ~wmask(w)) | *value, r.d[reg].mask | wmask(w)};
            set_nz(r, *value, w);
          } else {
            r.d[reg].mask &= ~wmask(w);
            r.f = Flags{};
          }
        } else if (is_memory(op.destination_ea)) {
          const bool stack = (op.destination_ea.mode == M68kEaMode::address_indirect || op.destination_ea.mode == M68kEaMode::address_postinc ||
                              op.destination_ea.mode == M68kEaMode::address_predec || op.destination_ea.mode == M68kEaMode::address_disp16) &&
                             (op.destination_ea.reg & 7U) == 7U;
          const auto at = address_of(r, op.destination_ea, w);
          const bool constant_form = op.source_ea.mode == M68kEaMode::immediate &&
                                     (op.destination_ea.mode == M68kEaMode::absolute_long || op.destination_ea.mode == M68kEaMode::absolute_word);
          if (store(c, at, w, value, true, constant_form, pc, stack) == Flow::dead) return;
          if (value) set_nz(r, *value, w); else r.f = Flags{};
        } else {
          clobber(r, op);
        }
        break;
      }
      case M68kIrKind::write_movea: {
        const auto value = read_value(r, c, op.source_ea, w);
        const unsigned reg = op.destination_ea.reg & 7U;
        if (reg != 7U) r.a[reg] = value ? K(sext(*value, w)) : Reg{};
        break;
      }
      case M68kIrKind::load_effective_address: {
        const unsigned reg = op.destination_ea.reg & 7U;
        const auto& ea = op.source_ea;
        if (reg == 7U) break;
        Reg out{};
        if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16) out = K(ea.absolute_address);
        else if (ea.mode == M68kEaMode::address_indirect && r.a[ea.reg & 7U].full()) out = r.a[ea.reg & 7U];
        else if (ea.mode == M68kEaMode::address_disp16 && r.a[ea.reg & 7U].full())
          out = K(r.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
        r.a[reg] = out;
        break;
      }
      case M68kIrKind::add_quick: case M68kIrKind::subtract_quick: case M68kIrKind::add_immediate: case M68kIrKind::subtract_immediate: {
        const auto& ea = op.destination_ea;
        const bool quick = op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::subtract_quick;
        const bool add = op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::add_immediate;
        if (op.source_ea.mode != M68kEaMode::immediate) { clobber(r, op); break; }
        (void)quick;
        const std::uint32_t amount = op.source_ea.immediate_value;
        if (ea.mode == M68kEaMode::address_register && (ea.reg & 7U) != 7U) {
          const unsigned reg = ea.reg & 7U;
          if (r.a[reg].full()) r.a[reg] = K(add ? r.a[reg].v + amount : r.a[reg].v - amount);
        } else if (ea.mode == M68kEaMode::data_register && r.d[ea.reg & 7U].has(wmask(w))) {
          const unsigned reg = ea.reg & 7U;
          const std::uint32_t old = r.d[reg].v & wmask(w);
          const std::uint32_t res = (add ? old + amount : old - amount) & wmask(w);
          r.d[reg].v = (r.d[reg].v & ~wmask(w)) | res;
          set_nz(r, res, w);
          r.f.c = -1;
          r.f.v = -1;
        } else if (is_memory(ea)) {
          // a read-modify-write of memory: never a Z80-window store the walk models
          const auto at = address_of(r, ea, w);
          touch(c, at, nbytes(w), (ea.reg & 7U) == 7U);
          if (at && *at >= kZ80Ram && *at < kZ80Window && !(c.busreq == 0U)) { refuse(StaticRefusal::z80_store_form_unsupported); return; }
          r.f = Flags{};
        } else {
          clobber(r, op);
        }
        break;
      }
      case M68kIrKind::test_operand: {
        const auto value = read_value(r, c, op.source_ea, w);
        if (value) set_nz(r, *value, w); else r.f = Flags{};
        break;
      }
      case M68kIrKind::compare: case M68kIrKind::compare_immediate: {
        const auto source = read_value(r, c, op.source_ea, w);
        const auto destination = read_value(r, c, op.destination_ea, w);
        if (source && destination) {
          const auto result = M68kSubtractionResultSpecification::evaluate(*source, *destination, w);
          r.f.z = result.zero ? 1 : 0; r.f.n = result.negative ? 1 : 0; r.f.v = result.overflow ? 1 : 0; r.f.c = result.carry ? 1 : 0;
        } else {
          r.f = Flags{};
        }
        break;
      }
      case M68kIrKind::bit_test: {
        // BTST: Z = the tested bit clear. Memory form is a byte; the BUSACK byte read is modelled.
        const auto bit = read_value(r, c, op.source_ea, Width::byte);
        std::optional<std::uint32_t> value;
        if (op.destination_ea.mode == M68kEaMode::data_register) value = read_value(r, c, op.destination_ea, Width::long_word);
        else value = read_value(r, c, op.destination_ea, Width::byte);
        if (bit && value) {
          const unsigned n = *bit & (op.destination_ea.mode == M68kEaMode::data_register ? 31U : 7U);
          r.f.z = ((*value >> n) & 1U) == 0U ? 1 : 0;
        } else {
          r.f.z = -1;
        }
        break;
      }
      case M68kIrKind::movem_transfer: {
        if (op.movem_direction == M68kMovemDirection::memory_to_registers) {
          // Loads (Sega boot tables): from constant ROM addresses; word loads sign-extend into the whole register.
          std::optional<std::uint32_t> at;
          const auto& ea = op.source_ea;
          const unsigned reg = ea.reg & 7U;
          if (ea.mode == M68kEaMode::address_postinc || ea.mode == M68kEaMode::address_indirect) {
            if (r.a[reg].full()) at = r.a[reg].v & 0x00FFFFFFU;
          } else if (ea.mode == M68kEaMode::pc_disp16 || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::absolute_word) {
            at = address_of(r, ea, w);
          }
          unsigned moved = 0;
          for (unsigned bit = 0; bit < 16; ++bit) {
            if (((op.movem_register_mask >> bit) & 1U) == 0U) continue;
            Reg value{};
            if (at) {
              const auto v = rom_value(*at + moved * nbytes(w), w);
              if (v) value = K(sext(*v, w));
            }
            ++moved;
            if (bit < 8) r.d[bit] = value; else if (bit - 8 != 7) r.a[bit - 8] = value;
          }
          if (ea.mode == M68kEaMode::address_postinc && reg != 7U) r.a[reg] = at ? K(*at + moved * nbytes(w)) : Reg{};
          r.f = r.f;  // MOVEM leaves the condition codes
        } else {
          const bool stack = (op.destination_ea.reg & 7U) == 7U &&
                             (op.destination_ea.mode == M68kEaMode::address_predec || op.destination_ea.mode == M68kEaMode::address_indirect);
          const auto at = address_of(r, op.destination_ea, w);
          if (!stack) c.stage.clear();  // a block store extends below a predecrement address: any non-stack MOVEM may reach the staged buffer
          else touch(c, at, 64U, stack);
          if (at && *at >= kZ80Ram && *at < kZ80Window && c.busreq != 0U) { refuse(StaticRefusal::z80_store_form_unsupported); return; }
          if (!at && !stack && c.busreq != 0U) { refuse(StaticRefusal::z80_store_unknown_address); return; }
          if (at && *at >= kIoBegin && *at < kIoEnd) c.io_written = 1;
          if (op.destination_ea.mode == M68kEaMode::address_predec && (op.destination_ea.reg & 7U) != 7U) {
            const unsigned reg = op.destination_ea.reg & 7U;
            r.a[reg] = Reg{};  // the block pushed an unknown count of registers: the pointer is no longer constant
          }
        }
        break;
      }
      case M68kIrKind::write_clr: {
        if (op.destination_ea.mode == M68kEaMode::data_register) {
          const unsigned reg = op.destination_ea.reg & 7U;
          r.d[reg] = Reg{r.d[reg].v & ~wmask(w), r.d[reg].mask | wmask(w)};
          r.f.z = 1; r.f.n = 0; r.f.c = 0; r.f.v = 0;
        } else if (is_memory(op.destination_ea)) {
          const bool stack = (op.destination_ea.reg & 7U) == 7U && op.destination_ea.mode != M68kEaMode::absolute_long &&
                             op.destination_ea.mode != M68kEaMode::absolute_word;
          const auto at = address_of(r, op.destination_ea, w);
          if (store(c, at, w, 0U, true, op.destination_ea.mode == M68kEaMode::absolute_long || op.destination_ea.mode == M68kEaMode::absolute_word, pc, stack) ==
              Flow::dead)
            return;
          r.f.z = 1; r.f.n = 0; r.f.c = 0; r.f.v = 0;
        }
        break;
      }
      case M68kIrKind::no_operation: break;
      case M68kIrKind::read_status_register:
        if (is_memory(op.destination_ea)) {  // MOVE SR,<ea>: a word store the walk does not model
          const bool stack = (op.destination_ea.reg & 7U) == 7U && op.destination_ea.mode != M68kEaMode::absolute_long &&
                             op.destination_ea.mode != M68kEaMode::absolute_word;
          const auto at = address_of(r, op.destination_ea, Width::word);
          touch(c, at, 2U, stack);
          if ((at && ((*at >= kZ80Ram && *at < kZ80Window) || (*at >= kBusRegionBegin && *at < kBusRegionEnd)) && c.busreq != 0U) ||
              (!at && !stack && c.busreq != 0U)) {
            refuse(StaticRefusal::z80_store_form_unsupported);
            return;
          }
        }
        break;
      case M68kIrKind::write_status_register: case M68kIrKind::logical_immediate_to_sr:
      case M68kIrKind::logical_immediate_to_ccr: case M68kIrKind::write_user_stack_pointer: case M68kIrKind::read_user_stack_pointer:
        if (op.kind == M68kIrKind::logical_immediate_to_ccr || op.kind == M68kIrKind::write_status_register || op.kind == M68kIrKind::logical_immediate_to_sr)
          r.f = Flags{};
        if (op.kind == M68kIrKind::read_user_stack_pointer) r.a[op.destination_ea.reg & 7U] = Reg{};
        break;
      case M68kIrKind::stop_until_interrupt: case M68kIrKind::trap_exception: case M68kIrKind::instruction_exception:
      case M68kIrKind::trap_on_overflow: case M68kIrKind::return_from_exception: case M68kIrKind::return_restore_condition_codes:
        end_path();
        return;
      default: {
        // Any other instruction: registers it may write become unknown. A memory destination inside the Z80 window while the 68K
        // owns the bus is a store form the walk does not model.
        if (is_memory(op.destination_ea) && op.kind != M68kIrKind::jump_general && op.kind != M68kIrKind::call_general &&
            op.kind != M68kIrKind::bit_test) {
          const bool stack = (op.destination_ea.reg & 7U) == 7U && op.destination_ea.mode != M68kEaMode::absolute_long &&
                             op.destination_ea.mode != M68kEaMode::absolute_word;
          switch (op.kind) {
            case M68kIrKind::branch_ne_short: case M68kIrKind::branch_always_short: case M68kIrKind::general_branch: case M68kIrKind::bsr_call:
            case M68kIrKind::dbcc_loop: case M68kIrKind::return_from_subroutine: case M68kIrKind::push_effective_address: break;
            default: {
              Regs probe = r;
              const auto at = address_of(probe, op.destination_ea, w);
              touch(c, at, 4U, stack);
              if (at) {
                if (*at >= kIoBegin && *at < kIoEnd) c.io_written = 1;
                if ((*at >= kZ80Ram && *at < kZ80Window) || (*at >= kBusRegionBegin && *at < kBusRegionEnd)) {
                  if (c.busreq != 0U || *at >= kBusRegionBegin) { refuse(StaticRefusal::z80_store_form_unsupported); return; }
                }
              } else if (!stack && c.busreq != 0U) {
                refuse(StaticRefusal::z80_store_unknown_address);
                return;
              }
            }
          }
        }
        switch (op.kind) {
          case M68kIrKind::branch_ne_short: case M68kIrKind::branch_always_short: case M68kIrKind::general_branch: case M68kIrKind::bsr_call:
          case M68kIrKind::dbcc_loop: case M68kIrKind::jump_general: case M68kIrKind::call_general: case M68kIrKind::return_from_subroutine: break;
          default: clobber(r, op);
        }
        break;
      }
    }

    // ---- control transfer ----------------------------------------------------------------------------------------------
    const auto continue_to = [&](std::uint32_t target, const Regs& state, const Context& context) { seed(target & 0x00FFFFFFU, intern(context), state); };
    switch (op.kind) {
      case M68kIrKind::return_from_subroutine: {
        if (c.calls.empty()) { end_path(); return; }
        Context popped = c;
        const std::uint32_t back = popped.calls.back();
        popped.calls.pop_back();
        continue_to(back, r, popped);
        return;
      }
      case M68kIrKind::bsr_call: case M68kIrKind::call_general: {
        std::optional<std::uint32_t> target;
        for (const auto& s : d.control.successors) if (s.kind == M68kControlSuccessorKind::call_target) target = s.target & 0x00FFFFFFU;
        if (op.kind == M68kIrKind::call_general && !target) {
          const auto& ea = op.source_ea;
          if (ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::absolute_word) target = m68k_data_access_ea_address(ea) & 0x00FFFFFFU;
          else if (ea.mode == M68kEaMode::address_indirect && r.a[ea.reg & 7U].full()) target = r.a[ea.reg & 7U].v & 0x00FFFFFFU;
        }
        if (!target) { end_path(); return; }
        bool consumed = false, live = true;
        decompressor_call(c, r, *target, consumed, live);
        if (consumed) {
          if (!live) return;
          Regs after;  // the routine clobbers its working registers; nothing about them is claimed
          continue_to(next, after, c);
          return;
        }
        c.stage.clear();  // an unrecognised callee may write any RAM
        if (granted(c)) {  // a routine the walk cannot recognise, called while the 68K owns the bus: its writes into the window are not proven
          refuse(StaticRefusal::decompression_unproven);
          return;
        }
        if (c.calls.size() >= kStaticMaxCallDepth) { refuse(StaticRefusal::call_depth); return; }
        Context inner = c;
        inner.calls.push_back(next);
        continue_to(*target, r, inner);
        return;
      }
      case M68kIrKind::jump_general: {
        std::optional<std::uint32_t> target;
        const auto& ea = op.source_ea;
        if (ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::absolute_word) target = m68k_data_access_ea_address(ea) & 0x00FFFFFFU;
        else if (ea.mode == M68kEaMode::address_indirect && r.a[ea.reg & 7U].full()) target = r.a[ea.reg & 7U].v & 0x00FFFFFFU;
        else if (ea.mode == M68kEaMode::pc_disp16) target = ea.absolute_address & 0x00FFFFFFU;
        if (!target) { end_path(); return; }
        continue_to(*target, r, c);
        return;
      }
      case M68kIrKind::dbcc_loop: {
        if (op.condition != M68kCondition::never) {
          // DBcc with a condition: not modelled beyond both edges, counter unknown
          Regs both = r;
          both.d[op.destination_ea.reg & 7U].mask &= ~0xFFFFU;
          for (const auto& s : d.control.successors) continue_to(s.target, both, c);
          return;
        }
        const unsigned reg = op.destination_ea.reg & 7U;
        Regs taken = r, fall = r;
        taken.d[reg].mask &= ~0xFFFFU;  // the loop counter is not tracked around a back edge unless exactly known below
        fall.d[reg] = Reg{(r.d[reg].v & 0xFFFF0000U) | 0xFFFFU, r.d[reg].mask | 0xFFFFU};
        bool take = true, leave = true;
        if (!r.d[reg].has(0xFFFFU) && granted(c)) { refuse(StaticRefusal::window_control_unknown); return; }
        if (r.d[reg].has(0xFFFFU)) {
          const std::uint32_t counter = r.d[reg].v & 0xFFFFU;
          if (counter == 0U) { take = false; } else { leave = false; taken.d[reg] = Reg{(r.d[reg].v & 0xFFFF0000U) | (counter - 1U), r.d[reg].mask | 0xFFFFU}; }
        }
        for (const auto& s : d.control.successors) {
          const std::uint32_t target = s.target & 0x00FFFFFFU;
          if (s.kind == M68kControlSuccessorKind::conditional_target) {
            if (take) { note_span(target, pc); continue_to(target, taken, c); }
          } else if (s.kind == M68kControlSuccessorKind::conditional_fallthrough) {
            if (leave) continue_to(target, fall, c);
          }
        }
        return;
      }
      case M68kIrKind::branch_ne_short: case M68kIrKind::branch_always_short: case M68kIrKind::general_branch: {
        const M68kCondition cc = op.kind == M68kIrKind::branch_always_short ? M68kCondition::always
                                 : op.kind == M68kIrKind::branch_ne_short  ? M68kCondition::ne
                                                                           : op.condition;
        bool outcome = false;
        const bool decided = condition_known(r, cc, outcome);
        if (!decided && granted(c)) { refuse(StaticRefusal::window_control_unknown); return; }
        for (const auto& s : d.control.successors) {
          const std::uint32_t target = s.target & 0x00FFFFFFU;
          const bool is_taken = s.kind == M68kControlSuccessorKind::branch_target || s.kind == M68kControlSuccessorKind::conditional_target;
          const bool is_fall = s.kind == M68kControlSuccessorKind::fallthrough || s.kind == M68kControlSuccessorKind::conditional_fallthrough;
          if (decided && ((is_taken && !outcome) || (is_fall && outcome))) continue;
          if (is_taken) note_span(target, pc);
          if (is_taken || is_fall) continue_to(target, r, c);
        }
        return;
      }
      default: break;
    }
    // sequential
    for (const auto& s : d.control.successors) {
      if (s.kind == M68kControlSuccessorKind::fallthrough || s.kind == M68kControlSuccessorKind::conditional_fallthrough) {
        continue_to(s.target, r, c);
        return;
      }
    }
    if (d.control.successors.empty() && d.control.dynamic == M68kDynamicControlFamily::none && !d.control.always_raises_exception) {
      continue_to(next, r, c);
      return;
    }
    end_path();
  }

  void note_span(std::uint32_t target, std::uint32_t branch_pc) {
    if (target <= branch_pc) spans_.emplace_back(target, branch_pc);
  }

  // ---- result ---------------------------------------------------------------------------------------------------------------
  void finish(bool bounded) {
    out_.candidates = static_cast<std::uint32_t>(candidates_.size());
    // an epoch is invalid when one of its non-constant stores lies inside a backward-branch span the walk observed
    std::vector<bool> invalid(candidates_.size(), false);
    for (std::size_t i = 0; i < candidates_.size(); ++i)
      for (const std::uint32_t store_pc : candidates_[i].store_pcs)
        if (std::any_of(spans_.begin(), spans_.end(), [&](const auto& s) { return store_pc >= s.first && store_pc <= s.second; })) {
          invalid[i] = true;
          refuse(StaticRefusal::loop_unproven);
          break;
        }
    // every context's trace, truncated at its first invalid epoch
    std::set<std::vector<std::uint32_t>> traces;
    for (const Context& c : contexts_) {
      std::vector<std::uint32_t> trace;
      for (const std::uint32_t id : c.epochs) {
        if (invalid[id]) break;
        trace.push_back(id);
      }
      if (!trace.empty()) traces.insert(std::move(trace));
    }
    // Epochs are compared by value (two paths may reach the same epoch through different contexts).
    const auto same = [&](std::uint32_t a, std::uint32_t b) {
      return candidates_[a].epoch.ram == candidates_[b].epoch.ram && candidates_[a].epoch.written == candidates_[b].epoch.written;
    };
    std::vector<std::uint32_t> chain;
    for (const auto& trace : traces) if (trace.size() > chain.size()) chain = trace;
    bool ambiguous = false;
    for (const auto& trace : traces)
      for (std::size_t i = 0; i < trace.size(); ++i)
        if (i >= chain.size() || !same(trace[i], chain[i])) {
          // a divergence: keep only the part every trace agrees on
          std::size_t agree = 0;
          while (agree < chain.size() && agree < trace.size() && same(trace[agree], chain[agree])) ++agree;
          chain.resize(agree);
          ambiguous = true;
          break;
        }
    if (ambiguous) refuse(StaticRefusal::ambiguous_epoch_order);
    if (chain.size() > kStaticMaxEpochs) { chain.resize(kStaticMaxEpochs); refuse(StaticRefusal::image_bound); }
    for (const std::uint32_t id : chain) out_.epochs.push_back({candidates_[id].epoch, candidates_[id].proof});

    bool affecting = false;
    for (const auto& [refusal, count] : out_.refusals) if (count != 0U && static_refusal_affects_epochs(refusal)) affecting = true;
    if (out_.epochs.empty()) {
      if (bounded) out_.outcome = StaticOutcome::resource_bound;
      else if (ambiguous) out_.outcome = StaticOutcome::unsupported;
      else out_.outcome = StaticOutcome::none;
    } else {
      out_.outcome = affecting || bounded ? StaticOutcome::partial : StaticOutcome::complete;
    }
  }

  std::span<const std::uint8_t> rom_;
  StaticProduction& out_;
  std::map<std::uint32_t, std::optional<Decoded>> decoded_;
  std::map<std::uint32_t, bool> routines_;
  std::vector<Context> contexts_;
  std::map<Context, std::uint32_t> context_ids_;
  struct NodeInfo {
    std::uint32_t exact = 0;
    bool widened = false;
    Regs accum{};
    std::set<std::string> seen;
  };
  struct WorkItem {
    std::uint32_t pc, ctx;
    Regs regs;
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, NodeInfo> nodes_;
  std::deque<WorkItem> work_;
  std::vector<CandidateEpoch> candidates_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> spans_;
  std::uint32_t steps_ = 0;
};

}  // namespace

StaticProduction produce_static_epochs(std::span<const std::uint8_t> rom) {
  StaticProduction production;
  Walker(rom, production).run();
  return production;
}

std::size_t preload_registry(Registry& registry, StaticProduction& production) {
  std::size_t registered = 0;
  for (const StaticEpoch& entry : production.epochs) {
    std::uint32_t ordinal = 0;
    const AddOutcome outcome = registry.add(entry.epoch, ordinal, Derivation::static_reconstruction, static_proof_name(entry.proof));
    if (outcome == AddOutcome::bound_exceeded) {
      ++production.refusals[StaticRefusal::image_bound];
      if (production.outcome == StaticOutcome::complete) production.outcome = StaticOutcome::partial;
      break;
    }
    if (outcome == AddOutcome::added) ++registered;
  }
  return registered;
}

std::size_t kosinski_routine_form_count() noexcept { return sizeof(kKosinskiForms) / sizeof(kKosinskiForms[0]); }

}  // namespace segarecomp::machine::genesis::z80
