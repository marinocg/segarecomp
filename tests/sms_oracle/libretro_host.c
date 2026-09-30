/*
 * SEG-009-T001 (ADR 0062): test-only libretro host for the Master System whole-machine oracle
 * smoke. It dlopen()s a core built locally from a pinned checkout (never linked into production,
 * never distributed), loads a project-authored fixture ROM, and drives the phase/acknowledge
 * handshake of the `oracle_smoke` fixture (tools/sms_fixture_rom.py):
 *
 *   RAM 0xC0F5 = phase written by the guest program, RAM 0xC0F6 = acknowledge written here.
 *   phase 1: capture framebuffer A after two frames;   phase 2: capture framebuffer B;
 *   phase 3: pause-button script (press 6 frames, release 4, press 1, release 3);
 *   phase 4: hold UP for three frames, then report.
 *
 * Output (stdout): one JSON object with the result block 0xC100-0xC1FF, the two framebuffer
 * summaries and the frame count. Expectations and citations live in tests/sms_oracle_smoke_test.py.
 *
 * The libretro API subset below is declared locally (values from the public MIT-licensed
 * libretro.h) so the host does not depend on either core's source tree.
 *
 * usage: libretro_host <core.so> <rom path> <max frames> [key=value option]...
 */
#include <dlfcn.h>
#include <stdarg.h>
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
#define RETRO_DEVICE_JOYPAD 1
#define RETRO_DEVICE_ID_JOYPAD_START 3
#define RETRO_DEVICE_ID_JOYPAD_UP 4
#define RETRO_MEMORY_SYSTEM_RAM 2
#define PIXEL_0RGB1555 0
#define PIXEL_XRGB8888 1
#define PIXEL_RGB565 2

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
static unsigned pixel_format = PIXEL_0RGB1555;
static uint32_t *frame; /* RGB888 in the low 24 bits */
static unsigned frame_w, frame_h;
static unsigned joypad; /* bit per RETRO_DEVICE_ID_JOYPAD_* */
static const char *tmp_dir = ".";

static void log_discard(enum retro_log_level level, const char *fmt, ...) { (void)level; (void)fmt; }

static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
    unsigned f = *(const unsigned *)data;
    if (f > PIXEL_RGB565) return false;
    pixel_format = f;
    return true;
  }
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

static uint32_t expand(unsigned v, unsigned bits) { return (v << (8 - bits)) | (v >> (2 * bits - 8)); }

static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data == NULL) return; /* duped frame: keep the previous one */
  uint32_t *next = realloc(frame, (size_t)w * h * sizeof *frame);
  if (next == NULL) { fprintf(stderr, "out of memory\n"); exit(2); }
  frame = next; frame_w = w; frame_h = h;
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t *row = (const uint8_t *)data + y * pitch;
    for (unsigned x = 0; x < w; ++x) {
      uint32_t r, g, b;
      if (pixel_format == PIXEL_XRGB8888) {
        uint32_t p = ((const uint32_t *)(const void *)row)[x];
        r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
      } else {
        uint16_t p = ((const uint16_t *)(const void *)row)[x];
        if (pixel_format == PIXEL_RGB565) { r = expand(p >> 11, 5); g = expand((p >> 5) & 0x3F, 6); b = expand(p & 0x1F, 5); }
        else { r = expand((p >> 10) & 0x1F, 5); g = expand((p >> 5) & 0x1F, 5); b = expand(p & 0x1F, 5); }
      }
      frame[(size_t)y * w + x] = (r << 16) | (g << 8) | b;
    }
  }
}
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port != 0 || device != RETRO_DEVICE_JOYPAD || id > 15) return 0;
  return (int16_t)((joypad >> id) & 1U);
}

/* Summary of the active area: the core's frame may include borders; the fixture paints the
 * backdrop red, the background black and two white tiles at name-table row 0, columns 0-1. */
static void frame_summary(const char *name, char *out, size_t size) {
  if (frame == NULL) { snprintf(out, size, "\"%s\":null", name); return; }
  uint32_t white = 0xFFFFFF;
  size_t whites = 0;
  long min_x = -1, min_y = -1, max_x = -1, max_y = -1;
  for (unsigned y = 0; y < frame_h; ++y)
    for (unsigned x = 0; x < frame_w; ++x)
      if (frame[(size_t)y * frame_w + x] == white) {
        ++whites;
        if (min_x < 0 || (long)x < min_x) min_x = (long)x;
        if (min_y < 0 || (long)y < min_y) min_y = (long)y;
        if ((long)x > max_x) max_x = (long)x;
        if ((long)y > max_y) max_y = (long)y;
      }
  /* colour immediately left of the first white pixel (column-0 content in phase B) */
  uint32_t left = 0xFFFFFFFFu, below = 0xFFFFFFFFu;
  if (min_x > 0) left = frame[(size_t)min_y * frame_w + (size_t)(min_x - 1)];
  if (max_y >= 0 && (unsigned)max_y + 1 < frame_h) below = frame[(size_t)(max_y + 1) * frame_w + (size_t)min_x];
  snprintf(out, size,
           "\"%s\":{\"width\":%u,\"height\":%u,\"white_pixels\":%zu,\"white_box\":[%ld,%ld,%ld,%ld],"
           "\"left_of_white\":%ld,\"below_white\":%ld}",
           name, frame_w, frame_h, whites, min_x, min_y, max_x, max_y,
           left == 0xFFFFFFFFu ? -1L : (long)left, below == 0xFFFFFFFFu ? -1L : (long)below);
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

/* ISO C has no object-to-function pointer conversion; copy the representation (POSIX dlsym). */
#define SYM(type, name)                                                                            \
  type name;                                                                                       \
  do {                                                                                             \
    void *symbol = dlsym(core, #name);                                                             \
    if (symbol == NULL) { fprintf(stderr, "missing %s\n", #name); return 2; }                      \
    memcpy(&name, &symbol, sizeof name);                                                           \
  } while (0);

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: libretro_host <core> <rom> <max-frames> [key=value]...\n"); return 2; }
  const long max_frames = strtol(argv[3], NULL, 10);
  if (max_frames <= 0 || max_frames > 100000) { fprintf(stderr, "max frames must be finite and positive\n"); return 2; }
  for (int i = 4; i < argc && option_count < MAX_OPTIONS; ++i) {
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
  static uint8_t rom[0x100000];
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
  if (ram == NULL || ram_size < 0x2000) { fprintf(stderr, "system RAM not exposed\n"); return 2; }

  char summary_a[512] = "\"frame_a\":null", summary_b[512] = "\"frame_b\":null";
  long frames = 0;
  unsigned handled = 0;
  static const unsigned pause_script[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0};
  bool done = false;
  while (!done && frames < max_frames) {
    retro_run();
    ++frames;
    unsigned phase = ram[0x00F5];
    if (phase == handled || phase == 0) continue;
    if (phase == 1 || phase == 2) {
      retro_run(); retro_run(); frames += 2;
      frame_summary(phase == 1 ? "frame_a" : "frame_b", phase == 1 ? summary_a : summary_b,
                    phase == 1 ? sizeof summary_a : sizeof summary_b);
    } else if (phase == 3) {
      for (size_t i = 0; i < sizeof pause_script / sizeof pause_script[0]; ++i) {
        joypad = pause_script[i] ? (1U << RETRO_DEVICE_ID_JOYPAD_START) : 0U;
        retro_run(); ++frames;
      }
      joypad = 0;
    } else if (phase == 4) {
      joypad = 1U << RETRO_DEVICE_ID_JOYPAD_UP;
      retro_run(); retro_run(); retro_run(); frames += 3;
      joypad = 0;
      done = true;
    } else {
      fprintf(stderr, "unexpected phase %u\n", phase);
      return 3;
    }
    handled = phase;
    ram[0x00F6] = (uint8_t)phase;
  }
  printf("{\"frames\":%ld,\"complete\":%s,\"results\":\"", frames, done ? "true" : "false");
  for (unsigned i = 0x100; i < 0x200; ++i) printf("%02X", ram[i]);
  printf("\",%s,%s}\n", summary_a, summary_b);
  retro_unload_game();
  retro_deinit();
  free(frame);
  return done ? 0 : 4;
}
