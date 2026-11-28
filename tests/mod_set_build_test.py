#!/usr/bin/env python3
"""SEG-141-T003/T006 (ADR 0327): BUILT Genesis mod sets, end to end, with FABRICATED inputs only. `segarecomp mod set build` links a Luau HUD mod, native C11 mods and a typed ROM edit into one
generated-native program; the program runs headless under the frame capture with a scripted pad and an instruction budget.

Proves: (1) the composite key equals `mod set variant`'s and changes with order; (2) the composed program is deterministic and every mod's effect is visible in guest work RAM (hand-derived relations);
(3) the input chain: the native mod's pad change reaches the guest and the later Luau mod; (4) save/restore of the whole set round-trips, and a saved state loads only into the same set; (5) a mod that
faults is disabled alone: the guest equals the set without it; (6) two native mods share a hooked function and run in set order, in both orders; (7) every refusal has its class and leaves no executable.
usage: mod_set_build_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
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
    print("mod_set_build_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/genesis/sdk/examples/gameplay"


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    out = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(out)
    return out


fixture = module(EXAMPLE / "fixture_rom.py", "fixture_rom")
hook_fixture = module(ROOT / "tests/hook_fixture_rom.py", "hook_fixture_rom")
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-set-build-"))
ROM = fixture.make_rom()
ROM_PATH = write(tmp / "fixture.rom", ROM)
HOOK_ROM = hook_fixture.make_rom()
HOOK_ROM_PATH = write(tmp / "hook.rom", HOOK_ROM)
RIGHT, B, C = 0x08, 0x10, 0x20


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-600:]}")
    return doc or {}


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")
    return doc or {}


def manifest(mod_id, image, extra, author=False):
    text = f'[mod]\nid = "{mod_id}"\nversion = "1.0.0"\nname = "{mod_id}"\nlicense = "CC0-1.0"\n' + ('author = "tests"\n' if author else "")
    return text + f'\n[target]\nplatform = "genesis"\n\n[[target.revision]]\nlabel = "fixture"\nsha256 = "{sha(image)}"\n' + extra


def memory(offset, length, access="write"):
    return f'\n[[memory]]\nrevision = "fixture"\nspace = "m68k.work_ram"\noffset = {offset}\nlength = {length}\naccess = "{access}"\n'


def native_mod(mod_id, source, caps, extra="", image=ROM):
    d = tmp / mod_id
    write(d / "src" / "provider.c", source.encode())
    write(d / "mod.toml", manifest(mod_id, image, '\n[[source]]\nkind = "native-c11"\npath = "src/provider.c"\n\n[abi]\nnative = 2\n\n[capabilities]\nrequired = [' + ", ".join(f'"{c}"' for c in caps) + "]\n" + extra, author=True))
    return d


def luau_mod(mod_id, script, caps, extra="", image=ROM):
    d = tmp / mod_id
    write(d / "scripts" / "main.luau", script.encode())
    write(d / "mod.toml", manifest(mod_id, image, '\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n\n[capabilities]\nrequired = [' + ", ".join(f'"{c}"' for c in caps) + "]\n" + extra))
    return d


PROVIDER = """#include <segarecomp_mod_genesis.h>
#include <stddef.h>
%(globals)s
static int provider_init(void *state, const SegaModHost *host) { (void)state; (void)host; return 0; }
%(body)s
static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) { (void)state; if (cap < 4U) return 1; out[0] = (uint8_t)total; out[1] = (uint8_t)(total >> 8); out[2] = 0; out[3] = 0; *len = 4U; return 0; }
static int provider_load(void *state, const uint8_t *data, uint32_t len) { (void)state; if (len != 4U) return 1; total = (uint32_t)data[0] | ((uint32_t)data[1] << 8); return 0; }
static const SegaModProvider provider = { sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "%(name)s", 1U, provider_init, provider_phase, %(hook)s, provider_save, provider_load, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL} };
const SegaModProvider *segarecomp_mod_provider(void **state_out) { if (state_out != NULL) *state_out = NULL; return &provider; }
"""

# "assist" (native): while C is held it also presses Right for the guest (input chain), and counts those frames at work RAM 0x40. Its non-static helper name is deliberately unique.
ASSIST = PROVIDER % {"name": "assist", "hook": "NULL", "globals": "static uint32_t total;\nstatic int pressed;", "body": """static void provider_phase(void *state, const SegaModHost *host) {
  uint32_t pad = 0U;
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_RESET) total = 0U;
  if (host->phase == SEGA_MOD_PHASE_INPUT && host->input_get(1U, &pad) == SEGA_MOD_OK && (pad & 0x20U) != 0U) {
    (void)host->input_set(1U, pad | 0x08U);
    pressed = 1;
  }
  if (host->phase == SEGA_MOD_PHASE_POST_INPUT && pressed != 0) {
    pressed = 0;
    ++total;
    (void)host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x40U, 2U, total & 0xFFFFU);
  }
}"""}
assist = native_mod("assist", ASSIST, ["input.intercept", "input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x40, 2))

# "watcher" (Luau, no presentation): counts, at 0x50, the frames in which it saw Right in the FINAL pad -- it only sees it when the native mod added it
WATCHER = """local seen = 0
function reset() seen = 0 end
function post_input(frame: number)
  if sega.input.get(1) // 8 % 2 == 1 then
    seen = seen + 1
    sega.mem.write("m68k.work_ram", 0x50, 2, seen % 65536)
  end
end
function save() return { seen = seen } end
function load(s) seen = s.seen end
"""
watcher = luau_mod("watcher", WATCHER, ["input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x50, 2))
# a second script with the SAME saved-state key (`seen`): each mod's saved values are its own
TWIN = WATCHER.replace("seen = seen + 1", "seen = seen + 3").replace("0x50", "0x52")
twin = luau_mod("watcher-twin", TWIN, ["input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x52, 2))
hud = tmp / "gameplay-example"
shutil.copytree(EXAMPLE, hud, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
EDIT_AT = len(ROM) - 2
edit = tmp / "edit"
write(edit / "edits" / "main.edits", f"fixture u16 0x{EDIT_AT:08x} 0x{ROM[EDIT_AT:].hex()} 0x5ac3\n".encode())
write(edit / "mod.toml", manifest("tail-edit", ROM, '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'))


def identity(project):
    return ok(f"check {project.name}", "mod", "check", project)["package_identity"]


def build(what, members, rom=ROM_PATH, name=None, approve=()):
    out = tmp / f"build-{name or what.replace(' ', '-')}"
    args = ["mod", "set", "build", *members, "--rom", rom, "--out", out, "--cc", CC, "--sdk-dir", SDK, "--optimize", "0", "--runtime-optimize", "1"]
    for a in approve:
        args += ["--approve", a]
    return args, out


def built(what, members, rom=ROM_PATH, approve=(), name=None):
    args, out = build(what, members, rom, name, approve)
    doc = ok(what, *args)
    return doc, out


def not_built(what, code, members, rom=ROM_PATH, approve=()):
    args, out = build(what, members, rom, None, approve)
    doc = refused(what, code, *args)
    cli.check(not (out / "game").exists(), f"{what}: a refused set leaves no executable")
    return doc


def run(out, pad, env_extra=None, frames=40):
    frames_dir = out / "frames"
    shutil.rmtree(frames_dir, ignore_errors=True)
    frames_dir.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith("SEGARECOMP_")}
    env.update({"SEGARECOMP_CAPTURE_DIR": str(frames_dir), "SEGARECOMP_CAPTURE_FIRST": "1", "SEGARECOMP_CAPTURE_COUNT": str(frames), "SEGARECOMP_CAPTURE_SLICE": "16", "SEGARECOMP_CAPTURE_STRIDE": "1",
                "SEGARECOMP_PAD_SCRIPT": pad, "SEGARECOMP_CAPTURE_RAM": "16:4,64:2,80:2,96:2"})
    env.update(env_extra or {})
    p = subprocess.run([str(out / "game"), "--instruction-budget", "20000000"], capture_output=True, text=True, env=env, timeout=300)
    log = p.stdout + p.stderr
    lines = {}
    for line in log.splitlines():
        tag, _, rest = line.partition(" ")
        if tag in ("MOD_SET_SUMMARY", "MOD_SET_LOADED", "MOD_REFUSED", "FRAME_LOG", "CAPTURE_SUMMARY"):
            lines[tag] = json.loads(rest) if rest.startswith("{") else rest
    return p.returncode, lines, log


def observation(out, pad, env_extra=None, frames=40):
    """(MOD_SET_SUMMARY, CAPTURE_SUMMARY). The capture holds one frame digest and one RAM sample per captured frame, and the final work-RAM digest."""
    rc, lines, log = run(out, pad, env_extra, frames)
    capture = lines.get("CAPTURE_SUMMARY", {})
    cli.check(rc in (0, 1) and "MOD_SET_SUMMARY" in lines and len(capture.get("digests", [])) == int((env_extra or {}).get("SEGARECOMP_CAPTURE_COUNT", frames)), f"the program ran every frame: rc={rc} {log[-1500:]}")
    return lines.get("MOD_SET_SUMMARY", {"members": []}), capture


def words(capture):
    """The sampled work-RAM words of the last captured frame: score, position, assist's counter, watcher's counter, the shared hook word."""
    sample = bytes.fromhex(capture.get("ram_frames", ["00" * 10])[-1])
    return [int.from_bytes(sample[i:i + 2], "big") for i in range(0, 10, 2)]


def same_guest(a, b):
    return all(a.get(k) == b.get(k) and a.get(k) for k in ("digests", "ram_frames", "final_work_ram_sha256"))


def members_of(summary):
    return {m["mod"]: m for m in summary["members"]}


PAD = f"0:1:0,5:1:{C},25:1:0"
ids = {name: identity(p) for name, p in (("assist", assist), ("watcher", watcher), ("hud", hud), ("edit", edit))}

# ---------------------------------------------------------------- refusals before a compiler runs
not_built("native without approval", "E_NOT_APPROVED", [hud, assist])
not_built("two drawing mods", "E_SET_PRESENTATION_CONFLICT", [hud, luau_mod("second-hud", 'function render(f: number) sega.overlay.rect(0,0,1,1,0xFFFFFFFF) end\n', ["overlay.rgba"])])
audio = luau_mod("audio-mod", 'function frame_end(f: number) sega.audio.event("now") end\n', ["audio.event.read"],
                 '\n[[audio.mapping]]\nid = "now"\nkind = "event"\nspace = "m68k.work_ram"\noffset = 112\nwidth = 1\n\n[[audio.mapping.value]]\nname = "idle"\nvalue = 0\n')
bad_overlap = tmp / "edit-two"
write(bad_overlap / "edits" / "main.edits", f"fixture u16 0x{EDIT_AT:08x} 0x{ROM[EDIT_AT:].hex()} 0x1111\n".encode())
write(bad_overlap / "mod.toml", manifest("tail-edit-two", ROM, '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'))
doc = not_built("overlapping edits", "E_SET_EDIT_OVERLAP", [edit, bad_overlap])
cli.check(doc.get("violations", [{}])[0].get("mods") == ["tail-edit", "tail-edit-two"], f"the build diagnostic names both mods: {doc}")
not_built("one mod twice", "E_SET_DUPLICATE_MOD", [watcher, watcher])
not_built("the audio API in a set", "E_SET_AUDIO_UNSUPPORTED", [watcher, audio])
clash = native_mod("clash", ASSIST.replace('"assist"', '"clash"') + "\nint set_clash_symbol(void) { return 1; }\n", ["input.intercept", "input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x42, 2))
clash_two = native_mod("clash-two", ASSIST + "\nint set_clash_symbol(void) { return 2; }\n", ["input.intercept", "input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x44, 2))
collision = not_built("colliding native symbols", "E_SET_NATIVE_LINK", [clash, clash_two], approve=(identity(clash), identity(clash_two)))
cli.check(collision.get("error") == "The mod set program could not be linked: two native mods may define the same external symbol (see build.log).", f"the pinned link diagnostic: {collision.get('error')}")

# ---------------------------------------------------------------- the composite program: Luau HUD + native + typed edit + Luau gameplay
order = [hud, assist, watcher, edit]
doc, out = built("the composite set", order, approve=(ids["assist"],))
status = json.loads((out / "status.json").read_text())
cli.check([m["id"] for m in status.get("mod_set", {}).get("members", [])] == ["gameplay-example", "assist", "watcher", "tail-edit"] and doc.get("running") == 3, f"status names every member in order: {status.get('mod_set')}")
want = bytearray(ROM)
want[EDIT_AT:] = b"\x5a\xc3"
cli.check(doc.get("effective_sha256") != sha(ROM), "the typed edit is in the image the program was compiled from")
composed = ok("apply", "mod", "set", "apply", *order, "--rom", ROM_PATH, "--out", tmp / "composed.bin")
cli.check(composed["effective_sha256"] == doc.get("effective_sha256"), "the built image is exactly the composed image")

# the same key as `mod set variant` over installed packages
library = tmp / "library"
packages = []
for project in order:
    package = tmp / f"{project.name}.segapatch"
    ok("pack", "mod", "pack", project, "--out", package)
    approve = ("--approve", ids["assist"]) if project == assist else ()
    packages.append(ok("install", "mod", "install", package, "--rom", ROM_PATH, "--library", library, *approve)["package_identity"])
save_args = ["mod", "set", "save", "--library", library, "--name", "composite"]
for p in packages:
    save_args += ["--identity", p]
ok("save", *save_args)
variant = ok("variant", "mod", "set", "variant", "--library", library, "--name", "composite", "--rom", ROM_PATH, "--cc", CC, "--sdk-dir", SDK)
cli.check(variant["variant_key"] == doc.get("set_identity"), f"the program's identity is the variant key: {variant['variant_key']} {doc.get('set_identity')}")

first, first_frames = observation(out, PAD)
second, second_frames = observation(out, PAD)
cli.check(first == second and same_guest(first_frames, second_frames), "the composite program is deterministic (summaries, frame digests, RAM samples, final state)")
by = members_of(first)
cli.check(first.get("set_identity") == doc.get("set_identity") and list(by) == ["gameplay-example", "assist", "watcher"] and not any(m["faulted"] for m in by.values()), f"three running mods, none faulted: {first}")
# C is held for guest frames 5..24. The HUD mod adds 10 to the score per such frame, assist counts the same frames, and watcher counts the frames whose FINAL pad has the Right that only assist adds.
score, position, assisted, watched, _ = words(first_frames)
cli.check(15 <= assisted <= 20 and score == 10 * assisted and watched == assisted, f"hand-derived relations of the three mods: score={score} assisted={assisted} watched={watched}")
cli.check(position > 0, f"the guest itself read the Right button a mod added: position={position}")
cli.check(all(by[n]["committed_writes"] == assisted for n in by), f"every mod wrote once per held frame: {[(n, by[n]['committed_writes']) for n in by]}")
idle, idle_frames = observation(out, "0:1:0")
cli.check(all(m["committed_writes"] == 0 for m in idle["members"]) and words(idle_frames)[:4] == [0, 0, 0, 0], f"without C no mod writes and the guest does not move: {words(idle_frames)}")
cli.check(by["gameplay-example"]["overlay_ops"] > 0 and by["assist"]["overlay_ops"] == 0, "only the drawing mod has an overlay")

# another order: post-input shows every mod the composed pad, so this set's guest is the same although it is a different variant
doc_rev, out_rev = built("reordered set", [watcher, assist, hud, edit], approve=(ids["assist"],))
cli.check(doc_rev.get("set_identity") != doc.get("set_identity") and doc_rev.get("effective_sha256") == doc.get("effective_sha256"), "another order is another variant of the same image")
reordered, reordered_frames = observation(out_rev, PAD)
cli.check([m["mod"] for m in reordered["members"]] == ["watcher", "assist", "gameplay-example"] and reordered_frames.get("final_work_ram_sha256") == first_frames.get("final_work_ram_sha256"), f"the reordered set reaches the same guest state: {reordered}")

# ---------------------------------------------------------------- performance reporting still works on a package build (its compiler-version.tmp is a guarded output, written and removed as before)
profiled_package = tmp / "profiled-member.segapatch"
profiled_identity = ok("pack the profiled member", "mod", "pack", edit, "--out", profiled_package)["package_identity"]
profiled_out, profiled_report = tmp / "build-profiled", tmp / "profiled-report.json"
p = subprocess.run([sys.argv[1], "build", "--rom", str(ROM_PATH), "--output", str(profiled_out), "--cc", CC, "--runtime-dir", str(SDK / "genesis"), "--optimize", "0", "--runtime-optimize", "1",
                    "--set-package", str(profiled_package), "--set-identity", profiled_identity, "--performance-report", str(profiled_report)], text=True, capture_output=True, timeout=900)
report = json.loads(profiled_report.read_text()) if profiled_report.is_file() else {}
cli.check(p.returncode == 0 and (profiled_out / "game").is_file() and report != {} and not (profiled_out / "compiler-version.tmp").exists() and ROM_PATH.read_bytes() == ROM,
          f"a profiled package build succeeds, writes its report and leaves no compiler-version.tmp: rc={p.returncode} {p.stdout[-300:]} {sorted(report)[:6]}")

# ---------------------------------------------------------------- the build never deletes a folder it did not create
victim = tmp / "build-keeps-user-files"
write(victim / "set-members" / "keep.txt", b"user data")
args, _ = build("keeps user files", [watcher], name="keeps-user-files")
ok("a user folder named set-members is left alone", *args)
cli.check((victim / "set-members" / "keep.txt").read_bytes() == b"user data" and not list(victim.glob(".set-members-*")), "the user's set-members folder survives and this build's staging is gone")
write(victim / "input" / "effective.rom", b"a stale generated image of an earlier, interrupted build")
ok("rebuilding into the same folder reuses its own marked staging and replaces a stale generated image", *args)
cli.check(not (victim / "input").exists() and (victim / "game").is_file() and ROM_PATH.read_bytes() == ROM, "the generated image is removed again and the original is untouched")
occupied = tmp / "build-occupied"
write(occupied / "set-mod" / "keep.txt", b"user data")
args, _ = build("occupied", [watcher], name="occupied")
refused("a user folder named set-mod is refused, not deleted", "E_SET_ARTIFACT_WRITE", *args)
cli.check(sorted(p.name for p in (occupied / "set-mod").iterdir()) == ["keep.txt"] and not (occupied / "game").exists(), "the user's set-mod folder is untouched")

# one native MOD with an unresolved symbol is an ordinary link failure, not a collision between two mods
broken = native_mod("broken-link", ASSIST.replace('"assist"', '"broken-link"') + "\nextern int set_missing_symbol(void);\nint set_use_missing(void) { return set_missing_symbol(); }\n",
                    ["input.intercept", "input.read", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x46, 2))
args, out_x = build("an unresolved symbol", [broken, assist], name="broken-link", approve=(identity(broken), ids["assist"]))
rc, link_doc, _ = cli.run(*args)
cli.check(rc == 1 and link_doc and link_doc.get("class") != "E_SET_NATIVE_LINK" and not (out_x / "game").exists(), f"an unresolved symbol is not reported as a collision: {link_doc}")

# ---------------------------------------------------------------- state: round trip inside the set; loads only into the same set
state_file = tmp / "set.state"
saved, saved_frames = observation(out, PAD, {"SEGARECOMP_MOD_CHECKPOINT": "12", "SEGARECOMP_MOD_SAVE": str(state_file)})
cli.check(saved["roundtrip"]["status"] == "ok" and members_of(saved) == by and same_guest(saved_frames, first_frames) and state_file.is_file(), f"the whole set round-trips mid-run with an unchanged continuation: {saved['roundtrip']}")
loaded, loaded_frames = observation(out, PAD, {"SEGARECOMP_MOD_LOAD": str(state_file), "SEGARECOMP_CAPTURE_FIRST": "20", "SEGARECOMP_CAPTURE_COUNT": "10"})
cli.check(loaded["loaded_state_sha256"] != "" and {n: m["write_digest"] for n, m in members_of(loaded).items()} == {n: m["write_digest"] for n, m in by.items()} and words(loaded_frames)[0] <= score,
          f"a fresh process loads the state and every mod's replay witness ends where the uninterrupted run's does: {loaded}")
rc, lines, log = run(out_rev, PAD, {"SEGARECOMP_MOD_LOAD": str(state_file)})
cli.check(rc == 5 and lines.get("MOD_REFUSED", {}).get("class") == "E_STATE_RESTORE", f"a state of the other order is refused: rc={rc} {lines.get('MOD_REFUSED')} {log[-300:]}")

doc_t, out_t = built("two mods with one state key", [assist, watcher, twin], approve=(ids["assist"],), name="same-key")
base_t, base_frames = observation(out_t, PAD)
twin_state = tmp / "twin.state"
observation(out_t, PAD, {"SEGARECOMP_MOD_CHECKPOINT": "12", "SEGARECOMP_MOD_SAVE": str(twin_state)})
resumed_t, resumed_frames = observation(out_t, PAD, {"SEGARECOMP_MOD_LOAD": str(twin_state), "SEGARECOMP_CAPTURE_FIRST": "20", "SEGARECOMP_CAPTURE_COUNT": "10"})
by_t = members_of(base_t)
cli.check(by_t["watcher"]["write_digest"] != by_t["watcher-twin"]["write_digest"] and {n: m["write_digest"] for n, m in members_of(resumed_t).items()} == {n: m["write_digest"] for n, m in by_t.items()},
          f"two mods with the same saved key keep separate values across save and load: {by_t}")

# ---------------------------------------------------------------- headless recordings carry the program's embedded set identity (never the environment's)
def headless_session(program, image, variable, recording, variant="e" * 64, pad=PAD):
    frames_dir = program / "session-frames"
    shutil.rmtree(frames_dir, ignore_errors=True)
    frames_dir.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith("SEGARECOMP_")}
    env.update({"SEGARECOMP_CAPTURE_DIR": str(frames_dir), "SEGARECOMP_CAPTURE_FIRST": "1", "SEGARECOMP_CAPTURE_COUNT": "30", "SEGARECOMP_CAPTURE_SLICE": "16", "SEGARECOMP_CAPTURE_STRIDE": "1",
                "SEGARECOMP_PAD_SCRIPT": pad if variable == "SEGARECOMP_RECORD" else "", "SEGARECOMP_ROM_SHA256": image, "SEGARECOMP_SAVE_VARIANT": variant, variable: str(recording)})
    p = subprocess.run([str(program / "game"), "--instruction-budget", "20000000"], capture_output=True, text=True, env=env, timeout=300)
    return p.returncode, p.stdout + p.stderr


def recorded_variant(recording):
    lines = recording.read_text().splitlines() if recording.is_file() else []
    return next((line.split()[1] for line in lines if line.startswith("variant ")), None)


def check_headless_sessions(label, program, other_programs, identity, image):
    recording = tmp / f"{label}.rec"
    rc, log = headless_session(program, image, "SEGARECOMP_RECORD", recording)
    cli.check(rc in (0, 1) and recorded_variant(recording) == identity, f"{label}: the headless recording carries the set identity although the environment names another variant: {recorded_variant(recording)} {log[-500:]}")
    rc, log = headless_session(program, image, "SEGARECOMP_REPLAY", recording, variant="d" * 64)
    cli.check(rc in (0, 1) and "VERIFIED, all" in log and "NOT VERIFIED" not in log, f"{label}: the same set replays it verified under any environment variant: {log[-600:]}")
    for other_label, other in other_programs:
        rc, log = headless_session(other, image, "SEGARECOMP_REPLAY", recording, variant=identity)   # even an environment that NAMES the recorded identity changes nothing
        cli.check("different image or version" in log and "VERIFIED, all" not in log, f"{label}: {other_label} rejects the recording: rc={rc} {log[-600:]}")
    return recording


_, out_c = built("a set with another membership", [assist, watcher, edit], approve=(ids["assist"],), name="membership")
image_sha = doc.get("effective_sha256", "")
set_recording = check_headless_sessions("provider set", out, [("another order", out_rev), ("another membership", out_c)], doc.get("set_identity", ""), image_sha)

data_two = tmp / "edit-second"
write(data_two / "edits" / "main.edits", f"fixture u16 0x{EDIT_AT - 2:08x} 0x{ROM[EDIT_AT - 2:EDIT_AT].hex()} 0x1357\n".encode())
write(data_two / "mod.toml", manifest("second-edit", ROM, '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'))
doc_data, out_data = built("a data-only set", [edit, data_two], name="data-ab")
doc_data_rev, out_data_rev = built("the data-only set reordered", [data_two, edit], name="data-ba")
_, out_data_one = built("a data-only set with another membership of the same kind", [edit], name="data-one")
cli.check(doc_data.get("running") == 0 and doc_data.get("effective_sha256") == doc_data_rev.get("effective_sha256") and doc_data.get("set_identity") != doc_data_rev.get("set_identity"), "two data-only sets of one image")
data_recording = check_headless_sessions("data-only set", out_data, [("another order", out_data_rev)], doc_data.get("set_identity", ""), doc_data.get("effective_sha256", ""))

# ---------------------------------------------------------------- failure isolation in the built program
FAULTY = """local n = 0
function frame_end(frame: number)
  n = n + 1
  if n == 8 then sega.mem.write("m68k.work_ram", 0x7000, 2, 1) end
end
"""
faulty = luau_mod("faulty", FAULTY, ["memory.write.m68k.work_ram"], memory(0x60, 2))
doc_f, out_f = built("a set with a faulting mod", [hud, assist, faulty, watcher, edit], approve=(ids["assist"],))
with_fault, fault_frames = observation(out_f, PAD)
by_f = members_of(with_fault)
cli.check(by_f["faulty"]["faulted"] and "mod=faulty" in by_f["faulty"]["fault"] and by_f["faulty"]["committed_writes"] == 0, f"the faulting mod is disabled with its own diagnostic: {by_f['faulty']}")
cli.check(all(by_f[n]["write_digest"] == by[n]["write_digest"] and not by_f[n]["faulted"] for n in by) and same_guest(fault_frames, first_frames), "the other mods and the guest behave exactly as without it")

# ---------------------------------------------------------------- shared hooks of two native mods, in set order; a shared replacement is refused at build
HOOKED = PROVIDER % {"name": "%(name)s", "hook": "provider_hook", "globals": "static uint32_t total;", "body": """static void provider_phase(void *state, const SegaModHost *host) { (void)state; if (host->phase == SEGA_MOD_PHASE_RESET) total = 0U; }
static int provider_hook(void *state, const SegaModHost *host, uint32_t id) {
  uint32_t v = 0U;
  (void)state; (void)id;
  if (total < 3U) {
    ++total;
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x60U, 2U, &v) == SEGA_MOD_OK) (void)host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x60U, 2U, (v * 10U + DIGIT) & 0xFFFFU);
  }
  return 0;
}"""}


def hook_declaration(mode):
    return (f'\n[[symbol]]\nname = "bump"\nrevision = "fixture"\naddress = "0x{hook_fixture.BUMP_ADDRESS:08x}"\nprovenance = "verified"\n'
            + f'\n[[hook]]\nid = "bump"\nsymbol = "bump"\nmode = "{mode}"\nreads = ["d0"]\n' + ('writes = ["d0", "ccr"]\ncost = 24\n' if mode == "replace" else ""))


def hooked(mod_id, digit, mode="before"):
    source = HOOKED.replace("%(name)s", mod_id).replace("DIGIT", f"{digit}U")
    return native_mod(mod_id, source, ["hook.function", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "state.mod"], "\n[state]\nschema = 1\n" + memory(0x60, 2, "read-write") + hook_declaration(mode), image=HOOK_ROM)


one, two = hooked("hook-one", 1), hooked("hook-two", 2)
approvals = (identity(one), identity(two))
for members, digits in (([one, two], (1, 2)), ([two, one], (2, 1))):
    expected = 0
    for _ in range(3):   # each mod appends its digit on its first three calls; the shared word records the interleaving
        for digit in digits:
            expected = (expected * 10 + digit) & 0xFFFF
    doc_h, out_h = built("shared hooks " + members[0].name, members, rom=HOOK_ROM_PATH, approve=approvals, name="hooks-" + members[0].name)
    summary, frames_h = observation(out_h, "0:1:0")
    hooks = members_of(summary)
    cli.check(list(hooks) == [m.name for m in members] and all(m["hooks"]["entry"] > 3 and not m["faulted"] and m["committed_writes"] == 3 for m in hooks.values()), f"both mods receive the shared hook: {summary}")
    cli.check(words(frames_h)[4] == expected, f"the shared word shows the dispatch order {[m.name for m in members]}: expected 0x{expected:04x}, got 0x{words(frames_h)[4]:04x}")
not_built("a shared replacement", "E_SET_HOOK_OWNER", [one, hooked("hook-swap", 3, "replace")], rom=HOOK_ROM_PATH, approve=(approvals[0], identity(tmp / "hook-swap")))
mixed = hooked("hook-after", 4, "after")
doc_m, out_m = built("a before and an after mod on one function", [one, mixed], rom=HOOK_ROM_PATH, approve=(approvals[0], identity(mixed)))
mixed_summary, _ = observation(out_m, "0:1:0")
mixed_by = members_of(mixed_summary)
cli.check(mixed_by["hook-one"]["hooks"]["entry"] > 0 and mixed_by["hook-one"]["hooks"]["exit"] == 0 and mixed_by["hook-after"]["hooks"]["entry"] == 0 and mixed_by["hook-after"]["hooks"]["exit"] > 0, f"each mod receives only the phase it declared: {mixed_summary}")

# ---------------------------------------------------------------- the played SDL3 program links the same set (optional 5th argument: the SDL3 prefix)
sdl = Path(sys.argv[5]) if len(sys.argv) > 5 else None
if sdl is not None and (sdl / "include").is_dir():
    from viewer_mod_support import Harness  # noqa: E402
    h = Harness(sys.argv[1], CC, ROOT, sdl, tmp)
    vanilla = h.build(ROM_PATH, tmp / "vanilla-viewer")
    cli.check(vanilla.returncode == 0, f"the Vanilla viewer program builds: {vanilla.stdout[-300:]}")
    _, _, vanilla_frame, vanilla_ram = h.play(tmp / "vanilla-viewer", "vanilla-viewer", budget=20_000_000)
    vanilla_pixels = vanilla_frame.read_bytes() if vanilla_frame.is_file() else b""
    args, out_v = build("the played composite set", order, name="viewer-set", approve=(ids["assist"],))
    ok("the played composite set builds", *args, "--sdl3-include", sdl / "include", "--sdl3-lib", sdl / "lib", "--test-frame-seam", "1")
    process, _, frame, ram = h.play(out_v, "viewer-set", budget=20_000_000)
    tagged = {line.partition(" ")[0]: json.loads(line.partition(" ")[2]) for line in process.stderr.splitlines() if line.startswith(("MOD_SET_LOADED ", "MOD_SET_SUMMARY "))}
    cli.check(tagged.get("MOD_SET_LOADED", {}).get("mods") == 3, f"the played program loads the three running mods: {process.stderr[-600:]}")
    summary = tagged.get("MOD_SET_SUMMARY", {"members": []})
    cli.check(len(summary["members"]) == 3 and not any(m["faulted"] for m in summary["members"]) and members_of(summary)["gameplay-example"]["overlay_ops"] > 0, f"the played set ran without faults and the drawing mod rendered: {summary}")
    cli.check(frame.is_file() and vanilla_pixels != b"" and frame.read_bytes() != vanilla_pixels, "the drawing mod's HUD is in the presented frame")
    # ---- saves and states are namespaced by the set (SEG-141-T005): two sets sharing one save folder never touch each other's files
    shared = tmp / "shared-saves"
    args, out_w = build("the played reordered set", [watcher, assist, hud, edit], name="viewer-reordered", approve=(ids["assist"],))
    reordered_doc = ok("the played reordered set builds", *args, "--sdl3-include", sdl / "include", "--sdl3-lib", sdl / "lib", "--test-frame-seam", "1")
    set_a = json.loads((out_v / "status.json").read_text())["mod_set"]["identity"]
    set_b = reordered_doc.get("set_identity", "")
    rom_sha = json.loads((out_v / "status.json").read_text())["mod_set"]["effective_sha256"]

    def played(program, tag, pad, folder):
        p, lines, _, _ = h.play(program, tag, pad=pad, save=folder, identity="f" * 64, rom_sha=rom_sha, budget=60_000_000)   # a foreign SEGARECOMP_SAVE_VARIANT is ignored by a set program
        events = [json.loads(x.split(" ", 1)[1]) for x in p.stderr.splitlines() if x.startswith("VIEWER_STATE ")]
        return p, events

    p, events = played(out_v, "save-a", "30 K\n31 -\n90 Q\n", shared)
    slot_a = shared / "sets" / set_a / "states" / "quick.sgstate"
    cli.check(bool(events) and events[0]["event"] == "save" and events[0]["status"] == "ok" and slot_a.is_file() and not (shared / "states").exists(), f"the first set saves inside its own namespace: {events} {p.stderr[-600:]}")
    kept = slot_a.read_bytes() if slot_a.is_file() else b""
    cli.check(kept[64:96] == bytes.fromhex(set_a), "the state records the set's composite identity, not the environment's variant")
    p, events = played(out_w, "load-b", "10 M\n60 Q\n", shared)
    cli.check(bool(events) and events[0]["event"] == "load" and events[0]["status"] != "ok", f"another set does not see the first set's slot: {events}")
    p, events = played(out_w, "save-b", "30 K\n31 -\n90 Q\n", shared)
    slot_b = shared / "sets" / set_b / "states" / "quick.sgstate"
    cli.check(set_a != set_b and slot_b.is_file() and slot_a.read_bytes() == kept, "saving in another set leaves the first set's save untouched")
    slot_b.write_bytes(kept)   # even a file copied across namespaces is refused by its identity
    p, events = played(out_w, "forged-b", "10 M\n60 Q\n", shared)
    cli.check(bool(events) and events[0]["status"] == "failed", f"a state from another set is rejected: {events}")
    p, events = played(out_v, "reload-a", "10 M\n60 Q\n", shared)
    cli.check(bool(events) and events[0]["event"] == "load" and events[0]["status"] == "ok", f"the first set still loads its own save: {events}")
    print("mod_set_build_test: SDL3 viewer section ran")

    # ---- a recording of one set is compatible between the headless and the played program of that set, in both directions
    def played_session(program, image, variable, recording):
        env = h.env(SEGARECOMP_ROM_SHA256=image, SEGARECOMP_SAVE_VARIANT="c" * 64, **{variable: str(recording)})
        if variable == "SEGARECOMP_RECORD":
            script = tmp / "session-pad.txt"
            script.write_text("40 Q\n")
            env["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(script)
        p = subprocess.run([str(program / "game"), "--instruction-budget", "60000000"], capture_output=True, text=True, env=env, timeout=300, cwd=tmp)
        return p.returncode, p.stderr

    args, data_played = build("the played data-only set", [edit, data_two], name="data-ab-viewer")
    ok("the played data-only set builds", *args, "--sdl3-include", sdl / "include", "--sdl3-lib", sdl / "lib", "--test-frame-seam", "1")
    for label, headless_recording, played_program, headless_program, identity_, image_ in (("provider set", set_recording, out_v, out, doc.get("set_identity", ""), image_sha),
                                                                                         ("data-only set", data_recording, data_played, out_data, doc_data.get("set_identity", ""), doc_data.get("effective_sha256", ""))):
        rc, log = played_session(played_program, image_, "SEGARECOMP_REPLAY", headless_recording)
        cli.check("VERIFIED, all" in log and "NOT VERIFIED" not in log, f"{label}: the played program verifies the headless recording: rc={rc} {log[-700:]}")
        recording = tmp / f"{label.split()[0]}-played.rec"
        rc, log = played_session(played_program, image_, "SEGARECOMP_RECORD", recording)
        cli.check(recorded_variant(recording) == identity_, f"{label}: the played recording carries the set identity: {recorded_variant(recording)} {log[-500:]}")
        rc, log = headless_session(headless_program, image_, "SEGARECOMP_REPLAY", recording)
        cli.check("VERIFIED, all" in log and "NOT VERIFIED" not in log, f"{label}: the headless program verifies the played recording: rc={rc} {log[-700:]}")
    rc, log = played_session(out_w, image_sha, "SEGARECOMP_REPLAY", set_recording)
    cli.check("different image or version" in log and "VERIFIED, all" not in log, f"the played program of another order rejects the recording: {log[-600:]}")
    print("mod_set_build_test: SDL3 session section ran")

    # ---- battery-backed SRAM (SEG-141-T005): the synthetic SRAM cartridge of the persistence tests. It copies its SAVED bytes to work RAM $FF0000 first, then writes a 16-byte pattern; typed edits give
    # each set its own first pattern byte, so what a set reads back says whose save it was given
    sys.path.insert(0, str(ROOT / "tests"))
    import genesis_cartridge_sram_generated_test as sram  # noqa: E402
    import genesis_persistence_test as persistence  # noqa: E402
    sram_rom = sram.build_rom(persistence.sram_guest())   # the cartridge of genesis_production_states_test, unchanged
    sram_path = write(tmp / "sram.rom", sram_rom)
    at = sram_rom.index(bytes.fromhex(sram.move_b_imm_abs(persistence.PATTERN[0], sram.SRAM_START)))
    assert sram_rom[at + 3] == persistence.PATTERN[0]

    def sram_edit(name, value):
        d = tmp / name
        write(d / "edits" / "main.edits", f"fixture u8 0x{at + 3:08x} 0x{persistence.PATTERN[0]:02x} 0x{value:02x}\n".encode())
        write(d / "mod.toml", manifest(name, sram_rom, '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'))
        return d

    quiet = luau_mod("sram-script", "function reset() end\n", ["host.log"], image=sram_rom)
    one_, two_ = sram_edit("sram-one", 0xA1), sram_edit("sram-two", 0xB2)
    saves = tmp / "sram-saves"
    programs = {}
    for label, members_ in (("A", [one_, quiet]), ("B", [two_])):   # a set with a provider and a data-only set
        args, program = build(f"the SRAM set {label}", members_, rom=sram_path, name=f"sram-{label}")
        d = ok(f"the SRAM set {label} builds", *args, "--sdl3-include", sdl / "include", "--sdl3-lib", sdl / "lib", "--test-frame-seam", "1")
        programs[label] = (program, saves / "sets" / d.get("set_identity", "missing") / "cartridge.srm", d.get("effective_sha256", ""))
    vanilla_sram = tmp / "sram-vanilla"
    cli.check(h.build(sram_path, vanilla_sram).returncode == 0, "the Vanilla SRAM program builds")
    single_sram = tmp / "sram-single"
    single_package = tmp / "sram-single.segapatch"
    packed_ = ok("pack the single SRAM mod", "mod", "pack", one_, "--out", single_package)
    cli.check(h.build(sram_path, single_sram, extra=("--data-package", single_package, "--data-identity", packed_["package_identity"])).returncode == 0, "the single-mod SRAM program builds")

    def sram_play(program, folder, image):
        p, _, _, ram = h.play(program, "sram-" + program.name, pad="20 Q\n", save=folder, rom_sha=image, budget=60_000_000)
        events = [json.loads(x.split(" ", 1)[1]) for x in p.stderr.splitlines() if x.startswith("VIEWER_PERSIST ")]
        return [e["status"] for e in events], (ram.read_bytes()[0:16] if ram.is_file() else b""), p.stderr

    a_program, a_file, a_image = programs["A"]
    b_program, b_file, b_image = programs["B"]
    statuses, read, log = sram_play(a_program, saves, a_image)
    cli.check(statuses == ["fresh", "stored"] and read == b"\xff" * 16 and a_file.is_file() and a_file.read_bytes()[0] == 0xA1, f"set A starts fresh and stores its own value: {statuses} {read.hex()} {log[-500:]}")
    a_bytes = a_file.read_bytes() if a_file.is_file() else b""
    statuses, read, log = sram_play(b_program, saves, b_image)
    cli.check(statuses == ["fresh", "stored"] and read == b"\xff" * 16 and b_file.is_file() and b_file.read_bytes()[0] == 0xB2 and a_file.read_bytes() == a_bytes,
              f"set B, given the same save folder, starts fresh, stores its own value and leaves A's file unchanged: {statuses} {read.hex()}")
    b_bytes = b_file.read_bytes() if b_file.is_file() else b""
    statuses, read, _ = sram_play(a_program, saves, a_image)
    cli.check(statuses == ["loaded", "stored"] and read[:1] == b"\xa1" and b_file.read_bytes() == b_bytes, f"reopening A retrieves A's value and leaves B's file unchanged: {statuses} {read.hex()}")
    a_bytes = a_file.read_bytes()
    statuses, read, _ = sram_play(b_program, saves, b_image)
    cli.check(statuses == ["loaded", "stored"] and read[:1] == b"\xb2" and a_file.read_bytes() == a_bytes, f"reopening B retrieves B's value and leaves A's file unchanged: {statuses} {read.hex()}")
    cli.check(not (saves / "cartridge.srm").exists(), "no set wrote the un-namespaced save")
    for label, program, value in (("Vanilla", vanilla_sram, persistence.PATTERN[0]), ("the single-mod program", single_sram, 0xA1)):
        folder = tmp / ("sram-plain-" + program.name)
        statuses, _, log = sram_play(program, folder, sha(sram_rom))
        cli.check(statuses == ["fresh", "stored"] and (folder / "cartridge.srm").is_file() and (folder / "cartridge.srm").read_bytes()[0] == value and not (folder / "sets").exists(),
                  f"{label} keeps its save directly in the given folder, as before: {statuses} {log[-400:]}")
    cli.check(a_file.read_bytes() == a_bytes, "the other programs did not touch a set's save")
    print("mod_set_build_test: SDL3 battery SRAM section ran")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_set_build_test")
