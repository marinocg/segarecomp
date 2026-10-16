/* SEG-062-T003/T004: independent two-processor reference for the Sega CD fixtures.
 *
 * Both processors run on the pinned, unmodified Musashi core (two saved contexts, switched with m68k_set_context), over a
 * separately written reference bus model of the contract subset the fixtures use: work RAM, PRG RAM with the main-side bank window
 * and the sub write protect, the communication flags/words, IFL2 -> level 2 (cleared by the acknowledge), SRES/SBRQ with the sub
 * reset taken by Musashi from PRG RAM $0/$4, and Word RAM in 2M (DMNA/RET ownership) and 1M (word-interleaved banks, RET swap)
 * modes. Interleaving is a plain round robin of one instruction per runnable processor - deliberately different from the
 * runtime's earliest-first clock scheduler - so agreement on the final state shows the fixtures' results do not depend on it.
 * No timer, CDC or VDP is modelled; a fixture that touches them makes the reference exit with status 3.
 *
 * usage: sega_cd_musashi_reference <system-area.bin> <ip-offset> <ip-size> <ip-entry> <sp-offset> <sp-size> <sp-entry> <out-dir>
 * Writes <out-dir>/state.json and the raw memories prg_ram.bin, work_ram.bin, word_ram.bin (2M layout). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "m68kcpu.h"

static unsigned char area[0x8000], prg[0x80000], work[0x10000], word2m[0x40000];
static unsigned current; /* 0 main, 1 sub */
static int sub_reset = 1, sub_busreq = 1, ifl2, ien, pending, wp, bank, mode1m, ret = 1, dmna;
static unsigned main_flag, sub_flag, cmd[8], stat_[8];
static int failed;
static int reset_pending;
static int in_setup = 1; /* Musashi's own power-on reset reads the (absent) main vectors before the profile state is set */
static unsigned char *sub_context, *main_context; /* m68k_context_size() bytes each */

static void fail(const char *what, unsigned address) {
  if (in_setup) return;
  if (!failed) fprintf(stderr, "reference: %s at %06X (cpu %u)\n", what, address, current);
  failed = 1;
}

/* 1M bank b, byte offset o (0..0x1FFFF) -> 2M byte offset: bank b word i is 2M word 2i+b. */
static unsigned bank_byte(unsigned b, unsigned o) { return ((o >> 1) * 4U + b * 2U) + (o & 1U); }
static unsigned main_bank(void) { return ret ? 1U : 0U; }

static void byte_ref(unsigned address, int write, unsigned char **cell) {
  address &= 0xFFFFFF;
  *cell = NULL;
  if (current == 0U) {
    if (address >= 0xFF0000) *cell = &work[address - 0xFF0000];
    else if (address >= 0x020000 && address < 0x040000) {
      if (!(sub_reset || sub_busreq)) fail("prg window without bus", address);
      else *cell = &prg[bank * 0x20000 + address - 0x020000];
    } else if (address >= 0x200000 && address < 0x240000) {
      if (!mode1m) {
        if (!ret) fail("main word ram not owned", address);
        else *cell = &word2m[address - 0x200000];
      } else if (address < 0x220000) *cell = &word2m[bank_byte(main_bank(), address - 0x200000)];
      else fail("cell image", address);
    }
  } else {
    if (address < 0x80000) {
      if (write && address < (unsigned)wp * 0x200U) return; /* dropped */
      *cell = &prg[address];
    } else if (address >= 0x080000 && address < 0x0C0000) {
      if (!mode1m && !ret) *cell = &word2m[address - 0x080000];
      else fail("sub word ram not owned / dot image", address);
    } else if (address >= 0x0C0000 && address < 0x0E0000) {
      if (mode1m) *cell = &word2m[bank_byte(main_bank() ^ 1U, address - 0x0C0000)];
      else fail("sub 1M bank in 2M mode", address);
    }
  }
}

static unsigned reg_read(unsigned address) {
  if (current == 0U && address >= 0xA12000 && address < 0xA12030) {
    const unsigned o = address - 0xA12000;
    if (o >= 0x10) return o >= 0x20 ? (o & 1 ? stat_[(o - 0x20) / 2] & 0xFF : stat_[(o - 0x20) / 2] >> 8)
                                     : (o & 1 ? cmd[(o - 0x10) / 2] & 0xFF : cmd[(o - 0x10) / 2] >> 8);
    switch (o) {
    case 0: return ((ien & 4) ? 0x80U : 0U) | (unsigned)ifl2;
    case 1: return (unsigned)(sub_busreq << 1) | (sub_reset ? 0U : 1U);
    case 2: return (unsigned)wp;
    case 3: return (unsigned)((bank << 6) | (mode1m << 2) | (dmna << 1) | ret);
    case 0x0E: return main_flag;
    case 0x0F: return sub_flag;
    }
  }
  if (current == 1U && address >= 0xFF8000 && address < 0xFF8034) {
    const unsigned o = address - 0xFF8000;
    if (o >= 0x10 && o < 0x30) return o >= 0x20 ? (o & 1 ? stat_[(o - 0x20) / 2] & 0xFF : stat_[(o - 0x20) / 2] >> 8)
                                                 : (o & 1 ? cmd[(o - 0x10) / 2] & 0xFF : cmd[(o - 0x10) / 2] >> 8);
    switch (o) {
    case 3: return (unsigned)((mode1m << 2) | (dmna << 1) | ret);
    case 0x0E: return main_flag;
    case 0x0F: return sub_flag;
    case 0x33: return (unsigned)ien;
    }
  }
  fail("register read", address);
  return 0;
}

static void reg_write(unsigned address, unsigned value) {
  if (current == 0U && address >= 0xA12000 && address < 0xA12020) {
    const unsigned o = address - 0xA12000;
    if (o >= 0x10) {
      unsigned *w = &cmd[(o - 0x10) / 2];
      *w = o & 1 ? (*w & 0xFF00) | value : (*w & 0xFF) | (value << 8);
      return;
    }
    switch (o) {
    case 0:
      if ((value & 1) && (ien & 4)) {
        ifl2 = 1;
        pending |= 4;
      }
      return;
    case 1:
      if (value & 1) {
        const int was = sub_reset;
        sub_reset = 0;
        sub_busreq = (value >> 1) & 1;
        if (was) reset_pending = 1; /* taken by the loop after this instruction (no mid-instruction context switch) */
      } else {
        sub_reset = 1;
        sub_busreq = 1;
      }
      return;
    case 2: wp = (int)value; return;
    case 3:
      bank = (int)(value >> 6) & 3;
      if ((value & 2) && !mode1m) { /* 2M: Word RAM to the sub */
        ret = 0;
        dmna = 1;
      } else if ((value & 2) && mode1m) fail("1M DMNA", address);
      return;
    case 0x0E: main_flag = value; return;
    }
  }
  if (current == 1U && address >= 0xFF8000 && address < 0xFF8034) {
    const unsigned o = address - 0xFF8000;
    if (o >= 0x20 && o < 0x30) {
      unsigned *w = &stat_[(o - 0x20) / 2];
      *w = o & 1 ? (*w & 0xFF00) | value : (*w & 0xFF) | (value << 8);
      return;
    }
    switch (o) {
    case 3:
      if (value & 4) { /* 1M mode; RET selects the banks */
        mode1m = 1;
        ret = (int)(value & 1);
        dmna = 0;
      } else if (value & 1) { /* 2M: return Word RAM to main */
        mode1m = 0;
        ret = 1;
        dmna = 0;
      }
      return;
    case 0x0F: sub_flag = value; return;
    case 0x33: ien = (int)value & 0x7E; return;
    }
  }
  fail("register write", address);
}

static int is_reg(unsigned address) {
  return (current == 0U && address >= 0xA12000 && address < 0xA12040) || (current == 1U && address >= 0xFF8000 && address < 0xFF8200);
}

unsigned int m68k_read_memory_8(unsigned int address) {
  unsigned char *cell;
  address &= 0xFFFFFF;
  if (is_reg(address)) return reg_read(address);
  byte_ref(address, 0, &cell);
  if (cell == NULL) {
    fail("read unmapped", address);
    return 0;
  }
  return *cell;
}
unsigned int m68k_read_memory_16(unsigned int address) {
  address &= 0xFFFFFF;
  if (is_reg(address)) return (reg_read(address) << 8) | reg_read(address + 1);
  return (m68k_read_memory_8(address) << 8) | m68k_read_memory_8(address + 1);
}
unsigned int m68k_read_memory_32(unsigned int address) { return (m68k_read_memory_16(address) << 16) | m68k_read_memory_16(address + 2); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

void m68k_write_memory_8(unsigned int address, unsigned int value) {
  unsigned char *cell;
  address &= 0xFFFFFF;
  if (is_reg(address)) {
    reg_write(address, value & 0xFF);
    return;
  }
  byte_ref(address, 1, &cell);
  if (cell == NULL) {
    if (!(current == 1U && address < (unsigned)wp * 0x200U)) fail("write unmapped", address);
    return;
  }
  *cell = (unsigned char)value;
}
void m68k_write_memory_16(unsigned int address, unsigned int value) {
  address &= 0xFFFFFF;
  if (is_reg(address)) { /* word register writes: the semantics the fixtures use */
    const unsigned o = address & 0xFF;
    if ((current == 0U && (o == 0x0E)) || (current == 1U && o == 0x0E)) {
      reg_write(address + 1, value & 0xFF);
      return;
    }
    reg_write(address, (value >> 8) & 0xFF);
    reg_write(address + 1, value & 0xFF);
    return;
  }
  m68k_write_memory_8(address, value >> 8);
  m68k_write_memory_8(address + 1, value & 0xFF);
}
void m68k_write_memory_32(unsigned int address, unsigned int value) {
  m68k_write_memory_16(address, value >> 16);
  m68k_write_memory_16(address + 2, value & 0xFFFF);
}

static int ack(int level) {
  if (current == 1U) {
    pending &= ~(1 << level);
    if (level == 2) ifl2 = 0;
  }
  return M68K_INT_ACK_AUTOVECTOR;
}

static int sub_level(void) {
  int level, active = pending & ien;
  for (level = 7; level >= 1; --level)
    if (active & (1 << level)) return level;
  return 0;
}

static void dump(const char *dir, const char *name, const unsigned char *data, size_t n) {
  char path[4096];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (!f || fwrite(data, 1, n, f) != n || fclose(f) != 0) exit(4);
}

int main(int argc, char **argv) {
  FILE *f;
  unsigned ip_off, ip_size, ip_entry, sp_off, sp_size, sp_entry, k, i;
  long steps = 0;
  int stopped[2] = {0, 0};
  char path[4096];
  if (argc != 9) return 2;
  sub_context = calloc(1, m68k_context_size());
  main_context = calloc(1, m68k_context_size());
  if (sub_context == NULL || main_context == NULL) return 2;
  f = fopen(argv[1], "rb");
  if (!f || fread(area, 1, sizeof area, f) != sizeof area) return 2;
  fclose(f);
  ip_off = (unsigned)strtoul(argv[2], 0, 0); ip_size = (unsigned)strtoul(argv[3], 0, 0); ip_entry = (unsigned)strtoul(argv[4], 0, 0);
  sp_off = (unsigned)strtoul(argv[5], 0, 0); sp_size = (unsigned)strtoul(argv[6], 0, 0); sp_entry = (unsigned)strtoul(argv[7], 0, 0);
  memcpy(work, area + ip_off, ip_size);
  memcpy(prg + 0x6000, area + sp_off, sp_size);
  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_set_int_ack_callback(ack);
  current = 1;
  m68k_pulse_reset();
  m68k_get_context(sub_context);
  current = 0;
  m68k_pulse_reset();
  m68k_set_reg(M68K_REG_SR, 0x2700);
  m68k_set_reg(M68K_REG_A7, 0xFFFE00);
  m68k_set_reg(M68K_REG_PC, 0xFF0000 + ip_entry);
  m68k_get_context(main_context);
  in_setup = 0;
  (void)sp_entry;
  for (steps = 0; steps < 2000000 && !failed; ++steps) {
    int ran = 0;
    for (k = 0; k < 2U; ++k) {
      if (k == 1U && (sub_reset || sub_busreq)) continue;
      current = k;
      m68k_set_context(k ? sub_context : main_context);
      m68k_set_irq(k ? (unsigned)sub_level() : 0U);
      m68k_execute(1);
      stopped[k] = m68ki_cpu.stopped != 0;
      m68k_get_context(k ? sub_context : main_context);
      ran = 1;
    }
    if (!ran) break;
    if (reset_pending) {
      current = 1;
      m68k_set_context(sub_context);
      m68k_pulse_reset(); /* SSP from PRG $0, PC from PRG $4 */
      m68k_get_context(sub_context);
      reset_pending = 0;
      stopped[1] = 0;
    }
    if (stopped[0]) {
      const int held = sub_reset || sub_busreq;
      const int sub_mask = (int)((m68k_get_reg(sub_context, M68K_REG_SR) >> 8) & 7U);
      if (held || (stopped[1] && sub_level() <= sub_mask)) break;
    }
  }
  if (failed) return 3;
  snprintf(path, sizeof path, "%s/state.json", argv[8]);
  f = fopen(path, "w");
  if (!f) return 4;
  fprintf(f, "{\"steps\":%ld,\"cpus\":[", steps);
  for (k = 0; k < 2U; ++k) {
    void *ctx = k ? sub_context : main_context;
    fprintf(f, "%s{\"pc\":%u,\"sr\":%u,\"stopped\":%d,\"d\":[", k ? "," : "", m68k_get_reg(ctx, M68K_REG_PC), m68k_get_reg(ctx, M68K_REG_SR),
            stopped[k]);
    for (i = 0; i < 8; ++i) fprintf(f, "%s%u", i ? "," : "", m68k_get_reg(ctx, (m68k_register_t)(M68K_REG_D0 + i)));
    fprintf(f, "],\"a\":[");
    for (i = 0; i < 8; ++i) fprintf(f, "%s%u", i ? "," : "", m68k_get_reg(ctx, (m68k_register_t)(M68K_REG_A0 + i)));
    fprintf(f, "]}");
  }
  fprintf(f, "],\"comstat\":[");
  for (i = 0; i < 8; ++i) fprintf(f, "%s%u", i ? "," : "", stat_[i]);
  fprintf(f, "],\"main_flag\":%u,\"sub_flag\":%u,\"ret\":%d,\"mode_1m\":%d}\n", main_flag, sub_flag, ret, mode1m);
  fclose(f);
  dump(argv[8], "prg_ram.bin", prg, sizeof prg);
  dump(argv[8], "work_ram.bin", work, sizeof work);
  dump(argv[8], "word_ram.bin", word2m, sizeof word2m);
  return 0;
}
