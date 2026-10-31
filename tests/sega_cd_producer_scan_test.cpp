// SEG-112 (ADR 0256): generation-time recognition of executable-image producers, on hand-assembled boot images (no disc, no game, no execution).
//
// Positive: the same counted-copy / byte-transform semantics are recognised with different registers, widths, destinations (main Work RAM, sub PRG RAM), control
// transfers (JSR/JMP), several fragments from one image, a transform chain, and a produced trampoline that itself produces (bounded closure).
// Negative: every operand the walk does not know, a source outside the immutable image, a provably rewritten source or destination, an unsupported destination,
// an unexecuted destination, an oversized or aliasing range, an unsupported loop body, a register-keyed transform and contradictory proofs are typed refusals that
// admit nothing. Determinism: the same images give the identical result.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/producer_scan.hpp"

namespace scd = segarecomp::machine::sega_cd;
using Bytes = std::vector<std::uint8_t>;

namespace {

int failures = 0;
void check(bool ok, const std::string &label) {
  std::cout << (ok ? "ok    " : "FAIL  ") << label << "\n";
  if (!ok) ++failures;
}

// A tiny big-endian assembler for the forms the producers use. Addresses are guest addresses of the image being assembled.
struct Asm {
  std::uint32_t base;
  Bytes out;
  explicit Asm(std::uint32_t b) : base(b) {}
  std::uint32_t here() const { return base + static_cast<std::uint32_t>(out.size()); }
  void w(std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v >> 8)); out.push_back(static_cast<std::uint8_t>(v)); }
  void l(std::uint32_t v) { w(v >> 16); w(v & 0xFFFFU); }
  void movea_l(unsigned an, std::uint32_t v) { w(0x207CU | (an << 9)); l(v); }
  void lea_abs(unsigned an, std::uint32_t v) { w(0x41F9U | (an << 9)); l(v); }
  void move_w_imm(unsigned dn, std::uint32_t v) { w(0x303CU | (dn << 9)); w(v); }
  void moveq(unsigned dn, unsigned v) { w(0x7000U | (dn << 9) | v); }
  void copy_post(char size, unsigned x, unsigned y) { w(((size == 'b' ? 1U : size == 'w' ? 3U : 2U) << 12) | (y << 9) | (3U << 6) | (3U << 3) | x); }
  void load_b(unsigned x, unsigned t) { w(0x1018U | (t << 9) | x); }
  void store_b(unsigned t, unsigned y) { w(0x10C0U | (y << 9) | t); }
  void eori_b(unsigned dn, unsigned k) { w(0x0A00U | dn); w(k); }
  void addi_b(unsigned dn, unsigned k) { w(0x0600U | dn); w(k); }
  void subi_b(unsigned dn, unsigned k) { w(0x0400U | dn); w(k); }
  void eor_dn(unsigned src, unsigned dn) { w(0xB100U | (src << 9) | dn); }   // EOR.B Ds,Dn
  void addq_w(unsigned dn) { w(0x5240U | dn); }
  void dbra(unsigned dn, std::uint32_t head) { w(0x51C8U | dn); w(head - here()); }
  void jsr(std::uint32_t a) { w(0x4EB9U); l(a); }
  void jmp(std::uint32_t a) { w(0x4EF9U); l(a); }
  void rts() { w(0x4E75U); }
  void nop() { w(0x4E71U); }
  void move_b_imm_abs(unsigned v, std::uint32_t a) { w(0x13FCU); w(v); l(a); }
  void movea_l_ind(unsigned an, unsigned src) { w(0x2050U | (an << 9) | src); }   // MOVEA.L (As),An
  void move_w_ind(unsigned dn, unsigned src) { w(0x3010U | (dn << 9) | src); }     // MOVE.W (As),Dn
  void pad_to(std::uint32_t at) { while (here() < at) nop(); }
};

constexpr std::uint32_t kIp = 0xFF0000U;
constexpr std::uint32_t kSp = 0x6000U;

scd::ProducerScan scan(const Bytes &ip, const Bytes &sp) {
  Bytes area(32768, 0);
  const std::string id = "SEGADISCSYSTEM  ";
  std::copy(id.begin(), id.end(), area.begin());
  const auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) area[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i)); };
  const std::uint32_t ip_size = static_cast<std::uint32_t>((ip.size() + 1U) & ~std::size_t{1});
  const std::uint32_t sp_size = static_cast<std::uint32_t>((sp.size() + 1U) & ~std::size_t{1});
  put32(0x30, 0x200); put32(0x34, std::max<std::uint32_t>(ip_size, 2U)); put32(0x38, 0); put32(0x3C, 0);
  put32(0x40, 0x1000); put32(0x44, std::max<std::uint32_t>(sp_size, 2U)); put32(0x48, 0); put32(0x4C, 0);
  std::copy(ip.begin(), ip.end(), area.begin() + 0x200);
  std::copy(sp.begin(), sp.end(), area.begin() + 0x1000);
  const auto header = segarecomp::parse_sega_cd_system_area(area);
  if (!header.header) { std::cout << "FAIL  header\n"; ++failures; return {}; }
  const auto boot = scd::produce_boot_images(*header.header, area);
  if (!boot.images) { std::cout << "FAIL  boot images\n"; ++failures; return {}; }
  return scd::scan_static_producers(area, *boot.images);
}

// IP: <setup> loop <transfer>; the fragment bytes sit at kIp + 0x100.
Bytes fragment_code(std::uint32_t at) {   // a few NOPs and RTS at `at`
  Asm f(at);
  for (int i = 0; i < 11; ++i) f.nop();
  f.rts();
  return f.out;
}
constexpr std::uint32_t kFrag = kIp + 0x100U;
constexpr std::uint32_t kDest = 0xFFA000U;

struct Builder {
  Asm a{kIp};
  Bytes tail;   // data at kFrag
  Bytes finish() { Bytes ip = a.out; ip.resize(0x100, 0x00); ip.insert(ip.end(), tail.begin(), tail.end()); return ip; }
};

std::size_t refused(const scd::ProducerScan &s, scd::ProducerRefusal r) { const auto it = s.refusals.find(r); return it == s.refusals.end() ? 0U : it->second; }

// the canonical copy producer with overridable parts
Bytes copy_ip(char size = 'b', unsigned ra = 0, unsigned rb = 1, unsigned rc = 0, std::uint32_t count_minus_one = 23, std::uint32_t src = kFrag, std::uint32_t dst = kDest,
              bool transfer = true, bool use_jmp = false) {
  Builder b;
  b.a.lea_abs(ra, src);
  b.a.movea_l(rb, dst);
  b.a.move_w_imm(rc, count_minus_one);
  const std::uint32_t head = b.a.here();
  b.a.copy_post(size, ra, rb);
  b.a.dbra(rc, head);
  if (transfer) { if (use_jmp) b.a.jmp(dst); else b.a.jsr(dst); }
  b.a.rts();
  b.tail = fragment_code(dst);
  b.tail.resize(64, 0x4E);
  return b.finish();
}

}  // namespace

int main() {
  const Bytes none(64, 0);
  const Bytes expect24 = fragment_code(kDest);

  // ---------------- positive ----------------
  auto s = scan(copy_ip(), none);
  check(s.fragments.size() == 1 && s.fragments[0].copy && s.fragments[0].base == kDest && s.fragments[0].length == 24 && s.fragments[0].bytes == expect24 &&
            s.fragments[0].disc_offset == 0x200U + 0x100U && s.fragments[0].processor == scd::Processor::main && s.candidates == 1,
        "byte copy loop (A0/A1/D0): source, destination, count, bytes and disc offset are derived from the code alone");
  check(scan(copy_ip(), none).fragments.size() == 1 && scan(copy_ip(), none).fragments[0].bytes == s.fragments[0].bytes, "the result is deterministic");
  s = scan(copy_ip('b', 3, 5, 6), none);
  check(s.fragments.size() == 1 && s.fragments[0].bytes == expect24, "different registers (A3/A5/D6): the same semantics are recognised, nothing depends on A0/A1/D0");
  s = scan(copy_ip('w', 2, 4, 7, 11), none);
  check(s.fragments.size() == 1 && s.fragments[0].length == 24 && s.fragments[0].bytes == expect24, "word copy loop (12 words)");
  s = scan(copy_ip('l', 0, 1, 0, 5), none);
  check(s.fragments.size() == 1 && s.fragments[0].length == 24 && s.fragments[0].bytes == expect24, "long copy loop (6 longs)");
  s = scan(copy_ip('b', 0, 1, 0, 23, kFrag, kDest, true, true), none);
  check(s.fragments.size() == 1, "a JMP to the destination proves executable use as well as a JSR");
  s = scan(copy_ip('b', 0, 1, 0, 20), none);
  check(s.fragments.size() == 1 && s.fragments[0].length == 20 && s.fragments[0].bytes == Bytes(expect24.begin(), expect24.begin() + 20), "a short fragment (21 bytes copied): the even prefix is claimed");
  {   // sub PRG RAM destination
    Asm a(kSp);
    a.lea_abs(0, kSp + 0x100); a.movea_l(1, 0x30000); a.move_w_imm(0, 23);
    const std::uint32_t head = a.here();
    a.copy_post('b', 0, 1); a.dbra(0, head); a.jsr(0x30000); a.rts();
    Bytes sp = a.out; sp.resize(0x100, 0);
    const Bytes frag = fragment_code(0x30000);
    sp.insert(sp.end(), frag.begin(), frag.end()); sp.resize(0x180, 0x4E);
    s = scan(none, sp);
    check(s.fragments.size() == 1 && s.fragments[0].processor == scd::Processor::sub && s.fragments[0].base == 0x30000U && s.fragments[0].bytes == frag && s.fragments[0].disc_offset == 0x1000U + 0x100U,
          "sub PRG RAM destination (SP image)");
  }
  {   // two fragments from one image
    Builder b;
    for (int k = 0; k < 2; ++k) {
      b.a.lea_abs(0, kFrag + 32U * static_cast<unsigned>(k)); b.a.movea_l(1, kDest + 0x100U * static_cast<unsigned>(k)); b.a.move_w_imm(0, 15);
      const std::uint32_t head = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, head); b.a.jsr(kDest + 0x100U * static_cast<unsigned>(k));
    }
    b.a.rts();
    Asm t(kFrag); for (int i = 0; i < 7; ++i) t.nop(); t.rts(); for (int i = 0; i < 8; ++i) t.nop(); t.rts();
    b.tail = t.out;
    s = scan(b.finish(), none);
    check(s.fragments.size() == 2 && s.candidates == 2, "two copies from one immutable image: both proven");
  }
  {   // transforms
    const auto xform = [&](int op, unsigned key, bool chain) {
      Builder b;
      b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
      const std::uint32_t head = b.a.here();
      b.a.load_b(0, 2);
      if (op == 0) b.a.eori_b(2, key); else if (op == 1) b.a.addi_b(2, key); else b.a.subi_b(2, key);
      if (chain) b.a.addi_b(2, 3);
      b.a.store_b(2, 1);
      b.a.dbra(0, head); b.a.jsr(kDest); b.a.rts();
      Bytes stored = expect24;
      for (auto &x : stored) {
        if (chain) x = static_cast<std::uint8_t>(x - 3);
        x = static_cast<std::uint8_t>(op == 0 ? (x ^ key) : op == 1 ? (x - key) : (x + key));
      }
      b.tail = stored; b.tail.resize(64, 0x4E);
      return scan(b.finish(), none);
    };
    for (int op = 0; op < 3; ++op) {
      s = xform(op, 0x5A, false);
      check(s.fragments.size() == 1 && !s.fragments[0].copy && s.fragments[0].bytes == expect24 && s.fragments[0].disc_offset == 0,
            std::string("byte transform recognised from the code and evaluated at generation time: ") + (op == 0 ? "eor" : op == 1 ? "add" : "sub") + " (output is on no disc span)");
    }
    s = xform(0, 0x33, true);
    check(s.fragments.size() == 1 && s.fragments[0].bytes == expect24 && s.fragments[0].transform.find(',') != std::string::npos, "a two-operation chain (eor then add) is evaluated");
  }
  {   // closure: the produced trampoline itself produces a second image
    Builder b;
    b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 63);
    const std::uint32_t head = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, head); b.a.jsr(kDest); b.a.rts();
    Asm t(kDest);   // the trampoline: copies the bytes at kFrag + 64 to 0xFFB000 and calls them
    t.lea_abs(2, kFrag + 64); t.movea_l(3, 0xFFB000U); t.move_w_imm(1, 7);
    const std::uint32_t th = t.here(); t.copy_post('b', 2, 3); t.dbra(1, th); t.jsr(0xFFB000U); t.rts();
    Bytes tramp = t.out; tramp.resize(64, 0x4E);
    Bytes inner = fragment_code(0xFFB000U); inner.resize(8, 0x4E);
    b.tail = tramp; b.tail.insert(b.tail.end(), inner.begin(), inner.end());
    s = scan(b.finish(), none);
    check(s.fragments.size() == 2 && s.fragments[0].base == kDest && s.fragments[1].base == 0xFFB000U, "bounded closure: a produced trampoline that produces a second image is analysed before any code is generated");
  }

  // ---------------- negative ----------------
  const auto none_proven = [&](const scd::ProducerScan &r, scd::ProducerRefusal why, const std::string &label) {
    check(r.fragments.empty() && refused(r, why) >= 1, label + " -> " + scd::producer_refusal_name(why));
  };
  {   // unknown source / destination / count
    Builder b; b.a.movea_l_ind(0, 6); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::loop_operand_unknown, "unknown source pointer");
  }
  {
    Builder b; b.a.lea_abs(0, kFrag); b.a.movea_l_ind(1, 6); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::loop_operand_unknown, "unknown destination pointer");
  }
  {
    Builder b; b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_ind(0, 5);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::loop_operand_unknown, "unknown (input-dependent) loop count");
  }
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, 0xFF8000U), none), scd::ProducerRefusal::source_not_immutable, "source in mutable RAM outside the boot image");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, 0x00000100U), none), scd::ProducerRefusal::source_not_immutable, "source in the BIOS window, not the immutable boot image");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, 0xC00000U), none), scd::ProducerRefusal::destination_unsupported, "destination is a device (VDP)");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, 0x200000U), none), scd::ProducerRefusal::destination_unsupported, "destination in Word RAM (no bank justified)");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, kDest + 1U), none), scd::ProducerRefusal::destination_unsupported, "odd destination");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, kDest, false), none), scd::ProducerRefusal::not_executed, "destination never executed (no control transfer lands in it)");
  none_proven(scan(copy_ip('b', 0, 1, 0, 4999), none), scd::ProducerRefusal::too_large, "more than the 4 KiB bound");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, kFrag + 8U), none), scd::ProducerRefusal::aliasing, "source and destination overlap (aliasing)");
  none_proven(scan(copy_ip('b', 0, 1, 0, 23, kFrag, kIp + 0x400U), none), scd::ProducerRefusal::aliasing, "destination inside the immutable image (self-modification)");
  {   // source written before the copy
    Builder b; b.a.move_b_imm_abs(0x11, kFrag + 3);
    b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::source_written, "the source is provably modified before the producer reads it");
  }
  {   // destination overwritten after the copy
    Builder b; b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.move_b_imm_abs(0x22, kDest + 4); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::destination_overwritten, "a previously produced image is provably overwritten");
  }
  {   // unsupported loop body
    Builder b; b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.addq_w(3); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::loop_body_unsupported, "a loop body with an extra effect");
  }
  {   // register-keyed (input-dependent) transform
    Builder b; b.a.lea_abs(0, kFrag); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 23);
    const std::uint32_t h = b.a.here(); b.a.load_b(0, 2); b.a.eor_dn(3, 2); b.a.store_b(2, 1); b.a.dbra(0, h); b.a.jsr(kDest); b.a.rts(); b.tail = fragment_code(kDest);
    none_proven(scan(b.finish(), none), scd::ProducerRefusal::loop_body_unsupported, "a transform whose key is a register value, not an immediate");
  }
  {   // contradictory proofs for one destination
    Builder b;
    for (int k = 0; k < 2; ++k) {
      b.a.lea_abs(0, kFrag + 32U * static_cast<unsigned>(k)); b.a.movea_l(1, kDest); b.a.move_w_imm(0, 15);
      const std::uint32_t h = b.a.here(); b.a.copy_post('b', 0, 1); b.a.dbra(0, h); b.a.jsr(kDest);
    }
    b.a.rts();
    Asm t(kFrag); for (int i = 0; i < 7; ++i) t.nop(); t.rts(); for (int i = 0; i < 7; ++i) t.nop(); t.w(0x4E70);   // second span differs
    b.tail = t.out;
    s = scan(b.finish(), none);
    check(refused(s, scd::ProducerRefusal::overlapping_fragments) >= 1 || refused(s, scd::ProducerRefusal::destination_overwritten) >= 1, "two proofs claim the same bytes differently: refused");
  }
  {   // garbage / data in the boot images: no candidates, no fragment, no crash
    Bytes junk(0x300);
    for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t>(i * 37U + 11U);
    s = scan(junk, junk);
    check(s.fragments.empty(), "arbitrary bytes admit nothing");
  }
  std::cout << failures << " failures\n";
  return failures != 0;
}
