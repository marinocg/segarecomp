#!/usr/bin/env python3
"""SEG-009-T001 (ADR 0062): chip-level adversarial smoke of the pinned PSG references.

Expectations are derived here from SMS Power! "SN76489" (Maxim) and the Genesis Plus GX hardware notes, never from a
reference's own tests. Observation adapters (tests/sms_oracle/psg_observe_*.cpp) are compiled from the pinned
checkouts (read-only; objects go to a temporary directory) and only print what they see.

Each reference runs with a committed deviation mask (ADR 0062). A masked check must still disagree (a reference that
silently starts agreeing makes its mask stale and fails the test), and every unmasked check must agree.

Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts.
usage: sms_psg_oracle_smoke_test.py [c++]
"""
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "sms_oracle"))
import pins  # noqa: E402

NAMES = ["ares-emulator_ares", "drhelius_Gearsystem"]

# Deviations from the contract, each resolved against a citation in ADR 0062. Each entry is (reason, predicate):
# the predicate states the exact documented deviation, so a masked check passes only when the reference deviates in
# precisely that way, and fails as STALE-MASK when the reference starts agreeing with the contract.
def _inverted(bits):
    return "".join("1" if c == "0" else "0" for c in bits)


MASKS = {
    "ares_sn76489": {
        "latch.vol2_after_data": ("ignores a data byte after an attenuation latch (SP-PSG: 'The data byte is NOT "
                                  "ignored', Alex Kidd); new SEG-009-T001 finding, no upstream issue",
                                  lambda o: o["latch"]["vol2_after_data"] == 15),
        "white_rate0": ("outputs the bit shifted off: one shift later than the SP-PSG reference implementation and "
                        "Genesis Plus GX", lambda o: o["white_rate0"][1:] == lfsr_outputs(True, 64)[:-1]),
        "periodic_rate0": ("same one-shift output phase as white_rate0",
                           lambda o: o["periodic_rate0"][1:] == lfsr_outputs(False, 32)[:-1]),
    },
    "blargg_sms_apu": {
        "white_rate0": ("channel on when LFSR bit 0 is 0 (inverted polarity; SP-PSG 'Output inversion')",
                        lambda o: _inverted(o["white_rate0"]) == lfsr_outputs(True, 64)),
        "periodic_rate0": ("same inverted polarity as white_rate0",
                           lambda o: _inverted(o["periodic_rate0"]) == lfsr_outputs(False, 32)),
        "tone_period0": ("holds a period-0 tone static instead of behaving as period 1 (GPGX-NOTE)",
                         lambda o: len(set(o["tone_period0"])) == 1),
    },
}


def lfsr_outputs(white, count):
    """SP-PSG reference implementation: 16-bit register, taps $0009 into bit 15, output = bit 0 after the shift."""
    register, out = 0x8000, []
    for _ in range(count):
        feedback = bin(register & 0x0009).count("1") & 1 if white else register & 1
        register = (register >> 1) | (feedback << 15)
        out.append(str(register & 1))
    return "".join(out)


def alternates(bits):
    return len(bits) >= 4 and all(bits[i] != bits[i + 1] for i in range(len(bits) - 1))


def expectations():
    """(key, predicate(observation), source)."""
    return [
        ("reset.tone", lambda o: o["reset"]["tone"] == [0, 0, 0], "SP-PSG: Sega integrated PSG starts with zero tone registers"),
        ("reset.volume", lambda o: o["reset"].get("volume", o["reset"].get("volume_reg")) == [15, 15, 15, 15],
         "SP-PSG: ... and all attenuations $F (silence)"),
        ("reset.lfsr", lambda o: o["reset"]["lfsr"] == 0x8000, "GPGX-NOTE: power-on LFSR holds only the highest bit (SP-PSG: after a noise write)"),
        ("latch.tone0_after_latch", lambda o: o["latch"]["tone0_after_latch"] == 0x00E, "SP-PSG: latch writes the low 4 bits immediately"),
        ("latch.tone0_after_data", lambda o: o["latch"]["tone0_after_data"] == 0x0FE, "SP-PSG: data byte writes the high 6 bits (440 Hz example)"),
        ("latch.vol2_after_latch", lambda o: o["latch"]["vol2_after_latch"] == 15, "SP-PSG: attenuation latch"),
        ("latch.vol2_after_data", lambda o: o["latch"]["vol2_after_data"] == 0, "SP-PSG: a data byte after an attenuation latch updates it (Alex Kidd)"),
        ("latch.noise_after_latch", lambda o: o["latch"]["noise_after_latch"] == [1, 1], "SP-PSG: noise latch %101 = white, rate 1"),
        ("latch.noise_after_data", lambda o: o["latch"]["noise_after_data"] == [0, 1], "SP-PSG: data byte updates the noise register (Micro Machines)"),
        ("latch.lfsr_after_noise_write", lambda o: o["latch"]["lfsr_after_noise_write"] == 0x8000, "SP-PSG: any noise write resets the LFSR"),
        ("white_rate0", lambda o: o["white_rate0"] == lfsr_outputs(True, 64), "SP-PSG: 16-bit LFSR, taps bits 0 and 3; output bit 0 after the shift (SP-PSG code; phase open U4), 1 = on"),
        ("periodic_rate0", lambda o: o["periodic_rate0"] == lfsr_outputs(False, 32), "SP-PSG: 'periodic' noise = 1/16 duty"),
        ("noise_shift_interval", lambda o: o["noise_shift_interval"] == [32, 64, 128, 10], "SP-PSG: reload $10/$20/$40/tone 2; one shift per two expiries"),
        ("tone_period1", lambda o: alternates(o["tone_period1"]), "SP-PSG tone formula/range ($001 = 111,861 Hz); SP-PSG also says constant +1 for 0/1: open U10"),
        ("tone_period0", lambda o: alternates(o["tone_period0"]), "GPGX-NOTE: period 0 behaves as period 1 (contract project decision, open U10)"),
        ("tone_period3", lambda o: o["tone_period3"] in ("111000111000", "000111000111"), "SP-PSG: output flips when the counter reloads"),
        ("attenuation_steps", attenuation_ok, "SP-PSG: 2 dB per step, $F = silence"),
    ]


def attenuation_ok(o):
    levels = o.get("attenuation_levels")
    if levels is None:  # ares' component reports the nibble; its mixer table is outside the component
        return o.get("attenuation_is_nibble") is True
    if levels[15] != 0:
        return False
    steps = [20 * math.log10(levels[i + 1] / levels[i]) for i in range(8)]
    return all(-2.6 <= s <= -1.4 for s in steps)


def build(compiler, root, tmp):
    ares = root / "ares-emulator_ares"
    gs = root / "drhelius_Gearsystem" / "src"
    shim = pathlib.Path(tmp) / "shim" / "ares"
    shim.mkdir(parents=True)
    # Include-path adapter only: pulls the real nall/ares type headers the component needs, without the whole
    # emulator (libco, sljit, vfs). No declaration of the component is replaced.
    (shim / "ares.hpp").write_text(
        "#pragma once\n#include <nall/platform.hpp>\n#include <nall/memory.hpp>\n#include <nall/primitives.hpp>\n"
        "#include <nall/array.hpp>\n#include <nall/serializer.hpp>\nusing namespace nall;\nusing namespace nall::primitives;\n"
        "#include \"%s\"\n" % (ares / "ares" / "ares" / "types.hpp").as_posix(), encoding="utf-8")
    exes = {}
    exes["ares_sn76489"] = str(pathlib.Path(tmp) / "psg_ares")
    subprocess.run([compiler, "-std=c++20", "-w", "-I%s" % shim.parent, "-I%s" % (ares / "ares"), "-I%s" % (ares / "nall"),
                    str(HERE / "sms_oracle" / "psg_observe_ares.cpp"),
                    str(ares / "ares" / "component" / "audio" / "sn76489" / "sn76489.cpp"), "-o", exes["ares_sn76489"]],
                   check=True)
    exes["blargg_sms_apu"] = str(pathlib.Path(tmp) / "psg_blargg")
    subprocess.run([compiler, "-std=c++17", "-w", "-I%s" % (gs / "audio"), "-I%s" % gs,
                    str(HERE / "sms_oracle" / "psg_observe_blargg.cpp"), str(gs / "audio" / "Sms_Apu.cpp"),
                    str(gs / "audio" / "Blip_Buffer.cpp"), "-o", exes["blargg_sms_apu"]], check=True)
    return exes


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "c++"
    if os.name != "posix" or pathlib.Path(compiler).stem.lower() in ("cl", "clang-cl"):
        print("skipped: the PSG observation adapters need a POSIX GCC/Clang-style compiler")
        return 0
    root = pins.checkout(NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(NAMES))
        return 0
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        exes = build(compiler, root, tmp)
        observations = {}
        for name, exe in exes.items():
            runs = [subprocess.run([exe], text=True, capture_output=True, timeout=60, check=True).stdout for _ in range(2)]
            if runs[0] != runs[1]:
                print("FAIL %s: observations are not deterministic" % name)
                failures += 1
            observations[name] = json.loads(runs[0])
        for key, predicate, source in expectations():
            cells = []
            for name, obs in observations.items():
                agrees = bool(predicate(obs))
                mask = MASKS[name].get(key)
                if mask and agrees:
                    cells.append("STALE-MASK")
                    failures += 1
                elif mask and not mask[1](obs):
                    cells.append("FAIL(mask)")
                    failures += 1
                elif mask:
                    cells.append("masked")
                elif agrees:
                    cells.append("pass")
                else:
                    cells.append("FAIL")
                    failures += 1
            print("%-24s %-14s %-14s %s" % (key, cells[0], cells[1], source))
    print("sms psg smoke: %d checks x %d references, %d masked, %d failures" % (
        len(expectations()), len(observations), sum(len(m) for m in MASKS.values()), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
