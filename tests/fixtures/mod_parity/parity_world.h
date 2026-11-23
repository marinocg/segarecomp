/* SEG-152-T008: the game logic of the viewer-level parity gate (tests/mod_parity_gate_abi2_test.py), shared BY TEXT by the ABI 1 twin and the ABI 2 provider so both are written to the same logic. FABRICATED, on the mid-frame
 * scenery fixture (tests/fixtures/mod_scenery/scenery_fixture_rom.py): the "game" keeps a camera word at work RAM 0x12 and scrolls plane A by -(camera * 8).
 * The authored world of plane A: cells 24..63 are the program's own plane (tile 1 / 3 alternating), every other cell is level data the program never loaded (tile 2 / 3 of palette line 1); plane B is blank.
 * The test replaces the three marks below (text edits): DECLINE (0 serve, 1 refuse the HUD with BAD_STATE, 2 refuse the whole frame as an unsupported scene), MASK_FROM (the first screen row of plane A that is
 * answered by the VDP's own frame-start word, the ABI 1 own_plane shape; 224 = none) and LEFT_DIV (the picture sits side / LEFT_DIV columns from the left: an asymmetric placement; 2 = centred). */
#ifndef PARITY_WORLD_H
#define PARITY_WORLD_H
#include <stdint.h>

#define DECLINE @@DECLINE@@
#define MASK_FROM @@MASK_FROM@@
#define LEFT_DIV @@LEFT_DIV@@

static uint16_t world_word(unsigned plane, int cell) {
  if (plane != 0U) return 0U;
  return (cell >= 24 && cell < 64) ? (uint16_t)((cell & 1) == 0 ? 1U : 3U) : (uint16_t)((cell & 1) == 0 ? 0x2002U : 0x2003U);
}
static int floor8(int v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); }
/* the VDP's own wrapped name-table word of plane A (64 x 32 at 0xC000) for plane cell (cx, cy) read from a VRAM image */
static uint16_t vdp_word(const uint8_t *vram, int cx, int cy) {
  const unsigned a = 0xC000U + (unsigned)((((cy % 32) + 32) % 32) * 64 + (((cx % 64) + 64) % 64)) * 2U;
  return (uint16_t)((vram[a] << 8) | vram[a + 1U]);
}
#endif
