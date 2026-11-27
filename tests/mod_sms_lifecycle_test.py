#!/usr/bin/env python3
"""SEG-147-T004/T005/T006/T011 (ADR 0325): Master System mods end to end with FABRICATED inputs only (platforms/master-system/sdk/examples/gameplay and gameplay-native; no commercial image).

The developer workflow runs on the real product: check -> build -> pack -> test -> install -> variant, then the REAL generated-native programs (the unmodified producer; the Modded ones link the ONE
Master System mod scheduler, the presentation compositor and either the Luau VM with the verified scripts or the package's C11 provider), every run bounded by --frames, a cycle budget and a timeout.

Proves: (1) deterministic packaging (pinned identity, content, bytes, bytecode); (2) the gameplay effect against an independent hand derivation, and a Vanilla program whose guest output equals the pinned
golden recorded from the product BEFORE the mod layer existed; (3) Luau and C11 identical (state, write witness, every presented picture); (4) replay, and independence from how the host slices the run;
(5) presentation is presentation only: a mod that only transforms the picture leaves every guest artifact bit-identical, an empty one presents the Vanilla picture, composed pixels equal an independent
rule; (6) save/restore: round trip, a state loaded into a FRESH program ends identically; corrupted and foreign states are refused before the guest runs; (7) faults (undeclared range, illegal phase,
render mutation, read-only space, bad presentation request, budget) disable only the mod, with the SAME diagnostic from Luau and C11, and the guest ends exactly as Vanilla; (8) every undeclared,
unsupported or mapper-incompatible declaration is refused with a typed class before a program exists.
usage: mod_sms_lifecycle_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

if sys.platform == "win32":   # the generated-native program build with a C++ Luau VM is exercised on the Linux and macOS runners
    print("mod_sms_lifecycle_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
EXE = cli.exe
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLES = ROOT / "platforms/master-system/sdk/examples"
spec = importlib.util.spec_from_file_location("sms_runner_fixture", EXAMPLES / "gameplay/fixture_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-sms-lifecycle-"))

# ---- pinned values (a change of the example, the writer, the packer, the trusted compiler or the compositor changes them on purpose) ----
FIXTURE_SHA = "f5a52a6d439868bc80a5b49d39b75811d1dbaa03b60c5e6b4e0e748c2c522a4a"
PINNED_IDENTITY = "c6e000f4dcb37f0e3030e5520df823bacdc0b8d0c36851a2f46e574377458074"
PINNED_CONTENT = "af5457df32f489e385a38b3107a55613d1416222d040e5d7c54e015cc14f7174"
PINNED_PACKAGE_SHA = "dd1358fad798ef142eba06659cc66de49578b1dff19a5f84aaf18ac6a17f8396"
PINNED_NATIVE_IDENTITY = "b811ee33174d3449e8e3ebf9208cc61a730ac586832d3013000a311e5642790e"
PINNED_BYTECODE = "08d79fb0c856685e57bddcd061aacf139bc9aa73c1fb33d2857f3b6399e29441"
PINNED_WRITE_DIGEST = "846f6bf30197fbb2b0dcc112260286bb53c3364bfffa43ecf3687abb17f811cc"
PINNED_PRESENTATION_STREAM = "678b54b108b207cc1cf2e336efe31ade46d8b0c135c3021efd1ed85c45366fdc"
# the Vanilla guest of the input below. Recorded with the product's Master System runtime of main@e8cbe58 (before this layer) and equal to the current one: a Vanilla program is unchanged.
VANILLA_GOLDEN_DIGEST = "29bcab040daf2f601a6fb72201b24fc416fa3e4c47f9d0cd350f946ee1636413"
VANILLA_GOLDEN_FRAMES = "a6e9e608e68696f93568ebe89b90790542020a3c45da7c3f45c79e6035249730"


def ok(what, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-300:]}")
    return doc or {}


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


rom = write(tmp / "runner.sms", fixture.build())
cli.check(sha(rom.read_bytes()) == FIXTURE_SHA and fixture.build() == fixture.build(), f"the fixture cartridge is deterministic and pinned: {sha(rom.read_bytes())}")


def project(name, source=EXAMPLES / "gameplay", mutate=None, script=None, native=None):
    d = tmp / name
    shutil.copytree(source, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
    if mutate:
        toml = d / "mod.toml"
        toml.write_text(mutate(toml.read_text()))
    if script is not None:
        (d / "scripts/main.luau").write_text(script)
    if native is not None:
        (d / "src/main.c").write_text(native)
    return d


def pack(name, directory):
    out = tmp / (name + ".segapatch")
    doc = ok(f"pack {name}", "mod", "pack", directory, "--out", out)
    return out, doc.get("package_identity", "")


# ---------------------------------------------------------------- check -> build -> pack -> test -> install -> variant
main_project = project("gameplay")
cli.check(f'sha256 = "{FIXTURE_SHA}"' in (main_project / "mod.toml").read_text(), "mod.toml names the fixture cartridge")
chk = ok("check", "mod", "check", main_project)
cli.check(chk.get("package_identity") == PINNED_IDENTITY and chk.get("content_digest") == PINNED_CONTENT and chk.get("trust_class") == "luau", f"pinned identity: {chk.get('package_identity')} {chk.get('content_digest')}")
built = tmp / "built.segapatch"
b = ok("build", "mod", "build", main_project, "--out", built)
cli.check(b.get("luau", {}).get("bytecode_digest") == PINNED_BYTECODE, f"pinned bytecode: {b.get('luau')}")
packed, identity = pack("packed", main_project)
again = tmp / "elsewhere" / "deeper"
again.mkdir(parents=True)
ok("pack from another directory", "mod", "pack", "../../gameplay", "--out", "again.segapatch", cwd=again)
cli.check(built.read_bytes() == packed.read_bytes() == (again / "again.segapatch").read_bytes() and sha(packed.read_bytes()) == PINNED_PACKAGE_SHA, f"packaging is deterministic and pinned: {sha(packed.read_bytes())}")
t = ok("mod test", "mod", "test", main_project, "--rom", rom, "--cc", CC, "--sdk-dir", SDK)
cli.check(len(t.get("scenarios", [])) == 2 and all(s["passed"] for s in t["scenarios"]), f"mod test passes both scenarios: {t}")
inst = ok("install", "mod", "install", packed, "--rom", rom, "--library", tmp / "lib")
cli.check(inst.get("approval_kind") == "consent" and "memory.write.z80.work_ram" in inst.get("install_sheet", ""), f"install: consent and the listed memory capability: {inst}")
cli.check((tmp / "lib/mods" / (identity + ".segapatch")).read_bytes() == packed.read_bytes(), "the installed package is the verified bytes")
cli.check(ok("variant", "mod", "variant", "--library", tmp / "lib", "--identity", identity, "--rom", rom).get("cache") == "miss", "the first variant is a cache miss")
native_project = project("gameplay-native", EXAMPLES / "gameplay-native")
shutil.copy(main_project / "tests.toml", native_project / "tests.toml")
native_package, native_identity = pack("native", native_project)
cli.check(native_identity == PINNED_NATIVE_IDENTITY, f"pinned native identity: {native_identity}")
ok("check native sources", "mod", "check", native_project)
ok("build native sources", "mod", "build", native_project, "--out", tmp / "native-built.segapatch", "--cc", CC, "--sdk-dir", SDK)
t = ok("mod test (native)", "mod", "test", native_project, "--rom", rom, "--cc", CC, "--sdk-dir", SDK)
cli.check(len(t.get("scenarios", [])) == 2 and all(s["passed"] for s in t["scenarios"]), f"the SAME expectations pass for the C11 twin: {t}")
refused("diff-run is Genesis only", "E_DIFF_PLATFORM", "mod", "diff-run", packed, "--rom", rom, "--cc", CC, "--sdk-dir", SDK, "--frames", "2", "--budget", "100", "--timeout", "5")


initialised = tmp / "initialised"
ok("mod init", "mod", "init", initialised, "--platform", "master-system", "--id", "my-sms-mod", "--template", "luau", "--rom-sha256", FIXTURE_SHA)
ok("the fresh template checks", "mod", "check", initialised)
ok("... builds", "mod", "build", initialised, "--out", tmp / "initialised-built.segapatch")
ok("... and packs", "mod", "pack", initialised, "--out", tmp / "initialised.segapatch")
wrong_tests = project("wrong-expectation")
toml_path = wrong_tests / "tests.toml"
write(toml_path, toml_path.read_text().replace('expect_ram_frames = ["00000000000001"', 'expect_ram_frames = ["ff000000000001"', 1).encode("utf-8"))
rc, t, _p = cli.run("mod", "test", wrong_tests, "--rom", rom, "--cc", CC, "--sdk-dir", SDK, "--scenario", "run-while-button-2-held")
cli.check(rc == 1 and t and t.get("class") == "E_TEST_FAILED" and t["scenarios"][0]["first_divergence"].get("frame") == 0, f"a failing Master System expectation names its 0-based machine frame: {t and t.get('scenarios', [{}])[0].get('first_divergence')}")


# ---------------------------------------------------------------- the programs
def build_program(name, package=None, ident=None, native=False, image=rom, extra=()):
    out = tmp / ("p-" + name)
    args = [EXE, "build", "--rom", image, "--output", out, "--cc", CC, "--runtime-dir", SDK / "master-system", "--platform", "master-system", "--optimize", "0"]
    if package is None:
        args += ["--mapper", "rom_only"]
    else:
        args += ["--native-package" if native else "--luau-package", package, "--native-identity" if native else "--luau-identity", ident]
    args += list(extra)
    p = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=900)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return p, out, status


def program(name, *args, **kwargs):
    p, out, status = build_program(name, *args, **kwargs)
    if p.returncode != 0:
        print((out / "build.log").read_text()[-3000:])
        raise SystemExit(f"mod_sms_lifecycle_test: {name} did not build: {p.stdout[-300:]}")
    return out, status


def run(out, script, frames=40, extra=(), tag="a"):
    work = tmp / (out.name + "-run-" + tag)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    (work / "input.txt").write_text(script)
    p = subprocess.run([str(out / "game"), "--frames", str(frames), "--cycle-budget", "10000000", "--input", str(work / "input.txt"), "--artifacts", str(work), *extra], text=True,
                       capture_output=True, timeout=120)   # bounded: frames, cycle budget and timeout
    lines = {}
    for line in p.stderr.splitlines():
        key, _, rest = line.partition(" ")
        if key in ("MOD_SUMMARY", "MOD_LOADED", "MOD_LOG", "MOD_REFUSED"):
            lines.setdefault(key, []).append(json.loads(rest))
    read = lambda n: (work / n).read_bytes() if (work / n).exists() else None   # noqa: E731
    return {"rc": p.returncode, "out": p.stdout, "lines": lines, "mod": (lines.get("MOD_SUMMARY") or [None])[0], "read": read}


GUEST_ARTIFACTS = ("state.sha256", "ram.bin", "vram.bin", "cram.bin", "frames.txt", "framebuffer.bin", "audio.sha256", "vdp.trace", "irq.trace")
guest = lambda r: {n: r["read"](n) for n in GUEST_ARTIFACTS}   # noqa: E731

vanilla, vstatus = program("vanilla")
luau, lstatus = program("luau", packed, identity)
native, nstatus = program("native", native_package, native_identity, native=True)
cli.check("luau" not in vstatus and "native_mod" not in vstatus and lstatus["luau"]["package_identity"] == identity and lstatus["luau"]["bytecode_digest"] == PINNED_BYTECODE
          and nstatus["native_mod"]["package_identity"] == native_identity and nstatus["native_mod"]["abi"] == 2, f"status.json records the verified mod: {lstatus.get('luau')} {nstatus.get('native_mod')}")
vbytes = (vanilla / "game").read_bytes()
cli.check(b"lua_newstate" not in vbytes and b"E_MOD_FAULT" not in vbytes and b"lua_newstate" not in (native / "game").read_bytes(), "a Vanilla program links no mod layer and no VM; a native mod program no VM")

INPUT = "20 ----1- ------ -\n24 ----12 ------ -\n28 ------ ------ -\n30 ---R-- ------ -\n33 ------ ------ -\n"
OBSERVE = ("--mod-ram", "16:7")
v = run(vanilla, INPUT)
a = run(luau, INPUT, extra=OBSERVE)
c = run(native, INPUT, extra=OBSERVE)
cli.check(v["rc"] == a["rc"] == c["rc"] == 0 and v["mod"] is None and a["mod"] and c["mod"], f"all three programs run: {v['rc']} {a['rc']} {c['rc']}")


def derived(modded):
    """The seven bytes at $C010..$C016 after each frame, from the two rules alone (the program handles its first VBlank in frame 11, after it wiped VRAM)."""
    pad, score, position, handled, seen, out = 0, 0, 0, 0, 0, []
    events = {20: 0x10, 24: 0x30, 28: 0x00, 30: 0x08, 33: 0x00}
    for frame in range(40):
        pad = events.get(frame, pad)
        read, speed = pad, 1
        if modded and pad & 0x20:
            read, speed = (pad & ~0x20) | 0x08, 3
        if frame >= 11:
            score += 1 if read & 0x10 else 0
            position += speed if read & 0x08 else 0
            seen, handled = read, handled + 1
        out.append((score.to_bytes(2, "little") + position.to_bytes(2, "little") + bytes([seen, handled, speed])).hex())
    return out


cli.check(a["mod"]["ram_frames"] == derived(True), f"the gameplay effect equals the hand derivation: {a['mod']['ram_frames'][22:30]}")
cli.check(v["read"]("ram.bin")[16:23].hex() == derived(False)[-1] and derived(False)[-1] != derived(True)[-1], "Vanilla ends where its own rules put it (position 3, not 15)")
cli.check(v["read"]("state.sha256").decode().strip() == VANILLA_GOLDEN_DIGEST and sha(v["read"]("frames.txt")) == VANILLA_GOLDEN_FRAMES,
          f"Vanilla is unchanged (golden machine state and frame hashes): {v['read']('state.sha256')} {sha(v['read']('frames.txt'))}")
ms = a["mod"]
cli.check(ms["faulted"] is False and ms["nondeterministic"] is False and ms["write_digest"] == PINNED_WRITE_DIGEST and ms["presentation_stream_sha256"] == PINNED_PRESENTATION_STREAM and ms["committed_writes"] == 41,
          f"pinned replay witness and presented stream: {ms['write_digest']} {ms['presentation_stream_sha256']} {ms['committed_writes']}")
cli.check(a["lines"]["MOD_LOADED"][0]["package_identity"] == identity and a["lines"]["MOD_LOG"][0]["line"] == "sms gameplay example ready", "the program loaded the verified package and ran reset once")

# ---------------------------------------------------------------- Luau == C11
same_fields = ("faulted", "fault", "committed_writes", "write_digest", "presented_frames", "active_frames", "presentation_stream_sha256", "last_presented_sha256", "final_work_ram_sha256", "digests", "ram_frames")
cli.check({k: c["mod"][k] for k in same_fields} == {k: ms[k] for k in same_fields} and guest(c) == guest(a), "Luau and C11 are identical: guest artifacts, write witness and every presented picture")

# ---------------------------------------------------------------- replay and host slicing
a2 = run(luau, INPUT, extra=OBSERVE, tag="b")
cli.check(a2["mod"] == ms and guest(a2) == guest(a), "replay: the same input reproduces everything")
for i, slicing in enumerate((("--slice-cycles", "977"), ("--slice-seed", "7"))):
    sliced = run(luau, INPUT, extra=(*OBSERVE, *slicing), tag=f"slice{i}")
    cli.check(sliced["mod"] == ms and sliced["read"]("state.sha256") == a["read"]("state.sha256"), f"phases do not depend on how the host slices the run ({slicing})")
other = run(luau, "20 ----12 ------ -\n32 ------ ------ -\n", extra=OBSERVE, tag="other")
cli.check(other["mod"]["write_digest"] != ms["write_digest"] and other["read"]("state.sha256") != a["read"]("state.sha256"), "different input gives different state and witness")

# ---------------------------------------------------------------- presentation is presentation only
def rgb(value):
    return ((value & 3) * 85, ((value >> 2) & 3) * 85, ((value >> 4) & 3) * 85)


def vanilla_picture(r):
    return b"".join(bytes(rgb(x)) for x in r["read"]("framebuffer.bin"))


PRESENT_ONLY = '''function render(frame)
  sega.present.color(0x30, 0x123456)
  sega.present.scroll(3, 2)
  sega.present.colors("00" .. "03" .. "0c" .. string.rep("00", 13))
  sega.present.pattern(0, "12000000" .. string.rep("00000000", 7))
  sega.present.sprite(100, 50, 1, 1, 0, 0)
  sega.present.sprite(120, 50, 1, 1, 0, 3)
  sega.overlay.rect(200, 100, 4, 4, 0xff000080)
  local v = sega.frame.sms_vdp("cram", 0)
  if v ~= 0x30 then sega.overlay.rect(0, 0, 8, 8, 0xffffffff) end
end
'''
only = lambda t: t.replace('required = ["frame.work_ram", "host.log", "input.intercept", "input.read", "memory.write.z80.work_ram", "overlay.rgba", "present.transform", "state.mod"]',   # noqa: E731
                           'required = ["frame.sms_vdp", "overlay.rgba", "present.transform"]').split("# The ONE byte")[0] + '[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n'
present_package, present_id = pack("present", project("present", mutate=only, script=PRESENT_ONLY))
present_program, _ = program("present", present_package, present_id)
p = run(present_program, INPUT, extra=OBSERVE)
cli.check(p["rc"] == 0 and p["mod"]["faulted"] is False and p["mod"]["committed_writes"] == 0 and p["mod"]["active_frames"] == 40, f"the presentation-only mod renders every frame: {p['mod'] and p['mod']['fault']}")
cli.check(guest(p) == guest(v), "emulated VDP and guest state are bit-identical with and without the presentation transforms (state, RAM, VRAM, CRAM, frame hashes, framebuffer, audio, traces)")
picture = p["read"]("presented.rgb")
cli.check(picture is not None and picture != vanilla_picture(v), "the presented picture differs")
pixel = lambda x, y: tuple(picture[(y * 256 + x) * 3:(y * 256 + x) * 3 + 3])   # noqa: E731
cli.check(pixel(2, 100) == (0, 0, 0) and pixel(100, 1) == (0, 0, 0) and pixel(3, 2) == (0x12, 0x34, 0x56), "scroll (3,2): vacated pixels are black; the sky colour value is presented as the override")
cli.check(pixel(100, 50) == rgb(0x03) and pixel(101, 50) == rgb(0x0C) and pixel(102, 50) == (0x12, 0x34, 0x56), "authored sprite: colour 1 and 2 from the mod's own palette, index 0 transparent")
cli.check(pixel(120 + 7, 50 + 7) == rgb(0x03) and pixel(120 + 6, 50 + 7) == rgb(0x0C), "both flips mirror the authored sprite")
blended = tuple((c0 * 128 + c1 * 127 + 127) // 255 for c0, c1 in zip((255, 0, 0), (0x12, 0x34, 0x56)))
cli.check(pixel(200, 100) == blended and pixel(204, 100) == (0x12, 0x34, 0x56), f"overlay: straight alpha over the transformed picture: {pixel(200, 100)} vs {blended}")
cli.check(pixel(8, 8) == (0x12, 0x34, 0x56), "the render-phase VDP copy reads the guest's real CRAM (not the presentation override)")

empty_package, empty_id = pack("empty", project("empty", mutate=only, script="function render(frame) end\n"))
empty_program, _ = program("empty", empty_package, empty_id)
e = run(empty_program, INPUT, extra=OBSERVE)
cli.check(guest(e) == guest(v) and e["mod"]["active_frames"] == 0 and e["read"]("presented.rgb") == vanilla_picture(v), "a mod that requests nothing presents the Vanilla picture bit for bit and leaves the guest alone")

# ---------------------------------------------------------------- save states (SEG-147-T005): round trip, a FRESH program, refusals
state = tmp / "mid.state"
saved = run(luau, INPUT, extra=(*OBSERVE, "--mod-checkpoint", "26", "--mod-state-save", str(state)), tag="save")
ss = saved["mod"]
cli.check(ss["roundtrip"]["status"] == "ok" and ss["roundtrip"]["frame"] == 26 and state.is_file() and state.read_bytes()[:7] == b"SMSSTAT", f"checkpoint while running: capture, restore, capture again: {ss.get('roundtrip')}")
cli.check(guest(saved) == guest(a) and ss["write_digest"] == ms["write_digest"] and ss["last_presented_sha256"] == ms["last_presented_sha256"], "the round trip loses nothing")
loaded = run(luau, INPUT, extra=(*OBSERVE, "--mod-state-load", str(state)), tag="load")
sl = loaded["mod"]
cli.check(loaded["rc"] == 0 and sl["loaded_state_sha256"] == ss["roundtrip"]["state_sha256"], f"a fresh program loads the state: {loaded['lines']}")
cli.check(all(loaded["read"](n) == a["read"](n) for n in ("ram.bin", "vram.bin", "cram.bin", "framebuffer.bin", "presented.rgb")) and sl["write_digest"] == ms["write_digest"] and sl["last_presented_sha256"] == ms["last_presented_sha256"],
          "... and ends with the identical RAM, VDP memory, picture, replay witness and presented picture (the restored mod state: its RUN counter is drawn in the HUD)")
# The whole canonical state (CPU, RAM, mapper, VDP, renderer, PSG, pad and MODSTATE) at a LATER barrier: the resumed run equals the uninterrupted one. (state.sha256 and the traces additionally cover
# diagnostic trace bookkeeping of the frames THIS process ran, so they are compared only between whole runs.)
whole = run(luau, INPUT, extra=("--mod-checkpoint", "38"), tag="whole")["mod"]["roundtrip"]
resumed = run(luau, INPUT, extra=("--mod-state-load", str(state), "--mod-checkpoint", "38"), tag="resumed")["mod"]["roundtrip"]
cli.check(whole["status"] == resumed["status"] == "ok" and whole["state_sha256"] == resumed["state_sha256"], f"canonical state equality at frame 38: {whole} vs {resumed}")
native_state = tmp / "native.state"
run(native, INPUT, extra=("--mod-checkpoint", "26", "--mod-state-save", str(native_state)), tag="nsave")
nl = run(native, INPUT, extra=(*OBSERVE, "--mod-state-load", str(native_state)), tag="nload")
cli.check(nl["mod"] and nl["mod"]["last_presented_sha256"] == ms["last_presented_sha256"] and nl["read"]("ram.bin") == a["read"]("ram.bin"), "the C11 twin restores its own state to the identical end")


def refused_load(what, out, path):
    r = run(out, INPUT, extra=("--mod-state-load", str(path)), tag="refused")
    cli.check(r["rc"] == 5 and r["lines"].get("MOD_REFUSED", [{}])[0].get("class") == "E_STATE_RESTORE" and r["read"]("state.sha256") is None, f"{what}: refused before the guest runs: {r['rc']} {r['lines'].get('MOD_REFUSED')}")
    return r["lines"].get("MOD_REFUSED", [{}])[0].get("reason", "")


bad = bytearray(state.read_bytes())
bad[len(bad) // 2] ^= 0xFF
refused_load("a corrupted state", luau, write(tmp / "corrupt.state", bytes(bad)))
changed_script = (EXAMPLES / "gameplay/scripts/main.luau").read_text().replace("running then 3 else 1", "running then 4 else 1")
changed_package, changed_id = pack("changed", project("changed", script=changed_script))
changed_program, _ = program("changed", changed_package, changed_id)
cli.check(changed_id != identity, "a changed script is another identity")
refused_load("a state of a changed mod identity", changed_program, state)
refused_load("another package's state (the native twin's) in the Luau program", luau, native_state)

# ---------------------------------------------------------------- faults: only the mod is disabled; Luau and C11 give the SAME diagnostic; the guest ends as Vanilla
FAULTS = {   # mode (selected by player 2's pad bits in `input`, applied in the named phase): (cause, phase, Luau statement, C statement)
    1: ("range", "post-input", 'sega.mem.write("z80.work_ram", 0x40, 1, 9)', "host->write(1U, 0x40U, 1U, 9U);"),
    2: ("phase", "input", 'sega.mem.write("z80.work_ram", 0x16, 1, 9)', "host->write(1U, 0x16U, 1U, 9U);"),
    3: ("phase", "render", 'sega.mem.write("z80.work_ram", 0x16, 1, 9)', "host->write(1U, 0x16U, 1U, 9U);"),
    4: ("phase", "post-input", "sega.input.set(1, 0)", "host->input_set(1U, 0U);"),
    5: ("readonly", "post-input", 'sega.mem.write("z80.cart_ram", 0, 1, 9)', "host->write(2U, 0U, 1U, 9U);"),
    6: ("range", "render", "sega.present.scroll(999, 0)", "(void)features->render->scroll(999, 0);"),
    7: ("budget", "render", "for i = 1, 65 do sega.overlay.rect(0, 0, 1, 1, 0xffffffff) end", "{ int i; for (i = 0; i < 65; ++i) (void)features->render->overlay_rect(0, 0, 1, 1, 0xffffffffU); }"),
    8: ("phase", "post-input", "sega.overlay.rect(0, 0, 1, 1, 0xffffffff)", "(void)features->render->overlay_rect(0, 0, 1, 1, 0xffffffffU);"),
    9: ("range", "post-input", 'sega.mem.write("z80.work_ram", 0x16, 4, 9)', "host->write(1U, 0x16U, 4U, 9U);"),
    10: ("space", "post-input", 'sega.mem.write("m68k.work_ram", 0x16, 1, 9)', "host->write(77U, 0x16U, 1U, 9U);"),
}
PHASE_FN = {"input": "input", "post-input": "post_input", "render": "render"}
luau_faults = "local mode = 0\n"
for phase_name, fn in PHASE_FN.items():
    luau_faults += f"function {fn}(frame)\n" + ("  if frame >= 22 then mode = sega.input.get(2) end\n" if fn == "input" else "")
    for mode, (_, where, statement, _c) in FAULTS.items():
        if where == phase_name:
            if fn == "post_input":
                luau_faults += f"  if mode == {mode} then sega.mem.write(\"z80.work_ram\", 0x16, 1, 3) sega.log(\"staged\") {statement} end\n"
            else:
                luau_faults += f"  if mode == {mode} then {statement} end\n"
    luau_faults += "end\n"
c_faults = '''#include <segarecomp_mod_sms.h>
static uint32_t mode;
static void phase(void *state, const SegaModHost *host) {
  const SegaMasterSystemFeatures *features = (const SegaMasterSystemFeatures *)host->platform_features;
  (void)state;
  (void)features;
  if (host->phase == SEGA_MOD_PHASE_INPUT) { if (host->frame >= 22U && host->input_get(2U, &mode) != SEGA_MOD_OK) return; }
'''
for phase_name, enum in (("input", "INPUT"), ("post-input", "POST_INPUT"), ("render", "RENDER")):
    c_faults += f"  if (host->phase == SEGA_MOD_PHASE_{enum}) {{\n"
    for mode, (_, where, _l, statement) in FAULTS.items():
        if where == phase_name:
            prefix = "host->write(1U, 0x16U, 1U, 3U); host->log(\"staged\", 6U); " if enum == "POST_INPUT" else ""
            c_faults += f"    if (mode == {mode}U) {{ {prefix}{statement} }}\n"
    c_faults += "  }\n"
c_faults += '''}
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 5U, SEGA_MOD_PLATFORM, "faults", 1U, 0, phase, 0, 0, 0, {0}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { if (state_out) *state_out = 0; return &provider; }
'''
fault_caps = lambda t: t.replace('required = ["frame.work_ram", "host.log", "input.intercept", "input.read", "memory.write.z80.work_ram", "overlay.rgba", "present.transform", "state.mod"]',   # noqa: E731
                                 'required = ["host.log", "input.intercept", "input.read", "memory.write.z80.work_ram", "overlay.rgba", "present.transform"]').replace("[state]\nschema = 1\n", "").replace('id = "sms-runner-', 'id = "x-sms-runner-')
fl_package, fl_id = pack("faults-luau", project("faults-luau", mutate=fault_caps, script=luau_faults))
fn_package, fn_id = pack("faults-native", project("faults-native", EXAMPLES / "gameplay-native", mutate=fault_caps, native=c_faults))
fl, _ = program("faults-luau", fl_package, fl_id)
fn_program, _ = program("faults-native", fn_package, fn_id, native=True)
keys = lambda m: "".join(n if m & (1 << i) else "-" for i, n in enumerate("UDLR12"))   # noqa: E731
for mode, (cause, where, _l, _c) in FAULTS.items():
    script = "20 ----1- ------ -\n22 ----1- %s -\n" % keys(mode)
    base = run(vanilla, script, tag=f"fv{mode}")
    results = {}
    for name, out in (("luau", fl), ("native", fn_program)):
        r = run(out, script, tag=f"f{mode}")
        results[name] = r
        m = r["mod"] or {}
        mod_id = r["lines"].get("MOD_LOADED", [{}])[0].get("mod", "")
        want = f"E_MOD_FAULT cause={cause} mod={mod_id} phase={where} frame=22"
        cli.check(r["rc"] == 0 and m.get("faulted") is True and m.get("fault") == want and m.get("committed_writes") == 0, f"fault {mode} ({name}): {want!r}, got {m.get('fault')!r} writes={m.get('committed_writes')}")
        cli.check(guest(r) == guest(base), f"fault {mode} ({name}): the guest ends exactly as Vanilla (the staged write of the faulting callback is discarded)")
        cli.check([entry["line"] for entry in r["lines"].get("MOD_LOG", [])] == [want], f"fault {mode} ({name}): the staged log line is discarded, the diagnostic is emitted once")
    cli.check(results["luau"]["mod"]["write_digest"] == results["native"]["mod"]["write_digest"], f"fault {mode}: same witness from both")

# ---------------------------------------------------------------- refusals before a program exists
def derived_project(name, mutate):
    return project(name, mutate=mutate)


def memory(space="z80.work_ram", offset=22, length=1, access="write"):
    return lambda t: t.replace('space = "z80.work_ram"\noffset = 22\nlength = 1\naccess = "write"', f'space = "{space}"\noffset = {offset}\nlength = {length}\naccess = "{access}"')


refused("a range outside the 8 KiB", "E_MEMORY_RANGE", "mod", "check", derived_project("m1", memory(offset=8191, length=2)))
refused("a Genesis space", "E_MEMORY_SPACE", "mod", "check", derived_project("m2", memory(space="m68k.work_ram")))
refused("write access without the capability", "E_CAPABILITY_INCONSISTENT", "mod", "check", derived_project("m3", lambda t: t.replace('"memory.write.z80.work_ram", ', "")))
cart = lambda access: lambda t: memory(space="z80.cart_ram", access=access)(t).replace('"memory.write.z80.work_ram", ', '"memory.read.z80.cart_ram", "memory.write.z80.work_ram", ')   # noqa: E731
refused("cartridge RAM is never mod-writable", "E_MEMORY_ACCESS", "mod", "check", derived_project("m4", cart("write")))
refused("rom_only has no cartridge RAM", "E_MEMORY_SPACE", "mod", "check", derived_project("m5", cart("read")))
refused("undeclared capability use is rejected statically", "E_LUAU_CAPABILITY", "mod", "check", project("s1", mutate=lambda t: t.replace('"present.transform", ', "")))
for name, call in (("sprite", 'sega.sprite.add("authored", "above_all", 0, 0, 1, 1, 0, 0, 0)'), ("audio", 'sega.audio.play_sfx("x")')):
    refused(f"another platform's API ({name})", "E_LUAU_API", "mod", "check", project("s-" + name, script=f"function render(f)\n  {call}\nend\n"))


def build_refused(what, code, package, ident, native_flag=False, image=rom, extra=()):
    p_, out, status = build_program("refused-" + code.lower() + "-" + ident[:6], package, ident, native=native_flag, image=image, extra=extra)
    cli.check(p_.returncode != 0 and status.get("diagnostic") == code and not (out / "game").exists(), f"{what}: {code}, got {status.get('diagnostic')}")


no_range_package, no_range_id = pack("no-range", project("no-range", mutate=lambda t: t.split("# The ONE byte")[0] + '[state]\nschema = 1\n\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n'))
build_refused("a memory capability without a range for this revision", "E_RANGE_DECLARATION", no_range_package, no_range_id)
wrong = write(tmp / "wrong.sms", fixture.build()[:-1] + b"\x00" if fixture.build()[-1] else fixture.build()[:-1] + b"\x01")
build_refused("another cartridge image", "LUAU_IMAGE_MISMATCH", packed, identity, image=wrong)
build_refused("a conflicting mapper declaration", "MOD_MAPPER_MISMATCH", packed, identity, extra=("--mapper", "sega"))
build_refused("a changed package", "LUAU_IDENTITY_CHANGED", packed, "0" * 64)

for name, expected in (("gameplay", ["README.md", "fixture_rom.py", "mod.toml", "scripts/main.luau", "tests.toml"]), ("gameplay-native", ["README.md", "mod.toml", "src/main.c"])):
    tree = sorted(str(f.relative_to(EXAMPLES / name)) for f in (EXAMPLES / name).rglob("*") if f.is_file() and "__pycache__" not in f.parts)
    cli.check(tree == expected, f"the {name} example tree is the published set of files: {tree}")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_sms_lifecycle_test")
