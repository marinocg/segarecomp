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
not_built("colliding native symbols", "E_SET_NATIVE_LINK", [clash, clash_two], approve=(identity(clash), identity(clash_two)))

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

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_set_build_test")
