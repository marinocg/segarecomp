#!/usr/bin/env python3
"""SEG-007-T102: direct, isolated coverage of genesis_route_access's 68k-side
Z80 bus-arbitration control-register routing extension ($A11100 BUSREQ,
$A11200 RESET).

This proves the runtime routing function alone -- no compiled/executed
generated program is involved. It calls `genesis_route_access` directly,
exactly as tests/genesis_startup_runtime_vdp_test.py does for the VDP region,
against a real `GenesisRuntime`, and asserts on the return value, `*value`,
`stop`, and the resulting `runtime.devices.z80_bus` latch fields.

The modeled behaviour is an explicitly labelled, replaceable PROJECT
COMPATIBILITY POLICY (see
docs/architecture/genesis-z80-bus-arbitration-compatibility-policy.md), not
verified bus-arbitration timing. Hardware register facts are from Sega's
*Genesis Technical Overview* v1.00 (1991) p. 76 SS4 "Z80 CONTROL" and p. 91.

Covered:
- BUSREQ request/release (WORD D8 and BYTE D0) updating the latch, including
  immediate grant and other-bit tolerance.
- BUSACK read-back (WORD and BYTE) reflecting `bus_granted`.
- Z80 RESET assert/release (WORD D8 and BYTE D0) updating `reset_asserted`,
  and its independence from the BUSREQ latch.
- Deterministic repeated execution of a mixed sequence.
- Adversarial fail-closed-BEFORE-mutation for every excluded neighbour: LONG
  width at either register, a READ of $A11200 (wrong direction) at WORD and
  BYTE, a BYTE access to the odd half of a register address, unmodeled
  sub-addresses inside the recognised region, the region's inclusive upper
  boundary, and a non-arbitration address just past the region (which keeps
  the pre-existing generic unmapped-region fail-close, NOT the new Z80-bus
  diagnostic). Every rejected access leaves the whole `GenesisRuntime`
  byte-identical to a zeroed copy (or to a pre-populated snapshot) and leaves
  the caller's `value` untouched.
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

/* Assert a rejected access is fail-closed with the Z80-bus device diagnostic
   AND mutated neither the runtime nor the caller's value. */
static void reject_z80_bus(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                            uint32_t address, GenesisAccessWidth width,
                            GenesisAccessDirection direction) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

int main(void) {
  GenesisRuntime runtime = {0};
  const GenesisRuntime zeroed = {0};
  GenesisRuntimeStop stop = {0};
  uint32_t value;

  /* ---- BUSREQ request/release, WORD ($A11100 D8; GTO1 p. 76 step 1/4). ---- */
  value = UINT32_C(0x0100);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == UINT32_C(0x0100)); /* WRITE never mutates the caller's value */
  assert(runtime.devices.z80_bus.bus_requested == 1U);
  assert(runtime.devices.z80_bus.bus_granted == 1U); /* immediate-grant policy */
  assert(runtime.devices.z80_bus.reset_asserted == 0U);

  /* BUSACK read-back, WORD: granted => D8 clear, every other bit 0. */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);
  assert(runtime.devices.z80_bus.bus_requested == 1U); /* read is side-effect-free */
  assert(runtime.devices.z80_bus.bus_granted == 1U);

  /* Release, WORD (GTO1 p. 76 step 4: write $0000). */
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 0U);
  assert(runtime.devices.z80_bus.bus_granted == 0U);

  /* BUSACK read-back, WORD: not granted => D8 set. */
  value = 0U;
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == UINT32_C(0x0100));

  /* ---- BUSREQ request/release, BYTE ($A11100 D0). ---- */
  value = UINT32_C(0x01);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 1U);
  assert(runtime.devices.z80_bus.bus_granted == 1U);

  /* BUSACK read-back, BYTE: granted => D0 clear. */
  value = UINT32_C(0xFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);

  value = UINT32_C(0x00);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 0U);
  assert(runtime.devices.z80_bus.bus_granted == 0U);

  /* BUSACK read-back, BYTE: not granted => D0 set. */
  value = 0U;
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == UINT32_C(0x01));

  /* Other-bit tolerance: only D8 (WORD) / D0 (BYTE) selects the request. */
  value = UINT32_C(0xFEFF); /* every bit but D8 set */
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 0U);
  value = UINT32_C(0x0101); /* D8 set (plus D0) */
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 1U);
  value = UINT32_C(0xFE); /* BYTE, D0 clear */
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.bus_requested == 0U);

  /* ---- Z80 RESET assert/release, WORD ($A11200 D8; GTO1 p. 76 / p. 91). ----
     Writing the bit as 0 asserts /RESET; writing it as 1 releases it. */
  value = UINT32_C(0x0000);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.reset_asserted == 1U);
  /* Independence: a RESET write leaves the BUSREQ latch untouched. */
  assert(runtime.devices.z80_bus.bus_requested == 0U);
  assert(runtime.devices.z80_bus.bus_granted == 0U);

  value = UINT32_C(0x0100);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.reset_asserted == 0U);

  /* ---- Z80 RESET assert/release, BYTE ($A11200 D0). ---- */
  value = UINT32_C(0x00);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.reset_asserted == 1U);
  value = UINT32_C(0x01);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.reset_asserted == 0U);
  /* Other-bit tolerance for RESET: only D0 (BYTE) selects the line. */
  value = UINT32_C(0xFE);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.z80_bus.reset_asserted == 1U);

  /* ---- Deterministic repeated execution of the documented startup shape
     (GTO1 p. 91: BUSREQ on, RESET off, ..., RESET on, BUSREQ off). ---- */
  {
    GenesisRuntime a = {0};
    GenesisRuntime b = {0};
    int run;
    int step;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = (run == 0) ? &a : &b;
      for (step = 0; step < 4; ++step) {
        value = UINT32_C(0x0100);
        assert(genesis_route_access(r, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        value = UINT32_C(0x0100);
        assert(genesis_route_access(r, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        value = UINT32_C(0xFFFFFFFF);
        assert(genesis_route_access(r, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                                     GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
        assert(value == 0U);
        value = UINT32_C(0x0000);
        assert(genesis_route_access(r, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        value = UINT32_C(0x0000);
        assert(genesis_route_access(r, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                                     GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      }
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    assert(a.devices.z80_bus.bus_requested == 0U);
    assert(a.devices.z80_bus.bus_granted == 0U);
    assert(a.devices.z80_bus.reset_asserted == 1U);
  }

  /* ---- Adversarial: every excluded neighbour fails closed BEFORE any
     mutation. Each runs against an all-zero runtime and must leave it
     byte-identical to `zeroed`. ---- */
  {
    GenesisRuntime rej = {0};
    /* LONG width at either documented register. */
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11100), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11100), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11200), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11200), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
    /* READ of the RESET register (write-only): wrong direction. */
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11200), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
    /* BYTE access to the odd half of a register address (not the D8 lane). */
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11101), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11201), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    /* Unmodeled sub-addresses inside the recognised region. */
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11102), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11102), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A111FE), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ);
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A11202), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
    /* Region's inclusive upper boundary ($A112FF is the last byte in range). */
    reject_z80_bus(&rej, &zeroed, UINT32_C(0x00A112FF), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
  }

  /* Adversarial with a pre-populated latch: rejection still mutates nothing. */
  {
    GenesisRuntime rej = {0};
    GenesisRuntime before;
    rej.devices.z80_bus.bus_requested = 1U;
    rej.devices.z80_bus.bus_granted = 1U;
    rej.devices.z80_bus.reset_asserted = 1U;
    before = rej;
    reject_z80_bus(&rej, &before, UINT32_C(0x00A11100), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
    reject_z80_bus(&rej, &before, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ);
    reject_z80_bus(&rej, &before, UINT32_C(0x00A11180), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
  }

  /* A non-arbitration address just past the region keeps the pre-existing
     generic unmapped-region fail-close, NOT the new Z80-bus diagnostic. */
  {
    GenesisRuntime rej = {0};
    value = UINT32_C(0xDEADBEEF);
    assert(genesis_route_access(&rej, UINT32_C(0x00A11300), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    assert(value == UINT32_C(0xDEADBEEF));
    assert(memcmp(&rej, &zeroed, sizeof(rej)) == 0);
  }

  /* The existing controller-I/O and VDP selectors are entirely unaffected by
     this addition (regression, mirroring the VDP test's own discipline). */
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10008), GENESIS_ACCESS_LONG,
                               GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == 0U);
  {
    GenesisRuntime clean = {0};
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&clean, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0U);
    assert(clean.devices.z80_bus.bus_requested == 0U &&
           clean.devices.z80_bus.bus_granted == 0U &&
           clean.devices.z80_bus.reset_asserted == 0U);
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
               "-o", str(directory / "runtime-z80-bus")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-z80-bus")], text=True,
                         capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime Z80 bus-arbitration control-register routing: ok")


if __name__ == "__main__":
  main()
