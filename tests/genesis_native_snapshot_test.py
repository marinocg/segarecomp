#!/usr/bin/env python3
"""ADR 0291: save states and rewind with the native GEMS sound-driver replacement bound, on the REAL Genesis runtime and production sound subsystem.

The 68K bus script (tests/tools/genesis_snapshot_harness.c, built with the production genesis_sound.c adapter and gems_native.c) uploads a project-authored
Z80 image whose activation signature is the one test signature this build trusts (SEGARECOMP_GEMS_TEST_SIGNATURE_HEX; the product build never defines it),
writes real GEMS commands into the Z80 RAM command FIFO, and lets the replacement play a project-authored cartridge. No commercial data.

Proved, each against the independent whole-state hash plus the SHA-256 over every mixed sample:
  * a state captured mid-sample restores, and the future (bus hold, /RESET + re-upload, further playback) is bit-identical to the uninterrupted one, repeatedly;
  * a state saved by one process is restored by a fresh process (native driver not yet started) and continues identically;
  * a native state is refused (unchanged) by a program without the replacement and vice versa; an identity / version / schema / signature mismatch, a damaged length,
    and arbitrary payload damage with a valid digest are refused without mutating the machine;
  * a faulted native driver restores inert, with no further chip writes.
usage: genesis_native_snapshot_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tests"))
sys.path.insert(0, str(root / "tools"))
import genesis_snapshot_test as base  # noqa: E402  (same argv layout; defines the script builder, the synthetic Z80 images and the helpers)
import z80_conformance as z  # noqa: E402
import genesis_ym2612_build as ymbuild  # noqa: E402

OK, INCOMPATIBLE, INVALID_STATE, BAD_LAYOUT = 0, 11, 12, 7
ZRAM, BUSREQ, RESET = base.ZRAM, base.BUSREQ, base.RESET
PROGRAM = base.PROGRAMS["X"]


def activation_signature(program):
    h = hashlib.sha256()
    h.update(b"segarecomp.genesis.z80.signature.v1.extents")
    h.update(bytes([0, 0, len(program) & 0xFF, len(program) >> 8]))
    h.update(program)
    return h.hexdigest()


def fifo(script, data, write_pointer):
    """The 68000 writes command bytes into the driver's FIFO ($1B40, 64-byte ring) and advances the write pointer ($36), with the Z80 bus held."""
    script.w16(BUSREQ, 0x100)
    script.advance(200, 100)
    for i, byte in enumerate(data):
        script.w8(ZRAM + 0x1B40 + ((write_pointer + i) & 0x3F), byte)
    script.w8(ZRAM + 0x36, (write_pointer + len(data)) & 0x3F)
    script.w16(BUSREQ, 0)


POINTERS = bytes([0xFF, 11, 0x00, 0x10, 0x10, 0, 0, 0, 0x00, 0x20, 0x10, 0x00, 0x30, 0x10])
START = lambda sequence: bytes([0xFF, 16, sequence])


def prefix(sequence=0, long_loop=0):
    s = base.Script()
    s.raw("cart %d" % long_loop)
    s.epoch("X")
    s.advance(20000, 1000)
    fifo(s, POINTERS + START(sequence), 0)
    s.advance(500000, 5000)       # a few frames: the sample is mid-way
    return s


def suffix(s, sequence=0, restart=True):
    s.advance(300000, 5000)
    s.w16(BUSREQ, 0x100)          # a long bus hold: interrupt edges inside it are lost
    s.advance(400000, 5000)
    s.w16(BUSREQ, 0)
    s.advance(300000, 5000)
    if restart:                   # /RESET and a re-upload: the replacement is started again and plays again
        s.epoch("X")
        s.advance(20000, 1000)
        fifo(s, POINTERS + START(sequence), 17)
        s.advance(400000, 5000)
    s.raw("mixer")
    s.raw("fullhash")
    s.raw("native")


def native(out):
    return [dict(kv.split("=") for kv in m.group(1).split()) for m in re.finditer(r"^NATIVE (.*)$", out, re.M)]


def main():
    failures = base.failures
    check = base.check
    signature = activation_signature(PROGRAM)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        harness = root / "tests" / "tools" / "genesis_snapshot_harness.c"
        rt = root / "platforms/genesis/runtime"
        includes = ["-I", str(rt), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")]
        ym_objects = ymbuild.build_objects(cc, cxx, root, tmp / "ymobj")
        spec = tmp / "x.spec"
        spec.write_text(base.registry_spec(["X"]))
        for tag in ("native", "plain"):   # separate work directories: the object files of the two builds must not collide
            done = subprocess.run([registry_emitter, str(spec), str(tmp / tag), "genesis_z80"], text=True, capture_output=True)
            assert done.returncode == 0, done.stdout + done.stderr
        sources = [harness, rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c", rt / "genesis_mixer.c", rt / "genesis_snapshot.c", root / "libs/device/sega/psg/src/sn76489.c"]
        exes = {}
        exe, message = z.compile_units(tc, tmp / "native", "genesis_z80", extra_sources=sources + [rt / "genesis_sound.c", rt / "gems_native.c"],
                                       extra_flags=includes + ["-DSEGARECOMP_GENESIS_SOUND", "-DSEGARECOMP_SNAPSHOT_NATIVE", "-DSEGARECOMP_NATIVE_SOUND_GEMS=1",
                                                               '-DSEGARECOMP_GEMS_TEST_SIGNATURE_HEX="%s"' % signature], extra_objects=ym_objects)
        assert exe is not None, message
        exes["native"] = exe
        exe, message = z.compile_units(tc, tmp / "plain", "genesis_z80", extra_sources=sources, extra_flags=includes + ["-DSEGARECOMP_GENESIS_SOUND"], extra_objects=ym_objects)
        assert exe is not None, message
        exes["plain"] = exe

        # ================= restore reproduces the future (mid-sample), repeatedly =================
        s = prefix()
        s.raw("native")
        s.raw("fullhash")
        s.raw("snap 1")
        s.raw("fullhash")
        suffix(s)
        s.raw("restore 1")
        s.raw("fullhash")
        suffix(s)
        s.raw("restore 1")
        suffix(s)
        out = base.run(exes["native"], s, "-")
        fh, st, mx = base.full(out), native(out), base.mixers(out)
        check(base.statuses(out, "SNAP") == [OK] and base.statuses(out, "RESTORE") == [OK, OK], "the native machine captures and restores (status ok)")
        check(len(st) == 4 and st[0]["active"] == "1" and st[0]["fault"] == "none" and int(st[0]["dac"]) > 0 and int(st[0]["commands"]) >= 2,
              "at S the replacement is bound, healthy, and in the middle of a sample (%s)" % (st[0] if st else None))
        check(len(fh) == 6 and fh[0] == fh[1], "capture does not modify the machine (including the replacement's state)")
        check(len(fh) == 6 and fh[3] == fh[0], "the restored machine, driver included, is bit-identical to the machine at S")
        check(len(fh) == 6 and fh[2] == fh[4] == fh[5] and fh[2] != fh[0], "the future after every restore is identical to the straight future")
        check(len(mx) == 3 and mx[0] == mx[1] == mx[2] and "nonsilent=1" in mx[0], "the mixed audio stream (SHA-256 over every sample) is identical and non-silent")
        check(len(st) == 4 and st[1] == st[2] == st[3] and st[1]["active"] == "1" and int(st[1]["activations"]) == 2, "the driver ran through the /RESET restart in every future and ended healthy (%s)" % (st[1] if len(st) > 1 else None))

        # ================= a fresh process restores a state saved by another =================
        path, path2, again, again2 = (tmp / n for n in ("native.sgstate", "native2.sgstate", "native.restored", "native2.restored"))
        s = prefix()
        s.raw("snap 1")
        s.raw("save 1 %s" % path)
        suffix(s, restart=False)
        s.raw("snap 2")
        s.raw("save 2 %s" % path2)
        a = base.run(exes["native"], s, "-")
        s = base.Script()
        s.raw("cart 0")
        s.raw("load 1 %s" % path)
        s.raw("restore 1")
        s.raw("snap 3")
        s.raw("save 3 %s" % again)
        s.raw("native")
        suffix(s, restart=False)
        s.raw("snap 2")
        s.raw("save 2 %s" % again2)
        b = base.run(exes["native"], s, "-")
        check(base.statuses(b, "RESTORE") == [OK], "a fresh process (replacement never started) accepts the saved state")
        check(path.read_bytes() == again.read_bytes(), "re-capturing in the fresh process yields the identical state file (every machine and driver field)")
        check(path2.read_bytes() == again2.read_bytes() and path2.read_bytes() != path.read_bytes(), "and after the same future the fresh process's state file is identical to the saving process's")
        check(base.mixers(a)[0] == base.mixers(b)[0] and native(a)[-1] == native(b)[-1], "the mixed audio digest and the driver's counters agree")
        check(native(b)[0]["active"] == "1" and native(b)[0]["fault"] == "none", "the restored replacement is active and healthy before any further run")

        # ================= build profile and identity boundaries =================
        s = base.Script()
        s.raw("fullhash")
        s.raw("load 1 %s" % path)
        s.raw("restore 1")
        s.raw("fullhash")
        out = base.run(exes["plain"], s, "-")
        fh = base.full(out)
        check(base.statuses(out, "RESTORE") == [INCOMPATIBLE] and fh[0] == fh[1], "a native state is refused by a program without the replacement, unchanged")
        s = base.Script()
        s.epoch("X")
        s.advance(20000, 1000)
        s.raw("snap 1")
        s.raw("save 1 %s" % (tmp / "plain.sgstate"))
        base.run(exes["plain"], s, "-")
        s = base.Script()
        s.raw("cart 0")
        s.raw("fullhash")
        s.raw("load 1 %s" % (tmp / "plain.sgstate"))
        s.raw("restore 1")
        s.raw("fullhash")
        out = base.run(exes["native"], s, "-")
        fh = base.full(out)
        check(base.statuses(out, "RESTORE") == [INCOMPATIBLE] and fh[0] == fh[1], "a state of a program without the replacement is refused by one that has it, unchanged")

        # ================= damage =================
        s = prefix()
        s.raw("snap 1")
        s.raw("layout 1")
        s.raw("save 1 %s" % (tmp / "slot.sgstate"))
        out = base.run(exes["native"], s, "-")
        secs, size = base.sections(out)
        z_off, z_len = secs[base.SECTION["Z80"]]
        data = (tmp / "slot.sgstate").read_bytes()
        section = data[z_off:z_off + z_len]
        name_at = section.find(b"gems" + b"test\0\0\0\0")
        check(name_at >= 5, "the Z80 section carries the replacement's identity trailer")
        payload_len = int.from_bytes(section[name_at + 52:name_at + 56], "little")
        check(payload_len > 0 and name_at + 56 + payload_len == z_len, "the trailer is exactly identity, signature, length and payload (%d payload bytes)" % payload_len)
        fields = {"driver name": name_at, "version text": name_at + 4, "implementation version": name_at + 12, "state schema": name_at + 16, "activation signature": name_at + 20}
        s = prefix()
        s.raw("snap 1")
        s.raw("fullhash")
        for key, at in fields.items():
            s.raw("snap 2")
            s.raw("tamper 2 %x 01" % (z_off + at))
            s.raw("redigest 2")
            s.raw("restore 2")
        s.raw("snap 2")
        s.raw("tamper 2 %x 01" % (z_off + name_at + 52))
        s.raw("redigest 2")
        s.raw("restore 2")
        s.raw("fullhash")
        out = base.run(exes["native"], s, "-")
        codes, fh = base.statuses(out, "RESTORE"), base.full(out)
        check(codes[:len(fields)] == [INCOMPATIBLE] * len(fields), "a different driver / version / implementation / schema / signature (valid digest) is refused as incompatible (%s)" % codes)
        check(codes[-1] == BAD_LAYOUT, "a damaged payload length is refused as a bad layout (%s)" % codes[-1])
        check(len(fh) == 2 and fh[0] == fh[1], "identity refusals never mutate the machine")
        s = prefix()
        s.raw("snap 1")
        s.raw("fullhash")
        positions = list(range(0, payload_len, 5))
        for at in positions:
            s.raw("snap 2")
            s.raw("tamper 2 %x A5" % (z_off + name_at + 56 + at))
            s.raw("redigest 2")
            s.raw("restore 2")
            s.raw("fullhash")
        out = base.run(exes["native"], s, "-")
        codes, fh = base.statuses(out, "RESTORE"), base.full(out)
        refused = [i for i, c in enumerate(codes) if c != OK]
        check(len(codes) == len(positions) and len(refused) > 10, "structural payload damage with a valid digest is refused (%d of %d; the rest are legal data values)" % (len(refused), len(codes)))
        check(all(c in (OK, INVALID_STATE, BAD_LAYOUT) for c in codes), "refusals are typed (%s)" % sorted(set(codes)))
        # a refused restore leaves the machine as the previous attempt left it: runs of refusals keep one hash; an accepted one is followed by the original state again below
        stable = all(fh[i] == fh[i - 1] for i in refused if i > 0 and (i - 1) in refused)
        check(stable, "consecutive refused restores never change the machine")

        # accepted (legal-value) damage still yields a machine that keeps running without a crash or hang
        s = prefix()
        s.raw("snap 1")
        for at in positions[:40:4]:
            s.raw("snap 2")
            s.raw("tamper 2 %x A5" % (z_off + name_at + 56 + at))
            s.raw("redigest 2")
            s.raw("restore 2")
            s.advance(200000, 5000)
            s.raw("native")
        out = base.run(exes["native"], s, "-")
        check(out.rstrip().endswith("END") and len(native(out)) == 10, "machines restored from accepted damaged payloads run on without a crash or hang")

        # ================= a faulted driver restores inert =================
        s = prefix(sequence=2)        # sequence 2 runs off the cartridge: an explicit, latched fault
        s.raw("native")
        s.raw("snap 1")
        s.raw("fullhash")
        s.advance(600000, 5000)
        s.raw("native")
        s.raw("fullhash")
        s.raw("restore 1")
        s.raw("fullhash")
        s.raw("native")
        s.advance(600000, 5000)
        s.raw("native")
        s.raw("fullhash")
        out = base.run(exes["native"], s, "-")
        st, fh = native(out), base.full(out)
        check(len(st) == 4 and st[0]["fault"] != "none" and st[0]["sound_fault"] == "67", "the sequence faults the replacement explicitly (%s)" % (st[0] if st else None))
        check(base.statuses(out, "RESTORE") == [OK] and len(fh) == 4 and fh[2] == fh[0] and fh[3] == fh[1], "the faulted state restores bit-identically and the same inert future follows")
        check(len(st) == 4 and st[2]["fault"] == st[0]["fault"] and st[2]["sound_fault"] == "67" and st[3]["fault"] == st[0]["fault"], "the restored driver is still faulted")
        check(len(st) == 4 and st[1]["ym_writes"] == st[0]["ym_writes"] and st[1]["psg_writes"] == st[0]["psg_writes"] and st[3]["ym_writes"] == st[2]["ym_writes"] and st[3]["psg_writes"] == st[2]["psg_writes"],
              "a faulted driver writes nothing further, before or after the restore")
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
