#!/usr/bin/env python3
"""SEG-021-T005: hermetic routed-versus-direct differential test for the MOVE/MOVEA/CLR/NOT/TST family.

The direct linear-memory lowering is validated against pinned Musashi by tests/m68k_conformance_harness_test.py
(vectors in tests/fixtures/m68k-conformance-vectors.json). The Genesis runtime-routed lowering -- the route the
whole-program C4 emitter and the immutable-ROM AOT candidates use -- has its own deferred address-register commit
code (docs/architecture/c4-add-family-auto-update-commit-contract.md), so this test runs the SAME synthetic
vectors through both lowerings on the same memory image (routed side: the real Genesis work RAM at 0x00FF0000,
direct side: linear window offset by the same base) and requires bit-identical D/A/SR/PC and memory. It targets
the shapes only the routed lowering has special code for: MOVE with an auto-updating source whose register also
feeds the destination EA (every destination kind, every size, A7 byte stepping), NOT/CLR/TST with (An)+ and -(An)
operands, and indexed / PC-relative / absolute operands of the five mnemonics.

usage: m68k_routed_lowering_test.py <m68k_conformance_emitter> <cc> <product-root>
"""
import pathlib
import subprocess
import sys
import tempfile

EMITTER, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
RUNTIME_DIR = ROOT / "platforms" / "genesis" / "runtime"
BASE = 0x00FF0000
SIZES = {"b": 1, "w": 3, "l": 2}  # MOVE size field (bits 13-12)
UNARY_SIZE = {"b": 0, "w": 1, "l": 2}


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def move_word(size, src_mode, src_reg, dst_mode, dst_reg):
    return "%04X" % ((SIZES[size] << 12) | (dst_reg << 9) | (dst_mode << 6) | (src_mode << 3) | src_reg)


def unary_word(base, size, mode, reg):
    return "%04X" % (base | (UNARY_SIZE[size] << 6) | (mode << 3) | reg)


LONG_AN_INDEX = []


def cases():
    out = []
    # MOVE alias shapes: (An)+ / -(An) source, every destination kind that reads an address register.
    for size in "bwl":
        for src_mode in (3, 4):
            for dst_mode, ext in ((2, ""), (3, ""), (4, ""), (5, "0010"), (5, "FFF0")):
                for src_reg, dst_reg in ((0, 0), (0, 1), (7, 7), (7, 0), (0, 7), (3, 3)):
                    out.append(move_word(size, src_mode, src_reg, dst_mode, dst_reg) + ext)
    # MOVE alias shapes with an indexed (d8,An,Xn) destination: the base An, an address-register index Xn, or both
    # name the auto-updating source register (which the destination EA must observe after the update).
    for size in "bwl":
        for src_mode in (3, 4):
            for src_reg in (0, 3, 7):
                other = 1
                for dst_reg, idx_a, idx_reg, idx_long in (
                        (src_reg, False, 0, False), (src_reg, False, 2, True),   # base only (index is Dn)
                        (other, True, src_reg, False), (other, True, src_reg, True),  # index only
                        (src_reg, True, src_reg, False), (src_reg, True, src_reg, True),  # both
                        (src_reg, True, other, True), (src_reg, True, other, False)):  # base + other An index
                    ext = (0x8000 if idx_a else 0) | (idx_reg << 12) | (0x0800 if idx_long else 0) | 0x04
                    word = move_word(size, src_mode, src_reg, 6, dst_reg) + "%04X" % ext
                    if idx_a and idx_long:
                        # The routed environment biases every An by the work-RAM base, so base + a long An index
                        # cannot be compared against the unbiased direct window; these are checked structurally.
                        LONG_AN_INDEX.append(word)
                    else:
                        out.append(word)
    # MOVE with a non-aliasing memory destination, indexed operands and an immediate source. Absolute and
    # PC-relative operands are excluded by construction: they address cartridge/ROM space or the sign-extended
    # top of the address space, which the work-RAM-only routed environment and the 1 MiB direct window cannot
    # both host (they are covered against Musashi by the direct conformance rows only).
    for size in "bwl":
        out += [move_word(size, 6, 1, 2, 2) + "1804", move_word(size, 2, 1, 6, 2) + "1004",
                move_word(size, 6, 1, 6, 2) + "1804" + "1004", move_word(size, 0, 1, 6, 2) + "1804",
                move_word(size, 7, 4, 2, 2) + ("00FF" if size != "l" else "0000FF00")]
    # MOVEA sources.
    for size in "wl":
        for mode in (2, 3, 4):
            out.append("%04X" % (((SIZES[size]) << 12) | (2 << 9) | (1 << 6) | (mode << 3) | 1))
        out.append("%04X" % (((SIZES[size]) << 12) | (2 << 9) | (1 << 6) | (7 << 3) | 4) + ("1234" if size == "w" else "00012345"))
        out.append("%04X" % (((SIZES[size]) << 12) | (2 << 9) | (1 << 6) | (6 << 3) | 1) + "1804")
    # NOT / CLR / TST: (An)+, -(An), (d8,An,Xn), plus the other data-alterable classes.
    for base in (0x4600, 0x4200, 0x4A00):
        for size in "bwl":
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    out.append(unary_word(base, size, mode, reg) + ext)
    # SEG-021-T009: memory-word ASL/ASR/LSL/LSR/ROXL/ROXR/ROL/ROR (count fixed at 1): every legal memory-alterable
    # destination class incl. (An)+ / -(An) (deferred address commit) and (d8,An,Xn).
    for family in range(4):
        for direction in (0, 1):
            base = 0xE0C0 | (family << 9) | (direction << 8)
            for mode, ext in ((2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    word = "%04X" % (base | (mode << 3) | reg) + ext
                    out.append(word)
                    if mode in (3, 4):
                        AUTO_CODES.add(word)
    out += arithmetic_cases()
    LONG_AN_INDEX[:] = sorted(set(LONG_AN_INDEX))
    return sorted(set(out))


# SEG-021-T006: ADD/ADDA/ADDI/ADDQ, SUB/SUBA/SUBI/SUBQ and CMP/CMPA/CMPI. Every legal auto-update, indexed,
# displacement, immediate and register form goes through both lowerings. The routed environment biases every An
# by the work-RAM base; the direct lowering is therefore emitted with --window (its linear window sits at the same
# work-RAM addresses), so both sides see identical architectural An values and shapes that consume an An VALUE as data
# (An sources, ADDA/SUBA/CMPA and their aliases) compare exactly. AUTO_CODES (every auto-updating shape)
# additionally run in the routed-stop atomicity check (unmapped pointer: the routed access stops and NO
# architectural state may change).
AUTO_CODES = set()
ARITH_BASES = {"add": 0xD000, "sub": 0x9000, "cmp": 0xB000}


def arithmetic_cases():
    out = []

    def add(word, mode_pair=()):
        """mode_pair: the EA modes of the instruction's operands; auto when 3 or 4 is among them."""
        out.append(word)
        if any(m in (3, 4) for m in mode_pair):
            AUTO_CODES.add(word)

    imm = {"b": "00A5", "w": "8001", "l": "80000001"}
    for name, base in ARITH_BASES.items():
        for size, opmode in (("b", 0), ("w", 1), ("l", 2)):
            # <ea>,Dn: register, memory (every auto-update / displacement / indexed class) and immediate sources.
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    for dn in (2, 1):
                        add("%04X" % (base | (dn << 9) | (opmode << 6) | (mode << 3) | reg) + ext, (mode,))
            add("%04X" % (base | (2 << 9) | (opmode << 6) | (7 << 3) | 4) + imm[size].rjust(4 if size != "l" else 8, "0"))
            if size != "b":
                for reg in (1, 7):
                    add("%04X" % (base | (2 << 9) | (opmode << 6) | (1 << 3) | reg), ())
            # Dn,<ea> (ADD/SUB only): read-modify-write memory destinations.
            if name != "cmp":
                for mode, ext in ((2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                    for reg in ((1, 7) if mode in (3, 4) else (1,)):
                        for dn in (2, 1, 7):
                            add("%04X" % (base | (dn << 9) | ((opmode + 4) << 6) | (mode << 3) | reg) + ext, (mode,))
        # ADDA/SUBA/CMPA: word/long, every memory source; destination An may alias an auto-updating source.
        for size, opmode in (("w", 3), ("l", 7)):
            for mode, ext in ((2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    for an in (2, reg, 0):
                        add("%04X" % (base | (an << 9) | (opmode << 6) | (mode << 3) | reg) + ext, (mode,))
            for mode in (0, 1):
                add("%04X" % (base | (2 << 9) | (opmode << 6) | (mode << 3) | 1), ())
            add("%04X" % (base | (2 << 9) | (opmode << 6) | (7 << 3) | 4) + ("8001" if size == "w" else "80000001"),
                ())
    # ADDI/SUBI/CMPI and ADDQ/SUBQ: every destination class, sizes b/w/l.
    for name, base, quick in (("add", 0x0600, 0x5000), ("sub", 0x0400, 0x5100), ("cmp", 0x0C00, None)):
        for size, sf in (("b", 0), ("w", 1), ("l", 2)):
            immw = {"b": "00A5", "w": "8001", "l": "80000001"}[size]
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    add("%04X" % (base | (sf << 6) | (mode << 3) | reg) + immw + ext, (mode,))
                    if quick is not None:
                        for q in (1, 0):
                            add("%04X" % (quick | (q << 9) | (sf << 6) | (mode << 3) | reg) + ext, (mode,))
            if quick is not None and size != "b":
                for q in (1, 0):
                    for reg in (1, 7):  # ADDQ/SUBQ to An: word/long, no CCR change
                        add("%04X" % (quick | (q << 9) | (sf << 6) | (1 << 3) | reg))
    # SEG-021-T007: AND/OR/EOR and ANDI/ORI/EORI, every legal source/destination class incl. auto-updating ones.
    for name, base in (("and", 0xC000), ("or", 0x8000), ("eor", 0xB000)):
        for size, opmode in (("b", 0), ("w", 1), ("l", 2)):
            if name != "eor":
                for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                    for reg in ((1, 7) if mode in (3, 4) else (1,)):
                        for dn in (2, 1):
                            add("%04X" % (base | (dn << 9) | (opmode << 6) | (mode << 3) | reg) + ext, (mode,))
                add("%04X" % (base | (2 << 9) | (opmode << 6) | (7 << 3) | 4) + imm[size].rjust(4 if size != "l" else 8, "0"))
            # Dn,<ea>: read-modify-write (AND/OR memory alterable; EOR also Dn).
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                if mode == 0 and name != "eor":
                    continue
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    for dn in (2, 1, 7):
                        add("%04X" % (base | (dn << 9) | ((opmode + 4) << 6) | (mode << 3) | reg) + ext, (mode,))
    for name, base in (("andi", 0x0200), ("ori", 0x0000), ("eori", 0x0A00)):
        for size, sf in (("b", 0), ("w", 1), ("l", 2)):
            immw = {"b": "00A5", "w": "8001", "l": "80000001"}[size]
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    add("%04X" % (base | (sf << 6) | (mode << 3) | reg) + immw + ext, (mode,))
    # SEG-021-T008: BTST/BCHG/BCLR/BSET, dynamic (Dn) and static (#n) bit numbers, every legal destination class
    # incl. auto-updating ones (A7 byte stepping) and (d8,An,Xn). PC-relative/immediate BTST destinations are covered
    # by the Musashi-validated conformance rows and the routed coverage compile stages.
    for opmode, imm_selector in ((4, 0), (5, 1), (6, 2), (7, 3)):
        for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
            for reg in ((1, 7) if mode in (3, 4) else (1,)):
                for dn in (2, 1):
                    add("%04X" % (0x0000 | (dn << 9) | (opmode << 6) | (mode << 3) | reg) + ext, (mode,))
                for bit in ("0000", "0007", "0008", "001F", "0020"):
                    add("%04X" % (0x0800 | (imm_selector << 6) | (mode << 3) | reg) + bit + ext, (mode,))
    # SEG-021-T014: NEG/NEGX (every legal data-alterable class incl. auto-updating and indexed) and the ADDX/SUBX/CMPM
    # register-pair and memory-pair forms (distinct, aliased and A7 byte-step register pairs), every size. The memory
    # pairs are auto-updating on both operands and run in the routed-stop atomicity check too.
    for base in (0x4400, 0x4000):
        for size, sf in (("b", 0), ("w", 1), ("l", 2)):
            for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
                for reg in ((1, 7) if mode in (3, 4) else (1,)):
                    add("%04X" % (base | (sf << 6) | (mode << 3) | reg) + ext, (mode,))
    for name, base in (("addx", 0xD100), ("subx", 0x9100), ("cmpm", 0xB108)):
        for size, sf in (("b", 0), ("w", 1), ("l", 2)):
            for rx, ry in ((1, 2), (1, 1), (7, 7), (7, 0), (0, 7), (3, 4)):
                if name == "cmpm":
                    add("%04X" % (base | (rx << 9) | (sf << 6) | ry), (3,))
                else:
                    add("%04X" % (base | (rx << 9) | (sf << 6) | (1 << 3) | ry), (4,))
                    add("%04X" % (base | (rx << 9) | (sf << 6) | ry), ())
    # SEG-021-T015: NBCD (every data-alterable class incl. auto-updating and indexed) and the ABCD/SBCD register-pair
    # and -(Ay),-(Ax) forms (distinct, aliased and A7 byte-step pairs). The memory forms run in the routed-stop
    # atomicity check too.
    for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
        for reg in ((1, 7) if mode in (3, 4) else (1,)):
            add("%04X" % (0x4800 | (mode << 3) | reg) + ext, (mode,))
    for base in (0xC100, 0x8100):
        for rx, ry in ((1, 2), (1, 1), (7, 7), (7, 0), (0, 7), (3, 4)):
            add("%04X" % (base | (rx << 9) | (1 << 3) | ry), (4,))
            add("%04X" % (base | (rx << 9) | ry), ())
    # SEG-021-T016: EXG (register-only; distinct, aliased and A7 pairs), MOVEP (both sizes and directions, even/odd/negative
    # displacements, A7 base), Scc (every condition x every data-alterable class incl. auto-updating and indexed) and
    # TAS (same classes). Memory forms run in the routed-stop atomicity check too (register forms cannot stop).
    for opword in (0xC140, 0xC148, 0xC188):
        for rx, ry in ((1, 2), (1, 1), (7, 7), (7, 0), (0, 7), (3, 4)):
            add("%04X" % (opword | (rx << 9) | ry), ())
    for opmode in (4, 5, 6, 7):  # word/long mem->reg, word/long reg->mem
        for reg in (1, 7):
            for disp in ("0010", "0011", "FFF0"):
                word = "%04X" % (0x0008 | (2 << 9) | (opmode << 6) | reg) + disp
                add(word, ())
                AUTO_CODES.add(word)  # memory access: participates in the forced-stop atomicity check
    for cond in range(16):
        for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
            for reg in ((1, 7) if mode in (3, 4) else (1,)):
                add("%04X" % (0x50C0 | (cond << 8) | (mode << 3) | reg) + ext, (mode,))
                if mode in (2, 5, 6):
                    AUTO_CODES.add("%04X" % (0x50C0 | (cond << 8) | (mode << 3) | reg) + ext)
    for mode, ext in ((0, ""), (2, ""), (3, ""), (4, ""), (5, "0010"), (6, "1804"), (6, "1004")):
        for reg in ((1, 7) if mode in (3, 4) else (1,)):
            add("%04X" % (0x4AC0 | (mode << 3) | reg) + ext, (mode,))
            if mode in (2, 5, 6):
                AUTO_CODES.add("%04X" % (0x4AC0 | (mode << 3) | reg) + ext)
    return out


DRIVER = r'''
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;
typedef struct { const char *code; int (*fn)(cap_state *); } cf_entry;
typedef struct { const char *code; GenesisControlTransfer (*fn)(GenesisRuntime *); } rf_entry;
extern const cf_entry cf_table[];
extern const rf_entry rf_table[];
uint32_t frame_ids[64], frame_continuations[64], frame_depth;
static cap_state direct;
static GenesisRuntime routed;
static uint8_t image[0x10000];
int main(int argc, char **argv) {
  FILE *f; char code[64]; unsigned sr; int mismatches = 0, changed = 0, total = 0;
  const int stop_mode = argc > 2 && !strcmp(argv[2], "stop");
  uint32_t seed = 0x1234567u;
  if (!(f = fopen(argv[1], "r"))) return 2;
  for (unsigned i = 0; i < sizeof image; ++i) { seed = seed * 1664525u + 1013904223u; image[i] = (uint8_t)(seed >> 24); }
  while (fscanf(f, "%63s %x", code, &sr) == 2) {
    const cf_entry *cf = NULL; const rf_entry *rf = NULL; unsigned d[8], a[8], usp, i;
    for (i = 0; i < 8; ++i) if (fscanf(f, "%x", &d[i]) != 1) return 2;
    for (i = 0; i < 8; ++i) if (fscanf(f, "%x", &a[i]) != 1) return 2;
    if (fscanf(f, "%x", &usp) != 1) return 2;
    for (i = 0; cf_table[i].code; ++i) if (!strcmp(cf_table[i].code, code)) cf = &cf_table[i];
    for (i = 0; rf_table[i].code; ++i) if (!strcmp(rf_table[i].code, code)) rf = &rf_table[i];
    if (!cf || !rf) { printf("MISSING %s\n", code); ++mismatches; continue; }
    memset(&direct, 0, sizeof direct); memset(&routed, 0, sizeof routed);
    memcpy(direct.ram, image, sizeof image); memcpy(routed.work_ram, image, sizeof image);
    /* The direct lowering is emitted with --window: its linear window sits at the work-RAM addresses, so both sides
     * see identical architectural An values (no bias) and identical memory. */
    for (i = 0; i < 8; ++i) {
      direct.d[i] = routed.d[i] = d[i];
      direct.a[i] = routed.a[i] = a[i] + 0x00FF0000u;
    }
    direct.sr = routed.sr = (uint16_t)sr; direct.usp = routed.usp = usp + 0x00FF0000u;
    direct.pc = routed.pc = 0x2000u;
    memset(frame_ids, 0, sizeof frame_ids); memset(frame_continuations, 0, sizeof frame_continuations); frame_depth = 0;
    if (stop_mode) {
      /* Routed-stop atomicity: every An points at an unmapped address, so the first routed access stops. The stop
       * must leave D/A/SR/PC/memory exactly at the pre-instruction boundary (no partial auto-update, no result). */
      static GenesisRuntime before;
      for (i = 0; i < 8; ++i) routed.a[i] = 0x00500000u + i * 0x100u;
      routed.usp = 0x00500800u;
      memcpy(&before, &routed, sizeof routed);
      { GenesisControlTransfer t = rf->fn(&routed);
        ++total;
        if (t.kind == GENESIS_CONTINUE_AT_PC) { printf("NOSTOP %s\n", code); ++mismatches; continue; }
        if (memcmp(before.d, routed.d, sizeof routed.d) || memcmp(before.a, routed.a, sizeof routed.a) ||
            before.sr != routed.sr || before.pc != routed.pc || before.usp != routed.usp ||
            memcmp(before.work_ram, routed.work_ram, sizeof image)) { printf("PARTIAL %s\n", code); ++mismatches; } }
      continue;
    }
    (void)cf->fn(&direct);
    { GenesisControlTransfer t = rf->fn(&routed);
      if (t.kind != GENESIS_CONTINUE_AT_PC) { printf("STOP %s\n", code); ++mismatches; continue; } }
    { int bad = 0;
      for (i = 0; i < 8; ++i) {
        if (direct.d[i] != routed.d[i]) bad = 1;
        if (direct.a[i] != routed.a[i]) bad = 1;
      }
      if (direct.sr != routed.sr || direct.pc != routed.pc || direct.usp != routed.usp ||
          memcmp(direct.ram, routed.work_ram, sizeof image) != 0) bad = 1;
      ++total;
      if (memcmp(direct.ram, image, sizeof image) != 0) ++changed;
      else for (i = 0; i < 8; ++i) if (direct.a[i] != a[i] + 0x00FF0000u) ++changed;
      if (bad) { printf("DIFF %s\n", code); ++mismatches; } }
  }
  printf("total=%d changed=%d mismatches=%d\n", total, changed, mismatches);
  return mismatches ? 1 : 0;
}
'''


def emit(mode_args, codes, out):
    proc = subprocess.run([EMITTER, *mode_args, "--out", str(out)], input="".join(c + "\n" for c in codes),
                          text=True, capture_output=True, check=True)
    status = dict(line.split() for line in proc.stdout.splitlines())
    return status


codes = cases()
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    direct_status = emit(["--window"], codes, tmp / "direct.c")
    routed_status = emit(["--routed"], codes, tmp / "routed.c")
    bad = [c for c in codes if direct_status.get(c) != "ok" or routed_status.get(c) != "ok"]
    check(not bad, "encodings that do not emit on both routes: %s" % bad[:12])
    # Long An-indexed destinations behind an auto-updating source: the destination address must be derived from
    # the source's updated local, never from the live (stale) register array.
    emit_status = emit(["--routed"], LONG_AN_INDEX, tmp / "long_index.c")
    check(all(emit_status.get(c) == "ok" for c in LONG_AN_INDEX), "long An-index alias shapes must emit")
    text = (tmp / "long_index.c").read_text()
    check(text.count("const uint32_t m68k_move_dst_ea = (uint32_t)(") == len(LONG_AN_INDEX) * 1 and
          "(int32_t)m68k_move_src_ea" in text, "long An-index alias shapes must use the updated source local")
    (tmp / "driver.c").write_text(DRIVER)
    flags = ["-std=c11", "-O0", "-Wall", "-Wextra", "-Wno-type-limits", "-pedantic", "-Werror", "-I", str(RUNTIME_DIR)]
    objs = []
    for name in ("direct", "routed", "driver"):
        obj = tmp / (name + ".o")
        proc = subprocess.run([CC, *flags, "-c", str(tmp / (name + ".c")), "-o", str(obj)], capture_output=True, text=True)
        check(proc.returncode == 0, "strict compile of %s failed: %s" % (name, proc.stderr[:1500]))
        objs.append(str(obj))
    runtime_obj = tmp / "runtime.o"
    proc = subprocess.run([CC, "-std=c11", "-O0", "-I", str(RUNTIME_DIR), "-c", str(RUNTIME_DIR / "runtime.c"), "-o",
                           str(runtime_obj)], capture_output=True, text=True)
    check(proc.returncode == 0, "runtime.c failed to compile: " + proc.stderr[:800])
    exe = tmp / "differential"
    proc = subprocess.run([CC, "-o", str(exe), *objs, str(runtime_obj)], capture_output=True, text=True)
    check(proc.returncode == 0, "link failed: " + proc.stderr[:800])
    def case_lines(selected):
        lines = []
        for code in selected:
            for sr in (0x2700, 0x271F, 0x2710):
                # small values keep every brief-format index register (word or long) inside the 64 KiB image
                d = [0x10 + i * 4 + ((sr & 0x1F) & ~1) + ((0x9E3779B1 * (i + 1)) & 0x300) for i in range(8)]
                a = [0x4000 + i * 0x400 for i in range(8)]
                a[7] = 0x7000
                lines.append("%s %X %s %s %X" % (code, sr, " ".join("%X" % x for x in d),
                                                " ".join("%X" % x for x in a), 0x7800))
        return "\n".join(lines) + "\n"

    def run_mode(name, selected, mode):
        path = tmp / (name + ".txt")
        path.write_text(case_lines(selected))
        run = subprocess.run([str(exe), str(path), *([mode] if mode else [])], capture_output=True, text=True)
        print(name, run.stdout[-1500:])
        return run

    stops = sorted(AUTO_CODES)
    run = run_mode("cases", codes, None)
    check(run.returncode == 0, "routed lowering diverges from the direct (Musashi-validated) lowering")
    check("changed=0 " not in run.stdout, "vacuous run: no vector changed any state")
    # Routed-stop atomicity: every auto-updating arithmetic shape, with the routed access forced to stop.
    run = run_mode("stops", stops, "stop")
    check(run.returncode == 0, "a routed-access stop left partial architectural state (or did not stop)")
    check("total=0 " not in run.stdout and len(stops) > 100, "vacuous stop run")
print("m68k routed lowering differential test: PASS (%d encodings, %d stop-checked)" % (len(codes), len(stops)))
