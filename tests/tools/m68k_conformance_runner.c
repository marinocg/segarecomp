/* SEG-021-T003: generated-native runner. Linked with the emitter's translation unit (cf_table). Reads the
 * vector file (argv[1]), runs each vector's emitted function on a fresh copy of the common memory image and
 * prints the boundary-schema JSON. A vector whose encoding has no emitted function prints
 * {"id":..,"missing":1}. */
#include "m68k_conformance_common.h"
typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;
typedef struct { const char *code; int (*fn)(cap_state *); } cf_entry;
extern const cf_entry cf_table[];
uint32_t frame_ids[64], frame_continuations[64], frame_depth;
static cap_state state;
static uint8_t before[CF_MEM_SIZE];
int main(int argc, char **argv) {
  FILE *f; char line[4096]; cf_vector v; unsigned i;
  if (argc != 2 || !(f = fopen(argv[1], "r"))) return 2;
  while (fgets(line, sizeof line, f)) {
    int (*fn)(cap_state *) = NULL; unsigned d[8], a[8];
    if (!cf_parse(line, &v)) { fprintf(stderr, "bad vector line\n"); return 2; }
    for (i = 0; cf_table[i].code; ++i) if (strcmp(cf_table[i].code, v.code) == 0) fn = cf_table[i].fn;
    if (!fn) { printf("{\"id\":\"%s\",\"missing\":1}\n", v.id); continue; }
    cf_init_memory(state.ram, &v); memcpy(before, state.ram, sizeof before);
    for (i = 0; i < 8U; ++i) { state.d[i] = v.d[i]; state.a[i] = v.a[i]; }
    /* SEG-021-T018 / ADR 0043 §6: a[7] is the active stack pointer (seeded above from the explicit SSP/USP) and
       `usp` is the INACTIVE slot -- the USP in supervisor mode, the SSP in user mode. */
    state.sr = (uint16_t)v.sr; state.usp = (v.sr & 0x2000U) ? v.usp : v.ssp; state.pc = CF_CODE_BASE;
    memset(frame_ids, 0, sizeof frame_ids); memset(frame_continuations, 0, sizeof frame_continuations); frame_depth = 0U;
    (void)fn(&state);
    for (i = 0; i < 8U; ++i) { d[i] = state.d[i]; a[i] = state.a[i]; }
    { const unsigned supervisor = state.sr & 0x2000U; /* the generated model: active A7 plus the inactive slot */
      cf_print(&v, before, state.ram, state.pc, state.sr, supervisor ? state.usp : a[7], supervisor ? a[7] : state.usp, d, a); }
  }
  fclose(f); return 0;
}
