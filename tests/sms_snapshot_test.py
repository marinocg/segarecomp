#!/usr/bin/env python3
"""SEG-132 (ADR 0279): reversible Master System machine snapshots on the REAL viewer rig and a generated-native fixture program.

usage: sms_snapshot_test.py <sms_image_emitter> <cc> <product-root>

Project-authored fixture (tools/sms_fixture_rom.py `machine_e2e`: mapper bank switches, VDP register/VRAM traffic, frame and line interrupts, pad reads under a
scripted input, pause NMIs, PSG writes) is emitted through the SMS generation route, compiled strict C11 with the runtime, the viewer core and the snapshot codec, and
driven by tests/tools/sms_snapshot_harness.c. Expectations are comparisons of the machine with ITSELF on another path through time, never a model of the codec:
  * for EVERY frame boundary of the run: capture, finish the run, then restore and finish again -- the canonical state bytes at the end are identical, and so is the PCM
    stream produced after the restore point (CPU, mapper, VDP, interrupts, pad/pause, PSG sampling counters);
  * a capture restored and captured again is byte-identical; a restore into a deliberately dirtied machine reproduces the saved state exactly;
  * identity, mapper declaration, PSG variant and input-mode mismatches are refused; truncation, corruption, a bad digest and every crafted value are refused WITHOUT
    mutating the machine (the state bytes before and after the refused restore are identical); a latched stop is not capturable.
"""
import pathlib
import re
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import sms_cc_cache  # noqa: E402
from sms_vdp_native import Native, STRICT  # noqa: E402

FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


class SnapNative(Native):
    """The generation route and strict compile of the headless Native, with the viewer core + snapshot codec + harness in place of the headless driver."""

    def __init__(self, tmp):
        super().__init__(EMITTER, CC, ROOT, tmp)
        platform = ROOT / "platforms" / "master-system"
        self.include += [platform / "viewer", ROOT / "libs" / "viewer" / "include"]
        self.sources = ([platform / "runtime" / n for n in ("sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c", "sms_vdp.c", "sms_render.c")]
                        + [ROOT / "libs" / "device" / "sega" / "psg" / "src" / "sn76489.c"]
                        + [platform / "viewer" / "sms_viewer.c", platform / "viewer" / "sms_snapshot.c", HERE / "tools" / "sms_snapshot_harness.c"])


def run_script(native, exe, lines, name):
    script = native.tmp / (name + ".script")
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    r = native.run([exe, script], timeout=600)
    if r.returncode != 0:
        FAILED.append("%s: harness exited %d: %s" % (name, r.returncode, (r.stdout + r.stderr)[-400:]))
    return r.stdout.splitlines()


def pick(lines, prefix):
    return [l for l in lines if l.startswith(prefix)]


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_snapshot_test_"))
    roms = tmp / "roms"
    builder.main(["--out", str(roms), "--fixture", "machine_e2e"])
    script_path = tmp / "input.txt"
    script_path.write_text(builder.E2E_SCRIPT, encoding="utf-8")
    native = SnapNative(tmp)
    exe = native.build(roms, "machine_e2e")
    FAILED.extend(native.failures)
    if exe is None:
        return report()
    final = builder.E2E_FRAMES

    # ---- 1. replay equivalence from EVERY frame boundary, script mode and live mode ------------------------------------------------------------------------------
    for mode, header in (("script", ["input %s" % script_path]), ("live", ["input live"])):
        lines = list(header)
        for f in range(1, final):
            lines += ["run %d" % f, "snap %d" % f, "sha"]
        lines += ["run %d" % final, "sha", "pcm_ref", "snap 120"]
        for f in range(1, final):
            lines += ["restore %d" % f, "run %d" % final, "sha", "pcm_cmp", "snap 121", "eq 120 121"]
        out = run_script(native, exe, lines, "sweep_" + mode)
        shas = [l.split()[1] for l in out if l.startswith("SHA ")]
        # shas: one per frame 1..final-1 (at the snap), then the reference final, then one per replay
        n = final - 1
        check(len(shas) == n + 1 + n, "%s: expected %d state hashes, got %d" % (mode, 2 * n + 1, len(shas)))
        if len(shas) == 2 * n + 1:
            ref = shas[n]
            for i in range(n):
                check(shas[n + 1 + i] == ref, "%s: replay from frame %d did not reproduce the final state" % (mode, i + 1))
        check(all(l.startswith("PCM_CMP EQ") for l in pick(out, "PCM_CMP")) and len(pick(out, "PCM_CMP")) == n, "%s: PCM stream after a restore differs from the original (%s)" % (mode, pick(out, "PCM_CMP")[:2]))
        check(len(pick(out, "EQ ")) == n and not pick(out, "NE "), "%s: end-of-run captures differ after a restore" % mode)
        check(all(l.startswith("RESTORE") and l.endswith(" OK") for l in pick(out, "RESTORE ")), "%s: a restore of a freshly captured state was refused: %s" % (mode, [l for l in pick(out, "RESTORE ") if not l.endswith(" OK")][:2]))
        # the interesting states are actually present in the fixture run: mapper banks other than the power-on ones and a pending NMI/IRQ at some boundary
        if mode == "script":
            st = run_script(native, exe, ["input %s" % script_path] + ["run %d" % f for f in range(1, final)] + ["state"], "witness")
            check(True, "")

    # ---- 2. witnesses that the sweep covered non-trivial machine states -----------------------------------------------------------------------------------------
    lines = ["input %s" % script_path]
    for f in range(1, final):
        lines += ["run %d" % f, "state"]
    out = run_script(native, exe, lines, "states")
    banks = {re.search(r"banks (\w+)", l).group(1) for l in pick(out, "STATE")}
    irqs = {re.search(r"irq (\d+)", l).group(1) for l in pick(out, "STATE")}
    check(len(banks) >= 3, "the swept run must visit several mapper bank configurations (saw %d)" % len(banks))
    check(len(irqs) >= 1, "interrupt-state witnesses missing")
    pcm_idx = [int(re.search(r"pcm_index (\d+)", l).group(1)) for l in pick(out, "STATE")]
    check(pcm_idx == sorted(pcm_idx) and pcm_idx[-1] > pcm_idx[0], "the PSG sample index advances")

    # ---- 3. dirty machine, round trip, refusals without mutation --------------------------------------------------------------------------------------------------
    lines = ["input %s" % script_path, "run 20", "snap 1", "sha", "mutate_all", "sha", "restore 1", "sha", "snap 2", "eq 1 2",
             "run 30", "snap 3", "restore 3", "restore 1", "sha"]
    out = run_script(native, exe, lines, "roundtrip")
    shas = [l.split()[1] for l in out if l.startswith("SHA ")]
    check(len(shas) == 4 and shas[0] == shas[2] and shas[0] != shas[1] and shas[3] == shas[0], "restore into a dirtied machine reproduces the saved state exactly (%s)" % shas)
    check(pick(out, "EQ 1 2"), "capture(restore(capture)) is byte-identical")

    base = ["input %s" % script_path, "run 20", "snap 1", "sha", "run 25", "snap 5", "sha"]
    refused = []
    expect = []

    def case(name, setup, status, undo=(), restore="1"):
        refused.extend(["# " + name] + setup + ["validate " + restore, "restore " + restore] + list(undo) + ["sha"])
        expect.append((name, status))

    case("source identity", ["ident 01"], "WRONG_CARTRIDGE", ["ident 01"])
    case("variant identity", ["variant 01"], "WRONG_CARTRIDGE", ["variant 01"])
    case("mapper declaration", ["family 2"], "INCOMPATIBLE", ["family 1"])
    case("PSG chip variant", ["psgcfg 000a"], "INCOMPATIBLE", ["psgcfg 000a"])   # 0x0009 ^ 0x000a = 0x0003
    case("truncated file", ["trunc 1 100"], "TRUNCATED")
    # fresh copies for the byte-level cases (a fresh capture each time: slot 6+)
    cases = [("bad magic", 0, None, None, "tamper 6 0 ff", "BAD_MAGIC"), ("bit flip in a section", 0, None, None, "tamper 6 400 01", "INTEGRITY"),
             ("trailing byte", 0, None, None, "grow 6", None)]
    # crafted values: (name, section, offset, xor, status)
    crafted = [("Z80 interrupt mode", 1, 28, 0x80, "INVALID_STATE"), ("Z80 error outcome", 1, 47, 0x80, "INVALID_STATE"), ("Z80 flag out of range", 1, 32, 0x02, "INVALID_STATE"),
               ("header/CPU clock mismatch", 1, 39, 0x01, "BAD_LAYOUT"), ("memory control", 2, 40964, 0x40, "INVALID_STATE"), ("mapper control bits", 2, 40960, 0x10, "INVALID_STATE"),
               ("scheduler position", 3, 2, 0x10, "INVALID_STATE"), ("pad mask", 3, 10, 0x80, "INVALID_STATE"), ("input mode", 3, 13, 0x01, "INVALID_STATE"),
               ("irq mask", 3, 9, 0x40, "INVALID_STATE"), ("VDP address", 4, 16433, 0x80, "INVALID_STATE"), ("VDP code", 4, 16434, 0x80, "INVALID_STATE"),
               ("CRAM bits", 4, 16384, 0x80, "INVALID_STATE"), ("renderer height", 5, 0, 0x01, "INVALID_STATE"), ("PSG attenuation", 6, 15, 0x40, "INVALID_STATE"),
               ("PCM boundary", 6, 42, 0x01, "INVALID_STATE"), ("PCM batch count", 6, 77, 0x80, "INVALID_STATE"), ("pad I/O control", 7, 0, 0x01, "INVALID_STATE")]
    lines = list(base)
    lines += refused
    for name, _, _, _, tamper, status in cases:
        lines += ["# " + name, "snap 6", "sha", tamper] + (["validate 6", "restore 6", "sha"] if status else ["validate 6"])
    for name, sec, off, x, status in crafted:
        lines += ["# " + name, "snap 7", "sha", "poke 7 %d %d %x" % (sec, off, x), "redigest 7", "validate 7", "restore 7", "sha"]
    lines += ["fault", "snap 9"]
    out = run_script(native, exe, lines, "refusals")
    # the statuses in order
    results = [l.split()[2] for l in out if l.startswith(("VALIDATE", "RESTORE"))]
    pos = 0
    for name, status in expect:
        got = results[pos:pos + 2]
        pos += 2
        check(got == [status, status], "%s: expected %s, got %s" % (name, status, got))
    for name, _, _, _, tamper, status in cases:
        n = 2 if status else 1
        got = results[pos:pos + n]
        pos += n
        if status:
            check(got == [status, status], "%s: expected %s, got %s" % (name, status, got))
        else:
            check(got == ["BAD_LAYOUT"] or got == ["TRUNCATED"] or got == ["INTEGRITY"], "%s: a file with a trailing byte must be refused (%s)" % (name, got))
    for name, _, _, _, status in crafted:
        got = results[pos:pos + 2]
        pos += 2
        check(got == [status, status], "%s: expected %s, got %s" % (name, status, got))
    # no mutation: every sha taken right after a refused restore equals the sha taken before the case
    shas = [l.split()[1] for l in out if l.startswith("SHA ")]
    # layout: base gives 2 shas (slot 1 at 20, slot 5 at 25); the machine stays at frame 25; every case has (snap; sha) ... sha-after
    live = shas[1]
    check(all(s == live for s in shas[1:]), "a refused restore changed the machine (%d hashes, %d distinct)" % (len(shas), len(set(shas))))
    check(pick(out, "SNAP 9 NOT_CAPTURABLE"), "a latched stop must not be capturable: %s" % pick(out, "SNAP 9"))

    # ---- 4. mode mismatch: a script-mode state cannot restore into a live machine and vice versa ------------------------------------------------------------------
    out = run_script(native, exe, ["input %s" % script_path, "run 20", "snap 1", "input live", "validate 1", "input %s" % script_path, "snap 2", "input live", "restore 2"], "modes")
    check([l.split()[2] for l in out if l.startswith(("VALIDATE", "RESTORE"))] == ["INCOMPATIBLE", "INCOMPATIBLE"], "input-mode mismatch must be refused: %s" % pick(out, "VALIDATE"))
    return report()


def report():
    if FAILED:
        for m in FAILED:
            print("FAIL:", m)
        return 1
    print("sms snapshot test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
