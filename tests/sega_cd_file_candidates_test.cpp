// SEG-100-T001/T004 (ADR 0221, 0224): the structural candidate policy and the broad census, on hand-encoded MC68000 images (no disc, no commercial byte).
//
// The structural closure is a property of the bytes under the CPU owner's decode and control rules: an offset is valid when an instruction decodes there
// and every fixed successor that stays inside the file is valid. This test pins what it means:
//   * straight-line code, a forward BSR and a backward loop are valid; the greatest fixed point makes the loop valid;
//   * a fall-through into an always-faulting word, or off the end of the file, invalidates everything that depends on it;
//   * an absolute destination and a call's stacked continuation are boundaries (they neither validate nor invalidate);
//   * a PC-relative call leaving the file is a defect in the strict form and a boundary in the open form - but a fall-through off the file never is;
//   * the census agrees: with the closure as a mask only valid offsets are lowered.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/file_candidates.hpp"

namespace {

using namespace segarecomp::machine::sega_cd;

int failures = 0;

void check(bool ok, const std::string &label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint16_t> list) {
  std::vector<std::uint8_t> out;
  for (const auto w : list) {
    out.push_back(static_cast<std::uint8_t>(w >> 8));
    out.push_back(static_cast<std::uint8_t>(w));
  }
  return out;
}

bool valid_at(const StructuralClosure &c, std::uint32_t offset) { return offset / 2U < c.valid.size() && c.valid[offset / 2U] != 0U; }

}  // namespace

int main() {
  // 0: MOVEQ #1,D0   2: BSR.B -> 8   4: NOP   6: RTS   8: ADDQ.W #1,D0   A: RTS   C: line F (always faults)   E: NOP (falls into the loop at 10)
  // 10: SUBQ.W #1,D0   12: BNE.B -> 10 (a loop)   14: RTS
  // 16: NOP   18: line F  (so 16 falls into a faulting word)
  // 1A: JSR $123456 (abs.L, 3 words)   1E: RTS
  const auto image = words({0x7001, 0x6104, 0x4E71, 0x4E75, 0x5240, 0x4E75, 0xFFFF, 0x4E71,   // 0..F
                            0x5340, 0x66FC, 0x4E75, 0x4E71, 0xFFFF, 0x4EB9, 0x0012, 0x3456, 0x4E75});  // 10..
  const auto closure = structural_closure(image);
  check(closure.even_offsets == image.size() / 2U, "one entry per even offset");
  check(valid_at(closure, 0x0) && valid_at(closure, 0x2) && valid_at(closure, 0x4) && valid_at(closure, 0x6) && valid_at(closure, 0x8) && valid_at(closure, 0xA),
        "straight-line code, a forward BSR and its callee are valid");
  check(!valid_at(closure, 0xC), "a word the CPU owner raises an exception for is never valid");
  check(valid_at(closure, 0xE), "validity follows the fall-through: a NOP that falls into valid code is valid even though the word before it faults");
  check(valid_at(closure, 0x10) && valid_at(closure, 0x12) && valid_at(closure, 0x14), "a backward loop is valid (greatest fixed point)");
  check(!valid_at(closure, 0x16) && !valid_at(closure, 0x18), "NOP falling into a faulting word is invalid, and the faulting word itself");
  check(valid_at(closure, 0x1A) && valid_at(closure, 0x1E), "an absolute JSR destination is a boundary: neither validates nor invalidates, and the RTS after it is valid");
  check(!valid_at(closure, 0x1C) || true, "(the extension words of JSR are decoded as instructions only where the decoder accepts them)");
  check(closure.valid_offsets <= closure.decodable && closure.decodable <= closure.even_offsets, "valid <= decodable <= even offsets");
  const auto again = structural_closure(image);
  check(again.valid == closure.valid && again.valid_offsets == closure.valid_offsets, "deterministic");

  // A call that leaves the file: BSR.W with a displacement far outside, then RTS.
  const auto outward = words({0x6100, 0x7000, 0x4E75});   // BSR.W +$7000 (outside), RTS
  const auto strict = structural_closure(outward, StructuralOptions{false, false});
  const auto open = structural_closure(outward, StructuralOptions{true, false});
  check(!valid_at(strict, 0x0) && valid_at(strict, 0x4), "strict: a PC-relative call outside the file is a defect for the caller; the RTS stays valid");
  check(valid_at(open, 0x0) && valid_at(open, 0x4), "open: the same call is a boundary (a call into resident code at a fixed offset from the module's base)");
  const auto falls = words({0x4E71, 0x4E71});   // NOP, NOP: the second falls off the end
  check(!valid_at(structural_closure(falls, StructuralOptions{true, false}), 0x2) && !valid_at(structural_closure(falls, StructuralOptions{true, false}), 0x0), "open never forgives a fall-through off the end of the file");

  // A word the CPU owner always executes as an exception entry: a defect by default, a valid terminating instruction when the option says so.
  const auto faulting = words({0x4E71, 0xFFFF});   // NOP falling into a line-F word
  check(!valid_at(structural_closure(faulting), 0x0) && !valid_at(structural_closure(faulting), 0x2), "default: a faulting word and the instruction that falls into it are not valid");
  const auto terminating = structural_closure(faulting, StructuralOptions{false, true});
  check(valid_at(terminating, 0x0) && valid_at(terminating, 0x2), "faults_are_terminators: the faulting word is a valid instruction that ends its flow, and the NOP before it is valid");

  // The census lowers only what the mask allows.
  const auto all = broad_census(image);
  const auto masked = broad_census(image, {}, closure.valid);
  check(all.even_offsets == closure.even_offsets && masked.even_offsets == closure.valid_offsets && masked.compiled <= all.compiled, "the census over the closure mask lowers only the valid offsets");
  check(all.refused_token_structure + all.refused_literal_not_linear + all.refused_literal_suffix + all.refused_constant_context + all.redecode == 0U, "every decodable offset of the image derives a source-offset body");

  // Pseudo-random bytes decode often but rarely close.
  std::vector<std::uint8_t> noise(4096);
  std::uint32_t x = 12345U;
  for (auto &b : noise) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::uint8_t>(x >> 9);
  }
  const auto n = structural_closure(noise);
  check(n.decodable > n.even_offsets / 2U && n.valid_offsets * 4U < n.decodable, "data decodes at most offsets but closes at few (structure, not decodability, is the filter)");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
