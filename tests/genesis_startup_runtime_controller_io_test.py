#!/usr/bin/env python3
"""SEG-007-T039: direct, isolated coverage of genesis_route_access's
controller-I/O device-routing extension.

This proves the runtime routing function alone -- no compiled/executed
generated program is involved (that end-to-end proof is SEG-007-T040's job,
once the generalized-startup bridge can actually lower a sequence that
reaches this boundary). It calls `genesis_route_access` directly for each of
the CTRL1/CTRL2 (SEG-007-T020/T021), CTRL3 WORD (SEG-007-T038), Version
register (SEG-007-T079), and CTRL3 BYTE (SEG-007-T111) selector addresses
and confirms the routed value
matches the static resolver's own returned value for the identical access
shape (`m68k_controller_io_access`, src/m68k_pipeline.cpp), while every other
address in the controller-I/O interval -- and a wrong width/direction at the
three selector addresses themselves -- remains unconditionally fail-closed,
matching SEG-007-T030's already-validated baseline.
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
  uint32_t value;

  /* The exact SEG-007-T020/T021 CTRL1/CTRL2 LONG-read selector: routed to
     the same zero-valued policy result the static resolver returns. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10008), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);

  /* The exact SEG-007-T038 CTRL3 WORD-read selector: routed to the same
     zero-valued policy result the static resolver returns. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);

  /* A neighboring in-interval address (DATA1, disjoint from both selectors)
     remains unconditionally fail-closed, matching SEG-007-T030's baseline. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10000), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
         value == UINT32_C(0xFFFFFFFF));

  /* A write at the exact CTRL1/CTRL2 selector address remains fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10008), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);

  /* SEG-068-T004: a WORD write at the CTRL3 selector address is the covered even-address WORD store (low byte latched);
     a LONG write there remains fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);

  /* A LONG read starting at the CTRL3 selector's own even address remains
     fail-closed: the CTRL3 selector is WORD-only. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);

  /* The exact SEG-007-T079 Version-register BYTE-read selector: routed to
     the same policy value (0xA0) the static resolver returns. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10001), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == UINT32_C(0xA0));

  /* The exact SEG-007-T111 CTRL3 BYTE-read selector ($A1000D, the odd/low
     meaningful-data byte lane of the same EXP-port CTRL register as the
     SEG-007-T038 WORD selector): routed to the same zero-valued policy
     result the static resolver returns. BYTE width has no alignment
     constraint, so the odd address reaches the controller-I/O stage. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000D), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);

  /* A BYTE read of the CTRL3 selector's even upper lane ($A1000C) is not a
     selector and remains fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
         value == UINT32_C(0xFFFFFFFF));

  /* SEG-007-T121: a BYTE write at the CTRL3 register address ($A1000D) -- the
     one runtime-reached GPIO write -- is an accepted latched store. $A1000D is
     also the SEG-007-T111 BYTE read selector; the write is verified on a scratch
     runtime so the whole-object memcmp below still holds for the read-only
     baseline, and a read at $A1000D still returns the unchanged read-selector
     constant (never the latch). */
  {
    GenesisRuntime ctrl3_write = {0};
    value = UINT32_C(0x00000042);
    assert(genesis_route_access(&ctrl3_write, UINT32_C(0x00A1000D), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(ctrl3_write.devices.controller_io.ctrl[2] == 0x42);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&ctrl3_write, UINT32_C(0x00A1000D), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x00000000)); /* T111 policy constant, not 0x42 */
  }

  /* A WORD read of the Version register remains fail-closed -- but the
     Version register's own public address ($A10001) is itself odd (unlike
     CTRL1/CTRL2/CTRL3's own even selector-transfer addresses), so a WORD/LONG
     request at this exact address is correctly rejected by the earlier,
     already-established odd-effective-address validation stage
     (GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS), before ever reaching the
     controller-I/O device-region stage at all; this is genuinely different,
     correct pre-existing pipeline behavior, not a controller-I/O-stage
     rejection, and this test asserts the diagnostic category that actually
     fires rather than forcing an inapplicable one. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10001), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION &&
         stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS &&
         value == UINT32_C(0xFFFFFFFF));

  /* A LONG read of the Version register remains fail-closed for the same
     odd-address reason as the WORD case above. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10001), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION &&
         stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS &&
         value == UINT32_C(0xFFFFFFFF));

  /* A BYTE write at the exact Version-register selector address is
     alignment-valid (BYTE width has no alignment constraint) and correctly
     reaches the controller-I/O device-region stage, where it remains
     fail-closed because the selector is read-only. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10001), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
         stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
         value == UINT32_C(0xFFFFFFFF));

  /* Neither the routed READ successes above nor any of the fail-closed
     rejections mutated the runtime object: every read selector is
     side-effect-free, and every rejection leaves the object untouched. */
  assert(memcmp(&runtime, &zeroed, sizeof(runtime)) == 0);

  /* SEG-007-T166: DATA1/DATA2 ($A10003/$A10005) BYTE-read three-button-pad
     TH-multiplexed combinational read. Both ports, both TH states. */
  {
    GenesisRuntime pad = {0};
    /* DATA1: TH configured output (CTRL1 bit 6), driven high -> TH=1 group
       (C, B, Right, Left, Down, Up all released -> bits 5-0 = 0x3F). */
    value = UINT32_C(0x00000040);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0x00000040);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x0000007F));

    /* DATA1: TH driven low -> TH=0 group (Start, A, Down, Up released, bits
       3-2 documented forced '0' -> bits 5-0 = 0x33). */
    value = UINT32_C(0x00000000);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x00000033));

    /* DATA2: identical protocol, independent CTRL2/DATA2 latches. TH=1. */
    value = UINT32_C(0x00000040);
    assert(genesis_route_access(&pad, UINT32_C(0x00A1000B), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0x00000040);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10005), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10005), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x0000007F));

    /* DATA1 is unaffected by DATA2's own latches (independent per-port
       state): re-reading DATA1 still returns its own TH=0 group. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x00000033));

    /* CTRL-direction interaction: every DATA1 pin configured output reads
       back exactly the driven DATA latch bits, never the released-state
       convention. */
    value = UINT32_C(0x0000007F);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0x00000055);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&pad, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x00000055));
  }

  /* Adversarial negatives for the DATA1/DATA2 read family: every neighboring
     selector/width/address stays byte-for-byte fail-closed exactly as
     before -- DATA3 read, WORD/LONG read at DATA1/DATA2, and CTRL1-3/
     Version reads are all unaffected by this new family. */
  {
    GenesisRuntime neg = {0};
    /* DATA3 ($A10007, expansion port, no device) BYTE read: every input line released (0x7F); a WORD read of it
       stays fail-closed. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10007), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x7F));
    assert(memcmp(&neg, &zeroed, sizeof(neg)) == 0);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A1000E), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
           value == UINT32_C(0xFFFFFFFF));
    /* A WORD read at DATA1 stays fail-closed (BYTE-only family). */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10010), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
           value == UINT32_C(0xFFFFFFFF));
    /* A LONG read at DATA2 stays fail-closed. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10004), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
           value == UINT32_C(0xFFFFFFFF));
    assert(memcmp(&neg, &zeroed, sizeof(neg)) == 0);
  }

  /* SEG-007-T121: the GPIO-register WRITE selector family (DATA1..DATA3
     $A10003/5/7, CTRL1..CTRL3 $A10009/B/D), BYTE width, is a deterministic
     latched store into devices->controller_io. */
  value = UINT32_C(0x0000005A);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.controller_io.ctrl[0] == 0x5A);
  value = UINT32_C(0x0000FF3C);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.controller_io.data[0] == 0x3C);
  value = UINT32_C(0x000000A1);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000D), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.controller_io.ctrl[2] == 0xA1);
  /* The write is deterministic: the same store reproduces the same latch. */
  {
    GenesisRuntime again = {0};
    value = UINT32_C(0x0000005A);
    assert(genesis_route_access(&again, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(again.devices.controller_io.ctrl[0] == runtime.devices.controller_io.ctrl[0]);
  }
  /* A write never produces a read-back value: *value is left as the caller set
     it (the store consumes it, it is not overwritten). */
  assert(value == UINT32_C(0x0000005A));

  /* Adversarial negatives: neighbouring controller-I/O write shapes outside
     the GPIO-register family remain fail-closed with the established
     diagnostic, and mutate nothing. */
  {
    GenesisRuntime neg = {0};
    const GenesisRuntime neg_zero = {0};
    /* LONG write intersecting the GPIO block. */
    value = UINT32_C(0xDEADBEEF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10008), GENESIS_ACCESS_LONG,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS &&
           stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    /* The serial S-CTRL register ($A10013) is a different owner: fail-closed. */
    value = UINT32_C(0x00000001);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10013), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    /* The serial S-CTRL registers and the Sega-mapper SRAM control register accept a BYTE write of their idle value
       (serial disabled / cartridge ROM stays mapped) and change nothing; every other value, width or neighbour stays
       fail-closed. */
    {
      const uint32_t idle_registers[] = {UINT32_C(0x00A10013), UINT32_C(0x00A10019), UINT32_C(0x00A1001F),
                                         UINT32_C(0x00A130F1)};
      size_t idle_index;
      GenesisRuntime idle = {0};
      const GenesisRuntime idle_zero = {0};
      for (idle_index = 0U; idle_index < sizeof(idle_registers) / sizeof(idle_registers[0]); ++idle_index) {
        value = UINT32_C(0);
        assert(genesis_route_access(&idle, idle_registers[idle_index], GENESIS_ACCESS_BYTE,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        assert(memcmp(&idle, &idle_zero, sizeof(idle)) == 0);
        value = UINT32_C(0);
        assert(genesis_route_access(&idle, idle_registers[idle_index], GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
        value = UINT32_C(0);
        assert(genesis_route_access(&idle, idle_registers[idle_index], GENESIS_ACCESS_BYTE,
                                     GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
      }
      /* Serial control: any non-zero value (a serial mode) is unsupported. */
      value = UINT32_C(0x00000080);
      assert(genesis_route_access(&idle, UINT32_C(0x00A10019), GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      /* Mapper SRAM control: bit 0 set would map cartridge SRAM over the ROM, which is not modelled. */
      value = UINT32_C(0x00000001);
      assert(genesis_route_access(&idle, UINT32_C(0x00A130F1), GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
      value = UINT32_C(0x00000002);  /* write protect bit only: ROM stays mapped */
      assert(genesis_route_access(&idle, UINT32_C(0x00A130F1), GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      /* A neighbouring mapper register is not admitted. */
      value = UINT32_C(0);
      assert(genesis_route_access(&idle, UINT32_C(0x00A130F3), GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(memcmp(&idle, &idle_zero, sizeof(idle)) == 0);
    }
    /* A BYTE write of the Version register remains fail-closed (read-only). */
    value = UINT32_C(0x000000A0);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10001), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    /* A BYTE write of the CTRL3 even upper lane ($A1000C) is not a register. */
    value = UINT32_C(0x000000FF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A1000C), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    /* A READ of an unwritten CTRL1 direction latch ($A10009, documented R/W) returns zero and mutates nothing; a
       WORD read of it is not the BYTE register and stays fail-closed. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0));
    assert(memcmp(&neg, &neg_zero, sizeof(neg)) == 0);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10012), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(memcmp(&neg, &neg_zero, sizeof(neg)) == 0);
    /* CTRL1/CTRL2 read back exactly what was last written; CTRL3 ($A1000D) keeps its fixed read-selector constant. */
    value = UINT32_C(0x00000040);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0x0000000F);
    assert(genesis_route_access(&neg, UINT32_C(0x00A1000B), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x40));
    assert(genesis_route_access(&neg, UINT32_C(0x00A1000B), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == UINT32_C(0x0F));
  }

  /* SEG-068-T004: WORD access at the even address of each GPIO register pair. Hardware fact (GTO1 pp. 72/74/75): the
     register is the lower byte of the WORD; project assumption: the even lane is discarded on writes and 0 on reads. */
  {
    static const uint32_t even[6] = {0xA10002U, 0xA10004U, 0xA10006U, 0xA10008U, 0xA1000AU, 0xA1000CU};
    GenesisRuntime w; GenesisRuntimeStop wstop; uint32_t wv; unsigned i;
    memset(&w, 0, sizeof(w)); memset(&wstop, 0, sizeof(wstop));
    for (i = 0U; i < 6U; ++i) {
      wv = UINT32_C(0xFFFF0000) | (0x10U + i); /* upper lane / high bits must be discarded */
      assert(genesis_route_access(&w, even[i], GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &wv, &wstop) == GENESIS_ACCESS_OK);
    }
    /* each write landed in exactly the odd-lane register of its pair */
    for (i = 0U; i < 3U; ++i) assert(w.devices.controller_io.data[i] == 0x10U + i);
    for (i = 0U; i < 3U; ++i) assert(w.devices.controller_io.ctrl[i] == 0x13U + i);
    /* WORD write equals the BYTE write at the odd address (single owner): CTRL1 read back through both widths */
    wv = UINT32_C(0xFFFFFF00) | 0x47U;
    assert(genesis_route_access(&w, UINT32_C(0x00A10008), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &wv, &wstop) == GENESIS_ACCESS_OK);
    wv = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&w, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &wv, &wstop) == GENESIS_ACCESS_OK);
    assert(wv == 0x47U);
    wv = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&w, UINT32_C(0x00A10008), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &wv, &wstop) == GENESIS_ACCESS_OK);
    assert(wv == 0x47U); /* even lane 0, odd lane = register */
    wv = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&w, UINT32_C(0x00A1000A), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &wv, &wstop) == GENESIS_ACCESS_OK);
    assert(wv == 0x14U);
    /* DATA ports read through the same pin model as their BYTE read (pad mask p1) */
    {
      uint32_t bv = 0U;
      wv = 0xFFFFFFFFU;
      assert(genesis_route_access(&w, UINT32_C(0x00A10003), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &bv, &wstop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&w, UINT32_C(0x00A10002), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &wv, &wstop) == GENESIS_ACCESS_OK);
      assert(wv == bv && (wv & 0xFF00U) == 0U);
    }
    /* CTRL3 WORD read keeps its SEG-007-T038 constant, the latch is never read back */
    wv = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&w, UINT32_C(0x00A1000C), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &wv, &wstop) == GENESIS_ACCESS_OK);
    assert(wv == 0U);
    /* residual fail-closed set: LONG write/read to pairs (except the T020 LONG read), odd-address WORD, serial pairs,
       Version WORD write, and a failed access mutates nothing */
    {
      GenesisRuntime before = w;
      static const struct { uint32_t a; GenesisAccessWidth wd; GenesisAccessDirection d; } neg2[] = {
        {0xA10002U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE},
        {0xA10004U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ},
        {0xA10008U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE},
        {0xA1000EU, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE}, /* TxDATA1 pair: serial owner */
        {0xA10010U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE},
        {0xA10012U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE},
        {0xA10000U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE}, /* Version pair is read-only policy */
      };
      for (i = 0U; i < sizeof(neg2) / sizeof(neg2[0]); ++i) {
        wv = UINT32_C(0xA5A5A5A5);
        assert(genesis_route_access(&w, neg2[i].a, neg2[i].wd, neg2[i].d, &wv, &wstop) == GENESIS_ACCESS_FAIL);
        assert(wstop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
      }
      /* odd-address WORD is rejected by the earlier effective-address stage */
      wv = 0U;
      assert(genesis_route_access(&w, UINT32_C(0x00A10003), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &wv, &wstop) == GENESIS_ACCESS_FAIL);
      assert(wstop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
      assert(memcmp(&w.devices.controller_io, &before.devices.controller_io, sizeof(before.devices.controller_io)) == 0);
    }
  }
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
               "-o", str(directory / "runtime-controller-io")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-controller-io")], text=True,
                          capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime controller-I/O device routing: ok")


if __name__ == "__main__":
  main()
