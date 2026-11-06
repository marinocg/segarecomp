/*
 * SEG-032-T001 (ADR 0072): test-only libretro host for the Genesis Z80/audio whole-machine reference smoke.
 * It dlopen()s a core built locally from a pinned checkout (never linked into production, never distributed), loads a
 * project-authored fixture ROM (tools/genesis_z80_fixture_rom.py), runs frames until the fixture writes the completion
 * byte $A5 at work RAM $FF00FF, and prints one JSON object: the 256-byte result block at $FF0000, the frame count and
 * the number and digest of the audio frames the core produced (the PCM itself can be written with --pcm).
 *
 * The libretro API subset is declared locally (values from the public MIT-licensed libretro.h).
 *
 * usage: genesis_libretro_host <core> <rom> <max frames> [--pcm FILE] [--pad FILE] [--capture DIR first stride count] [key=value option]...
 *
 * SEG-122-T011 (ADR 0272), optional and inert unless given: `--pad FILE` applies a frame-keyed player-1 mask (lines `<frame> <hex mask>`, native
 * GENESIS_PAD_* bits: 01 up 02 down 04 left 08 right 10 B 20 C 40 A 80 Start) from retro_run number <frame> (0-based) on; `--capture DIR first stride count`
 * writes the video frame after retro_run number first + k*stride (1-based, k < count, count <= 64) as DIR/frame-<n>.ppm and the 64 KiB system RAM as
 * DIR/ram-<n>.bin. Time advances only by retro_run; no host clock is involved.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_MEMORY_SYSTEM_RAM 2

struct retro_variable { const char *key; const char *value; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
enum retro_log_level { RETRO_LOG_DEBUG = 0 };
typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };
typedef bool (*env_cb)(unsigned, void *);
typedef void (*video_cb)(const void *, unsigned, unsigned, size_t);
typedef void (*audio_cb)(int16_t, int16_t);
typedef size_t (*audio_batch_cb)(const int16_t *, size_t);
typedef void (*poll_cb)(void);
typedef int16_t (*input_cb)(unsigned, unsigned, unsigned, unsigned);

#define MAX_OPTIONS 16
static struct retro_variable options[MAX_OPTIONS];
static int option_count;
static const char *tmp_dir = ".";
static FILE *pcm_file;
static unsigned pixel_format;  /* 0 = 0RGB1555, 1 = XRGB8888, 2 = RGB565 (libretro enum values) */
static const char *capture_dir;
static long cap_first, cap_stride, cap_count;
static uint64_t audio_frames;
static uint64_t audio_hash = 0xcbf29ce484222325ULL;

static void log_discard(enum retro_log_level level, const char *fmt, ...) { (void)level; (void)fmt; }
static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixel_format = *(const unsigned *)data; return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    for (int i = 0; i < option_count; ++i)
      if (strcmp(options[i].key, v->key) == 0) { v->value = options[i].value; return true; }
    v->value = NULL;
    return false;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = tmp_dir; return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = log_discard; return true;
  default: return false;
  }
}
static uint8_t last_frame[512 * 512 * 3];
static unsigned last_w, last_h;
static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data == NULL || w > 512 || h > 512) return;  /* a duplicated frame keeps the previous pixels */
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x) {
      uint8_t r, g, b;
      const uint8_t *row = (const uint8_t *)data + y * pitch;
      if (pixel_format == 1) { const uint32_t p = ((const uint32_t *)row)[x]; r = (uint8_t)(p >> 16); g = (uint8_t)(p >> 8); b = (uint8_t)p; }
      else if (pixel_format == 2) { const uint16_t p = ((const uint16_t *)row)[x]; r = (uint8_t)(((p >> 11) & 31) * 255 / 31); g = (uint8_t)(((p >> 5) & 63) * 255 / 63); b = (uint8_t)((p & 31) * 255 / 31); }
      else { const uint16_t p = ((const uint16_t *)row)[x]; r = (uint8_t)(((p >> 10) & 31) * 255 / 31); g = (uint8_t)(((p >> 5) & 31) * 255 / 31); b = (uint8_t)((p & 31) * 255 / 31); }
      uint8_t *out = last_frame + (y * w + x) * 3;
      out[0] = r; out[1] = g; out[2] = b;
    }
  last_w = w; last_h = h;
}
static void take(const int16_t *d, size_t frames) {
  for (size_t i = 0; i < frames * 2; ++i) {
    uint16_t v = (uint16_t)d[i];
    audio_hash = (audio_hash ^ (v & 0xFF)) * 0x100000001b3ULL;
    audio_hash = (audio_hash ^ (v >> 8)) * 0x100000001b3ULL;
  }
  if (pcm_file != NULL) fwrite(d, sizeof d[0], frames * 2, pcm_file);
  audio_frames += frames;
}
static void audio(int16_t l, int16_t r) { int16_t p[2] = {l, r}; take(p, 1); }
static size_t audio_batch(const int16_t *d, size_t n) { take(d, n); return n; }
static void input_poll(void) {}
#define MAX_PAD 4096
static struct { long frame; unsigned mask; } pad_script[MAX_PAD];
static int pad_count;
static unsigned pad_mask;  /* native GENESIS_PAD_* bits currently held by player 1 */
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port != 0 || device != 1 || id != 256) return 0;  /* RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_MASK */
  /* native -> libretro joypad ids as the pinned core maps them: Genesis A = Y(1), B = B(0), C = A(8), Start = 3, Up 4, Down 5, Left 6, Right 7 */
  int16_t r = 0;
  if (pad_mask & 0x01U) r |= 1 << 4;
  if (pad_mask & 0x02U) r |= 1 << 5;
  if (pad_mask & 0x04U) r |= 1 << 6;
  if (pad_mask & 0x08U) r |= 1 << 7;
  if (pad_mask & 0x10U) r |= 1 << 0;
  if (pad_mask & 0x20U) r |= 1 << 8;
  if (pad_mask & 0x40U) r |= 1 << 1;
  if (pad_mask & 0x80U) r |= 1 << 3;
  return r;
}

typedef void (*set_env_fn)(env_cb);
typedef void (*set_video_fn)(video_cb);
typedef void (*set_audio_fn)(audio_cb);
typedef void (*set_audio_batch_fn)(audio_batch_cb);
typedef void (*set_poll_fn)(poll_cb);
typedef void (*set_input_fn)(input_cb);
typedef void (*void_fn)(void);
typedef bool (*load_fn)(const struct retro_game_info *);
typedef void *(*memory_data_fn)(unsigned);
typedef size_t (*memory_size_fn)(unsigned);

#define SYM(type, name)                                                                            \
  type name;                                                                                       \
  do {                                                                                             \
    void *symbol = dlsym(core, #name);                                                             \
    if (symbol == NULL) { fprintf(stderr, "missing %s\n", #name); return 2; }                      \
    memcpy(&name, &symbol, sizeof name);                                                           \
  } while (0);

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: genesis_libretro_host <core> <rom> <max-frames> [--pcm FILE] [key=value]...\n"); return 2; }
  const long max_frames = strtol(argv[3], NULL, 10);
  if (max_frames <= 0 || max_frames > 100000) { fprintf(stderr, "max frames must be finite and positive\n"); return 2; }
  for (int i = 4; i < argc; ++i) {
    if (strcmp(argv[i], "--pcm") == 0 && i + 1 < argc) { pcm_file = fopen(argv[++i], "wb"); continue; }
    if (strcmp(argv[i], "--pad") == 0 && i + 1 < argc) {
      FILE *pf = fopen(argv[++i], "r");
      if (pf == NULL) { fprintf(stderr, "cannot open pad script\n"); return 2; }
      while (pad_count < MAX_PAD && fscanf(pf, "%ld %x", &pad_script[pad_count].frame, &pad_script[pad_count].mask) == 2) ++pad_count;
      fclose(pf);
      continue;
    }
    if (strcmp(argv[i], "--capture") == 0 && i + 4 < argc) {
      capture_dir = argv[i + 1]; cap_first = strtol(argv[i + 2], NULL, 10); cap_stride = strtol(argv[i + 3], NULL, 10); cap_count = strtol(argv[i + 4], NULL, 10);
      if (cap_first < 1 || cap_stride < 1 || cap_count < 1 || cap_count > 64) { fprintf(stderr, "bad --capture\n"); return 2; }
      i += 4;
      continue;
    }
    if (option_count >= MAX_OPTIONS) return 2;
    char *eq = strchr(argv[i], '=');
    if (eq == NULL) { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    *eq = '\0';
    options[option_count].key = argv[i];
    options[option_count].value = eq + 1;
    ++option_count;
  }
  const char *env_tmp = getenv("TMPDIR");
  if (env_tmp != NULL) tmp_dir = env_tmp;
  FILE *rom_file = fopen(argv[2], "rb");
  if (rom_file == NULL) { fprintf(stderr, "cannot open rom\n"); return 2; }
  static uint8_t rom[0x400000];
  size_t rom_size = fread(rom, 1, sizeof rom, rom_file);
  fclose(rom_file);
  void *core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (core == NULL) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  SYM(set_env_fn, retro_set_environment)
  SYM(set_video_fn, retro_set_video_refresh)
  SYM(set_audio_fn, retro_set_audio_sample)
  SYM(set_audio_batch_fn, retro_set_audio_sample_batch)
  SYM(set_poll_fn, retro_set_input_poll)
  SYM(set_input_fn, retro_set_input_state)
  SYM(void_fn, retro_init)
  SYM(load_fn, retro_load_game)
  SYM(void_fn, retro_run)
  SYM(memory_data_fn, retro_get_memory_data)
  SYM(memory_size_fn, retro_get_memory_size)
  SYM(void_fn, retro_unload_game)
  SYM(void_fn, retro_deinit)
  retro_set_environment(environment);
  retro_init();
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio);
  retro_set_audio_sample_batch(audio_batch);
  retro_set_input_poll(input_poll);
  retro_set_input_state(input_state);
  struct retro_game_info info = {argv[2], rom, rom_size, NULL};
  if (!retro_load_game(&info)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }
  uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
  size_t ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
  if (ram == NULL || ram_size < 0x10000) { fprintf(stderr, "system RAM not exposed\n"); return 2; }
  long frames = 0;
  int xor1 = -1;
  while (frames < max_frames) {
    for (int k = 0; k < pad_count; ++k)
      if (pad_script[k].frame <= frames) pad_mask = pad_script[k].mask;  /* the last entry at or before this frame wins */
    retro_run();
    ++frames;
    if (capture_dir != NULL && frames >= cap_first && (frames - cap_first) % cap_stride == 0 && (frames - cap_first) / cap_stride < cap_count) {
      char path[1100];
      snprintf(path, sizeof path, "%s/frame-%ld.ppm", capture_dir, frames);
      FILE *out = fopen(path, "wb");
      if (out != NULL) { fprintf(out, "P6\n%u %u\n255\n", last_w, last_h); fwrite(last_frame, 1, (size_t)last_w * last_h * 3, out); fclose(out); }
      snprintf(path, sizeof path, "%s/ram-%ld.bin", capture_dir, frames);
      out = fopen(path, "wb");
      if (out != NULL) { fwrite(ram, 1, 0x10000, out); fclose(out); }
    }
    if (ram[0xFF] == 0xA5) { xor1 = 0; break; }
    if (ram[0xFE] == 0xA5) { xor1 = 1; break; }
  }
  if (xor1 >= 0) { retro_run(); retro_run(); frames += 2; }
  printf("{\"frames\":%ld,\"complete\":%s,\"results\":\"", frames, xor1 >= 0 ? "true" : "false");
  for (unsigned i = 0; i < 0x100; ++i) printf("%02X", ram[i ^ (xor1 > 0 ? 1 : 0)]);
  printf("\",\"audio_frames\":%llu,\"audio_fnv64\":\"%016llx\"}\n", (unsigned long long)audio_frames, (unsigned long long)audio_hash);
  if (pcm_file != NULL) fclose(pcm_file);
  retro_unload_game();
  retro_deinit();
  return xor1 >= 0 ? 0 : 4;
}
