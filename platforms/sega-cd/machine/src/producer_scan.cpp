// SEG-112 (ADR 0256): see producer_scan.hpp.
#include "segarecomp/machine/sega_cd/producer_scan.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <set>
#include <tuple>
#include <span>
#include <sstream>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp::machine::sega_cd {

const char *producer_refusal_name(ProducerRefusal refusal) noexcept {
  switch (refusal) {
    case ProducerRefusal::loop_operand_unknown: return "loop_operand_unknown";
    case ProducerRefusal::loop_body_unsupported: return "loop_body_unsupported";
    case ProducerRefusal::destination_unsupported: return "destination_unsupported";
    case ProducerRefusal::source_not_immutable: return "source_not_immutable";
    case ProducerRefusal::too_large: return "too_large";
    case ProducerRefusal::aliasing: return "aliasing";
    case ProducerRefusal::source_written: return "source_written";
    case ProducerRefusal::destination_overwritten: return "destination_overwritten";
    case ProducerRefusal::not_executed: return "not_executed";
    case ProducerRefusal::overlapping_fragments: return "overlapping_fragments";
    case ProducerRefusal::too_many_fragments: return "too_many_fragments";
    case ProducerRefusal::budget_exhausted: return "budget_exhausted";
  }
  return "unknown";
}

namespace {

using Width = M68kMemoryAccessWidth;

// A 32-bit value with the mask of the bits the walk knows (MOVE.W #imm,Dn defines only the low half).
struct Reg {
  std::uint32_t v{};
  std::uint32_t mask{};
  [[nodiscard]] bool full() const { return mask == 0xFFFFFFFFU; }
  [[nodiscard]] bool has(std::uint32_t bits) const { return (mask & bits) == bits; }
  bool operator==(const Reg &) const = default;
};
Reg K(std::uint32_t v) { return Reg{v, 0xFFFFFFFFU}; }
Reg meet(const Reg &x, const Reg &y) {
  const std::uint32_t m = x.mask & y.mask & ~(x.v ^ y.v);
  return Reg{x.v & m, m};
}

struct St {
  std::array<Reg, 8> d{}, a{};
  std::uint32_t made{};   // candidates whose producer loop has run on EVERY path that reaches here (a bit per candidate): executable use must follow its producer
  bool operator==(const St &) const = default;
};

St join(const St &x, const St &y) {
  St out;
  for (std::size_t i = 0; i < 8; ++i) {
    out.d[i] = meet(x.d[i], y.d[i]);
    out.a[i] = meet(x.a[i], y.a[i]);
  }
  out.made = x.made & y.made;
  return out;
}

std::uint32_t nbytes(Width w) { return w == Width::byte ? 1U : w == Width::word ? 2U : 4U; }
std::uint32_t wmask(Width w) { return w == Width::byte ? 0xFFU : w == Width::word ? 0xFFFFU : 0xFFFFFFFFU; }
std::uint32_t sext(std::uint32_t v, Width w) {
  if (w == Width::byte) return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(v)));
  if (w == Width::word) return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v)));
  return v;
}

struct Range {
  std::uint32_t lo{}, hi{};   // [lo, hi)
  [[nodiscard]] bool overlaps(const Range &o) const { return lo < o.hi && o.lo < hi; }
};

// One recognised loop whose operands are constants on the walked path.
struct Candidate {
  Processor processor{};
  std::uint32_t head_pc{};
  Range source, destination;
  bool copy{};
  std::string transform;
  std::vector<std::uint8_t> bytes;
  bool executed{};
  std::uint32_t entry{};
  std::optional<ProducerRefusal> refused;
};

class Walker {
 public:
  Walker(Processor processor, std::uint32_t base, std::vector<std::uint8_t> bytes, std::uint32_t entry, std::uint32_t disc_address, ProducerScan &out)
      : processor_(processor), out_(out), disc_address_(disc_address), image_base_(base), entry_(entry), image_(std::move(bytes)) {}

  void run() {
    seed(entry_, St{});
    while (!work_.empty()) {
      if (walker_steps_ >= kProducerMaxSteps || states_.size() > kProducerMaxPcs) {
        out_.complete = false;
        ++out_.refusals[ProducerRefusal::budget_exhausted];
        break;
      }
      const std::uint32_t pc = work_.front();
      work_.pop_front();
      queued_.erase(pc);
      ++out_.steps;
      ++walker_steps_;
      step(pc, states_.at(pc));
    }
    finish();
  }

 private:
  // ---- the code the walk may decode: the boot image and the proven bytes of produced fragments ------------------------------
  std::span<const std::uint8_t> code_at(std::uint32_t pc) const {
    if (pc >= image_base_ && pc - image_base_ < image_.size()) return std::span<const std::uint8_t>(image_).subspan(pc - image_base_);
    for (const Candidate &c : cands_)
      if (!c.refused && pc >= c.destination.lo && pc < c.destination.hi) return std::span<const std::uint8_t>(c.bytes).subspan(pc - c.destination.lo);
    return {};
  }

  std::optional<M68kIrOperation> decode(std::uint32_t pc) const {
    const auto bytes = code_at(pc);
    if (bytes.empty() || (pc & 1U) != 0U) return std::nullopt;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{0}};
    const auto result = decode_m68k_instruction(bytes, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) return std::nullopt;
    auto op = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && op.kind == M68kIrKind::write_moveq) return std::nullopt;
    return op;
  }

  void seed(std::uint32_t pc, const St &s) {
    auto it = states_.find(pc);
    if (it == states_.end()) {
      states_.emplace(pc, s);
    } else {
      const St joined = join(it->second, s);
      if (joined == it->second) return;
      it->second = joined;
    }
    if (queued_.insert(pc).second) work_.push_back(pc);
  }

  // ---- operand evaluation ------------------------------------------------------------------------------------------------
  std::optional<std::uint32_t> value_of(const St &s, const M68kEffectiveAddress &ea, Width w) const {
    switch (ea.mode) {
      case M68kEaMode::immediate: return ea.immediate_value & wmask(w);
      case M68kEaMode::data_register: return s.d[ea.reg & 7U].has(wmask(w)) ? std::optional<std::uint32_t>(s.d[ea.reg & 7U].v & wmask(w)) : std::nullopt;
      case M68kEaMode::address_register: return s.a[ea.reg & 7U].has(wmask(w)) ? std::optional<std::uint32_t>(s.a[ea.reg & 7U].v & wmask(w)) : std::nullopt;
      default: return std::nullopt;
    }
  }

  // Main Work RAM is mirrored through $E00000-$FFFFFF: one cell, one address.
  std::uint32_t norm(std::uint32_t a) const {
    a &= 0x00FFFFFFU;
    return processor_ == Processor::main && a >= 0x00E00000U ? (0x00FF0000U | (a & 0xFFFFU)) : a;
  }

  // The address a memory operand denotes when the walk knows it (no side effect applied).
  std::optional<std::uint32_t> address_of(const St &s, const M68kEffectiveAddress &ea, std::uint32_t size = 2U) const {
    switch (ea.mode) {
      case M68kEaMode::absolute_word:
      case M68kEaMode::absolute_long: return norm(m68k_data_access_ea_address(ea));
      case M68kEaMode::pc_disp16: return norm(ea.absolute_address);
      case M68kEaMode::address_indirect:
      case M68kEaMode::address_postinc: return s.a[ea.reg & 7U].full() ? std::optional<std::uint32_t>(norm(s.a[ea.reg & 7U].v)) : std::nullopt;
      case M68kEaMode::address_predec: return s.a[ea.reg & 7U].full() ? std::optional<std::uint32_t>(norm(s.a[ea.reg & 7U].v - (ea.reg == 7U ? 2U : size))) : std::nullopt;
      case M68kEaMode::address_disp16:
        return s.a[ea.reg & 7U].full() ? std::optional<std::uint32_t>(norm(s.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)))) : std::nullopt;
      default: return std::nullopt;   // indexed forms: not tracked
    }
  }

  static bool is_memory(const M68kEffectiveAddress &ea) {
    return ea.mode != M68kEaMode::unused && ea.mode != M68kEaMode::data_register && ea.mode != M68kEaMode::address_register && ea.mode != M68kEaMode::immediate;
  }

  // ---- one instruction ---------------------------------------------------------------------------------------------------
  void step(std::uint32_t pc, const St &entry_state) {
    St s = entry_state;
    cur_pc_ = pc;
    transfers_[pc].clear();
    if (try_loop(pc, s)) return;
    const auto op = decode(pc);
    if (!op) return;   // not decodable here (data, outside the walked code): the path ends
    const std::uint32_t next = pc + static_cast<std::uint32_t>(op->raw_bytes.size());
    note_writes(s, *op);
    const auto control = m68k_control_successors(*op);
    apply(s, *op);
    // constant control transfers: executable use of a produced range, and the successors
    if (op->kind == M68kIrKind::jump_general || op->kind == M68kIrKind::call_general) {
      std::optional<std::uint32_t> target;
      const auto &ea = op->source_ea;
      if (ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::absolute_word) target = norm(m68k_data_access_ea_address(ea));
      else if (ea.mode == M68kEaMode::address_indirect && entry_state.a[ea.reg & 7U].full()) target = norm(entry_state.a[ea.reg & 7U].v);
      if (target) {
        note_transfer(*target, entry_state.made);
        if (code_at(*target).size() != 0U) seed(*target, op->kind == M68kIrKind::call_general ? entry_state : s);
      }
      if (op->kind == M68kIrKind::call_general) seed(next, St{});   // after a call every register is unknown
      return;
    }
    if (op->kind == M68kIrKind::bsr_call) {
      for (const auto &succ : control.successors)
        if (succ.kind == M68kControlSuccessorKind::call_target) { note_transfer(norm(succ.target), s.made); seed(norm(succ.target), s); }
      seed(next, St{});
      return;
    }
    if (control.dynamic != M68kDynamicControlFamily::none || control.always_raises_exception) return;   // rts / rte / computed: the path ends
    for (const auto &succ : control.successors) {
      if (succ.kind == M68kControlSuccessorKind::fallthrough || succ.kind == M68kControlSuccessorKind::conditional_fallthrough) seed(succ.target & 0x00FFFFFFU, s);
      else if (succ.kind == M68kControlSuccessorKind::branch_target || succ.kind == M68kControlSuccessorKind::conditional_target) {
        note_transfer(norm(succ.target), s.made);
        seed(norm(succ.target), s);
      }
    }
  }

  // Constant control transfers are recorded per instruction (the last evaluation, i.e. the one under the final joined state wins); whether a transfer is
  // executable use of a produced range is decided at the fixed point (finish), from the `made` bits that survive every join: a path that skips the
  // producer clears the bit, so use that does not follow the producer on every path is never proof.
  void note_transfer(std::uint32_t target, std::uint32_t) { transfers_[cur_pc_].push_back(target); }

  // Stores whose destination the walk knows are facts against a proof; stores through a pointer it does not know are counted.
  void note_writes(const St &s, const M68kIrOperation &op) {
    const auto &ea = op.destination_ea;
    if (!is_memory(ea)) return;
    switch (op.kind) {
      case M68kIrKind::compare: case M68kIrKind::compare_immediate: case M68kIrKind::compare_address: case M68kIrKind::test_operand: case M68kIrKind::bit_test:
      case M68kIrKind::jump_general: case M68kIrKind::call_general: case M68kIrKind::load_effective_address: case M68kIrKind::push_effective_address:
        return;
      default: break;
    }
    std::uint32_t bytes = nbytes(op.size);
    if (op.kind == M68kIrKind::movem_transfer) {
      if (op.movem_direction != M68kMovemDirection::registers_to_memory) return;
      std::uint32_t n = 0;
      for (unsigned bit = 0; bit < 16; ++bit) n += (op.movem_register_mask >> bit) & 1U;
      bytes = n * nbytes(op.size);
    }
    if (const auto at = address_of(s, ea, bytes)) known_writes_.push_back({*at, *at + bytes});
    else if (!(ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_postinc || ea.mode == M68kEaMode::address_indirect) || (ea.reg & 7U) != 7U) ++out_.unknown_writes;
  }

  void apply(St &s, const M68kIrOperation &op) {
    const auto effect = m68k_operation_effect(op);
    const Width w = op.size;
    St before = s;
    const bool no_register_write =
        op.kind == M68kIrKind::compare || op.kind == M68kIrKind::compare_immediate || op.kind == M68kIrKind::compare_address || op.kind == M68kIrKind::test_operand ||
        op.kind == M68kIrKind::bit_test || op.kind == M68kIrKind::no_operation || op.kind == M68kIrKind::general_branch || op.kind == M68kIrKind::branch_ne_short ||
        op.kind == M68kIrKind::branch_always_short || op.kind == M68kIrKind::bsr_call || op.kind == M68kIrKind::jump_general || op.kind == M68kIrKind::call_general ||
        op.kind == M68kIrKind::return_from_subroutine || op.kind == M68kIrKind::write_moveq || op.kind == M68kIrKind::load_effective_address;   // LEA clears its own destination below (its footprint is not advertised)
    if (no_register_write) {
      // registers survive (written explicitly below where modelled)
    } else if (!effect.register_write_footprint_complete) {
      s = St{};
      s.made = before.made;
    } else {
      for (unsigned r = 0; r < 8; ++r) {
        if ((effect.data_register_write_mask >> r) & 1U) s.d[r] = Reg{};
        if (r < 7 && ((effect.address_register_write_mask >> r) & 1U)) s.a[r] = Reg{};
      }
    }
    s.a[7] = Reg{};
    switch (op.kind) {
      case M68kIrKind::write_moveq:
        s.d[static_cast<unsigned>(op.destination)] = K(static_cast<std::uint32_t>(static_cast<std::int32_t>(op.operand)));
        return;
      case M68kIrKind::write_move:
        if (op.destination_ea.mode == M68kEaMode::data_register) {
          const auto v = value_of(before, op.source_ea, w);
          const unsigned r = op.destination_ea.reg & 7U;
          if (v) s.d[r] = Reg{(before.d[r].v & ~wmask(w)) | *v, before.d[r].mask | wmask(w)};
        }
        return;
      case M68kIrKind::write_movea: {
        const auto v = value_of(before, op.source_ea, w);
        const unsigned r = op.destination_ea.reg & 7U;
        if (v && r != 7U) s.a[r] = K(sext(*v, w));
        return;
      }
      case M68kIrKind::load_effective_address: {
        const unsigned r = op.destination_ea.reg & 7U;
        const auto &ea = op.source_ea;
        if (r == 7U) return;
        s.a[r] = Reg{};   // unknown unless a form below defines it (never the previous constant)
        if (ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16) s.a[r] = K(ea.absolute_address);
        else if (ea.mode == M68kEaMode::address_indirect && before.a[ea.reg & 7U].full()) s.a[r] = before.a[ea.reg & 7U];
        else if (ea.mode == M68kEaMode::address_disp16 && before.a[ea.reg & 7U].full())
          s.a[r] = K(before.a[ea.reg & 7U].v + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)));
        return;
      }
      case M68kIrKind::add_quick: case M68kIrKind::subtract_quick: case M68kIrKind::add_immediate: case M68kIrKind::subtract_immediate: {
        const auto &ea = op.destination_ea;
        const bool quick = op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::subtract_quick;
        const std::uint32_t amount = quick ? (op.source_ea.mode == M68kEaMode::immediate ? op.source_ea.immediate_value : static_cast<std::uint32_t>(op.operand)) : op.source_ea.immediate_value;
        const bool add = op.kind == M68kIrKind::add_quick || op.kind == M68kIrKind::add_immediate;
        if ((quick && op.source_ea.mode != M68kEaMode::immediate) || (!quick && op.source_ea.mode != M68kEaMode::immediate)) return;
        if (ea.mode == M68kEaMode::address_register && before.a[ea.reg & 7U].full() && (ea.reg & 7U) != 7U)
          s.a[ea.reg & 7U] = K(add ? before.a[ea.reg & 7U].v + amount : before.a[ea.reg & 7U].v - amount);
        else if (ea.mode == M68kEaMode::data_register && before.d[ea.reg & 7U].full() && w == Width::long_word)
          s.d[ea.reg & 7U] = K(add ? before.d[ea.reg & 7U].v + amount : before.d[ea.reg & 7U].v - amount);
        return;
      }
      default: return;
    }
  }

  // ---- the counted loop idiom --------------------------------------------------------------------------------------------
  // Symbolic value of a data register inside one iteration: unknown, a constant, or a function of the loaded element.
  struct Sym {
    enum class Kind { unknown, constant, element } kind{Kind::unknown};
    std::uint32_t value{};
    std::vector<std::pair<char, std::uint8_t>> ops;   // 'x' eor, '+' add, '-' sub; applied to the element, byte wide
  };

  bool try_loop(std::uint32_t pc, St &s) {
    // body = instructions pc .. DBF, whose taken target is pc
    std::vector<M68kIrOperation> body;
    std::uint32_t at = pc;
    std::optional<M68kIrOperation> dbra;
    for (std::uint32_t i = 0; i <= kProducerMaxLoopBody; ++i) {
      const auto op = decode(at);
      if (!op) return false;
      if (op->kind == M68kIrKind::dbcc_loop) {
        const auto control = m68k_control_successors(*op);
        const bool back = std::any_of(control.successors.begin(), control.successors.end(),
                                      [&](const M68kControlSuccessor &c) { return c.kind == M68kControlSuccessorKind::conditional_target && (c.target & 0x00FFFFFFU) == pc; });
        if (!back) return false;
        dbra = *op;
        at += static_cast<std::uint32_t>(op->raw_bytes.size());
        break;
      }
      body.push_back(*op);
      at += static_cast<std::uint32_t>(op->raw_bytes.size());
    }
    if (!dbra || body.empty() || dbra->condition != M68kCondition::never) return false;
    ++out_.candidates;
    Candidate c;
    c.processor = processor_;
    c.head_pc = pc;
    const auto refuse = [&](ProducerRefusal r) { ++out_.refusals[r]; return false; };

    // symbolic iteration
    std::array<Sym, 8> sym{};
    std::optional<unsigned> src_reg, dst_reg;
    std::optional<Width> element;
    bool stored = false;
    for (const auto &op : body) {
      const Width w = op.size;
      if (op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::address_postinc && op.destination_ea.mode == M68kEaMode::address_postinc) {
        if (src_reg || stored) return refuse(ProducerRefusal::loop_body_unsupported);
        src_reg = op.source_ea.reg & 7U; dst_reg = op.destination_ea.reg & 7U; element = w; element_ = w; stored = true; c.copy = true;
        continue;
      }
      if (op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::address_postinc && op.destination_ea.mode == M68kEaMode::data_register && w == Width::byte) {
        if (src_reg) return refuse(ProducerRefusal::loop_body_unsupported);
        src_reg = op.source_ea.reg & 7U; element = w;
        sym[op.destination_ea.reg & 7U] = Sym{Sym::Kind::element, 0, {}};
        continue;
      }
      if ((op.kind == M68kIrKind::exclusive_or_immediate || op.kind == M68kIrKind::add_immediate || op.kind == M68kIrKind::subtract_immediate) &&
          op.destination_ea.mode == M68kEaMode::data_register && w == Width::byte && op.source_ea.mode == M68kEaMode::immediate) {
        Sym &t = sym[op.destination_ea.reg & 7U];
        if (t.kind != Sym::Kind::element) return refuse(ProducerRefusal::loop_body_unsupported);
        t.ops.push_back({op.kind == M68kIrKind::exclusive_or_immediate ? 'x' : op.kind == M68kIrKind::add_immediate ? '+' : '-', static_cast<std::uint8_t>(op.source_ea.immediate_value & 0xFFU)});
        continue;
      }
      if (op.kind == M68kIrKind::write_move && op.source_ea.mode == M68kEaMode::data_register && op.destination_ea.mode == M68kEaMode::address_postinc && w == Width::byte &&
          sym[op.source_ea.reg & 7U].kind == Sym::Kind::element) {
        if (stored) return refuse(ProducerRefusal::loop_body_unsupported);
        dst_reg = op.destination_ea.reg & 7U; stored = true;
        c.transform = describe(sym[op.source_ea.reg & 7U]);
        c.copy = c.transform.empty();
        continue;
      }
      return refuse(ProducerRefusal::loop_body_unsupported);
    }
    if (!src_reg || !dst_reg || !stored || *src_reg == *dst_reg || *src_reg == 7U || *dst_reg == 7U) return refuse(ProducerRefusal::loop_body_unsupported);
    const unsigned counter = dbra->destination_ea.reg & 7U;
    if (sym[counter].kind != Sym::Kind::unknown) return refuse(ProducerRefusal::loop_body_unsupported);   // the body rewrites the loop counter: the count is not the entry value
    const auto &rs = s.a[*src_reg];
    const auto &rd = s.a[*dst_reg];
    const auto &rc = s.d[counter];
    if (!rs.full() || !rd.full() || !rc.has(0xFFFFU)) return refuse(ProducerRefusal::loop_operand_unknown);
    const std::uint32_t count = (rc.v & 0xFFFFU) + 1U;
    const std::uint32_t total = count * nbytes(*element);
    const std::uint32_t src = norm(rs.v), dst = norm(rd.v);
    c.source = {src, src + total};
    c.destination = {dst, dst + total};
    // post-state of the summarised loop (always applied: the path continues after the loop with these values)
    St post = s;
    post.a[*src_reg] = K(rs.v + total);
    post.a[*dst_reg] = K(rd.v + total);
    post.d[counter] = Reg{(rc.v & 0xFFFF0000U) | 0xFFFFU, rc.mask};
    for (unsigned r = 0; r < 8; ++r) if (sym[r].kind != Sym::Kind::unknown && r != counter) post.d[r] = Reg{};
    post.a[7] = Reg{};
    // one candidate per loop head: a head re-evaluated with other operands (a later-joining path) makes the earlier proof unstable
    std::size_t index = cands_.size();
    for (std::size_t i = 0; i < cands_.size(); ++i) {
      if (cands_[i].head_pc != pc) continue;
      if (cands_[i].source.lo == c.source.lo && cands_[i].source.hi == c.source.hi && cands_[i].destination.lo == c.destination.lo && cands_[i].destination.hi == c.destination.hi && cands_[i].transform == c.transform) { index = i; break; }
      if (!cands_[i].refused) cands_[i].refused = ProducerRefusal::loop_operand_unknown;
    }
    if (index == cands_.size()) {
      if (index >= 32U) { ++out_.refusals[ProducerRefusal::too_many_fragments]; seed(at, post); return true; }
      if (!classify(c)) c.refused = last_refusal_;
      cands_.push_back(std::move(c));
    }
    if (!cands_[index].refused) post.made |= (1U << index);
    seed(at, post);
    return true;
  }

  static std::string describe(const Sym &t) {
    std::ostringstream out;
    for (const auto &[kind, k] : t.ops) out << (out.tellp() > 0 ? "," : "") << (kind == 'x' ? "eor" : kind == '+' ? "add" : "sub") << ".b:" << static_cast<unsigned>(k);
    return out.str();
  }

  // Destination, source and bounds; evaluates the produced bytes. A failed check is a counted refusal.
  bool classify(Candidate &c) {
    const auto refuse = [&](ProducerRefusal r) { last_refusal_ = r; return false; };   // counted when the candidate is finished
    const std::uint32_t length = c.destination.hi - c.destination.lo;
    const bool main_cpu = processor_ == Processor::main;
    const bool ram = main_cpu ? (c.destination.lo >= kMainWorkRamBegin && c.destination.hi <= kMainWorkRamEnd) : (c.destination.lo >= image_base_ && c.destination.hi <= kPrgRamBytes);   // sub: at or above the SP image (below it is BIOS-resident)
    if (!ram || (c.destination.lo & 1U) != 0U || length < 4U) return refuse(ProducerRefusal::destination_unsupported);
    if (c.source.lo < image_base_ || c.source.hi > image_base_ + image_.size()) return refuse(ProducerRefusal::source_not_immutable);
    if (element_ != Width::byte && (c.source.lo & 1U) != 0U) return refuse(ProducerRefusal::destination_unsupported);   // a word/long access at an odd address is an address error on hardware
    if (length > kProducerMaxBytes) return refuse(ProducerRefusal::too_large);
    if (c.source.overlaps(c.destination)) return refuse(ProducerRefusal::aliasing);
    if (c.destination.overlaps(Range{image_base_, image_base_ + static_cast<std::uint32_t>(image_.size())})) return refuse(ProducerRefusal::aliasing);
    c.bytes.assign(image_.begin() + (c.source.lo - image_base_), image_.begin() + (c.source.hi - image_base_));
    if (!c.copy) apply_transform(c);
    if ((length & 1U) != 0U) c.bytes.pop_back();   // an odd count leaves one byte past the last whole word: instructions are word-aligned, the claim is the even prefix (the byte stays in `destination` for the overwrite checks)
    return true;
  }

  void apply_transform(Candidate &c) const {
    std::vector<std::pair<char, std::uint8_t>> ops;
    std::istringstream in(c.transform);
    for (std::string item; std::getline(in, item, ',');) {
      const auto colon = item.find(':');
      ops.push_back({item[0] == 'e' ? 'x' : item[0] == 'a' ? '+' : '-', static_cast<std::uint8_t>(std::stoul(item.substr(colon + 1)))});
    }
    for (auto &b : c.bytes)
      for (const auto &[kind, k] : ops) b = static_cast<std::uint8_t>(kind == 'x' ? (b ^ k) : kind == '+' ? (b + k) : (b - k));
  }

  void finish() {
    // every known write of the walk against every proof
    for (const auto &[at, targets] : transfers_) {
      const auto state = states_.find(at);
      if (state == states_.end()) continue;
      for (const std::uint32_t target : targets)
        for (std::size_t i = 0; i < cands_.size() && i < 32U; ++i)
          if (!cands_[i].refused && !cands_[i].executed && ((state->second.made >> i) & 1U) != 0U && target >= cands_[i].destination.lo && target < cands_[i].destination.hi) { cands_[i].executed = true; cands_[i].entry = target; }
    }
    const Range image{image_base_, image_base_ + static_cast<std::uint32_t>(image_.size())};
    for (Candidate &c : cands_) {
      if (c.refused) continue;
      if (!out_.complete) { c.refused = ProducerRefusal::budget_exhausted; continue; }
      for (const Range &w : known_writes_) {
        if (w.overlaps(image)) { c.refused = ProducerRefusal::source_written; break; }   // a located store into the walked image (self-patching): its operands cannot be trusted
        if (w.overlaps(c.source)) { c.refused = ProducerRefusal::source_written; break; }
        if (w.overlaps(c.destination)) { c.refused = ProducerRefusal::destination_overwritten; break; }
      }
      if (!c.refused && !c.executed) c.refused = ProducerRefusal::not_executed;
    }
    for (std::size_t i = 0; i < cands_.size(); ++i)
      for (std::size_t j = i + 1; j < cands_.size(); ++j)
        if (!cands_[i].refused && !cands_[j].refused && cands_[i].destination.overlaps(cands_[j].destination) &&
            (cands_[i].destination.lo != cands_[j].destination.lo || cands_[i].bytes != cands_[j].bytes))
          cands_[i].refused = cands_[j].refused = ProducerRefusal::overlapping_fragments;
    for (const Candidate &c : cands_) {
      if (c.refused) { ++out_.refusals[*c.refused]; continue; }
      const bool duplicate = std::any_of(out_.fragments.begin(), out_.fragments.end(), [&](const StaticFragment &f) { return f.processor == c.processor && f.base == c.destination.lo && f.bytes == c.bytes; });
      if (duplicate) continue;
      if (out_.fragments.size() >= kProducerMaxFragments) { ++out_.refusals[ProducerRefusal::too_many_fragments]; continue; }
      StaticFragment f;
      f.processor = c.processor;
      f.copy = c.copy;
      f.base = c.destination.lo;
      f.length = static_cast<std::uint32_t>(c.bytes.size());
      f.disc_offset = c.copy ? disc_address_ + (c.source.lo - image_base_) : 0U;
      f.entry = c.entry;
      f.transform = c.transform;
      f.bytes = c.bytes;
      out_.fragments.push_back(std::move(f));
    }
  }

  Processor processor_;
  ProducerScan &out_;
  std::uint32_t disc_address_{};
  std::uint32_t image_base_{}, entry_{};
  std::vector<std::uint8_t> image_;
  std::map<std::uint32_t, St> states_;
  std::deque<std::uint32_t> work_;
  std::set<std::uint32_t> queued_;
  std::vector<Candidate> cands_;
  std::uint32_t walker_steps_{};
  std::uint32_t cur_pc_{};
  std::map<std::uint32_t, std::vector<std::uint32_t>> transfers_;
  Width element_{Width::byte};
  ProducerRefusal last_refusal_{ProducerRefusal::loop_operand_unknown};
  std::vector<Range> known_writes_;
};

}  // namespace

ProducerScan scan_static_producers(const std::vector<std::uint8_t> &system_area, const BootImages &images) {
  ProducerScan out;
  if (system_area.size() >= kMainBootDiscOffset + kMainBootBytes) {
    Walker main_walker(Processor::main, kMainWorkRamBegin, std::vector<std::uint8_t>(system_area.begin() + kMainBootDiscOffset, system_area.begin() + kMainBootDiscOffset + kMainBootBytes),
                       kMainWorkRamBegin, kMainBootDiscOffset, out);
    main_walker.run();
  }
  const auto &sp = images.sub.images.images.front();
  Walker sub_walker(Processor::sub, sp.mappings.front().execution_base, sp.bytes, images.sub.entry_pc, images.header.sp.disc_address, out);
  sub_walker.run();
  std::sort(out.fragments.begin(), out.fragments.end(), [](const StaticFragment &x, const StaticFragment &y) {
    return std::tie(x.processor, x.base, x.length) < std::tie(y.processor, y.base, y.length);
  });
  return out;
}

}  // namespace segarecomp::machine::sega_cd
