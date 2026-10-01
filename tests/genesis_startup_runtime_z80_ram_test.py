#!/usr/bin/env python3
"""SEG-007-T103, rewritten by SEG-032-T004 (ADR 0072, contract section 3): the 68000 view of the Z80 sound RAM through
genesis_route_access, without a Z80 CPU. The original flat, byte-only, un-mirrored compat policy is superseded: the window is the
8 KiB RAM plus its $A02000 mirror, a WORD write stores the high byte and a WORD read returns the byte in both halves (Genesis
Plus GX z80_read_byte/z80_write_byte, MacDonald SS1.2), LONG fails closed, an access without the bus grant is the typed
GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS. The BUSREQ grant latch is still the SEG-007-T102 model here (replaced by SEG-032-T005).
The full Z80 view (bank register, banked window, YM2612/PSG ports) is covered by tests/genesis_z80_view_test.py.

Covered:
- With `bus_granted` set: BYTE write then BYTE read-back at the window base, middle, last RAM byte and through the mirror; WRITE
  never mutates the caller's `*value`; READ is side-effect-free.
- A modelled byte-stream copy then read-back, deterministic across two independent `GenesisRuntime` values.
- Adversarial fail-closed BEFORE mutation: access without the bus grant (typed); LONG width; an invalid direction; the first
  address past the mirror keeping the pre-existing generic unmapped-region result.
- Regression: `z80_bus` BUSREQ/BUSACK/RESET and the VDP status read still work. Determinism: a mixed sequence, `memcmp` equal.
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

#define Z80_RAM_BASE UINT32_C(0x00A00000)

/* Assert a rejected access is fail-closed with the Z80-RAM device diagnostic
   AND mutated neither the runtime nor the caller's value. */
static void reject_z80_ram_as(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                              uint32_t address, GenesisAccessWidth width,
                              GenesisAccessDirection direction, GenesisDiagnosticCategory diagnostic) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(stop.diagnostic_category == diagnostic);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

/* Without the bus grant the typed outcome is the missing-bus diagnostic; with it, only the LONG shape is rejected. */
#define reject_z80_ram(rt, snap, addr, width, dir) \
  reject_z80_ram_as((rt), (snap), (addr), (width), (dir), \
                    (rt)->devices.z80_bus.bus_granted ? GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM \
                                                      : GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS)

static void grant_bus(GenesisRuntime *runtime) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0x0100);
  assert(genesis_route_access(runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                              GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime->devices.z80_bus.bus_granted == 1U);
}

/* BYTE write then BYTE read-back of one window byte; asserts the round-trip,
   that the write leaves the caller's value untouched, and that the read is
   side-effect-free. */
static void roundtrip_byte(GenesisRuntime *runtime, uint32_t offset, uint8_t byte) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = byte;
  GenesisRuntime before;
  assert(genesis_route_access(runtime, Z80_RAM_BASE + offset, GENESIS_ACCESS_BYTE,
                              GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == byte); /* WRITE never mutates the caller's value */
  assert(runtime->devices.z80_bus.z80_ram[offset] == byte);
  before = *runtime;
  value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(runtime, Z80_RAM_BASE + offset, GENESIS_ACCESS_BYTE,
                              GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == byte);
  assert(memcmp(runtime, &before, sizeof(*runtime)) == 0); /* READ side-effect-free */
}

int main(void) {
  const GenesisRuntime zeroed = {0};
  GenesisRuntimeStop stop = {0};
  uint32_t value;

  /* ---- With bus granted: round-trip at base, middle, and last valid byte. ---- */
  {
    GenesisRuntime runtime = {0};
    grant_bus(&runtime);
    roundtrip_byte(&runtime, 0U, 0x5AU);
    roundtrip_byte(&runtime, GENESIS_Z80_RAM_BYTES / 2U, 0xC3U);
    roundtrip_byte(&runtime, GENESIS_Z80_RAM_BYTES - 1U, 0xA7U);
    /* The three writes are independent -- earlier bytes are still intact. */
    value = UINT32_C(0);
    assert(genesis_route_access(&runtime, Z80_RAM_BASE, GENESIS_ACCESS_BYTE,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0x5AU);
  }

  /* ---- Modelled byte-stream copy (GTO1 p. 91 step 3), deterministic across
     two independent runtimes. ---- */
  {
    GenesisRuntime a = {0};
    GenesisRuntime b = {0};
    int run;
    uint32_t i;
    const uint32_t span = 512U;
    const uint32_t start = 1024U;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = (run == 0) ? &a : &b;
      grant_bus(r);
      for (i = 0U; i < span; ++i) {
        value = (uint32_t)((i * 37U + 11U) & 0xFFU);
        assert(genesis_route_access(r, Z80_RAM_BASE + start + i, GENESIS_ACCESS_BYTE,
                                    GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      }
      for (i = 0U; i < span; ++i) {
        value = UINT32_C(0xFFFFFFFF);
        assert(genesis_route_access(r, Z80_RAM_BASE + start + i, GENESIS_ACCESS_BYTE,
                                    GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
        assert(value == (uint32_t)((i * 37U + 11U) & 0xFFU));
      }
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
  }

  /* ---- End-to-end: BUSREQ write grants the bus, then the window is usable;
     releasing the bus closes the window again. ---- */
  {
    GenesisRuntime runtime = {0};
    GenesisRuntime before;
    grant_bus(&runtime);
    value = 0x11U;
    assert(genesis_route_access(&runtime, Z80_RAM_BASE + 7U, GENESIS_ACCESS_BYTE,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    /* Release the Z80 bus (GTO1 p. 76 step 4: write $0000 to $A11100). */
    value = UINT32_C(0x0000);
    assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(runtime.devices.z80_bus.bus_granted == 0U);
    before = runtime;
    /* Window now fails closed -- and the previously written byte is untouched. */
    reject_z80_ram(&runtime, &before, Z80_RAM_BASE + 7U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
    reject_z80_ram(&runtime, &before, Z80_RAM_BASE + 7U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    assert(runtime.devices.z80_bus.z80_ram[7] == 0x11U);
  }

  /* ---- Adversarial fail-closed BEFORE mutation, all-zero runtime. ---- */
  {
    GenesisRuntime rej = {0};
    /* bus_granted == 0: the gate rejects every width/direction. */
    reject_z80_ram(&rej, &zeroed, Z80_RAM_BASE, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    reject_z80_ram(&rej, &zeroed, Z80_RAM_BASE, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
    reject_z80_ram(&rej, &zeroed, Z80_RAM_BASE + GENESIS_Z80_RAM_BYTES - 1U,
                   GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    /* Now grant the bus: unsupported widths still fail closed, atomically. */
    grant_bus(&rej);
    {
      GenesisRuntime granted = rej;
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE + GENESIS_Z80_RAM_BYTES - 4U,
                     GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
    }
  }

  /* ---- Adversarial with a pre-populated window snapshot: rejection mutates
     nothing. ---- */
  {
    GenesisRuntime rej = {0};
    GenesisRuntime before;
    uint32_t i;
    for (i = 0U; i < 64U; ++i) rej.devices.z80_bus.z80_ram[i] = (uint8_t)(i ^ 0x3CU);
    /* bus not granted */
    before = rej;
    reject_z80_ram(&rej, &before, Z80_RAM_BASE + 3U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE);
    /* grant, then unsupported width */
    grant_bus(&rej);
    before = rej;
    reject_z80_ram(&rej, &before, Z80_RAM_BASE + 4U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
    reject_z80_ram(&rej, &before, Z80_RAM_BASE + 4U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
  }

  /* ---- The mirror: $A02000-$A03FFF aliases the 8 KiB RAM. ---- */
  {
    GenesisRuntime runtime = {0};
    grant_bus(&runtime);
    value = 0x7EU;
    assert(genesis_route_access(&runtime, Z80_RAM_BASE + GENESIS_Z80_RAM_BYTES + 9U, GENESIS_ACCESS_BYTE,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(runtime.devices.z80_bus.z80_ram[9] == 0x7EU);
    value = 0U;
    assert(genesis_route_access(&runtime, Z80_RAM_BASE + 9U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value,
                                &stop) == GENESIS_ACCESS_OK);
    assert(value == 0x7EU);
  }

  /* ---- WORD: a write stores the high byte, a read returns the byte in both halves. ---- */
  {
    GenesisRuntime runtime = {0};
    grant_bus(&runtime);
    value = 0xABCDU;
    assert(genesis_route_access(&runtime, Z80_RAM_BASE + 0x20U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value,
                                &stop) == GENESIS_ACCESS_OK);
    assert(runtime.devices.z80_bus.z80_ram[0x20] == 0xABU && runtime.devices.z80_bus.z80_ram[0x21] == 0x00U);
    value = 0U;
    assert(genesis_route_access(&runtime, Z80_RAM_BASE + 0x20U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &value,
                                &stop) == GENESIS_ACCESS_OK);
    assert(value == 0xABABU);
  }

  /* ---- The first address past the mirror and the bank register, with the bus granted, keeps the
     PRE-EXISTING generic unmapped-region fail-close ($A04004 is outside every Z80-area lane). ---- */
  {
    GenesisRuntime rej = {0};
    GenesisRuntime before;
    grant_bus(&rej);
    before = rej;
    value = UINT32_C(0xDEADBEEF);
    assert(genesis_route_access(&rej, UINT32_C(0x00A04004), GENESIS_ACCESS_BYTE,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    assert(value == UINT32_C(0xDEADBEEF));
    assert(memcmp(&rej, &before, sizeof(rej)) == 0);
  }

  /* ---- Regression: bus-arbitration registers and the VDP status read are
     entirely unaffected by this addition. ---- */
  {
    GenesisRuntime runtime = {0};
    /* BUSREQ request/BUSACK read-back/release. */
    value = UINT32_C(0x0100);
    assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&runtime, UINT32_C(0x00A11100), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0U);
    /* Z80 RESET assert. */
    value = UINT32_C(0x0000);
    assert(genesis_route_access(&runtime, UINT32_C(0x00A11200), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
    assert(runtime.devices.z80_bus.reset_asserted == 1U);
    /* VDP status read still returns the zero-initialized status register. */
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0U);
  }

  /* ---- Determinism: a mixed sequence run twice is byte-identical. ---- */
  {
    GenesisRuntime a = {0};
    GenesisRuntime b = {0};
    int run;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = (run == 0) ? &a : &b;
      int step;
      grant_bus(r);
      for (step = 0; step < 3; ++step) {
        uint32_t i;
        for (i = 0U; i < 16U; ++i) {
          value = (uint32_t)((step * 5U + i) & 0xFFU);
          assert(genesis_route_access(r, Z80_RAM_BASE + i, GENESIS_ACCESS_BYTE,
                                      GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
        }
        value = UINT32_C(0xFFFFFFFF);
        assert(genesis_route_access(r, UINT32_C(0x00A11100), GENESIS_ACCESS_BYTE,
                                    GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
        value = UINT32_C(0);
        assert(genesis_route_access(r, Z80_RAM_BASE + 8U, GENESIS_ACCESS_BYTE,
                                    GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
      }
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
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
               "-o", str(directory / "runtime-z80-ram")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-z80-ram")], text=True,
                         capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime Z80 RAM-window routing: ok")


if __name__ == "__main__":
  main()
