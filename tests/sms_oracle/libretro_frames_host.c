/*
 * SEG-009-T008 (ADR 0062): test-only libretro host that runs a project-authored fixture for a fixed number of frames under
 * a scripted input and dumps what a black-box reference exposes, frame by frame. It dlopen()s a core built locally from a
 * pinned checkout (never linked into production, never distributed). The libretro API subset is declared locally (values
 * from the public MIT-licensed libretro.h), as in libretro_host.c.
 *
 *   libretro_frames_host <core> <rom> <frames> <input script> <out dir> [key=value option]...
 *
 * The input script is the platform's scripted-input text (`<frame> <p1 UDLR12> <p2 UDLR12> <pause P|->`, `#` comments):
 * an event takes effect at the first retro_run whose index is >= its frame and holds until the next one; pause is the
 * Start button of player 1. Output in <out dir>:
 *   frames.rgb  every retro_run's frame, RGB888 row-major, consecutively (the dimensions are in meta.txt)
 *   meta.txt    one line per retro_run: `<run> <width> <height> <audio frames produced> <RAM $C010>`
 *   ram.bin     the first 2 KiB of system RAM after the last frame
 *   ram_frames.bin  only with the host option `host_ram_frames=1` (consumed by the host, never passed to the core): the full
 *               8 KiB of system RAM after every retro_run, consecutively (for frame-by-frame work-RAM comparison)
 *   audio.raw   interleaved s16le stereo samples as produced by the core
 *   av.txt      `<sample rate> <fps>`
 * Exit: 0 success, 2 host/core failure.
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
#define RETRO_DEVICE_JOYPAD 1
#define RETRO_DEVICE_ID_JOYPAD_B 0
#define RETRO_DEVICE_ID_JOYPAD_START 3
#define RETRO_DEVICE_ID_JOYPAD_UP 4
#define RETRO_DEVICE_ID_JOYPAD_DOWN 5
#define RETRO_DEVICE_ID_JOYPAD_LEFT 6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT 7
#define RETRO_DEVICE_ID_JOYPAD_A 8
#define RETRO_MEMORY_SYSTEM_RAM 2
#define PIXEL_0RGB1555 0
#define PIXEL_XRGB8888 1
#define PIXEL_RGB565 2

struct retro_variable { const char *key; const char *value; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_system_av_info { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; double fps, sample_rate; };
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
#define MAX_EVENTS 4096
static struct retro_variable options[MAX_OPTIONS];
static int option_count;
static unsigned pixel_format = PIXEL_0RGB1555;
static uint8_t *frame; /* RGB888 of the last video callback */
static unsigned frame_w, frame_h;
static unsigned joypad[2];
static FILE *audio_file;
static unsigned long audio_frames_run;
static const char *tmp_dir = ".";
static int host_ram_frames;

struct event { unsigned long frame; unsigned pad[2]; unsigned pause; };
static struct event events[MAX_EVENTS];
static int event_count;

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
  uint8_t *next = realloc(frame, (size_t)w * h * 3);
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
      frame[((size_t)y * w + x) * 3 + 0] = (uint8_t)r;
      frame[((size_t)y * w + x) * 3 + 1] = (uint8_t)g;
      frame[((size_t)y * w + x) * 3 + 2] = (uint8_t)b;
    }
  }
}
static void audio(int16_t l, int16_t r) {
  int16_t pair[2] = {l, r};
  if (audio_file != NULL) fwrite(pair, sizeof pair, 1, audio_file);
  ++audio_frames_run;
}
static size_t audio_batch(const int16_t *d, size_t n) {
  if (audio_file != NULL) fwrite(d, 2u * sizeof *d, n, audio_file);
  audio_frames_run += (unsigned long)n;
  return n;
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)index;
  if (port > 1 || device != RETRO_DEVICE_JOYPAD || id > 15) return 0;
  return (int16_t)((joypad[port] >> id) & 1U);
}

static unsigned map_pad(const char *text) {
  static const unsigned ids[6] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
                                  RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A};
  static const char letters[6] = {'U', 'D', 'L', 'R', '1', '2'};
  unsigned mask = 0;
  for (int i = 0; i < 6; ++i)
    if (text[i] == letters[i]) mask |= 1u << ids[i];
  return mask;
}

static int load_script(const char *path) {
  FILE *f = fopen(path, "r");
  char line[256];
  if (f == NULL) return 0;
  while (fgets(line, sizeof line, f) != NULL) {
    unsigned long fr;
    char a[16], b[16], p[8];
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%lu %15s %15s %7s", &fr, a, b, p) != 4 || strlen(a) != 6 || strlen(b) != 6 || event_count >= MAX_EVENTS) {
      fclose(f);
      return 0;
    }
    events[event_count].frame = fr;
    events[event_count].pad[0] = map_pad(a);
    events[event_count].pad[1] = map_pad(b);
    events[event_count].pause = p[0] == 'P';
    ++event_count;
  }
  fclose(f);
  return 1;
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
typedef void (*av_fn)(struct retro_system_av_info *);

#define SYM(type, name)                                                                            \
  type name;                                                                                       \
  do {                                                                                             \
    void *symbol = dlsym(core, #name);                                                             \
    if (symbol == NULL) { fprintf(stderr, "missing %s\n", #name); return 2; }                      \
    memcpy(&name, &symbol, sizeof name);                                                           \
  } while (0);

static FILE *open_out(const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  return fopen(path, "wb");
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: libretro_frames_host <core> <rom> <frames> <script> <out dir> [key=value]...\n"); return 2; }
  const long max_frames = strtol(argv[3], NULL, 10);
  if (max_frames <= 0 || max_frames > 100000) { fprintf(stderr, "frames must be finite and positive\n"); return 2; }
  if (!load_script(argv[4])) { fprintf(stderr, "bad input script\n"); return 2; }
  const char *out = argv[5];
  for (int i = 6; i < argc && option_count < MAX_OPTIONS; ++i) {
    char *eq = strchr(argv[i], '=');
    if (eq == NULL) { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    *eq = '\0';
    if (strcmp(argv[i], "host_ram_frames") == 0) { host_ram_frames = eq[1] == '1'; continue; } /* host-only option */
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
  SYM(av_fn, retro_get_system_av_info)
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
  struct retro_system_av_info av;
  memset(&av, 0, sizeof av);
  retro_get_system_av_info(&av);

  FILE *frames_file = open_out(out, "frames.rgb"), *meta = open_out(out, "meta.txt"), *avf = open_out(out, "av.txt");
  audio_file = open_out(out, "audio.raw");
  if (frames_file == NULL || meta == NULL || avf == NULL || audio_file == NULL) { fprintf(stderr, "cannot write %s\n", out); return 2; }
  fprintf(avf, "%.3f %.6f\n", av.sample_rate, av.fps);
  fclose(avf);

  FILE *ram_frames_file = host_ram_frames ? open_out(out, "ram_frames.bin") : NULL;
  if (host_ram_frames && ram_frames_file == NULL) { fprintf(stderr, "cannot write %s\n", out); return 2; }
  int next_event = 0;
  unsigned pause = 0;
  for (long run = 0; run < max_frames; ++run) {
    while (next_event < event_count && events[next_event].frame <= (unsigned long)run) {
      joypad[0] = events[next_event].pad[0];
      joypad[1] = events[next_event].pad[1];
      pause = events[next_event].pause;
      ++next_event;
    }
    joypad[0] = (joypad[0] & ~(1u << RETRO_DEVICE_ID_JOYPAD_START)) | (pause ? 1u << RETRO_DEVICE_ID_JOYPAD_START : 0u);
    audio_frames_run = 0;
    retro_run();
    if (frame != NULL) fwrite(frame, 3, (size_t)frame_w * frame_h, frames_file);
    if (ram_frames_file != NULL && fwrite(ram, 1, 0x2000, ram_frames_file) != 0x2000) { fprintf(stderr, "cannot write ram frames\n"); return 2; }
    fprintf(meta, "%ld %u %u %lu %u\n", run, frame_w, frame_h, audio_frames_run, (unsigned)ram[0x10]);
  }
  fclose(frames_file);
  fclose(meta);
  if (ram_frames_file != NULL) fclose(ram_frames_file);
  fclose(audio_file);
  FILE *ram_file = open_out(out, "ram.bin");
  if (ram_file == NULL || fwrite(ram, 1, 0x800, ram_file) != 0x800) { fprintf(stderr, "cannot write ram\n"); return 2; }
  fclose(ram_file);
  retro_unload_game();
  retro_deinit();
  free(frame);
  return 0;
}
