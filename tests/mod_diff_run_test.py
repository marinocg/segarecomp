#!/usr/bin/env python3
"""SEG-136-T007 (ADR 0300): `segarecomp mod diff-run` and `segarecomp mod test` with FABRICATED, redistributable inputs only (the gameplay example's runnable fixture ROM; no commercial image).

Proves, through real generated-native programs built by the unmodified product and run headless under the existing frame capture:
  * a Vanilla-versus-Modded run reports the FIRST divergence at the frame the scripted input makes the mod act, with the changed guest RAM byte (offset, Vanilla value, Modded value), and the
    counts of every later diverged frame; `--aggregate` reports the same without any byte value; the report is deterministic;
  * identical variants (a mod that only logs; a data-only package whose edit lies in code the program never reaches) report NO divergence, and `--code` shows the edited image bytes;
  * the runs are bounded and explicit: a missing or out-of-range --budget/--timeout/--frames, a malformed input script or region is refused with a stable class before anything is built; the
    accepted bounds are echoed in the report; an exhausted instruction budget is a typed refusal, not a partial success;
  * `mod test` passes the declared scenarios, and a deliberately broken expectation fails with the first divergence (field, frame, expected, actual) and a non-zero exit.
usage: mod_diff_run_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

if sys.platform == "win32":   # the generated-native program build (a C++ Luau VM next to the generated C) is exercised on the Linux and macOS runners
    print("mod_diff_run_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/genesis/sdk/examples/gameplay"
spec = importlib.util.spec_from_file_location("fixture_rom", EXAMPLE / "fixture_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-diffrun-"))
ROM = write(tmp / "fixture.rom", fixture.make_rom())
ROM_SHA = sha(ROM.read_bytes())
INPUT = "10:1:0x20,20:1:0x00"          # button C held for frames 10..19 (the mod's bonus rule)
BOUNDS = ["--frames", "30", "--budget", "4000000", "--timeout", "120"]


def project(name, script=None, caps=None, extra_toml=""):
    d = tmp / name
    shutil.copytree(EXAMPLE, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__", "tests.toml"))
    if script is not None:
        (d / "scripts/main.luau").write_text(script)
    if caps is not None:
        toml = (d / "mod.toml").read_text()
        head = toml.split("[capabilities]")[0]
        (d / "mod.toml").write_text(head + "[capabilities]\nrequired = [" + ", ".join(f'"{c}"' for c in caps) + "]\n" + extra_toml)
    return d


def diff(target, *args, expect_ok=True):
    rc, doc, p = cli.run("mod", "diff-run", target, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK, *args)
    if expect_ok:
        cli.check(rc == 0 and doc and doc.get("ok") is True, f"diff-run {Path(str(target)).name}: rc={rc} {doc} {p.stderr[-300:]}")
    return rc, doc


def refused(what, code, *args):
    rc, doc, p = cli.run("mod", "diff-run", EXAMPLE, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK, *args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


# ---------------------------------------------------------------- bounds are explicit and enforced BEFORE anything is built
refused("no budget", "E_BUDGET", "--frames", "30", "--timeout", "60")
refused("no timeout", "E_TIMEOUT", "--frames", "30", "--budget", "1000000")
refused("budget 0", "E_BUDGET", "--frames", "30", "--budget", "0", "--timeout", "60")
refused("budget beyond the cap", "E_BUDGET", "--frames", "30", "--budget", "2000000001", "--timeout", "60")
refused("timeout beyond the cap", "E_TIMEOUT", "--frames", "30", "--budget", "1000000", "--timeout", "3601")
refused("frames beyond the cap", "E_FRAMES", "--frames", "65", "--budget", "1000000", "--timeout", "60")
refused("malformed input script", "E_INPUT_SCRIPT", *BOUNDS, "--input", "10:3:0x20")
refused("decreasing input frames", "E_INPUT_SCRIPT", *BOUNDS, "--input", "20:1:0x20,10:1:0x00")
refused("mask beyond a byte", "E_INPUT_SCRIPT", *BOUNDS, "--input", "10:1:0x100")
refused("a region outside the work RAM", "E_REGION", *BOUNDS, "--ram", "65535:2")
refused("too many ram bytes", "E_REGION", *BOUNDS, "--ram", "0:1025")
refused("malformed region", "E_REGION", *BOUNDS, "--ram", "16")

# ---------------------------------------------------------------- the gameplay example: the first divergence is where the input makes the mod act
gp = project("gameplay")
RAM = ["--ram", "16:4"]
rc, d1 = diff(gp, *BOUNDS, "--input", INPUT, *RAM)
cli.check(d1 is not None and d1["identical"] is False and d1["command"] == "diff-run", "the example diverges from Vanilla")
fd = d1["first_divergence"]
# Button C is keyed at guest frame 10; the mod adds 10 to the score (work RAM 0xFF0010..11, big-endian word) in the next frame's post-input phase, visible in the sample of publication ordinal 11.
cli.check(fd["frame"] == 11 and fd["ram_differs"] is True and fd["frame_digest_differs"] is False, f"the first divergence is the RAM at frame 11: {fd}")
cli.check(fd["ram"] == [{"region": 0, "offset": 16, "length": 4, "changed_byte_count": 1, "changed_bytes": [{"offset": 17, "vanilla": 0, "modded": 10}], "truncated": False}], f"the changed guest byte: {fd['ram']}")
cli.check(d1["diverged_frames"] == 20 and d1["final_state_differs"] is True, f"every frame from 11 on diverges (the bonus stays): {d1['diverged_frames']}")
cli.check(d1["bounds"] == {"budget": 4000000, "timeout_seconds": 120, "build_timeout_seconds": 900, "slice_dispatches": 16} and d1["input"] == INPUT and d1["frames"] == 30, f"the bounds are echoed: {d1['bounds']}")
cli.check(d1["vanilla"]["outcome"] == "window_complete" and d1["modded"]["outcome"] == "window_complete" and d1["vanilla"].get("mod") is None, "both programs complete the window; Vanilla has no mod")
m = d1["modded"]["mod"]
cli.check(m["id"] == "gameplay-example" and m["faulted"] is False and m["committed_writes"] == 10 and m["nondeterministic"] is False and m["overlay_ops"] == 4, f"the mod ran and committed exactly its ten bonus writes: {m}")
cli.check(d1["vanilla"]["final_frame_sha256"] == d1["modded"]["final_frame_sha256"], "the picture is identical (the mod changes the game state; its HUD is a host overlay, not a VDP frame)")
# determinism: the same command is the same report
rc, d2 = diff(gp, *BOUNDS, "--input", INPUT, *RAM)
cli.check(d2 == d1, "two runs of the same diff-run produce the identical report")
# without the key press nothing diverges
rc, d3 = diff(gp, *BOUNDS, "--input", "10:1:0x10,20:1:0x00", *RAM)   # button B only: the fixture's own rule, no mod rule
cli.check(d3["identical"] is True and d3["first_divergence"] is None and d3["diverged_frames"] == 0 and d3["final_state_differs"] is False, f"another button leaves the variants identical: {d3['first_divergence']}")
# a window that starts later: the divergence is still reported by its absolute frame ordinal, and only the frames inside the window count
rc, dw = diff(gp, "--first", "8", "--frames", "10", "--budget", "4000000", "--timeout", "120", "--input", INPUT, *RAM)
cli.check(dw is not None and dw["first_frame"] == 8 and dw["first_divergence"]["frame"] == 11 and dw["diverged_frames"] == 7, f"--first moves the window, not the numbering: {dw and (dw['first_frame'], dw['first_divergence'], dw['diverged_frames'])}")
refused("--first beyond the cap", "E_FRAMES", *BOUNDS, "--first", "2000001")
# aggregate mode: the same classification with no byte values
rc, d4 = diff(gp, *BOUNDS, "--input", INPUT, *RAM, "--aggregate")
agg = d4["first_divergence"]
cli.check(agg["frame"] == 11 and agg["ram"][0]["changed_byte_count"] == 1 and "changed_bytes" not in agg["ram"][0] and d4["aggregate"] is True, f"aggregate output carries counts, not values: {agg}")

# ---------------------------------------------------------------- bounded runs: a too small budget is a typed refusal
rc, doc, p = cli.run("mod", "diff-run", gp, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK, "--frames", "30", "--budget", "1000", "--timeout", "60")
cli.check(rc == 1 and doc and doc["class"] == "E_RUN_INCOMPLETE" and "vanilla" in doc["error"], f"an exhausted instruction budget is refused: {doc}")

# ---------------------------------------------------------------- identical variants
quiet = project("quiet", script='function reset() sega.log("quiet") end\n', caps=["host.log"])
rc, q = diff(quiet, *BOUNDS, "--input", INPUT, *RAM)
cli.check(q is not None and q["identical"] is True and q["first_divergence"] is None and q["modded"]["mod"]["committed_writes"] == 0, f"a mod that only logs is identical to Vanilla: {q and q['first_divergence']}")
cli.check(q["vanilla"]["final_work_ram_sha256"] == q["modded"]["final_work_ram_sha256"] and q["vanilla"]["pcm_sha256"] == q["modded"]["pcm_sha256"], "state and PCM digests agree")

# a data-only package: a typed ROM edit in image bytes the program never reaches (the filler `bra.s *` after the code) is a different IMAGE but identical behaviour; --code reports the bytes
edit = tmp / "edit"
write(edit / "mod.toml", f'[mod]\nid = "edit-only"\nversion = "1.0.0"\nname = "Edit only"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "genesis"\n\n[[target.revision]]\nlabel = "fixture"\nsha256 = "{ROM_SHA}"\n\n'
      '[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n')
write(edit / "edits/main.edits", "fixture u8 0x000003f0 0x60 0x62\n")
rc, e = diff(edit, *BOUNDS, "--input", INPUT, *RAM, "--code", "1008:4")
cli.check(e is not None and e["identical"] is True and e["modded"].get("mod") is None, f"an edit in unreachable image bytes does not change behaviour: {e and e['first_divergence']}")
cli.check(e["code"] == [{"offset": 1008, "length": 4, "changed_byte_count": 1, "changed_bytes": [{"offset": 1008, "vanilla": 96, "modded": 98}]}], f"--code reports the edited image bytes: {e['code']}")
# an edit the program reads: the fixture's score rule `addq.w #1` becomes `addq.w #2`, a behavioural divergence found at the first turn that holds B
edit2 = tmp / "edit2"
shutil.copytree(edit, edit2)
image = fixture.make_rom()
at = image.index(bytes.fromhex("526a0010"))
write(edit2 / "edits/main.edits", f"fixture u16 0x{at:08x} 0x526a 0x546a\n")
rc, e2 = diff(edit2, *BOUNDS, "--input", "0:1:0x10", *RAM)
fd2 = e2["first_divergence"] if e2 else None
cli.check(e2 is not None and e2["identical"] is False and fd2["frame"] <= 3 and fd2["ram"][0]["changed_bytes"][0]["offset"] == 17, f"a changed rule is a behavioural divergence at the first scoring turn: {fd2}")
change = fd2["ram"][0]["changed_bytes"][0]
cli.check(change["modded"] == 2 * change["vanilla"] and change["vanilla"] >= 1, f"the edited program scores twice as much: {change}")

# ---------------------------------------------------------------- mod test: declared scenarios, bounded, first divergence on a broken expectation
rc, t1, p = cli.run("mod", "test", EXAMPLE, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 0 and t1 and t1["ok"] is True and [s["name"] for s in t1["scenarios"]] == ["bonus-while-c-held", "no-bonus-without-c"] and all(s["passed"] for s in t1["scenarios"]), f"the example's scenarios pass: {t1}")
cli.check(all(s["budget"] == 4000000 and s["timeout_seconds"] == 60 for s in t1["scenarios"]) and t1["bounds"]["build_timeout_seconds"] == 900, "every scenario states its explicit budget and timeout")
broken = tmp / "broken"
shutil.copytree(EXAMPLE, broken, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
text = (broken / "tests.toml").read_text()
assert '"00640000"' in text
(broken / "tests.toml").write_text(text.replace('"00640000"', '"00630000"', 1))
rc, t2, p = cli.run("mod", "test", broken, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
bad = t2["scenarios"][0] if t2 else {}
cli.check(rc == 1 and t2["ok"] is False and t2["class"] == "E_TEST_FAILED" and bad["passed"] is False, f"a broken expectation fails: rc={rc}")
cli.check(bad["first_divergence"] == {"field": "ram", "frame": 20, "expected": "00630000", "actual": "00640000"} and t2["scenarios"][1]["passed"] is True, f"the first divergence is named: {bad.get('first_divergence')}")
rc, t3, p = cli.run("mod", "test", broken, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK, "--scenario", "no-bonus-without-c")
cli.check(rc == 0 and [s["name"] for s in t3["scenarios"]] == ["no-bonus-without-c"], "--scenario selects one scenario")
rc, t4, p = cli.run("mod", "test", EXAMPLE, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK, "--scenario", "nope")
cli.check(rc == 1 and t4["class"] == "E_TEST_SCENARIO", "an unknown scenario is refused")
# a scenario without an explicit budget or timeout is refused: tests are bounded or they do not run
unbounded = tmp / "unbounded"
shutil.copytree(EXAMPLE, unbounded, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
(unbounded / "tests.toml").write_text('[[scenario]]\nname = "x"\nframes = 4\ntimeout = 5\n')
rc, t5, p = cli.run("mod", "test", unbounded, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 1 and t5["class"] == "E_BUDGET", f"a scenario needs an explicit budget: {t5}")
(unbounded / "tests.toml").write_text('[[scenario]]\nname = "x"\nframes = 4\nbudget = 1000\n')
rc, t6, p = cli.run("mod", "test", unbounded, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 1 and t6["class"] == "E_TIMEOUT", f"a scenario needs an explicit timeout: {t6}")
(unbounded / "tests.toml").write_text('[[scenario]]\nname = "x"\nframes = 4\nbudget = 4000000\ntimeout = 5\nexpect_pcm = "' + "0" * 64 + '"\n')
rc, t7, p = cli.run("mod", "test", unbounded, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 1 and t7["scenarios"][0]["first_divergence"]["field"] == "pcm", f"a PCM digest expectation is checked: {t7}")
(unbounded / "tests.toml").write_text('[[scenario]]\nname = "x"\nframes = 4\nbudget = 4000000\ntimeout = 5\nwho = "x"\n')
rc, t8, p = cli.run("mod", "test", unbounded, "--rom", ROM, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 1 and t8["class"] == "E_TEST_SYNTAX", f"an unknown key is refused: {t8}")
# the commands write nothing into the project they were given
cli.check(sorted(str(p.relative_to(EXAMPLE)) for p in EXAMPLE.rglob("*") if "__pycache__" not in p.parts) == sorted(["README.md", "fixture_rom.py", "mod.toml", "scripts", "scripts/main.luau", "tests.toml"]), "the example directory is untouched")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_diff_run_test")
