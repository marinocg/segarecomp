/* SEG-070-T002 (ADR 0149): the timed standard-pad machine input of the Sega CD machine.
 *
 * A project-authored text script supplies button transitions at absolute machine ticks since reset. The machine applies each
 * transition to the Genesis device host's pad mask (the existing Genesis controller pin model then serves it to the processors: TH
 * multiplexing, CTRL direction, data latch, 0 = pressed) as a device event: at its tick, before any processor instruction or
 * interrupt acceptance at or after that tick, in script order, independent of how the run is partitioned into deadlines.
 *
 * Script grammar (ASCII; LF or CRLF; `#` starts a comment line; blank lines are ignored):
 *   segarecomp-input-script 1
 *   <tick> <port> <press|release> <BUTTON>[,<BUTTON>...]
 * tick: decimal, 0 .. 2^63-1, non-decreasing in file order. port: 1 or 2 (a standard 3-button pad). BUTTON: UP DOWN LEFT RIGHT A B C
 * START. A transition must change the button (no press of a held, release of a released button) and one tick may touch a button of
 * a port once. MODE X Y Z are refused as unsupported (6-button protocol not modelled). Anything else malformed is refused before any
 * guest instruction. */
#include "scd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

typedef struct ScdInputButton {
  const char *name;
  uint8_t mask;
} ScdInputButton;

static const ScdInputButton scd_input_buttons[] = {
    {"UP", GENESIS_PAD_UP},   {"DOWN", GENESIS_PAD_DOWN}, {"LEFT", GENESIS_PAD_LEFT}, {"RIGHT", GENESIS_PAD_RIGHT},
    {"A", GENESIS_PAD_A},     {"B", GENESIS_PAD_B},       {"C", GENESIS_PAD_C},       {"START", GENESIS_PAD_START},
};

/* Parse a decimal tick; 0 on a non-digit, an empty field or a value above 2^63-1 (error class chosen by the caller). */
static int scd_input_tick(const char *s, size_t n, uint64_t *out, const char **error) {
  uint64_t v = 0U;
  size_t i;
  if (n == 0U) {
    *error = "bad_tick";
    return 0;
  }
  for (i = 0; i < n; ++i) {
    const unsigned d = (unsigned)(s[i] - '0');
    if (d > 9U) {
      *error = "bad_tick";
      return 0;
    }
    if (v > (UINT64_C(0x7FFFFFFFFFFFFFFF) - d) / 10U) {
      *error = "tick_range";
      return 0;
    }
    v = v * 10U + d;
  }
  *out = v;
  return 1;
}

/* One comma-separated button list into a mask; refusal classes: bad_button, unsupported_button, duplicate_button. */
static int scd_input_mask(const char *s, size_t n, uint8_t *mask, const char **error) {
  size_t start = 0U;
  uint8_t m = 0U;
  if (n == 0U) {
    *error = "bad_button";
    return 0;
  }
  for (;;) {
    size_t end = start;
    size_t b;
    uint8_t found = 0U;
    while (end < n && s[end] != ',') ++end;
    for (b = 0U; b < sizeof scd_input_buttons / sizeof scd_input_buttons[0]; ++b) {
      const size_t len = strlen(scd_input_buttons[b].name);
      if (len == end - start && memcmp(s + start, scd_input_buttons[b].name, len) == 0) found = scd_input_buttons[b].mask;
    }
    if (found == 0U) {
      const size_t len = end - start;
      const int unsupported = (len == 1U && (s[start] == 'X' || s[start] == 'Y' || s[start] == 'Z')) ||
                              (len == 4U && memcmp(s + start, "MODE", 4U) == 0);
      *error = unsupported ? "unsupported_button" : "bad_button";
      return 0;
    }
    if ((m & found) != 0U) {
      *error = "duplicate_button";
      return 0;
    }
    m = (uint8_t)(m | found);
    if (end == n) break;
    start = end + 1U;
    if (start == n) { /* a trailing comma */
      *error = "bad_button";
      return 0;
    }
  }
  *mask = m;
  return 1;
}

int scd_input_parse(const char *text, size_t length, ScdInputEvent *events, size_t *count, const char **error, unsigned *line) {
  uint64_t last_touch[2][8];
  uint8_t held[2] = {0U, 0U};
  uint64_t last_tick = 0U;
  size_t pos = 0U, n = 0U;
  unsigned number = 0U;
  int have_header = 0;
  unsigned p, b;
  *count = 0U;
  *line = 0U;
  *error = NULL;
  for (p = 0U; p < 2U; ++p)
    for (b = 0U; b < 8U; ++b) last_touch[p][b] = UINT64_MAX;
  if (text == NULL || length > SCD_INPUT_MAX_SCRIPT_BYTES) {
    *error = "too_large";
    return 1;
  }
  if (memchr(text, 0, length) != NULL) {
    *error = "binary";
    return 1;
  }
  while (pos < length) {
    size_t end = pos, len, f[4], fl[4], k;
    unsigned fields = 0U;
    const char *l;
    ++number;
    while (end < length && text[end] != '\n') ++end;
    len = end - pos;
    l = text + pos;
    pos = end < length ? end + 1U : end;
    if (len > 0U && l[len - 1U] == '\r') --len;
    *line = number;
    if (len > SCD_INPUT_MAX_LINE) {
      *error = "line_too_long";
      return 1;
    }
    if (len == 0U || l[0] == '#') continue;
    if (!have_header) {
      static const char header[] = "segarecomp-input-script 1";
      const size_t hl = sizeof header - 1U;
      if (len == hl && memcmp(l, header, hl) == 0) {
        have_header = 1;
        continue;
      }
      *error = (len >= 23U && memcmp(l, "segarecomp-input-script", 23U) == 0) ? "bad_header" : "missing_header";
      return 1;
    }
    /* exactly four single-space-separated, non-empty fields */
    for (k = 0U; k <= len; ++k) {
      size_t s = k;
      while (k < len && l[k] != ' ') ++k;
      if (fields < 4U) {
        f[fields] = s;
        fl[fields] = k - s;
      }
      ++fields;
    }
    if (fields != 4U || fl[0] == 0U || fl[1] == 0U || fl[2] == 0U || fl[3] == 0U) {
      *error = "bad_event";
      return 1;
    }
    {
      uint64_t tick = 0U;
      uint8_t mask = 0U, port, press;
      if (!scd_input_tick(l + f[0], fl[0], &tick, error)) return 1;
      if (fl[1] != 1U || (l[f[1]] != '1' && l[f[1]] != '2')) {
        *error = "bad_port";
        return 1;
      }
      port = (uint8_t)(l[f[1]] - '0');
      if (fl[2] == 5U && memcmp(l + f[2], "press", 5U) == 0) press = 1U;
      else if (fl[2] == 7U && memcmp(l + f[2], "release", 7U) == 0) press = 0U;
      else {
        *error = "bad_action";
        return 1;
      }
      if (!scd_input_mask(l + f[3], fl[3], &mask, error)) return 1;
      if (n > 0U && tick < last_tick) {
        *error = "tick_order";
        return 1;
      }
      for (b = 0U; b < 8U; ++b) {
        if (((mask >> b) & 1U) == 0U) continue;
        if (last_touch[port - 1U][b] == tick) {
          *error = "same_tick_conflict";
          return 1;
        }
        last_touch[port - 1U][b] = tick;
      }
      if (press ? (held[port - 1U] & mask) != 0U : (held[port - 1U] & mask) != mask) {
        *error = "redundant_transition";
        return 1;
      }
      held[port - 1U] = press ? (uint8_t)(held[port - 1U] | mask) : (uint8_t)(held[port - 1U] & ~mask);
      if (n >= SCD_INPUT_MAX_EVENTS) {
        *error = "too_many_events";
        return 1;
      }
      events[n].tick = tick;
      events[n].port = port;
      events[n].press = press;
      events[n].mask = mask;
      ++n;
      last_tick = tick;
    }
  }
  if (!have_header) {
    *line = 0U;
    *error = "missing_header";
    return 1;
  }
  *count = n;
  *line = 0U;
  return 0;
}

int scd_input_install(ScdMachine *m, const char *text, size_t length, int log, const char **error, unsigned *line) {
  ScdInput *in;
  ScdInputEvent *events = (ScdInputEvent *)malloc(SCD_INPUT_MAX_EVENTS * sizeof *events);
  size_t count = 0U;
  if (events == NULL) {
    *error = "memory";
    return 1;
  }
  if (scd_input_parse(text, length, events, &count, error, line) != 0) {
    free(events);
    return 1;
  }
  in = (ScdInput *)calloc(1U, sizeof *in);
  if (in == NULL) {
    free(events);
    *error = "memory";
    return 1;
  }
  in->events = events;
  in->count = count;
  if (log) {
    in->log = (ScdInputRead *)malloc(SCD_INPUT_LOG_MAX * sizeof *in->log);
    if (in->log == NULL) {
      free(events);
      free(in);
      *error = "memory";
      return 1;
    }
  }
  scd_input_release(m);
  m->input = in;
  return 0;
}

uint64_t scd_input_next_event(const ScdMachine *m) {
  const ScdInput *in = m->input;
  return (in != NULL && in->next < in->count) ? in->events[in->next].tick : UINT64_MAX;
}

void scd_input_advance(ScdMachine *m, uint64_t until) {
  ScdInput *in = m->input;
  if (in == NULL) return;
  while (in->next < in->count && in->events[in->next].tick <= until) {
    const ScdInputEvent *e = &in->events[in->next++];
    uint8_t *pad = &in->pad[e->port - 1U];
    *pad = e->press ? (uint8_t)(*pad | e->mask) : (uint8_t)(*pad & ~e->mask);
    scd_vdp_set_pad(m, e->port, *pad);
  }
}

void scd_input_note_read(ScdMachine *m, uint32_t port, uint32_t value) {
  ScdInput *in = m->input;
  if (in == NULL || port < 1U || port > 2U) return;
  in->reads[port - 1U] += 1U;
  if (in->log == NULL) return;
  if (in->log_count >= SCD_INPUT_LOG_MAX) {
    in->log_dropped += 1U;
    return;
  }
  in->log[in->log_count].tick = m->now;
  in->log[in->log_count].onset = m->vdp_frames;
  in->log[in->log_count].port = (uint8_t)port;
  in->log[in->log_count].value = (uint8_t)value;
  in->log_count += 1U;
}

void scd_input_report(const ScdMachine *m, FILE *out) {
  const ScdInput *in = m->input;
  uint64_t fnv = UINT64_C(0xCBF29CE484222325);
  size_t i;
  unsigned k;
  if (in == NULL) return;
  for (i = 0U; i < in->log_count; ++i) {
    uint64_t words[3];
    words[0] = in->log[i].tick;
    words[1] = in->log[i].onset;
    words[2] = ((uint64_t)in->log[i].port << 8) | in->log[i].value;
    for (k = 0U; k < 3U; ++k) {
      unsigned b;
      for (b = 0U; b < 8U; ++b) {
        fnv ^= (words[k] >> (8U * b)) & 0xFFU;
        fnv *= UINT64_C(0x100000001B3);
      }
    }
  }
  fprintf(out, ",\"input\":{\"events\":%llu,\"applied\":%llu,\"pad\":[%u,%u],\"reads\":[%llu,%llu],\"logged\":%llu,\"log_dropped\":%llu,"
               "\"log_fnv\":\"%016llx\"}",
          (unsigned long long)in->count, (unsigned long long)in->next, in->pad[0], in->pad[1], (unsigned long long)in->reads[0],
          (unsigned long long)in->reads[1], (unsigned long long)in->log_count, (unsigned long long)in->log_dropped,
          (unsigned long long)fnv);
}

int scd_input_write_log(const ScdMachine *m, const char *path) {
  const ScdInput *in = m->input;
  FILE *f;
  size_t i;
  if (in == NULL || in->log == NULL) return 1;
  f = fopen(path, "w");
  if (f == NULL) return 1;
  for (i = 0U; i < in->log_count; ++i)
    fprintf(f, "%llu %llu %u %02X\n", (unsigned long long)in->log[i].tick, (unsigned long long)in->log[i].onset, in->log[i].port,
            in->log[i].value);
  return fclose(f) != 0;
}

/* ---- SEG-078-T001 (ADR 0204): live recording ------------------------------------------------------------------------------ */

#define SCD_INPUT_HEADER "segarecomp-input-script 1\n"

int scd_input_live_begin(ScdMachine *m, int log) {
  const char *error = NULL;
  unsigned line = 0U;
  if (scd_input_install(m, SCD_INPUT_HEADER, sizeof SCD_INPUT_HEADER - 1U, log, &error, &line) != 0) return 1;
  m->input->live = 1U;
  m->input->script_bytes = sizeof SCD_INPUT_HEADER - 1U;
  return 0;
}

/* One serialised event line into `out` (at least 64 bytes); returns its length. Buttons in the table's order. */
static size_t scd_input_format(const ScdInputEvent *e, char *out) {
  size_t n = (size_t)sprintf(out, "%llu %u %s ", (unsigned long long)e->tick, (unsigned)e->port, e->press ? "press" : "release");
  int first = 1;
  size_t b;
  for (b = 0U; b < sizeof scd_input_buttons / sizeof scd_input_buttons[0]; ++b) {
    if ((e->mask & scd_input_buttons[b].mask) == 0U) continue;
    n += (size_t)sprintf(out + n, "%s%s", first ? "" : ",", scd_input_buttons[b].name);
    first = 0;
  }
  out[n++] = '\n';
  out[n] = '\0';
  return n;
}

ScdLiveSample scd_input_live_sample(ScdMachine *m, uint64_t tick, uint8_t pad1) {
  ScdInput *in = m->input;
  ScdInputEvent pending[2];
  size_t need = 0U, bytes = 0U, i;
  const uint8_t was = in != NULL ? in->live_pad[0] : 0U;
  const uint8_t released = (uint8_t)(was & ~pad1), pressed = (uint8_t)(pad1 & ~was);
  char line[64];
  if (in == NULL || !in->live) return SCD_LIVE_SAMPLE_NO_INPUT;
  if (released == 0U && pressed == 0U) return SCD_LIVE_SAMPLE_OK;  /* nothing changed: the recorder holds no redundant transition */
  if (tick <= m->now || tick > UINT64_C(0x7FFFFFFFFFFFFFFF) || (in->count > 0U && tick <= in->events[in->count - 1U].tick))
    return SCD_LIVE_SAMPLE_PASSED;
  if (released != 0U) {
    pending[need].tick = tick; pending[need].port = 1U; pending[need].press = 0U; pending[need].mask = released;
    ++need;
  }
  if (pressed != 0U) {
    pending[need].tick = tick; pending[need].port = 1U; pending[need].press = 1U; pending[need].mask = pressed;
    ++need;
  }
  for (i = 0U; i < need; ++i) bytes += scd_input_format(&pending[i], line);
  if (in->count + need > SCD_INPUT_MAX_EVENTS || in->script_bytes + bytes > SCD_INPUT_MAX_SCRIPT_BYTES) return SCD_LIVE_SAMPLE_CAPACITY;
  for (i = 0U; i < need; ++i) in->events[in->count++] = pending[i];
  in->script_bytes += bytes;
  in->live_pad[0] = pad1;
  return SCD_LIVE_SAMPLE_OK;
}

size_t scd_input_serialize(const ScdMachine *m, char *out, size_t capacity) {
  const ScdInput *in = m->input;
  size_t n, i;
  static ScdInputEvent check[SCD_INPUT_MAX_EVENTS];
  size_t count = 0U;
  const char *error = NULL;
  unsigned line = 0U;
  if (in == NULL || !in->live || capacity < in->script_bytes + 1U) return 0U;
  memcpy(out, SCD_INPUT_HEADER, sizeof SCD_INPUT_HEADER - 1U);
  n = sizeof SCD_INPUT_HEADER - 1U;
  for (i = 0U; i < in->count; ++i) n += scd_input_format(&in->events[i], out + n);
  out[n] = '\0';
  if (n != in->script_bytes || scd_input_parse(out, n, check, &count, &error, &line) != 0 || count != in->count) return 0U;
  return n;
}

int scd_input_live_write(const ScdMachine *m, const char *path) {
  static char text[SCD_INPUT_MAX_SCRIPT_BYTES + 1U];
  FILE *f;
  const size_t n = scd_input_serialize(m, text, sizeof text);
  if (n == 0U || (f = fopen(path, "wb")) == NULL) return 1;
  if (fwrite(text, 1U, n, f) != n) {
    fclose(f);
    return 1;
  }
  return fclose(f) != 0;
}

void scd_input_release(ScdMachine *m) {
  if (m->input == NULL) return;
  free(m->input->events);
  free(m->input->log);
  free(m->input);
  m->input = NULL;
}
