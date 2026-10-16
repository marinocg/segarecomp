// SEG-068-T008 (ADR 0135): D0 static Z80 code-write discovery. See the header. No guest instruction is executed.
#include "z80_static_mutation.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <set>
#include <unordered_map>

#include "segarecomp/cpu/z80/decode.hpp"
#include "segarecomp/cpu/z80/effects.hpp"

namespace segarecomp::experiments::z80_mutation {
namespace {

using namespace segarecomp::cpu::z80;

enum Slot : unsigned { sA, sB, sC, sD, sE, sH, sL, sIX, sIY, sSP, kSlots };

// Cause of an unknown value (attribution only; never changes a decision): 0 entry, 1 call return, 2 memory load, 3 opaque ALU result,
// 4 widened, 5 cardinality overflow, 100+n unsupported effect of Mnemonic n.
enum : std::uint16_t { kCauseEntry = 0, kCauseCall = 1, kCauseLoad = 2, kCauseOpaque = 3, kCauseWidened = 4, kCauseOverflow = 5, kCauseUnsupported = 100 };
struct Val {
  bool top{true};
  std::vector<std::uint16_t> v;  // sorted unique when !top
  std::uint16_t cause{kCauseEntry};
};
struct State {
  std::array<Val, kSlots> r;
};

std::string cause_name(std::uint16_t c);
Val top_val(std::uint16_t cause = kCauseEntry) { Val t; t.cause = cause; return t; }
Val singleton(std::uint16_t x) { Val t; t.top = false; t.v = {x}; return t; }

class Analyzer {
 public:
  Analyzer(const std::vector<std::uint8_t>& ram, const Limits& limits) : ram_(ram), lim_(limits) {
    table_ = make_table(ram_);
    classes_ = classify_all(table_);
  }

  Report run(const std::vector<Root>& roots);

 private:
  static TableFetch make_table(const std::vector<std::uint8_t>& ram) {
    TableFetch t;
    for (std::uint32_t a = 0; a < kCodeWindow; ++a)
      t.set(static_cast<std::uint16_t>(a), {FetchKind::byte, ram[a & (kRamBytes - 1)], 1, static_cast<std::uint32_t>(a & (kRamBytes - 1))});
    return t;
  }

  Val normalize(std::vector<std::uint16_t> v) const {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() > lim_.max_values) return top_val(kCauseOverflow);
    Val t;
    t.top = false;
    t.v = std::move(v);
    return t;
  }
  static unsigned slot_of(Reg r) {
    switch (r) {
      case Reg::a: return sA; case Reg::b: return sB; case Reg::c: return sC; case Reg::d: return sD; case Reg::e: return sE;
      case Reg::h: return sH; case Reg::l: return sL; case Reg::ix: return sIX; case Reg::iy: return sIY; case Reg::sp: return sSP;
      default: return kSlots;
    }
  }
  static bool pair_of(Reg r, unsigned& hi, unsigned& lo) {
    if (r == Reg::bc) { hi = sB; lo = sC; return true; }
    if (r == Reg::de) { hi = sD; lo = sE; return true; }
    if (r == Reg::hl) { hi = sH; lo = sL; return true; }
    return false;
  }
  Val read_reg(const State& s, Reg r) const {
    unsigned hi, lo;
    if (pair_of(r, hi, lo)) {
      const Val &h = s.r[hi], &l = s.r[lo];
      if (h.top) return h;
      if (l.top) return l;
      if (h.v.size() * l.v.size() > lim_.max_values) return top_val(kCauseOverflow);
      std::vector<std::uint16_t> out;
      for (auto x : h.v)
        for (auto y : l.v) out.push_back(static_cast<std::uint16_t>((x << 8) | y));
      return normalize(std::move(out));
    }
    return s.r[slot_of(r)];
  }
  void write_reg(State& s, Reg r, const Val& v) const {
    unsigned hi, lo;
    if (pair_of(r, hi, lo)) {
      if (v.top) { s.r[hi] = v; s.r[lo] = v; return; }
      std::vector<std::uint16_t> h, l;
      for (auto x : v.v) { h.push_back(static_cast<std::uint16_t>(x >> 8)); l.push_back(static_cast<std::uint16_t>(x & 0xFF)); }
      s.r[hi] = normalize(h);
      s.r[lo] = normalize(l);
      return;
    }
    s.r[slot_of(r)] = v;
  }
  Val eval(const ValueExpr& e, const State& s, std::uint16_t mask) const {
    switch (e.kind) {
      case ValueExpr::Kind::constant: return singleton(static_cast<std::uint16_t>(e.constant & mask));
      case ValueExpr::Kind::copy: {
        Val v = read_reg(s, e.source);
        if (v.top) return v;
        for (auto& x : v.v) x = static_cast<std::uint16_t>(x & mask);
        return normalize(v.v);
      }
      case ValueExpr::Kind::add_constant: {
        Val v = read_reg(s, e.source);
        if (v.top) return v;
        for (auto& x : v.v) x = static_cast<std::uint16_t>((static_cast<std::int32_t>(x) + e.delta) & mask);
        return normalize(v.v);
      }
      case ValueExpr::Kind::load: return top_val(kCauseLoad);  // memory is not tracked: never guessed
      case ValueExpr::Kind::opaque: return top_val(kCauseOpaque);
    }
    return top_val();
  }
  Val eval_address(const AddressExpr& a, const State& s) const {
    if (a.kind == AddressExpr::Kind::absolute) return singleton(a.absolute);
    Val v = read_reg(s, a.base);
    if (v.top) return v;
    for (auto& x : v.v) x = static_cast<std::uint16_t>((static_cast<std::int32_t>(x) + a.offset) & 0xFFFF);
    return normalize(v.v);
  }
  static State all_top(std::uint16_t cause = kCauseEntry) {
    State s;
    for (Val& v : s.r) v.cause = cause;
    return s;
  }
  static State reset_state() {
    // Architectural reset defines only AF and SP (= $FFFF); every other register is undefined on hardware and stays unknown here even
    // though the project's own runtime happens to initialise them: a static certificate must not depend on that choice.
    State s;
    s.r[sA] = singleton(0xFF);
    s.r[sSP] = singleton(0xFFFF);
    return s;
  }
  bool join(State& dst, const State& src) const {  // returns true when dst grew
    bool changed = false;
    for (unsigned i = 0; i < kSlots; ++i) {
      Val& d = dst.r[i];
      const Val& x = src.r[i];
      if (d.top) continue;
      if (x.top) { d = x; changed = true; continue; }
      std::vector<std::uint16_t> merged = d.v;
      merged.insert(merged.end(), x.v.begin(), x.v.end());
      Val nv = normalize(std::move(merged));
      if (nv.top != d.top || nv.v != d.v) { d = std::move(nv); changed = true; }
    }
    return changed;
  }
  State transfer(const State& in, const Z80Effect& e) const {
    State out = in;
    std::vector<Val> values;
    for (const RegisterWrite& w : e.writes) values.push_back(eval(w.value, in, is_wide(w.target) ? 0xFFFF : 0xFF));
    for (std::size_t i = 0; i < e.writes.size(); ++i) write_reg(out, e.writes[i].target, values[i]);
    return out;
  }

  void enqueue(std::uint16_t pc, const State& s) {
    auto it = states_.find(pc);
    if (it == states_.end()) {
      if (states_.size() >= lim_.max_starts) { budget_ = true; return; }
      states_.emplace(pc, s);
      queue_.push_back(pc);
      return;
    }
    if (!join(it->second, s)) return;
    if (++revisits_[pc] > lim_.max_revisits) it->second = all_top(kCauseWidened);  // widening: sound, absorbing
    queue_.push_back(pc);
  }

  void fixpoint() {
    while (!queue_.empty()) {
      if (++steps_ > lim_.max_steps) { budget_ = true; return; }
      const std::uint16_t pc = queue_.front();
      queue_.pop_front();
      const State in = states_.at(pc);
      const StartClassification& cls = classes_[pc];
      if (cls.kind != StartKind::decoded) continue;
      const Z80Effect eff = project_effect(cls.instruction);
      const std::uint16_t next = static_cast<std::uint16_t>(pc + logical_length(cls.instruction));
      if (!eff.supported) {
        const FormDescriptor& d = form_descriptor(cls.instruction.form);
        switch (d.mnemonic) {
          case Mnemonic::ret: case Mnemonic::reti: case Mnemonic::retn: break;           // no static successor
          case Mnemonic::jp: case Mnemonic::jr: case Mnemonic::djnz: case Mnemonic::call: case Mnemonic::rst: break;  // unresolved control
          default: enqueue(next, all_top(static_cast<std::uint16_t>(kCauseUnsupported + static_cast<unsigned>(d.mnemonic)))); break;  // successor is the next start; every register becomes unknown
        }
        continue;
      }
      const State out = transfer(in, eff);
      switch (eff.control) {
        case ControlKind::fallthrough: enqueue(eff.fallthrough, out); break;
        case ControlKind::jump: enqueue(eff.target, out); break;
        case ControlKind::conditional_jump: enqueue(eff.target, out); enqueue(eff.fallthrough, out); break;
        case ControlKind::call: enqueue(eff.target, out); enqueue(eff.fallthrough, all_top(kCauseCall)); break;  // no summary
        case ControlKind::return_: break;
        case ControlKind::conditional_return: enqueue(eff.fallthrough, out); break;
        case ControlKind::computed: {
          const Val base = read_reg(in, eff.computed_base);
          if (!base.top)
            for (auto t : base.v) enqueue(t, out);
          break;
        }
      }
    }
  }

  bool ei_reachable() const {
    for (const auto& [pc, st] : states_) {
      (void)st;
      const StartClassification& c = classes_[pc];
      if (c.kind == StartKind::decoded && form_descriptor(c.instruction.form).mnemonic == Mnemonic::ei) return true;
    }
    return false;
  }
  Val sp_union() const {
    std::vector<std::uint16_t> all;
    for (const auto& [pc, st] : states_) {
      (void)pc;
      if (st.r[sSP].top) return top_val();
      all.insert(all.end(), st.r[sSP].v.begin(), st.r[sSP].v.end());
    }
    return normalize(std::move(all));
  }

  std::vector<StartChange> changes_for(const std::vector<std::uint8_t>& modified, const std::set<std::uint16_t>& reach,
                                       unsigned& total, unsigned& structural, unsigned& reach_structural) const {
    const TableFetch t = make_table(modified);
    const std::vector<StartClassification> mod = classify_all(t);
    std::vector<StartChange> out;
    total = structural = reach_structural = 0;
    for (std::uint32_t q = 0; q < kCodeWindow; ++q) {
      const StartClassification &o = classes_[q], &m = mod[q];
      if (o == m) continue;
      if (q >= kRamBytes && reach.count(static_cast<std::uint16_t>(q)) == 0) continue;  // second mirror: same bytes, counted once
      StartChange c;
      c.start = static_cast<std::uint16_t>(q);
      c.reachable_start = reach.count(c.start) != 0;
      const bool od = o.kind == StartKind::decoded, md = m.kind == StartKind::decoded;
      c.original = od ? form_name(o.instruction.form) : "undecodable";
      c.modified = md ? form_name(m.instruction.form) : "undecodable";
      c.original_length = od ? logical_length(o.instruction) : 0;
      c.modified_length = md ? logical_length(m.instruction) : 0;
      if (od && md) {
        if (c.original_length != c.modified_length) c.kind = ChangeKind::length_changed;
        else if (o.instruction.form != m.instruction.form) c.kind = ChangeKind::form_changed;
        else c.kind = ChangeKind::payload_only;
      } else {
        c.kind = od ? ChangeKind::becomes_undecodable : ChangeKind::becomes_decodable;
      }
      ++total;
      if (c.kind != ChangeKind::payload_only) {
        ++structural;
        if (c.reachable_start) ++reach_structural;
      }
      if (out.size() < 64) out.push_back(std::move(c));
    }
    return out;
  }

  std::vector<std::uint8_t> ram_;
  Limits lim_;
  TableFetch table_;
  std::vector<StartClassification> classes_;
  std::unordered_map<std::uint16_t, State> states_;
  std::unordered_map<std::uint16_t, unsigned> revisits_;
  std::deque<std::uint16_t> queue_;
  unsigned steps_{};
  bool budget_{};
};

Report Analyzer::run(const std::vector<Root>& roots) {
  Report rep;
  for (const Root& r : roots) enqueue(r.pc, r.reset_state ? reset_state() : all_top());
  fixpoint();
  // The INT vector is a root only when interrupts can be enabled (an EI is reachable). Its entry SP is the interrupted SP minus the
  // hardware push; its other registers are unknown. Iterate until the SP union is stable (bounded).
  bool ei = false;
  for (int round = 0; round < 4 && !budget_; ++round) {
    if (!ei_reachable()) break;
    ei = true;
    const Val sp = sp_union();
    State h = all_top();
    if (!sp.top) {
      std::vector<std::uint16_t> v;
      for (auto x : sp.v) v.push_back(static_cast<std::uint16_t>(x - 2));
      h.r[sSP] = normalize(std::move(v));
    }
    const auto sp_before = sp_union();
    enqueue(0x0038, h);
    fixpoint();
    const auto sp_after = sp_union();
    if (sp_after.top == sp_before.top && sp_after.v == sp_before.v) break;
  }

  // ---- post pass over the reachable set (ascending, deterministic) ----
  std::vector<std::uint16_t> pcs;
  for (const auto& [pc, st] : states_) { (void)st; pcs.push_back(pc); }
  std::sort(pcs.begin(), pcs.end());
  rep.reachable_starts = static_cast<unsigned>(pcs.size());
  rep.steps = steps_;
  rep.budget_exceeded = budget_;
  if (budget_) rep.incomplete_reasons["analysis_budget_exceeded"]++;

  std::set<std::uint16_t> reach(pcs.begin(), pcs.end());
  std::set<std::uint16_t> covered;  // physical code bytes covered by a reachable decoded instruction
  const auto note_reason = [&](const std::string& why, bool reach_flag, bool effect_flag) {
    rep.incomplete_reasons[why]++;
    if (reach_flag) rep.reachability_complete = false;
    if (effect_flag) rep.effects_complete = false;
  };
  for (std::uint16_t pc : pcs) {
    const StartClassification& cls = classes_[pc];
    if (cls.kind != StartKind::decoded) {
      note_reason(cls.kind == StartKind::prefix_lock ? "prefix_lock"
                  : cls.kind == StartKind::unresolved_fetch_mapping ? "unresolved_fetch_mapping" : "fetch_outside_code_window",
                  true, true);
      continue;
    }
    for (unsigned i = 0; i < logical_length(cls.instruction); ++i) covered.insert(static_cast<std::uint16_t>((pc + i) & (kRamBytes - 1)));
  }
  for (std::uint16_t pc : pcs) {
    const StartClassification& cls = classes_[pc];
    if (cls.kind != StartKind::decoded) continue;
    const Z80Effect eff = project_effect(cls.instruction);
    const FormDescriptor& d = form_descriptor(cls.instruction.form);
    const State& in = states_.at(pc);
    if (!eff.supported) {
      std::string m = mnemonic_name(d.mnemonic);
      std::transform(m.begin(), m.end(), m.begin(), [](char c) { return static_cast<char>(c | 0x20); });
      switch (d.mnemonic) {
        case Mnemonic::ret: case Mnemonic::reti: case Mnemonic::retn: note_reason("unsupported_effect:" + m, false, true); break;
        case Mnemonic::jp: case Mnemonic::jr: case Mnemonic::djnz: case Mnemonic::call: case Mnemonic::rst:
          note_reason("unsupported_control:" + m, true, true);
          break;
        default: note_reason("unsupported_effect:" + m, false, true); break;
      }
      if (d.mnemonic == Mnemonic::im && d.src == Operand::im2) note_reason("interrupt_vector_unresolved_im2", true, false);
      continue;
    }
    if (eff.control == ControlKind::computed && read_reg(in, eff.computed_base).top) note_reason("unresolved_indirect_jump", true, false);
    if (!eff.store) continue;
    StoreSite site;
    site.pc = pc;
    site.width = eff.store->width;
    const Val addr = eval_address(eff.store->address, in);
    const Val value = eval(eff.store->value, in, eff.store->width == 2 ? 0xFFFF : 0xFF);
    site.address = addr.top ? Certainty::unknown : addr.v.size() == 1 ? Certainty::exact : Certainty::finite;
    site.value = value.top ? Certainty::unknown : value.v.size() == 1 ? Certainty::exact : Certainty::finite;
    if (!addr.top) site.addresses = addr.v;
    if (!value.top) site.values = value.v;
    if (addr.top) {
      note_reason("store_address_unknown", false, false);
      rep.incomplete_reasons["  address_cause:" + cause_name(addr.cause)]++;
      rep.store_addresses_complete = false;
      ++rep.unknown_address_stores;
      site.hits_reachable_code = true;  // may
    } else {
      for (auto a : site.addresses)
        for (unsigned b = 0; b < site.width; ++b) {
          const std::uint32_t dest = (a + b) & 0xFFFF;
          if (dest < kCodeWindow && covered.count(static_cast<std::uint16_t>(dest & (kRamBytes - 1)))) site.hits_reachable_code = true;
        }
    }
    if (value.top) {
      ++rep.unknown_value_stores;
      rep.incomplete_reasons["  value_cause:" + cause_name(value.cause)]++;
      if (site.hits_reachable_code) { note_reason("store_value_unknown_may_hit_code", false, false); rep.store_values_complete = false; }
    }
    rep.stores.push_back(site);
  }

  // Hardware interrupt pushes (only when interrupts can be enabled).
  if (ei) {
    note_reason("interrupt_register_effects_unverified", false, true);
    const Val sp = sp_union();
    StoreSite site;
    site.hardware_push = true;
    site.width = 2;
    site.value = Certainty::unknown;
    if (sp.top) {
      site.address = Certainty::unknown;
      note_reason("hardware_push_sp_unknown", false, false);
      rep.store_addresses_complete = false;
      ++rep.unknown_address_stores;
      site.hits_reachable_code = true;
    } else {
      std::set<std::uint16_t> dests;
      for (auto x : sp.v) dests.insert(static_cast<std::uint16_t>(x - 2));
      site.addresses.assign(dests.begin(), dests.end());
      site.address = site.addresses.size() == 1 ? Certainty::exact : Certainty::finite;
      for (auto a : site.addresses)
        for (unsigned b = 0; b < 2; ++b) {
          const std::uint32_t dest = (a + b) & 0xFFFF;
          if (dest < kCodeWindow && covered.count(static_cast<std::uint16_t>(dest & (kRamBytes - 1)))) site.hits_reachable_code = true;
        }
    }
    ++rep.unknown_value_stores;
    if (site.hits_reachable_code) { note_reason("hardware_push_may_hit_code", false, false); rep.store_values_complete = false; }
    rep.stores.push_back(site);
  }

  // Candidates: finite address x finite value pairs whose destination lies in reachable code.
  constexpr std::size_t kCandidateCap = 4096;
  for (const StoreSite& s : rep.stores) {
    if (s.hardware_push || s.address == Certainty::unknown || s.value == Certainty::unknown || !s.hits_reachable_code) continue;
    for (auto a : s.addresses)
      for (auto v : s.values) {
        bool hit = false;
        for (unsigned b = 0; b < s.width; ++b) {
          const std::uint32_t dest = (a + b) & 0xFFFF;
          if (dest < kCodeWindow && covered.count(static_cast<std::uint16_t>(dest & (kRamBytes - 1)))) hit = true;
        }
        if (!hit) continue;
        if (rep.candidates.size() >= kCandidateCap) { rep.budget_exceeded = true; rep.incomplete_reasons["candidate_cap_exceeded"]++; continue; }
        std::vector<std::uint8_t> mod = ram_;
        for (unsigned b = 0; b < s.width; ++b) {
          const std::uint32_t dest = (a + b) & 0xFFFF;
          if (dest < kCodeWindow) mod[dest & (kRamBytes - 1)] = static_cast<std::uint8_t>(v >> (8 * b));
        }
        Candidate c;
        c.site_pc = s.pc;
        c.destination = a;
        c.value = v;
        c.width = s.width;
        c.changes = changes_for(mod, reach, c.changed_starts, c.structural_changed_starts, c.reachable_structural_changed_starts);
        rep.candidates.push_back(std::move(c));
      }
  }
  return rep;
}

std::string cause_name(std::uint16_t c) {
  switch (c) {
    case kCauseEntry: return "unknown_entry_or_reset_state";
    case kCauseCall: return "call_return_continuation";
    case kCauseLoad: return "memory_load";
    case kCauseOpaque: return "alu_result_not_modelled";
    case kCauseWidened: return "join_widened";
    case kCauseOverflow: return "cardinality_bound";
    default: break;
  }
  std::string m = mnemonic_name(static_cast<Mnemonic>(c - kCauseUnsupported));
  std::transform(m.begin(), m.end(), m.begin(), [](char ch) { return static_cast<char>(ch | 0x20); });
  return "unsupported_effect:" + m;
}

}  // namespace

const char* certainty_name(Certainty c) {
  switch (c) { case Certainty::exact: return "exact"; case Certainty::finite: return "finite"; case Certainty::unknown: return "unknown"; }
  return "?";
}
const char* change_kind_name(ChangeKind k) {
  switch (k) {
    case ChangeKind::payload_only: return "payload_only";
    case ChangeKind::form_changed: return "form_changed";
    case ChangeKind::length_changed: return "length_changed";
    case ChangeKind::becomes_undecodable: return "becomes_undecodable";
    case ChangeKind::becomes_decodable: return "becomes_decodable";
  }
  return "?";
}
bool Candidate::structural() const { return structural_changed_starts != 0; }

std::vector<Root> hardware_roots() { return {{0x0000, true}}; }

Report analyze(const std::vector<std::uint8_t>& ram, const std::vector<Root>& roots, const Limits& limits) {
  if (ram.size() != kRamBytes) {
    Report r;
    r.reachability_complete = false;
    r.incomplete_reasons["bad_image_size"]++;
    return r;
  }
  return Analyzer(ram, limits).run(roots);
}

std::string format_report(const Report& r, bool detail) {
  std::string out;
  char b[256];
  const auto line = [&](const char* fmt, auto... args) { std::snprintf(b, sizeof b, fmt, args...); out += b; };
  unsigned exact_addr = 0, finite_addr = 0, unk_addr = 0, exact_val = 0, finite_val = 0, unk_val = 0, hit = 0, hw = 0;
  for (const StoreSite& s : r.stores) {
    if (s.hardware_push) { ++hw; continue; }
    (s.address == Certainty::exact ? exact_addr : s.address == Certainty::finite ? finite_addr : unk_addr)++;
    (s.value == Certainty::exact ? exact_val : s.value == Certainty::finite ? finite_val : unk_val)++;
    if (s.hits_reachable_code) ++hit;
  }
  unsigned structural = 0, reach_structural = 0, payload_only = 0;
  for (const Candidate& c : r.candidates) {
    if (c.structural()) ++structural; else ++payload_only;
    if (c.reachable_structural_changed_starts) ++reach_structural;
  }
  line("reachable_starts %u\nsteps %u\n", r.reachable_starts, r.steps);
  line("reachability_complete %d\neffects_complete %d\nstore_addresses_complete %d\nstore_values_complete %d\nbudget_exceeded %d\ncandidate_set_complete %d\nclosed_under_mutation %d\n",
       r.reachability_complete, r.effects_complete, r.store_addresses_complete, r.store_values_complete, r.budget_exceeded, r.candidate_set_complete(), r.closed_under_mutation());
  line("instruction_stores %u\n  address exact %u finite %u unknown %u\n  value exact %u finite %u unknown %u\n  may_hit_reachable_code %u\n",
       static_cast<unsigned>(r.stores.size()) - hw, exact_addr, finite_addr, unk_addr, exact_val, finite_val, unk_val, hit);
  line("hardware_push_sites %u\ncandidates %u\n  structural %u (of which changing a reachable start %u)\n  payload_only %u\n", hw,
       static_cast<unsigned>(r.candidates.size()), structural, reach_structural, payload_only);
  for (const auto& [why, n] : r.incomplete_reasons) line("reason %s %u\n", why.c_str(), n);
  if (detail) {  // PRIVATE local diagnosis only: addresses and values
    for (const Candidate& c : r.candidates) {
      line("candidate site=%04x dest=%04x value=%0*x width=%u changed=%u structural=%u\n", c.site_pc, c.destination,
           c.width == 2 ? 4 : 2, c.value, c.width, c.changed_starts, c.structural_changed_starts);
      for (const StartChange& ch : c.changes)
        line("  start=%04x %s %s/%u -> %s/%u reachable=%d\n", ch.start, change_kind_name(ch.kind), ch.original.c_str(),
             ch.original_length, ch.modified.c_str(), ch.modified_length, ch.reachable_start);
    }
  }
  return out;
}

}  // namespace segarecomp::experiments::z80_mutation
