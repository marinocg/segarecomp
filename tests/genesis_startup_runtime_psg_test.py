#!/usr/bin/env python3
"""SEG-007-T109: direct, isolated coverage of genesis_route_access's PSG
(SN76489) audio-port routing extension -- the single co-located port at the
odd byte address $C00011.

This proves the runtime routing function alone -- no compiled/executed
generated program is involved. It calls `genesis_route_access` directly,
exactly as tests/genesis_startup_runtime_z80_bus_test.py does, against a real
`GenesisRuntime`, and asserts on the return value, `*value`, `stop`, and the
resulting `runtime.devices.psg` latch fields.

The modelled behaviour is an explicitly labelled, replaceable PROJECT
COMPATIBILITY POLICY (see
docs/architecture/genesis-psg-sn76489-port-write-compatibility-policy.md), not
verified SN76489/Genesis hardware behaviour. There is no audio synthesis. The
SN76489 command-byte format is triangulated across SMS Power
"Development/SN76489", plutiedev.com "psg", and Charles MacDonald's Sega
Genesis hardware notes; the port address is GTO1 v1.00 p. 10.

All values below are synthetic; none are commercial-derived.

Covered:
- LATCH+DATA byte for a tone channel updates the latch (channel + tone/noise
  selector) and writes the low 4 bits of that channel's 10-bit period.
- A following DATA byte sets the high 6 bits of the last-latched tone channel's
  period.
- Volume/attenuation LATCH byte per channel; a following DATA byte updates the
  4-bit attenuation.
- Noise-control LATCH byte (channel 3) and a following DATA byte.
- Each of the four channels.
- Determinism: a mixed write sequence run twice over two runtimes yields
  byte-identical device state.
- Adversarial fail-closed-BEFORE-mutation for every excluded neighbour: WORD
  and LONG width, a PSG-port READ (WORD and BYTE), a wrong address just outside
  the tight interval (must keep the PRE-EXISTING VDP / unmapped fail-close, NOT
  the PSG diagnostic), the reserved noise LATCH encoding (channel 3, tone/noise
  type, data bit 3 set), and a DATA byte with no prior LATCH. Every rejected
  access leaves the whole `GenesisRuntime` byte-identical to a zeroed/snapshot
  copy and the caller's `value` untouched.
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

#define PSG_PORT UINT32_C(0x00C00011)

/* A rejected access must fail closed with the PSG device diagnostic AND mutate
   neither the runtime nor the caller's value. */
static void reject_psg(GenesisRuntime *runtime, const GenesisRuntime *snapshot,
                        uint32_t address, GenesisAccessWidth width,
                        GenesisAccessDirection direction) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(runtime, address, width, direction, &value, &stop) ==
         GENESIS_ACCESS_FAIL);
  assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

/* A rejected access that must keep a PRE-EXISTING (non-PSG) fail-close. */
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
  assert(stop.diagnostic_category != GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
  assert(value == UINT32_C(0xDEADBEEF));
  assert(memcmp(runtime, snapshot, sizeof(*runtime)) == 0);
}

static void psg_write(GenesisRuntime *runtime, uint8_t command) {
  GenesisRuntimeStop stop = {0};
  uint32_t value = command;
  assert(genesis_route_access(runtime, PSG_PORT, GENESIS_ACCESS_BYTE,
                               GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(value == command); /* a WRITE never mutates the caller's value */
}

int main(void) {
  const GenesisRuntime zeroed = {0};
  GenesisRuntimeStop stop = {0};
  uint32_t value;

  /* ---- LATCH+DATA byte for a tone channel, then a DATA byte. ----
     Tone latch: %1 cc 0 dddd. Channel 1, tone/noise, low nibble 0xA => 0xAA.
     ($80 | (1<<5) | 0x0A). */
  {
    GenesisRuntime r = {0};
    psg_write(&r, 0xAAU);
    assert(r.devices.psg.latch_valid == 1U);
    assert(r.devices.psg.latched_channel == 1U);
    assert(r.devices.psg.latched_volume == 0U);
    assert(r.devices.psg.tone_period[1] == UINT16_C(0x000A));
    /* DATA byte %0-DDDDDD: high 6 bits of the 10-bit period. 0x2D => 0x2D << 4. */
    psg_write(&r, 0x2DU);
    assert(r.devices.psg.tone_period[1] == (uint16_t)((0x2DU << 4) | 0x0AU));
    /* Other channels/registers untouched. */
    assert(r.devices.psg.tone_period[0] == 0U && r.devices.psg.tone_period[2] == 0U);
    assert(r.devices.psg.attenuation[0] == 0U && r.devices.psg.attenuation[1] == 0U);
    assert(r.devices.psg.noise_control == 0U);
  }

  /* ---- Volume/attenuation LATCH byte, then a DATA byte, each channel. ----
     Volume latch: %1 cc 1 dddd => $90 | (channel<<5) | attenuation. */
  {
    GenesisRuntime r = {0};
    uint8_t channel;
    for (channel = 0U; channel < 4U; ++channel) {
      psg_write(&r, (uint8_t)(0x90U | (channel << 5) | 0x0FU)); /* attenuation 15 (silent) */
      assert(r.devices.psg.latched_channel == channel);
      assert(r.devices.psg.latched_volume == 1U);
      assert(r.devices.psg.attenuation[channel] == 0x0FU);
    }
    /* All four channels silenced; tone periods untouched. */
    assert(r.devices.psg.attenuation[0] == 0x0FU && r.devices.psg.attenuation[1] == 0x0FU &&
           r.devices.psg.attenuation[2] == 0x0FU && r.devices.psg.attenuation[3] == 0x0FU);
    assert(r.devices.psg.tone_period[0] == 0U);
    /* A DATA byte updates the last-latched (channel 3) attenuation, low 4 bits. */
    psg_write(&r, 0x33U); /* %00 110011 -> low nibble 0x3 */
    assert(r.devices.psg.attenuation[3] == 0x03U);
  }

  /* ---- Each tone channel via LATCH low nibble. ---- */
  {
    GenesisRuntime r = {0};
    psg_write(&r, 0x80U | 0x03U);            /* channel 0 tone, nibble 3 */
    assert(r.devices.psg.tone_period[0] == UINT16_C(0x0003));
    psg_write(&r, 0x80U | (2U << 5) | 0x07U); /* channel 2 tone, nibble 7 */
    assert(r.devices.psg.latched_channel == 2U);
    assert(r.devices.psg.tone_period[2] == UINT16_C(0x0007));
  }

  /* ---- Noise-control LATCH byte (channel 3) and a following DATA byte. ----
     Noise latch: %1 11 0 dddd, documented set $E0-$E7. $E5 => bits: feedback
     mode 1 (white), shift rate 1. */
  {
    GenesisRuntime r = {0};
    psg_write(&r, 0xE5U);
    assert(r.devices.psg.latched_channel == 3U);
    assert(r.devices.psg.latched_volume == 0U);
    assert(r.devices.psg.noise_control == 0x05U);
    /* A DATA byte re-writes the 3-bit noise register (low 3 bits). */
    psg_write(&r, 0x02U);
    assert(r.devices.psg.noise_control == 0x02U);
  }

  /* ---- Determinism: a mixed write sequence run twice over two runtimes
     yields byte-identical device state. ---- */
  {
    GenesisRuntime a = {0};
    GenesisRuntime b = {0};
    static const uint8_t sequence[] = {
      0x9FU, 0xBFU, 0xDFU, 0xFFU, /* silence all four channels (standard mute) */
      0x84U, 0x2CU,               /* channel 0 tone: low nibble 4, then high 6 bits 0x2C */
      0xC1U, 0x08U,               /* channel 2 tone: low nibble 1, then high bits 0x08 */
      0xE7U,                      /* noise: white, tone2 rate */
      0x90U, 0x0AU,               /* channel 0 volume latch 0, then DATA atten 0x0A */
    };
    int run;
    size_t i;
    for (run = 0; run < 2; ++run) {
      GenesisRuntime *r = (run == 0) ? &a : &b;
      for (i = 0U; i < sizeof(sequence) / sizeof(sequence[0]); ++i)
        psg_write(r, sequence[i]);
    }
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    assert(a.devices.psg.latch_valid == 1U);
  }

  /* ---- Adversarial: every excluded neighbour fails closed BEFORE any
     mutation, against an all-zero runtime left byte-identical to `zeroed`. ---- */
  {
    GenesisRuntime rej = {0};
    /* A PSG-port READ (write-only device): BYTE reaches the PSG lane's own
       direction check. */
    reject_psg(&rej, &zeroed, PSG_PORT, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
    /* The reserved noise LATCH encodings: channel 3, tone/noise type, data bit
       3 set ($E8 = %1 11 0 1000, $EF = %1 11 0 1111) -- outside the documented
       $E0-$E7 set -- plus a bare DATA byte with no prior LATCH. */
    {
      static const uint8_t reserved[] = {0xE8U, 0xEFU};
      size_t i;
      for (i = 0U; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        GenesisRuntimeStop s = {0};
        uint32_t v = reserved[i];
        assert(genesis_route_access(&rej, PSG_PORT, GENESIS_ACCESS_BYTE,
                                     GENESIS_ACCESS_WRITE, &v, &s) == GENESIS_ACCESS_FAIL);
        assert(s.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
        assert(v == reserved[i]);
        assert(memcmp(&rej, &zeroed, sizeof(rej)) == 0);
      }
    }
    /* A DATA byte (bit 7 clear) with no prior LATCH. */
    {
      GenesisRuntimeStop s = {0};
      uint32_t v = 0x3FU;
      assert(genesis_route_access(&rej, PSG_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &s) == GENESIS_ACCESS_FAIL);
      assert(s.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
      assert(v == 0x3FU);
      assert(memcmp(&rej, &zeroed, sizeof(rej)) == 0);
    }
  }

  /* ---- Adversarial: WORD and LONG width at the PSG port. The odd address
     $C00011 is rejected by genesis_route_access's odd-effective-address guard
     BEFORE the PSG lane -- a pre-existing fail-close, NOT the PSG diagnostic. ---- */
  {
    GenesisRuntime rej = {0};
    reject_preexisting(&rej, &zeroed, PSG_PORT, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    reject_preexisting(&rej, &zeroed, PSG_PORT, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    /* A PSG-port READ at WORD width hits the same odd-address guard. */
    reject_preexisting(&rej, &zeroed, PSG_PORT, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ,
                       GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  }

  /* ---- Adversarial: wrong address just outside the tight interval keeps the
     PRE-EXISTING fail-close (the co-located VDP lane), NOT the PSG diagnostic. ---- */
  {
    GenesisRuntime rej = {0};
    /* $C00010 (even neighbour, inside the VDP window) -> VDP lane fail-close. */
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00C00010), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    /* $C00012 (even neighbour, inside the VDP window) -> VDP lane fail-close. */
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00C00012), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    /* $C00013 (the first secondarily-attested odd mirror, deliberately NOT
       recognised) -> VDP lane fail-close, not PSG. */
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00C00013), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    /* $C00020 (just past the VDP window) -> generic unmapped fail-close. */
    reject_preexisting(&rej, &zeroed, UINT32_C(0x00C00020), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  }

  /* ---- Adversarial with a pre-populated latch: rejection still mutates
     nothing. ---- */
  {
    GenesisRuntime rej = {0};
    GenesisRuntime before;
    rej.devices.psg.latch_valid = 1U;
    rej.devices.psg.latched_channel = 2U;
    rej.devices.psg.tone_period[2] = UINT16_C(0x01F5);
    rej.devices.psg.attenuation[3] = 0x07U;
    rej.devices.psg.noise_control = 0x04U;
    before = rej;
    reject_psg(&rej, &before, PSG_PORT, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);
    reject_preexisting(&rej, &before, PSG_PORT, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE,
                       GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    {
      GenesisRuntimeStop s = {0};
      uint32_t v = 0xE8U; /* reserved noise encoding */
      assert(genesis_route_access(&rej, PSG_PORT, GENESIS_ACCESS_BYTE,
                                   GENESIS_ACCESS_WRITE, &v, &s) == GENESIS_ACCESS_FAIL);
      assert(memcmp(&rej, &before, sizeof(rej)) == 0);
    }
  }

  /* ---- Regression: the co-located VDP CONTROL-port selector is unaffected. ---- */
  {
    GenesisRuntime clean = {0};
    value = UINT32_C(0xFFFFFFFF);
    assert(genesis_route_access(&clean, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                                 GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
    assert(value == 0U);
    assert(clean.devices.psg.latch_valid == 0U);
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
    ran = subprocess.run([str(directory / "runtime-psg")], text=True,
                         capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime PSG (SN76489) audio-port routing: ok")


if __name__ == "__main__":
  main()
