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

  /* A write at the exact CTRL3 selector address remains fail-closed. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A1000C), GENESIS_ACCESS_WORD,
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
    /* DATA3 ($A10007) BYTE read: deliberately out of scope, stays
       fail-closed. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10007), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
           value == UINT32_C(0xFFFFFFFF));
    /* A WORD read at DATA1 stays fail-closed (BYTE-only family). */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10002), GENESIS_ACCESS_WORD,
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
    /* A READ of a GPIO write-only register address remains fail-closed. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&neg, UINT32_C(0x00A10009), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO &&
           value == UINT32_C(0xFFFFFFFF));
    assert(memcmp(&neg, &neg_zero, sizeof(neg)) == 0);
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
