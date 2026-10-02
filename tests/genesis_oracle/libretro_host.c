/*
 * SEG-032-T001 (ADR 0072): test-only libretro host for the Genesis Z80/audio whole-machine reference smoke.
 * It dlopen()s a core built locally from a pinned checkout (never linked into production, never distributed), loads a
 * project-authored fixture ROM (tools/genesis_z80_fixture_rom.py), runs frames until the fixture writes the completion
 * byte $A5 at work RAM $FF00FF, and prints one JSON object: the 256-byte result block at $FF0000, the frame count and
 * the number and digest of the audio frames the core produced (the PCM itself can be written with --pcm).
 *
 * The libretro API subset is declared locally (values from the public MIT-licensed libretro.h).
 *
 * usage: genesis_libretro_host <core> <rom> <max frames> [--pcm FILE] [key=value option]...
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
static uint64_t audio_frames;
static uint64_t audio_hash = 0xcbf29ce484222325ULL;

static void log_discard(enum retro_log_level level, const char *fmt, ...) { (void)level; (void)fmt; }
static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
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
static void video(const void *data, unsigned w, unsigned h, size_t pitch) { (void)data; (void)w; (void)h; (void)pitch; }
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
static int16_t input_state(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

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
    retro_run();
    ++frames;
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
