#!/usr/bin/env python3
"""SEG-132 (ADR 0279) Checkpoint E/G: reversible Sega CD machine snapshots on a generated-native program, hermetic.

usage: sega_cd_snapshot_test.py <segarecomp> <c-compiler> <source-root>

Project-authored fixtures (tools/sega_cd_cdda_fixture.py: a BIOS-like main program that releases the sub CPU, whose program drives the CD drive through the real Gate Array command
registers -- play, pause, resume, a play running through a pregap, seek, play, stop -- over a disc of closed-form audio tracks; tools/sega_cd_bios_hw_fixture.py: the main-side
hardware, VDP DMA from RAM and Word RAM, interrupts) are built into viewer-variant programs linked with tests/tools/sega_cd_snapshot_host.c instead of the SDL3 viewer: the SAME live
driver loop, the same codec units, a plan file as the "player". At every frame barrier the host logs the SHA-256 of a fresh canonical capture (both processors, all RAM, Gate Array, CDC,
CDD, PCM, CD-DA and mixer clocks, graphics ASIC, backup RAM, input, overlay/dynamic state, the VDP/I-O host, a frame in progress), every presented frame's digest, the digest of the audio
delivered in each interval and the digests of the in-memory backup RAM and the main work RAM. Expectations compare the machine with ITSELF on another path through time:
  * snapshot/restore windows across the whole run (the guest repeatedly goes back and forwards): for every frame key the state, the presented picture, the delivered audio and the RAM
    digests are identical in every execution of that frame, and equal to a reference run that never restored;
  * a state saved in one process loads in another and reproduces the reference run from that frame on;
  * wrong disc, wrong BIOS, another executable-image contract, a damaged / truncated / crafted file are refused and the machine is byte-identical afterwards.
"""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
HOST = ROOT / "tests" / "tools" / "sega_cd_snapshot_host.c"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 400_000_000
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def build(tmp, fixture, name, frames_pass=3):
    out = tmp / name
    subprocess.run([sys.executable, str(ROOT / "tools" / fixture), "build", "--out", str(out / "fx")], check=True, capture_output=True, text=True)
    if not (out / "fx" / "disc").exists():
        import sega_cd_common as common
        common.build_disc(out / "fx" / "disc", "comm")
    built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(out / "fx" / "disc" / "disc.cue"), "--bios", str(out / "fx" / "bios.bin"),
                            "--work", str(out / "work"), "--pass-set", "main:d6=%d" % frames_pass, "--budget", "60000000", "--timeout", "900", "--viewer-host-source", str(HOST)],
                           capture_output=True, text=True, timeout=3000)
    summary = json.loads(built.stdout) if built.stdout.strip() else {}
    check(built.returncode == 0 and summary.get("ok"), "%s: the program builds with its test-host viewer variant" % name + ("" if summary.get("ok") else ": %s" % (built.stdout[-400:] + built.stderr[-400:])))
    if not summary.get("ok"):
        return None
    return {"exe": pathlib.Path(summary["viewer"]["program"]), "disc": out / "fx" / "disc", "bios": out / "fx" / "bios.bin", "dir": out, "args": ["--instruction-budget", str(BUDGET)],
            "set": "main:d6=%d"}


def build_z80_audio(tmp):
    """The production configuration: the main-side Z80 with the YM2612 / PSG / mixer (hardware fixture case 18: the Z80 drives the sound chips), built by the product `sega-cd build`."""
    import sega_cd_common as common
    out = tmp / "hwz80"
    subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_hw_fixture.py"), "build", "--out", str(out / "fx")], check=True, capture_output=True, text=True)
    common.build_disc(out / "disc", "comm")
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(out / "disc" / "disc.cue"), "--bios", str(out / "fx" / "bios.bin"), "--output", str(out / "out"), "--runtime-dir", str(ROOT),
                           "--cc", CC, "--budget", "3000000", "--jobs", "4", "--cache-dir", str(out / "cache"), "--pass-set", "main:d7=18", "--z80-audio", "1",
                           "--emit-arg", "--application-entry", "--emit-arg", "0xFF0000", "--emit-arg", "--copy-image", "--emit-arg", "main:0xFF0000:0x1780:4",
                           "--viewer", "1", "--viewer-host-source", str(HOST)], capture_output=True, text=True, timeout=3000)
    check(done.returncode == 0 and "@result ok" in done.stdout, "z80+audio: the production configuration builds with the test-host viewer variant" + ("" if done.returncode == 0 else ": " + (done.stdout + done.stderr)[-400:]))
    if done.returncode != 0:
        return None
    return {"exe": out / "out" / "program-viewer", "disc": out / "disc", "bios": out / "fx" / "bios.bin", "dir": out, "args": ["--instruction-budget", "3000000"], "set": "main:d7=18"}


def session(prog, plan, frames, tmp, tag, extra=()):
    plan_file, log = tmp / (tag + ".plan"), tmp / (tag + ".log")
    plan_file.write_text("".join(line + "\n" for line in plan))
    env = dict(os.environ, SCD_SNAP_PLAN=str(plan_file), SCD_SNAP_LOG=str(log))
    set_arg = prog["set"] % frames if "%d" in prog["set"] else prog["set"]
    r = subprocess.run([str(prog["exe"]), "--disc-dir", str(prog["disc"]), "--bios", str(prog["bios"]), *prog["args"], "--set", set_arg, *extra],
                       capture_output=True, text=True, timeout=900, env=env)
    return r.returncode, parse(log) if log.exists() else None


def parse(log):
    """Keyed observations: H/F/R by frame key (every occurrence, in order), A by the key of its interval, S operations in order."""
    out = {"H": {}, "F": {}, "R": {}, "A": {}, "S": [], "order": []}
    for line in pathlib.Path(log).read_text().splitlines():
        f = line.split()
        if f[0] == "H":
            out["H"].setdefault(int(f[1]), []).append(f[3])
            out["order"].append(int(f[1]))
        elif f[0] == "F":
            out["F"].setdefault(int(f[1]), []).append(f[2])
        elif f[0] == "R":
            out["R"].setdefault(int(f[1]), []).append((f[2], f[3]))
        elif f[0] == "A":
            out["A"].setdefault(int(f[1]), []).append((f[2], f[3]))
        elif f[0] == "S":
            out["S"].append(line)
    return out


def consistent(obs, reference, label, kinds=("H", "F", "R", "A")):
    """Every execution of a frame key (original and replays) agrees with every other and with the reference run that never restored."""
    bad = []
    for kind in kinds:
        for key, values in obs[kind].items():
            ref = reference[kind].get(key)
            if len(set(values)) != 1:
                bad.append((kind, key, "executions differ"))
            elif ref is not None and values[0] != ref[0]:
                bad.append((kind, key, "differs from the reference"))
    check(not bad, "%s: %s" % (label, "every observation equals the reference in every execution" if not bad else "mismatches %s" % bad[:4]))
    return not bad


def sha_fix(data):
    data = bytearray(data)
    data[-32:] = hashlib.sha256(bytes(data[:-32])).digest()
    return bytes(data)


def sections(data):
    count = struct.unpack_from("<I", data, 28)[0]
    out = {}
    for i in range(count):
        sid, flags, off, length = struct.unpack_from("<HHII", data, 156 + 12 * i)
        out[sid] = (off, length)
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="scd-snap-") as tmp:
        tmp = pathlib.Path(tmp)
        prog = build(tmp, "sega_cd_cdda_fixture.py", "cdda")
        if prog is None:
            return 1
        FRAMES = 110
        # ---- the reference: the run that never restores ----
        code, ref = session(prog, ["%d quit" % FRAMES], 400, tmp, "ref")
        check(code == 0 and ref is not None and len(ref["H"]) >= FRAMES, "reference: %d frame barriers observed (exit %d)" % (len(ref["H"]) if ref else 0, code))
        check(ref is not None and len(set(h[0] for h in ref["H"].values())) == len(ref["H"]) and len(ref["A"]) >= 40, "reference: every frame's canonical state is distinct; audio is delivered every interval")
        snap = ref and None

        # ---- snapshot / restore windows across the run, two phases so every frame is a restore point ----
        for phase in (0, 1):
            plan = []
            t = 2 + phase
            while t + 4 < FRAMES - 4:
                plan += ["%d snap 1" % t, "%d restore 1" % (t + 3)]
                t += 6
            plan.append("%d quit" % (FRAMES + 40))
            code, obs = session(prog, plan, 400, tmp, "sweep%d" % phase)
            ok_ops = obs is not None and obs["S"] and all(s.endswith("ok") or " ok " in s for s in obs["S"])
            check(code == 0 and ok_ops, "sweep %d: every snapshot and restore succeeded (%d operations)" % (phase, len(obs["S"]) if obs else 0))
            if obs:
                replays = sum(1 for v in obs["H"].values() if len(v) > 1)
                check(replays >= 30, "sweep %d: %d frames were executed more than once" % (phase, replays))
                consistent(obs, ref, "sweep %d" % phase)
        # ---- a dirty machine: every restorable region corrupted after the snapshot; the restore must bring the exact saved state back ----
        code, dirty = session(prog, ["6 snap 1", "9 dirty", "9 restore 1", "13 snap 2", "15 dirty", "15 restore 2", "%d quit" % FRAMES], 400, tmp, "dirty")
        log_x = [l.split()[1] for l in (tmp / "dirty.log").read_text().splitlines() if l.startswith("X ")]
        check(code == 0 and len(log_x) == 2 and all(x not in ref["H"][k] for x in log_x for k in (6, 13)), "dirty: the corruption changes the machine (%d dirty states)" % len(log_x))
        consistent(dirty, ref, "dirty: restore brings back the exact saved machine (every region) and the run continues as the reference")
        # the state sizes
        code, obs = session(prog, ["3 snap 1", "6 quit"], 400, tmp, "size")
        size = int(next(l for l in obs["S"] if "snap 1" in l).split()[-1])
        print("      measured: one Sega CD snapshot is %d bytes (%.2f MiB)" % (size, size / 1048576.0))
        check(1_000_000 < size < 1_300_000, "snapshot size is about 1.1 MiB (%d bytes)" % size)

        # ---- cross-process: save in one process, load in another ----
        dump = tmp / "state.scd"
        code, a = session(prog, ["12 snap 1", "12 dump 1 %s" % dump, "%d quit" % FRAMES], 400, tmp, "save")
        data = dump.read_bytes()
        check(code == 0 and data[:8] == b"SCDSTAT\x00", "saved: an SCDSTAT container of %d bytes" % len(data))
        code, b = session(prog, ["3 slurp 1 %s" % dump, "4 restore 1", "%d quit" % (FRAMES - 8)], 400, tmp, "load")
        consistent(b, ref, "a separate process loaded the state and reproduced the reference run")
        check(b is not None and 12 in b["H"] and len(b["H"][12]) >= 1 and max(b["H"]) >= FRAMES - 20, "the loading process ran on past the loaded frame")

        # ---- refusals: the machine is byte-identical afterwards ----
        base = data
        sec = sections(base)
        cases = []   # (label, mutated bytes, expected status text fragment)

        def mutate(label, data_bytes, expect):
            cases.append((label, data_bytes, expect))

        def poke(sid, offset, xor, digest=True):
            d = bytearray(base)
            d[sec[sid][0] + offset] ^= xor
            return sha_fix(bytes(d)) if digest else bytes(d)

        def header(offset, xor):
            d = bytearray(base)
            d[offset] ^= xor
            return sha_fix(bytes(d))
        mutate("another disc", header(32, 1), "different disc or BIOS")
        mutate("another BIOS", header(64, 1), "different disc or BIOS")
        mutate("another executable-image contract / sound configuration", header(96, 1), "does not match this build")
        mutate("machine flags", header(12, 1), "does not match this build")
        mutate("bad magic", b"X" + base[1:], "not a save state")
        mutate("truncated", base[:2000], "truncated")
        mutate("bit flip", bytes(bytearray(base[:5000]) + bytes([base[5000] ^ 1]) + base[5001:]), "corrupt")
        mutate("trailing byte", base + b"\0", "damaged")
        mutate("version", header(8, 2), "incompatible version")
        crafted = [("interrupt level", 1, 74, 0x80), ("machine time", 2, 7, 0x40), ("Gate Array bank", 6, 8, 0x80), ("CDC DMA state", 7, 2, 0x80), ("CDD track index", 8, 33, 0x80),
                   ("PCM channel index", 9, 56, 0x08), ("CD-DA fader", 11, 12, 0x40), ("backup RAM presence", 12, 0, 0x01), ("delivery ledger count", 16, 3, 0x40),
                   ("device host block length", 17, 3, 0x40)]
        for label, sid, off, x in crafted:
            mutate("crafted: " + label, poke(sid, off, x), "values the console cannot have")
        plan = ["3 snap 2"]
        files = []
        for i, (label, blob, expect) in enumerate(cases):
            path = tmp / ("case%d.scd" % i)
            path.write_bytes(blob)
            files.append((label, path, expect))
        # one process per case keeps the log readable; the plan: slurp the mutated file, validate then restore it, and the next barrier's hash must be the same machine
        for label, path, expect in files:
            code, obs = session(prog, ["4 slurp 3 %s" % path, "5 validate 3", "5 restore 3", "9 quit"], 400, tmp, "refuse")
            status = [s for s in obs["S"] if " validate 3 " in s or " restore 3 " in s] if obs else []
            texts = [s.split(" ", 4)[4] for s in status]
            ok_status = len(texts) == 2 and all(expect in t for t in texts)
            check(ok_status, "refused (%s): %s" % (label, texts))
            if obs:   # the machine continued exactly like the reference: no mutation by a refused restore
                same = all(obs["H"].get(k, [None])[0] == ref["H"][k][0] for k in obs["H"] if k in ref["H"])
                check(same, "refused (%s): the machine is unchanged (the run continues as the reference)" % label)

        # ---- the snapshot of a different program (the live fixture: another BIOS image) is refused by this one ----
        other = build(tmp, "sega_cd_bios_live_fixture.py", "live")
        if other is not None:
            code, o = session(other, ["3 snap 1", "3 dump 1 %s" % (tmp / "other.scd"), "4 quit"], 400, tmp, "otherdump")
            code, r2 = session(prog, ["4 slurp 3 %s" % (tmp / "other.scd"), "5 restore 3", "7 quit"], 400, tmp, "foreign")
            texts = [s for s in r2["S"] if "restore 3" in s] if r2 else []
            check(texts and ("different disc or BIOS" in texts[0] or "does not match" in texts[0]), "a state of another program (disc / BIOS / image contract) is refused: %s" % texts)

        # ---- the production configuration: the main-side Z80, YM2612, PSG and mixer sections ----
        z = build_z80_audio(tmp)
        if z is not None:
            code, zref = session(z, ["30 quit"], 0, tmp, "zref")
            frames = max(zref["H"]) if zref else 0
            check(code == 0 and frames >= 12, "z80+audio: reference run of %d frame barriers" % frames)
            plan = []
            t = 2
            while t + 4 < frames:
                plan += ["%d snap 1" % t, "%d restore 1" % (t + 3)]
                t += 5
            plan.append("%d quit" % (frames + 40))
            code, zobs = session(z, plan, 0, tmp, "zsweep")
            check(code == 0 and zobs is not None and zobs["S"] and all(" ok" in x for x in zobs["S"]), "z80+audio: every snapshot and restore succeeded (%d operations)" % (len(zobs["S"]) if zobs else 0))
            if zobs:
                consistent(zobs, zref, "z80+audio sweep (Z80 CPU, YM2612, PSG, mixer, VDP DMA, interrupts)")
            code, zdirty = session(z, ["5 snap 1", "7 dirty", "7 restore 1", "%d quit" % (frames + 40)], 0, tmp, "zdirty")
            consistent(zdirty, zref, "z80+audio dirty: the restore brings back the exact saved machine")
            code, zsave = session(z, ["6 snap 1", "6 dump 1 %s" % (tmp / "z.scd"), "%d quit" % (frames + 40)], 0, tmp, "zsave")
            code, zload = session(z, ["3 slurp 1 %s" % (tmp / "z.scd"), "4 restore 1", "%d quit" % (frames + 40)], 0, tmp, "zload")
            consistent(zload, zref, "z80+audio: a state saved in one process reproduces the reference run in another")
            zsz = (tmp / "z.scd").stat().st_size
            print("      measured: one Sega CD snapshot with the Z80, YM2612, PSG and mixer is %d bytes" % zsz)
            # the Z80 state is bound into the build contract: a state of the Z80 build is not accepted by the build without it, and vice versa
            check(zsz > 1_000_000, "z80+audio: snapshot size %d" % zsz)
            code, cross = session(prog, ["4 slurp 3 %s" % (tmp / "z.scd"), "5 restore 3", "7 quit"], 400, tmp, "crossz")
            check(cross is not None and any("restore 3" in x and ("different disc or BIOS" in x or "does not match" in x) for x in cross["S"]), "a state of the Z80 program is refused by another program")

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
