#pragma once

// NMOS Z80 encoding surface: the seven finite canonical opcode spaces, typed forms, byte classification and the
// static timing descriptor (docs/architecture/z80-cpu-contract.md sections 3 and 7; ADR 0056/0058/0059).
// Written from the public Zilog UM0080 / Young / Dinu references. It never reads the T001 dataset.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace segarecomp::cpu::z80 {

enum class Space : std::uint8_t { base, cb, ed, dd, fd, ddcb, fdcb };
inline constexpr std::size_t kSpaceCount = 7;

enum class Mnemonic : std::uint8_t {
  nop, ld, inc, dec, rlca, rrca, rla, rra, ex, add, djnz, jr, daa, cpl, scf, ccf, halt, adc, sub, sbc, and_, xor_,
  or_, cp, ret, pop, jp, call, push, rst, exx, out, in, di, ei, rlc, rrc, rl, rr, sla, sra, sll, srl, bit, res, set,
  neg, retn, reti, im, rrd, rld, ldi, cpi, ini, outi, ldd, cpd, ind, outd, ldir, cpir, inir, otir, lddr, cpdr, indr,
  otdr,
};

// Operand classes (coarse addressing/register class of the destination or source).
enum class Operand : std::uint8_t {
  none, a, r, rr, rr2, rr_ind, hl, hl_ind, de, sp, sp_ind, nn, n, e, p, cc, cc4, bit, af, af_alt, i, r_refresh,
  im0, im1, im2, n_port, c_port, nn_ind, flags_only, zero, ed_undefined, rr_ed, ix, iy, ix_d, iy_d, ix_half,
  iy_half, rr_ix, rr_iy, ix_d_copy_r, iy_d_copy_r,
};

// Timing descriptor. Only the classes where timing genuinely differs by outcome carry an alternate value.
//   fixed:       primary = T-states
//   conditional: primary = not taken, alternate = taken
//   repeat:      primary = final iteration, alternate = repeating iteration
//   halt:        primary = the HALT instruction, alternate = each further halted NOP cycle
enum class TimingClass : std::uint8_t { fixed, conditional, repeat, halt };
struct Timing {
  TimingClass klass{TimingClass::fixed};
  std::uint16_t primary{};
  std::uint16_t alternate{};
  friend bool operator==(const Timing&, const Timing&) = default;
};

using FormId = std::uint16_t;
inline constexpr FormId kNoForm = 0xFFFF;
inline constexpr std::uint8_t kNoIndex = 0xFF;

// A form: one (space, mnemonic, operand-class pair) identity. `length` counts the bytes of the canonical
// instruction (the effective prefix included, superseded/ignored prefixes excluded); index fields locate the
// opcode byte, displacement and immediate in it.
struct FormDescriptor {
  FormId id{kNoForm};
  Space space{Space::base};
  Mnemonic mnemonic{Mnemonic::nop};
  Operand dst{Operand::none};
  Operand src{Operand::none};
  bool documented{true};
  FormId alias_of{kNoForm};  // another form with the same architectural effect
  std::uint8_t length{1};
  std::uint8_t m1_fetches{1};  // R-register increment of the canonical instruction
  std::uint8_t opcode_index{0};
  std::uint8_t displacement_index{kNoIndex};
  std::uint8_t immediate_index{kNoIndex};
  std::uint8_t immediate_size{0};
  Timing timing{};
};

enum class ByteClassKind : std::uint8_t {
  form,
  escape_cb,
  escape_ed,
  escape_dd,
  escape_fd,
  escape_ddcb,
  escape_fdcb,
  prefix_chain,
  prefix_ignored,
  prefix_ignored_before_ed,
};
struct ByteClass {
  ByteClassKind kind{ByteClassKind::form};
  FormId form{kNoForm};  // valid iff kind == form, and for prefix_ignored (the base-space form executed)
  friend bool operator==(const ByteClass&, const ByteClass&) = default;
};

// Every form, in deterministic order (space order, then first opcode byte).
std::span<const FormDescriptor> all_forms();
const FormDescriptor& form_descriptor(FormId id);

// Classify one opcode byte of one canonical space. In the dd/fd spaces `prefix_ignored` carries the base-space
// form that then executes; `prefix_chain` marks DD/FD (the earlier prefix is superseded).
ByteClass classify_opcode_byte(Space space, std::uint8_t byte);

// Deterministic diagnostic names, e.g. "ld.r_ix_d.dd", "neg.a.ed.alias", "sll.r.cb.undoc".
std::string form_name(FormId id);
const char* space_name(Space space);
const char* mnemonic_name(Mnemonic mnemonic);
const char* operand_name(Operand operand);
const char* byte_class_name(ByteClassKind kind);

}  // namespace segarecomp::cpu::z80
