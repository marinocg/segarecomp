#include "segarecomp/cpu/z80/forms.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

namespace segarecomp::cpu::z80 {
namespace {

// A form identity before interning. `alias_space` names the space of the documented form this encoding aliases.
struct Key {
  Space space{Space::base};
  Mnemonic mnemonic{Mnemonic::nop};
  Operand dst{Operand::none};
  Operand src{Operand::none};
  bool documented{true};
  bool has_alias{false};
  Space alias_space{Space::base};
  friend bool operator==(const Key&, const Key&) = default;
};

struct Classified {
  ByteClassKind kind{ByteClassKind::form};
  Key key{};  // for form, and for prefix_ignored the base-space form that executes
};

constexpr std::array<Mnemonic, 8> kAlu = {Mnemonic::add, Mnemonic::adc, Mnemonic::sub, Mnemonic::sbc,
                                          Mnemonic::and_, Mnemonic::xor_, Mnemonic::or_, Mnemonic::cp};
constexpr std::array<Mnemonic, 8> kShift = {Mnemonic::rlc, Mnemonic::rrc, Mnemonic::rl, Mnemonic::rr,
                                            Mnemonic::sla, Mnemonic::sra, Mnemonic::sll, Mnemonic::srl};

Key make(Space space, Mnemonic mnemonic, Operand dst, Operand src, bool documented = true) {
  Key key;
  key.space = space;
  key.mnemonic = mnemonic;
  key.dst = dst;
  key.src = src;
  key.documented = documented;
  return key;
}

Key alias(Key key, Space target_space) {
  key.documented = false;
  key.has_alias = true;
  key.alias_space = target_space;
  return key;
}

Classified form(Key key) { return {ByteClassKind::form, key}; }

Classified classify_base(std::uint8_t b) {
  const unsigned x = b >> 6, y = (b >> 3) & 7, z = b & 7, p = y >> 1, q = y & 1;
  auto mk = [](Mnemonic m, Operand d, Operand s) { return form(make(Space::base, m, d, s)); };
  switch (x) {
    case 0:
      switch (z) {
        case 0:
          if (y == 0) return mk(Mnemonic::nop, Operand::none, Operand::none);
          if (y == 1) return mk(Mnemonic::ex, Operand::af, Operand::af_alt);
          if (y == 2) return mk(Mnemonic::djnz, Operand::none, Operand::e);
          if (y == 3) return mk(Mnemonic::jr, Operand::none, Operand::e);
          return mk(Mnemonic::jr, Operand::cc4, Operand::e);
        case 1:
          return q == 0 ? mk(Mnemonic::ld, Operand::rr, Operand::nn) : mk(Mnemonic::add, Operand::hl, Operand::rr);
        case 2:
          if (q == 0) {
            if (p < 2) return mk(Mnemonic::ld, Operand::rr_ind, Operand::a);
            return p == 2 ? mk(Mnemonic::ld, Operand::nn_ind, Operand::hl)
                          : mk(Mnemonic::ld, Operand::nn_ind, Operand::a);
          }
          if (p < 2) return mk(Mnemonic::ld, Operand::a, Operand::rr_ind);
          return p == 2 ? mk(Mnemonic::ld, Operand::hl, Operand::nn_ind)
                        : mk(Mnemonic::ld, Operand::a, Operand::nn_ind);
        case 3:
          return mk(q == 0 ? Mnemonic::inc : Mnemonic::dec, Operand::rr, Operand::none);
        case 4:
          return mk(Mnemonic::inc, y == 6 ? Operand::hl_ind : Operand::r, Operand::none);
        case 5:
          return mk(Mnemonic::dec, y == 6 ? Operand::hl_ind : Operand::r, Operand::none);
        case 6:
          return mk(Mnemonic::ld, y == 6 ? Operand::hl_ind : Operand::r, Operand::n);
        default: {
          constexpr std::array<Mnemonic, 8> ops = {Mnemonic::rlca, Mnemonic::rrca, Mnemonic::rla, Mnemonic::rra,
                                                   Mnemonic::daa, Mnemonic::cpl, Mnemonic::scf, Mnemonic::ccf};
          return mk(ops[y], Operand::a, Operand::none);
        }
      }
    case 1:
      if (z == 6 && y == 6) return mk(Mnemonic::halt, Operand::none, Operand::none);
      if (z == 6) return mk(Mnemonic::ld, Operand::r, Operand::hl_ind);
      if (y == 6) return mk(Mnemonic::ld, Operand::hl_ind, Operand::r);
      return mk(Mnemonic::ld, Operand::r, Operand::r);
    case 2:
      return mk(kAlu[y], Operand::a, z == 6 ? Operand::hl_ind : Operand::r);
    default:
      switch (z) {
        case 0: return mk(Mnemonic::ret, Operand::cc, Operand::none);
        case 1:
          if (q == 0) return mk(Mnemonic::pop, Operand::rr2, Operand::none);
          if (p == 0) return mk(Mnemonic::ret, Operand::none, Operand::none);
          if (p == 1) return mk(Mnemonic::exx, Operand::none, Operand::none);
          if (p == 2) return mk(Mnemonic::jp, Operand::none, Operand::hl);
          return mk(Mnemonic::ld, Operand::sp, Operand::hl);
        case 2: return mk(Mnemonic::jp, Operand::cc, Operand::nn);
        case 3:
          switch (y) {
            case 0: return mk(Mnemonic::jp, Operand::none, Operand::nn);
            case 1: return {ByteClassKind::escape_cb, {}};
            case 2: return mk(Mnemonic::out, Operand::n_port, Operand::a);
            case 3: return mk(Mnemonic::in, Operand::a, Operand::n_port);
            case 4: return mk(Mnemonic::ex, Operand::sp_ind, Operand::hl);
            case 5: return mk(Mnemonic::ex, Operand::de, Operand::hl);
            case 6: return mk(Mnemonic::di, Operand::none, Operand::none);
            default: return mk(Mnemonic::ei, Operand::none, Operand::none);
          }
        case 4: return mk(Mnemonic::call, Operand::cc, Operand::nn);
        case 5:
          if (q == 0) return mk(Mnemonic::push, Operand::none, Operand::rr2);
          if (p == 0) return mk(Mnemonic::call, Operand::none, Operand::nn);
          if (p == 1) return {ByteClassKind::escape_dd, {}};
          if (p == 2) return {ByteClassKind::escape_ed, {}};
          return {ByteClassKind::escape_fd, {}};
        case 6: return mk(kAlu[y], Operand::a, Operand::n);
        default: return mk(Mnemonic::rst, Operand::none, Operand::p);
      }
  }
}

Classified classify_cb(std::uint8_t b) {
  const unsigned x = b >> 6, y = (b >> 3) & 7, z = b & 7;
  const Operand target = z == 6 ? Operand::hl_ind : Operand::r;
  if (x == 0) return form(make(Space::cb, kShift[y], target, Operand::none, kShift[y] != Mnemonic::sll));
  const Mnemonic m = x == 1 ? Mnemonic::bit : x == 2 ? Mnemonic::res : Mnemonic::set;
  return form(make(Space::cb, m, Operand::bit, target));
}

Classified classify_ed(std::uint8_t b) {
  auto nop = [] { return form(make(Space::ed, Mnemonic::nop, Operand::none, Operand::ed_undefined, false)); };
  auto mk = [](Mnemonic m, Operand d, Operand s) { return form(make(Space::ed, m, d, s)); };
  if (b >= 0x40 && b < 0x80) {
    const unsigned y = (b >> 3) & 7, z = b & 7, p = y >> 1, q = y & 1;
    switch (z) {
      case 0:
        if (y == 6) return form(make(Space::ed, Mnemonic::in, Operand::flags_only, Operand::c_port, false));
        return mk(Mnemonic::in, Operand::r, Operand::c_port);
      case 1:
        if (y == 6) return form(make(Space::ed, Mnemonic::out, Operand::c_port, Operand::zero, false));
        return mk(Mnemonic::out, Operand::c_port, Operand::r);
      case 2: return mk(q == 0 ? Mnemonic::sbc : Mnemonic::adc, Operand::hl, Operand::rr);
      case 3:
        if (p == 2) {  // documented encoding of the base-space form
          Key k = q == 0 ? make(Space::ed, Mnemonic::ld, Operand::nn_ind, Operand::hl)
                         : make(Space::ed, Mnemonic::ld, Operand::hl, Operand::nn_ind);
          k.has_alias = true;
          k.alias_space = Space::base;
          return form(k);
        }
        return q == 0 ? mk(Mnemonic::ld, Operand::nn_ind, Operand::rr_ed) : mk(Mnemonic::ld, Operand::rr_ed, Operand::nn_ind);
      case 4: {
        Key k = make(Space::ed, Mnemonic::neg, Operand::a, Operand::none);
        return form(y == 0 ? k : alias(k, Space::ed));
      }
      case 5: {
        if (y == 1) return mk(Mnemonic::reti, Operand::none, Operand::none);
        Key k = make(Space::ed, Mnemonic::retn, Operand::none, Operand::none);
        return form(y == 0 ? k : alias(k, Space::ed));
      }
      case 6: {
        static constexpr std::array<Operand, 8> mode = {Operand::im0, Operand::im0, Operand::im1, Operand::im2,
                                                        Operand::im0, Operand::im0, Operand::im1, Operand::im2};
        Key k = make(Space::ed, Mnemonic::im, mode[y], Operand::none);
        return form(y == 0 || y == 2 || y == 3 ? k : alias(k, Space::ed));
      }
      default:
        switch (y) {
          case 0: return mk(Mnemonic::ld, Operand::i, Operand::a);
          case 1: return mk(Mnemonic::ld, Operand::r_refresh, Operand::a);
          case 2: return mk(Mnemonic::ld, Operand::a, Operand::i);
          case 3: return mk(Mnemonic::ld, Operand::a, Operand::r_refresh);
          case 4: return mk(Mnemonic::rrd, Operand::a, Operand::hl_ind);
          case 5: return mk(Mnemonic::rld, Operand::a, Operand::hl_ind);
          default: return nop();
        }
    }
  }
  if (b >= 0xA0 && b < 0xC0 && (b & 4) == 0) {
    const unsigned y = (b >> 3) & 7, z = b & 3;  // y 4..7
    static constexpr Mnemonic block[4][4] = {
        {Mnemonic::ldi, Mnemonic::cpi, Mnemonic::ini, Mnemonic::outi},
        {Mnemonic::ldd, Mnemonic::cpd, Mnemonic::ind, Mnemonic::outd},
        {Mnemonic::ldir, Mnemonic::cpir, Mnemonic::inir, Mnemonic::otir},
        {Mnemonic::lddr, Mnemonic::cpdr, Mnemonic::indr, Mnemonic::otdr}};
    return mk(block[y - 4][z], Operand::none, Operand::none);
  }
  return nop();
}

Classified classify_index(Space space, std::uint8_t b) {
  const bool iy = space == Space::fd;
  const Operand ix = iy ? Operand::iy : Operand::ix, ixd = iy ? Operand::iy_d : Operand::ix_d,
                half = iy ? Operand::iy_half : Operand::ix_half, rr_ix = iy ? Operand::rr_iy : Operand::rr_ix;
  auto mk = [&](Mnemonic m, Operand d, Operand s, bool doc = true) { return form(make(space, m, d, s, doc)); };
  auto ignored = [&] {
    Classified c = classify_base(b);
    return Classified{ByteClassKind::prefix_ignored, c.key};
  };
  switch (b) {
    case 0x09: case 0x19: case 0x29: case 0x39: return mk(Mnemonic::add, ix, rr_ix);
    case 0x21: return mk(Mnemonic::ld, ix, Operand::nn);
    case 0x22: return mk(Mnemonic::ld, Operand::nn_ind, ix);
    case 0x23: return mk(Mnemonic::inc, ix, Operand::none);
    case 0x24: case 0x2C: return mk(Mnemonic::inc, half, Operand::none, false);
    case 0x25: case 0x2D: return mk(Mnemonic::dec, half, Operand::none, false);
    case 0x26: case 0x2E: return mk(Mnemonic::ld, half, Operand::n, false);
    case 0x2A: return mk(Mnemonic::ld, ix, Operand::nn_ind);
    case 0x2B: return mk(Mnemonic::dec, ix, Operand::none);
    case 0x34: return mk(Mnemonic::inc, ixd, Operand::none);
    case 0x35: return mk(Mnemonic::dec, ixd, Operand::none);
    case 0x36: return mk(Mnemonic::ld, ixd, Operand::n);
    case 0xCB: return {iy ? ByteClassKind::escape_fdcb : ByteClassKind::escape_ddcb, {}};
    case 0xDD: case 0xFD: return {ByteClassKind::prefix_chain, {}};
    case 0xED: return {ByteClassKind::prefix_ignored_before_ed, {}};
    case 0xE1: return mk(Mnemonic::pop, ix, Operand::none);
    case 0xE3: return mk(Mnemonic::ex, Operand::sp_ind, ix);
    case 0xE5: return mk(Mnemonic::push, Operand::none, ix);
    case 0xE9: return mk(Mnemonic::jp, Operand::none, ix);
    case 0xF9: return mk(Mnemonic::ld, Operand::sp, ix);
    default: break;
  }
  const unsigned x = b >> 6, y = (b >> 3) & 7, z = b & 7;
  const bool y_half = y == 4 || y == 5, z_half = z == 4 || z == 5;
  if (x == 1 && b != 0x76) {
    if (z == 6) return mk(Mnemonic::ld, Operand::r, ixd);
    if (y == 6) return mk(Mnemonic::ld, ixd, Operand::r);
    if (y_half && z_half) return mk(Mnemonic::ld, half, half, false);
    if (y_half) return mk(Mnemonic::ld, half, Operand::r, false);
    if (z_half) return mk(Mnemonic::ld, Operand::r, half, false);
  } else if (x == 2) {
    if (z == 6) return mk(kAlu[y], Operand::a, ixd);
    if (z_half) return mk(kAlu[y], Operand::a, half, false);
  }
  return ignored();
}

Classified classify_indexed_cb(Space space, std::uint8_t b) {
  const bool iy = space == Space::fdcb;
  const Operand ixd = iy ? Operand::iy_d : Operand::ix_d, copy = iy ? Operand::iy_d_copy_r : Operand::ix_d_copy_r;
  const unsigned x = b >> 6, y = (b >> 3) & 7, z = b & 7;
  if (x == 0) {
    const Mnemonic m = kShift[y];
    if (z == 6) return form(make(space, m, ixd, Operand::none, m != Mnemonic::sll));
    return form(make(space, m, copy, Operand::none, false));
  }
  if (x == 1) {
    Key k = make(space, Mnemonic::bit, Operand::bit, ixd);
    return form(z == 6 ? k : alias(k, space));
  }
  const Mnemonic m = x == 2 ? Mnemonic::res : Mnemonic::set;
  if (z == 6) return form(make(space, m, Operand::bit, ixd));
  return form(make(space, m, Operand::bit, copy, false));
}

Classified classify_key(Space space, std::uint8_t b) {
  switch (space) {
    case Space::base: return classify_base(b);
    case Space::cb: return classify_cb(b);
    case Space::ed: return classify_ed(b);
    case Space::dd:
    case Space::fd: return classify_index(space, b);
    default: return classify_indexed_cb(space, b);
  }
}

std::uint8_t immediate_bytes(const Key& k) {
  auto size = [](Operand o) -> std::uint8_t {
    switch (o) {
      case Operand::n: case Operand::e: case Operand::n_port: return 1;
      case Operand::nn: case Operand::nn_ind: return 2;
      default: return 0;
    }
  };
  return static_cast<std::uint8_t>(size(k.dst) + size(k.src));
}

bool is_indexed_d(Operand o) {
  return o == Operand::ix_d || o == Operand::iy_d || o == Operand::ix_d_copy_r || o == Operand::iy_d_copy_r;
}

Timing fixed(unsigned t) { return {TimingClass::fixed, static_cast<std::uint16_t>(t), 0}; }
Timing cond(unsigned not_taken, unsigned taken) {
  return {TimingClass::conditional, static_cast<std::uint16_t>(not_taken), static_cast<std::uint16_t>(taken)};
}

bool is_alu(Mnemonic m) {
  return m == Mnemonic::add || m == Mnemonic::adc || m == Mnemonic::sub || m == Mnemonic::sbc ||
         m == Mnemonic::and_ || m == Mnemonic::xor_ || m == Mnemonic::or_ || m == Mnemonic::cp;
}

// Static NMOS T-states from the public UM0080 / Young tables, by (space, mnemonic, operand classes).
Timing timing_of(const Key& k) {
  const Mnemonic m = k.mnemonic;
  const Operand d = k.dst, s = k.src;
  switch (k.space) {
    case Space::base:
      switch (m) {
        case Mnemonic::halt: return {TimingClass::halt, 4, 4};
        case Mnemonic::djnz: return cond(8, 13);
        case Mnemonic::jr: return d == Operand::cc4 ? cond(7, 12) : fixed(12);
        case Mnemonic::ret: return d == Operand::cc ? cond(5, 11) : fixed(10);
        case Mnemonic::call: return d == Operand::cc ? cond(10, 17) : fixed(17);
        case Mnemonic::jp: return s == Operand::hl ? fixed(4) : fixed(10);
        case Mnemonic::push: return fixed(11);
        case Mnemonic::pop: return fixed(10);
        case Mnemonic::rst: return fixed(11);
        case Mnemonic::out: case Mnemonic::in: return fixed(11);
        case Mnemonic::ex: return d == Operand::sp_ind ? fixed(19) : fixed(4);
        case Mnemonic::add:
          if (d == Operand::hl) return fixed(11);
          break;
        case Mnemonic::inc: case Mnemonic::dec:
          if (d == Operand::rr) return fixed(6);
          if (d == Operand::hl_ind) return fixed(11);
          return fixed(4);
        case Mnemonic::ld:
          if (d == Operand::rr) return fixed(10);
          if (d == Operand::sp) return fixed(6);
          if (d == Operand::rr_ind || s == Operand::rr_ind) return fixed(7);
          if (d == Operand::nn_ind || s == Operand::nn_ind) return fixed(d == Operand::hl || s == Operand::hl ? 16 : 13);
          if (d == Operand::hl_ind && s == Operand::n) return fixed(10);
          if (d == Operand::hl_ind || s == Operand::hl_ind || s == Operand::n) return fixed(7);
          return fixed(4);
        default: break;
      }
      if (is_alu(m)) return fixed(s == Operand::hl_ind || s == Operand::n ? 7 : 4);
      return fixed(4);
    case Space::cb:
      if (m == Mnemonic::bit) return fixed(s == Operand::hl_ind ? 12 : 8);
      return fixed(d == Operand::hl_ind || s == Operand::hl_ind ? 15 : 8);
    case Space::ed:
      switch (m) {
        case Mnemonic::nop: case Mnemonic::neg: case Mnemonic::im: return fixed(8);
        case Mnemonic::in: case Mnemonic::out: return fixed(12);
        case Mnemonic::sbc: case Mnemonic::adc: return fixed(15);
        case Mnemonic::ld: return (d == Operand::nn_ind || s == Operand::nn_ind) ? fixed(20) : fixed(9);
        case Mnemonic::retn: case Mnemonic::reti: return fixed(14);
        case Mnemonic::rrd: case Mnemonic::rld: return fixed(18);
        case Mnemonic::ldir: case Mnemonic::cpir: case Mnemonic::inir: case Mnemonic::otir:
        case Mnemonic::lddr: case Mnemonic::cpdr: case Mnemonic::indr: case Mnemonic::otdr:
          return {TimingClass::repeat, 16, 21};
        default: return fixed(16);  // single-iteration block instructions
      }
    case Space::dd:
    case Space::fd: {
      const bool ixd = is_indexed_d(d) || is_indexed_d(s);
      const bool half = d == Operand::ix_half || d == Operand::iy_half || s == Operand::ix_half || s == Operand::iy_half;
      if (is_alu(m) && d == Operand::a) return fixed(ixd ? 19 : 8);
      switch (m) {
        case Mnemonic::add: return fixed(15);  // ADD IX,rr
        case Mnemonic::ld:
          if (ixd) return fixed(19);
          if (half) return fixed(s == Operand::n ? 11 : 8);
          if (d == Operand::nn_ind || s == Operand::nn_ind) return fixed(20);
          if (d == Operand::sp) return fixed(10);
          return fixed(14);  // LD IX,nn
        case Mnemonic::inc: case Mnemonic::dec: return fixed(ixd ? 23 : half ? 8 : 10);
        case Mnemonic::pop: return fixed(14);
        case Mnemonic::push: return fixed(15);
        case Mnemonic::ex: return fixed(23);
        default: return fixed(8);  // JP (IX)
      }
    }
    default:  // ddcb / fdcb
      return fixed(m == Mnemonic::bit ? 20 : 23);
  }
}

struct Tables {
  std::vector<FormDescriptor> forms;
  std::array<std::array<ByteClass, 256>, kSpaceCount> bytes{};
};

FormId intern(std::vector<Key>& keys, const Key& key) {
  const auto it = std::find(keys.begin(), keys.end(), key);
  if (it != keys.end()) return static_cast<FormId>(it - keys.begin());
  keys.push_back(key);
  return static_cast<FormId>(keys.size() - 1);
}

Tables build() {
  Tables t;
  std::vector<Key> keys;
  std::vector<Classified> classified;
  for (std::size_t sp = 0; sp < kSpaceCount; ++sp) {
    for (unsigned b = 0; b < 256; ++b) {
      const Space space = static_cast<Space>(sp);
      const Classified c = classify_key(space, static_cast<std::uint8_t>(b));
      ByteClass bc{c.kind, kNoForm};
      if (c.kind == ByteClassKind::form || c.kind == ByteClassKind::prefix_ignored)
        bc.form = intern(keys, c.key);
      t.bytes[sp][b] = bc;
    }
  }
  // Alias targets: the documented form of the alias space with the same operand classes.
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const Key& k = keys[i];
    FormDescriptor f;
    f.id = static_cast<FormId>(i);
    f.space = k.space;
    f.mnemonic = k.mnemonic;
    f.dst = k.dst;
    f.src = k.src;
    f.documented = k.documented;
    f.m1_fetches = k.space == Space::base ? 1 : 2;
    const bool indexed_cb = k.space == Space::ddcb || k.space == Space::fdcb;
    const bool prefixed = k.space != Space::base;
    f.opcode_index = indexed_cb ? 3 : prefixed ? 1 : 0;
    std::uint8_t next = static_cast<std::uint8_t>(f.opcode_index + 1);
    if (indexed_cb) {
      f.displacement_index = 2;
      f.length = 4;
    } else {
      if (is_indexed_d(k.dst) || is_indexed_d(k.src)) f.displacement_index = next++;
      const std::uint8_t imm = immediate_bytes(k);
      if (imm != 0) {
        f.immediate_index = next;
        f.immediate_size = imm;
        next = static_cast<std::uint8_t>(next + imm);
      }
      f.length = next;
    }
    f.timing = timing_of(k);
    t.forms.push_back(f);
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (!keys[i].has_alias) continue;
    Key target = keys[i];
    target.space = keys[i].alias_space;
    target.documented = true;
    target.has_alias = false;
    FormId found = kNoForm;
    for (std::size_t j = 0; j < keys.size(); ++j) {
      if (keys[j].space == target.space && keys[j].mnemonic == target.mnemonic && keys[j].dst == target.dst &&
          keys[j].src == target.src && keys[j].documented && !keys[j].has_alias) {
        found = static_cast<FormId>(j);
        break;
      }
    }
    t.forms[i].alias_of = found;
  }
  return t;
}

const Tables& tables() {
  static const Tables t = build();
  return t;
}

constexpr std::array<const char*, 7> kSpaceNames = {"base", "cb", "ed", "dd", "fd", "ddcb", "fdcb"};
constexpr std::array<const char*, 68> kMnemonicNames = {
    "NOP", "LD", "INC", "DEC", "RLCA", "RRCA", "RLA", "RRA", "EX", "ADD", "DJNZ", "JR", "DAA", "CPL", "SCF", "CCF",
    "HALT", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP", "RET", "POP", "JP", "CALL", "PUSH", "RST", "EXX", "OUT",
    "IN", "DI", "EI", "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL", "BIT", "RES", "SET", "NEG", "RETN",
    "RETI", "IM", "RRD", "RLD", "LDI", "CPI", "INI", "OUTI", "LDD", "CPD", "IND", "OUTD", "LDIR", "CPIR", "INIR",
    "OTIR", "LDDR", "CPDR", "INDR", "OTDR"};
constexpr std::array<const char*, 42> kOperandNames = {
    "none", "a", "r", "rr", "rr2", "rr_ind", "hl", "hl_ind", "de", "sp", "sp_ind", "nn", "n", "e", "p", "cc", "cc4",
    "bit", "af", "af_alt", "i", "r_refresh", "im0", "im1", "im2", "n_port", "c_port", "nn_ind", "flags_only", "zero",
    "ed_undefined", "rr_ed", "ix", "iy", "ix_d", "iy_d", "ix_half", "iy_half", "rr_ix", "rr_iy", "ix_d_copy_r",
    "iy_d_copy_r"};
static_assert(static_cast<std::size_t>(Mnemonic::otdr) + 1 == kMnemonicNames.size());
static_assert(static_cast<std::size_t>(Operand::iy_d_copy_r) + 1 == kOperandNames.size());

}  // namespace

std::span<const FormDescriptor> all_forms() { return tables().forms; }

const FormDescriptor& form_descriptor(FormId id) {
  assert(id < tables().forms.size());
  return tables().forms[id];
}

ByteClass classify_opcode_byte(Space space, std::uint8_t byte) {
  return tables().bytes[static_cast<std::size_t>(space)][byte];
}

const char* space_name(Space space) { return kSpaceNames[static_cast<std::size_t>(space)]; }
const char* mnemonic_name(Mnemonic mnemonic) { return kMnemonicNames[static_cast<std::size_t>(mnemonic)]; }
const char* operand_name(Operand operand) { return kOperandNames[static_cast<std::size_t>(operand)]; }

const char* byte_class_name(ByteClassKind kind) {
  switch (kind) {
    case ByteClassKind::form: return "form";
    case ByteClassKind::escape_cb: return "escape_cb";
    case ByteClassKind::escape_ed: return "escape_ed";
    case ByteClassKind::escape_dd: return "escape_dd";
    case ByteClassKind::escape_fd: return "escape_fd";
    case ByteClassKind::escape_ddcb: return "escape_ddcb";
    case ByteClassKind::escape_fdcb: return "escape_fdcb";
    case ByteClassKind::prefix_chain: return "prefix_chain";
    case ByteClassKind::prefix_ignored: return "prefix_ignored";
    case ByteClassKind::prefix_ignored_before_ed: return "prefix_ignored_before_ed";
  }
  return "?";
}

std::string form_name(FormId id) {
  const FormDescriptor& f = form_descriptor(id);
  std::string name = mnemonic_name(f.mnemonic);
  std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(c | 0x20); });
  std::string operands;
  if (f.dst != Operand::none) operands = operand_name(f.dst);
  if (f.src != Operand::none) operands += (operands.empty() ? "" : "_") + std::string(operand_name(f.src));
  if (!operands.empty()) name += "." + operands;
  name += std::string(".") + space_name(f.space);
  if (f.alias_of != kNoForm && !f.documented) name += ".alias";
  else if (!f.documented) name += ".undoc";
  return name;
}

}  // namespace segarecomp::cpu::z80
