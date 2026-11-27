#!/usr/bin/env python3
"""SEG-145-T008 (ADR 0323): OPTIONAL audio capability negotiation in built programs, Luau and C11, on the project-authored sound fixture (platforms/genesis/sdk/examples/sound; no commercial input).

An audio capability is granted only with a servable mapping of its kind. For every case this proves that the one resolved grant set is what the build status, the generated artifact (names and, for C11,
the required/optional capability bitsets), the artifact's mapping rows, the running mod and MOD_LOADED all report: an optional capability without a usable mapping is absent (the program builds, the mod
attaches, its unrelated functionality works), one with a mapping works, and a required one without a mapping is refused at build with E_AUDIO_TARGET. With an SDL3 prefix the played programs are checked
too, including the HUD-only viewer route, which serves no audio and must not advertise it.
usage: mod_audio_optional_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root> [SDL3 prefix]"""
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402
from mod_sound_test_support import Programs, distinct, load_fixture  # noqa: E402

if sys.platform == "win32":   # the headless program build is exercised on Linux and macOS (as mod_gameplay_example_test)
    print("mod_audio_optional_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
SDL = sys.argv[5] if len(sys.argv) > 5 else ""
fixture = load_fixture(ROOT)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-audio-optional-"))
programs = Programs(cli, SDK, CC, tmp)
rom = write(tmp / "fixture.md", fixture.make_rom())

SEND, READ = "audio.command.send", "audio.event.read"
IDS = {"host.log": 1, "overlay.rgba": 6, "input.read": 9, READ: 13, SEND: 14}
PAD = "8:1:0x20,20:1:0x00"   # button C held across guest frame 10, where both providers read the pad


def mapping(ident, kind, offset, values, space="m68k.work_ram"):
    text = f'\n[[audio.mapping]]\nid = "{ident}"\nkind = "{kind}"\nspace = "{space}"\noffset = {offset}\nwidth = 1\n'
    return text + "".join(f'\n[[audio.mapping.value]]\nname = "{n}"\nvalue = {v}\n' for n, v in values)


SFX = mapping("sfx", "command", 32, [("drum", 4)])
EVENT = mapping("accepted", "event", 34, [("theme", 1), ("drum", 4)])
FOREIGN = mapping("voice", "command", 32, [("drum", 4)])             # a command mapping the API cannot serve (not sfx/music)
Z80_EVENT = mapping("accepted", "event", 34, [("theme", 1)], "z80.ram")   # an event location the scheduler cannot observe


def project(name, language, required, optional, mappings, script=None, send=False, read=False):
    d = tmp / ("project-" + name)
    shutil.rmtree(d, ignore_errors=True)
    source = ('[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n' if language == "luau"
              else '[[source]]\nkind = "native-c11"\npath = "src/provider.c"\n\n[abi]\nnative = 2\nminor = 4\n')
    quoted = lambda names: ", ".join(f'"{n}"' for n in names)
    write(d / "mod.toml", f'[mod]\nid = "optional-audio"\nauthor = "segarecomp tests"\nversion = "0.1.0"\nname = "Optional audio"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "genesis"\n\n[[target.revision]]\nlabel = "fixture"\n'
          f'sha256 = "{sha(rom.read_bytes())}"\n\n{source}\n[capabilities]\nrequired = [{quoted(required)}]\noptional = [{quoted(optional)}]\n{mappings}')
    if language == "luau":
        write(d / "scripts/main.luau", script if script is not None else luau_script(send, read))
    else:
        write(d / "src/provider.c", C_PROVIDER)
    return d


def luau_script(send, read):
    """Unrelated work (the pad and the log) always; audio calls only for what the case expects to be granted (an absent Luau binding faults when reached)."""
    lines = ['function post_input(frame: number)', '  if frame == 10 then', '    sega.log("alive pad=" .. sega.input.get(1))']
    if send:
        lines.append('    sega.audio.play_sfx("drum")')
    lines += ['  end', 'end']
    if read:
        lines += ['function frame_end(frame: number)', '  if frame == 6 then sega.log("accepted " .. (sega.audio.event("accepted") or "none")) end', 'end']
    return "\n".join(lines) + "\n"


# The C11 provider probes what the host granted and uses exactly that. It reports the probe, so a stale grant or a stale table would be visible.
C_PROVIDER = '''#include <segarecomp_mod_genesis.h>
#include <stddef.h>
static void line(const SegaModHost *host, const char *a, const char *b) {
  char text[96];
  uint32_t n = 0U, i;
  for (i = 0U; a[i] != '\\0'; ++i) text[n++] = a[i];
  for (i = 0U; b[i] != '\\0'; ++i) text[n++] = b[i];
  host->log(text, n);
}
static void phase(void *state, const SegaModHost *host) {
  const SegaModAudioApi *audio = ((const SegaGenesisFeatures *)host->platform_features)->audio;
  const int send = sega_mod_is_granted(host, SEGA_MOD_CAP_AUDIO_COMMAND_SEND), read = sega_mod_is_granted(host, SEGA_MOD_CAP_AUDIO_EVENT_READ);
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_POST_INPUT && host->frame == 10U) {
    uint32_t pad = 0U;
    char probe[] = "send=0 read=0 table=0";
    (void)host->input_get(1U, &pad);
    probe[5] = (char)('0' + send);
    probe[12] = (char)('0' + read);
    probe[20] = audio != 0 ? '1' : '0';
    line(host, "alive pad=", pad == 32U ? "32" : "other");
    line(host, "probe ", probe);
    if (send && audio != 0) (void)audio->play_sfx("drum");
  }
  if (host->phase == SEGA_MOD_PHASE_FRAME_END && host->frame == 6U && read && audio != 0) {
    char name[SEGA_MOD_AUDIO_NAME_BYTES];
    if (audio->read_event("accepted", name) == SEGA_MOD_OK) line(host, "accepted ", name[0] != '\\0' ? name : "none");
  }
}
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 4U, SEGA_MOD_PLATFORM, "optional-audio", 1U, NULL, phase, NULL, NULL, NULL, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { *state_out = NULL; return &provider; }
'''


def pack(directory, language):
    out = tmp / (directory.name + ".segapatch")
    rc, doc, p = cli.run("mod", "build", directory, "--out", out, "--cc", CC, "--sdk-dir", ROOT / "platforms")
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{directory.name}: packs: {doc} {p.stderr[-200:]}")
    return out, doc and doc.get("package_identity")


def names(text):
    return sorted(x for x in text.split(",") if x)


def check_artifacts(what, out, language, status, granted, rows):
    """The status, the generated artifact (and the native bitsets) and the mapping rows all carry the one resolved grant set."""
    key = "luau" if language == "luau" else "native_mod"
    cli.check(names(status.get(key, {}).get("capabilities", "")) == sorted(granted), f"{what}: status.json grants {sorted(granted)}: {status.get(key, {}).get('capabilities')}")
    artifact = (out / ("luau/luau_artifact.c" if language == "luau" else "native-mod/native_artifact.c")).read_text()
    symbol = "segarecomp_luau_capabilities" if language == "luau" else "segarecomp_mod_capabilities"
    declared = re.search(symbol + r'\[\] = "([^"]*)"', artifact)
    cli.check(declared is not None and names(declared.group(1)) == sorted(granted), f"{what}: the artifact grants {sorted(granted)}: {declared and declared.group(1)}")
    if language == "native":
        bits = re.search(r"segarecomp_mod_caps_required\[2\] = \{(\d+)ULL, (\d+)ULL\}, segarecomp_mod_caps_optional\[2\] = \{(\d+)ULL, (\d+)ULL\}", artifact)
        union = int(bits.group(1)) | int(bits.group(3))
        cli.check(union == sum(1 << IDS[n] for n in granted), f"{what}: the native capability bits are exactly the granted ones: {bits.groups()}")
    count = re.search(r"segarecomp_mod_audio_count = (\d+)UL", artifact)
    cli.check((int(count.group(1)) if count else 0) == rows, f"{what}: {rows} audio mapping rows: {count and count.group(1)}")


# ---------------------------------------------------------------- the matrix: (case, required, optional, mappings, audio capabilities granted, mapping rows)
BASE = ["host.log", "input.read"]
CASES = (
    ("optional-command-unmapped", BASE, [SEND], "", set(), 0),
    ("optional-event-unmapped", BASE, [READ], "", set(), 0),
    ("optional-unservable", BASE, [SEND, READ], FOREIGN + Z80_EVENT, set(), 0),
    ("command-valid-event-absent", BASE, [SEND, READ], SFX, {SEND}, 1),
    ("event-valid-command-absent", BASE, [SEND, READ], EVENT, {READ}, 2),
    ("optional-both-mapped", BASE, [SEND, READ], SFX + EVENT, {SEND, READ}, 3),
    ("optional-valid-beside-unservable", BASE, [SEND, READ], SFX + Z80_EVENT, {SEND}, 1),
    ("required-command-optional-event-absent", BASE + [SEND], [READ], SFX, {SEND}, 1),
)
for case, required, optional, mappings, audio, rows in CASES:
    for language in ("luau", "native"):
        what = f"{case} ({language})"
        granted = BASE + sorted(audio)
        directory = project(f"{case}-{language}", language, required, optional, mappings, send=SEND in audio, read=READ in audio)
        package, identity = pack(directory, language)
        p, out, status = programs.build(f"{case}-{language}", rom, package, identity, language == "native")
        cli.check(p.returncode == 0 and (out / "game").exists(), f"{what}: the program builds: {status.get('diagnostic')} {status.get('message')}")
        if not (out / "game").exists():
            continue
        check_artifacts(what, out, language, status, granted, rows)
        _, lines, _ = programs.run(out, pad=PAD, frames=32)
        cli.check("MOD_REFUSED" not in lines, f"{what}: the mod attaches: {lines.get('MOD_REFUSED')}")
        if "MOD_SUMMARY" not in lines:
            cli.check(False, f"{what}: no mod summary")
            continue
        loaded, summary = lines["MOD_LOADED"][0], lines["MOD_SUMMARY"][0]
        cli.check(names(loaded["capabilities"]) == sorted(granted), f"{what}: MOD_LOADED advertises exactly the grant set: {loaded['capabilities']}")
        log = [m["line"] for m in lines.get("MOD_LOG", [])]
        cli.check(summary["faulted"] is False and "alive pad=32" in log, f"{what}: the mod's unrelated work (pad read, log) runs without a fault: {summary['fault']} {log}")
        if language == "native":
            probe = f"probe send={int(SEND in audio)} read={int(READ in audio)} table={int(bool(audio))}"
            cli.check(probe in log, f"{what}: the provider sees exactly the grant (and a table only with one): {log}")
        forwarded = [v for v, _ in distinct(lines["CAPTURE_SUMMARY"][0]["ram_frames"])]
        cli.check(summary["committed_writes"] == int(SEND in audio) and forwarded == ([0, 1, 4] if SEND in audio else [0, 1]), f"{what}: the command works exactly when granted: writes={summary['committed_writes']} {forwarded}")
        cli.check(("accepted theme" in log) == (READ in audio), f"{what}: the event works exactly when granted: {log}")

# an absent Luau binding stays the established optional-capability behaviour: a script that reaches for it faults with cause `capability`
reach = project("reach-absent", "luau", BASE, [SEND], "", script='function post_input(frame: number)\n  local a = sega.audio\n  if frame == 10 then a.play_sfx("drum") end\nend\n')
package, identity = pack(reach, "luau")
p, out, _ = programs.build("reach-absent", rom, package, identity)
_, lines, _ = programs.run(out, pad=PAD, frames=32)
cli.check(p.returncode == 0 and lines["MOD_SUMMARY"][0]["fault"].startswith("E_MOD_FAULT cause=capability "), f"reaching the absent binding is a capability fault: {lines.get('MOD_SUMMARY')}")

# required capabilities without a usable mapping keep the build refusal
for case, required, mappings in (("required-command-unmapped", BASE + [SEND], ""), ("required-event-unmapped", BASE + [READ], ""),
                                 ("required-command-unservable", BASE + [SEND], FOREIGN), ("required-event-unservable", BASE + [READ], Z80_EVENT),
                                 ("required-command-valid-beside-unservable", BASE + [SEND], SFX + mapping("voice", "command", 36, [("x", 1)]))):
    for language in ("luau", "native"):
        directory = project(f"{case}-{language}", language, required, [], mappings, script=luau_script(False, False))
        package, identity = pack(directory, language)
        p, out, status = programs.build(f"{case}-{language}", rom, package, identity, language == "native")
        cli.check(p.returncode != 0 and status.get("diagnostic") == "E_AUDIO_TARGET" and not (out / "game").exists(), f"{case} ({language}): E_AUDIO_TARGET and no program: {status.get('diagnostic')} {status.get('message')}")

# ---------------------------------------------------------------- the played programs (SDL3 viewer, dummy drivers, explicit instruction budget)
if SDL and sys.platform == "linux":
    from viewer_mod_support import Harness  # noqa: E402
    harness = Harness(cli.exe, CC, ROOT, SDL, tmp)

    def played(name, language, required, optional, mappings, **script):
        directory = project(name, language, required, optional, mappings, **script)
        package, identity = pack(directory, language)
        out = tmp / ("viewer-" + name)
        built = harness.build(rom, out, package, identity, native=language == "native")
        cli.check(built.returncode == 0, f"viewer {name}: builds: {built.stdout[-300:]}")
        if built.returncode != 0:
            return out, None, {}
        run, lines, _, _ = harness.play(out, name)
        extra = [json.loads(t.partition(" ")[2]) for t in run.stderr.splitlines() if t.startswith("LUAU_MOD_LOADED ")]
        if extra:
            lines["LUAU_MOD_LOADED"] = extra
        cli.check(run.returncode == 0, f"viewer {name}: runs: {run.returncode} {run.stderr[-400:]}")
        return out, harness.status(out), lines

    for language in ("luau", "native"):
        for case, optional, mappings, audio, rows in (("absent", [SEND, READ], "", set(), 0), ("mixed", [SEND, READ], SFX, {SEND}, 1)):
            name = f"{case}-{language}"
            out, status, lines = played(name, language, BASE, optional, mappings, send=SEND in audio, read=False)
            if status is None:
                continue
            granted = BASE + sorted(audio)
            check_artifacts("viewer " + name, out, language, status, granted, rows)
            cli.check("MOD_REFUSED" not in lines and lines.get("MOD_LOADED") and names(lines["MOD_LOADED"][0]["capabilities"]) == sorted(granted), f"viewer {name}: attaches and advertises the grant set: {lines.get('MOD_LOADED')} {lines.get('MOD_REFUSED')}")
            summary = lines.get("MOD_SUMMARY", [{}])[0]
            cli.check(summary.get("faulted") is False and summary.get("committed_writes") == int(SEND in audio) and any(m.get("line", "").startswith("alive") for m in lines.get("MOD_LOG", [])),
                      f"viewer {name}: unrelated work and the granted command: {summary} {lines.get('MOD_LOG')}")
    # The HUD-only viewer route (a package whose REQUIRED set it serves) has no audio: the optional capability is not granted there even with a valid mapping, and nothing claims it is.
    hud = 'function render(frame: number)\n  sega.overlay.rect(0, 0, 8, 8, 0xff0000ff)\nend\n'
    out, status, lines = played("hud-optional-audio", "luau", ["host.log", "overlay.rgba"], [SEND], SFX, script=hud)
    if status is not None:
        advertised = status.get("luau", {}).get("capabilities", "")
        loaded = lines.get("LUAU_MOD_LOADED", [{}])[0].get("capabilities", "")
        artifact = (out / "luau/luau_artifact.c").read_text()
        cli.check(names(advertised) == names(loaded) == ["host.log", "overlay.rgba"] and "segarecomp_mod_audio_count" not in artifact and "MOD_REFUSED" not in lines,
                  f"the HUD route neither grants nor advertises audio: status={advertised} loaded={loaded}")
    print("mod_audio_optional_test: SDL3 viewer section ran")
else:
    print("mod_audio_optional_test: no SDL3 prefix: the viewer section did not run")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_audio_optional_test")
