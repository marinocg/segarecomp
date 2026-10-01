#include "sms_input.h"

static int is_space(char c) { return c == ' ' || c == '\t'; }

static int parse_mask(const char *p, uint8_t *mask) {
  static const char letters[6] = {'U', 'D', 'L', 'R', '1', '2'};
  unsigned i;
  *mask = 0;
  for (i = 0; i < 6u; ++i) {
    if (p[i] == letters[i]) *mask = (uint8_t)(*mask | (1u << i));
    else if (p[i] != '-') return 0;
  }
  return 1;
}

unsigned sms_input_parse(const char *text, size_t size, SmsInputEvent *events, uint32_t capacity, uint32_t *count) {
  size_t pos = 0;
  unsigned line_no = 0;
  uint64_t last_frame = 0;
  *count = 0;
  while (pos < size) {
    size_t end = pos;
    size_t p;
    uint64_t frame = 0;
    int digits = 0;
    SmsInputEvent ev;
    ++line_no;
    while (end < size && text[end] != '\n') ++end;
    p = pos;
    pos = end < size ? end + 1u : end;
    while (p < end && is_space(text[p])) ++p;
    if (p >= end || text[p] == '#' || text[p] == '\r') continue;
    while (p < end && text[p] >= '0' && text[p] <= '9') {
      if (frame > (UINT64_MAX - 9u) / 10u) return line_no;
      frame = frame * 10u + (uint64_t)(text[p] - '0');
      ++p;
      ++digits;
    }
    if (digits == 0 || p >= end || !is_space(text[p])) return line_no;
    while (p < end && is_space(text[p])) ++p;
    if (end - p < 6u || !parse_mask(text + p, &ev.p1)) return line_no;
    p += 6u;
    if (p >= end || !is_space(text[p])) return line_no;
    while (p < end && is_space(text[p])) ++p;
    if (end - p < 6u || !parse_mask(text + p, &ev.p2)) return line_no;
    p += 6u;
    if (p >= end || !is_space(text[p])) return line_no;
    while (p < end && is_space(text[p])) ++p;
    if (p >= end || (text[p] != 'P' && text[p] != '-')) return line_no;
    ev.pause = text[p] == 'P' ? 1u : 0u;
    ++p;
    while (p < end && is_space(text[p])) ++p;
    if (p < end && text[p] != '#' && text[p] != '\r') return line_no;
    if (*count > 0 && frame < last_frame) return line_no;
    if (*count >= capacity) return line_no;
    ev.frame = frame;
    last_frame = frame;
    events[(*count)++] = ev;
  }
  return 0;
}
