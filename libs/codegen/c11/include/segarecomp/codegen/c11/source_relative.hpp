#pragma once

// SEG-097-T002 (ADR 0207): source-relative generated instruction bodies.
//
// A CPU-owned lowering produces, for one decoded instruction at one guest PC, a C text in which some numeric literals are derived from
// that PC (a branch target, a PC-relative effective address, a stacked continuation, the instruction's own address in a stop report)
// and the rest are not (an absolute address, an immediate, a cycle count). To reuse ONE body at several guest PCs the PC-derived
// literals must become `pc0 + constant` while every other literal stays what the CPU lowering wrote.
//
// This helper decides which is which from the lowering itself, never from a table of instruction forms: the instruction is lowered at
// three distinct PCs; the texts must be token-for-token identical except for numeric literals, and every differing literal must move by
// exactly the PC difference (linear in the PC, same suffix). Anything else - a different token, an identifier that embeds the PC, a
// literal that does not move linearly (masked, hashed) or sits where only a constant expression is allowed - is a typed refusal and the
// caller keeps the per-mapping emission for that instruction. The CPU lowering is not modified; no semantics live here.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace segarecomp {

enum class SourceRelativeStatus : std::uint8_t {
  ok,
  token_structure,       // the three texts differ in anything but numeric literals
  literal_not_linear,    // a differing literal does not move by the PC difference
  literal_suffix,        // a differing literal changes its type suffix
  constant_context,      // a PC-derived literal appears where only a constant expression is allowed (case label, initializer)
};
[[nodiscard]] const char *source_relative_status_name(SourceRelativeStatus status) noexcept;

struct SourceRelativeText {
  SourceRelativeStatus status{SourceRelativeStatus::ok};
  std::string text;                    // the first text with each PC-derived literal replaced by `(uint32_t)(<pc_symbol> + UINT32_C(delta))`
  std::uint32_t relative_literals{};   // how many literals were replaced
};

// `texts[i]` is the lowering at `pcs[i]` (pairwise distinct, even). `pc_symbol` names the C variable holding the instruction's own
// address at run time.
[[nodiscard]] SourceRelativeText derive_source_relative_text(const std::array<std::string, 3> &texts, const std::array<std::uint32_t, 3> &pcs,
                                                             std::string_view pc_symbol);

}  // namespace segarecomp
