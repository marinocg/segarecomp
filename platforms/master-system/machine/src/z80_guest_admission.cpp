#include "segarecomp/machine/master_system/z80_guest_admission.hpp"

#include <string>

#include "segarecomp/cpu/z80/decode.hpp"
#include "segarecomp/cpu/z80/effects.hpp"

namespace segarecomp::machine::master_system {
namespace {

namespace z80 = segarecomp::cpu::z80;

struct Flow {
  bool falls_through = true;
  std::optional<std::uint16_t> target;
};

mod::Rejection refuse(const mod::GuestBlock &block, std::size_t position, const std::string &why) {
  return {"E_GUEST_NOT_ADMITTED", "the injected code at " + mod::image_location("master-system", block.image_offset + position) + " is not statically admitted: " + why};
}

// The few straight-line forms the injection subset emits that the CPU library's effect projection does not describe. Every one of them continues at the next instruction and transfers no control.
bool plain_form(const z80::FormDescriptor &form) {
  switch (form.mnemonic) {
    case z80::Mnemonic::push:
    case z80::Mnemonic::pop:
    case z80::Mnemonic::rlca:
    case z80::Mnemonic::rrca:
    case z80::Mnemonic::cpl:
    case z80::Mnemonic::scf:
    case z80::Mnemonic::ccf: return true;
    case z80::Mnemonic::inc:
    case z80::Mnemonic::dec: return form.dst == z80::Operand::hl_ind;   // memory read-modify-write
    case z80::Mnemonic::add: return form.dst == z80::Operand::hl;
    case z80::Mnemonic::ex: return form.dst == z80::Operand::de && form.src == z80::Operand::hl;
    default: return false;
  }
}

// The cartridge offset a control target executes while `from` runs: the running block's own window holds its own bank; a fixed-area block (and every `rom_only` block) executes at its image offsets.
std::uint64_t image_offset_of(const mod::GuestBlock &from, std::uint16_t target) {
  if (target < 0x400U) return target;   // the fixed first 1 KiB of bank 0, also when the running block's window is window 0
  if (!from.fixed && target / 0x4000U == from.address / 0x4000U) return from.image_offset / 0x4000U * 0x4000U + target % 0x4000U;
  return target;
}

// Why a control transfer from `from` to `target` is not admitted, or null. Into injected bytes it must reach the START of an admitted instruction, at the address that instruction was assembled for; the
// rest of a declared free range is padding and work RAM is mutable, so neither is a target. Anything else is existing cartridge code: the assembler only lets a verified @symbol name it.
const char *target_fault(const mod::GuestListing &listing, const mod::GuestBlock &from, std::uint16_t target) {
  constexpr const char *not_start = "the control target is not the start of an admitted instruction";
  if (target >= 0xC000U) return "the control target is not cartridge code";
  const std::uint64_t offset = image_offset_of(from, target);
  for (const mod::GuestBlock &to : listing.blocks) {
    if (offset < to.image_offset || offset >= to.image_offset + to.bytes.size()) continue;
    const std::uint64_t position = offset - to.image_offset;
    if (to.address + position != target) return not_start;
    for (const mod::GuestItem &item : to.items)
      if (item.code && item.position == position) return nullptr;
    return not_start;
  }
  for (const mod::Range &free : listing.free_ranges)
    if (offset >= free.start && offset < free.end) return "the control target lies in free-range padding";
  return nullptr;
}

}  // namespace

std::optional<mod::Rejection> admit_z80_guest(const mod::GuestListing &listing) {
  for (const mod::GuestBlock &block : listing.blocks) {
    if (block.address + block.bytes.size() > 0x10000U) return refuse(block, 0, "the block leaves the 16-bit address space");
    z80::TableFetch fetch;
    for (std::size_t i = 0; i < block.bytes.size(); ++i)
      fetch.set(static_cast<std::uint16_t>(block.address + i), {z80::FetchKind::byte, block.bytes[i], 1U, static_cast<std::uint32_t>(i)});
    std::optional<std::size_t> continues_at;   // the position the previous instruction falls through to
    std::size_t previous = 0;
    for (const mod::GuestItem &item : block.items) {
      if (continues_at && (!item.code || item.position != *continues_at)) return refuse(block, previous, "execution falls through into bytes that are not admitted instructions");
      continues_at.reset();
      if (!item.code) continue;
      const auto start = static_cast<std::uint16_t>(block.address + item.position);
      const z80::StartClassification c = z80::decode_at(fetch, start);
      if (c.kind != z80::StartKind::decoded || c.instruction.extra_prefix_count != 0 || z80::logical_length(c.instruction) != item.length)
        return refuse(block, item.position, "the CPU decoder does not read one instruction at the assembled boundary");
      const z80::FormDescriptor &form = z80::form_descriptor(c.instruction.form);
      const z80::Z80Effect effect = z80::project_effect(c.instruction);
      Flow flow;
      if (effect.supported) {
        switch (effect.control) {
          case z80::ControlKind::fallthrough:
          case z80::ControlKind::conditional_return: break;
          case z80::ControlKind::jump: flow = {false, effect.target}; break;
          case z80::ControlKind::conditional_jump:
          case z80::ControlKind::call: flow = {true, effect.target}; break;
          case z80::ControlKind::return_: flow.falls_through = false; break;
          case z80::ControlKind::computed: return refuse(block, item.position, "an indirect jump has no statically known target");
        }
      } else if (form.mnemonic == z80::Mnemonic::call && form.dst == z80::Operand::cc) {
        const auto target = z80::immediate(c.instruction);
        if (!target) return refuse(block, item.position, "the call target is not an immediate");
        flow.target = *target;
      } else if (!plain_form(form)) {
        return refuse(block, item.position, "the form \"" + z80::form_name(c.instruction.form) + "\" has no admitted effect");
      }
      if (flow.target)
        if (const char *fault = target_fault(listing, block, *flow.target)) return refuse(block, item.position, fault);
      if (flow.falls_through) { continues_at = item.position + item.length; previous = item.position; }
    }
    if (continues_at) return refuse(block, previous, "execution runs off the end of the block (end every path with ret, jp or jr)");
  }
  return std::nullopt;
}

}  // namespace segarecomp::machine::master_system
