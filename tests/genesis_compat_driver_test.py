#!/usr/bin/env python3
"""SEG-068-T007: the headless compatibility driver over the existing product route (synthetic ROMs only, no commercial data).

Two project-authored ROMs are built with `segarecomp build` and run once each through tools/genesis_compat_driver.py:
  * a controller-echo ROM (hand-encoded MC68000, tools/genesis_z80_fixture_rom.py's encoder): it sets TH as an output and every
    pass copies the player-1 three-button bits into the red and the player-2 bits into the green channel of CRAM colour 0, so the
    backdrop colour of every captured frame is a function of exactly the two host pad masks;
  * the `sound_tone` fixture (Z80 driver playing PSG/FM/DAC): proves one guest execution yields the captured frames AND the existing
    sound summary, equal to the capture-free sound run.
Proved: injection timing (frames before the keyed frame equal the no-script run, frames after differ), per-player masks, determinism,
that an unreached script is byte-identical to no script, the slice-granularity rule (steady-state frames are slice independent), a
malformed script exits 3 before the run, capture + audio from one execution, PNG/contact-sheet output and the sanitized report.
usage: genesis_compat_driver_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_compat_driver as driver  # noqa: E402
import genesis_z80_fixture_rom as fx  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def pad_echo_rom() -> bytes:
    m = fx.M68k()
    m.w(0x7000)              # MOVEQ #0,D0
    m.branch(0x67, "real")   # BEQ.W real
    m.w(0x4E70)              # RESET (never executed: keeps the program on the immutable-ROM AOT route, like the sound fixtures)
    m.label("real")
    for word in (0x8004, 0x8144, 0x8700, 0x8C81):      # VDP registers 0, 1 (display on, mode 5), 7 (backdrop), 12
        m.move_w_imm(word, 0xC00004)
    m.move_b_imm(0x40, 0xA10009)                        # CTRL1: TH is an output
    m.move_b_imm(0x40, 0xA1000B)                        # CTRL2
    m.move_b_imm(0x40, 0xA10003)                        # DATA1: TH high -> reads 1CBRLDU
    m.move_b_imm(0x40, 0xA10005)                        # DATA2
    m.label("loop")
    m.move_w_imm(0xC000, 0xC00004)                      # CRAM write, address 0 (two command words)
    m.move_w_imm(0x0000, 0xC00004)
    m.w(0x1039); m.l(0xA10003)                          # MOVE.B $A10003.L,D0   (player 1)
    m.w(0x0240, 0x0007)                                 # ANDI.W #7,D0          (U, D, L bits)
    m.w(0xD040)                                         # ADD.W D0,D0           (-> red channel)
    m.w(0x1239); m.l(0xA10005)                          # MOVE.B $A10005.L,D1   (player 2)
    m.w(0x0241, 0x0007)                                 # ANDI.W #7,D1
    m.w(0xEB49)                                         # LSL.W #5,D1           (-> green channel)
    m.w(0x8041)                                         # OR.W D1,D0
    m.w(0x33C0); m.l(0xC00000)                          # MOVE.W D0,$C00000.L   (CRAM colour 0)
    m.branch(0x60, "loop")                              # BRA.W loop
    return fx.build_rom(m.resolve())


def build(rom_bytes: bytes, out: pathlib.Path) -> pathlib.Path:
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    done = subprocess.run([cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir",
                           str(root / "platforms" / "genesis"), "--optimize", "0", "--runtime-optimize", "1"],
                          text=True, capture_output=True, timeout=1200)
    exe = next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)
    if done.returncode != 0 or exe is None:
        print(done.stdout[-2000:], done.stderr[-2000:])
        raise SystemExit("build failed")
    return exe


def drive(exe, out, script="", count=8, slice_=256, first=1, budget=3_000_000, extra=()):
    args = driver.argparse.Namespace(
        exe=exe, rom=None, out_dir=str(out), pad_script=script, capture_first=first, capture_count=count, capture_stride=1,
        capture_slice=slice_, budget=budget, run_timeout=600, pcm_out="", png=False, contact_sheet=False, columns=4)
    for key, value in extra:
        setattr(args, key, value)
    return driver.run_game(args)


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-compat-driver-") as directory:
        tmp = pathlib.Path(directory)
        exe = build(pad_echo_rom(), tmp / "pad")

        base = drive(exe, tmp / "base")
        digests = base["capture"]["frame_digests"]
        check(base["capture"]["outcome"] == "window_complete" and base["capture"]["frames_captured"] == 8 and
              base["capture"]["dispatches"] < 3_000_000,
              "the controller-echo ROM runs headless and the 8-frame window completes well inside the finite budget")
        check(len(set(digests[2:])) == 1, "with no pad activity the steady-state frames are identical")
        check(base["capture"]["pad_entries_applied"] == 0, "an empty script applies nothing")

        # Injection timing and per-player masks (slice 256 dispatches ~ a quarter of a frame for this ROM). Ordinals 1..12 are the
        # published frames; the P1 key is frame 4, the P2 key is frame 8, P1 is released at frame 10.
        script = "4:1:0x01,8:2:0x01,10:1:0"
        a = drive(exe, tmp / "a", script, count=12)
        b = drive(exe, tmp / "b", script, count=12)
        plain = drive(exe, tmp / "p", "", count=12)
        da, dp = a["capture"]["frame_digests"], plain["capture"]["frame_digests"]
        check(da == b["capture"]["frame_digests"], "the same script and slice size give identical frame digests across runs")
        check(da[:3] == dp[:3] == [dp[0]] * 3, "frames before the first keyed frame equal the no-script run")
        changed = [i for i in range(12) if da[i] != dp[i]]
        check(changed and changed[0] == 3, "player 1's mask first changes the frame published at its keyed frame (first differing ordinal %s)" % (changed[:1],))
        check(len({da[4], da[5], da[6]}) == 1 and da[4] != dp[4], "while only player 1 is held the frames are steady and differ from the no-script run")
        check(da[8] != da[4] and da[8] != dp[8] and len({da[8], da[9]}) <= 2, "player 2's key adds a different channel's change on top of player 1's")
        check(da[11] != da[8] and da[11] != da[4] and da[11] != dp[11] and da[10] == da[11],
              "releasing player 1 (mask 0) leaves only player 2's effect (distinct from player 1 alone and from both)")
        check(a["capture"]["pad_entries_applied"] == 3, "all three keyed entries were applied")

        # An unreached script is byte-identical to no script.
        late = drive(exe, tmp / "late", "999999:1:0xFF", count=6)
        check(late["capture"]["frame_digests"] == dp[:6] and late["capture"]["pad_entries_applied"] == 0,
              "a script whose keys are never reached does not change the run")

        # Slice granularity: a key is honoured at slice resolution; frames before the key and steady frames after it are slice independent
        # (only the application point inside a frame - the one partially changed frame - moves with the slice size).
        digests = {size: drive(exe, tmp / ("s%d" % size), script, count=12, slice_=size)["capture"]["frame_digests"] for size in (64, 128, 256)}
        check(all(d[:3] == da[:3] and d[4:7] == da[4:7] and d[8] == da[8] and d[10:12] == da[10:12] for d in digests.values()),
              "frames before the key and steady frames after it are identical for slice sizes 64, 128 and 256")

        # A malformed or unordered script is rejected before the run (exit 3), and by the host driver before building anything.
        # Mask grammar: unprefixed decimal (leading zeroes are decimal, "010" is 10 not octal 8), 0x-hex, 0..255.
        check([driver.parse_mask(t) for t in ("08", "008", "010", "255", "0x80", "0XfF", "0")] == [8, 8, 10, 255, 128, 255, 0],
              "the host parser reads decimal (with leading zeroes) and 0x-hex masks")
        for good in ("4:1:08", "4:1:008", "4:1:0x00", "4:2:255", "4:1:0XFF"):
            try:
                driver.validate_pad_script(good)
                check(True, "host driver accepts %r" % good)
            except driver.DriverError:
                check(False, "host driver accepts %r" % good)
        # Generated side agrees: leading-zero decimal 010 == 10 == 0x0A, observed through the echo ROM's picture.
        pics = {m: drive(exe, tmp / ("m" + m.replace("x", "")), "2:1:" + m, count=5)["capture"]["frame_digests"][4] for m in ("1", "01", "001", "0x01", "010", "10", "0x0A", "08", "8", "0x08")}
        check(pics["1"] == pics["01"] == pics["001"] == pics["0x01"] and pics["010"] == pics["10"] == pics["0x0A"] and pics["08"] == pics["8"] == pics["0x08"]
              and len({pics["1"], pics["10"], pics["8"]}) == 3,
              "the generated parser agrees: leading-zero decimals equal their decimal value (no octal), 0x masks are hexadecimal")
        for bad in ("abc", "4:3:1", "4:1:256", "4:1", "5:1:1,4:1:1", "4:1:0x", "4:1:0xG", "4:1:-1", "4:1:1.5", "4:1:0256", "4:1:0x100"):
            done = subprocess.run([str(exe), "--instruction-budget", "1000"], capture_output=True, text=True, timeout=60,
                                  env={"PATH": "/usr/bin:/bin", "SEGARECOMP_CAPTURE_DIR": str(tmp), "SEGARECOMP_CAPTURE_FIRST": "1",
                                       "SEGARECOMP_CAPTURE_COUNT": "1", "SEGARECOMP_PAD_SCRIPT": bad})
            check(done.returncode == 3 and "malformed SEGARECOMP_PAD_SCRIPT" in done.stderr, "malformed generated-side script %r exits 3 before the run" % bad)
            try:
                driver.validate_pad_script(bad)
                check(False, "host driver rejects %r" % bad)
            except driver.DriverError:
                check(True, "host driver rejects %r" % bad)

        # Host-side conversion and the sanitized report.
        out = tmp / "png"
        report = drive(exe, out, script, count=4, extra=(("png", True), ("contact_sheet", True)))
        pngs = sorted((out / "frames").glob("*.png"))
        sheet = (out / "contact-sheet.png").read_bytes()
        check(len(pngs) == 4 and sheet[:8] == b"\x89PNG\r\n\x1a\n" and all(p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" for p in pngs),
              "PPM frames are converted to PNG and tiled into a contact sheet host-side")
        text = json.dumps(report)
        check(set(report) == {"schema", "result", "stop_class", "diagnostic_category", "exit_status", "capture", "sound"} and "/" not in text.replace("\\/", ""),
              "the report is aggregate-only (no path, address or byte)")
        try:
            drive(exe, root / "docs" / "compat-driver-output")  # inside the repository and not ignored
            check(False, "a non-ignored in-repository output directory is refused")
        except driver.DriverError:
            check(True, "a non-ignored in-repository output directory is refused")

        # The sanitized compatibility matrix: labels and classifications only; an unbuildable game is a row, not a crash.
        import contextlib
        import io
        games = tmp / "games.json"
        (tmp / "pad-matrix.md").write_bytes(pad_echo_rom())
        (tmp / "garbage.md").write_bytes(b"not a rom")
        games.write_text(json.dumps([{"label": "echo", "rom": str(tmp / "pad-matrix.md"), "pad_script": "4:1:0x01"},
                                     {"label": "garbage", "rom": str(tmp / "garbage.md")}]), encoding="utf-8")
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            status = driver.main(["matrix", "--games", str(games), "--segarecomp", cli, "--cc", cc, "--cxx", cxx, "--runtime-dir",
                                  str(root / "platforms" / "genesis"), "--out-dir", str(tmp / "matrix"), "--capture-count", "4",
                                  "--budget", "2000000"])
        rows = json.loads(captured.getvalue())["rows"]
        check(status == 0 and [r["label"] for r in rows] == ["echo", "garbage"] and rows[0]["build"] == "ok" and rows[0]["frames_captured"] == 4 and
              rows[1]["build"] != "ok" and set(rows[0]) == {"label", "build", "result", "stop_class", "diagnostic_category", "capture_outcome",
                                                           "frames_captured", "frames_published", "non_silent"},
              "the matrix emitter prints sanitized rows (classifications and aggregates only) and records a build failure as a row")

        # One guest execution yields the captured frames AND the sound summary, equal to the capture-free run.
        def vdp_on(m):
            for word in (0x8004, 0x8144, 0x8700, 0x8C81):
                m.move_w_imm(word, 0xC00004)
        sound_exe = build(fx.build_sound([("raw", "t")], {"t": fx.sound_tone_driver()}, prelude=vdp_on), tmp / "sound")
        env = {"PATH": "/usr/bin:/bin", "SEGARECOMP_SOUND_SUMMARY": "1"}
        both = drive(sound_exe, tmp / "both", "", count=3, slice_=4096, budget=1_500_000)
        # The capture ends the run at the slice boundary after its window completes; the capture-free run with exactly that many
        # dispatches ends in the same guest state, so the sound summaries must agree bit for bit.
        free = subprocess.run([str(sound_exe), "--instruction-budget", str(both["capture"]["dispatches"])], capture_output=True, text=True,
                              timeout=300, env=env)
        free_summary = json.loads(re.search(r"SOUND_SUMMARY (\{.*\})", free.stderr).group(1))
        check(both["capture"]["frames_captured"] == 3 and both["capture"]["frames_published"] >= 3,
              "the display-enabled sound ROM yields captured frames from the same execution")
        check(both["sound"]["sha256"] == free_summary["audio"]["sha256"] and both["sound"]["non_silent"] is True and both["sound"]["fault"] == 0,
              "its sound summary (audio digest, non-silence) equals the capture-free run's: the capture does not perturb the guest")
    print("genesis compat driver: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
