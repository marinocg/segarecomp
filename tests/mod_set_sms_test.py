#!/usr/bin/env python3
"""SEG-141-T007 (ADR 0327): BUILT Master System mod sets on the shared composition engine and set dispatcher, with FABRICATED inputs only.

The set is the Luau gameplay example (HUD and presentation, intercepts button 2), a native C11 mod that presses button 1 while button 2 is held, and a typed ROM edit. Order matters by hand
derivation: with the native mod FIRST it sees button 2 and presses button 1, so the game scores AND runs; with it SECOND the example has already consumed button 2, so the game runs without scoring.

Also: composite identity changes with membership and order and equals `mod set variant`'s (an identical set reuses it); overlapping bank-qualified edits are rejected naming both mods; replay is
deterministic; a saved state restores only with the same set; another mapper or source image is rejected; a faulting member is disabled alone; hooks are refused.
usage: mod_set_sms_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

if sys.platform == "win32":
    print("mod_set_sms_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLES = ROOT / "platforms/master-system/sdk/examples"
spec = importlib.util.spec_from_file_location("sms_runner_fixture", EXAMPLES / "gameplay/fixture_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-sms-set-"))
ROM = fixture.build()
ROM_PATH = write(tmp / "runner.sms", ROM)


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-500:]}")
    return doc or {}


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")
    return doc or {}


def manifest(mod_id, extra, author=False, image=ROM, mapper="rom_only"):
    text = f'[mod]\nid = "{mod_id}"\nversion = "1.0.0"\nname = "{mod_id}"\nlicense = "CC0-1.0"\n' + ('author = "tests"\n' if author else "")
    return text + f'\n[target]\nplatform = "master-system"\n\n[[target.revision]]\nlabel = "fixture"\nsha256 = "{sha(image)}"\nmapper = "{mapper}"\n' + extra


NATIVE = """#include <segarecomp_mod_sms.h>
typedef struct State { uint32_t pressed; } State;
static State g_state;
static void phase(void *state, const SegaModHost *host) {
  State *s = (State *)state;
  uint32_t pad = 0U;
  if (host->phase == SEGA_MOD_PHASE_RESET) s->pressed = 0U;
  if (host->phase == SEGA_MOD_PHASE_INPUT && host->input_get(1U, &pad) == SEGA_MOD_OK && (pad & SEGA_SMS_BUTTON_2) != 0U) {
    (void)host->input_set(1U, pad | SEGA_SMS_BUTTON_1);
    ++s->pressed;
  }
}
static int save_state(void *state, uint8_t *out, uint32_t cap, uint32_t *len) { if (cap < 1U) return 1; out[0] = (uint8_t)((const State *)state)->pressed; *len = 1U; return 0; }
static int load_state(void *state, const uint8_t *in, uint32_t len) { if (len != 1U) return 1; ((State *)state)->pressed = in[0]; return 0; }
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 5U, SEGA_MOD_PLATFORM, "turbo", 1U, NULL, phase, NULL, save_state, load_state, {NULL}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { if (state_out != NULL) *state_out = &g_state; return &provider; }
"""
turbo = tmp / "turbo"
write(turbo / "src" / "main.c", NATIVE.encode())
write(turbo / "mod.toml", manifest("turbo", '\n[[source]]\nkind = "native-c11"\npath = "src/main.c"\n\n[abi]\nnative = 2\nminor = 5\n\n[capabilities]\nrequired = ["input.intercept", "input.read", "state.mod"]\n\n[state]\nschema = 1\n', author=True))
runner = tmp / "gameplay"
shutil.copytree(EXAMPLES / "gameplay", runner, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
# a typed edit in untouched padding (the fixture is 32 KiB of mostly zero)
EDIT_AT = 0x6000
assert ROM[EDIT_AT] == ROM[EDIT_AT + 1]


def edit_mod(name, value, at=EDIT_AT):
    d = tmp / name
    write(d / "edits" / "main.edits", f"fixture u8 0x{at:08x} 0x{ROM[at]:02x} 0x{value:02x}\n".encode())
    write(d / "mod.toml", manifest(name, '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'))
    return d


edit = edit_mod("tail-edit", 0x5A)


def identity(project):
    return ok(f"check {project.name}", "mod", "check", project)["package_identity"]


ids = {p.name: identity(p) for p in (turbo, runner, edit)}


def build_args(members, name, approve=(), rom=ROM_PATH):
    out = tmp / ("build-" + name)
    args = ["mod", "set", "build", *members, "--rom", rom, "--out", out, "--cc", CC, "--sdk-dir", SDK, "--optimize", "0"]
    for a in approve:
        args += ["--approve", a]
    return args, out


def built(what, members, name, approve=(ids["turbo"],)):
    args, out = build_args(members, name, approve)
    return ok(what, *args), out


def not_built(what, code, members, name, approve=(ids["turbo"],), rom=ROM_PATH):
    args, out = build_args(members, name, approve, rom)
    doc = refused(what, code, *args)
    cli.check(not (out / "game").exists(), f"{what}: a refused set leaves no executable")
    return doc


# button 2 alone (0x20) is held for machine frames 20..29
INPUT = "20 -----2 ------ -\n30 ------ ------ -\n"


def run(out, extra=(), tag="a"):
    work = tmp / (out.name + "-run-" + tag)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    (work / "input.txt").write_text(INPUT)
    p = subprocess.run([str(out / "game"), "--frames", "40", "--cycle-budget", "10000000", "--input", str(work / "input.txt"), "--artifacts", str(work), "--mod-ram", "16:7", *extra], text=True, capture_output=True, timeout=120)
    lines = {}
    for line in p.stderr.splitlines():
        key, _, rest = line.partition(" ")
        if key in ("MOD_SET_SUMMARY", "MOD_SET_LOADED", "MOD_REFUSED"):
            lines[key] = json.loads(rest)
    read = lambda n: (work / n).read_bytes() if (work / n).exists() else None   # noqa: E731
    return {"rc": p.returncode, "summary": lines.get("MOD_SET_SUMMARY"), "lines": lines, "read": read, "log": p.stdout + p.stderr}


def game(r):
    """(score, position) from the final work RAM ($C010 and $C012, little-endian)."""
    ram = r["read"]("ram.bin") or bytes(0x2000)
    return int.from_bytes(ram[0x10:0x12], "little"), int.from_bytes(ram[0x12:0x14], "little")


GUEST = ("state.sha256", "ram.bin", "vram.bin", "frames.txt", "framebuffer.bin", "audio.sha256")
guest = lambda r: {n: r["read"](n) for n in GUEST}   # noqa: E731

# ---------------------------------------------------------------- composition refusals, shared with Genesis
doc = not_built("overlapping bank-qualified edits", "E_SET_EDIT_OVERLAP", [edit, edit_mod("tail-edit-two", 0x11)], "overlap")
cli.check(doc.get("violations", [{}])[0].get("mods") == ["tail-edit", "tail-edit-two"] and "bank 1" in doc.get("error", ""), f"both mods and the bank are named: {doc}")
other_rom = bytearray(ROM)
other_rom[0x6100] ^= 1
not_built("another source image", "E_SET_SOURCE", [edit, runner], "source", rom=write(tmp / "other.sms", bytes(other_rom)))
mapper = edit_mod("mapper-edit", 0x22, at=0x6004)
write(mapper / "mod.toml", manifest("mapper-edit", '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n', mapper="sega"))
rc, doc, p = cli.run(*build_args([edit, mapper], "mapper")[0])
cli.check(rc == 1 and doc and doc.get("class") in ("E_SET_SOURCE", "E_MAPPER_UNSUPPORTED"), f"another mapper declaration is rejected: {doc}")
hooked = tmp / "hooked"
shutil.copytree(turbo, hooked)
write(hooked / "mod.toml", (turbo / "mod.toml").read_text().replace('id = "turbo"', 'id = "hooked"') + '\n[[symbol]]\nname = "fn"\nrevision = "fixture"\naddress = "0x00000100"\nprovenance = "verified"\n\n[[hook]]\nid = "watch"\nsymbol = "fn"\nmode = "before"\nreads = ["a"]\n')
rc, doc, p = cli.run(*build_args([hooked, edit], "hooked", approve=())[0])
cli.check(rc == 1 and doc and str(doc.get("class", "")).startswith("E_HOOK"), f"a Master System hook declaration is rejected: {doc}")

# ---------------------------------------------------------------- the composite program
doc_a, out_a = built("turbo first", [turbo, runner, edit], "a")
doc_b, out_b = built("turbo second", [runner, turbo, edit], "b")
doc_c, out_c = built("without the edit", [turbo, runner], "c")
cli.check(len({doc_a.get("set_identity"), doc_b.get("set_identity"), doc_c.get("set_identity")}) == 3, "membership and order change the composite identity")
cli.check(doc_a.get("effective_sha256") == doc_b.get("effective_sha256") != doc_c.get("effective_sha256"), "order keeps the image; membership with an edit changes it")
applied = ok("apply", "mod", "set", "apply", turbo, runner, edit, "--rom", ROM_PATH, "--out", tmp / "composed.sms")
cli.check(applied["effective_sha256"] == doc_a.get("effective_sha256") and (tmp / "composed.sms").read_bytes()[EDIT_AT] == 0x5A, "the program is compiled from the composed image")

library = tmp / "library"
save_args = ["mod", "set", "save", "--library", library, "--name", "sms-set"]
for project in (turbo, runner, edit):
    package = tmp / f"{project.name}.segapatch"
    ok("pack", "mod", "pack", project, "--out", package)
    approve = ("--approve", ids["turbo"]) if project == turbo else ()
    save_args += ["--identity", ok("install", "mod", "install", package, "--rom", ROM_PATH, "--library", library, *approve)["package_identity"]]
ok("save", *save_args)
first_variant = ok("variant", "mod", "set", "variant", "--library", library, "--name", "sms-set", "--rom", ROM_PATH, "--cc", CC, "--sdk-dir", SDK)
again = ok("variant again", "mod", "set", "variant", "--library", library, "--name", "sms-set", "--rom", ROM_PATH, "--cc", CC, "--sdk-dir", SDK)
cli.check(first_variant["variant_key"] == doc_a.get("set_identity") and again["cache"] == "hit", f"the program's identity is the variant key and the same set reuses it: {first_variant['variant_key']} {again['cache']}")
stored_out = tmp / "build-stored"
stored = ok("build a stored set (library approvals)", "mod", "set", "build", "--library", library, "--name", "sms-set", "--rom", ROM_PATH, "--out", stored_out, "--cc", CC, "--sdk-dir", SDK, "--optimize", "0")
cli.check(stored.get("set_identity") == doc_a.get("set_identity"), "a stored set builds the same variant")

a, a2, b = run(out_a), run(out_a, tag="replay"), run(out_b)
cli.check(a["rc"] == 0 and b["rc"] == 0 and a["summary"] and b["summary"], f"both programs run: {a['rc']} {b['rc']} {a['log'][-800:]}")
cli.check(a["summary"] == a2["summary"] and guest(a) == guest(a2), "replay is deterministic")
cli.check([m["mod"] for m in a["summary"]["members"]] == ["turbo", "sms-runner-example"] and not any(m["faulted"] for m in a["summary"]["members"]), f"set order and no fault: {a['summary']['members']}")
# ten held frames; the example turns button 2 into Right at speed 3
cli.check(game(a) == (10, 30), f"turbo first: the game scores 10 and runs 30: {game(a)}")
cli.check(game(b) == (0, 30), f"turbo second: button 2 was already consumed: {game(b)}")
cli.check(a["summary"]["active_frames"] > 0, "the drawing mod's presentation is composed")

# ---------------------------------------------------------------- state: same set only
state = tmp / "set.state"
saved = run(out_a, extra=("--mod-checkpoint", "25", "--mod-state-save", str(state)), tag="save")
cli.check(saved["rc"] == 0 and saved["summary"]["roundtrip"]["status"] == "ok" and guest(saved) == guest(a) and state.is_file(), f"the whole set round-trips mid-run: {saved['summary'] and saved['summary'].get('roundtrip')} {saved['log'][-500:]}")
loaded = run(out_a, extra=("--mod-state-load", str(state)), tag="load")
cli.check(loaded["rc"] == 0 and game(loaded) == (10, 30) and {m["mod"]: m["write_digest"] for m in loaded["summary"]["members"]} == {m["mod"]: m["write_digest"] for m in a["summary"]["members"]}, f"a fresh program resumes the set: {loaded['log'][-500:]}")
for label, other in (("another order", out_b), ("another membership", out_c)):
    r = run(other, extra=("--mod-state-load", str(state)), tag="foreign")
    cli.check(r["rc"] == 5 and r["lines"].get("MOD_REFUSED", {}).get("class") == "E_STATE_RESTORE", f"a state of {label} is refused: {r['rc']} {r['lines'].get('MOD_REFUSED')}")

# ---------------------------------------------------------------- failure isolation
faulty = tmp / "faulty"
write(faulty / "scripts" / "main.luau", b'function frame_end(f: number)\n  if f == 22 then sega.mem.write("z80.work_ram", 256, 1, 1) end\nend\n')
write(faulty / "mod.toml", manifest("faulty", '\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n\n[capabilities]\nrequired = ["memory.write.z80.work_ram"]\n\n[[memory]]\nrevision = "fixture"\nspace = "z80.work_ram"\noffset = 32\nlength = 1\naccess = "write"\n'))
_, out_f = built("with a faulting mod", [turbo, faulty, runner, edit], "faulty")
f = run(out_f)
members = {m["mod"]: m for m in (f["summary"] or {"members": []})["members"]}
cli.check(f["rc"] == 0 and members.get("faulty", {}).get("faulted") and "mod=faulty" in members["faulty"]["fault"] and not members["turbo"]["faulted"] and not members["sms-runner-example"]["faulted"], f"only the faulting mod is disabled: {members}")
cli.check(game(f) == (10, 30) and guest(f) == guest(a), "the guest equals the set without the faulting mod")

# ---------------------------------------------------------------- the played SDL3 program (Linux with SDL3, as sms_viewer_mod_test): live input, replay by the headless set, save and load
import os  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import genesis_startup_bridge as bridge  # noqa: E402

if sys.platform == "linux" and bridge.find_sdl3() is not None:
    include, lib = bridge.find_sdl3()[0][1], bridge.find_sdl3()[1][1]
    args, played = build_args([turbo, runner, edit], "played", (ids["turbo"],))
    ok("the played set builds", *args, "--sdl3-include", include, "--sdl3-lib", lib)
    work = tmp / "viewer-work"
    work.mkdir()

    def session(frames, script, save=None, record=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("SEGARECOMP_")}
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"), LD_LIBRARY_PATH=lib + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""))
        (work / "pad.txt").write_text(script)
        report = work / "report.json"
        report.unlink(missing_ok=True)
        env.update(SEGARECOMP_VIEWER_PAD_SCRIPT=str(work / "pad.txt"), SEGARECOMP_VIEWER_STATE_REPORT=str(report))
        if save is not None:
            env["SEGARECOMP_SAVE_DIR"] = str(save)
        argv = [str(played / "game"), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", str(frames)]
        if record is not None:
            argv += ["--viewer-record", str(record)]
        p = subprocess.run(argv, text=True, capture_output=True, env=env, timeout=180)
        tagged = {}
        events = []
        for line in p.stderr.splitlines():
            key, _, rest = line.partition(" ")
            if key in ("MOD_SET_SUMMARY", "MOD_SET_LOADED"):
                tagged[key] = json.loads(rest)
            elif key == "VIEWER_STATE":
                events.append(json.loads(rest))
        return p, (json.loads(report.read_text()) if report.exists() else None), events, tagged

    record = work / "record.txt"
    p, plain, _, tagged = session(120, "20 2\n", record=record)
    summary = tagged.get("MOD_SET_SUMMARY")
    cli.check(p.returncode == 0 and tagged.get("MOD_SET_LOADED", {}).get("mods") == 2 and summary is not None and not any(m["faulted"] for m in summary["members"]), f"the played program runs the set: {p.returncode} {p.stderr[-800:]}")
    replay = subprocess.run([str(out_b / "game"), "--frames", "120", "--cycle-budget", "20000000", "--input", str(record)], text=True, capture_output=True, timeout=120)
    lines = [json.loads(x.split(" ", 1)[1]) for x in replay.stderr.splitlines() if x.startswith("MOD_SET_SUMMARY ")]
    witness = lambda doc: {m["mod"]: (m["write_digest"], m["committed_writes"]) for m in doc["members"]}   # noqa: E731
    cli.check(bool(lines) and summary is not None and witness(lines[0]) == witness(summary), f"the headless set of the same order replays the played input to the same witnesses: {lines} {summary}")
    save = tmp / "viewer-save"
    _, saved_report, events, _ = session(120, "20 2\n60 2K\n61 2\n", save)
    cli.check(saved_report == plain and bool(events) and events[0]["status"] == "ok", f"saving the played set does not perturb it: {events}")
    _, loaded_report, events, loaded_tags = session(70, "10 2M\n11 2\n", save)
    cli.check(bool(events) and events[0]["event"] == "load" and events[0]["status"] == "ok" and loaded_report is not None and plain is not None and loaded_report["canonical_sha256"] == plain["canonical_sha256"],
              f"a separate process loads the set state and reaches the same canonical state: {events} {loaded_report} {plain}")
    print("mod_set_sms_test: SDL3 viewer section ran")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_set_sms_test")
