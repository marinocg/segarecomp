#!/usr/bin/env python3
"""SEG-007-T103: direct, isolated coverage of genesis_route_access's flat
68000-visible Z80 program-RAM window routing extension (window base $A00000,
GENESIS_Z80_RAM_BYTES bytes).

This proves the runtime routing function alone -- no compiled/executed
generated program is involved. It calls `genesis_route_access` directly,
exactly as tests/genesis_startup_runtime_z80_bus_test.py does for the
bus-arbitration registers, against a real `GenesisRuntime`, and asserts on the
return value, `*value`, `stop`, and the resulting
`runtime.devices.z80_bus.z80_ram` / latch fields.

The modeled behaviour is an explicitly labelled, replaceable PROJECT
COMPATIBILITY POLICY (see
docs/architecture/genesis-z80-ram-window-compatibility-policy.md), not verified
Z80-area access behaviour. There is no Z80 core, decode, JIT, or instruction
fetch: the only modeled effect is a flat byte-array read/write, gated on the
SEG-007-T102 `bus_granted` latch. Public size/range/access facts are from
Sega's *Genesis Technical Overview* v1.00 (1991) p. 2 / p. 7 / p. 76 / p. 77 /
p. 91 and Charles MacDonald's *Sega Genesis hardware notes* v0.8 SS1/SS2.

Covered:
- With `bus_granted` set: BYTE WRITE then BYTE READ-back at the window base,
  near the middle, and at the last valid byte -- round-trips the value; WRITE
  never mutates the caller's `*value`; READ is side-effect-free.
- A modelled byte-stream copy (loop of BYTE writes across a bounded span) then
  read-back, deterministic across two independent `GenesisRuntime` values.
- Adversarial fail-closed BEFORE mutation: access while `bus_granted == 0`;
  WORD width; LONG width; an in-window unsupported-width access yielding the new
  Z80-RAM device diagnostic; the first address just past the window keeping the
  PRE-EXISTING generic unmapped-region result. Each against an all-zero runtime
  (byte-identical to `zeroed`) and a pre-populated snapshot.
- Regression: `z80_bus` BUSREQ/BUSACK/RESET and the VDP status read still work
  and are unaffected; an end-to-end BUSREQ-write-then-window-access path.
- Determinism: a mixed sequence repeated twice, `memcmp` equal.
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
static void reject_z80_ram(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                           uint32_t address, GenesisAccessWidth width,
                           GenesisAccessDirection direction) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

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
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
      reject_z80_ram(&rej, &granted, Z80_RAM_BASE + GENESIS_Z80_RAM_BYTES - 2U,
                     GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
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
    reject_z80_ram(&rej, &before, Z80_RAM_BASE + 2U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE);
    reject_z80_ram(&rej, &before, Z80_RAM_BASE + 4U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);
  }

  /* ---- The first address just past the window ($A00000 + GENESIS_Z80_RAM_BYTES,
     i.e. the deliberately-excluded Z80-RAM mirror address) keeps the
     PRE-EXISTING generic unmapped-region fail-close, NOT the new Z80-RAM
     diagnostic -- even with the bus granted. ---- */
  {
    GenesisRuntime rej = {0};
    GenesisRuntime before;
    grant_bus(&rej);
    before = rej;
    value = UINT32_C(0xDEADBEEF);
    assert(genesis_route_access(&rej, Z80_RAM_BASE + GENESIS_Z80_RAM_BYTES, GENESIS_ACCESS_BYTE,
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
