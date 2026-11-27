#!/usr/bin/env python3
"""SEG-145-T008 (ADR 0323): the Luau and C11 audio API end to end, in built generated-native programs of the project-authored sound fixture (platforms/genesis/sdk/examples/sound; no commercial input).

Proves: (1) `mod check` / build refuse every call without a verified interface with stable classes (unknown name, undeclared mapping, missing "stop", no volume call, undeclared capability, a package whose
audio capability has no servable mapping, a C11 source that does not declare abi.minor 4); (2) the example Luau mod and its C11 twin reach the game's own driver only through the declared mailboxes: the
guest's own record of the forwarded commands equals the hand-derived sequence, and both give the SAME PCM digest, committed-write witness, event log and final guest state; (3) replay and save/restore are
deterministic; (4) a call the static check cannot see (computed name, aliased channel, wrong phase) faults only the mod, with the same stable cause for Luau and C11, commits nothing and leaves the guest
and its audio exactly Vanilla.
usage: mod_audio_api_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402
from mod_sound_test_support import PAD, Programs, distinct, load_fixture  # noqa: E402

if sys.platform == "win32":   # the headless program build is exercised on Linux and macOS (as mod_gameplay_example_test)
    print("mod_audio_api_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/genesis/sdk/examples/sound"
fixture = load_fixture(ROOT)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-audio-api-"))
programs = Programs(cli, SDK, CC, tmp)
rom = write(tmp / "fixture.md", fixture.make_rom())
LUAU, NATIVE = EXAMPLE / "luau", EXAMPLE / "native"


def project(base, name, toml=None, script=None):
    d = tmp / ("project-" + name)
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(base, d)
    if toml is not None:
        path = d / "mod.toml"
        text = path.read_text()
        changed = toml(text)
        cli.check(changed != text, f"{name}: the manifest mutation applies")
        path.write_text(changed)
    if script is not None:
        (d / ("scripts/main.luau" if base == LUAU else "src/provider.c")).write_text(script)
    return d


def without_mapping(text, mapping):
    """The manifest without one [[audio.mapping]] and its values."""
    blocks = re.split(r"(?m)^(?=\[\[audio\.mapping\]\])", text)
    return "".join(b for b in blocks if f'id = "{mapping}"' not in b)


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))} {p.stderr[-200:]}")


def pack(directory, native=False):
    out = tmp / (directory.name + ".segapatch")
    args = ["mod", "build", directory, "--out", out] + (["--cc", CC, "--sdk-dir", ROOT / "platforms"] if native else [])
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{directory.name}: packs: {doc} {p.stderr[-200:]}")
    return out, doc and doc.get("package_identity")


def build_mod(name, directory, native=False):
    package, identity = pack(directory, native)
    p, out, status = programs.build(name, rom, package, identity, native)
    return p, out, status


# ---------------------------------------------------------------- 1. the static check (`mod check`)
SCRIPT = (LUAU / "scripts/main.luau").read_text()
for what, code, mutate in (
        ("an unknown effect name", "E_AUDIO_NAME", lambda s: s.replace('play_sfx("drum")', 'play_sfx("boom")')),
        ("an sfx name on the music mapping", "E_AUDIO_NAME", lambda s: s.replace('play_music("alt")', 'play_music("drum")')),
        ("an undeclared event mapping", "E_AUDIO_TARGET", lambda s: s.replace('event("accepted")', 'event("voices")')),
        ("a command mapping is not an event", "E_AUDIO_TARGET", lambda s: s.replace('event("accepted")', 'event("sfx")')),
        ("a volume call (no verified control exists)", "E_LUAU_API", lambda s: s.replace('sega.audio.stop_music()', 'sega.audio.set_music_volume(3)')),
        ("a raw id instead of a name", "E_LUAU_TYPE", lambda s: s.replace('play_sfx("drum")', 'play_sfx(4)'))):
    refused(what, code, "mod", "check", project(LUAU, "check", script=mutate(SCRIPT)))
refused("play_music without a music mapping", "E_AUDIO_TARGET", "mod", "check", project(LUAU, "check", toml=lambda t: without_mapping(t, "music")))
refused("stop_music without a declared \"stop\" value", "E_AUDIO_NAME", "mod", "check", project(LUAU, "check", toml=lambda t: t.replace('name = "stop"\nvalue = 127\n\n# The byte', 'name = "halt"\nvalue = 127\n\n# The byte')))
refused("an audio call without the capability", "E_LUAU_CAPABILITY", "mod", "check",
        project(LUAU, "check", toml=lambda t: without_mapping(without_mapping(t, "music"), "sfx").replace('"audio.command.send", ', "")))
refused("a mailbox in a space the mod may not write", "E_SPACE_READONLY", "mod", "check", project(LUAU, "check", toml=lambda t: t.replace('space = "m68k.work_ram"\noffset = 32', 'space = "z80.ram"\noffset = 32')))

# ---------------------------------------------------------------- 2. the build/load rule: no verified interface = no capability
QUIET = "function post_input(frame: number)\nend\nfunction save() return {} end\nfunction load(s) end\n"
for name, toml in (
        ("no-mapping", lambda t: without_mapping(without_mapping(t, "music"), "sfx")),
        ("foreign-channel", lambda t: t.replace('id = "sfx"\nkind = "command"', 'id = "voice"\nkind = "command"')),
        ("event-in-z80-ram", lambda t: t.replace('space = "m68k.work_ram"\noffset = 34', 'space = "z80.ram"\noffset = 34'))):
    p, out, status = build_mod(name, project(LUAU, name, toml=toml, script=QUIET))
    cli.check(p.returncode != 0 and status.get("diagnostic") == "E_AUDIO_TARGET" and not (out / "game").exists(), f"{name}: refused at build with E_AUDIO_TARGET and no program: {status.get('diagnostic')} {p.stdout[-200:]}")

old_minor = project(NATIVE, "old-minor", toml=lambda t: t.replace("minor = 4", "minor = 3"))
refused("a C11 source using the audio table without abi.minor 4", "E_ABI_MINOR_UNDECLARED", "mod", "build", old_minor, "--out", tmp / "old.segapatch", "--cc", CC, "--sdk-dir", ROOT / "platforms")

# ---------------------------------------------------------------- 3. the examples in built programs: parity, hand-derived commands, replay and restore
p, vanilla_out, _ = programs.build("vanilla", rom)
cli.check(p.returncode == 0, f"vanilla builds: {p.stdout[-300:]}")
_, vlines, vpcm = programs.run(vanilla_out)
vcap = vlines["CAPTURE_SUMMARY"][0]

p, luau_out, status = build_mod("luau", LUAU)
cli.check(p.returncode == 0 and "audio.command.send" in status.get("luau", {}).get("capabilities", ""), f"the Luau example builds: {p.stdout[-300:]}")
p, native_out, status = build_mod("native", NATIVE, native=True)
cli.check(p.returncode == 0 and "audio.command.send" in status.get("native_mod", {}).get("capabilities", ""), f"the C11 example builds: {p.stdout[-300:]}")


def observe(out, **kw):
    rc, lines, pcm = programs.run(out, **kw)
    return lines, pcm


llines, lpcm = observe(luau_out)
nlines, npcm = observe(native_out)
lm, nm, lc, nc = llines["MOD_SUMMARY"][0], nlines["MOD_SUMMARY"][0], llines["CAPTURE_SUMMARY"][0], nlines["CAPTURE_SUMMARY"][0]
cli.check(lm["faulted"] is False and nm["faulted"] is False, f"neither mod faults: {lm['fault']} / {nm['fault']}")
# Hand derivation of the commands the GAME forwards to its driver (byte 0xFF0022): its own music (1) on turn 3; the mod's drum (4) for the C press of frame 10; the game's own blip (3) for B at 20; the mod's alt
# music (2) at guest frame 24; the mod's chime (5) for Right at 32; the mod's stop (127) at guest frame 44. Each change is sampled in the frame that asked for it (a captured frame's 0-based index is the guest
# frame counter or one less, as the sample is taken at the frame's end).
got = distinct(lc["ram_frames"])
cli.check([v for v, _ in got] == [0, 1, 4, 3, 2, 5, 127], f"the forwarded commands are the hand-derived sequence: {got}")
cli.check(all(asked - 1 <= at <= asked + 1 for (_, at), asked in zip(got[1:], (3, 10, 20, 24, 32, 44))), f"each command is forwarded in the frame that asked for it: {got}")
cli.check([v for v, _ in distinct(vcap["ram_frames"])] == [0, 1, 3], "Vanilla only has the game's own two requests")
cli.check(lm["committed_writes"] == 4 and nm["committed_writes"] == 4, "four mailbox writes: drum, alt, chime, stop")
cli.check(lm["write_digest"] == nm["write_digest"] and re.fullmatch(r"[0-9a-f]{64}", lm["write_digest"]), f"Luau and C11 commit the identical writes (same frame, phase, mailbox, value): {lm['write_digest']} / {nm['write_digest']}")
cli.check(lpcm == npcm and lc["ram_frames"] == nc["ram_frames"] and lc["final_work_ram_sha256"] == nc["final_work_ram_sha256"] and lc["digests"] == nc["digests"], "Luau and C11 give identical PCM, guest trace, state and frames")
cli.check(lpcm != vpcm, "the mapped commands are audible: the PCM differs from Vanilla")
want_log = [f"accepted {n}" for n in ("theme", "drum", "blip", "alt", "chime", "stop")]
cli.check([m["line"] for m in llines["MOD_LOG"]] == want_log and [m["line"] for m in nlines["MOD_LOG"]] == want_log, f"the read-only events: {[m['line'] for m in llines.get('MOD_LOG', [])]}")

for name, out, first_pcm, summary in (("luau", luau_out, lpcm, lm), ("native", native_out, npcm, nm)):
    again, pcm2 = observe(out, tag="again")
    cli.check(pcm2 == first_pcm and again["MOD_SUMMARY"][0] == summary, f"{name}: replay reproduces the PCM and the whole mod summary")
    state = tmp / (name + ".state")
    # run to the program's own guest stop (turn 90) so every compared state is final; save at guest frame 26 (between the alt music and the chime)
    saved, _ = observe(out, frames=1, first=200, extra={"SEGARECOMP_MOD_CHECKPOINT": "26", "SEGARECOMP_MOD_SAVE": str(state)}, tag="save")
    plain, _ = observe(out, frames=1, first=200, tag="plain")
    cli.check(saved["MOD_SUMMARY"][0]["roundtrip"]["status"] == "ok" and saved["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == plain["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"], f"{name}: the checkpoint round trip is lossless")
    loaded, _ = observe(out, frames=1, first=200, extra={"SEGARECOMP_MOD_LOAD": str(state)}, tag="load")
    cli.check(loaded["MOD_SUMMARY"][0]["write_digest"] == summary["write_digest"] and loaded["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == plain["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"],
              f"{name}: a fresh program restored mid-run ends with the identical guest state and write witness (the later commands are replayed)")

# ---------------------------------------------------------------- 4. calls only the run can see: one stable cause for both languages, nothing committed, guest and audio exactly Vanilla
FAULTS = {
    # name: (cause, phase, Luau body, C11 phase condition, C11 call)
    "computed-name": ("audio-name", "post-input", 'function post_input(frame: number)\n  if frame == 10 then sega.audio.play_sfx("dr" .. "ums") end\nend\n', "SEGA_MOD_PHASE_POST_INPUT", '(void)audio->play_sfx("drums");'),
    "unmapped-channel": ("audio-target", "post-input", 'function post_input(frame: number)\n  local a = sega.audio\n  if frame == 10 then a.play_music("alt") end\nend\n', "SEGA_MOD_PHASE_POST_INPUT", '(void)audio->play_music("alt");'),
    "command-in-render": ("phase", "render", 'function render(frame: number)\n  if frame == 10 then sega.audio.play_sfx("drum") end\nend\n', "SEGA_MOD_PHASE_RENDER", '(void)audio->play_sfx("drum");'),
    "valid-then-invalid": ("audio-name", "post-input", 'function post_input(frame: number)\n  if frame == 10 then\n    sega.audio.play_sfx("drum")\n    sega.audio.play_sfx("dr" .. "ums")\n  end\nend\n', "SEGA_MOD_PHASE_POST_INPUT",
                           '(void)audio->play_sfx("drum"); (void)audio->play_sfx("drums");'),
}
C_TEMPLATE = '''#include <segarecomp_mod_genesis.h>
#include <stddef.h>
static void phase(void *state, const SegaModHost *host) {
  const SegaModAudioApi *audio = ((const SegaGenesisFeatures *)host->platform_features)->audio;
  (void)state;
  if (audio != 0 && host->phase == %s && host->frame == 10U) { %s }
}
static int save_state(void *state, uint8_t *out, uint32_t cap, uint32_t *len) { (void)state; (void)out; (void)cap; *len = 0U; return 0; }
static int load_state(void *state, const uint8_t *in, uint32_t len) { (void)state; (void)in; return len == 0U ? 0 : 1; }
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 4U, SEGA_MOD_PLATFORM, "audio-fault", 1U, NULL, phase, NULL, save_state, load_state, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { *state_out = NULL; return &provider; }
'''
for name, (cause, phase_name, body, c_phase, c_call) in FAULTS.items():
    toml = (lambda t: without_mapping(t, "music")) if name == "unmapped-channel" else None
    script = body + "function save() return {} end\nfunction load(s) end\n"
    _, a_out, a_status = build_mod("luau-" + name, project(LUAU, "luau-" + name, toml=toml, script=script))
    _, b_out, b_status = build_mod("native-" + name, project(NATIVE, "native-" + name, toml=toml, script=C_TEMPLATE % (c_phase, c_call)), native=True)
    for language, out, status in (("luau", a_out, a_status), ("native", b_out, b_status)):
        cli.check((out / "game").exists(), f"{name} ({language}) builds: {status.get('diagnostic')} {status.get('message')}")
        if not (out / "game").exists():
            continue
        lines, pcm = observe(out)
        summary, capture = lines["MOD_SUMMARY"][0], lines["CAPTURE_SUMMARY"][0]
        match = re.fullmatch(r"E_MOD_FAULT cause=([a-z-]+) mod=[a-z0-9-]+ phase=([a-z-]+) frame=(\d+)", summary["fault"])
        cli.check(summary["faulted"] is True and match and match.group(1) == cause and match.group(2) == phase_name and match.group(3) == "10", f"{name} ({language}): {summary['fault']}, expected cause={cause} phase={phase_name} frame=10")
        cli.check(summary["committed_writes"] == 0 and capture["ram_frames"] == vcap["ram_frames"] and pcm == vpcm, f"{name} ({language}): nothing was committed; guest and audio are exactly Vanilla")

# the sound gate never calls a run with a faulted mod verified
rc, doc, _ = cli.run("mod", "diff-run", tmp / "project-luau-computed-name", "--rom", rom, "--cc", CC, "--sdk-dir", SDK, "--frames", "32", "--budget", "20000000", "--timeout", "120", "--input", PAD, "--audio")
cli.check(rc == 1 and doc and doc.get("class") == "E_AUDIO_MOD_FAULT" and "cause=audio-name" in doc.get("error", ""), f"--audio refuses a faulted mod: {doc and (doc.get('class'), doc.get('error'))}")

# mappings decide which guest bytes are written and observed: they are part of the content digest
digests = []
for mutate in (None, lambda t: t.replace("offset = 32", "offset = 36"), lambda t: t.replace('name = "chime"\nvalue = 5\n\n[[audio.mapping]]\nid = "music"', 'name = "chime"\nvalue = 6\n\n[[audio.mapping]]\nid = "music"')):
    rc, doc, _ = cli.run("mod", "check", project(LUAU, "digest", toml=mutate))
    cli.check(rc == 0 and doc, f"the digest variant checks: {doc}")
    digests.append(doc and doc.get("content_digest"))
cli.check(len(set(digests)) == 3 and None not in digests, f"a moved mailbox and another value each change the content digest: {digests}")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_audio_api_test")
