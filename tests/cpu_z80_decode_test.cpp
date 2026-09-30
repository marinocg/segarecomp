// SEG-008-T002: Z80 decoder over a logical fetch function. Synthetic mappings only. Covers logical-fetch wrap,
// unresolved/mutable classification, DD/FD prefix chains of any length, prefix lock, linear-time classification of
// all 65,536 starts and provenance replay. The word-by-word comparison with the legal-form dataset lives in
// tools/z80_capability_coverage.py (driven by z80_capability_ratchet_test).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "segarecomp/core/address.hpp"
#include "segarecomp/cpu/z80/decode.hpp"

using namespace segarecomp::cpu::z80;

namespace {

int failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                                     \
    }                                                                                 \
  } while (0)

class CountingFetch final : public LogicalFetch {
 public:
  explicit CountingFetch(const TableFetch& t) : table_(t) {}
  FetchedByte fetch(std::uint16_t a) const override {
    ++calls;
    return table_.fetch(a);
  }
  mutable std::size_t calls = 0;

 private:
  const TableFetch& table_;
};

void put(TableFetch& t, std::uint16_t address, const std::vector<std::uint8_t>& bytes, std::uint32_t image = 1) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::uint16_t a = static_cast<std::uint16_t>(address + i);
    t.set(a, {FetchKind::byte, bytes[i], image, a});
  }
}

void mark(TableFetch& t, std::uint16_t address, FetchKind kind) { t.set(address, {kind, 0, 0, 0}); }

// A canonical encoding of every classifiable (space, byte): prefixes, opcode, then displacement/immediates.
std::vector<std::uint8_t> encode(Space space, std::uint8_t b, std::uint8_t d, std::uint8_t n0, std::uint8_t n1) {
  const FormDescriptor* f = nullptr;
  const ByteClass bc = classify_opcode_byte(space, b);
  std::vector<std::uint8_t> out;
  switch (space) {
    case Space::cb: return {0xCB, b};
    case Space::ddcb: return {0xDD, 0xCB, d, b};
    case Space::fdcb: return {0xFD, 0xCB, d, b};
    default: break;
  }
  if (space == Space::ed) out.push_back(0xED);
  if (space == Space::dd) out.push_back(0xDD);
  if (space == Space::fd) out.push_back(0xFD);
  out.push_back(b);
  if (bc.form == kNoForm) return out;  // escape/chain bytes are exercised separately
  f = &form_descriptor(bc.form);
  const std::size_t total = static_cast<std::size_t>(f->length) + (bc.kind == ByteClassKind::prefix_ignored ? 1u : 0u);
  // Operand bytes in fetch order: displacement (if any), then immediates.
  std::vector<std::uint8_t> ops;
  if (f->displacement_index != kNoIndex) ops.push_back(d);
  if (f->immediate_size >= 1) ops.push_back(n0);
  if (f->immediate_size == 2) ops.push_back(n1);
  out.insert(out.end(), ops.begin(), ops.end());
  (void)total;
  return out;
}

void test_form_table() {
  CHECK(all_forms().size() == 261);
  for (const FormDescriptor& f : all_forms()) {
    CHECK(form_descriptor(f.id).id == f.id);
    CHECK(f.length >= 1 && f.length <= 4);
    CHECK(f.timing.primary > 0);
    if (f.alias_of != kNoForm) CHECK(form_descriptor(f.alias_of).alias_of == kNoForm);
  }
  CHECK(classify_opcode_byte(Space::base, 0x00).form != kNoForm);
  CHECK(classify_opcode_byte(Space::base, 0xDD).kind == ByteClassKind::escape_dd);
  CHECK(classify_opcode_byte(Space::dd, 0xDD).kind == ByteClassKind::prefix_chain);
  CHECK(classify_opcode_byte(Space::dd, 0xED).kind == ByteClassKind::prefix_ignored_before_ed);
  CHECK(classify_opcode_byte(Space::fd, 0xCB).kind == ByteClassKind::escape_fdcb);
  CHECK(classify_opcode_byte(Space::dd, 0x41).kind == ByteClassKind::prefix_ignored);
}

// Every canonical instruction placed so that its bytes wrap across 0xFFFF decodes like the unwrapped placement.
void test_wrap() {
  std::size_t placements = 0;
  for (unsigned s = 0; s < kSpaceCount; ++s) {
    const Space space = static_cast<Space>(s);
    for (unsigned b = 0; b < 256; ++b) {
      const ByteClass bc = classify_opcode_byte(space, static_cast<std::uint8_t>(b));
      if (bc.form == kNoForm) continue;
      const std::vector<std::uint8_t> bytes = encode(space, static_cast<std::uint8_t>(b), 0x85, 0x12, 0x34);
      TableFetch flat = TableFetch::invariant(7, 0x00);
      put(flat, 0x1000, bytes, 7);
      const StartClassification ref = decode_at(flat, 0x1000);
      CHECK(ref.kind == StartKind::decoded);
      if (ref.kind != StartKind::decoded) continue;
      CHECK(ref.instruction.form == bc.form);
      CHECK(logical_length(ref.instruction) == bytes.size());
      CHECK(ref.instruction.provenance.logical_byte_count == bytes.size());
      for (std::size_t j = 1; j <= bytes.size(); ++j) {  // j bytes before the wrap point
        TableFetch wrapped = TableFetch::invariant(7, 0x00);
        const std::uint16_t start = static_cast<std::uint16_t>(0x10000 - j);
        put(wrapped, start, bytes, 7);
        const StartClassification w = decode_at(wrapped, start);
        CHECK(w.kind == StartKind::decoded);
        CHECK(w.instruction.form == ref.instruction.form);
        CHECK(w.instruction.provenance.operands == ref.instruction.provenance.operands);
        CHECK(w.instruction.provenance.opcode == ref.instruction.provenance.opcode);
        CHECK(w.instruction.provenance.logical_byte_count == bytes.size());
        // Provenance replays through the fetch function, across the wrap.
        const auto replay = replay_bytes(wrapped, w.instruction.provenance);
        CHECK(replay.has_value() && *replay == bytes);
        ++placements;
      }
    }
  }
  CHECK(placements > 1000);
}

void test_unresolved_and_mutable() {
  for (unsigned s = 0; s < kSpaceCount; ++s) {
    const Space space = static_cast<Space>(s);
    for (unsigned b = 0; b < 256; ++b) {
      const ByteClass bc = classify_opcode_byte(space, static_cast<std::uint8_t>(b));
      if (bc.form == kNoForm) continue;
      const std::vector<std::uint8_t> bytes = encode(space, static_cast<std::uint8_t>(b), 3, 4, 5);
      if (bytes.size() < 2) continue;
      for (std::size_t cut = 1; cut < bytes.size(); ++cut) {  // continues into a foreign window / non-code at `cut`
        for (FetchKind kind : {FetchKind::unresolved_mapping, FetchKind::non_code}) {
          TableFetch t = TableFetch::invariant(1, 0);
          put(t, 0xFFF0, bytes);
          mark(t, static_cast<std::uint16_t>(0xFFF0 + cut), kind);
          const StartClassification c = decode_at(t, 0xFFF0);
          CHECK(c.kind == (kind == FetchKind::non_code ? StartKind::mutable_code : StartKind::unresolved_fetch_mapping));
          CHECK(c.blocking_address == static_cast<std::uint16_t>(0xFFF0 + cut));
        }
      }
    }
  }
  TableFetch t = TableFetch::invariant(1, 0);
  mark(t, 0x0000, FetchKind::unresolved_mapping);
  CHECK(decode_at(t, 0x0000).kind == StartKind::unresolved_fetch_mapping);
  put(t, 0xFFFF, {0x3E});  // LD A,n whose immediate wraps into the unresolved byte at 0x0000
  CHECK(decode_at(t, 0xFFFF).kind == StartKind::unresolved_fetch_mapping);
  CHECK(decode_at(t, 0xFFFF).blocking_address == 0x0000);
}

std::vector<std::uint8_t> chain(std::size_t k, bool alternate, std::uint8_t first = 0xDD) {
  std::vector<std::uint8_t> v;
  for (std::size_t i = 0; i < k; ++i) v.push_back(alternate ? ((i & 1) ? (first == 0xDD ? 0xFD : 0xDD) : first) : first);
  return v;
}

void test_chains() {
  for (std::size_t k : {1u, 2u, 3u, 255u, 4096u}) {
    for (int mode = 0; mode < 3; ++mode) {  // DD-only, FD-only, alternating
      const std::uint8_t first = mode == 1 ? 0xFD : 0xDD;
      const std::vector<std::uint8_t> prefixes = chain(k, mode == 2, first);
      const std::uint8_t last = prefixes.back();
      const EffectivePrefix eff = last == 0xDD ? EffectivePrefix::dd : EffectivePrefix::fd;
      struct Case { std::vector<std::uint8_t> tail; bool effective_form; std::uint32_t extra_delta; };
      // LD HL-class (LD IX,nn / LD IY,nn), ignored non-HL (LD B,C), CB d op, ED op, and DD/FD-before-ED.
      const Case cases[] = {{{0x21, 0x34, 0x12}, true, 1}, {{0x41}, false, 0}, {{0xCB, 0x05, 0x06}, true, 1},
                            {{0xED, 0x44}, false, 0}, {{0x3E, 0x77}, false, 0}};
      for (const Case& c : cases) {
        for (std::uint16_t base : {std::uint16_t{0x2000}, static_cast<std::uint16_t>(0x10000 - k / 2)}) {  // 2nd wraps
          TableFetch t = TableFetch::invariant(3, 0x00);
          std::vector<std::uint8_t> all = prefixes;
          all.insert(all.end(), c.tail.begin(), c.tail.end());
          put(t, base, all, 3);
          const StartClassification d = decode_at(t, base);
          CHECK(d.kind == StartKind::decoded);
          if (d.kind != StartKind::decoded) continue;
          const FormDescriptor& f = form_descriptor(d.instruction.form);
          CHECK(d.instruction.provenance.prefix_count == k);
          CHECK(d.instruction.provenance.effective_prefix == eff);
          // Contract 3.2: length = extra + form length, T = 4*extra + T(form), M1 = extra + M1(form).
          const std::uint32_t extra = c.effective_form ? static_cast<std::uint32_t>(k - 1) : static_cast<std::uint32_t>(k);
          CHECK(d.instruction.extra_prefix_count == extra);
          CHECK(d.instruction.provenance.logical_byte_count == all.size());
          CHECK(logical_length(d.instruction) == all.size());
          CHECK(t_states(d.instruction, TimingOutcome::primary) == 4ULL * extra + f.timing.primary);
          CHECK(m1_fetches(d.instruction) == extra + f.m1_fetches);
          const auto replay = replay_bytes(t, d.instruction.provenance);
          CHECK(replay.has_value() && *replay == all);
        }
      }
      // The chain runs into non-code or a foreign window before reaching an opcode.
      TableFetch t = TableFetch::invariant(3, 0x00);
      put(t, 0x8000, prefixes, 3);
      mark(t, static_cast<std::uint16_t>(0x8000 + k), FetchKind::unresolved_mapping);
      CHECK(decode_at(t, 0x8000).kind == StartKind::unresolved_fetch_mapping);
      mark(t, static_cast<std::uint16_t>(0x8000 + k), FetchKind::non_code);
      CHECK(decode_at(t, 0x8000).kind == StartKind::mutable_code);
      CHECK(decode_at(t, 0x8000).blocking_address == static_cast<std::uint16_t>(0x8000 + k));
    }
  }
}

void test_prefix_lock_and_linear_time() {
  for (int mode = 0; mode < 3; ++mode) {
    TableFetch t = TableFetch::invariant(9, 0);
    for (std::size_t a = 0; a < 65536; ++a) {
      const std::uint8_t v = mode == 0 ? 0xDD : mode == 1 ? 0xFD : ((a & 1) ? 0xFD : 0xDD);
      t.set(static_cast<std::uint16_t>(a), {FetchKind::byte, v, 9, static_cast<std::uint32_t>(a)});
    }
    CountingFetch counting(t);
    const std::vector<StartClassification> all = classify_all(counting);
    CHECK(counting.calls == 65536);  // one fetch per address: linear, not per-start rescans
    CHECK(all.size() == 65536);
    bool every_lock = true;
    for (const StartClassification& c : all) every_lock = every_lock && c.kind == StartKind::prefix_lock;
    CHECK(every_lock);
    CHECK(decode_at(t, 0x1234).kind == StartKind::prefix_lock);
    CHECK(decode_at(t, 0xFFFF).kind == StartKind::prefix_lock);
    // One non-prefix byte breaks the lock: every start reaches it (wrapping) and decodes.
    t.set(0x4000, {FetchKind::byte, 0x00, 9, 0x4000});  // NOP
    const std::vector<StartClassification> broken = classify_all(t);
    std::size_t decoded = 0;
    for (const StartClassification& c : broken) decoded += c.kind == StartKind::decoded;
    CHECK(decoded == 65536);
    CHECK(broken[0x4001].instruction.provenance.prefix_count == 65535);  // the whole ring before the NOP
    CHECK(broken[0x4001].instruction.provenance.logical_byte_count == 65536);
    CHECK(broken[0x4001].instruction.extra_prefix_count == 65535);
  }
}

// classify_all agrees with per-start decode_at on a pseudo-random mapping rich in prefixes and holes.
void test_classify_all_agrees() {
  TableFetch t;
  std::uint32_t x = 12345;
  for (std::size_t a = 0; a < 65536; ++a) {
    x = x * 1664525u + 1013904223u;
    const unsigned r = (x >> 24) & 0xFF;
    static constexpr std::uint8_t pool[] = {0xDD, 0xFD, 0xCB, 0xED, 0x21, 0x36, 0x00, 0x41, 0x76, 0xC3, 0xE9, 0x40};
    FetchedByte b{FetchKind::byte, pool[r % sizeof(pool)], static_cast<std::uint32_t>(a >> 15), static_cast<std::uint32_t>(a & 0x7FFF)};
    if (r == 250) b.kind = FetchKind::unresolved_mapping;
    if (r == 251) b.kind = FetchKind::non_code;
    t.set(static_cast<std::uint16_t>(a), b);
  }
  const std::vector<StartClassification> all = classify_all(t);
  std::size_t mismatches = 0, decoded = 0;
  for (std::size_t a = 0; a < 65536; ++a) {
    const StartClassification one = decode_at(t, static_cast<std::uint16_t>(a));
    mismatches += !(one == all[a]);
    decoded += one.kind == StartKind::decoded;
    if (one.kind == StartKind::decoded) {  // provenance round-trips through the (cross-window) fetch function
      const auto replay = replay_bytes(t, one.instruction.provenance);
      CHECK(replay.has_value());
      CHECK(one.instruction.provenance.image_id == t.fetch(static_cast<std::uint16_t>(a)).image_id);
      CHECK(one.instruction.provenance.image_offset == t.fetch(static_cast<std::uint16_t>(a)).image_offset);
    }
  }
  CHECK(mismatches == 0);
  CHECK(decoded > 1000);
}

// Whole-mapping bound: an unresolved/mutable hole stops the chain, so a mapping full of prefixes with one hole is
// never a lock (each start blocks at the hole).
void test_hole_breaks_lock() {
  TableFetch t = TableFetch::invariant(1, 0xDD);
  mark(t, 0x8000, FetchKind::unresolved_mapping);
  const std::vector<StartClassification> all = classify_all(t);
  CHECK(all[0x7FFF].kind == StartKind::unresolved_fetch_mapping);
  CHECK(all[0x7FFF].blocking_address == 0x8000);
  CHECK(all[0x0000].kind == StartKind::unresolved_fetch_mapping);
  CHECK(all[0x8000].kind == StartKind::unresolved_fetch_mapping);
}

void test_ignored_prefix_and_operands() {
  TableFetch t = TableFetch::invariant(1, 0);
  put(t, 0x100, {0xDD, 0xEB});  // DD EX DE,HL: the prefix is a no-operation
  const StartClassification a = decode_at(t, 0x100);
  CHECK(a.kind == StartKind::decoded);
  CHECK(a.instruction.extra_prefix_count == 1);
  CHECK(form_name(a.instruction.form) == "ex.de_hl.base");
  CHECK(t_states(a.instruction, TimingOutcome::primary) == 8);
  put(t, 0x200, {0xFD, 0xDD, 0xED, 0xB0});  // ignored prefixes, then LDIR (repeat timing)
  const StartClassification b = decode_at(t, 0x200);
  CHECK(form_name(b.instruction.form) == "ldir.ed");
  CHECK(b.instruction.extra_prefix_count == 2);
  CHECK(t_states(b.instruction, TimingOutcome::alternate) == 21 + 8);
  put(t, 0x300, {0xDD, 0x36, 0xFE, 0x99});  // LD (IX-2),0x99
  const StartClassification c = decode_at(t, 0x300);
  CHECK(displacement(c.instruction) == std::optional<std::int8_t>(-2));
  CHECK(immediate(c.instruction) == std::optional<std::uint16_t>(0x99));
  put(t, 0x400, {0xDD, 0x2A, 0x34, 0x12});  // LD IX,(0x1234)
  CHECK(immediate(decode_at(t, 0x400).instruction) == std::optional<std::uint16_t>(0x1234));
}

void test_core_variant() {
  CHECK(segarecomp::CpuVariant::z80 != segarecomp::CpuVariant::mc68000);
}

}  // namespace

int main() {
  test_form_table();
  test_wrap();
  test_unresolved_and_mutable();
  test_chains();
  test_prefix_lock_and_linear_time();
  test_classify_all_agrees();
  test_hole_breaks_lock();
  test_ignored_prefix_and_operands();
  test_core_variant();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("cpu_z80_decode_tests OK");
  return 0;
}
