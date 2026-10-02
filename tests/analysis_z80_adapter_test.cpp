// SEG-029-T004 (ADR 0078, abstract-analysis-core-contract.md section 5): the synthetic Z80 second-CPU adapter instantiates the
// generic solver unchanged. Every program below is project-authored and hand-assembled from the public Zilog UM0080 encoding
// tables; nothing is derived from a commercial image.
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/analysis/solver.hpp"
#include "segarecomp/cpu/z80/analysis/adapter.hpp"
#include "segarecomp/cpu/z80/effects.hpp"

namespace core = segarecomp::analysis;
using segarecomp::cpu::z80::ControlKind;
using segarecomp::cpu::z80::Reg;
using segarecomp::cpu::z80::StartKind;
using segarecomp::cpu::z80::ValueExpr;
using segarecomp::cpu::z80::analysis::Adapter;
using segarecomp::cpu::z80::analysis::ImageView;
using segarecomp::cpu::z80::analysis::State;
using core::FiniteValue;
using core::UnknownReason;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

static_assert(core::Adapter<Adapter>, "the Z80 adapter satisfies the generic seam unchanged");

// A byte image at logical 0x0000: `size` NOP bytes with `code` patched in at the given addresses.
struct Image {
  std::vector<std::uint8_t> bytes;
  Image(std::size_t size, const std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>>& code) : bytes(size, 0x00) {
    for (const auto& [at, run] : code)
      for (std::size_t i = 0; i < run.size(); ++i) bytes.at(at + i) = run[i];
  }
};

struct Run {
  core::Solution<State> solution;
  std::string text;
};

Run run(const Image& image, std::size_t set_bound = core::default_set_bound) {
  const ImageView view(0x0000, image.bytes);
  Adapter adapter(view, set_bound);
  Run out;
  out.solution = core::solve(adapter, {{0x0000, Adapter::entry_state()}});
  out.text = segarecomp::cpu::z80::analysis::describe(out.solution);
  return out;
}

FiniteValue at(const Run& r, std::uint64_t point, Reg reg) {
  return r.solution.query(point, [&](const State& s) { return Adapter::read(s, reg); });
}
FiniteValue set(std::vector<std::uint64_t> values) { return FiniteValue::of(std::move(values)); }
FiniteValue unknown(UnknownReason reason) { return FiniteValue::unknown(reason); }

// Fixture A: constant/finite register values, HL/IX/IY address values, immutable vs RAM loads, a RAM store, an unsupported form.
//   0000 3E 12        LD A,0x12
//   0002 06 34        LD B,0x34
//   0004 0E 56        LD C,0x56          BC composed from its halves: {0x3456}
//   0006 21 00 01     LD HL,0x0100
//   0009 7E           LD A,(HL)          immutable table byte: {0xA0}
//   000A DD 21 02 01  LD IX,0x0102
//   000E DD 5E 01     LD E,(IX+1)        immutable 0x0103: {0xA3}
//   0011 FD 21 00 80  LD IY,0x8000
//   0015 FD 56 00     LD D,(IY+0)        RAM: Unknown(non_immutable_read)
//   0018 3A 00 80     LD A,(0x8000)      RAM: Unknown(non_immutable_read)
//   001B 32 00 80     LD (0x8000),A      store to RAM: accepted, creates no precise memory
//   001E 2A 04 01     LD HL,(0x0104)     immutable little-endian word: {0x1234}
//   0021 23           INC HL             {0x1235}
//   0022 3A 00 80     LD A,(0x8000)      the earlier RAM store did not make the read precise
//   0025 76           HALT               unsupported: no successor, unresolved(unsupported_transfer)
//   0100 A0 A1 A2 A3 34 12               table
void fixture_values() {
  const Image image(0x0106, {{0x0000, {0x3E, 0x12, 0x06, 0x34, 0x0E, 0x56, 0x21, 0x00, 0x01, 0x7E, 0xDD, 0x21, 0x02, 0x01,
                                       0xDD, 0x5E, 0x01, 0xFD, 0x21, 0x00, 0x80, 0xFD, 0x56, 0x00, 0x3A, 0x00, 0x80, 0x32,
                                       0x00, 0x80, 0x2A, 0x04, 0x01, 0x23, 0x3A, 0x00, 0x80, 0x76}},
                             {0x0100, {0xA0, 0xA1, 0xA2, 0xA3, 0x34, 0x12}}});
  const Run r = run(image);
  CHECK(r.solution.complete);
  CHECK(at(r, 0x0000, Reg::a) == unknown(UnknownReason::unknown_input));
  CHECK(at(r, 0x0006, Reg::a) == set({0x12}));
  CHECK(at(r, 0x0006, Reg::b) == set({0x34}));
  CHECK(at(r, 0x0006, Reg::c) == set({0x56}));
  CHECK(at(r, 0x0006, Reg::bc) == set({0x3456}));
  CHECK(at(r, 0x0004, Reg::bc) == unknown(UnknownReason::unknown_input));  // C still unknown after LD B,n
  CHECK(at(r, 0x0009, Reg::hl) == set({0x0100}));
  CHECK(at(r, 0x0009, Reg::h) == set({0x01}));
  CHECK(at(r, 0x0009, Reg::l) == set({0x00}));
  CHECK(at(r, 0x000A, Reg::a) == set({0xA0}));
  CHECK(at(r, 0x0011, Reg::ix) == set({0x0102}));
  CHECK(at(r, 0x0011, Reg::e) == set({0xA3}));
  CHECK(at(r, 0x0011, Reg::de) == unknown(UnknownReason::unknown_input));
  CHECK(at(r, 0x0015, Reg::iy) == set({0x8000}));
  CHECK(at(r, 0x0018, Reg::d) == unknown(UnknownReason::non_immutable_read));
  CHECK(at(r, 0x0018, Reg::de) == unknown(UnknownReason::non_immutable_read));
  CHECK(at(r, 0x001B, Reg::a) == unknown(UnknownReason::non_immutable_read));
  CHECK(r.solution.reached(0x001E));
  CHECK(at(r, 0x0021, Reg::hl) == set({0x1234}));
  CHECK(at(r, 0x0021, Reg::h) == set({0x12}));
  CHECK(at(r, 0x0021, Reg::l) == set({0x34}));
  CHECK(at(r, 0x0022, Reg::hl) == set({0x1235}));
  CHECK(at(r, 0x0025, Reg::a) == unknown(UnknownReason::non_immutable_read));
  CHECK(at(r, 0x0025, Reg::bc) == set({0x3456}));
  // The unsupported form invents no successor and is reported with its typed reason.
  CHECK(r.solution.unresolved_computed.size() == 1U);
  CHECK(r.solution.unresolved_computed.count(0x0025) == 1U &&
        r.solution.unresolved_computed.at(0x0025) == UnknownReason::unsupported_transfer);
  CHECK(!r.solution.reached(0x0026));
  CHECK(at(r, 0x0026, Reg::a).is_bottom());
  CHECK(r.solution.computed_targets.empty());
}

// Fixture B: a conditional branch join and a register-derived indirect jump resolved to exactly the joined constants.
//   0000 20 05        JR NZ,+5 -> 0007   (flags not modelled: both edges)
//   0002 21 00 02     LD HL,0x0200
//   0005 18 03        JR +3 -> 000A
//   0007 21 10 03     LD HL,0x0310
//   000A E9           JP (HL)            computed {0x0200,0x0310} (the pair keeps identity: not the 4-element product)
//   0200 3E 01 C9     LD A,1 ; RET
//   0310 3E 02 C9     LD A,2 ; RET
void fixture_join_and_computed() {
  const Image image(0x0313, {{0x0000, {0x20, 0x05, 0x21, 0x00, 0x02, 0x18, 0x03, 0x21, 0x10, 0x03, 0xE9}},
                             {0x0200, {0x3E, 0x01, 0xC9}},
                             {0x0310, {0x3E, 0x02, 0xC9}}});
  const Run r = run(image);
  CHECK(r.solution.complete);
  CHECK(r.solution.reached(0x0002) && r.solution.reached(0x0007));
  CHECK(at(r, 0x000A, Reg::hl) == set({0x0200, 0x0310}));
  CHECK(at(r, 0x000A, Reg::h) == set({0x02, 0x03}));
  CHECK(at(r, 0x000A, Reg::l) == set({0x00, 0x10}));
  CHECK(r.solution.computed_targets.size() == 1U);
  CHECK(r.solution.computed_targets.count(0x000A) == 1U &&
        r.solution.computed_targets.at(0x000A) == (std::set<std::uint64_t>{0x0200, 0x0310}));
  CHECK(r.solution.unresolved_computed.empty());
  CHECK(at(r, 0x0202, Reg::a) == set({0x01}));
  CHECK(at(r, 0x0312, Reg::a) == set({0x02}));
  CHECK(!r.solution.reached(0x000B));   // JP (HL) has no fallthrough
  CHECK(!r.solution.reached(0x0203));   // RET has no static successor
}

// Fixture C: a table-driven computed jump, an unresolved computed jump and a bounded DJNZ loop.
//   0000 28 05        JR Z,+5 -> 0007
//   0002 2A 20 00     LD HL,(0x0020)      immutable word 0x0030
//   0005 18 03        JR +3 -> 000A
//   0007 2A 22 00     LD HL,(0x0022)      immutable word 0x0038
//   000A E9           JP (HL)             computed {0x0030,0x0038}
//   0020 30 00 38 00                      pointer table
//   0030 06 03        LD B,3
//   0032 10 FE        DJNZ -2 -> 0032     B: 3,2,1,0,0xFF,... (no flag/zero refinement: every byte value)
//   0034 C9           RET
//   0038 2A 00 80     LD HL,(0x8000)      RAM word: Unknown(non_immutable_read)
//   003B E9           JP (HL)             unresolved(non_immutable_read)
const Image& table_image() {
  static const Image image(0x0040, {{0x0000, {0x28, 0x05, 0x2A, 0x20, 0x00, 0x18, 0x03, 0x2A, 0x22, 0x00, 0xE9}},
                                    {0x0020, {0x30, 0x00, 0x38, 0x00}},
                                    {0x0030, {0x06, 0x03, 0x10, 0xFE, 0xC9}},
                                    {0x0038, {0x2A, 0x00, 0x80, 0xE9}}});
  return image;
}

void fixture_table_dispatch() {
  const Run r = run(table_image());
  CHECK(r.solution.complete);
  CHECK(at(r, 0x000A, Reg::hl) == set({0x0030, 0x0038}));
  CHECK(r.solution.computed_targets.count(0x000A) == 1U &&
        r.solution.computed_targets.at(0x000A) == (std::set<std::uint64_t>{0x0030, 0x0038}));
  CHECK(at(r, 0x003B, Reg::hl) == unknown(UnknownReason::non_immutable_read));
  CHECK(r.solution.unresolved_computed.size() == 1U);
  CHECK(r.solution.unresolved_computed.count(0x003B) == 1U &&
        r.solution.unresolved_computed.at(0x003B) == UnknownReason::non_immutable_read);
  CHECK(r.solution.computed_targets.count(0x003B) == 0U);
  // The DJNZ loop: B decrements modulo 256 and, with no flag refinement, covers every byte value.
  const FiniteValue loop_b = at(r, 0x0032, Reg::b);
  CHECK(loop_b.is_precise() && loop_b.values().size() == 256U);
  CHECK(at(r, 0x0034, Reg::b) == loop_b);

  // With an adapter set bound of 16 the decrement's exact image overflows: Unknown(set_bound), never a wider guess.
  const Run small = run(table_image(), 16U);
  CHECK(small.solution.complete);
  CHECK(at(small, 0x0034, Reg::b) == unknown(UnknownReason::set_bound));
  CHECK(at(small, 0x000A, Reg::hl) == set({0x0030, 0x0038}));

  // Solver bounds are reported as typed Unknown for every query, never partial truth.
  const ImageView view(0x0000, table_image().bytes);
  Adapter adapter(view);
  core::Bounds bounds;
  bounds.max_iterations = 3U;
  const auto truncated = core::solve(adapter, {{0x0000, Adapter::entry_state()}}, bounds);
  CHECK(!truncated.complete);
  CHECK(truncated.query(0x0000, [](const State& s) { return Adapter::read(s, Reg::a); }) ==
        unknown(UnknownReason::iteration_bound));
}

// Fixture D: call/return edges, ALU opaque values and a store contradicting the immutability premise.
//   0000 31 00 90     LD SP,0x9000
//   0003 CD 10 00     CALL 0x0010        call edge (SP 0x8FFE); continuation is a return edge with an all-Unknown state
//   0006 3E 05        LD A,5
//   0008 32 40 00     LD (0x0040),A      a precise store into the immutable image: unresolved(unsupported_transfer)
//   000B 00           NOP                (not reached)
//   0010 3E 07        LD A,7
//   0012 C0           RET NZ             fallthrough only (the taken return has no static successor)
//   0013 C6 01        ADD A,1            A := Unknown(unsupported_transfer)
//   0015 FE 01        CP 1               writes no register
//   0017 DD 77 05     LD (IX+5),A        IX unknown: accepted under the caller's immutability premise
//   001A C9           RET
void fixture_call_alu_store() {
  const Image image(0x0041, {{0x0000, {0x31, 0x00, 0x90, 0xCD, 0x10, 0x00, 0x3E, 0x05, 0x32, 0x40, 0x00, 0x00}},
                             {0x0010, {0x3E, 0x07, 0xC0, 0xC6, 0x01, 0xFE, 0x01, 0xDD, 0x77, 0x05, 0xC9}}});
  const Run r = run(image);
  CHECK(r.solution.complete);
  CHECK(at(r, 0x0010, Reg::sp) == set({0x8FFE}));
  CHECK(at(r, 0x0012, Reg::a) == set({0x07}));
  CHECK(at(r, 0x0013, Reg::a) == set({0x07}));
  CHECK(at(r, 0x0015, Reg::a) == unknown(UnknownReason::unsupported_transfer));
  CHECK(at(r, 0x0017, Reg::a) == unknown(UnknownReason::unsupported_transfer));
  CHECK(at(r, 0x0017, Reg::sp) == set({0x8FFE}));
  CHECK(r.solution.reached(0x001A));
  CHECK(at(r, 0x0006, Reg::sp) == unknown(UnknownReason::unsupported_transfer));  // no call summary
  CHECK(at(r, 0x0008, Reg::a) == set({0x05}));
  CHECK(r.solution.unresolved_computed.size() == 1U);
  CHECK(r.solution.unresolved_computed.count(0x0008) == 1U &&
        r.solution.unresolved_computed.at(0x0008) == UnknownReason::unsupported_transfer);
  CHECK(!r.solution.reached(0x000B));
}

// Undecodable and non-code points: typed, never invented.
void fixture_non_code() {
  // 0000 C3 00 40  JP 0x4000 (outside the image: non-code bytes)
  const Image image(0x0003, {{0x0000, {0xC3, 0x00, 0x40}}});
  const Run r = run(image);
  CHECK(r.solution.complete);
  CHECK(r.solution.reached(0x4000));
  CHECK(r.solution.unresolved_computed.count(0x4000) == 1U &&
        r.solution.unresolved_computed.at(0x4000) == UnknownReason::non_immutable_read);
}

// Fixture E: pair/half consistency through the exact pointwise update, from a joined two-value pair.
//   0000 20 05        JR NZ,+5 -> 0007
//   0002 21 34 12     LD HL,0x1234
//   0005 18 03        JR +3 -> 000A
//   0007 21 78 56     LD HL,0x5678
//   000A 2E 00        LD L,0x00          pointwise constant: HL {0x1200,0x5600}
//   000C 24           INC H              pointwise same-pair: HL {0x1300,0x5700} (not the 4-element product of the halves)
//   000D 65           LD H,L             pointwise same-pair copy: HL {0x0000}
//   000E 2E 99        LD L,0x99          HL {0x0099}
//   0010 7E           LD A,(HL)          0x0099 is outside the 0x0012-byte image: Unknown(non_immutable_read)
//   0011 76           HALT
// and a load through an Unknown address (entry HL) is Unknown(non_immutable_read), never bottom.
void fixture_pair_halves() {
  const Image image(0x0012, {{0x0000, {0x20, 0x05, 0x21, 0x34, 0x12, 0x18, 0x03, 0x21, 0x78, 0x56, 0x2E, 0x00, 0x24, 0x65,
                                       0x2E, 0x99, 0x7E, 0x76}}});
  const Run r = run(image);
  CHECK(r.solution.complete);
  CHECK(at(r, 0x000A, Reg::hl) == set({0x1234, 0x5678}));
  CHECK(at(r, 0x000C, Reg::hl) == set({0x1200, 0x5600}));
  CHECK(at(r, 0x000C, Reg::l) == set({0x00}));
  CHECK(at(r, 0x000D, Reg::hl) == set({0x1300, 0x5700}));
  CHECK(at(r, 0x000D, Reg::h) == set({0x13, 0x57}));
  CHECK(at(r, 0x000E, Reg::hl) == set({0x0000}));
  CHECK(at(r, 0x0010, Reg::hl) == set({0x0099}));
  CHECK(at(r, 0x0010, Reg::h) == set({0x00}) && at(r, 0x0010, Reg::l) == set({0x99}));
  CHECK(at(r, 0x0011, Reg::a) == unknown(UnknownReason::non_immutable_read));

  // 0000 7E LD A,(HL) with HL Unknown(unknown_input); 0001 76 HALT
  const Run u = run(Image(0x0002, {{0x0000, {0x7E, 0x76}}}));
  CHECK(at(u, 0x0001, Reg::a) == unknown(UnknownReason::non_immutable_read));
}

// The projection itself (no analysis involved).
segarecomp::cpu::z80::Z80Effect project(const std::vector<std::uint8_t>& bytes, std::uint16_t base = 0x1000) {
  const ImageView view(base, bytes);
  const auto start = segarecomp::cpu::z80::decode_at(view, base);
  CHECK(start.kind == StartKind::decoded);
  return segarecomp::cpu::z80::project_effect(start.instruction);
}

void projection() {
  auto e = project({0x18, 0xFE});  // JR -2: a self-loop
  CHECK(e.supported && e.control == ControlKind::jump && e.target == 0x1000 && e.fallthrough == 0x1002);
  e = project({0x38, 0x10});  // JR C,+16
  CHECK(e.supported && e.control == ControlKind::conditional_jump && e.target == 0x1012);
  e = project({0xFD, 0xE9});  // JP (IY)
  CHECK(e.supported && e.control == ControlKind::computed && e.computed_base == Reg::iy);
  e = project({0xCD, 0x34, 0x12});  // CALL 0x1234
  CHECK(e.supported && e.control == ControlKind::call && e.target == 0x1234 && e.fallthrough == 0x1003);
  CHECK(e.store.has_value() && e.store->width == 2 && e.store->value.constant == 0x1003);
  e = project({0xDD, 0x36, 0xFE, 0x5A});  // LD (IX-2),0x5A
  CHECK(e.supported && e.store.has_value() && e.store->address.base == Reg::ix && e.store->address.offset == -2 &&
        e.store->value.kind == ValueExpr::Kind::constant && e.store->value.constant == 0x5A && e.writes.empty());
  e = project({0xDD, 0x66, 0x03});  // LD H,(IX+3): H is the real H, not IXH
  CHECK(e.supported && e.writes.size() == 1U && e.writes[0].target == Reg::h &&
        e.writes[0].value.kind == ValueExpr::Kind::load && e.writes[0].value.address.base == Reg::ix &&
        e.writes[0].value.address.offset == 3);
  e = project({0x10, 0x00});  // DJNZ +0
  CHECK(e.supported && e.control == ControlKind::conditional_jump && e.writes.size() == 1U &&
        e.writes[0].target == Reg::b && e.writes[0].value.delta == -1);
  e = project({0xDD, 0x00});  // NOP with an ignored DD prefix: two logical bytes
  CHECK(e.supported && e.control == ControlKind::fallthrough && e.fallthrough == 0x1002);
  e = project({0x18, 0x00}, 0xFFFE);  // fallthrough wraps modulo 65,536
  CHECK(e.supported && e.fallthrough == 0x0000);
  // A few enumerated-unsupported forms: typed false, nothing else populated.
  for (const auto& bytes : std::vector<std::vector<std::uint8_t>>{
           {0xD9}, {0xEB}, {0xC5}, {0xE1}, {0xED, 0xB0}, {0xCB, 0x00}, {0x76}, {0xFF}, {0xC4, 0x00, 0x00}, {0x34},
           {0x09}, {0xDD, 0x24}}) {
    e = project(bytes);
    CHECK(!e.supported && e.writes.empty() && !e.store.has_value());
  }
}

void determinism() {
  const Run first = run(table_image());
  const Run second = run(table_image());
  CHECK(!first.text.empty() && first.text == second.text);
  CHECK(first.text.find("computed 0x000a: 0x0030 0x0038") != std::string::npos);
  CHECK(first.text.find("unresolved 0x003b: non_immutable_read") != std::string::npos);
}

}  // namespace

int main() {
  fixture_values();
  fixture_join_and_computed();
  fixture_table_dispatch();
  fixture_call_alu_store();
  fixture_non_code();
  fixture_pair_halves();
  projection();
  determinism();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("analysis_z80_adapter_test: all checks passed");
  return 0;
}
