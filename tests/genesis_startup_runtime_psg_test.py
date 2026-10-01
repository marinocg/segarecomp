#!/usr/bin/env python3
"""SEG-007-T109, rewritten by SEG-032-T006 (ADR 0072, contract section 9): the 68000's PSG (SN76489) port through
genesis_route_access, without a sound device and with a recording stand-in device.

The SEG-007 command-latch compatibility model is retired: the PSG is the shared Sega device (libs/device/sega/psg) attached by the
program through `runtime->audio_hooks` and reached by BOTH CPUs; the real device is covered by tests/genesis_audio_psg_test.py. What the
runtime itself owns, and this test proves, is the 68000-side port:
- BYTE writes at the four odd addresses $C00011/13/15/17 (GTO1 v1.00 p. 10; MacDonald, Genesis Plus GX and ares agree) are accepted
  and delivered to the attached device with the byte and the guest time of the access; without a device they are accepted and
  discarded (absent hardware);
- the evidence-bearing PSG record is the log of the port traffic: byte count and an FNV-1a digest over the bytes in order;
- fail closed BEFORE any mutation (the whole GenesisRuntime stays byte-identical, the caller's value untouched): a read (the chip is
  write-only), WORD and LONG width, and a device that rejects the byte (typed GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG); the even
  neighbours keep the pre-existing VDP lane's result, not the PSG diagnostic;
- determinism: a mixed sequence run twice yields byte-identical runtimes.
All values are synthetic.
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

static uint8_t seen_bytes[64];
static uint64_t seen_ticks[64];
static unsigned seen_count;
static int device_accepts = 1;

static int recorder(void *context, GenesisRuntime *runtime, uint8_t value, uint64_t master_ticks) {
  (void)context; (void)runtime;
  if (!device_accepts) return 0;
  seen_bytes[seen_count] = value;
  seen_ticks[seen_count++] = master_ticks;
  return 1;
}

static uint32_t digest_of(const uint8_t *bytes, unsigned count) {
  uint32_t d = UINT32_C(2166136261);
  unsigned i;
  for (i = 0; i < count; ++i) d = (d ^ bytes[i]) * UINT32_C(16777619);
  return d;
}

static void write_byte(GenesisRuntime *r, uint32_t address, uint8_t byte) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = byte;
  assert(genesis_route_access(r, address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == byte);
}

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
  static const uint32_t ports[4] = {UINT32_C(0x00C00011), UINT32_C(0x00C00013), UINT32_C(0x00C00015), UINT32_C(0x00C00017)};
  static const uint8_t sequence[8] = {0x9F, 0x80, 0x0A, 0xBF, 0xDF, 0xFF, 0xE5, 0x03};
  static const GenesisAudioHooks hooks = {0, recorder};
  unsigned i;

  /* ---- no sound device attached: accepted and discarded, the log still counts ---- */
  {
    GenesisRuntime r = {0};
    for (i = 0; i < 8; ++i) write_byte(&r, ports[i % 4], sequence[i]);
    assert(r.devices.psg.write_count == 8U && r.devices.psg.write_digest == digest_of(sequence, 8));
  }

  /* ---- an attached device sees every byte with the guest time of the access ---- */
  {
    GenesisRuntime r = {0};
    r.audio_hooks = &hooks;
    for (i = 0; i < 8; ++i) {
      r.scheduler.master_ticks = 1000U + 7U * i;
      write_byte(&r, ports[i % 4], sequence[i]);
    }
    assert(seen_count == 8U && memcmp(seen_bytes, sequence, 8) == 0);
    for (i = 0; i < 8; ++i) assert(seen_ticks[i] == 1000U + 7U * i);
    assert(r.devices.psg.write_count == 8U && r.devices.psg.write_digest == digest_of(sequence, 8));
  }

  /* ---- fail closed before any mutation ---- */
  {
    GenesisRuntime r = {0};
    r.audio_hooks = &hooks;
    seen_count = 0;
    write_byte(&r, ports[0], 0x90);
    reject(&r, ports[0], GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    reject(&r, ports[1], GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    /* WORD/LONG at the odd address are caught by the odd-effective-address guard before the PSG lane */
    reject(&r, ports[0], GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    reject(&r, ports[0], GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    /* a device that rejects the byte: typed stop, the log is not advanced */
    device_accepts = 0;
    reject(&r, ports[2], GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    device_accepts = 1;
    assert(r.devices.psg.write_count == 1U && seen_count == 1U);
    /* the even neighbours are not PSG ports: the pre-existing VDP lane answers */
    {
      GenesisRuntimeStop stop = {0};
      uint32_t value = 0x55U;
      assert(genesis_route_access(&r, UINT32_C(0x00C00010), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
      assert(stop.diagnostic_category != GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    }
  }

  /* ---- determinism ---- */
  {
    GenesisRuntime a = {0}, b = {0};
    int run;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = run == 0 ? &a : &b;
      r->audio_hooks = &hooks;
      seen_count = 0;
      for (i = 0; i < 8; ++i) { r->scheduler.master_ticks += 9U; write_byte(r, ports[(i * 3) % 4], sequence[i]); }
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
               "-o", str(directory / "runtime-psg")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-psg")], text=True, capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime PSG (SN76489) audio-port routing: ok")


if __name__ == "__main__":
  main()
