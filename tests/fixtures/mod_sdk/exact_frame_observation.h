/* SEG-152-T003/T004: what the synthetic exact-frame provider (exact_frame_provider.c) observed; shared by the C11 provider and the C++ host test. */
#ifndef SEGARECOMP_TEST_EXACT_FRAME_OBSERVATION_H
#define SEGARECOMP_TEST_EXACT_FRAME_OBSERVATION_H
#include <segarecomp_mod_genesis.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ExactObservation {
  unsigned render_calls;
  unsigned other_phase_calls;
  unsigned frame_ptr_outside_render;   /* phases other than render that saw a non-NULL view pointer: must stay 0 */
  uint64_t last_render_frame;
  const SegaGenesisFrameView *kept;    /* a deliberately retained pointer: it must be NULL again outside the callback */
  int view_seen;
  uint32_t size, width, height, side, requested, flags, line_count, vdp_helpers_present;
  int work_ram_present;
  uint64_t ram_hash, vram_hash, lines_hash;
  const uint8_t *vram_ptr;
  int ram_read_status;
  uint32_t ram_read_value;
  int hscroll_a, hscroll_b, vscroll_a, vscroll_b, per_column, rgb_status;
  int32_t hscroll_a_value, hscroll_b_value, vscroll_a_value, vscroll_b_value;
  uint8_t rgb[3];
  int fault_next;                      /* set by the test: the next exact-frame callback makes an invalid access (a fault) */
} ExactObservation;
extern ExactObservation g_exact_obs;
extern unsigned g_exact_required_minor;
const SegaModProvider *exact_frame_provider(void **state_out);   /* the fixture's entry (segarecomp_mod_provider has C++ linkage when the SDK header is included from C++) */
#ifdef __cplusplus
}
#endif
#endif
