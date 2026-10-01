#!/usr/bin/env python3
"""SEG-007-T171, rewritten by SEG-032-T007 (ADR 0072/0074, contract section 9): the 68000's YM2612 ports through genesis_route_access,
without a sound device and with a recording stand-in device.

The SEG-007 status-port-only compatibility model is retired: the YM2612 is the host-clocked device of libs/device/sega/ym2612 attached
through `runtime->audio_hooks` and shared with the Z80 (covered by tests/genesis_audio_ym_test.py with the real chip). What the runtime
owns, and this test proves, is the 68000-side port:
- BYTE read or write at each of $A04000-$A04003: the access reaches the attached device with the port (`address & 3`), the value and
  the guest time of the access; a read returns the device's status byte whatever the port; without a device a read returns 0 and a
  write is accepted and discarded (absent hardware);
- the device-reset hook is not driven by 68000 accesses;
- fail closed BEFORE any mutation (the whole GenesisRuntime stays byte-identical): WORD and LONG width (typed
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612 or the pre-existing odd-address guard), a device that rejects the access, and the
  neighbours outside the four ports keep the pre-existing generic unmapped fail-close;
- determinism. All values are synthetic.
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

static unsigned reads, writes;
static uint32_t last_port, last_value;
static uint64_t last_ticks;
static int accepts = 1;

static int ym_read(void *context, GenesisRuntime *runtime, uint32_t port, uint64_t ticks, uint8_t *value) {
  (void)context; (void)runtime;
  if (!accepts) return 0;
  ++reads; last_port = port; last_ticks = ticks;
  *value = (uint8_t)(0xA0U | port);
  return 1;
}
static int ym_write(void *context, GenesisRuntime *runtime, uint32_t port, uint8_t value, uint64_t ticks) {
  (void)context; (void)runtime;
  if (!accepts) return 0;
  ++writes; last_port = port; last_value = value; last_ticks = ticks;
  return 1;
}
static const GenesisAudioHooks hooks = {0, 0, ym_read, ym_write, 0};

static void reject(GenesisRuntime *r, uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction,
                   GenesisStopClass stop_class, GenesisDiagnosticCategory diagnostic) {
  GenesisRuntime before = *r;
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(r, address, width, direction, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == stop_class && stop.diagnostic_category == diagnostic);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(r, &before, sizeof(*r)) == 0);
}

int main(void) {
  GenesisRuntimeStop stop = {0};
  uint32_t value, port;

  /* ---- no sound device attached: a read is 0, a write is accepted and discarded, the runtime is untouched ---- */
  {
    GenesisRuntime r = {0};
    const GenesisRuntime zeroed = {0};
    for (port = 0; port < 4; ++port) {
      value = UINT32_C(0xFFFFFFFF);
      assert(genesis_route_access(&r, UINT32_C(0x00A04000) + port, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
      assert(value == 0U);
      value = 0x5AU;
      assert(genesis_route_access(&r, UINT32_C(0x00A04000) + port, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      assert(value == 0x5AU); /* a write never modifies the caller's value */
    }
    assert(memcmp(&r, &zeroed, sizeof(r)) == 0);
  }

  /* ---- an attached device sees port, value and guest time; a read returns its status byte ---- */
  {
    GenesisRuntime r = {0};
    r.audio_hooks = &hooks;
    for (port = 0; port < 4; ++port) {
      r.scheduler.master_ticks = 700U + 11U * port;
      value = 0x30U + port;
      assert(genesis_route_access(&r, UINT32_C(0x00A04000) + port, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
      assert(last_port == port && last_value == 0x30U + port && last_ticks == 700U + 11U * port);
      value = 0U;
      assert(genesis_route_access(&r, UINT32_C(0x00A04000) + port, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
      assert(value == (0xA0U | port) && last_port == port);
    }
    assert(reads == 4U && writes == 4U);
  }

  /* ---- fail closed before any mutation ---- */
  {
    GenesisRuntime r = {0};
    r.audio_hooks = &hooks;
    /* WORD/LONG at an even port: the pre-existing odd-address guard does not apply to $A04000/$A04002, so the YM2612 lane rejects */
    reject(&r, UINT32_C(0x00A04000), GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    reject(&r, UINT32_C(0x00A04000), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    reject(&r, UINT32_C(0x00A04002), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    /* odd-address WORD/LONG: the generic guard answers first */
    reject(&r, UINT32_C(0x00A04001), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    /* a device that rejects the access */
    accepts = 0;
    reject(&r, UINT32_C(0x00A04001), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    reject(&r, UINT32_C(0x00A04000), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    accepts = 1;
    /* outside the four ports: the pre-existing generic fail-close, not the YM2612 diagnostic */
    reject(&r, UINT32_C(0x00A04004), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    reject(&r, UINT32_C(0x00A05FFF), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  }

  /* ---- determinism ---- */
  {
    GenesisRuntime a = {0}, b = {0};
    int run;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = run == 0 ? &a : &b;
      r->audio_hooks = &hooks;
      for (port = 0; port < 8; ++port) {
        r->scheduler.master_ticks += 13U;
        value = port * 3U;
        assert(genesis_route_access(r, UINT32_C(0x00A04000) + (port & 3U), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
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
               "-o", str(directory / "runtime-ym2612")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-ym2612")], text=True, capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime YM2612 port routing: ok")


if __name__ == "__main__":
  main()
