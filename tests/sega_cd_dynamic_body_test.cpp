// SEG-100-T002 (ADR 0222): the source-offset instruction body for a guest base that is not known when the body is compiled.
//
// Hand-encoded MC68000 instructions (no disc, no commercial byte) prove, against the CPU-owned decode and lowering:
//   * a PC-relative instruction (branch, call, LEA/MOVE d16(PC), DBcc) becomes a body whose PC-derived literals are `scd_pc0 + constant`, with no
//     sentinel PC left in the text;
//   * an absolute reference (JSR abs.L, MOVE to abs.L, an immediate) is NOT rebased: its literal is exactly what the lowering wrote;
//   * a computed transfer, a return and an exception form need no PC literal and still derive;
//   * the body is a function of the instruction's bytes alone: the same bytes at any offset, in any image, give identical text;
//   * the derivation is deterministic;
//   * negative: an odd offset, an offset outside the visible bytes and an instruction whose extension words leave the visible bytes are a
//     typed `not_decoded`, never a body; nothing silently treats a PC-dependent instruction as position independent.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/dynamic_body.hpp"

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

bool has(const std::string &text, const std::string &needle) { return text.find(needle) != std::string::npos; }

std::string all_text(const DynamicBody &b) { return b.slots + b.body + b.timing + b.refresh_skip; }

bool no_sentinel(const DynamicBody &b) {
  const std::string t = all_text(b);
  return !has(t, "0x00200000") && !has(t, "0x00300000") && !has(t, "0x00480000") && !has(t, "0x002000") && !has(t, "0x003000") && !has(t, "0x004800");
}

DynamicBody derive(const std::vector<std::uint8_t> &bytes) { return derive_dynamic_body(bytes, static_cast<std::uint32_t>(bytes.size()), 0U); }

}  // namespace

int main() {
  struct Form {
    const char *name;
    std::vector<std::uint8_t> bytes;
    bool pc_relative;
  };
  const std::vector<Form> forms = {
      {"NOP", words({0x4E71}), false},
      {"RTS", words({0x4E75}), false},
      {"RTE", words({0x4E73}), false},
      {"BRA.B +8", words({0x6008}), true},
      {"BRA.B -4", words({0x60FC}), true},
      {"BRA.W", words({0x6000, 0x0010}), true},
      {"BSR.W", words({0x6100, 0x0020}), true},
      {"BEQ.B", words({0x6706}), true},
      {"DBRA D0", words({0x51C8, 0xFFFA}), true},
      {"LEA d16(PC),A0", words({0x41FA, 0x0010}), true},
      {"MOVE.W d16(PC),D0", words({0x303A, 0x0012}), true},
      {"JSR (A0)", words({0x4E90}), false},
      {"JMP (A0)", words({0x4ED0}), false},
      {"TRAP #1", words({0x4E41}), true},
      {"DIVU D0,D1", words({0x82C0}), true},
  };
  for (const auto &f : forms) {
    const auto b = derive(f.bytes);
    check(b.status == DynamicBodyStatus::ok && b.raw == f.bytes, std::string(f.name) + ": a source-offset body derives and verifies exactly its bytes");
    check(!f.pc_relative || (b.relative_literals != 0U && has(all_text(b), "scd_pc0")), std::string(f.name) + ": PC-derived literals are scd_pc0-relative");
    check(no_sentinel(b), std::string(f.name) + ": no sentinel PC remains in the text");
    check(has(b.timing, "") && !b.timing.empty(), std::string(f.name) + ": the CPU-owned timing expression is carried");
  }

  // Absolute references keep their absolute meaning.
  {
    const auto jsr = derive(words({0x4EB9, 0x0012, 0x3456}));  // JSR $123456
    check(jsr.status == DynamicBodyStatus::ok && (has(jsr.body, "0x00123456") || has(jsr.body, "0x123456")), "JSR abs.L: the absolute destination is not rebased");
    check(!has(jsr.body, "scd_pc0 + UINT32_C(0x00123456") && !has(jsr.body, "scd_pc0 + UINT32_C(0x123456"), "JSR abs.L: no scd_pc0 sum of the absolute destination");
    const auto st = derive(words({0x33C0, 0x00FF, 0x1234}));  // MOVE.W D0,$FF1234
    check(st.status == DynamicBodyStatus::ok && (has(st.body, "0x00FF1234") || has(st.body, "0x00ff1234") || has(st.body, "0xFF1234") || has(st.body, "0xff1234")), "MOVE.W D0,abs.L: the absolute store address is not rebased");
    const auto imm = derive(words({0x223C, 0x1234, 0x5678}));  // MOVE.L #$12345678,D1
    check(imm.status == DynamicBodyStatus::ok && (has(imm.body, "0x12345678") || has(imm.body, "0x12345678")), "MOVE.L #imm,D1: the immediate is not rebased");
  }

  // The body is a function of the instruction's bytes alone: location independence.
  {
    std::vector<std::uint8_t> image = words({0x4E71, 0x4E71, 0x4E71, 0x4E71});
    const auto lea = words({0x41FA, 0x0010});
    image.insert(image.end(), lea.begin(), lea.end());
    image.insert(image.end(), {0x4E, 0x75});
    const auto alone = derive(lea);
    const auto inside = derive_dynamic_body(image, static_cast<std::uint32_t>(image.size()), 8U);
    check(alone.status == DynamicBodyStatus::ok && inside.status == DynamicBodyStatus::ok && alone.body == inside.body && alone.slots == inside.slots &&
              alone.timing == inside.timing && alone.refresh_skip == inside.refresh_skip && alone.raw == inside.raw,
          "the same bytes at another offset of another image give identical text");
    const auto again = derive(lea);
    check(again.body == alone.body && again.timing == alone.timing, "the derivation is deterministic");
  }

  // Negative cases: typed, never a body.
  {
    const auto bytes = words({0x4E71, 0x4E71});
    check(derive_dynamic_body(bytes, 4U, 1U).status != DynamicBodyStatus::ok, "an odd source offset is refused");
    check(derive_dynamic_body(bytes, 4U, 4U).status != DynamicBodyStatus::ok, "an offset at the end of the visible bytes is refused");
    check(derive_dynamic_body(bytes, 4U, 400U).status != DynamicBodyStatus::ok, "an offset outside the image is refused");
    const auto move_l = words({0x223C, 0x1234, 0x5678});
    check(derive_dynamic_body(move_l, 4U, 0U).status == DynamicBodyStatus::not_decoded, "an instruction whose extension words leave the visible bytes (a source-range boundary) is not decoded");
    check(derive_dynamic_body(move_l, static_cast<std::uint32_t>(move_l.size() + 8U), 0U).status == DynamicBodyStatus::not_decoded, "a visible length larger than the image is refused");
    check(DynamicBody{}.raw.empty() && DynamicBody{}.status == DynamicBodyStatus::not_decoded, "the default body is the refusal");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
