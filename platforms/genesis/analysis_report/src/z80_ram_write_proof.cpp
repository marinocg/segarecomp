// SEG-030-T010 (ADR 0079 decision 7, record T010; report-only). See z80_ram_write_proof.hpp.

#include "segarecomp/genesis_analysis_report/z80_ram_write_proof.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <span>

#include "segarecomp/cpu/z80/decode.hpp"
#include "segarecomp/cpu/z80/effects.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp {
namespace {

namespace z80 = cpu::z80;
using Reason = GenesisZ80ProofReason;

constexpr std::uint32_t ram_size = 0x2000U;          // Z80 sound RAM (mirrored once below $4000)
constexpr std::uint32_t ram_view_end = 0x4000U;
constexpr std::uint32_t bank_first = 0x6000U, bank_last = 0x6100U;  // Z80 view of the bank register
constexpr std::uint32_t window_first = 0x8000U;
constexpr std::uint16_t interrupt_entry = 0x0038U;  // IM 0 with acknowledge byte $FF and IM 1 (contract section 8)
constexpr std::size_t slot_value_bound = 256U;     // return-slot value-set bound (beyond: Unknown)
constexpr std::size_t iteration_bound = 1000000U;
constexpr std::uint32_t bank_mask = 0x1FFU;  // the 9-bit latch
constexpr std::uint32_t bank_z80_area_first = 0x140U, bank_z80_area_last = 0x142U;  // 68K $A00000-$A0FFFF
// The 68K view of the Z80 area: sound RAM (and the undefined upper half, conservatively an alias), and the bank register block
// (bank register plus the unused space and the VDP window behind it, conservatively).
constexpr std::uint32_t m68k_ram_first = 0xA00000U, m68k_ram_last = 0xA04000U;
constexpr std::uint32_t m68k_upper_first = 0xA08000U, m68k_upper_last = 0xA10000U;
constexpr std::uint32_t m68k_bank_first = 0xA06000U, m68k_bank_last = 0xA08000U;

std::uint32_t physical(std::uint32_t address) { return address & (ram_size - 1U); }

// ---------------------------------------------------------------------------------------------------------------
// Abstract state.

enum R8 : std::size_t { A, B, C, D, E, H, L, R8_COUNT };

struct State {
  std::array<std::optional<std::uint8_t>, R8_COUNT> r{};
  std::optional<std::uint16_t> ix, iy, sp;
  std::uint8_t iff{};   // bit 0: may be disabled, bit 1: may be enabled, bit 2: may be enabled by the previous EI (not yet accepted)
  std::uint8_t im{};    // bit n: may be IM n
  std::uint16_t bank_known{};  // latch bits whose value is known
  std::uint16_t bank_value{};  // their values (unknown bits are 0)
  std::map<std::uint16_t, std::set<std::uint16_t>> slots;  // return slot (logical address of its low byte) -> values; absent: Unknown
  friend bool operator==(const State &, const State &) = default;
};

template <typename T>
std::optional<T> join_value(const std::optional<T> &left, const std::optional<T> &right) {
  return left && right && *left == *right ? left : std::nullopt;
}

State join(const State &left, const State &right) {
  State out;
  for (std::size_t i = 0; i < R8_COUNT; ++i) out.r[i] = join_value(left.r[i], right.r[i]);
  out.ix = join_value(left.ix, right.ix);
  out.iy = join_value(left.iy, right.iy);
  out.sp = join_value(left.sp, right.sp);
  out.iff = left.iff | right.iff;
  out.im = left.im | right.im;
  out.bank_known = static_cast<std::uint16_t>(left.bank_known & right.bank_known & ~(left.bank_value ^ right.bank_value) & bank_mask);
  out.bank_value = static_cast<std::uint16_t>(left.bank_value & out.bank_known);
  for (const auto &[slot, values] : left.slots) {
    const auto found = right.slots.find(slot);
    if (found == right.slots.end()) continue;
    std::set<std::uint16_t> merged = values;
    merged.insert(found->second.begin(), found->second.end());
    if (merged.size() <= slot_value_bound) out.slots.emplace(slot, std::move(merged));
  }
  return out;
}

std::optional<std::uint16_t> pair(const State &s, R8 high, R8 low) {
  if (!s.r[high] || !s.r[low]) return std::nullopt;
  return static_cast<std::uint16_t>(*s.r[high] << 8U | *s.r[low]);
}

std::optional<std::uint16_t> read(const State &s, z80::Reg reg) {
  using z80::Reg;
  switch (reg) {
  case Reg::a: return s.r[A];
  case Reg::b: return s.r[B];
  case Reg::c: return s.r[C];
  case Reg::d: return s.r[D];
  case Reg::e: return s.r[E];
  case Reg::h: return s.r[H];
  case Reg::l: return s.r[L];
  case Reg::bc: return pair(s, B, C);
  case Reg::de: return pair(s, D, E);
  case Reg::hl: return pair(s, H, L);
  case Reg::sp: return s.sp;
  case Reg::ix: return s.ix;
  case Reg::iy: return s.iy;
  }
  return std::nullopt;
}

void write(State &s, z80::Reg reg, std::optional<std::uint16_t> value) {
  using z80::Reg;
  const auto byte = [&](bool high) -> std::optional<std::uint8_t> {
    if (!value) return std::nullopt;
    return static_cast<std::uint8_t>(high ? *value >> 8U : *value);
  };
  switch (reg) {
  case Reg::a: s.r[A] = byte(false); return;
  case Reg::b: s.r[B] = byte(false); return;
  case Reg::c: s.r[C] = byte(false); return;
  case Reg::d: s.r[D] = byte(false); return;
  case Reg::e: s.r[E] = byte(false); return;
  case Reg::h: s.r[H] = byte(false); return;
  case Reg::l: s.r[L] = byte(false); return;
  case Reg::bc: s.r[B] = byte(true), s.r[C] = byte(false); return;
  case Reg::de: s.r[D] = byte(true), s.r[E] = byte(false); return;
  case Reg::hl: s.r[H] = byte(true), s.r[L] = byte(false); return;
  case Reg::sp: s.sp = value; return;
  case Reg::ix: s.ix = value; return;
  case Reg::iy: s.iy = value; return;
  }
}

void havoc_general(State &s) {
  s.r.fill(std::nullopt);
  s.ix.reset();
  s.iy.reset();
}

std::optional<std::uint16_t> evaluate(const State &s, const z80::ValueExpr &value, z80::Reg target) {
  using Kind = z80::ValueExpr::Kind;
  const std::uint32_t mask = z80::is_wide(target) ? 0xFFFFU : 0xFFU;
  switch (value.kind) {
  case Kind::constant: return static_cast<std::uint16_t>(value.constant & mask);
  case Kind::copy: {
    const auto source = read(s, value.source);
    if (!source) return std::nullopt;
    return static_cast<std::uint16_t>(*source & mask);
  }
  case Kind::add_constant: {
    const auto source = read(s, value.source);
    if (!source) return std::nullopt;
    return static_cast<std::uint16_t>((static_cast<std::int64_t>(*source) + value.delta) & mask);
  }
  case Kind::load:     // Z80 RAM is writable by both CPUs: never a constant
  case Kind::opaque: return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::uint16_t> address_of(const State &s, const z80::AddressExpr &address) {
  if (address.kind == z80::AddressExpr::Kind::absolute) return address.absolute;
  const auto base = read(s, address.base);
  if (!base) return std::nullopt;
  return static_cast<std::uint16_t>((static_cast<std::int64_t>(*base) + address.offset) & 0xFFFF);
}

// ---------------------------------------------------------------------------------------------------------------
// One instruction (or interrupt entry) transfer.

// A store of `width` bytes: little-endian value bytes at `address`, `address + 1`.
struct Store {
  std::optional<std::uint16_t> address;  // nullopt: Unknown target
  std::uint8_t width{1};
  std::optional<std::uint16_t> value;    // nullopt: Unknown value
  bool slot{};                            // a stack push (CALL/RST/interrupt/PUSH): the written word is a return slot
};

struct Transfer {
  std::vector<std::pair<std::uint16_t, State>> successors;
  std::vector<Store> stores;
  std::vector<std::uint16_t> slot_reads;  // slots read (RET/RETI/RETN/POP/EX (SP)), each with a known value set
  std::set<Reason> reasons;
  std::uint32_t length{};                 // instruction bytes (0 for an interrupt entry)
  bool decoded{};
};

class Image final : public z80::LogicalFetch {
public:
  explicit Image(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  z80::FetchedByte fetch(std::uint16_t address) const override {
    z80::FetchedByte out;
    if (address >= ram_view_end || physical(address) >= bytes_.size()) return out;  // non_code
    out.kind = z80::FetchKind::byte;
    out.value = bytes_[physical(address)];
    out.image_id = 1U;
    out.image_offset = physical(address);
    return out;
  }

private:
  std::span<const std::uint8_t> bytes_;
};

// Applies the stores to the latch and the return slots (bytes in program order).
void apply_stores(State &s, const std::vector<Store> &stores) {
  for (const auto &store : stores) {
    if (!store.address) {
      s.slots.clear();  // the proof fails anyway (store_target_unknown); keep the state small
      continue;
    }
    for (std::uint32_t i = 0; i < store.width; ++i) {
      const auto byte_address = static_cast<std::uint16_t>(*store.address + i);
      if (byte_address >= bank_first && byte_address < bank_last) {
        // contract section 2: bank = (data & 1) << 8 | bank >> 1 (nine writes select a bank)
        const std::uint16_t known = static_cast<std::uint16_t>((s.bank_known >> 1U) | (store.value ? 0x100U : 0U));
        const std::uint16_t value =
            static_cast<std::uint16_t>((s.bank_value >> 1U) | (store.value ? ((*store.value >> (8U * i)) & 1U) << 8U : 0U));
        s.bank_known = known;
        s.bank_value = static_cast<std::uint16_t>(value & known);
      }
      if (byte_address < ram_view_end) {
        const auto p = physical(byte_address);
        for (auto it = s.slots.begin(); it != s.slots.end();)
          if (physical(it->first) == p || physical(it->first + 1U) == p) it = s.slots.erase(it);
          else ++it;
      }
    }
    if (store.slot && store.width == 2U && *store.address + 1U < ram_view_end && store.value)
      s.slots[*store.address] = {*store.value};
  }
}

class Analyzer {
public:
  Analyzer(std::span<const std::uint8_t> bytes, bool bank_volatile) : image_(bytes), bank_volatile_(bank_volatile) {}

  // Interrupt acceptance at `pc` (taken before the instruction executes).
  Transfer interrupt(std::uint16_t pc, State s) const {
    Transfer out;
    if (bank_volatile_) s.bank_known = s.bank_value = 0U;
    if (s.im & 0x4U) {
      out.reasons.insert(Reason::interrupt_mode_unbounded);
      return out;
    }
    if (!s.sp) {
      out.reasons.insert(Reason::stack_pointer_unknown);
      return out;
    }
    const auto slot = static_cast<std::uint16_t>(*s.sp - 2U);
    out.stores.push_back({slot, 2U, pc, true});
    apply_stores(s, out.stores);
    s.sp = slot;
    s.iff = 1U;  // accepting an interrupt disables it
    out.successors.emplace_back(interrupt_entry, std::move(s));
    return out;
  }

  Transfer step(std::uint16_t pc, State s) const {
    Transfer out;
    if (bank_volatile_) s.bank_known = s.bank_value = 0U;
    // UM0080: an interrupt is not accepted at the instruction that follows EI; after it, the EI shadow is an ordinary enable.
    if (s.iff & 4U) s.iff = static_cast<std::uint8_t>((s.iff & ~4U) | 2U);
    const auto start = z80::decode_at(image_, pc);
    if (start.kind != z80::StartKind::decoded) {
      out.reasons.insert(Reason::undecodable_code);
      return out;
    }
    out.decoded = true;
    const auto &instruction = start.instruction;
    out.length = z80::logical_length(instruction);
    const auto &form = z80::form_descriptor(instruction.form);
    const auto effect = z80::project_effect(instruction);
    const auto next = static_cast<std::uint16_t>(pc + out.length);
    const auto imm = z80::immediate(instruction);
    const auto disp = z80::displacement(instruction).value_or(0);
    const std::uint8_t opcode = instruction.provenance.opcode;
    using M = z80::Mnemonic;
    using O = z80::Operand;
    const M m = form.mnemonic;

    const auto push = [&](State &state, std::optional<std::uint16_t> value) -> bool {
      if (!state.sp) {
        out.reasons.insert(Reason::stack_pointer_unknown);
        return false;
      }
      const auto slot = static_cast<std::uint16_t>(*state.sp - 2U);
      out.stores.push_back({slot, 2U, value, true});
      apply_stores(state, out.stores);
      state.sp = slot;
      return true;
    };
    // The slot at SP: its value set, or nullopt (return_unbounded is the caller's choice of reason).
    const auto slot_values = [&](const State &state) -> std::optional<std::set<std::uint16_t>> {
      if (!state.sp) return std::nullopt;
      const auto found = state.slots.find(*state.sp);
      if (found == state.slots.end()) return std::nullopt;
      out.slot_reads.push_back(*state.sp);
      return found->second;
    };
    const auto return_edges = [&](State state) {
      if (!state.sp) {
        out.reasons.insert(Reason::stack_pointer_unknown);
        return;
      }
      const auto values = slot_values(state);
      if (!values) {
        out.reasons.insert(Reason::return_unbounded);
        return;
      }
      state.sp = static_cast<std::uint16_t>(*state.sp + 2U);
      for (const auto target : *values) out.successors.emplace_back(target, state);
    };

    switch (m) {
    case M::call:
    case M::rst: {
      const std::uint16_t target = m == M::rst ? static_cast<std::uint16_t>(opcode & 0x38U) : imm.value_or(0);
      if (m == M::call && form.dst == O::cc) out.successors.emplace_back(next, s);
      State called = s;
      if (push(called, next)) out.successors.emplace_back(target, std::move(called));
      return out;
    }
    case M::ret:
    case M::reti:
    case M::retn: {
      if (form.dst == O::cc) out.successors.emplace_back(next, s);
      State returned = s;
      if (m != M::ret) returned.iff = 3U;  // IFF1 <- IFF2 (not tracked)
      return_edges(std::move(returned));
      return out;
    }
    case M::push: {
      std::optional<std::uint16_t> value;
      if (form.src == O::ix) value = s.ix;
      else if (form.src == O::iy) value = s.iy;
      else {
        const unsigned p = (opcode >> 4U) & 3U;
        if (p == 0U) value = pair(s, B, C);
        else if (p == 1U) value = pair(s, D, E);
        else if (p == 2U) value = pair(s, H, L);
        // AF: F is not tracked
      }
      if (push(s, value)) out.successors.emplace_back(next, std::move(s));
      return out;
    }
    case M::pop: {
      if (!s.sp) {
        out.reasons.insert(Reason::stack_pointer_unknown);
        return out;
      }
      const auto values = slot_values(s);
      const std::optional<std::uint16_t> value =
          values && values->size() == 1U ? std::optional<std::uint16_t>(*values->begin()) : std::nullopt;
      if (form.dst == O::ix) s.ix = value;
      else if (form.dst == O::iy) s.iy = value;
      else {
        const unsigned p = (opcode >> 4U) & 3U;
        if (p == 0U) write(s, z80::Reg::bc, value);
        else if (p == 1U) write(s, z80::Reg::de, value);
        else if (p == 2U) write(s, z80::Reg::hl, value);
        else s.r[A] = value ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*value >> 8U)) : std::nullopt;
      }
      s.sp = static_cast<std::uint16_t>(*s.sp + 2U);
      out.successors.emplace_back(next, std::move(s));
      return out;
    }
    case M::di:
    case M::ei:
      s.iff = m == M::di ? 1U : 4U;
      out.successors.emplace_back(next, std::move(s));
      return out;
    case M::im:
      s.im = form.dst == O::im0 ? 1U : form.dst == O::im1 ? 2U : 4U;
      out.successors.emplace_back(next, std::move(s));
      return out;
    case M::halt:  // resumes at the next instruction after an interrupt (the interrupt pushes `next`)
      out.successors.emplace_back(next, std::move(s));
      return out;
    default: break;
    }

    // Stores: the projection's own store when the form is supported, else the conservative description of the form.
    if (effect.supported) {
      if (effect.store) {
        const auto &store = *effect.store;
        std::optional<std::uint16_t> value;
        if (store.value.kind == z80::ValueExpr::Kind::constant) value = store.value.constant;
        else if (store.value.kind == z80::ValueExpr::Kind::copy) value = read(s, store.value.source);
        out.stores.push_back({address_of(s, store.address), store.width, value, false});
      }
    } else {
      conservative_stores(form, opcode, imm, disp, s, out.stores);
    }

    State after = s;
    if (m == M::ex && form.dst == O::sp_ind) {  // EX (SP),HL/IX/IY: the register takes the slot, the slot takes the register
      const auto reg = form.src == O::ix ? z80::Reg::ix : form.src == O::iy ? z80::Reg::iy : z80::Reg::hl;
      const auto values = slot_values(s);
      apply_stores(after, out.stores);
      write(after, reg, values && values->size() == 1U ? std::optional<std::uint16_t>(*values->begin()) : std::nullopt);
      out.successors.emplace_back(next, std::move(after));
      return out;
    }
    apply_stores(after, out.stores);
    if (effect.supported) {
      for (const auto &w : effect.writes) write(after, w.target, evaluate(s, w.value, w.target));
    } else if (m == M::ex && form.dst == O::de && form.src == O::hl) {
      const auto de = pair(s, D, E), hl = pair(s, H, L);
      write(after, z80::Reg::de, hl);
      write(after, z80::Reg::hl, de);
    } else if (m == M::nop || m == M::out) {
      // no register write
    } else {
      havoc_general(after);
      const bool sp_field = ((opcode >> 4U) & 3U) == 3U;
      if (form.dst == O::sp || ((form.dst == O::rr || form.dst == O::rr_ed) && sp_field)) after.sp.reset();
    }

    switch (effect.supported ? effect.control : z80::ControlKind::fallthrough) {
    case z80::ControlKind::fallthrough: out.successors.emplace_back(next, std::move(after)); break;
    case z80::ControlKind::jump: out.successors.emplace_back(effect.target, std::move(after)); break;
    case z80::ControlKind::conditional_jump:
      out.successors.emplace_back(effect.target, after);
      out.successors.emplace_back(next, std::move(after));
      break;
    case z80::ControlKind::computed: {
      const auto target = read(s, effect.computed_base);
      if (!target) out.reasons.insert(Reason::indirect_control);
      else out.successors.emplace_back(*target, std::move(after));
      break;
    }
    case z80::ControlKind::call:
    case z80::ControlKind::return_:
    case z80::ControlKind::conditional_return:
      out.reasons.insert(Reason::analysis_bound);  // handled above; unreachable
      break;
    }
    return out;
  }

private:
  // The memory writes of a form the effect projection does not enumerate (Zilog UM0080 instruction set): a memory destination
  // operand (or the memory operand of RES/SET), the block transfers and RRD/RLD. Every other unsupported form writes no memory.
  static void conservative_stores(const z80::FormDescriptor &form, std::uint8_t opcode, std::optional<std::uint16_t> imm, int disp,
                                  const State &s, std::vector<Store> &stores) {
    using M = z80::Mnemonic;
    using O = z80::Operand;
    const auto indexed = [&](z80::Reg base) -> std::optional<std::uint16_t> {
      const auto value = read(s, base);
      if (!value) return std::nullopt;
      return static_cast<std::uint16_t>(*value + disp);
    };
    // nullopt: not a memory operand; otherwise the (possibly Unknown) address.
    using Address = std::optional<std::uint16_t>;
    const auto memory_operand = [&](O operand) -> std::optional<Address> {
      switch (operand) {
      case O::hl_ind: return std::optional<Address>(std::in_place, read(s, z80::Reg::hl));
      case O::ix_d:
      case O::ix_d_copy_r: return std::optional<Address>(std::in_place, indexed(z80::Reg::ix));
      case O::iy_d:
      case O::iy_d_copy_r: return std::optional<Address>(std::in_place, indexed(z80::Reg::iy));
      default: return std::nullopt;
      }
    };
    switch (form.mnemonic) {
    case M::bit: return;  // reads only
    case M::res:
    case M::set:
      if (const auto address = memory_operand(form.src)) stores.push_back({*address, 1U, std::nullopt, false});
      return;
    case M::rrd:
    case M::rld: stores.push_back({read(s, z80::Reg::hl), 1U, std::nullopt, false}); return;
    case M::ldi:
    case M::ldd: stores.push_back({read(s, z80::Reg::de), 1U, std::nullopt, false}); return;
    case M::ini:
    case M::ind: stores.push_back({read(s, z80::Reg::hl), 1U, std::nullopt, false}); return;
    case M::ldir:
    case M::lddr:
    case M::inir:
    case M::indr: {
      // a repeated transfer: a range from DE (HL for INIR/INDR), counted by BC (B): bounded only when both are known
      const bool load = form.mnemonic == M::ldir || form.mnemonic == M::lddr;
      const auto base = read(s, load ? z80::Reg::de : z80::Reg::hl);
      const auto count = load ? read(s, z80::Reg::bc) : read(s, z80::Reg::b);
      if (!base || !count) {
        stores.push_back({std::nullopt, 1U, std::nullopt, false});
        return;
      }
      const std::uint32_t n = *count == 0U ? (load ? 0x10000U : 0x100U) : *count;
      const bool down = form.mnemonic == M::lddr || form.mnemonic == M::indr;
      for (std::uint32_t i = 0; i < n; ++i)
        stores.push_back({static_cast<std::uint16_t>(down ? *base - i : *base + i), 1U, std::nullopt, false});
      return;
    }
    default: break;
    }
    if (const auto address = memory_operand(form.dst)) {  // INC/DEC (HL)/(IX+d), CB/DDCB shifts, LD (IX+d) forms
      stores.push_back({*address, 1U, std::nullopt, false});
      return;
    }
    if (form.dst == O::rr_ind) {
      stores.push_back({((opcode >> 4U) & 1U) == 0U ? read(s, z80::Reg::bc) : read(s, z80::Reg::de), 1U, std::nullopt, false});
      return;
    }
    if (form.dst == O::nn_ind) {
      const bool word = form.src != O::a;
      std::optional<std::uint16_t> value;
      if (form.src == O::hl) value = read(s, z80::Reg::hl);
      else if (form.src == O::ix) value = s.ix;
      else if (form.src == O::iy) value = s.iy;
      else if (form.src == O::rr_ed) {
        constexpr std::array<z80::Reg, 4> rr = {z80::Reg::bc, z80::Reg::de, z80::Reg::hl, z80::Reg::sp};
        value = read(s, rr[(opcode >> 4U) & 3U]);
      } else if (form.src == O::a) value = s.r[A];
      stores.push_back({imm, static_cast<std::uint8_t>(word ? 2U : 1U), value, false});
      return;
    }
    if (form.dst == O::sp_ind) {  // EX (SP),rr writes the slot with the register
      const auto reg = form.src == O::ix ? z80::Reg::ix : form.src == O::iy ? z80::Reg::iy : z80::Reg::hl;
      stores.push_back({s.sp, 2U, read(s, reg), true});
      return;
    }
    (void)imm;
  }

  Image image_;
  bool bank_volatile_{};
};

State reset_state() {
  State s;
  s.r[A] = 0xFFU;  // AF = $FFFF
  s.sp = 0xFFFFU;
  s.iff = 1U;
  s.im = 1U;  // IM 0
  return s;
}

struct ImageResult {
  std::set<Reason> reasons;
  std::set<std::uint32_t> control_bytes;  // physical Z80 RAM bytes: reachable instruction bytes and read return slots
  std::set<std::uint32_t> work_ram;       // physical work-RAM offsets the Z80 may write
  std::size_t reachable{}, store_sites{}, window_store_sites{}, bank_store_sites{}, interrupt_entries{};
};

void classify_store(const Store &store, const State &before, ImageResult &result, std::set<std::uint32_t> &ram_bytes,
                    bool &window, bool &bank) {
  if (!store.address) {
    result.reasons.insert(Reason::store_target_unknown);
    return;
  }
  State latch = before;  // the latch as each byte is written (a word store to the bank register shifts twice)
  for (std::uint32_t i = 0; i < store.width; ++i) {
    const auto address = static_cast<std::uint16_t>(*store.address + i);
    if (address < ram_view_end) {
      ram_bytes.insert(physical(address));
    } else if (address >= bank_first && address < bank_last) {
      bank = true;
    } else if (address >= window_first) {
      window = true;
      // every completion of the unknown latch bits
      const std::uint32_t unknown = ~static_cast<std::uint32_t>(latch.bank_known) & bank_mask;
      for (std::uint32_t sub = unknown;; sub = (sub - 1U) & unknown) {
        const std::uint32_t bank = latch.bank_value | sub;
        if (bank >= bank_z80_area_first && bank < bank_z80_area_last) result.reasons.insert(Reason::window_store_into_z80_area);
        if (bank >= genesis_z80_bank_ram_first) result.work_ram.insert(((bank << 15U) | (address & 0x7FFFU)) & 0xFFFFU);
        if (sub == 0U) break;
      }
    }
    Store one{address, 1U, store.value ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(*store.value >> (8U * i)))
                                       : std::nullopt, false};
    apply_stores(latch, {one});
  }
}

ImageResult analyse_image(std::span<const std::uint8_t> bytes, bool bank_volatile) {
  ImageResult result;
  const Analyzer analyzer{bytes, bank_volatile};
  std::map<std::uint16_t, State> states;
  std::set<std::uint16_t> worklist;
  const auto arrive = [&](std::uint16_t pc, const State &state) {
    const auto [found, inserted] = states.emplace(pc, state);
    if (!inserted) {
      auto joined = join(found->second, state);
      if (joined == found->second) return;
      found->second = std::move(joined);
    }
    worklist.insert(pc);
  };
  arrive(0U, reset_state());
  std::size_t iterations = 0U;
  while (!worklist.empty()) {
    if (++iterations > iteration_bound) {
      result.reasons.insert(Reason::analysis_bound);
      return result;
    }
    const auto pc = *worklist.begin();
    worklist.erase(worklist.begin());
    const auto &state = states.at(pc);
    if (state.iff & 2U)
      for (auto &[target, next] : analyzer.interrupt(pc, state).successors) arrive(target, next);
    for (auto &[target, next] : analyzer.step(pc, states.at(pc)).successors) arrive(target, next);
  }

  // Final pass over the fixed point: reasons, store classification, control bytes.
  std::set<std::uint32_t> ram_store_bytes;
  for (const auto &[pc, state] : states) {
    std::vector<Transfer> transfers;
    if (state.iff & 2U) {
      transfers.push_back(analyzer.interrupt(pc, state));
      ++result.interrupt_entries;
    }
    transfers.push_back(analyzer.step(pc, state));
    State effective = state;
    if (bank_volatile) effective.bank_known = effective.bank_value = 0U;
    for (std::size_t t = 0; t < transfers.size(); ++t) {
      const auto &transfer = transfers[t];
      result.reasons.insert(transfer.reasons.begin(), transfer.reasons.end());
      if (transfer.decoded) {
        ++result.reachable;
        for (std::uint32_t i = 0; i < transfer.length; ++i) result.control_bytes.insert(physical(pc + i));
      }
      for (const auto slot : transfer.slot_reads) {
        result.control_bytes.insert(physical(slot));
        result.control_bytes.insert(physical(slot + 1U));
      }
      if (transfer.stores.empty()) continue;
      ++result.store_sites;
      bool window = false, bank = false;
      State latch = effective;
      for (const auto &store : transfer.stores) {
        classify_store(store, latch, result, ram_store_bytes, window, bank);
        apply_stores(latch, {store});
      }
      result.window_store_sites += window ? 1U : 0U;
      result.bank_store_sites += bank ? 1U : 0U;
    }
  }
  // A Z80 store into its own reachable instruction bytes invalidates the decoding (return slots are modelled by the slot map).
  std::set<std::uint32_t> code;
  for (const auto &[pc, state] : states) {
    const auto start = z80::decode_at(Image{bytes}, pc);
    if (start.kind != z80::StartKind::decoded) continue;
    for (std::uint32_t i = 0; i < z80::logical_length(start.instruction); ++i) code.insert(physical(pc + i));
  }
  for (const auto byte : ram_store_bytes)
    if (code.contains(byte)) result.reasons.insert(Reason::self_modifying_store);
  return result;
}

void add_bytes(std::set<std::uint32_t> &out, std::uint32_t first, std::uint32_t last, std::uint32_t lo, std::uint32_t hi) {
  const auto a = std::max(first, lo), b = std::min(last, hi);
  if (a >= b) return;
  if (b - a >= ram_size) {
    for (std::uint32_t p = 0; p < ram_size; ++p) out.insert(p);
    return;
  }
  for (std::uint32_t address = a; address < b; ++address) out.insert(physical(address));
}

}  // namespace

const char *genesis_z80_ram_writes_name(GenesisZ80RamWrites writes) noexcept {
  switch (writes) {
  case GenesisZ80RamWrites::none: return "none";
  case GenesisZ80RamWrites::ranges: return "ranges";
  case GenesisZ80RamWrites::all: return "all";
  }
  return "invalid";
}

const char *genesis_z80_proof_reason_name(GenesisZ80ProofReason reason) noexcept {
  switch (reason) {
  case Reason::image_set_unknown: return "image_set_unknown";
  case Reason::image_invalid: return "image_invalid";
  case Reason::undecodable_code: return "undecodable_code";
  case Reason::store_target_unknown: return "store_target_unknown";
  case Reason::stack_pointer_unknown: return "stack_pointer_unknown";
  case Reason::indirect_control: return "indirect_control";
  case Reason::return_unbounded: return "return_unbounded";
  case Reason::interrupt_mode_unbounded: return "interrupt_mode_unbounded";
  case Reason::self_modifying_store: return "self_modifying_store";
  case Reason::window_store_into_z80_area: return "window_store_into_z80_area";
  case Reason::m68k_store_into_z80_code: return "m68k_store_into_z80_code";
  case Reason::m68k_store_into_z80_ram_unbounded: return "m68k_store_into_z80_ram_unbounded";
  case Reason::analysis_bound: return "analysis_bound";
  case Reason::proof_not_stable: return "proof_not_stable";
  }
  return "invalid";
}

std::optional<std::vector<M68kAsyncRange>> GenesisZ80RamWriteProof::bound() const {
  if (outcome == GenesisZ80RamWrites::all) return std::nullopt;
  return work_ram;
}

GenesisZ80RamWriteProof prove_genesis_z80_ram_writes(const std::optional<std::vector<GenesisZ80Image>> &images,
                                                     const std::vector<GenesisZ80AreaStores> &m68k_stores) {
  GenesisZ80RamWriteProof proof;
  // The 68K side: bank-latch writers and Z80 RAM bytes written while the Z80 may run.
  std::set<std::uint32_t> running_ram_bytes;
  for (const auto &group : m68k_stores) {
    const bool running = !group.held_in_reset;
    proof.m68k_unknown_target_stores += group.unknown_target_stores;
    if (group.unknown_target_stores != 0U) {
      proof.bank_volatile = true;
      if (running) proof.reasons.insert(Reason::m68k_store_into_z80_ram_unbounded);
    }
    for (const auto &[first, last] : group.ranges) {
      if (first >= last) continue;
      ++proof.m68k_known_ranges;
      if (first < m68k_bank_last && m68k_bank_first < last) proof.bank_volatile = true;
      if (!running) continue;
      add_bytes(running_ram_bytes, first, last, m68k_ram_first, m68k_ram_last);
      add_bytes(running_ram_bytes, first, last, m68k_upper_first, m68k_upper_last);
    }
  }
  if (!images) {
    proof.reasons.insert(Reason::image_set_unknown);
  } else {
    std::set<std::uint32_t> work_ram;
    for (const auto &image : *images) {
      proof.image_hashes.push_back(sha256_hex(image.bytes));
      if (image.bytes.empty() || image.bytes.size() > ram_size) {
        proof.reasons.insert(Reason::image_invalid);
        continue;
      }
      const auto result = analyse_image(image.bytes, proof.bank_volatile);
      proof.reasons.insert(result.reasons.begin(), result.reasons.end());
      for (const auto byte : running_ram_bytes)
        if (result.control_bytes.contains(byte)) proof.reasons.insert(Reason::m68k_store_into_z80_code);
      work_ram.insert(result.work_ram.begin(), result.work_ram.end());
      proof.reachable_instructions += result.reachable;
      proof.store_sites += result.store_sites;
      proof.window_store_sites += result.window_store_sites;
      proof.bank_register_store_sites += result.bank_store_sites;
      proof.interrupt_entries += result.interrupt_entries;
    }
    if (images->empty()) proof.reasons.insert(Reason::image_set_unknown);  // an empty set is not a statically known image set
    for (const auto offset : work_ram) {
      if (!proof.work_ram.empty() && proof.work_ram.back().hi == offset) ++proof.work_ram.back().hi;
      else proof.work_ram.push_back({M68kRegionKind::mutable_ram, 0U, offset, offset + 1U});
    }
  }
  if (!proof.reasons.empty()) {
    proof.outcome = GenesisZ80RamWrites::all;
    proof.work_ram.clear();
  } else {
    proof.outcome = proof.work_ram.empty() ? GenesisZ80RamWrites::none : GenesisZ80RamWrites::ranges;
  }
  return proof;
}

std::string format_genesis_z80_ram_write_proof(const GenesisZ80RamWriteProof &proof, const char *credited_bound) {
  std::string out = "{\"outcome\":\"" + std::string(genesis_z80_ram_writes_name(proof.outcome)) + "\",\"reasons\":[";
  bool first = true;
  for (const auto reason : proof.reasons) {
    out += (first ? "\"" : ",\"") + std::string(genesis_z80_proof_reason_name(reason)) + "\"";
    first = false;
  }
  std::size_t bytes = 0U;
  for (const auto &range : proof.work_ram) bytes += range.hi - range.lo;
  out += "],\"credited_bound\":\"" + std::string(credited_bound) + "\",\"images\":" + std::to_string(proof.image_hashes.size()) +
         ",\"work_ram_ranges\":" + std::to_string(proof.work_ram.size()) + ",\"work_ram_bytes\":" + std::to_string(bytes) +
         ",\"reachable_instructions\":" + std::to_string(proof.reachable_instructions) +
         ",\"store_sites\":" + std::to_string(proof.store_sites) + ",\"window_store_sites\":" + std::to_string(proof.window_store_sites) +
         ",\"bank_register_store_sites\":" + std::to_string(proof.bank_register_store_sites) +
         ",\"interrupt_entries\":" + std::to_string(proof.interrupt_entries) +
         ",\"m68k_z80_area\":{\"known_ranges\":" + std::to_string(proof.m68k_known_ranges) +
         ",\"unknown_target_stores\":" + std::to_string(proof.m68k_unknown_target_stores) +
         ",\"bank_volatile\":" + (proof.bank_volatile ? "true" : "false") + "}}";
  return out;
}

}  // namespace segarecomp
