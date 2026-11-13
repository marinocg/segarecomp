#!/usr/bin/env python3
"""SEG-138-T009 (ADR 0300): the synthetic gameplay-changing Genesis example mod, end to end, with FABRICATED inputs only (platforms/genesis/sdk/examples/gameplay; no commercial image).

The whole developer workflow runs in CI: check -> build -> test -> pack -> install -> variant, then the REAL generated-native programs: the unmodified product builds Vanilla and Modded programs of the
fixture ROM (the Modded one links the ONE mod scheduler, the Luau VM and the package's verified compiled scripts) and both run headless under the frame capture with the same scripted pad.

Proves: (1) the package builds and packs deterministically (pinned identity/content/bytes/bytecode digests; independent of the working directory) and is the source tree of the public example; (2) the
gameplay effect is real and hand-derived: while C is held the score word in guest work RAM grows by 10 per frame (typed, bounded RAM access under a manifest-declared range), Vanilla's does not; the
final guest state equals an independent derivation; (3) replay: the same input reproduces identical frame digests, RAM samples, final state, replay witness and overlay hash, a different input does not;
(4) save/restore: a state saved mid-run (with the mod's own `granted` counter) and loaded into a FRESH program ends in the identical final state, witness and overlay hash, and a corrupted or foreign
state is refused; (5) the HUD overlay is produced (4 operations, pinned hash); (6) a mod that touches memory outside its declared range faults deterministically: its write is discarded, only it is
disabled and the guest ends in the Vanilla state; (7) every undeclared or unsupported memory/capability combination is refused with a typed class before any program exists.
usage: mod_gameplay_example_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

if sys.platform == "win32":   # the generated-native program build (a C++ Luau VM next to the generated C) is exercised on the Linux and macOS runners
    print("mod_gameplay_example_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
EXE = cli.exe
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/genesis/sdk/examples/gameplay"
spec = importlib.util.spec_from_file_location("fixture_rom", EXAMPLE / "fixture_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-gameplay-"))

# ---- pinned values (a change of the example, the schema 3 writer, the packer, the trusted compiler or the overlay rasterizer changes them on purpose) ----
FIXTURE_SHA = "fc7126ee2497cfbc7e1e3337c230a967d21c4c98e44493a4dfd01ae3ff6a5a34"
PINNED_IDENTITY = "09a6f3da161f156d4431b50137f8ac5eaea3b32b5bff8e671fdf722bdbb8ed1e"
PINNED_CONTENT = "492fe5d7ab38cc4152b273705e1b52044b7746fbee9601abaac79aa5cebedb8d"
PINNED_PACKAGE_SHA = "4b5dcabb41cabedea8f758416f0d1657aac4f77d4eae22cbe17e629e1f107e0e"
PINNED_BYTECODE = "359a90dd27a84a1aa348bca8534909424fd66b2eaa5fdd9061186f073aded4e2"
PINNED_OVERLAY = "8be9abb7b96693c73de79afda2dd73085f1fb06cb9c42fe390316dd63787450f"
PINNED_WRITE_DIGEST = "fef0737a0ff08f72d166e969072ca1bd9762a35b9e017e3406f79f28c76b6f8c"


def derived_state(score, position):
    """The final work RAM, derived independently: all zero except the two words the fixture program owns."""
    ram = bytearray(65536)
    ram[0x10:0x14] = score.to_bytes(2, "big") + position.to_bytes(2, "big")
    return sha(bytes(ram))


def ok(what, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-300:]}")
    return doc


def refused(what, code, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


# ---------------------------------------------------------------- the fixture and the example tree
rom = write(tmp / "fixture.rom", fixture.make_rom())
cli.check(sha(rom.read_bytes()) == FIXTURE_SHA and fixture.make_rom() == fixture.make_rom(), "the fixture image is deterministic and has the pinned digest")
cli.check(f'sha256 = "{FIXTURE_SHA}"' in (EXAMPLE / "mod.toml").read_text(), "mod.toml names the fixture image")
project = tmp / "gameplay"
shutil.copytree(EXAMPLE, project, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))

# ---------------------------------------------------------------- the workflow: check -> build -> test -> pack -> install -> variant
chk = ok("check", "mod", "check", project)
cli.check(chk["package_identity"] == PINNED_IDENTITY and chk["content_digest"] == PINNED_CONTENT and chk["trust_class"] == "luau", f"check: the pinned package identity {chk.get('package_identity')}")
built = tmp / "built.segapatch"
b = ok("build", "mod", "build", project, "--out", built)
cli.check(b["luau"]["bytecode_digest"] == PINNED_BYTECODE and b["built"] is True, f"build: the pinned bytecode digest {b['luau']}")
packed = tmp / "packed.segapatch"
ok("pack", "mod", "pack", project, "--out", packed)
elsewhere = tmp / "elsewhere" / "deeper"
elsewhere.mkdir(parents=True)
ok("pack from another directory", "mod", "pack", "../../gameplay", "--out", "again.segapatch", cwd=elsewhere)
cli.check(built.read_bytes() == packed.read_bytes() == (elsewhere / "again.segapatch").read_bytes(), "build, pack and pack from another directory are byte-identical")
cli.check(sha(packed.read_bytes()) == PINNED_PACKAGE_SHA, f"the package bytes are pinned: {sha(packed.read_bytes())}")
inspect = ok("inspect", "mod", "inspect", packed)
cli.check(inspect["trust_class"] == "luau" and inspect["capabilities"] == ["host.log", "input.read", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "overlay.rgba", "state.mod"], "inspect lists the declared capabilities")

rc, t, p = cli.run("mod", "test", project, "--rom", rom, "--cc", CC, "--sdk-dir", SDK)
cli.check(rc == 0 and t and t["ok"] is True and all(s["passed"] for s in t["scenarios"]) and len(t["scenarios"]) == 2, f"mod test passes both scenarios: {t}")
inst = ok("install", "mod", "install", packed, "--rom", rom, "--library", tmp / "lib")
cli.check(inst["approval_kind"] == "consent" and inst["trust_class"] == "luau" and "memory.write.m68k.work_ram" in inst["install_sheet"], f"install: a Luau-only package needs consent and lists its memory capability: {inst}")
cli.check((tmp / "lib/mods" / (PINNED_IDENTITY + ".segapatch")).read_bytes() == packed.read_bytes(), "the installed package is the verified bytes")
var = ok("variant", "mod", "variant", "--library", tmp / "lib", "--identity", PINNED_IDENTITY, "--rom", rom)
cli.check(var.get("cache") == "miss", f"the first variant is a cache miss: {var.get('cache')}")

# ---------------------------------------------------------------- the programs
def build_program(name, package=None, identity=None, image=rom):
    out = tmp / name
    args = ["build", "--rom", image, "--output", out, "--cc", CC, "--runtime-dir", SDK / "genesis", "--optimize", "0", "--runtime-optimize", "1"]
    if package is not None:
        args += ["--luau-package", package, "--luau-identity", identity]
    p = subprocess.run([EXE, *map(str, args)], text=True, capture_output=True, timeout=900)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return p, out, status


van_p, van, van_status = build_program("vanilla")
mod_p, mod, mod_status = build_program("modded", packed, PINNED_IDENTITY)
cli.check(van_p.returncode == 0 and mod_p.returncode == 0, f"both programs build: {van_p.stdout[-200:]} {mod_p.stdout[-200:]}")
cli.check("luau" not in van_status and mod_status["luau"]["package_identity"] == PINNED_IDENTITY and mod_status["luau"]["bytecode_digest"] == PINNED_BYTECODE and mod_status["luau"]["effective_sha256"] == FIXTURE_SHA,
          f"status.json records the verified package, its bytecode and the effective image: {mod_status.get('luau')}")
cli.check(b"lua_newstate" not in (van / "game").read_bytes(), "a Vanilla program contains no Luau VM")

PAD = "10:1:0x20,20:1:0x00,30:1:0x08,40:1:0x00"      # C held for frames 10..19, Right held for frames 30..39


def run(out, pad=PAD, frames="64", extra=None, first="1"):
    work = tmp / ("cap-" + out.name)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    env = dict(os.environ, SEGARECOMP_CAPTURE_DIR=str(work), SEGARECOMP_CAPTURE_FIRST=first, SEGARECOMP_CAPTURE_COUNT=frames, SEGARECOMP_CAPTURE_SLICE="16", SEGARECOMP_CAPTURE_RAM="16:4",
               SEGARECOMP_PAD_SCRIPT=pad)
    env.update(extra or {})
    p = subprocess.run([str(out / "game"), "--instruction-budget", "4000000"], text=True, capture_output=True, env=env, timeout=120)   # bounded: explicit budget and timeout
    lines = {}
    for line in p.stderr.splitlines():
        tag, _, rest = line.partition(" ")
        if tag in ("CAPTURE_SUMMARY", "MOD_SUMMARY", "MOD_LOADED", "MOD_LOG", "MOD_REFUSED"):
            lines.setdefault(tag, []).append(json.loads(rest))
    return p, lines


def stop_run(out, extra=None, pad=PAD):
    """Runs to the program's own guest stop (the 90th turn), so every compared state is the program's final state."""
    return run(out, pad=pad, frames="1", extra=extra, first="200")


# ---------------------------------------------------------------- the gameplay effect, hand-derived
pv, lv = run(van)
pm, lm = run(mod)
cv, cm = lv["CAPTURE_SUMMARY"][0], lm["CAPTURE_SUMMARY"][0]
cli.check(cv["outcome"] == "window_complete" and cm["outcome"] == "window_complete" and "MOD_SUMMARY" not in lv, "both programs complete the 64-frame window; Vanilla reports no mod")
vanilla_ram = ["00000000"] * 30 + ["0000%04x" % k for k in range(1, 11)] + ["0000000a"] * 24   # Vanilla: only Right moves the position (frames 30..39); C does nothing
modded_ram = ["00000000"] * 10 + ["%04x0000" % (10 * k) for k in range(1, 11)] + ["00640000"] * 10 + ["0064%04x" % k for k in range(1, 11)] + ["0064000a"] * 24   # +10 score per frame while C is held
cli.check(cv["ram_frames"] == vanilla_ram, f"Vanilla's per-frame guest RAM equals the hand derivation: {cv['ram_frames'][8:14]}")
cli.check(cm["ram_frames"] == modded_ram, f"the mod's bonus: +10 per frame while C is held, then the score stays: {cm['ram_frames'][8:14]}")
cli.check(cv["digests"] == cm["digests"], "the picture is the same (the example changes the game state and draws a host overlay)")
ms = lm["MOD_SUMMARY"][0]
cli.check(ms["faulted"] is False and ms["committed_writes"] == 10 and ms["write_digest"] == PINNED_WRITE_DIGEST and ms["overlay_ops"] == 4 and ms["overlay_sha256"] == PINNED_OVERLAY and ms["nondeterministic"] is False,
          f"the mod committed exactly ten bonus writes and rendered its HUD (4 requests, pinned hash): {ms}")
cli.check(lm["MOD_LOADED"][0]["package_identity"] == PINNED_IDENTITY and lm["MOD_LOADED"][0]["ranges"] == 1 and lm["MOD_LOG"][0]["line"] == "gameplay example ready", f"the program loaded the verified package: {lm['MOD_LOADED']}")
sv, sm = stop_run(van)[1]["CAPTURE_SUMMARY"][0], stop_run(mod)[1]["CAPTURE_SUMMARY"][0]
cli.check(sv["outcome"] == "incomplete_guest_stop" and sm["outcome"] == "incomplete_guest_stop", "both run to the program's own guest stop")
cli.check(sv["final_work_ram_sha256"] == derived_state(0, 10) and sm["final_work_ram_sha256"] == derived_state(100, 10), "the final guest states equal the independent derivation (score 0/100, position 10)")

# ---------------------------------------------------------------- replay: identical input, identical everything; different input, different state
_, lm2 = run(mod)
cli.check(lm2["CAPTURE_SUMMARY"][0] == cm and lm2["MOD_SUMMARY"][0] == ms, "replay: the same input reproduces frame digests, RAM samples, final state, witness and overlay hash")
_, ln = run(mod, pad="10:1:0x20,25:1:0x00")
cli.check(ln["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] != cm["final_work_ram_sha256"] and ln["MOD_SUMMARY"][0]["write_digest"] != ms["write_digest"], "a different recording gives a different state and witness")

# ---------------------------------------------------------------- save/restore: mid-run state, a FRESH program, identical end
state_file = tmp / "mid.state"
_, lsave = stop_run(mod, {"SEGARECOMP_MOD_CHECKPOINT": "15", "SEGARECOMP_MOD_SAVE": str(state_file)})
ss = lsave["MOD_SUMMARY"][0]
cli.check(ss["roundtrip"]["status"] == "ok" and ss["roundtrip"]["frame"] == 15 and state_file.is_file(), f"checkpoint at frame 15: capture, restore into the same machine, capture again: {ss['roundtrip']}")
cli.check(lsave["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == sm["final_work_ram_sha256"] and ss["write_digest"] == PINNED_WRITE_DIGEST and ss["overlay_sha256"] == PINNED_OVERLAY, "the round trip loses nothing: the end of the run is unchanged")
_, lload = stop_run(mod, {"SEGARECOMP_MOD_LOAD": str(state_file)})
sl = lload["MOD_SUMMARY"][0]
cli.check(lload["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == sm["final_work_ram_sha256"], "a FRESH program restored from the frame-15 state ends in the identical guest state")
cli.check(sl["write_digest"] == ss["write_digest"] and sl["overlay_sha256"] == PINNED_OVERLAY and sl["loaded_state_sha256"] == ss["roundtrip"]["state_sha256"], f"... with the identical replay witness, the same HUD and the loaded state's digest: {sl}")
cli.check(lload["CAPTURE_SUMMARY"][0]["dispatches"] < lsave["CAPTURE_SUMMARY"][0]["dispatches"], "the restored program resumed from the saved point (it ran fewer dispatches)")
data = bytearray(state_file.read_bytes())
data[len(data) // 2] ^= 0xFF
bad = write(tmp / "corrupt.state", bytes(data))
pbad, lbad = stop_run(mod, {"SEGARECOMP_MOD_LOAD": str(bad)})
cli.check(lbad.get("MOD_REFUSED", [{}])[0].get("class") == "E_STATE_RESTORE" and pbad.returncode == 5 and "CAPTURE_SUMMARY" not in lbad, f"a corrupted state is refused before the guest runs: {lbad.get('MOD_REFUSED')}")
# a state saved by ANOTHER mod build is bound to that mod's identity and refused
other = tmp / "other"
shutil.copytree(project, other)
(other / "scripts/main.luau").write_text((other / "scripts/main.luau").read_text().replace("local BONUS = 10", "local BONUS = 11"))
other_pkg = tmp / "other.segapatch"
other_id = ok("pack the other mod", "mod", "pack", other, "--out", other_pkg)["package_identity"]
_, o_out, _ = build_program("other", other_pkg, other_id)
pforeign, lforeign = stop_run(o_out, {"SEGARECOMP_MOD_LOAD": str(state_file)})
cli.check(lforeign.get("MOD_REFUSED", [{}])[0].get("class") == "E_STATE_RESTORE" and pforeign.returncode == 5, f"another mod's state is refused: {lforeign.get('MOD_REFUSED')}")

# ---------------------------------------------------------------- failure isolation: a write outside the declared range faults the mod, never the guest
rogue = tmp / "rogue"
shutil.copytree(project, rogue)
(rogue / "scripts/main.luau").write_text('function post_input(frame: number)\n  sega.mem.write("m68k.work_ram", 0x20, 2, 99)\nend\nfunction save() return {} end\nfunction load(s) end\n')
rogue_pkg = tmp / "rogue.segapatch"
rogue_id = ok("pack the rogue mod", "mod", "pack", rogue, "--out", rogue_pkg)["package_identity"]
r_p, r_out, _ = build_program("rogue", rogue_pkg, rogue_id)
cli.check(r_p.returncode == 0, "the rogue mod builds (the range violation is a run-time fault, not a build error)")
_, lr = run(r_out)
rs = lr["MOD_SUMMARY"][0]
cli.check(rs["faulted"] is True and "cause=range" in rs["fault"] and rs["committed_writes"] == 0, f"the out-of-range write faults the mod deterministically: {rs['fault']}")
cli.check(lr["CAPTURE_SUMMARY"][0]["ram_frames"] == vanilla_ram and lr["CAPTURE_SUMMARY"][0]["digests"] == cv["digests"], "the guest continues unmodified: its state is Vanilla's, frame for frame")
cli.check(stop_run(r_out)[1]["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == sv["final_work_ram_sha256"], "and it ends in the Vanilla final state")

# ---------------------------------------------------------------- refusals: nothing runs with parts missing
def toml_with(project_dir, mutate):
    path = project_dir / "mod.toml"
    path.write_text(mutate(path.read_text()))


def derived(name, mutate):
    d = tmp / name
    shutil.copytree(project, d)
    toml_with(d, mutate)
    return d


refused("a range for an undeclared revision", "E_MEMORY_REVISION", "mod", "check", derived("m1", lambda t: t.replace('revision = "fixture"\nspace', 'revision = "nope"\nspace')))
refused("a range outside the 64 KiB", "E_MEMORY_RANGE", "mod", "check", derived("m2", lambda t: t.replace("offset = 16", "offset = 65535").replace("length = 2", "length = 2")))
refused("an empty range", "E_MEMORY_RANGE", "mod", "check", derived("m3", lambda t: t.replace("length = 2", "length = 0")))
refused("an unknown space", "E_MEMORY_SPACE", "mod", "check", derived("m4", lambda t: t.replace('space = "m68k.work_ram"', 'space = "m68k.rom"')))
refused("a bad access", "E_MEMORY_ACCESS", "mod", "check", derived("m5", lambda t: t.replace('access = "read-write"', 'access = "rw"')))
refused("write access without the capability", "E_CAPABILITY_INCONSISTENT", "mod", "check", derived("m6", lambda t: t.replace('"memory.write.m68k.work_ram", ', "")))
refused("a duplicated range", "E_MEMORY_DUPLICATE", "mod", "check", derived("m7", lambda t: t + '\n[[memory]]\nrevision = "fixture"\nspace = "m68k.work_ram"\noffset = 16\nlength = 2\naccess = "read-write"\n'))
refused("an unknown key", "E_FIELD_UNKNOWN", "mod", "check", derived("m8", lambda t: t.replace('access = "read-write"', 'access = "read-write"\nmode = "x"')))

# a memory capability with NO range for this revision is refused at BUILD (the capability alone never authorizes an offset)
no_range = derived("no_range", lambda t: t.split("# The score word")[0])
nr_pkg = tmp / "no_range.segapatch"
nr_id = ok("pack without ranges", "mod", "pack", no_range, "--out", nr_pkg)["package_identity"]
p, out, status = build_program("no_range_program", nr_pkg, nr_id)
cli.check(p.returncode != 0 and status.get("diagnostic") == "E_RANGE_DECLARATION" and not (out / "game").exists(), f"no declared range: the build is refused and leaves no executable: {status.get('diagnostic')}")
# a range declared for ANOTHER revision does not authorize this image
other_rev = derived("other_rev", lambda t: t.replace('[[target.revision]]\nlabel = "fixture"', '[[target.revision]]\nlabel = "alt"\nsha256 = "' + "ab" * 32 + '"\n\n[[target.revision]]\nlabel = "fixture"').replace('revision = "fixture"\nspace', 'revision = "alt"\nspace'))
or_pkg = tmp / "other_rev.segapatch"
or_id = ok("pack the other-revision mod", "mod", "pack", other_rev, "--out", or_pkg)["package_identity"]
p, out, status = build_program("other_rev_program", or_pkg, or_id)
cli.check(p.returncode != 0 and status.get("diagnostic") == "E_RANGE_DECLARATION" and not (out / "game").exists(), f"a range of another revision is refused for this image: {status.get('diagnostic')}")
# a capability the scheduled path does not host
frame_ram = derived("frame_ram", lambda t: t.replace('"overlay.rgba", ', '"overlay.rgba", "frame.work_ram", '))
fr_pkg = tmp / "frame_ram.segapatch"
fr_id = ok("pack the frame.work_ram mod", "mod", "pack", frame_ram, "--out", fr_pkg)["package_identity"]
p, out, status = build_program("frame_ram_program", fr_pkg, fr_id)
cli.check(p.returncode != 0 and status.get("diagnostic") == "E_LIVE_CAPABILITY" and not (out / "game").exists(), f"an unsupported capability is refused: {status.get('diagnostic')}")
# the wrong image
wrong = write(tmp / "wrong.rom", fixture.make_rom()[:-2] + b"\x00\x00")
p, out, status = build_program("wrong_image", packed, PINNED_IDENTITY, image=wrong)
cli.check(p.returncode != 0 and status.get("diagnostic") == "LUAU_IMAGE_MISMATCH" and not (out / "game").exists(), f"another image is refused: {status.get('diagnostic')}")

# the pinned example the public repository publishes is exactly this tree
cli.check(sorted(str(p.relative_to(EXAMPLE)) for p in EXAMPLE.rglob("*") if p.is_file() and "__pycache__" not in p.parts) == ["README.md", "fixture_rom.py", "mod.toml", "scripts/main.luau", "tests.toml"], "the example tree is the published set of files")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_gameplay_example_test")
