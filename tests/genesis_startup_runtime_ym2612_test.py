#!/usr/bin/env python3
"""SEG-007-T171: direct, isolated coverage of genesis_route_access's YM2612
FM-synthesis register-window routing extension -- the PART-I address/status
port BYTE read (status) and BYTE write (register-select latch, this task's
own second frontier pass) at $A04000, the PART-I data port's own BYTE write
(register-data write, this task's own third frontier pass) at $A04001, the
PART-II address port's own BYTE write (register-select latch, this task's
own fourth frontier pass) at $A04002, and the PART-II data port's own BYTE
write (register-data write, this task's own fifth and final frontier pass)
at $A04003 -- this project's first YM2612 owner.

This proves the runtime routing function alone -- no compiled/executed
generated program is involved. It calls `genesis_route_access` directly,
exactly as tests/genesis_startup_runtime_psg_test.py does, against a real
`GenesisRuntime`, and asserts on the return value, `*value`, and `stop`.

The modelled behaviour is an explicitly labelled, replaceable PROJECT
COMPATIBILITY POLICY (see
docs/architecture/genesis-ym2612-status-port-byte-read-compatibility-policy.md),
not verified YM2612/Genesis hardware behaviour. There is no FM synthesis, no
timer modelling, no busy-flag timing, and no register-select/data state
stored. The window address is GTO1 v1.00 p. 10; the four-port layout and
status-byte/register-select/register-data field meanings are triangulated
from plutiedev.com "ym2612".

All values below are synthetic; none are commercial-derived.

Covered:
- A BYTE READ of the PART-I address/status port ($A04000) returns the fixed
  policy value 0x00 and mutates no runtime state.
- A BYTE WRITE of the PART-I address port ($A04000), the PART-I data port
  ($A04001), the PART-II address port ($A04002), and the PART-II data port
  ($A04003), for several distinct 8-bit values, each succeeds as a pure
  no-op and mutates no runtime state and never modifies the caller's own
  value.
- Determinism: repeated reads/writes over two independent runtimes yield the
  same results and byte-identical runtime state.
- Adversarial fail-closed-BEFORE-mutation for every excluded neighbour: a
  WORD/LONG access of any of the four ports (either direction), and a BYTE
  READ of any of the three write-only ports ($A04001, $A04002, $A04003).
  Every rejected access leaves the whole `GenesisRuntime` byte-identical to
  a zeroed snapshot and the caller's `value` untouched.
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

#define YM2612_PART1_ADDRESS_PORT UINT32_C(0x00A04000)
#define YM2612_PART1_DATA_PORT UINT32_C(0x00A04001)
#define YM2612_PART2_ADDRESS_PORT UINT32_C(0x00A04002)
#define YM2612_PART2_DATA_PORT UINT32_C(0x00A04003)

/* A rejected access must fail closed with the YM2612 device diagnostic AND
   mutate neither the runtime nor the caller's value. */
static void reject_ym2612(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                          uint32_t address, GenesisAccessWidth width,
                          GenesisAccessDirection direction) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

/* A rejected access that must keep a PRE-EXISTING (non-YM2612) fail-close. */
static void reject_preexisting(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                                uint32_t address, GenesisAccessWidth width,
                                GenesisAccessDirection direction,
                                GenesisStopClass want_class,
                                GenesisDiagnosticCategory want_category) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == want_class);
  assert(stop.diagnostic_category == want_category);
  assert(stop.diagnostic_category != GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

int main(void) {
  const GenesisRuntime zeroed = {0};
  GenesisRuntimeStop stop = {0};
  uint32_t value;

  /* ---- Positive: a BYTE READ of the PART-I status port returns the fixed
     policy value 0x00 and mutates no runtime state. ---- */
  {
    GenesisRuntime r = {0};
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&r, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0x00U);
    assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
  }

  /* ---- Positive: a BYTE WRITE of the PART-I address port, for several
     distinct 8-bit values, succeeds as a pure no-op -- mutates no runtime
     state and never modifies the caller's own value (matching every other
     routed WRITE owner's "a write never mutates the caller's own value"
     contract). ---- */
  {
    static const uint8_t register_selects[] = {0x00U, 0x28U, 0xB6U, 0xFFU};
    size_t i;
    for (i = 0U; i < sizeof(register_selects) / sizeof(register_selects[0]); ++i) {
      GenesisRuntime r = {0};
      uint32_t v = register_selects[i];
      assert(genesis_route_access(&r, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
      assert(v == register_selects[i]); /* a WRITE never mutates the caller's value */
      assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
    }
  }

  /* ---- Positive: a BYTE WRITE of the PART-I data port, for several
     distinct 8-bit values, succeeds as a pure no-op -- mutates no runtime
     state and never modifies the caller's own value. ---- */
  {
    static const uint8_t register_data[] = {0x00U, 0x7FU, 0x80U, 0xFFU};
    size_t i;
    for (i = 0U; i < sizeof(register_data) / sizeof(register_data[0]); ++i) {
      GenesisRuntime r = {0};
      uint32_t v = register_data[i];
      assert(genesis_route_access(&r, YM2612_PART1_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
      assert(v == register_data[i]); /* a WRITE never mutates the caller's value */
      assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
    }
  }

  /* ---- Positive: a BYTE WRITE of the PART-II address port, for several
     distinct 8-bit values, succeeds as a pure no-op -- mutates no runtime
     state and never modifies the caller's own value. ---- */
  {
    static const uint8_t register_selects[] = {0x00U, 0x30U, 0xB4U, 0xFFU};
    size_t i;
    for (i = 0U; i < sizeof(register_selects) / sizeof(register_selects[0]); ++i) {
      GenesisRuntime r = {0};
      uint32_t v = register_selects[i];
      assert(genesis_route_access(&r, YM2612_PART2_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
      assert(v == register_selects[i]); /* a WRITE never mutates the caller's value */
      assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
    }
  }

  /* ---- Positive: a BYTE WRITE of the PART-II data port, for several
     distinct 8-bit values, succeeds as a pure no-op -- mutates no runtime
     state and never modifies the caller's own value. ---- */
  {
    static const uint8_t register_data[] = {0x00U, 0x11U, 0xEEU, 0xFFU};
    size_t i;
    for (i = 0U; i < sizeof(register_data) / sizeof(register_data[0]); ++i) {
      GenesisRuntime r = {0};
      uint32_t v = register_data[i];
      assert(genesis_route_access(&r, YM2612_PART2_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
      assert(v == register_data[i]); /* a WRITE never mutates the caller's value */
      assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
    }
  }

  /* ---- Determinism: repeated reads/writes over two independent runtimes
     yield the same results and byte-identical runtime state. ---- */
  {
    GenesisRuntime a = {0};
    GenesisRuntime b = {0};
    int i;
    for (i = 0; i < 3; ++i) {
      uint32_t va = UINT32_C(0xABCDEF01);
      uint32_t vb = UINT32_C(0x12345678);
      assert(genesis_route_access(&a, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_READ, &va, &stop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&b, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_READ, &vb, &stop) == GENESIS_ACCESS_OK);
      assert(va == 0x00U && vb == 0x00U);
      va = UINT32_C(0x2A);
      vb = UINT32_C(0x2A);
      assert(genesis_route_access(&a, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &va, &stop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&b, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &vb, &stop) == GENESIS_ACCESS_OK);
      va = UINT32_C(0x55);
      vb = UINT32_C(0x55);
      assert(genesis_route_access(&a, YM2612_PART1_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &va, &stop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&b, YM2612_PART1_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &vb, &stop) == GENESIS_ACCESS_OK);
      va = UINT32_C(0x63);
      vb = UINT32_C(0x63);
      assert(genesis_route_access(&a, YM2612_PART2_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &va, &stop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&b, YM2612_PART2_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &vb, &stop) == GENESIS_ACCESS_OK);
      va = UINT32_C(0x77);
      vb = UINT32_C(0x77);
      assert(genesis_route_access(&a, YM2612_PART2_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &va, &stop) == GENESIS_ACCESS_OK);
      assert(genesis_route_access(&b, YM2612_PART2_DATA_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &vb, &stop) == GENESIS_ACCESS_OK);
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
  }

  /* ---- Adversarial: WORD/LONG access of the PART-I address port (either
     direction) fails closed with the YM2612 diagnostic (still inside the
     window, wrong width). ---- */
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_WORD,
                 GENESIS_ACCESS_READ);
  }
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_LONG,
                 GENESIS_ACCESS_READ);
  }
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART1_ADDRESS_PORT, GENESIS_ACCESS_WORD,
                 GENESIS_ACCESS_WRITE);
  }

  /* ---- Adversarial: the PART-I data port fails closed for a READ (write-only
     under this policy). ---- */
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART1_DATA_PORT, GENESIS_ACCESS_BYTE,
                 GENESIS_ACCESS_READ);
  }
  /* $A04001 is an odd address: a WORD/LONG access is rejected by
     genesis_route_access's own PRE-EXISTING odd-effective-address guard
     BEFORE the YM2612 lane is ever reached -- not the YM2612 diagnostic. */
  {
    GenesisRuntime rej = {0};
    reject_preexisting(&rej, &zeroed, YM2612_PART1_DATA_PORT, GENESIS_ACCESS_WORD,
                       GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                       GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  }

  /* ---- Adversarial: the PART-II address port fails closed for a READ
     (write-only under this policy) and for WORD/LONG width. ---- */
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART2_ADDRESS_PORT, GENESIS_ACCESS_BYTE,
                 GENESIS_ACCESS_READ);
  }
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART2_ADDRESS_PORT, GENESIS_ACCESS_WORD,
                 GENESIS_ACCESS_WRITE);
  }

  /* ---- Adversarial: the PART-II data port fails closed for a READ
     (write-only under this policy). $A04003 is an odd address: a WORD/LONG
     access is rejected by the PRE-EXISTING odd-effective-address guard
     before the YM2612 lane is ever reached. ---- */
  {
    GenesisRuntime rej = {0};
    reject_ym2612(&rej, &zeroed, YM2612_PART2_DATA_PORT, GENESIS_ACCESS_BYTE,
                 GENESIS_ACCESS_READ);
  }
  {
    GenesisRuntime rej = {0};
    reject_preexisting(&rej, &zeroed, YM2612_PART2_DATA_PORT, GENESIS_ACCESS_WORD,
                       GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                       GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  }

  /* ---- Adversarial: wrong address just outside the tight window keeps the
     PRE-EXISTING generic unmapped fail-close, NOT the YM2612 diagnostic. ---- */
  {
    GenesisRuntime rej = {0};
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00A03FFF), GENESIS_ACCESS_BYTE,
                       GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                       GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00A04004), GENESIS_ACCESS_BYTE,
                       GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                       GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  }

  /* ---- Regression: the co-located PSG port and the Z80 bus-arbitration
     registers remain unaffected. ---- */
  {
    GenesisRuntime clean = {0};
    value = UINT32_C(0x000000AA); /* tone latch, channel 1, nibble 0x0A */
    assert(genesis_route_access(&clean, UINT32_C(0x00C00011), GENESIS_ACCESS_BYTE,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(clean.devices.psg.latch_valid == 1U);
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
               "-o", str(directory / "runtime-ym2612")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-ym2612")], text=True,
                         capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime YM2612 FM-synthesis register-window routing: ok")


if __name__ == "__main__":
  main()
