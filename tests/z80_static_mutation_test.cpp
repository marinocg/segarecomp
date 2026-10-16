// SEG-068-T008 (ADR 0135): project-authored synthetic fixtures and adversarial soundness cases for the D0 static analyzer.
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "tools/z80_static_mutation.hpp"

namespace zm = segarecomp::experiments::z80_mutation;
using Image = std::vector<std::uint8_t>;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++failures;
}
Image blank() { return Image(zm::kRamBytes, 0x00); }  // zeros are NOP
void put(Image& img, unsigned at, std::initializer_list<unsigned> bytes) {
  for (unsigned b : bytes) img[at++] = static_cast<std::uint8_t>(b);
}
zm::Report run(const Image& img, const zm::Limits& limits = {}) { return zm::analyze(img, zm::hardware_roots(), limits); }
const zm::Candidate* find(const zm::Report& r, unsigned dest, unsigned value) {
  for (const auto& c : r.candidates)
    if (c.destination == dest && c.value == value) return &c;
  return nullptr;
}
const zm::StartChange* change_at(const zm::Candidate& c, unsigned start) {
  for (const auto& ch : c.changes)
    if (ch.start == start) return &ch;
  return nullptr;
}
bool has_reason(const zm::Report& r, const std::string& prefix) {
  for (const auto& [why, n] : r.incomplete_reasons) { (void)n; if (why.rfind(prefix, 0) == 0) return true; }
  return false;
}
}  // namespace

int main() {
  // ---- Fixture A: constant instruction replacement (JR -> RET) ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x10, 0x00, 0x36, 0xC9, 0xC3, 0x10, 0x00});  // LD HL,0010 ; LD (HL),C9 ; JP 0010
    put(i, 0x0010, {0x18, 0xFE});                                       // JR $  (the encoded JR)
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].address == zm::Certainty::exact && r.stores[0].value == zm::Certainty::exact, "A: one store, exact destination, exact value");
    const zm::Candidate* c = find(r, 0x0010, 0xC9);
    check(c && c->structural(), "A: candidate (dest, C9) found and structural");
    const zm::StartChange* ch = c ? change_at(*c, 0x0010) : nullptr;
    check(ch && ch->kind == zm::ChangeKind::length_changed && ch->original_length == 2 && ch->modified_length == 1 && ch->reachable_start,
          "A: original JR (2 bytes) vs modified RET (1 byte) distinguished at a reachable start");
    check(ch && ch->original.find("jr") == 0 && ch->modified.find("ret") == 0, "A: both forms decode through the existing decoder (jr / ret)");
    check(r.candidate_set_complete() && !r.closed_under_mutation(),
          "A: first-order set complete, but NOT closed under mutation (the mutated code may store further)");
  }
  // ---- Reset state: no register is assumed; A/F and SP are never guessed either ----
  {
    Image i = blank();
    put(i, 0x0000, {0x36, 0xC9, 0x18, 0xFE});  // LD (HL),C9 with HL undefined at reset
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].address == zm::Certainty::unknown && r.candidates.empty(), "R1: HL is undefined after reset -> UNKNOWN destination");
  }
  {
    Image i = blank();  // F1 (second review): the value of A and the stack location after reset are not guessed
    put(i, 0x0000, {0x21, 0x04, 0x00, 0x77, 0x18, 0xFE});  // LD HL,4 ; LD (HL),A ; JR $
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].value == zm::Certainty::unknown && !r.candidate_set_complete(), "R2: A is undefined after reset -> UNKNOWN value, incomplete");
    Image j = blank();
    put(j, 0x0000, {0xCD, 0x10, 0x00, 0x18, 0xFE});
    put(j, 0x0010, {0x18, 0xFE});
    const zm::Report q = run(j);
    check(!q.store_addresses_complete && !q.candidate_set_complete(), "R3: the CALL push address depends on an undefined SP -> UNKNOWN, incomplete");
  }
  // ---- Fixture B1: two branches write different constant opcodes to the same location ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0x20, 0x04, 0x36, 0xC9, 0x18, 0x04, 0x36, 0xD9, 0x18, 0x00, 0xC3, 0x20, 0x00});
    put(i, 0x0020, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(r.stores.size() == 2 && find(r, 0x20, 0xC9) && find(r, 0x20, 0xD9) && r.candidates.size() == 2,
          "B1: both branch writes (RET and EXX) reported, neither guessed");
    const zm::Candidate* d9 = find(r, 0x20, 0xD9);
    const zm::StartChange* ch = d9 ? change_at(*d9, 0x20) : nullptr;
    check(ch && ch->modified.find("exx") == 0 && ch->kind == zm::ChangeKind::length_changed, "B1: the EXX alternative decodes (exx, 1 byte)");
    check(r.candidate_set_complete(), "B1: complete");
  }
  // ---- Fixture B2: branch join gives one store a finite value set ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0x3E, 0xC9, 0x20, 0x02, 0x3E, 0xD9, 0x77, 0xC3, 0x20, 0x00});
    put(i, 0x0020, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].value == zm::Certainty::finite && r.stores[0].values.size() == 2 && r.candidates.size() == 2,
          "B2: join of two constants -> finite value set {C9, D9}");
  }
  // ---- Fixture C: unknown value / address / unbounded pointer ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0x3E, 0x05, 0x80, 0x77, 0xC3, 0x20, 0x00});  // ADD A,B makes A opaque ; LD (HL),A
    put(i, 0x0020, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].value == zm::Certainty::unknown && r.candidates.empty(), "C1: opaque value -> UNKNOWN, no guessed candidate");
    check(!r.candidate_set_complete() && has_reason(r, "store_value_unknown_may_hit_code"), "C1: result is incomplete, never presented as complete");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x2A, 0x40, 0x00, 0x36, 0xC9, 0xC3, 0x10, 0x00});  // LD HL,(0040) is a memory load -> unknown ; LD (HL),C9
    put(i, 0x0010, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(r.stores.size() == 1 && r.stores[0].address == zm::Certainty::unknown && r.candidates.empty(), "C2: memory-loaded address -> UNKNOWN");
    check(!r.store_addresses_complete && !r.candidate_set_complete(), "C2: incomplete");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0x36, 0xC9, 0x23, 0x20, 0xFB, 0x18, 0xFE});  // loop: LD (HL),C9 ; INC HL ; JR NZ
    const zm::Report r = run(i);
    check(!r.stores.empty() && r.stores[0].address == zm::Certainty::unknown, "C3: pointer incremented in a loop widens to UNKNOWN (no finite guess)");
  }
  // ---- Fixture D: overlapping and variable-length instructions ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x10, 0x00, 0x36, 0xC9, 0xC3, 0x10, 0x00});
    put(i, 0x0010, {0x21, 0x34, 0x12, 0x18, 0xFE});  // LD HL,1234 (3 bytes)
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x10, 0xC9);
    const zm::StartChange* ch = c ? change_at(*c, 0x10) : nullptr;
    check(ch && ch->kind == zm::ChangeKind::length_changed && ch->original_length == 3 && ch->modified_length == 1, "D1: a longer instruction replaced by a shorter one (3 -> 1)");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x0F, 0x00, 0x36, 0xDD, 0xC3, 0x0F, 0x00});
    put(i, 0x000F, {0x00, 0x21, 0x34, 0x12, 0x18, 0xFE});  // NOP ; LD HL,1234
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x0F, 0xDD);
    const zm::StartChange* ch = c ? change_at(*c, 0x0F) : nullptr;
    check(ch && ch->kind == zm::ChangeKind::length_changed && ch->original_length == 1 && ch->modified_length == 4 &&
              ch->modified.find("ix") != std::string::npos,
          "D2: a written prefix makes the previous NOP absorb the next instruction (1 -> 4, LD IX,nn)");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x12, 0x00, 0x36, 0x55, 0xC3, 0x10, 0x00});  // write the high immediate byte of LD HL,nn
    put(i, 0x0010, {0x21, 0x34, 0x12, 0x18, 0xFE});
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x12, 0x55);
    const zm::StartChange* ch = c ? change_at(*c, 0x10) : nullptr;
    check(c && c->reachable_structural_changed_starts == 0 && ch && ch->kind == zm::ChangeKind::payload_only,
          "D3: operand-only write changes no reachable start structurally (payload_only at the instruction; unreachable overlapping starts may differ)");
  }
  // ---- Fixture E: unsupported semantics -> conservative UNKNOWN ----
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0xC5, 0x36, 0xC9, 0xC3, 0x20, 0x00});  // PUSH BC (unsupported) havocs HL
    put(i, 0x0020, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(has_reason(r, "unsupported_effect:push") && !r.effects_complete, "E1: PUSH is reported unsupported and the result incomplete");
    check(!r.stores.empty() && r.stores[0].address == zm::Certainty::unknown && r.candidates.empty(), "E1: the register state after the unsupported form is UNKNOWN");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x20, 0x00, 0xD9, 0x36, 0xC9, 0xC3, 0x20, 0x00});  // EXX
    put(i, 0x0020, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(has_reason(r, "unsupported_effect:exx") && r.candidates.empty(), "E2: EXX is not modelled: no guessed candidate");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0xC4, 0x20, 0x00, 0x18, 0xFE});  // CALL NZ,0020 : unsupported control
    const zm::Report r = run(i);
    check(has_reason(r, "unsupported_control:call") && !r.reachability_complete, "E3: unsupported control flow makes reachability incomplete");
  }
  // ---- Adversarial soundness ----
  {
    Image i = blank();  // the second RAM mirror aliases the same byte
    put(i, 0x0000, {0x21, 0x10, 0x20, 0x36, 0xC9, 0xC3, 0x10, 0x00});
    put(i, 0x0010, {0x18, 0xFE});
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x2010, 0xC9);
    check(c && c->structural() && change_at(*c, 0x0010), "M1: a write through the mirror hits the reachable instruction it aliases");
  }
  {
    Image i = blank();  // no code-hitting write: closed
    put(i, 0x0000, {0x21, 0x30, 0x00, 0x36, 0xC9, 0x18, 0xFE});
    check(run(i).closed_under_mutation(), "M2: no write to reachable code -> closed under mutation");
  }
  {
    Image i = blank();  // callee write with the caller's constant
    put(i, 0x0000, {0x21, 0x30, 0x00, 0xCD, 0x20, 0x00, 0x18, 0xFE});
    put(i, 0x0020, {0x36, 0xC9, 0xE9});  // LD (HL),C9 ; JP (HL)
    put(i, 0x0030, {0x18, 0xFE});
    const zm::Report r = run(i);
    check(find(r, 0x30, 0xC9) != nullptr, "F1: a write inside a callee uses the proven caller state");
  }
  {
    Image i = blank();  // the CALL return address is a two-byte constant store into executable RAM
    put(i, 0x0000, {0x31, 0x12, 0x00, 0xCD, 0x20, 0x00, 0x18, 0xFE});
    put(i, 0x0010, {0x18, 0xFE});
    put(i, 0x0020, {0xC3, 0x10, 0x00});
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x10, 0x0006);
    check(c && c->width == 2 && c->structural(), "F2: a stack push (return address) into reachable code is a width-2 candidate");
  }
  {
    Image i = blank();  // self-overwrite of the storing instruction
    put(i, 0x0000, {0x21, 0x03, 0x00, 0x36, 0x00, 0x18, 0xFE});
    const zm::Report r = run(i);
    const zm::Candidate* c = find(r, 0x03, 0x00);
    const zm::StartChange* ch = c ? change_at(*c, 0x03) : nullptr;
    check(ch && ch->kind == zm::ChangeKind::length_changed && ch->original_length == 2 && ch->modified_length == 1, "F3: a store that overwrites its own opcode is accounted");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x10, 0x00, 0x36, 0xC9, 0xC3, 0x10, 0x00});
    put(i, 0x0010, {0x18, 0xFE});
    zm::Limits tiny;
    tiny.max_steps = 3;
    const zm::Report r = run(i, tiny);
    check(r.budget_exceeded && !r.candidate_set_complete(), "F4: an exceeded step budget yields an incomplete result");
  }
  {
    Image i = blank();  // EI is an unsupported effect: SP becomes unknown after it
    put(i, 0x0000, {0x31, 0x12, 0x00, 0xFB, 0x18, 0xFE});
    put(i, 0x0010, {0x18, 0xFE});
    const zm::Report r = run(i);
    unsigned hw = 0;
    for (const auto& s : r.stores) hw += s.hardware_push && s.address == zm::Certainty::unknown;
    check(hw == 1 && has_reason(r, "interrupt_register_effects_unverified") && has_reason(r, "hardware_push_sp_unknown") && !r.candidate_set_complete(),
          "F5: EI is unmodelled (havoc), so the hardware push destination is UNKNOWN and the result incomplete");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x2A, 0x40, 0x00, 0xF9, 0xFB, 0x18, 0xFE});  // SP from memory, then EI
    const zm::Report r = run(i);
    check(has_reason(r, "hardware_push_sp_unknown") && !r.store_addresses_complete, "F6: unknown SP with interrupts enabled -> hardware push destination UNKNOWN");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x2A, 0x40, 0x00, 0xE9});  // JP (HL) with an unknown HL
    const zm::Report r = run(i);
    check(has_reason(r, "unresolved_indirect_jump") && !r.reachability_complete, "G1: unresolved indirect jump -> reachability incomplete");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x10, 0x00, 0xE9});  // JP (HL) with a proven HL reaches the target
    put(i, 0x0010, {0x21, 0x20, 0x00, 0x36, 0xC9, 0x18, 0xFE});
    const zm::Report r = run(i);
    check(find(r, 0x20, 0xC9) == nullptr && r.stores.size() == 1 && r.stores[0].address == zm::Certainty::exact,
          "G2: a proven computed jump is followed (store found; dest not code -> no candidate)");
  }
  {
    Image i = blank();  // S1 (independent review): a forged return slot sends RET to code the analysis never followed
    put(i, 0x0000, {0x31, 0x00, 0x01, 0x21, 0x20, 0x00, 0x22, 0x00, 0x01, 0xC9});  // LD SP,0100 ; LD HL,0020 ; LD (0100),HL ; RET
    put(i, 0x0020, {0x3E, 0x00, 0x32, 0x00, 0x00, 0x18, 0x00});                      // LD A,0 ; LD (0000),A ; JR +0
    const zm::Report r = run(i);
    check(!r.reachability_complete && !r.candidate_set_complete() && !r.closed_under_mutation() && has_reason(r, "return_target_unproven"),
          "S1: an unproven RET target makes reachability, completeness and closure false");
  }
  {
    Image i = blank();  // S2 (independent review): an operand-only write retargets a JP to code with another code store
    put(i, 0x0000, {0x3E, 0x20, 0x32, 0x06, 0x00, 0xC3, 0x10, 0x00});  // LD A,20 ; LD (0006),A ; JP 0010 (operand byte patched to 20)
    put(i, 0x0010, {0x18, 0xFE});
    put(i, 0x0020, {0x3E, 0x00, 0x32, 0x00, 0x00, 0x18, 0xFE});
    const zm::Report r = run(i);
    check(find(r, 0x0006, 0x20) != nullptr && r.candidate_set_complete() && !r.closed_under_mutation(),
          "S2: a payload-only change to a reachable start (JP target) is NOT closed under mutation");
  }
  {
    Image i = blank();
    put(i, 0x0000, {0x21, 0x10, 0x00, 0x36, 0xC9, 0xC3, 0x10, 0x00});
    put(i, 0x0010, {0x18, 0xFE});
    check(zm::format_report(run(i), true) == zm::format_report(run(i), true), "H1: the report is deterministic");
    check(!zm::analyze(Image(100), zm::hardware_roots()).candidate_set_complete(), "H2: a wrong-sized image is rejected as incomplete");
  }
  std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
