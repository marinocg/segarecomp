#!/usr/bin/env python3
"""SEG-009-T004: SMS VDP differential against both pinned machine references (ADR 0062) and U2/U9/U11 classification.

usage: sms_vdp_oracle_test.py <sms_image_emitter> <cc> <product-root>

The project-authored VDP fixtures (tools/sms_fixture_rom.py) run black-box through each finalist's libretro core
(tests/sms_oracle/libretro_host.c, RAM result blocks) and through the generated-native platform (headless driver with the
T004 VDP), and the observed results are compared:
  * vdp_seq_a / vdp_seq_b: generated port-operation sequences; every read result, the VRAM read-back checksum. Status bits
    7-5 are masked: bit 6/5 are the sprite flags (T005) and bit 7 is timing-dependent in a run-from-reset sequence, both
    checked by the interrupt fixtures below;
  * vdp_irq: handler-observed V counter and status per phase (frame flag line, line counter period and reload, enable
    gating, pending-but-disabled, acknowledge), status masked to bits 7 and 4-0;
  * vdp_straddle_write (U11, VDP part): an R10 write straddling the next line's counter reload: exact agreement with both
    references at the 4-T resolution (the line-counter underflow/reload is at T offset 0 of its line in all three);
  * vdp_straddle (U11, VDP part): where the status frame flag / V counter flip relative to a line interrupt dispatched at
    line 192. Both references agree with each other and order the flip 3 NOP steps (12 T) before the platform, which keeps
    every in-line event at T offset 0 (U2): CLASSIFIED and bounded (0 <= shift <= 4 steps), not silently accepted;
  * vdp_reset_probe (U9): the observable post-reset VDP state. Genesis Plus GX powers on with a pending frame flag and the
    sprite-overflow flag (first status byte $DF, time origin/T005), Gearsystem and the platform with none; everything
    else matches.
Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts (references are never run in CI).
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "sms_oracle"))
import pins  # noqa: E402
import sms_fixture_rom as builder  # noqa: E402
import sms_oracle_smoke_test as smoke  # noqa: E402
import sms_vdp_model as model  # noqa: E402
from sms_vdp_native import Native, write_straddle_flip  # noqa: E402

FRAMES = {"vdp_seq_a": "60", "vdp_seq_b": "60", "vdp_irq": "60", "vdp_straddle": "200", "vdp_straddle_write": "60", "vdp_reset_probe": "120"}
FAILED, CLASSIFIED = [], []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def reference_ram(result):
    """RAM $C100-$C7FF of a reference run, indexed from $C100."""
    return bytes.fromhex(result["results"]) + bytes.fromhex(result["ram_ext"])


def flips(ram):
    steps = builder.VDP_STRADDLE_STEPS
    return (next((i for i in range(steps) if ram[i] >> 7), None), next((i for i in range(steps) if ram[0x40 + i] == 0xC1), None))


def main():
    if os.name != "posix":
        print("skipped: the libretro host needs POSIX dlopen")
        return 0
    root = pins.checkout(smoke.NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(smoke.NAMES))
        return 0
    names = list(FRAMES)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        host = str(tmp / "libretro_host")
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1",
                        str(HERE / "sms_oracle" / "libretro_host.c"), "-o", host, "-ldl"], check=True)
        roms = tmp / "roms"
        args = []
        for n in names:
            args += ["--fixture", n]
        check(builder.main(["--out", str(roms), *args]) == 0, "fixture builder failed")
        native = Native(EMITTER, CC, ROOT, tmp)
        runs = {}
        for n in names:
            exe = native.build(roms, n)
            if exe is None:
                print("\n".join(native.failures))
                return 1
            runs[n] = native.execute(exe, n, "--frames", FRAMES[n])
        ref = {}
        for emu, spec in smoke.FINALISTS.items():
            core = smoke.build_core(root, spec, tmp)
            for n in names:
                first = smoke.run(host, core, str(roms / (n + ".sms")), spec["options"])
                second = smoke.run(host, core, str(roms / (n + ".sms")), spec["options"])
                check(first == second and first["complete"], "%s/%s: reference run not deterministic or incomplete" % (emu, n))
                ref[(emu, n)] = reference_ram(first)

        for emu in smoke.FINALISTS:
            # ---- port/state sequences ----
            for n, seed in builder.VDP_SEQ_SEEDS.items():
                reads, kinds, (hl, bc), _ = model.run_sequence(seed)
                ram = ref[(emu, n)]  # indexed from $C100
                got = [ram[0x100 + i] for i in range(len(reads))]
                masked = [(v & 0x1F) if k == "status" else v for v, k in zip(got, kinds)]
                nat = runs[n]["ram"]
                nat_masked = [(nat[0x200 + i] & 0x1F) if k == "status" else nat[0x200 + i] for i, k in enumerate(kinds)]
                first = next((i for i, (a, b) in enumerate(zip(masked, nat_masked)) if a != b), None)
                check(masked == nat_masked, "%s/%s: read %s differs from the platform" % (emu, n, first))
                check((ram[0xF0] | ram[0xF1] << 8, ram[0xF2] | ram[0xF3] << 8) == (hl, bc),
                      "%s/%s: VRAM read-back checksum differs from the model/platform" % (emu, n))
                check(ram[0xFF] == 0xA5, "%s/%s: program did not complete" % (emu, n))
            # ---- interrupt phases ----
            nat = runs["vdp_irq"]["ram"]
            ref_low = ref[(emu, "vdp_irq")]
            nat_b = [nat[0xE0 + i] for i in range(9)]  # phase boundaries counted by the platform run
            # $C0E0.. is below the dumped window ($C100..): the platform's boundaries apply (the same program counts the
            # same interrupts in every implementation; a divergence shows up in the lists below)
            total = nat_b[8]
            nat_v = [nat[0x100 + i] for i in range(total)]
            nat_s = [nat[0x200 + i] & 0x9F for i in range(total)]
            ref_v = [ref_low[i] for i in range(total)]
            ref_s = [ref_low[0x100 + i] & 0x9F for i in range(total)]
            check(ref_v == nat_v, "%s/vdp_irq: V counters differ: %r vs %r" % (emu, ref_v[:40], nat_v[:40]))
            check(ref_s == nat_s, "%s/vdp_irq: status bytes differ" % emu)
            # ---- reset probe (U9) ----
            r, nr = ref[(emu, "vdp_reset_probe")], runs["vdp_reset_probe"]["ram"]
            check(r[0] == nr[0x100] == 0 and list(r[2:10]) == [0] * 8, "%s/vdp_reset_probe: buffer/VRAM reads" % emu)
            check((r[1] & 0x1F) == 0x1F == nr[0x101], "%s/vdp_reset_probe: status low bits" % emu)
            check(r[0x0A] == 0 == nr[0x10A] and r[0x0B] != 0 and nr[0x10B] != 0,
                  "%s/vdp_reset_probe: interrupt behaviour with the reset registers" % emu)
            if r[1] != nr[0x101]:
                CLASSIFIED.append("U9 %s: first status byte after reset $%02X (platform $%02X): power-on flags (time origin / T005 sprite flags)" % (emu, r[1], nr[0x101]))
            # ---- straddle (U11 VDP part, U2) ----
            steps = builder.VDP_STRADDLE_STEPS
            r = ref[(emu, "vdp_straddle")]
            n_s = next((i for i in range(steps) if r[i] >> 7), None)
            n_v = next((i for i in range(steps) if r[0x40 + i] == 0xC1), None)
            nat = runs["vdp_straddle"]["ram"]
            p_s = next((i for i in range(steps) if nat[0x100 + i] >> 7), None)
            p_v = next((i for i in range(steps) if nat[0x140 + i] == 0xC1), None)
            check(all((r[i] >> 7) == (1 if i >= n_s else 0) for i in range(steps)) and
                  all(r[0x40 + i] == (0xC1 if i >= n_v else 0xC0) for i in range(steps)), "%s/vdp_straddle: not a clean step" % emu)
            shifts = (p_s - n_s, p_v - n_v) if None not in (n_s, n_v, p_s, p_v) else None
            check(shifts is not None and all(0 <= s <= 4 for s in shifts),
                  "%s/vdp_straddle: flip shift (platform - reference) %r steps is outside the classified 0..4 bound" % (emu, shifts))
            # the R10 write straddle (reload at the next line's underflow): exact agreement at the 4-T resolution
            w_ref, w_clean = write_straddle_flip(ref[(emu, "vdp_straddle_write")], 0)
            w_nat, _ = write_straddle_flip(runs["vdp_straddle_write"]["ram"], 0x100)
            check(w_clean and w_ref == w_nat, "%s/vdp_straddle_write: R10 write flip n = %s (clean %s), platform %s" % (emu, w_ref, w_clean, w_nat))
            CLASSIFIED.append("U11 %s: R10 write vs line reload flips at n = %s NOP steps, platform %s (agreement)" % (emu, w_ref, w_nat))
            CLASSIFIED.append("U2/U11 %s: frame flag/V counter flip at n = (%s, %s) NOP steps after dispatch; platform (%s, %s); shift %s steps (x4 T)"
                              % (emu, n_s, n_v, p_s, p_v, shifts))
        # both references agree with each other on the straddle flip (the U2 rule: adopt a value only if they agree)
        a, b = (flips(ref[(emu, "vdp_straddle")]) for emu in smoke.FINALISTS)
        check(a == b, "the references disagree with each other on the straddle flips: %r vs %r" % (a, b))

        for line in CLASSIFIED:
            print("classified:", line)
        failures = native.failures + FAILED
        for f in failures:
            print("FAIL:", f)
        print("sms vdp oracle: %d failure(s)" % len(failures))
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
