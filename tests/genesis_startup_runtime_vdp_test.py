#!/usr/bin/env python3
"""SEG-007-T081/SEG-007-T091: direct, isolated coverage of
genesis_route_access's VDP register-area device-routing extension.

This proves the runtime routing function alone -- no compiled/executed
generated program is involved. It calls `genesis_route_access` directly for:

- The VDP CONTROL-port base-address WORD-read selector (SEG-007-T081; see
  the VDP addendum to
  docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md),
  confirming the routed value matches the current, zero-initialized
  `GenesisVdpState.status_register` field.
- The VDP CONTROL-port base-address WORD-write command-word protocol
  (SEG-007-T091, same addendum): the one-word register-set command and the
   non-DMA two-word VRAM/CRAM/VSRAM address-set command, confirming each
   reaches the correct literal `registers[]`/`auto_increment_value`/
  `control_port_awaiting_second_word`/`control_port_first_word`/
  `addressed_pointer` state.
- The VDP CONTROL-port base-address LONG-write command-word protocol
  (SEG-007-T091, second frontier pass, same addendum): a LONG write is
  decomposed into two sequential WORD writes (D31-D16 first) through the
  identical command-word state machine, per GTO1 p. 20. This covers a LONG
  write completing two chained register-set commands, a LONG write whose
  high/low halves are a two-word address-set sequence's first/second words,
  a LONG write whose high word alone is invalid (rejected before any
  mutation, matching the general "on failure neither it nor the runtime is
  modified" contract), and a LONG write whose high word succeeds but whose
  low word fails -- the one deliberate, explicitly documented, narrow
  exception to that general contract (see the block comment above
  `genesis_vdp_access`): the high word's own already-successful mutation is
  left durably committed even though the overall LONG write reports
  failure, because GTO1 documents the LONG write as two independent
  sequential bus transactions, not one atomic operation.

- The plain (non-armed-fill, non-DMA) CPU DATA-port ($C00000) WRITE
  (SEG-007-T108; see docs/references/genesis-vdp-data-port-cpu-write-contract.md):
  a completed non-DMA two-word address-set command records its CD5-CD0 code in
  `GenesisVdpState.data_port_transfer_code`; a subsequent LONG or WORD write to
  $C00000 stores big-endian halfwords into the selected target -- CRAM (code
  0x03) or VSRAM (code 0x05) -- and advances `addressed_pointer` by the
  register-15 auto-increment, wrapping modulo the target's documented byte size
  (128 for CRAM, 80 for VSRAM). Adversarial negatives (each FAIL + `devices.vdp`
  byte-identical before mutation): BYTE width, DATA-port read, wrong address,
  no code selected, a READ code selected, the out-of-scope VRAM WRITE code, an
  armed memory-to-VRAM DMA, and an odd current VDP address.

- SEG-007-T191 generalizes the plain CPU DATA-port WRITE surface: the VRAM
  WRITE target (CD5-CD0 code 0x01) is now routed alongside CRAM/VSRAM, all of
  BYTE/WORD/LONG widths are covered (BYTE mirrored into both halves), VRAM at
  an odd current address applies GTO1's documented high/low byte exchange
  (CRAM/VSRAM odd stays fail-closed), and a data-port write with no completed
  or an incomplete two-word control-port command is deterministic fail-closed.

It also confirms every other address, width, and direction in the
recognized VDP register-area window remains unconditionally fail-closed
with `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP` -- including the DATA
port, HV COUNTER, and PSG 76489 sub-offsets `GTO1` p. 10's own "VDP AREA"
diagram names, every wrong-width write form (BYTE remains fail-closed at
   every width; LONG is now implemented per the above), unsupported DMA
   modes, and an undocumented CD5-CD0 code. It also
re-confirms the pre-existing controller-I/O CTRL1/CTRL2/CTRL3/Version
selectors and their own neighboring fail-closed cases are entirely
unaffected by this addition, mirroring
`tests/genesis_startup_runtime_controller_io_test.py`'s own regression
discipline.
"""
import pathlib
import subprocess
import sys
import tempfile


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"

int main(void) {
  GenesisRuntime runtime = {0};
  const GenesisRuntime zeroed = {0};
  GenesisRuntimeStop stop = {0};
  static const uint8_t dma_source[] = { UINT8_C(0x12), UINT8_C(0x34), UINT8_C(0x56), UINT8_C(0x78) };
  GenesisOwnedCartridgeRegion dma_region = {
      UINT32_C(0x00000100), UINT32_C(0x00000104), dma_source, UINT32_C(4) };
  uint32_t value;

  /* The exact SEG-007-T081 VDP CONTROL-port base-address WORD-read
     selector: routed to the current (zero-initialized) status_register
     field value. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);
  assert(runtime.devices.vdp.status_register == 0U);

  /* Pre-existing controller-I/O selectors (SEG-007-T020/T038/T079) are
     entirely unaffected by this addition. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10008), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10001), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == UINT32_C(0xA0));

  /* The DATA port ($C00000), a neighboring VDP-area sub-offset per GTO1
     p. 10's own named diagram, remains unconditionally fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP &&
         value == UINT32_C(0xFFFFFFFF));

  /* The HV COUNTER ($C00008), a neighboring VDP-area sub-offset, remains
     unconditionally fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00008), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP &&
         value == UINT32_C(0xFFFFFFFF));

  /* $C00012 (a neighboring VDP-area sub-offset -- NOT the PSG port, which is
     the odd byte $C00011 handled by its own SEG-007-T109 lane) remains
     unconditionally fail-closed in this VDP lane. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00012), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP &&
         value == UINT32_C(0xFFFFFFFF));

  /* A BYTE read at the exact CONTROL-port selector address remains
     fail-closed: the selector is WORD-only. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);

  /* A LONG read starting at the CONTROL-port selector address remains
     fail-closed: the selector is WORD-only. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);

  /* The second CONTROL-port mirror lane ($C00006, per MCD1) is outside this
     project's own recognized 0x20-byte VDP-area window's one recognized
     selector shape and remains fail-closed within the recognized window. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00006), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);

  /* An address just outside the recognized VDP-area window (SEG-007-T080's
     own settled outer boundary is at least this wide) falls through to the
     pre-existing generic unmapped-region fail-close, exactly as before this
     task. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00020), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION &&
         stop.diagnostic_category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS &&
         value == UINT32_C(0xFFFFFFFF));

  /* SEG-007-T091: a BYTE write to the exact CONTROL-port selector address
     remains fail-closed -- only WORD and (SEG-007-T091 second frontier pass)
     LONG-width writes are implemented. Mutates no GenesisVdpState field. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);

  /* SEG-007-T091 (second frontier pass), negative: a LONG write whose high
     (D31-D16) word alone is invalid -- register #24, one past the
     documented valid #0-#23 range, still representable by the 5-bit RS4-RS0
     field: high word = 1001 1000 0000 0000 = 0x9800 (register #24, data
     0x00); low word is irrelevant (0x0000) because
     genesis_vdp_control_port_write_word's own register-set path rejects the
     high word before mutating anything, so genesis_vdp_access's LONG case
     never even calls the helper for the low word. The whole LONG write
     therefore fails closed exactly like every other rejected access in this
     file, mutating NO GenesisVdpState field at all -- this is the ordinary
     (non-exceptional) LONG-write failure case, fully honoring the general
     "on failure neither it nor the runtime is modified" contract. */
  {
    GenesisVdpState before = runtime.devices.vdp;
    value = UINT32_C(0x98000000);
    assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
           stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    assert(memcmp(&runtime.devices.vdp, &before, sizeof(before)) == 0);
  }

  /* A WORD write to the second CONTROL-port mirror lane ($C00006) and a
     WORD write to the DATA port ($C00000) both remain fail-closed: this
     task's own WRITE selectors are narrowed to exactly $C00004, mirroring
     SEG-007-T081's own READ-side narrowing. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00006), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);

  /* Nothing so far -- the two routed READ successes, every neighboring
     fail-closed READ, and every fail-closed WRITE-shape rejection above --
     left all unrelated state untouched.  The one successful VDP status read
     is now allowed to change exactly its routed interrupt observation count;
     zero status must not assert pending, create a transition, or affect the
     checkpoint snapshot. */
  {
    GenesisRuntime expected = zeroed;
    expected.devices.interrupt.vblank_status_read_count = 1U;
    assert(memcmp(&runtime, &expected, sizeof(runtime)) == 0);
  }

  /* --- SEG-007-T091 (a): one-word register-set command WRITEs. ---
     Word format (GTO1 p. 20 "WRITE1: REGISTER SET"): bits 15-13 = "100",
     RS4-RS0 = bits 12-8 (register number), D7-D0 = bits 7-0 (data byte). */

  /* Register #0, data 0x04: word = 1000 0000 0000 0100 = 0x8004. */
  value = UINT32_C(0x8004);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.registers[0] == UINT32_C(0x0004));

  /* Register #23 (GENESIS_VDP_REGISTER_COUNT - 1, the documented top of the
     valid write-register range), data 0xFF: word = 1001 0111 1111 1111 =
     0x97FF. */
  value = UINT32_C(0x97FF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   assert(runtime.devices.vdp.registers[23] == UINT32_C(0x00FF));

  /* Register #15 (the documented VRAM/CRAM/VSRAM address auto-increment
     register, GTO1 p. 28/p. 37), data 0x02: word = 1000 1111 0000 0010 =
     0x8F02. This must also update auto_increment_value (a named post-access
     side effect; see the block comment above genesis_vdp_access). */
  value = UINT32_C(0x8F02);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.registers[15] == UINT32_C(0x0002));
  assert(runtime.devices.vdp.auto_increment_value == UINT32_C(0x0002));

  /* Register #24 -- one past the documented valid range (#0-#23) but still
     representable by the 5-bit RS4-RS0 field -- remains fail-closed, and
     mutates no GenesisVdpState field: word = 1001 1000 0000 0000 = 0x9800. */
  {
    GenesisVdpState before = runtime.devices.vdp;
    value = UINT32_C(0x9800);
    assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
           stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    assert(memcmp(&runtime.devices.vdp, &before, sizeof(before)) == 0);
  }

  /* --- SEG-007-T091 (b): non-DMA two-word VRAM/CRAM/VSRAM address-set
     command WRITEs. --- Word format (GTO1 p. 20 "WRITE2: ADDRESS SET"):
     1st word bits 15-14 = CD1,CD0; bits 13-0 = A13-A0. 2nd word bits 15-8
     fixed 0; bits 7-4 = CD5-CD2; bits 3-2 fixed 0; bits 1-0 = A15,A14. */

  /* VRAM WRITE (CD5..CD0 = 000001) at address 0x1234: 1st word
     = 0100 0001 0010 0011 0100 (CD1=0,CD0=1,A13-A0=0x1234) = 0x5234;
     2nd word = 0x0000 (CD5-CD2=0000, A15A14=00, since 0x1234 < 0x4000). */
  value = UINT32_C(0x5234);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 1U);
  assert(runtime.devices.vdp.control_port_first_word == UINT32_C(0x5234));
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);
  assert(runtime.devices.vdp.control_port_first_word == 0U);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x1234));

  /* CRAM WRITE (CD5..CD0 = 000011) at address 0x0056: 1st word
     = 1100 0000 0000 0000 0101 0110 (CD1=1,CD0=1,A13-A0=0x0056) = 0xC056;
     2nd word = 0x0000 (CD5-CD2=0000, A15A14=00). */
  value = UINT32_C(0xC056);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0056));

   /* SEG-007-T084: selected memory-to-VRAM DMA. GTO1 §7 documents register
      #1 bit 4 as DMA enable; #19/#20 as word length; #21--#23 as source;
      #23 mode 00 as 68000-memory source; and CD5..CD0=0x21 as VRAM-write
      DMA. The synthetic immutable source uses the existing owned-cartridge
      route, never a ROM fixture. */
   runtime.owned_regions = &dma_region;
   runtime.owned_region_count = 1U;
   value = UINT32_C(0x8110); /* register #1: DMA enable */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9302); /* register #19: two words */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400); /* register #20 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9580); /* register #21: source byte address 0x100 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9600); /* register #22 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9700); /* register #23: memory-to-VDP mode */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x6004); /* VRAM destination 0x2004, CD1/CD0 = 01 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0080);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   /* SEG-007-T175: real Genesis hardware stops the 68000 from executing any
      further instruction while a memory-to-VDP DMA transfer owns the bus, so
      this arming CONTROL-port write is now itself the synchronous
      suspension/progression event -- a routable DMA source (this test's own
      owned-cartridge region) drains fully to GENESIS_VDP_DMA_IDLE as a
      device-side effect of this exact write, before it returns, rather than
      staying BUSY and progressing one word per later status read. */
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);
   assert(runtime.devices.vdp.dma.source_address == UINT32_C(0x00000104));
   assert(runtime.devices.vdp.dma.remaining_length == 0U);
   assert(runtime.devices.vdp.dma.transfer_access_count == 2U);
   assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x2008));
   assert(runtime.devices.vdp.vram[UINT32_C(0x2004)] == UINT8_C(0x12) &&
          runtime.devices.vdp.vram[UINT32_C(0x2005)] == UINT8_C(0x34) &&
          runtime.devices.vdp.vram[UINT32_C(0x2006)] == UINT8_C(0x56) &&
          runtime.devices.vdp.vram[UINT32_C(0x2007)] == UINT8_C(0x78));
   /* A status-port read after the DMA has already completed is an ordinary
      no-op read: the pre-existing status-read-triggered progression path
      (genesis_vdp_progress_dma's own phase guard) is a documented no-op once
      the DMA is already IDLE, exactly as it always was before any DMA was
      armed at all. */
   value = UINT32_C(0xFFFFFFFF);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
   assert(value == 0U);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);
   assert(runtime.devices.vdp.dma.remaining_length == 0U);
   assert(runtime.devices.vdp.dma.transfer_access_count == 2U);

   /* Memory-to-VDP DMA's register #23 bit 6 is source A23, not a mode bit.
      This reaches the documented 68000 work-RAM source through the existing
      routed work-RAM owner: source word address 0x7F8000 encodes $FF0000. */
   runtime.work_ram[0] = UINT8_C(0xBE);
   runtime.work_ram[1] = UINT8_C(0xEF);
   value = UINT32_C(0x9301); /* one word */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9500);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9680);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x977F);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x6020);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0080);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   /* SEG-007-T175: the single-word transfer already completed synchronously
      as a device-side effect of the arming write above -- source_address has
      already advanced past its own armed value and the DMA is already IDLE,
      before this arming write itself returns. */
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);
   assert(runtime.devices.vdp.dma.source_address == UINT32_C(0x00FF0002));
   assert(runtime.devices.vdp.vram[UINT32_C(0x2020)] == UINT8_C(0xBE) &&
          runtime.devices.vdp.vram[UINT32_C(0x2021)] == UINT8_C(0xEF));

   /* SEG-007-T169: selected memory-to-CRAM DMA. The identical #1/#19/#20/
      #21-23 memory-to-VDP register protocol as the memory-to-VRAM case
      above; only the second command word's low bits select the CRAM WRITE
      code (0x03, GTO1 p. 20/p. 27 -- the same non-DMA CRAM WRITE code the
      plain CPU DATA-port path already accepts, SEG-007-T108) instead of
      VRAM WRITE (0x01), with CD5 additionally set for the DMA family:
      CD5..CD0 = 0x23. */
   value = UINT32_C(0x9302); /* register #19: two words */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400); /* register #20 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9580); /* register #21: source byte address 0x100 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9600); /* register #22 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9700); /* register #23: memory-to-VDP mode */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0xC002); /* CRAM destination 0x0002, CD1/CD0 = 11 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0080); /* CD5..CD2 = 1000 -> masked write code 0x03 (CRAM) */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   /* SEG-007-T175: the arming write is now the synchronous suspension/
      progression event -- the whole 2-word transfer completes before it
      returns. */
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE &&
          runtime.devices.vdp.dma.kind == GENESIS_VDP_DMA_MEMORY_TO_VRAM &&
          runtime.devices.vdp.dma.write_target_code == UINT8_C(0x03));
   assert(runtime.devices.vdp.dma.source_address == UINT32_C(0x00000104));
   assert(runtime.devices.vdp.dma.remaining_length == 0U);
   assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0006));
   assert(runtime.devices.vdp.cram[UINT32_C(0x0002)] == UINT8_C(0x12) &&
          runtime.devices.vdp.cram[UINT32_C(0x0003)] == UINT8_C(0x34) &&
          runtime.devices.vdp.cram[UINT32_C(0x0004)] == UINT8_C(0x56) &&
          runtime.devices.vdp.cram[UINT32_C(0x0005)] == UINT8_C(0x78));
   /* VRAM is untouched: the CRAM target never aliases the VRAM buffer. */
   assert(runtime.devices.vdp.vram[UINT32_C(0x0002)] == 0U &&
          runtime.devices.vdp.vram[UINT32_C(0x0003)] == 0U);

   /* SEG-007-T169: selected memory-to-VSRAM DMA. The identical #1/#19/#20/
      #21-23 memory-to-VDP register protocol as the memory-to-VRAM/CRAM cases
      above; only the second command word's low bits select the VSRAM WRITE
      code (0x05, GTO1 p. 20/p. 27 -- the same non-DMA VSRAM WRITE code the
      plain CPU DATA-port path already accepts, SEG-007-T108) instead of
      VRAM/CRAM WRITE, with CD5 additionally set for the DMA family:
      CD5..CD0 = 0x25. The destination (0x004E) and a two-word transfer are
      deliberately chosen so the second word crosses VSRAM's own documented
      80-byte boundary (78 + auto-increment 2 = 80, wrapping to 0) -- a wrap
      point that could not be exercised through either VRAM's 64 KiB or
      CRAM's 128-byte modulus, proving this target's own destination size is
      actually consulted rather than reusing another target's. */
   value = UINT32_C(0x9302); /* register #19: two words */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400); /* register #20 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9580); /* register #21: source byte address 0x100 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9600); /* register #22 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9700); /* register #23: memory-to-VDP mode */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x404E); /* VSRAM destination 0x004E, CD1/CD0 = 01 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0090); /* CD5..CD2 = 1001 -> masked write code 0x05 (VSRAM) */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   /* SEG-007-T175: the arming write is now the synchronous suspension/
      progression event -- the whole 2-word transfer (including the
      documented 80-byte VSRAM wrap: 78 + auto-increment 2 = 80 -> 0)
      completes before it returns. */
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE &&
          runtime.devices.vdp.dma.kind == GENESIS_VDP_DMA_MEMORY_TO_VRAM &&
          runtime.devices.vdp.dma.write_target_code == UINT8_C(0x05));
   assert(runtime.devices.vdp.dma.source_address == UINT32_C(0x00000104));
   assert(runtime.devices.vdp.dma.remaining_length == 0U);
   assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0002)); /* wrapped: 78+2=80->0, then +2 */
   assert(runtime.devices.vdp.vsram[UINT32_C(0x004E)] == UINT8_C(0x12) &&
          runtime.devices.vdp.vsram[UINT32_C(0x004F)] == UINT8_C(0x34) &&
          runtime.devices.vdp.vsram[UINT32_C(0x0000)] == UINT8_C(0x56) &&
          runtime.devices.vdp.vsram[UINT32_C(0x0001)] == UINT8_C(0x78));
   /* VRAM and CRAM are untouched: the VSRAM target never aliases either. */
   assert(runtime.devices.vdp.vram[UINT32_C(0x004E)] == 0U &&
          runtime.devices.vdp.vram[UINT32_C(0x004F)] == 0U &&
          runtime.devices.vdp.vram[UINT32_C(0x0000)] == 0U &&
          runtime.devices.vdp.vram[UINT32_C(0x0001)] == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x004E)] == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x004F)] == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x0000)] == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x0001)] == 0U);

   /* SEG-007-T169: an odd VSRAM DMA destination fails closed before any
      mutation, mirroring CRAM's own identical policy above (and the plain
      CPU DATA-port write's documented-uncertainty policy). */
   value = UINT32_C(0x9301); /* register #19: one word */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9580);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9600);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9700);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x4007); /* VSRAM destination 0x0007 (odd, untouched by the positive
                                 block above), CD1/CD0 = 01 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0090); /* CD5..CD2 = 1001 -> masked write code 0x05 (VSRAM) */
   /* SEG-007-T175: the arm itself (register/command-word commit) always
      succeeds atomically first, exactly as before this task; the synchronous
      drain this exact write now also triggers then immediately fails closed
      on the odd destination, before any transfer mutation, so THIS write's
      own overall access now reports failure -- the same documented
      non-atomic partial-completion policy the LONG-write decomposition
      above already uses (the already-committed arm is left in place; only
      the drain's own attempted transfer is what fails). */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
   assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
          stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
          runtime.devices.vdp.dma.remaining_length == 1U &&
          runtime.devices.vdp.dma.transfer_access_count == 0U &&
          runtime.devices.vdp.vsram[UINT32_C(0x0007)] == 0U);
   /* A later status read still observes the identical failure (the DMA
      state is unchanged, so it fails the same way every time it is
      reattempted -- idempotent, not a new mutation). */
   value = UINT32_C(0xFFFFFFFF);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
   assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
          stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
          runtime.devices.vdp.dma.remaining_length == 1U &&
          runtime.devices.vdp.vsram[UINT32_C(0x0007)] == 0U);
   /* Recover: idle the runtime's own DMA state so subsequent tests below are
      unaffected by this synthetic odd-destination corruption coverage. */
   runtime.devices.vdp.dma.phase = GENESIS_VDP_DMA_IDLE;
   runtime.devices.vdp.dma.remaining_length = 0U;

   /* SEG-007-T169: CRAM's own DMA destination wraps modulo its documented
      128-byte size (distinct from VRAM's 64 KiB), and an odd CRAM DMA
      destination fails closed before any mutation -- mirroring the plain
      CPU DATA-port write's identical documented-uncertainty policy
      (genesis_vdp_data_port_target_write_halfword); VRAM's own DMA
      destination policy is completely unchanged by this task. */
   value = UINT32_C(0x9301); /* register #19: one word */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9400);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9580);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9600);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x9700);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0xC07F); /* CRAM destination 0x007F (odd), CD1/CD0 = 11 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0080);
   /* SEG-007-T175: same partial-completion policy as the VSRAM odd-
      destination case above -- the arm commits, then the synchronous drain
      immediately fails closed on the odd destination, so this write's own
      overall access reports failure. */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
   assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
          stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
          runtime.devices.vdp.dma.remaining_length == 1U &&
          runtime.devices.vdp.dma.transfer_access_count == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x007F)] == 0U);
   value = UINT32_C(0xFFFFFFFF);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
   assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
          stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
          runtime.devices.vdp.dma.remaining_length == 1U &&
          runtime.devices.vdp.dma.transfer_access_count == 0U &&
          runtime.devices.vdp.cram[UINT32_C(0x007F)] == 0U);
   /* Recover: idle the runtime's own DMA state so subsequent tests below are
      unaffected by this synthetic odd-destination corruption coverage. */
   runtime.devices.vdp.dma.phase = GENESIS_VDP_DMA_IDLE;
   runtime.devices.vdp.dma.remaining_length = 0U;

   /* Negative (SEG-007-T169): the bounded VRAM-fill engine (SEG-007-T098/
      T101) stays VRAM-only. Arming a fill-mode DMA (register #23 bits 7-6 =
      10, GTO1 §7's documented fill selector) together with the CRAM WRITE
      code (0x23) remains fail-closed rather than silently mismatch the
      fill engine's own hardcoded VRAM destination
      (genesis_vdp_data_port_fill_write always targets vdp->vram). */
   value = UINT32_C(0x9780); /* register #23: fill mode (bits 7-6 = 10) */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0xC010); /* destination 0x0010, CD1/CD0 = 11 */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   value = UINT32_C(0x0080); /* CD5..CD2 = 1000 -> masked write code 0x03 (CRAM) */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
   assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
          stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
   assert(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE); /* unchanged: arm rejected */
   assert(runtime.devices.vdp.control_port_awaiting_second_word == 1U); /* pending word untouched */
   value = UINT32_C(0x0000); /* complete the pending word with a benign one to clear state */
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
   assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);

   /* A malformed/unroutable retained DMA source is fail-closed at the
      access-caused progress boundary: no target byte, source/count, or phase
      is changed. This pre-populated state is synthetic corruption coverage;
      production starts DMA only through the routed command above. */
   {
     GenesisRuntime dma_failure = {0};
     dma_failure.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
     dma_failure.devices.vdp.dma.source_address = UINT32_C(0x00800000);
     dma_failure.devices.vdp.dma.remaining_length = 1U;
     /* SEG-007-T169: an armed memory-to-target DMA always has a valid
        write_target_code (arm-time validation in
        genesis_vdp_control_port_write_word guarantees this); this synthetic
        corruption-coverage struct sets it explicitly to VRAM (0x01) so this
        block continues to isolate exactly the source-routability failure it
        was written to prove, not a target-lookup failure. */
     dma_failure.devices.vdp.dma.write_target_code = UINT8_C(0x01);
     dma_failure.devices.vdp.addressed_pointer = UINT32_C(0x0010);
     value = UINT32_C(0xFFFFFFFF);
     assert(genesis_route_access(&dma_failure, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION &&
            stop.diagnostic_category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
     assert(dma_failure.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
            dma_failure.devices.vdp.dma.source_address == UINT32_C(0x00800000) &&
            dma_failure.devices.vdp.dma.remaining_length == 1U &&
            dma_failure.devices.vdp.dma.transfer_access_count == 0U &&
            dma_failure.devices.vdp.vram[UINT32_C(0x10)] == 0U);
   }

  /* Negative: an undocumented (non-DMA) CD5-CD0 combination remains
     fail-closed. 1st word (address 0x0009, CD1=0, CD0=1) = 0x4009; 2nd word
     with CD5..CD2 = 0010 (code 0x09, not one of GTO1's six documented
     non-DMA codes 0x00/0x01/0x03/0x04/0x05/0x08) = 0x0020. */
  value = UINT32_C(0x4009);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x0020);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 1U);
  assert(runtime.devices.vdp.control_port_first_word == UINT32_C(0x4009));
   /* SEG-007-T191: the first word already merged its A13-A0 bits (0x0009)
      over the retained A15-A14 bits (prior 0x0010 -> 0) per the Mode-5
      two-halves command model; only the rejected second word is dropped. */
   assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0009));
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0009));

  /* Negative: a documented-fixed-zero reserved bit set in the 2nd word
     (bit 2, between the CD2 lane and A15) remains fail-closed even though
     the composed CD5-CD0 code would otherwise be a documented one (VRAM
     WRITE, 0x01). 1st word (address 0x0001, CD1=0, CD0=1) = 0x4001; 2nd
     word = 0x0004. */
  value = UINT32_C(0x4001);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x0004);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0001)); /* SEG-007-T191: first word's A13-A0 already merged */
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0001));

  /* Negative: a documented-fixed-zero reserved bit set in the 2nd word's
     high byte (bits 15-8; GTO1 p. 20 draws these as all-zero) remains
     fail-closed. 1st word (address 0x0002, CD1=0, CD0=1) = 0x4002; 2nd word
     = 0x0100 (bit 8 set). */
  value = UINT32_C(0x4002);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x0100);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0002)); /* SEG-007-T191: first word's A13-A0 already merged */
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0002));
   assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);
   assert(runtime.devices.vdp.control_port_first_word == 0U);

   /* Switch the documented DMA mode selector to an intentionally unsupported
      mode so the inherited LONG partial-completion negative stays a negative
      test after T084 adds only memory-to-VRAM mode 00. */
    value = UINT32_C(0x97C0);
   assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);

  /* --- SEG-007-T091 (second frontier pass): LONG-word writes to $C00004,
     decomposed as two sequential WORD writes (D31-D16 first) through the
     identical command-word state machine above, per GTO1 p. 20. See the
     block comment above genesis_vdp_access for the full citation and this
     LONG-write's own deliberate, narrow partial-completion exception to
     genesis_route_access's general "on failure neither it nor the runtime
     is modified" contract. --- */

  /* Positive: one LONG write completing two chained one-word register-set
     commands in a single access. High word (D31-D16): register #1, data
     0x11 = 1000 0001 0001 0001 = 0x8111. Low word (D15-D0): register #2,
     data 0x22 = 1000 0010 0010 0010 = 0x8222. */
  value = UINT32_C(0x81118222);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.registers[1] == UINT32_C(0x0011));
  assert(runtime.devices.vdp.registers[2] == UINT32_C(0x0022));

  /* Positive: one LONG write whose high word is the first word of a
     non-DMA two-word VRAM WRITE address-set sequence and whose low word is
     that same sequence's second (completing) word, in a single access.
     Address 0x0ABC (CD5..CD0 = 000001, VRAM WRITE): high word (1st word) =
     0100 0000 1010 1011 1100 (CD1=0,CD0=1,A13-A0=0x0ABC) = 0x4ABC; low word
     (2nd word) = 0x0000 (CD5-CD2=0000, A15A14=00, since 0x0ABC < 0x4000). */
  value = UINT32_C(0x4ABC0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0ABC));
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);
  assert(runtime.devices.vdp.control_port_first_word == 0U);

  /* Negative: the deliberate, documented partial-completion exception --
     a LONG write whose high word succeeds (it is accepted, unconditionally,
     as a fresh two-word address-set sequence's first word: address 0x0007,
     CD1=0, CD0=1 = 0100 0000 0000 0111 = 0x4007) but whose low word fails
      (a DMA-triggering variant whose register #23 selects an unsupported
      mode). The overall
     LONG write reports GENESIS_ACCESS_FAIL, but -- unlike every other
     rejection in this file -- the high word's own already-successful
     mutation (latching control_port_first_word/control_port_awaiting_
     second_word) is left durably committed rather than rolled back: this is
     the one narrow, explicitly documented exception to the general
     "on failure neither it nor the runtime is modified" contract (see the
     block comment above genesis_vdp_access). A subsequent, correctly-shaped
     ordinary WORD write still completes that same now-pending sequence
     normally afterward, exactly like the WORD-only DMA-reject-then-retry
     case above -- demonstrating this exception leaves the state machine in
     a well-defined, still-usable state, not a corrupt one. */
  value = UINT32_C(0x40070080);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 1U); /* high word's own committed mutation */
  assert(runtime.devices.vdp.control_port_first_word == UINT32_C(0x4007)); /* high word's own committed mutation */
  /* SEG-007-T191: the accepted high (first) word already merged its A13-A0
     bits (0x0007) over the retained A15-A14 bits (prior 0x0ABC -> 0); the
     rejected low word never mutates this further. */
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0007));
  value = UINT32_C(0x0000); /* valid non-DMA VRAM WRITE completion for the same now-pending 1st word */
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0007));
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);
  assert(runtime.devices.vdp.control_port_first_word == 0U);

   /* --- SEG-007-T098 / SEG-007-T101: armed VRAM-fill DATA-port WORD write. ---
      Fill mode is selected by register #23's existing mode field. The CPU
      source word is captured before the fill mutations; the high byte is the
      fill value. SEG-007-T101 additionally adopts a modulo-64KiB
      boundary-wrap compatibility policy for the selected destination
      progression -- an explicitly replaceable project policy, not a verified
      hardware claim. These fixtures are entirely synthetic runtime state. */
   {
     GenesisRuntime fill = {0};
     GenesisVdpState before;

      /* Even pointer, increment one: capture writes distinct low/high source
         bytes at P/P+1. The one pre-fill increment preserves the low source
         byte at P while the first fill replaces the upper source slot P+1. */
     value = UINT32_C(0x8110); /* DMA enable */
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x9303); /* three fill bytes */
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x9400);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x9780); /* VRAM-fill mode, not copy */
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x8F01); /* increment one */
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x6002); /* VRAM-write DMA destination 0x2002 */
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0080);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(fill.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
            fill.devices.vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL &&
            fill.devices.vdp.dma.fill_byte_count == 3U);
      value = UINT32_C(0xABCD);
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      assert(fill.devices.vdp.vram[UINT32_C(0x2002)] == UINT8_C(0xCD) &&
             fill.devices.vdp.vram[UINT32_C(0x2003)] == UINT8_C(0xAB) &&
             fill.devices.vdp.vram[UINT32_C(0x2004)] == UINT8_C(0xAB) &&
             fill.devices.vdp.vram[UINT32_C(0x2005)] == UINT8_C(0xAB));
      assert(fill.devices.vdp.addressed_pointer == UINT32_C(0x2006) &&
             fill.devices.vdp.dma.fill_byte_count == 0U &&
             fill.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);

      /* The public fill evidence covers only an even VRAM address for the
         DATA-port source WORD. An odd armed pointer is therefore rejected
         before the source capture, fill, pointer, or DMA state can mutate. */
      fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
      fill.devices.vdp.dma.fill_byte_count = 2U;
      fill.devices.vdp.addressed_pointer = UINT32_C(0x2003);
      fill.devices.vdp.auto_increment_value = 2U;
      fill.devices.vdp.vram[UINT32_C(0x2002)] = UINT8_C(0x11);
      fill.devices.vdp.vram[UINT32_C(0x2003)] = UINT8_C(0x22);
      fill.devices.vdp.vram[UINT32_C(0x2005)] = UINT8_C(0x33);
      before = fill.devices.vdp;
      value = UINT32_C(0x5A11);
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
             stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
      assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);

     /* Wrong port/direction/width remain fail-closed while armed. */
     fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
     fill.devices.vdp.dma.fill_byte_count = 1U;
     fill.devices.vdp.addressed_pointer = UINT32_C(0x2010);
     before = fill.devices.vdp;
     value = UINT32_C(0x1234);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00002), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);

      /* A zero count is still an explicit fail-closed preflight case: armed
         state, pointer, and VRAM remain untouched. */
     fill.devices.vdp.dma.fill_byte_count = 0U;
     before = fill.devices.vdp;
     value = UINT32_C(0xBEEF);
     assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);

      /* A non-fill armed DMA kind at the DATA port still fails closed before
         any mutation: only the armed VRAM-fill shape is accepted here. */
      fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_MEMORY_TO_VRAM;
      fill.devices.vdp.dma.fill_byte_count = 2U;
      fill.devices.vdp.addressed_pointer = UINT32_C(0x1000);
      before = fill.devices.vdp;
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;

      /* SEG-007-T101 boundary-wrap compatibility policy: an armed fill whose
         selected destination progression crosses the 64KiB VRAM boundary is
         now permitted. Every advance wraps modulo 64KiB, the source WORD is
         captured at the even P/P+1 pair, and the committed post-fill pointer
         is the wrapped value. This is a replaceable project policy, not a
         verified hardware fact.

         P=0xFFFE, increment 1, three fill bytes: source low->0xFFFE,
         source high->0xFFFF, then fill high overwrites 0xFFFF, then wraps to
         0x0000 and 0x0001; final pointer = (0xFFFE + 1 + 3) mod 0x10000. */
      fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
      fill.devices.vdp.dma.fill_byte_count = 3U;
      fill.devices.vdp.addressed_pointer = UINT32_C(0xFFFE);
      fill.devices.vdp.auto_increment_value = 1U;
      fill.devices.vdp.vram[UINT32_C(0xFFFE)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0xFFFF)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0x0000)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0x0001)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0x0002)] = UINT8_C(0x99); /* untouched neighbour */
      value = UINT32_C(0x5AA5);
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      assert(fill.devices.vdp.vram[UINT32_C(0xFFFE)] == UINT8_C(0xA5) && /* source low preserved */
             fill.devices.vdp.vram[UINT32_C(0xFFFF)] == UINT8_C(0x5A) && /* source high, then fill */
             fill.devices.vdp.vram[UINT32_C(0x0000)] == UINT8_C(0x5A) && /* wrapped fill byte */
             fill.devices.vdp.vram[UINT32_C(0x0001)] == UINT8_C(0x5A) && /* wrapped fill byte */
             fill.devices.vdp.vram[UINT32_C(0x0002)] == UINT8_C(0x99));  /* neighbour untouched */
      assert(fill.devices.vdp.addressed_pointer == UINT32_C(0x0002) &&
             fill.devices.vdp.dma.fill_byte_count == 0U &&
             fill.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);

      /* Deterministic repeated execution: re-arming the identical crossing
         fill produces byte-identical VRAM and identical final pointer/DMA
         state. */
      {
        GenesisVdpState first_result = fill.devices.vdp;
        fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
        fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
        fill.devices.vdp.dma.fill_byte_count = 3U;
        fill.devices.vdp.addressed_pointer = UINT32_C(0xFFFE);
        fill.devices.vdp.vram[UINT32_C(0xFFFE)] = 0U;
        fill.devices.vdp.vram[UINT32_C(0xFFFF)] = 0U;
        fill.devices.vdp.vram[UINT32_C(0x0000)] = 0U;
        fill.devices.vdp.vram[UINT32_C(0x0001)] = 0U;
        value = UINT32_C(0x5AA5);
        assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                    GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        assert(memcmp(&fill.devices.vdp, &first_result, sizeof(first_result)) == 0);
      }

      /* Mid-VRAM crossing with increment 2: intermediate destinations wrap
         too, not just the committed pointer. P=0xFFFC, increment 2, two fill
         bytes -> 0xFFFE then wraps to 0x0000; final pointer = 0x0002. */
      fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
      fill.devices.vdp.dma.fill_byte_count = 2U;
      fill.devices.vdp.addressed_pointer = UINT32_C(0xFFFC);
      fill.devices.vdp.auto_increment_value = 2U;
      fill.devices.vdp.vram[UINT32_C(0xFFFC)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0xFFFD)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0xFFFE)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0x0000)] = 0U;
      fill.devices.vdp.vram[UINT32_C(0xFFFF)] = UINT8_C(0x77); /* skipped by increment 2 */
      value = UINT32_C(0x11EE);
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      assert(fill.devices.vdp.vram[UINT32_C(0xFFFC)] == UINT8_C(0xEE) &&
             fill.devices.vdp.vram[UINT32_C(0xFFFD)] == UINT8_C(0x11) &&
             fill.devices.vdp.vram[UINT32_C(0xFFFE)] == UINT8_C(0x11) &&
             fill.devices.vdp.vram[UINT32_C(0x0000)] == UINT8_C(0x11) &&
             fill.devices.vdp.vram[UINT32_C(0xFFFF)] == UINT8_C(0x77));
      assert(fill.devices.vdp.addressed_pointer == UINT32_C(0x0002) &&
             fill.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);

      /* An odd armed pointer still fails closed even at the boundary. */
      fill.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      fill.devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
      fill.devices.vdp.dma.fill_byte_count = 2U;
      fill.devices.vdp.addressed_pointer = UINT32_C(0xFFFF);
      before = fill.devices.vdp;
      assert(genesis_route_access(&fill, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                  GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(memcmp(&fill.devices.vdp, &before, sizeof(before)) == 0);
   }

   /* --- SEG-007-T108: plain (non-armed-fill, non-DMA) CPU DATA-port
      ($C00000) WRITE. --- The non-DMA two-word address-set command records
      its CD5-CD0 code in data_port_transfer_code; a subsequent LONG/WORD
      write to $C00000 stores big-endian halfwords into the selected target
      (CRAM code 0x03, VSRAM code 0x05) and advances addressed_pointer by the
      register-15 auto-increment, modulo the target's documented byte size.
      GTO1 p. 20 ("Long word access is equivalent to two word accesses, with
      D31-D16 written first"), p. 20/p. 27 (CD5-CD0 table), p. 28 (auto-
      increment), p. 2/p. 12 (CRAM 64 words / VSRAM 40 words). See
      docs/references/genesis-vdp-data-port-cpu-write-contract.md. Every
      fixture here is synthetic runtime state; no ROM is involved. */
   {
     GenesisRuntime dp = {0};
     GenesisDeviceState dp_before;

     /* Register #15 = 2: auto-increment two (the reached startup value). */
     value = UINT32_C(0x8F02);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);

     /* Select CRAM WRITE (CD5..CD0 = 000011) at address 0x0000: 1st word
        = 1100 0000 0000 0000 = 0xC000 (CD1=1,CD0=1,A13-A0=0); 2nd word
        = 0x0000 (CD5-CD2=0000, A15A14=00). */
     value = UINT32_C(0xC000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.data_port_transfer_code == UINT8_C(0x03));
     assert(dp.devices.vdp.data_port_transfer_code_valid == 1U);
     assert(dp.devices.vdp.addressed_pointer == 0U);

     /* Positive: a LONG data-port write. High halfword (D31-D16) 0x1122 is
        stored big-endian at CRAM[0..1], pointer 0 -> 2; low halfword 0x3344
        at CRAM[2..3], pointer 2 -> 4. */
     value = UINT32_C(0x11223344);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.cram[0] == UINT8_C(0x11) && dp.devices.vdp.cram[1] == UINT8_C(0x22));
     assert(dp.devices.vdp.cram[2] == UINT8_C(0x33) && dp.devices.vdp.cram[3] == UINT8_C(0x44));
     assert(dp.devices.vdp.addressed_pointer == 4U);

     /* Positive: a WORD data-port write at the current pointer. */
     value = UINT32_C(0x5566);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.cram[4] == UINT8_C(0x55) && dp.devices.vdp.cram[5] == UINT8_C(0x66));
     assert(dp.devices.vdp.addressed_pointer == 6U);

     /* CRAM address wrap: point near the top and confirm the write wraps
        modulo GENESIS_VDP_CRAM_BYTES (128). Re-select CRAM at 0x007E. */
     value = UINT32_C(0xC07E); /* 1st word: CD1=1,CD0=1,A13-A0=0x7E */
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.addressed_pointer == UINT32_C(0x7E));
     value = UINT32_C(0x778899AA); /* halfword 0x7788 at 0x7E; pointer -> 0; 0x99AA at 0..1 */
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.cram[UINT32_C(0x7E)] == UINT8_C(0x77) &&
            dp.devices.vdp.cram[UINT32_C(0x7F)] == UINT8_C(0x88));
     assert(dp.devices.vdp.cram[0] == UINT8_C(0x99) && dp.devices.vdp.cram[1] == UINT8_C(0xAA));
     assert(dp.devices.vdp.addressed_pointer == 2U);

     /* Deterministic repeat: the same command + LONG-write sequence on a
        fresh runtime yields byte-identical CRAM and identical pointer. */
     {
       GenesisRuntime dp2 = {0};
       value = UINT32_C(0x8F02);
       assert(genesis_route_access(&dp2, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0xC000);
       assert(genesis_route_access(&dp2, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x0000);
       assert(genesis_route_access(&dp2, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x11223344);
       assert(genesis_route_access(&dp2, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(memcmp(dp2.devices.vdp.cram, (const uint8_t[4]){0x11,0x22,0x33,0x44}, 4) == 0);
       assert(dp2.devices.vdp.addressed_pointer == 4U);
     }

     /* VSRAM WRITE (CD5..CD0 = 000101 = 0x05): 1st word bits 15-14 = CD1,CD0
        = 01 -> 0x4000; 2nd word bits 7-4 = CD5-CD2 = 0001 -> 0x0010. Address
        0x0000. */
     {
       GenesisRuntime vs = {0};
       value = UINT32_C(0x8F02);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x4000);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x0010);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(vs.devices.vdp.data_port_transfer_code == UINT8_C(0x05));
       /* LONG write of zero: the reached palette/scroll-clear shape. */
       value = UINT32_C(0x00000000);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(vs.devices.vdp.vsram[0] == 0U && vs.devices.vdp.vsram[3] == 0U);
       assert(vs.devices.vdp.addressed_pointer == 4U);
       /* Non-zero LONG write stores big-endian halfwords. */
       value = UINT32_C(0xDEADBEEF);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(vs.devices.vdp.vsram[4] == UINT8_C(0xDE) && vs.devices.vdp.vsram[5] == UINT8_C(0xAD));
       assert(vs.devices.vdp.vsram[6] == UINT8_C(0xBE) && vs.devices.vdp.vsram[7] == UINT8_C(0xEF));
       assert(vs.devices.vdp.addressed_pointer == 8U);
       /* VSRAM wrap: modulo 80 (not a power of two). Re-select at 0x004E
          (78); a LONG write stores 0x0102 at 78-79 then wraps to 0/1. */
       value = UINT32_C(0x404E);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x0010);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(vs.devices.vdp.addressed_pointer == UINT32_C(78));
       value = UINT32_C(0x01020304);
       assert(genesis_route_access(&vs, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(vs.devices.vdp.vsram[78] == UINT8_C(0x01) && vs.devices.vdp.vsram[79] == UINT8_C(0x02));
       assert(vs.devices.vdp.vsram[0] == UINT8_C(0x03) && vs.devices.vdp.vsram[1] == UINT8_C(0x04));
       assert(vs.devices.vdp.addressed_pointer == 2U);
     }

     /* --- Adversarial negatives: each FAILs and leaves devices.vdp byte-
        identical (checked by memcmp) BEFORE any mutation. dp currently has
        CRAM WRITE (0x03) selected, pointer 2, increment 2. --- */

     /* BYTE width (SEG-007-T191): modeled as a WORD write with the data
        byte mirrored into both halves. CRAM code 0x03 selected, pointer 2,
        increment 2 -> 0x77 mirrored to 0x7777 lands at CRAM[2..3]. */
     value = UINT32_C(0x77);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.cram[2] == UINT8_C(0x77) && dp.devices.vdp.cram[3] == UINT8_C(0x77));
     assert(dp.devices.vdp.addressed_pointer == 4U);
     dp_before = dp.devices;

     /* DATA-port read. */
     value = UINT32_C(0xFFFFFFFF);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&dp.devices, &dp_before, sizeof(dp_before)) == 0);

     /* Wrong address (still inside the VDP window). */
     value = UINT32_C(0x11223344);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00002), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&dp.devices, &dp_before, sizeof(dp_before)) == 0);

     /* A READ code selected (CRAM READ = 0x08 = 001000): 1st word CD1=0,CD0=0,
        A13-A0=0 -> 0x0000; 2nd word CD5-CD2 = 0010 -> 0x0020. Then a
        data-port write fails closed. */
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0020);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.data_port_transfer_code == UINT8_C(0x08));
     dp_before = dp.devices;
     value = UINT32_C(0x11223344);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
     assert(memcmp(&dp.devices, &dp_before, sizeof(dp_before)) == 0);

     /* The VRAM WRITE code 0x01 (SEG-007-T191): now a routed target. Select
        VRAM at address 0 -- 1st word CD1=0,CD0=1 -> 0x4000; 2nd word 0x0000
        -- then a LONG data-port write stores big-endian halfwords into VRAM
        with auto-increment 2. */
     value = UINT32_C(0x4000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.data_port_transfer_code == UINT8_C(0x01));
     assert(dp.devices.vdp.addressed_pointer == 0U);
     value = UINT32_C(0x11223344);
     assert(genesis_route_access(&dp, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(dp.devices.vdp.vram[0] == UINT8_C(0x11) && dp.devices.vdp.vram[1] == UINT8_C(0x22));
     assert(dp.devices.vdp.vram[2] == UINT8_C(0x33) && dp.devices.vdp.vram[3] == UINT8_C(0x44));
     assert(dp.devices.vdp.addressed_pointer == 4U);

     /* No code selected: a fresh runtime, CPU data-port write fails closed. */
     {
       GenesisRuntime nc = {0};
       GenesisDeviceState nc_before = nc.devices;
       value = UINT32_C(0x11223344);
       assert(genesis_route_access(&nc, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
       assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
              stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
       assert(memcmp(&nc.devices, &nc_before, sizeof(nc_before)) == 0);
     }

     /* An armed DMA code / armed engine at the data port: a CRAM-selected
        CPU write fails closed while a memory-to-VRAM DMA is armed. */
     {
       GenesisRuntime ad = {0};
       GenesisDeviceState ad_before;
       ad.devices.vdp.data_port_transfer_code = UINT8_C(0x03);
       ad.devices.vdp.data_port_transfer_code_valid = 1U;
       ad.devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
       ad.devices.vdp.dma.kind = GENESIS_VDP_DMA_MEMORY_TO_VRAM;
       ad.devices.vdp.dma.source_address = UINT32_C(0x00000100);
       ad.devices.vdp.dma.remaining_length = 1U;
       ad_before = ad.devices;
       value = UINT32_C(0x11223344);
       assert(genesis_route_access(&ad, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
       assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
              stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
       assert(memcmp(&ad.devices, &ad_before, sizeof(ad_before)) == 0);
     }

     /* Odd current VDP address: fail closed before any mutation (the LONG
        partial-completion policy is not reached because the first sub-write
        is rejected). Re-select CRAM at odd address 0x0001. */
     {
       GenesisRuntime od = {0};
       GenesisDeviceState od_before;
       value = UINT32_C(0x8F02);
       assert(genesis_route_access(&od, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0xC001); /* CRAM, A13-A0 = 1 */
       assert(genesis_route_access(&od, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x0000);
       assert(genesis_route_access(&od, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(od.devices.vdp.addressed_pointer == 1U);
       od_before = od.devices;
       value = UINT32_C(0x11223344);
       assert(genesis_route_access(&od, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
       assert(memcmp(&od.devices, &od_before, sizeof(od_before)) == 0);
     }
   }

   /* --- SEG-007-T191: generalized VDP data-port ($C00000) WRITE surface --
      the VRAM WRITE target (CD5-CD0 code 0x01), BYTE/WORD/LONG widths, the
      GTO1-documented VRAM odd-address byte exchange, and the coupled
      control-port latch fail-closed cases. GTO1 (Sega, Genesis Technical
      Overview v1.00, 1991) pp. 20/27-33; see
      docs/references/genesis-vdp-data-port-cpu-write-contract.md. All state
      here is synthetic runtime state; no ROM is involved. */
   {
     GenesisRuntime vr = {0};

     /* Register #15 = 2 (auto-increment two, the reached startup value). */
     value = UINT32_C(0x8F02);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);

     /* Select VRAM WRITE (CD5..CD0 = 000001) at address 0x1000: 1st word
        bits 15-14 = CD1,CD0 = 01, A13-A0 = 0x1000 -> 0x5000; 2nd word
        = 0x0000 (CD5-CD2 = 0000, A15A14 = 00). */
     value = UINT32_C(0x5000);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.data_port_transfer_code == UINT8_C(0x01));
     assert(vr.devices.vdp.data_port_transfer_code_valid == 1U);
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x1000));

     /* WORD data-port writes land big-endian at the expected VRAM offsets
        with the documented auto-increment applied between them. */
     value = UINT32_C(0x1234);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.vram[UINT32_C(0x1000)] == UINT8_C(0x12) &&
            vr.devices.vdp.vram[UINT32_C(0x1001)] == UINT8_C(0x34));
     value = UINT32_C(0x5678);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.vram[UINT32_C(0x1002)] == UINT8_C(0x56) &&
            vr.devices.vdp.vram[UINT32_C(0x1003)] == UINT8_C(0x78));
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x1004));

     /* LONG data-port write decomposes into two WORD transfers, D31-D16
        first, with auto-increment applied between them (GTO1 p. 20). */
     value = UINT32_C(0x9ABCDEF0);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.vram[UINT32_C(0x1004)] == UINT8_C(0x9A) &&
            vr.devices.vdp.vram[UINT32_C(0x1005)] == UINT8_C(0xBC) &&
            vr.devices.vdp.vram[UINT32_C(0x1006)] == UINT8_C(0xDE) &&
            vr.devices.vdp.vram[UINT32_C(0x1007)] == UINT8_C(0xF0));
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x1008));

     /* BYTE data-port write: the data byte is mirrored into both halves. */
     value = UINT32_C(0xA5);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00000), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.vram[UINT32_C(0x1008)] == UINT8_C(0xA5) &&
            vr.devices.vdp.vram[UINT32_C(0x1009)] == UINT8_C(0xA5));
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x100A));

     /* VRAM odd current address: A0 is ignored for address decoding and the
        two data bytes are exchanged (GTO1). Select VRAM at 0x2001. */
     value = UINT32_C(0x6001); /* CD1=0,CD0=1, A13-A0 = 0x2001 */
     assert(genesis_route_access(&vr, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     value = UINT32_C(0x0000);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x2001));
     value = UINT32_C(0x1122);
     assert(genesis_route_access(&vr, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
     assert(vr.devices.vdp.vram[UINT32_C(0x2000)] == UINT8_C(0x22) && /* low byte, exchanged */
            vr.devices.vdp.vram[UINT32_C(0x2001)] == UINT8_C(0x11));  /* high byte, exchanged */
     assert(vr.devices.vdp.addressed_pointer == UINT32_C(0x2003)); /* 0x2001 + increment 2 */

     /* A data-port write with no valid prior control-port setup is
        deterministic and fail-closed (fresh runtime, nothing selected). */
     {
       GenesisRuntime ns = {0};
       GenesisDeviceState ns_before = ns.devices;
       value = UINT32_C(0x1234);
       assert(genesis_route_access(&ns, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
       assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
              stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
       assert(memcmp(&ns.devices, &ns_before, sizeof(ns_before)) == 0);
     }

     /* SEG-007-T191 cancel-and-consume: an incomplete two-word command latch
        (first word latched, second not yet seen) is BROKEN by an intervening
        DATA-port access, exactly as real Mode-5 VDP hardware does -- the
        write-pending flip-flop is shared between the CONTROL and DATA ports
        (Genesis Plus GX / BlastEm `pending`; Charles MacDonald "Sega Genesis
        VDP documentation"; plutiedev "VDP command reference"). This is NOT a
        fail-closed immutable case. When no completed command has selected a
        transfer code, the DATA write itself still fails closed, but the
        pending-second-word latch is cleared regardless and the NEXT
        CONTROL-port word is a fresh first half. */
     {
       GenesisRuntime pw = {0};
       value = UINT32_C(0x8F02);
       assert(genesis_route_access(&pw, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x5000); /* first word of a VRAM address-set command */
       assert(genesis_route_access(&pw, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(pw.devices.vdp.control_port_awaiting_second_word == 1U);
       value = UINT32_C(0x1234);
       assert(genesis_route_access(&pw, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
       assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
              stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
       /* The pending sequence is cancelled by the DATA-port access even
          though the write itself was rejected (no transfer code selected). */
       assert(pw.devices.vdp.control_port_awaiting_second_word == 0U);
       assert(pw.devices.vdp.control_port_first_word == 0U);
       /* The next CONTROL-port word is a NEW first half, not the abandoned
          command's second half: it latches as control_port_first_word and
          re-arms the pending flag. (0x0100 as a second word would have set
          the fixed-zero high byte and been rejected.) */
       value = UINT32_C(0x4321);
       assert(genesis_route_access(&pw, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(pw.devices.vdp.control_port_awaiting_second_word == 1U);
       assert(pw.devices.vdp.control_port_first_word == UINT32_C(0x4321));
     }

     /* SEG-007-T191 cancel-and-consume with a completed prior target: the
        DATA write after the cancelled first half lands using the retained
        A15-A14 / CD5-CD2 of the last completed command plus the A13-A0 /
        CD1-CD0 the cancelled first word already applied (Mode-5 two-halves
        command model: Genesis Plus GX `vdp_ctrl_w`; BlastEm; plutiedev
        "The address register"). */
     {
       GenesisRuntime cx = {0};
       value = UINT32_C(0x8F02); /* reg #15 = auto-increment 2 */
       assert(genesis_route_access(&cx, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       /* Completed prior command: VRAM WRITE (CD5..CD0 = 0x01) at 0xC004
          (A15=A14=1). 1st word: CD1CD0=01, A13-A0=0x0004 -> 0x4004.
          2nd word: CD5-CD2=0000, A15A14=11 -> 0x0003. */
       value = UINT32_C(0x4004);
       assert(genesis_route_access(&cx, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       value = UINT32_C(0x0003);
       assert(genesis_route_access(&cx, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(cx.devices.vdp.addressed_pointer == UINT32_C(0xC004));
       assert(cx.devices.vdp.data_port_transfer_code == UINT8_C(0x01));
       /* First half of a NEW two-word command: CD1CD0=01, A13-A0=0x0008. */
       value = UINT32_C(0x4008);
       assert(genesis_route_access(&cx, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(cx.devices.vdp.control_port_awaiting_second_word == 1U);
       /* A13-A0 already merged over retained A15-A14 (0xC000): 0xC008. */
       assert(cx.devices.vdp.addressed_pointer == UINT32_C(0xC008));
       /* CD1-CD0 already merged over retained CD5-CD2 (0x00): still 0x01. */
       assert(cx.devices.vdp.data_port_transfer_code == UINT8_C(0x01));
       /* Intervening DATA-port write cancels the pending second word and
          lands at 0xC008 (retained high bits + first-word low bits). */
       value = UINT32_C(0xAABB);
       assert(genesis_route_access(&cx, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(cx.devices.vdp.control_port_awaiting_second_word == 0U);
       assert(cx.devices.vdp.control_port_first_word == 0U);
       assert(cx.devices.vdp.vram[UINT32_C(0xC008)] == UINT8_C(0xAA) &&
              cx.devices.vdp.vram[UINT32_C(0xC009)] == UINT8_C(0xBB));
       assert(cx.devices.vdp.addressed_pointer == UINT32_C(0xC00A)); /* +2 */
       /* The next CONTROL-port word is a fresh first half. */
       value = UINT32_C(0x4010);
       assert(genesis_route_access(&cx, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       assert(cx.devices.vdp.control_port_awaiting_second_word == 1U);
       assert(cx.devices.vdp.control_port_first_word == UINT32_C(0x4010));
       assert(cx.devices.vdp.addressed_pointer == UINT32_C(0xC010)); /* merged again */
     }

     /* Deterministic repeat: the identical VRAM command + LONG/WORD/BYTE
        sequence on two fresh runtimes yields byte-identical VRAM and an
        identical final pointer. */
     {
       GenesisRuntime a = {0};
       GenesisRuntime b = {0};
       GenesisRuntime *each[2];
       int i;
       each[0] = &a;
       each[1] = &b;
       for (i = 0; i < 2; ++i) {
         GenesisRuntime *r = each[i];
         value = UINT32_C(0x8F02);
         assert(genesis_route_access(r, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
         value = UINT32_C(0x5000);
         assert(genesis_route_access(r, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
         value = UINT32_C(0x0000);
         assert(genesis_route_access(r, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
         value = UINT32_C(0x9ABCDEF0);
         assert(genesis_route_access(r, UINT32_C(0x00C00000), GENESIS_ACCESS_LONG,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
         value = UINT32_C(0x4321);
         assert(genesis_route_access(r, UINT32_C(0x00C00000), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
         value = UINT32_C(0x7F);
         assert(genesis_route_access(r, UINT32_C(0x00C00000), GENESIS_ACCESS_BYTE,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
       }
       assert(memcmp(a.devices.vdp.vram, b.devices.vdp.vram, GENESIS_VDP_VRAM_BYTES) == 0);
       assert(a.devices.vdp.addressed_pointer == b.devices.vdp.addressed_pointer);
     }
   }

    /* Final literal accounting of every field this task mutates. Nothing
     else in GenesisVdpState (status_register, in particular -- still the
     T081 READ-only observed field) is touched by any WRITE path above. */
  assert(runtime.devices.vdp.registers[0] == UINT32_C(0x0004));
  assert(runtime.devices.vdp.registers[1] == UINT32_C(0x0011));
  assert(runtime.devices.vdp.registers[2] == UINT32_C(0x0022));
  assert(runtime.devices.vdp.registers[15] == UINT32_C(0x0002));
    assert(runtime.devices.vdp.registers[23] == UINT32_C(0x00C0));
  assert(runtime.devices.vdp.auto_increment_value == UINT32_C(0x0002));
  assert(runtime.devices.vdp.addressed_pointer == UINT32_C(0x0007));
  assert(runtime.devices.vdp.control_port_awaiting_second_word == 0U);
  assert(runtime.devices.vdp.control_port_first_word == 0U);
  assert(runtime.devices.vdp.status_register == 0U);
  return 0;
}
'''


def main() -> None:
  compiler, source_root = sys.argv[1:]
  source_root = pathlib.Path(source_root)
  with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    (directory / "harness.c").write_text(HARNESS)
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(source_root / "platforms/genesis/runtime"),
               str(directory / "harness.c"),
                str(source_root / "platforms/genesis/runtime/runtime.c"),
               "-o", str(directory / "runtime-vdp")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-vdp")], text=True,
                          capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime VDP register-area device routing: ok")


if __name__ == "__main__":
  main()
