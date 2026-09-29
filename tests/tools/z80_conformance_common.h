/* SEG-008-T003: shared vector reader and result writer of the Z80 differential harness. The generated-native runner and
 * the pinned-oracle adapter both include this header, so they read the same vector text and print the same result
 * schema; tools/z80_conformance.py compares the two outputs. See docs/testing/z80-conformance-harness.md.
 *
 * Vector text (hex unless noted; `#` lines ignored):
 *   V <name>
 *   R a f b c d e h l a2 f2 b2 c2 d2 e2 h2 l2
 *   P ix iy sp pc wz
 *   X i r im iff1 iff2 q halted deferral ldair prefix_run nmireject
 *   M <addr> <hexbytes>            memory patch (code and data alike)
 *   F <addr> <count> <byte>        memory fill
 *   IN <hexbytes>                  I/O input script (successive IN values, then 0xFF)
 *   ACK <hexbytes>                 interrupt-acknowledge bytes (then 0xFF)
 *   K <set> <lo> <hi> <identity-dec> <base>   code-image map entry [lo, hi) of map set <set> (generated side only)
 *   S <i|r> <budget> <int> <nmi> [map=<set>]   one step: `i` = one instruction / interrupt response / halted cycle
 *                                              (prefix runs completed); `r` = raw run of <budget> T-states
 *   E                                          end of vector
 */
#ifndef Z80_CONFORMANCE_COMMON_H
#define Z80_CONFORMANCE_COMMON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZC_MAX_STEPS 64
#define ZC_MAX_MAPS 8
#define ZC_MAX_RANGES 8
#define ZC_MAX_SCRIPT 4096
#define ZC_MAX_EVENTS 256

typedef struct {
  unsigned a, f, b, c, d, e, h, l, a2, f2, b2, c2, d2, e2, h2, l2;
  unsigned ix, iy, sp, pc, wz;
  unsigned i, r, im, iff1, iff2, q, halted, deferral, ldair, prefix_run, nmireject;
} zc_state;

typedef struct { char mode; unsigned long budget; int int_line, nmi, map; } zc_step;
typedef struct { unsigned lo, hi, identity, base; } zc_range;
typedef struct { int count; zc_range ranges[ZC_MAX_RANGES]; } zc_map;

typedef struct {
  char name[96];
  zc_state state;
  unsigned char mem[65536];
  unsigned char in_script[ZC_MAX_SCRIPT], ack_script[ZC_MAX_SCRIPT];
  int in_count, ack_count;
  zc_map maps[ZC_MAX_MAPS];
  int step_count;
  zc_step steps[ZC_MAX_STEPS];
} zc_vector;

/* Event log of one step. */
typedef struct {
  int write_count, io_count;
  unsigned write_addr[ZC_MAX_EVENTS], write_value[ZC_MAX_EVENTS];
  char io_kind[ZC_MAX_EVENTS];
  unsigned io_port[ZC_MAX_EVENTS], io_value[ZC_MAX_EVENTS];
} zc_events;

static int zc_hex_value(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
static int zc_parse_bytes(const char *text, unsigned char *out, int capacity) {
  int count = 0;
  while (zc_hex_value(text[0]) >= 0 && zc_hex_value(text[1]) >= 0) {
    if (count >= capacity) return -1;
    out[count++] = (unsigned char)(zc_hex_value(text[0]) * 16 + zc_hex_value(text[1]));
    text += 2;
  }
  return count;
}

/* Reads the next vector; returns 1, 0 at end of file, -1 on a malformed line. */
static int zc_read_vector(FILE *file, zc_vector *v) {
  char *line = NULL;
  size_t capacity = 0;
  int started = 0;
  while (getline(&line, &capacity, file) >= 0) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char verb[8] = {0};
    int consumed = 0;
    if (sscanf(line, "%7s%n", verb, &consumed) < 1) continue;
    const char *rest = line + consumed;
    if (!strcmp(verb, "V")) {
      memset(v, 0, sizeof *v);
      if (sscanf(rest, "%95s", v->name) != 1) { free(line); return -1; }
      started = 1;
    } else if (!started) {
      free(line);
      return -1;
    } else if (!strcmp(verb, "R")) {
      zc_state *s = &v->state;
      if (sscanf(rest, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", &s->a, &s->f, &s->b, &s->c, &s->d, &s->e,
                 &s->h, &s->l, &s->a2, &s->f2, &s->b2, &s->c2, &s->d2, &s->e2, &s->h2, &s->l2) != 16) { free(line); return -1; }
    } else if (!strcmp(verb, "P")) {
      zc_state *s = &v->state;
      if (sscanf(rest, "%x %x %x %x %x", &s->ix, &s->iy, &s->sp, &s->pc, &s->wz) != 5) { free(line); return -1; }
    } else if (!strcmp(verb, "X")) {
      zc_state *s = &v->state;
      if (sscanf(rest, "%x %x %x %x %x %x %x %x %x %x %x", &s->i, &s->r, &s->im, &s->iff1, &s->iff2, &s->q, &s->halted,
                 &s->deferral, &s->ldair, &s->prefix_run, &s->nmireject) != 11) { free(line); return -1; }
    } else if (!strcmp(verb, "M")) {
      unsigned addr;
      int used = 0;
      if (sscanf(rest, "%x%n", &addr, &used) != 1) { free(line); return -1; }
      const char *hex = rest + used;
      while (*hex == ' ') ++hex;
      unsigned char *bytes = malloc(strlen(hex) / 2 + 1);
      const int count = zc_parse_bytes(hex, bytes, (int)(strlen(hex) / 2 + 1));
      for (int i = 0; i < count; ++i) v->mem[(addr + (unsigned)i) & 0xFFFFu] = bytes[i];
      free(bytes);
    } else if (!strcmp(verb, "F")) {
      unsigned addr, count, byte;
      if (sscanf(rest, "%x %x %x", &addr, &count, &byte) != 3) { free(line); return -1; }
      for (unsigned i = 0; i < count; ++i) v->mem[(addr + i) & 0xFFFFu] = (unsigned char)byte;
    } else if (!strcmp(verb, "IN")) {
      char hex[2 * ZC_MAX_SCRIPT + 2] = {0};
      if (sscanf(rest, "%8191s", hex) == 1) v->in_count = zc_parse_bytes(hex, v->in_script, ZC_MAX_SCRIPT);
    } else if (!strcmp(verb, "ACK")) {
      char hex[2 * ZC_MAX_SCRIPT + 2] = {0};
      if (sscanf(rest, "%8191s", hex) == 1) v->ack_count = zc_parse_bytes(hex, v->ack_script, ZC_MAX_SCRIPT);
    } else if (!strcmp(verb, "K")) {
      unsigned set, lo, hi, identity, base;
      if (sscanf(rest, "%u %x %x %u %x", &set, &lo, &hi, &identity, &base) != 5 || set >= ZC_MAX_MAPS ||
          v->maps[set].count >= ZC_MAX_RANGES) { free(line); return -1; }
      v->maps[set].ranges[v->maps[set].count++] = (zc_range){lo, hi, identity, base};
    } else if (!strcmp(verb, "S")) {
      zc_step *st = &v->steps[v->step_count];
      char mode;
      unsigned long budget;
      int int_line, nmi, map = 0;
      char extra[32] = {0};
      const int n = sscanf(rest, " %c %lu %d %d %31s", &mode, &budget, &int_line, &nmi, extra);
      if (n < 4 || v->step_count >= ZC_MAX_STEPS) { free(line); return -1; }
      if (n == 5 && !strncmp(extra, "map=", 4)) map = atoi(extra + 4);
      *st = (zc_step){mode, budget, int_line, nmi, map};
      v->step_count++;
    } else if (!strcmp(verb, "E")) {
      free(line);
      return 1;
    }
  }
  free(line);
  return 0;
}

static void zc_print_step(FILE *out, const char *name, int step, unsigned long t, const char *outcome, const zc_state *s,
                          const zc_events *ev) {
  fprintf(out,
          "S %s %d t=%lu out=%s a=%02X f=%02X b=%02X c=%02X d=%02X e=%02X h=%02X l=%02X a2=%02X f2=%02X b2=%02X "
          "c2=%02X d2=%02X e2=%02X h2=%02X l2=%02X ix=%04X iy=%04X sp=%04X pc=%04X wz=%04X i=%02X r=%02X im=%u iff1=%u "
          "iff2=%u q=%02X halted=%u deferral=%u ldair=%u prefix_run=%u nmireject=%u w=",
          name, step, t, outcome, s->a, s->f, s->b, s->c, s->d, s->e, s->h, s->l, s->a2, s->f2, s->b2, s->c2, s->d2,
          s->e2, s->h2, s->l2, s->ix, s->iy, s->sp, s->pc, s->wz, s->i, s->r, s->im, s->iff1, s->iff2, s->q, s->halted,
          s->deferral, s->ldair, s->prefix_run, s->nmireject);
  if (ev->write_count == 0) fputc('-', out);
  for (int i = 0; i < ev->write_count; ++i) fprintf(out, "%s%04X:%02X", i ? "," : "", ev->write_addr[i], ev->write_value[i]);
  fputs(" io=", out);
  if (ev->io_count == 0) fputc('-', out);
  for (int i = 0; i < ev->io_count; ++i)
    fprintf(out, "%s%c:%04X:%02X", i ? "," : "", ev->io_kind[i], ev->io_port[i], ev->io_value[i]);
  fputc('\n', out);
}

#endif
