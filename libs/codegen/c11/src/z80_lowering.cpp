#include "segarecomp/codegen/c11/z80_lowering.hpp"

#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace segarecomp::codegen::z80 {
namespace {

using Key = std::tuple<int, int, int, int>;

Key key_of(cpu::z80::Space space, cpu::z80::Mnemonic mnemonic, cpu::z80::Operand dst, cpu::z80::Operand src) {
  return {static_cast<int>(space), static_cast<int>(mnemonic), static_cast<int>(dst), static_cast<int>(src)};
}

const std::map<Key, const LoweringRow*>& registry() {
  static const std::map<Key, const LoweringRow*> table = [] {
    std::map<Key, const LoweringRow*> built;
    for (const auto rows : {data_alu_lowering_rows(), control_stack_lowering_rows(), cb_bit_prefix_lowering_rows(),
                            ed_io_interrupt_lowering_rows()}) {
      for (const LoweringRow& row : rows) {
        if (!built.emplace(key_of(row.space, row.mnemonic, row.dst, row.src), &row).second)
          throw std::logic_error("z80 lowering: a form is claimed by two rows");
      }
    }
    return built;
  }();
  return table;
}

}  // namespace

const LoweringRow* find_lowering(const cpu::z80::FormDescriptor& form) {
  const auto found = registry().find(key_of(form.space, form.mnemonic, form.dst, form.src));
  return found == registry().end() ? nullptr : found->second;
}

bool has_lowering(cpu::z80::FormId form) { return find_lowering(cpu::z80::form_descriptor(form)) != nullptr; }

std::string reg8(unsigned index) {
  static const char* const names[8] = {"s->b", "s->c", "s->d", "s->e", "s->h", "s->l", "", "s->a"};
  return index < 8 ? names[index] : "";
}

std::string hex_literal(unsigned value, unsigned digits) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string text = "0x";
  for (unsigned i = digits; i > 0; --i) text += hex[(value >> (4 * (i - 1))) & 0xFu];
  return text + "u";
}

}  // namespace segarecomp::codegen::z80
