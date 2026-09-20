// SEG-007-T049 checkpoint C2: focused unit coverage for
// platforms/genesis/runtime/vdp_render.c's plane/nametable addressing and bound
// scrolling behavior. All fixtures are project-authored synthetic
// VRAM/VSRAM/register byte patterns with hand-computed expected values --
// no commercial ROM content is read or referenced, and no expected value is
// computed by calling the implementation under test.

#include "vdp_render.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// A minimal synthetic register set: Mode 5 selected (reg1 bit2=1 -> reg1=
// 0x04), H40 selected (reg12 RS0=bit7, RS1=bit0, both set -> reg12=0x81),
// non-interlaced (reg12 LSM1/LSM0 = bits2-1 = 00, already satisfied by
// 0x81), Plane A base = 0xC000 (reg2 bits5-3=110b -> reg2=0x30), Plane B
// base = 0x8000 (reg4 bits2-0=100b -> reg4=0x04), plane size 64x32 (reg16
// width field=1 -> 64, height field=0 -> 32, reg16=0x01), full-screen
// H-scroll (reg11 bits1-0=00), 2-cell V-scroll (reg11 bit2=1 -> reg11=0x04),
// H-scroll table base = 0 (reg13=0).
void make_default_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  std::memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04;  // Mode 5 select (bit 2)
  regs[2] = 0x30;  // Plane A base -> (0x30<<10)&0xE000 = 0xC000
  regs[4] = 0x04;  // Plane B base -> (0x04<<13)&0xE000 = 0x8000
  regs[11] = 0x04; // full-screen H, 2-cell V
  regs[12] = 0x81; // H40 (RS0 bit7 + RS1 bit0), non-interlaced (LSM1/LSM0=0)
  regs[13] = 0x00; // H-scroll table base = 0
  regs[16] = 0x01; // width field 1 (64), height field 0 (32)
}

void test_plane_base_addresses_distinct_registers() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint32_t base_a = 0xFFFFFFFFU;
  uint32_t base_b = 0xFFFFFFFFU;
  check(genesis_vdp_plane_base_address(regs, GENESIS_VDP_PLANE_A, &base_a) == 0, "plane A base decodes");
  check(base_a == 0xC000U, "plane A base value (reg2)");
  check(genesis_vdp_plane_base_address(regs, GENESIS_VDP_PLANE_B, &base_b) == 0, "plane B base decodes");
  check(base_b == 0x8000U, "plane B base value (reg4)");
  check(base_a != base_b, "plane A/B bases distinct");
}

void test_plane_size_valid_classes() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  std::memset(regs, 0, sizeof(regs));
  unsigned w = 0, h = 0;

  regs[16] = 0x00; // 32x32
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 32 && h == 32, "plane size 32x32");

  regs[16] = 0x01; // 64x32
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 64 && h == 32, "plane size 64x32");

  regs[16] = 0x03; // 128x32
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 128 && h == 32, "plane size 128x32");

  regs[16] = 0x10; // height field 1 -> 32x64
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 32 && h == 64, "plane size 32x64");

  regs[16] = 0x11; // 64x64
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 64 && h == 64, "plane size 64x64");

  regs[16] = 0x30; // height field 3 -> 32x128
  check(genesis_vdp_decode_plane_size(regs, &w, &h) == 0 && w == 32 && h == 128, "plane size 32x128");
}

void test_plane_size_reserved_and_oversized_rejected() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  std::memset(regs, 0, sizeof(regs));
  unsigned w = 0, h = 0;

  regs[16] = 0x02; // width field reserved (2)
  check(genesis_vdp_decode_plane_size(regs, &w, &h) != 0, "reserved width field rejected");

  regs[16] = 0x20; // height field reserved (2)
  check(genesis_vdp_decode_plane_size(regs, &w, &h) != 0, "reserved height field rejected");

  regs[16] = 0x31; // width=64,height=3(128) -> 64*128=8192 > 4096 cells
  check(genesis_vdp_decode_plane_size(regs, &w, &h) != 0, "oversized 64x128 combo rejected");

  regs[16] = 0x13; // width=128,height=1(64) -> 128*64=8192 > 4096 cells
  check(genesis_vdp_decode_plane_size(regs, &w, &h) != 0, "oversized 128x64 combo rejected");
}

void test_nametable_word_decode_fields() {
  // priority=1, palette=2 (10b), v_flip=1, h_flip=0, tile=0x123
  uint16_t raw = (uint16_t)(0x8000U | (0x02U << 13) | (0x1U << 12) | 0x123U);
  GenesisVdpNametableEntry entry = genesis_vdp_decode_nametable_word(raw);
  check(entry.priority == 1, "nametable priority bit");
  check(entry.palette == 2, "nametable palette field");
  check(entry.v_flip == 1, "nametable v_flip bit");
  check(entry.h_flip == 0, "nametable h_flip bit");
  check(entry.tile_index == 0x123, "nametable tile index");
}

void test_zero_scroll_origin_maps_to_first_cell() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  // Plane A base 0xC000, cell (0,0) entry: tile index 7.
  vram[0xC000] = 0x00;
  vram[0xC001] = 0x07;

  GenesisVdpPlanePixelLocation loc;
  int rc = genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc);
  check(rc == 0, "resolve_plane_pixel zero-scroll succeeds");
  check(loc.cell_col == 0 && loc.cell_row == 0, "zero-scroll origin cell");
  check(loc.tile_x == 0 && loc.tile_y == 0, "zero-scroll origin tile-local position");
  check(loc.entry.tile_index == 7, "zero-scroll origin entry tile index");
}

void set_hscroll_word(uint8_t *vram, uint32_t table_base, unsigned plane_offset, int16_t value) {
  uint16_t raw = (uint16_t)value;
  vram[table_base + plane_offset] = (uint8_t)(raw >> 8);
  vram[table_base + plane_offset + 1] = (uint8_t)(raw & 0xFF);
}

void set_vscroll_word(uint8_t *vsram, unsigned group, unsigned plane_offset, int16_t value) {
  uint16_t raw = (uint16_t)value;
  unsigned off = group * 4 + plane_offset;
  vsram[off] = (uint8_t)(raw >> 8);
  vsram[off + 1] = (uint8_t)(raw & 0xFF);
}

void test_hscroll_tile_boundary_and_wrap() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs); // plane A width 64 cells = 512px
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_hscroll_word(vram, 0, 0, 8); // Plane A hscroll = 8 (crosses one tile boundary)
  // plane_x = screen_x - hscroll; screen_x=8 -> plane_x=0 -> cell 0.
  vram[0xC000] = 0x00; vram[0xC001] = 0x01; // cell(0,0) tile=1

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 8, 0, &loc) == 0,
        "hscroll tile-boundary resolve succeeds");
  check(loc.cell_col == 0 && loc.tile_x == 0, "hscroll=8 screen_x=8 maps to plane_x=0");
  check(loc.entry.tile_index == 1, "hscroll tile boundary entry");

  // Wrap boundary: hscroll = -8 (i.e. subtract negative -> add 8); screen_x=0
  // -> plane_x = 0 - (-8) = 8 -> still within plane, not a wrap case. Use a
  // genuine wrap: hscroll = 8, screen_x = 0 -> plane_x = 0-8 = -8 -> wraps to
  // 512-8=504 -> cell_col = 504/8 = 63.
  set_hscroll_word(vram, 0, 0, 8);
  uint32_t wrap_cell_addr = 0xC000 + (63U * 2U);
  vram[wrap_cell_addr] = 0x00; vram[wrap_cell_addr + 1] = 0x2A; // tile=0x2A
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) == 0,
        "hscroll wrap resolve succeeds");
  check(loc.cell_col == 63, "hscroll negative-wrap cell_col");
  check(loc.tile_x == 504 % 8, "hscroll negative-wrap tile_x");
  check(loc.entry.tile_index == 0x2A, "hscroll negative-wrap entry");
}

void test_negative_signed_hscroll() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_hscroll_word(vram, 0, 0, -16); // Plane A hscroll = -16
  // plane_x = screen_x - (-16) = screen_x + 16; screen_x=0 -> plane_x=16 -> cell 2.
  uint32_t addr = 0xC000 + (2U * 2U);
  vram[addr] = 0x00; vram[addr + 1] = 0x55;

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) == 0,
        "negative hscroll resolve succeeds");
  check(loc.cell_col == 2 && loc.tile_x == 0, "negative hscroll maps forward correctly");
  check(loc.entry.tile_index == 0x55, "negative hscroll entry");
}

void test_plane_a_and_b_use_own_hscroll_entries() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_hscroll_word(vram, 0, 0, 0);  // Plane A hscroll = 0
  set_hscroll_word(vram, 0, 2, 8);  // Plane B hscroll = 8

  // Plane A base 0xC000 cell(0,0), Plane B base 0x8000 cell(0,0).
  vram[0xC000] = 0x00; vram[0xC001] = 0x11;
  vram[0x8000] = 0x00; vram[0x8001] = 0x22;

  GenesisVdpPlanePixelLocation loc_a;
  GenesisVdpPlanePixelLocation loc_b;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc_a) == 0,
        "plane A resolve succeeds");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_B, 8, 0, &loc_b) == 0,
        "plane B resolve succeeds");
  check(loc_a.cell_col == 0 && loc_a.entry.tile_index == 0x11, "plane A uses its own hscroll (0)");
  check(loc_b.cell_col == 0 && loc_b.entry.tile_index == 0x22, "plane B uses its own hscroll (8)");
}

// SEG-007-T050: per-scanline ("line") horizontal-scroll mode (register #11
// bits 1-0 == 11). Two different scanlines get two different, independently
// hand-picked Plane A hscroll values; each screen_y must resolve using its
// OWN row (table_base + screen_y*4), never row 0 or any other row.
void test_line_hscroll_mode_distinct_rows_per_scanline() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs); // plane A width 64 cells = 512px
  regs[11] = 0x07; // H-scroll mode bits = 11 (line), V-scroll bit2 = 1 (2-cell)
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  check(genesis_vdp_hscroll_mode_is_line(regs), "reg11=0x07 selects line hscroll mode");
  check(!genesis_vdp_hscroll_mode_is_fullscreen(regs), "reg11=0x07 is not fullscreen hscroll mode");

  // Row 0 (screen_y=0): Plane A hscroll = 0 -> screen_x=0 maps to plane_x=0
  // -> cell 0.
  set_hscroll_word(vram, 0 * 4, 0, 0);
  vram[0xC000] = 0x00; vram[0xC001] = 0x10; // cell(0,0) tile=0x10

  // Row 5 (screen_y=5): Plane A hscroll = 8 -> screen_x=0 maps to
  // plane_x = 0-8 = -8 -> wraps to 512-8=504 -> cell_col=63.
  set_hscroll_word(vram, 5 * 4, 0, 8);
  uint32_t row5_wrap_addr = 0xC000 + (63U * 2U);
  vram[row5_wrap_addr] = 0x00; vram[row5_wrap_addr + 1] = 0x20; // tile=0x20

  GenesisVdpPlanePixelLocation loc_row0;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc_row0) == 0,
        "line-mode row 0 resolve succeeds");
  check(loc_row0.cell_col == 0, "line-mode row 0 uses its own (zero) hscroll");
  check(loc_row0.entry.tile_index == 0x10, "line-mode row 0 entry");

  GenesisVdpPlanePixelLocation loc_row5;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 5, &loc_row5) == 0,
        "line-mode row 5 resolve succeeds");
  check(loc_row5.cell_col == 63, "line-mode row 5 uses its OWN (nonzero) hscroll, wrapping");
  check(loc_row5.entry.tile_index == 0x20, "line-mode row 5 entry");
  check(loc_row0.entry.tile_index != loc_row5.entry.tile_index,
        "line-mode rows 0 and 5 are genuinely independent (not aliased to the same row)");
}

// SEG-007-T050: Plane A and Plane B each read their OWN word within the
// SAME per-scanline row (offset +0 vs +2), matching the full-screen case's
// existing per-plane addressing, just row-indexed by screen_y now.
void test_line_hscroll_mode_plane_a_and_b_distinct_within_row() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x07; // line H-scroll, 2-cell V-scroll
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  unsigned row = 10;
  set_hscroll_word(vram, row * 4, 0, 0); // Plane A hscroll = 0
  set_hscroll_word(vram, row * 4, 2, 8); // Plane B hscroll = 8

  // V-scroll is all-zero (2-cell mode, but every VSRAM word is 0), so
  // plane_y == screen_y == row here; cell_row = row / 8 (kFixturePlaneWidth-
  // independent tile height), matching genesis_vdp_resolve_plane_pixel's own
  // documented cell math -- NOT cell_row 0, since row=10 is not < 8.
  constexpr unsigned kWidthCells = 64U; // matches make_default_registers' reg16=0x01.
  unsigned cell_row = row / 8U;
  uint32_t cell_a_addr = 0xC000 + (cell_row * kWidthCells + 0U) * 2U;
  uint32_t cell_b_addr = 0x8000 + (cell_row * kWidthCells + 0U) * 2U;
  vram[cell_a_addr] = 0x00; vram[cell_a_addr + 1] = 0x31; // Plane A cell(0, cell_row)
  vram[cell_b_addr] = 0x00; vram[cell_b_addr + 1] = 0x32; // Plane B cell(0, cell_row)

  GenesisVdpPlanePixelLocation loc_a;
  GenesisVdpPlanePixelLocation loc_b;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, row, &loc_a) == 0,
        "line-mode plane A resolve succeeds");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_B, 8, row, &loc_b) == 0,
        "line-mode plane B resolve succeeds");
  check(loc_a.cell_col == 0 && loc_a.entry.tile_index == 0x31,
        "line-mode plane A uses its own row word (hscroll 0)");
  check(loc_b.cell_col == 0 && loc_b.entry.tile_index == 0x32,
        "line-mode plane B uses its own row word (hscroll 8)");
}

// SEG-007-T050 boundary: the last H40 scanline (screen_y=223) still resolves
// via its own row (table_base + 223*4), not via any wraparound to row 0.
void test_line_hscroll_mode_last_scanline_boundary() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x07;
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_hscroll_word(vram, 0 * 4, 0, 0);      // row 0: hscroll 0 (must NOT be used for screen_y=223)
  set_hscroll_word(vram, 223 * 4, 0, -16);  // row 223: hscroll -16 -> plane_x = 0-(-16) = 16 -> cell 2.

  // V-scroll is all-zero, so plane_y == screen_y == 223; cell_row = 223/8 =
  // 27 (integer division) -- NOT cell_row 0.
  constexpr unsigned kWidthCells = 64U; // matches make_default_registers' reg16=0x01.
  unsigned cell_row = 223U / 8U;
  uint32_t cell0_addr = 0xC000 + (cell_row * kWidthCells + 0U) * 2U;
  uint32_t cell2_addr = 0xC000 + (cell_row * kWidthCells + 2U) * 2U;
  vram[cell0_addr] = 0x00; vram[cell0_addr + 1] = 0x40; // cell(0,27): would be hit by row-0's hscroll
  vram[cell2_addr] = 0x00; vram[cell2_addr + 1] = 0x41; // cell(2,27): hit by row-223's hscroll=-16

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 223, &loc) == 0,
        "line-mode last scanline (223) resolve succeeds");
  check(loc.cell_col == 2, "line-mode screen_y=223 uses row 223's hscroll (-16), not row 0's (0)");
  check(loc.entry.tile_index == 0x41, "line-mode last scanline entry");
}

// SEG-007-T050 adversarial: a pathologically large screen_y combined with a
// nonzero h-scroll-table base must still fail closed (out-of-VRAM row read)
// rather than silently wrapping or reading garbage.
void test_line_hscroll_mode_oob_row_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x07;
  // Table base near the very top of VRAM so even a moderate row index
  // overflows GENESIS_VDP_VRAM_BYTES.
  regs[13] = 0x3F; // table_base = 0x3F << 10 = 0xFC00
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));

  int32_t value = 0;
  // 0xFC00 + 223*4 + 1 = 0xFC00 + 892 + 1 = 0xFF7D, still < 0x10000: succeeds.
  check(genesis_vdp_read_hscroll(vram, regs, GENESIS_VDP_PLANE_A, 223, &value) == 0,
        "line-mode row 223 at a high table base still fits (boundary sanity check)");
  // A screen_y far beyond this bound surface's 224-line frame height (a
  // malformed/untrusted caller value) must still fail closed rather than
  // reading past GENESIS_VDP_VRAM_BYTES.
  check(genesis_vdp_read_hscroll(vram, regs, GENESIS_VDP_PLANE_A, 100000, &value) != 0,
        "line-mode wildly out-of-range screen_y fails closed rather than OOB-reading VRAM");
}

// An overflowing row multiplication must not alias a valid h-scroll row.
// The renderer supports only its 224 visible scanlines in line mode, so this
// rejected call also must leave the output untouched.
void test_line_hscroll_mode_overflowing_screen_y_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x07; // line H-scroll, 2-cell V-scroll
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));

  int32_t value = 0x12345678;
  check(genesis_vdp_read_hscroll(vram, regs, GENESIS_VDP_PLANE_A, UINT32_C(0x40000000), &value) != 0,
        "line-mode overflowing screen_y fails closed");
  check(value == 0x12345678, "line-mode overflowing screen_y leaves output unmodified");
}

void test_vscroll_selects_correct_column_group() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_vscroll_word(vsram, 0, 0, 16); // group 0 (screen cell cols 0-1), Plane A vscroll=16
  set_vscroll_word(vsram, 1, 0, 24); // group 1 (screen cell cols 2-3), Plane A vscroll=24

  // screen_x=0 (plane_x=0, cell col 0, group 0): plane_y = 0+16=16 -> cell_row=2.
  uint32_t addr_g0 = 0xC000 + (2U * 64U + 0U) * 2U;
  vram[addr_g0] = 0x00; vram[addr_g0 + 1] = 0x33;
  // screen_x=16 (plane_x=16, cell col 2, group 1): plane_y = 0+24=24 -> cell_row=3.
  uint32_t addr_g1 = 0xC000 + (3U * 64U + 2U) * 2U;
  vram[addr_g1] = 0x00; vram[addr_g1 + 1] = 0x44;

  GenesisVdpPlanePixelLocation loc0;
  GenesisVdpPlanePixelLocation loc1;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc0) == 0,
        "vscroll group0 resolve succeeds");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 16, 0, &loc1) == 0,
        "vscroll group1 resolve succeeds");
  check(loc0.cell_row == 2 && loc0.entry.tile_index == 0x33, "vscroll group0 correct row/entry");
  check(loc1.cell_row == 3 && loc1.entry.tile_index == 0x44, "vscroll group1 correct row/entry");
}

void test_plane_a_and_b_vscroll_distinct() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  set_vscroll_word(vsram, 0, 0, 5);  // Plane A group 0
  set_vscroll_word(vsram, 0, 2, 9);  // Plane B group 0

  int32_t va = 0, vb = 0;
  check(genesis_vdp_read_vscroll(vsram, regs, GENESIS_VDP_PLANE_A, 0, &va) == 0, "read plane A vscroll");
  check(genesis_vdp_read_vscroll(vsram, regs, GENESIS_VDP_PLANE_B, 0, &vb) == 0, "read plane B vscroll");
  check(va == 5, "plane A vscroll value");
  check(vb == 9, "plane B vscroll value");
  check(va != vb, "plane A/B vscroll distinct");
}

void test_full_plane_vscroll_a_b_signed_and_wrap() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x00; // full-screen H and full-plane V
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  set_vscroll_word(vsram, 0, 0, -8);
  set_vscroll_word(vsram, 0, 2, 8);
  int32_t a = 0, b = 0;
  check(genesis_vdp_read_vscroll(vsram, regs, GENESIS_VDP_PLANE_A, 0, &a) == 0,
        "full-plane A reads offset zero");
  check(genesis_vdp_read_vscroll(vsram, regs, GENESIS_VDP_PLANE_B, 39, &b) == 0,
        "full-plane B ignores column and reads offset two");
  check(a == -8 && b == 8, "full-plane A/B signed values are independent");
  check(genesis_vdp_plane_y(0, a, 256) == 248, "negative full-plane vscroll wraps");
  check(genesis_vdp_plane_y(255, b, 256) == 7, "positive full-plane vscroll wraps");
}

void test_full_plane_vscroll_interacts_with_line_hscroll() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x03; // line H, full-plane V
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  std::memset(vsram, 0, sizeof(vsram));
  set_hscroll_word(vram, 8U * 4U, 0, 8);
  set_vscroll_word(vsram, 0, 0, 8);
  uint32_t addr = 0xC000 + (2U * 64U) * 2U;
  vram[addr] = 0x00; vram[addr + 1U] = 0x5A;
  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 8, 8, &loc) == 0,
        "line hscroll and full-plane vscroll compose");
  check(loc.cell_col == 0 && loc.cell_row == 2 && loc.entry.tile_index == 0x5A,
        "line hscroll/full-plane vscroll expected location");
}

void test_vscroll_tile_and_wrap_boundary() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs); // height 32 cells = 256px
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_vscroll_word(vsram, 0, 0, 8); // Plane A vscroll = 8 -> crosses one tile boundary
  // screen_y=0 -> plane_y=8 -> cell_row=1.
  uint32_t addr1 = 0xC000 + (1U * 64U * 2U);
  vram[addr1] = 0x00; vram[addr1 + 1] = 0x66;

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) == 0,
        "vscroll tile boundary resolve succeeds");
  check(loc.cell_row == 1 && loc.tile_y == 0, "vscroll tile boundary cell/tile_y");
  check(loc.entry.tile_index == 0x66, "vscroll tile boundary entry");

  // Wrap: vscroll=8, screen_y = 255 (last visible row) -> plane_y = 263 -> wraps to 263-256=7 -> cell_row=0, tile_y=7.
  uint32_t addr0 = 0xC000; // cell_row 0
  vram[addr0] = 0x00; vram[addr0 + 1] = 0x77;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 255, &loc) == 0,
        "vscroll plane-height wrap resolve succeeds");
  check(loc.cell_row == 0 && loc.tile_y == 7, "vscroll plane-height wrap cell/tile_y");
  check(loc.entry.tile_index == 0x77, "vscroll plane-height wrap entry");
}

void test_combined_hv_scroll() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  set_hscroll_word(vram, 0, 0, 3);  // Plane A hscroll = 3
  set_vscroll_word(vsram, 0, 0, 5); // Plane A vscroll = 5 (screen cell col 0 -> group 0)

  // screen (10, 10): plane_x = 10-3=7 -> cell_col 0, tile_x 7.
  // plane_y = 10+5=15 -> cell_row 1, tile_y 7.
  uint32_t addr = 0xC000 + (1U * 64U * 2U);
  vram[addr] = 0x00; vram[addr + 1] = 0x99;

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 10, 10, &loc) == 0,
        "combined h+v resolve succeeds");
  check(loc.cell_col == 0 && loc.tile_x == 7, "combined h+v cell_col/tile_x");
  check(loc.cell_row == 1 && loc.tile_y == 7, "combined h+v cell_row/tile_y");
  check(loc.entry.tile_index == 0x99, "combined h+v entry");
}

void test_h40_boundary_screen_coordinates_no_oob() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) == 0,
        "boundary x=0,y=0 resolves without OOB");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 319, 0, &loc) == 0,
        "boundary x=319 resolves without OOB");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 223, &loc) == 0,
        "boundary y=223 (representative y=223 visible-row boundary) resolves without OOB");
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_B, 319, 223, &loc) == 0,
        "boundary x=319,y=223 plane B resolves without OOB");
}

void test_unsupported_hscroll_mode_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  // H-scroll mode bits = 10 ("scroll every tile" / per-8-line, out of scope
  // -- see vdp_render.h's SEG-007-T050 "line" horizontal-scroll-mode
  // citation for the full 00/01/10/11 mode table), still V=0 (full, not
  // 2-cell).
  regs[11] = 0x02;
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  int32_t value = 0;
  check(genesis_vdp_read_hscroll(vram, regs, GENESIS_VDP_PLANE_A, 0, &value) != 0,
        "unsupported hscroll mode (every-tile) fails closed");

  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) != 0,
        "resolve_plane_pixel fails closed for unsupported (every-tile) hscroll mode");
}

// SEG-007-T050: the other non-implemented non-full-screen horizontal-scroll
// encoding (`01`, "scroll eight lines, then repeat") must also still fail
// closed -- only `00` (full) and `11` (line) are implemented.
void test_unsupported_hscroll_mode_eight_line_repeat_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x01; // H-scroll mode bits = 01 ("scroll eight lines, then repeat").
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  int32_t value = 0;
  check(genesis_vdp_read_hscroll(vram, regs, GENESIS_VDP_PLANE_A, 0, &value) != 0,
        "unsupported hscroll mode (eight-line-repeat) fails closed");
  check(!genesis_vdp_hscroll_mode_is_fullscreen(regs), "mode 01 is not fullscreen");
  check(!genesis_vdp_hscroll_mode_is_line(regs), "mode 01 is not line mode");

  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) != 0,
        "resolve_plane_pixel fails closed for unsupported (eight-line-repeat) hscroll mode");
}

void test_full_plane_vscroll_mode_is_supported() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[11] = 0x00; // full-screen H, full-plane V
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  int32_t value = 0;
  check(genesis_vdp_read_vscroll(vsram, regs, GENESIS_VDP_PLANE_A, 0, &value) == 0,
        "full-plane vscroll mode succeeds");
}

void test_reserved_plane_size_encoding_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[16] = 0x02; // reserved width field
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));
  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) != 0,
        "reserved plane-size encoding does not silently produce output");
}

// Fills a GenesisVdpPlanePixelLocation with a recognizable sentinel pattern
// so a rejected call's "output unmodified" claim can be verified byte-for-
// byte rather than merely by checking a return code.
void fill_location_sentinel(GenesisVdpPlanePixelLocation *loc) {
  std::memset(loc, 0xAB, sizeof(*loc));
}

bool location_is_sentinel(const GenesisVdpPlanePixelLocation &loc) {
  GenesisVdpPlanePixelLocation sentinel;
  fill_location_sentinel(&sentinel);
  return std::memcmp(&loc, &sentinel, sizeof(loc)) == 0;
}

void test_valid_mode5_h40_noninterlace_succeeds() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  GenesisVdpPlanePixelLocation loc;
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) == 0,
        "valid Mode5+H40+non-interlace resolves");
}

void test_h32_selected_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[12] = 0x00; // RS0=RS1=0 -> H32
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  GenesisVdpPlanePixelLocation loc;
  fill_location_sentinel(&loc);
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) != 0,
        "H32 selected fails closed");
  check(location_is_sentinel(loc), "H32 rejection leaves output unmodified");
}

void test_mode4_selected_fails_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[1] = 0x00; // Mode 5 bit clear -> Mode 4 (or earlier)
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  GenesisVdpPlanePixelLocation loc;
  fill_location_sentinel(&loc);
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc) != 0,
        "Mode 5 disabled fails closed");
  check(location_is_sentinel(loc), "Mode 5 disabled rejection leaves output unmodified");
}

void test_interlace_modes_fail_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  std::memset(vsram, 0, sizeof(vsram));

  // Interlace mode 1: LSM1=0, LSM0=1 (bit2=0, bit1=1).
  make_default_registers(regs);
  regs[12] = (uint16_t)(regs[12] | 0x02U);
  GenesisVdpPlanePixelLocation loc1;
  fill_location_sentinel(&loc1);
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc1) != 0,
        "interlace mode 1 fails closed");
  check(location_is_sentinel(loc1), "interlace mode 1 rejection leaves output unmodified");

  // Interlace mode 2: LSM1=1, LSM0=1 (bit2=1, bit1=1).
  make_default_registers(regs);
  regs[12] = (uint16_t)(regs[12] | 0x06U);
  GenesisVdpPlanePixelLocation loc2;
  fill_location_sentinel(&loc2);
  check(genesis_vdp_resolve_plane_pixel(vram, vsram, regs, GENESIS_VDP_PLANE_A, 0, 0, &loc2) != 0,
        "interlace mode 2 fails closed");
  check(location_is_sentinel(loc2), "interlace mode 2 rejection leaves output unmodified");
}

void test_invalid_plane_selector_hscroll_vscroll_fail_closed() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  std::memset(vsram, 0, sizeof(vsram));

  static_assert(sizeof(GenesisVdpPlaneSelector) == sizeof(unsigned),
                "plane selector is an int-sized enum");
  const unsigned bogus_plane_raw = 2U;  // deliberately outside the enumerator range
  GenesisVdpPlaneSelector bogus_plane;
  std::memcpy(&bogus_plane, &bogus_plane_raw, sizeof(bogus_plane));

  int32_t hscroll_value = 0x11223344;
  int32_t hscroll_sentinel = hscroll_value;
  check(genesis_vdp_read_hscroll(vram, regs, bogus_plane, 0, &hscroll_value) != 0,
        "invalid plane selector rejected by genesis_vdp_read_hscroll");
  check(hscroll_value == hscroll_sentinel, "genesis_vdp_read_hscroll leaves output unmodified");

  int32_t vscroll_value = 0x55667788;
  int32_t vscroll_sentinel = vscroll_value;
  check(genesis_vdp_read_vscroll(vsram, regs, bogus_plane, 0, &vscroll_value) != 0,
        "invalid plane selector rejected by genesis_vdp_read_vscroll");
  check(vscroll_value == vscroll_sentinel, "genesis_vdp_read_vscroll leaves output unmodified");

  // genesis_vdp_plane_base_address was already documented/implemented to
  // reject any non-A/B selector; confirm it still does (no aliasing bug
  // found there, but this locks the already-correct behavior in place).
  uint32_t base_addr_value = 0xDEADBEEFU;
  check(genesis_vdp_plane_base_address(regs, bogus_plane, &base_addr_value) != 0,
        "invalid plane selector rejected by genesis_vdp_plane_base_address");
  check(base_addr_value == 0xDEADBEEFU,
        "genesis_vdp_plane_base_address leaves output unmodified for invalid selector");
}

} // namespace

int main() {
  test_plane_base_addresses_distinct_registers();
  test_plane_size_valid_classes();
  test_plane_size_reserved_and_oversized_rejected();
  test_nametable_word_decode_fields();
  test_zero_scroll_origin_maps_to_first_cell();
  test_hscroll_tile_boundary_and_wrap();
  test_negative_signed_hscroll();
  test_plane_a_and_b_use_own_hscroll_entries();
  test_vscroll_selects_correct_column_group();
  test_plane_a_and_b_vscroll_distinct();
  test_full_plane_vscroll_a_b_signed_and_wrap();
  test_full_plane_vscroll_interacts_with_line_hscroll();
  test_vscroll_tile_and_wrap_boundary();
  test_combined_hv_scroll();
  test_h40_boundary_screen_coordinates_no_oob();
  test_unsupported_hscroll_mode_fails_closed();
  test_unsupported_hscroll_mode_eight_line_repeat_fails_closed();
  test_line_hscroll_mode_distinct_rows_per_scanline();
  test_line_hscroll_mode_plane_a_and_b_distinct_within_row();
  test_line_hscroll_mode_last_scanline_boundary();
  test_line_hscroll_mode_oob_row_fails_closed();
  test_line_hscroll_mode_overflowing_screen_y_fails_closed();
  test_full_plane_vscroll_mode_is_supported();
  test_reserved_plane_size_encoding_fails_closed();
  test_valid_mode5_h40_noninterlace_succeeds();
  test_h32_selected_fails_closed();
  test_mode4_selected_fails_closed();
  test_interlace_modes_fail_closed();
  test_invalid_plane_selector_hscroll_vscroll_fail_closed();

  if (failures == 0) {
    std::printf("OK: all genesis_vdp_render_plane_scroll checks passed\n");
    return 0;
  }
  std::printf("FAILED: %d check(s)\n", failures);
  return 1;
}
