#!/usr/bin/env python3
"""SEG-145-T003 (ADR 0323): the original-hardware Genesis sound-mod workflow, on the project-authored sound fixture (platforms/genesis/sdk/examples/sound; no commercial input).

The game's own data-driven Z80 driver runs as generated-native Z80. Every edit class the task names is exercised as a typed, expected-value-asserted change of the ROM: track data, SFX sequence, DAC
samples, FM instrument data, command mapping table, selection behaviour (an immediate, and injected guest code), plus the same change delivered as a deterministic BPS. Proves: (1) the effective image
differs from the original EXACTLY in the declared bytes; (2) each change gives a different, reproducible PCM digest, and a change of one sound leaves the audio before that sound bit-identical; (3) a
wrong expected value is refused and produces nothing; (4) `mod diff-run --audio` accepts a legitimate sound mod and rejects, with E_AUDIO_STATIC_MATERIALIZATION, a driver code edit the product cannot
compile statically (which an ordinary build would only report as degraded audio), and with E_AUDIO_BASELINE a baseline that is already broken.
usage: mod_sound_workflow_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402
from mod_sound_test_support import BYTES_PER_FRAME, PAD, Programs, distinct, first_difference, load_fixture  # noqa: E402

if sys.platform == "win32":   # the headless program build is exercised on Linux and macOS (as mod_gameplay_example_test)
    print("mod_sound_workflow_test: skipped (the headless program build is exercised on Linux and macOS)")
    raise SystemExit(0)

cli = Cli(sys.argv[1])
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/genesis/sdk/examples/sound"
fixture = load_fixture(ROOT)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-sound-"))
programs = Programs(cli, SDK, CC, tmp)

FIXTURE_SHA = "92c51033a77f6af58de65bff8d6b822c14b5d7b0bdb234f69027267f613fdfee"
rom_bytes = fixture.make_rom()
L = fixture.layout()
rom = write(tmp / "fixture.md", rom_bytes)
cli.check(sha(rom_bytes) == FIXTURE_SHA and fixture.make_rom() == rom_bytes, f"the fixture is deterministic and pinned: {sha(rom_bytes)}")
for name in ("data", "luau", "native"):
    cli.check(f'sha256 = "{FIXTURE_SHA}"' in (EXAMPLE / name / "mod.toml").read_text(), f"{name}/mod.toml names the fixture image")


def edit(item, new, at=0, expected=None):
    """One `bytes` edit line at layout item + `at`, asserting the original bytes (or `expected`)."""
    offset = L[item][0] + at
    old = rom_bytes[offset:offset + len(new)] if expected is None else expected
    return f"fixture bytes 0x{offset:08x} {old.hex()} {new.hex()}\n"


def package(name, edits, source=None, extra="", extra_files=()):
    project = tmp / ("project-" + name)
    shutil.rmtree(project, ignore_errors=True)
    shutil.copytree(EXAMPLE / "data", project)
    text = (project / "mod.toml").read_text()
    if source is not None:
        text = text.replace(FIXTURE_SHA, sha(source))
    write(project / "mod.toml", text + extra)
    write(project / "edits/main.edits", edits)
    for path, data in extra_files:
        write(project / path, data)
    return project


def apply(project, image=rom, name=None):
    out = tmp / ((name or project.name) + ".img")
    rc, doc, p = cli.run("mod", "apply", project, "--rom", image, "--out", out)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{project.name}: apply: {doc} {p.stderr[-200:]}")
    return out


def declared_spans(text):
    spans = []
    for line in text.splitlines():
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        offset, new = int(parts[2], 16), parts[4]
        length = len(new) // 2 if parts[1] == "bytes" else 1
        value = bytes.fromhex(new if parts[1] == "bytes" else new[2:])
        spans.append((offset, length, value))
    return spans


# ---------------------------------------------------------------- 1. the example package: exactly the declared bytes change
example_edits = (EXAMPLE / "data/edits/main.edits").read_text()
example_image = apply(EXAMPLE / "data", name="example").read_bytes()
expected_image = bytearray(rom_bytes)
for offset, length, value in declared_spans(example_edits):
    expected_image[offset:offset + length] = value
cli.check(len(declared_spans(example_edits)) == 6 and example_image == bytes(expected_image), "the effective image is the original with exactly the six declared sites replaced: every other byte is identical")

# ---------------------------------------------------------------- 2. the programs
built = {}


def pcm_of(name, image, pad=PAD):
    if name not in built:
        p, out, status = programs.build(name, image)
        cli.check(p.returncode == 0 and status.get("genesis_audio") == "supported", f"{name}: builds with supported audio: {status.get('genesis_audio')} {p.stdout[-200:]}")
        built[name] = out
    _, lines, data = programs.run(built[name], pad=pad)
    capture, sound = lines["CAPTURE_SUMMARY"][0], lines["SOUND_SUMMARY"][0]
    cli.check(capture["outcome"] == "window_complete" and sound["sound_fault"] == 0 and sound["audio"]["non_silent"] is True, f"{name}: a complete, audible, fault-free window: {sound['sound_fault']}")
    return data, capture


vanilla, vcap = pcm_of("vanilla", rom)
again, _ = pcm_of("vanilla", rom)
cli.check(vanilla == again and len(vanilla) > 40 * BYTES_PER_FRAME, "the vanilla PCM is reproducible")
cli.check([v for v, _ in distinct(vcap["ram_frames"])] == [0, fixture.GAME_MUSIC, fixture.GAME_SFX], f"vanilla: the game asks for its music, then the effect on B: {distinct(vcap['ram_frames'])}")
music_at = distinct(vcap["ram_frames"])[1][1]
sfx_at = distinct(vcap["ram_frames"])[2][1]
cli.check(music_at < sfx_at, "the music starts before the effect")


def variant(name, edits, base=None, base_edits="", first_audible_frame=None):
    """A single edit class on top of `base_edits` (a selection change that makes the edited sound audible), compared with the base program."""
    project = package(name, base_edits + edits)
    image = apply(project)
    data, _ = pcm_of(name, image)
    repeat, _ = pcm_of(name, image)
    cli.check(data == repeat, f"{name}: the modded PCM is reproducible")
    reference = vanilla if base is None else base
    at = first_difference(reference, data)
    cli.check(at is not None, f"{name}: the PCM differs from its base")
    if first_audible_frame is not None and at is not None:
        cli.check(at >= (first_audible_frame - 2) * BYTES_PER_FRAME, f"{name}: the audio before the edited sound is bit-identical (first difference at byte {at}, sound starts about frame {first_audible_frame})")
    return data


# track data (music): audible from the music's start
variant("track", edit("theme", fixture.note(0, 0x12C, 2) + fixture.note(0, 0x0FE, 2)), first_audible_frame=music_at)
# SFX sequence and command mapping table: only the effect requested on B changes, so everything before it is identical
variant("sfx-sequence", edit("blip", fixture.note(1, 0x08F, 1)), first_audible_frame=sfx_at)
variant("command-table", edit("table_blip", bytes([1]), at=3), first_audible_frame=sfx_at)
# selection behaviour: the game asks for another effect
select_drum = edit("game_sfx_immediate", bytes([4]))
drum_base = variant("selection-drum", select_drum, first_audible_frame=sfx_at)
select_chime = edit("game_sfx_immediate", bytes([5]))
chime_base = variant("selection-chime", select_chime, first_audible_frame=sfx_at)
# DAC samples and FM instrument data, each against the base that plays that sound
variant("dac-samples", edit("drum", bytes([0xC0] * 6 + [0x40] * 6)), base=drum_base, base_edits=select_drum, first_audible_frame=sfx_at)
variant("instrument", edit("chime", bytes([0x10]), at=7), base=chime_base, base_edits=select_chime, first_audible_frame=sfx_at)
# an edit of data the run never plays changes nothing audible (the PCM is a function of what the guest plays, not of the image bytes)
unused_project = package("unused", edit("alt", fixture.note(0, 0x200, 2)))
unused_data, _ = pcm_of("unused", apply(unused_project))
cli.check(unused_data == vanilla, "an edit of a track the run never requests leaves the PCM bit-identical")

# ---------------------------------------------------------------- 3. mismatched original bytes
wrong = package("mismatch", edit("theme", fixture.note(0, 0x12C, 2), expected=b"\x00\x00\x00"))
out = tmp / "mismatch.img"
rc, doc, _ = cli.run("mod", "apply", wrong, "--rom", rom, "--out", out)
cli.check(rc == 1 and doc and doc.get("class") == "E_EDIT_EXPECTED_MISMATCH" and not out.exists(), f"wrong expected bytes are refused with nothing written: {doc}")


def gate(project, image=rom, extra=("--audio",)):
    return cli.run("mod", "diff-run", project, "--rom", image, "--cc", CC, "--sdk-dir", SDK, "--frames", "64", "--budget", "20000000", "--timeout", "120", "--input", PAD, "--ram", "32:3", *extra)


# ---------------------------------------------------------------- 4. the sound gate: data edits, injected code and BPS
rc, doc, p = gate(EXAMPLE / "data")
cli.check(rc == 0 and doc and doc.get("ok") is True and doc.get("pcm_differs") is True and doc.get("audio") == {"vanilla_status": "supported", "modded_status": "supported", "implementation": "original_z80_aot", "repeat_identical": True},
          f"the gate accepts the example data mod: {doc and (doc.get('class'), doc.get('error'), doc.get('audio'))} {p.stderr[-200:]}")
cli.check(doc and doc["vanilla"]["pcm_sha256"] != doc["modded"]["pcm_sha256"], "the gate reports both PCM digests")

# injected guest code: the whole selection instruction becomes a call into a routine in declared free space (typed edit + guest-asm, both asserting the original bytes)
site = L["game_sfx_instruction"][0]
free, free_bytes = L["free"]
call = bytes.fromhex("4eb9") + free.to_bytes(4, "big") + bytes.fromhex("4e71")
asm = f"free * 0x{free:08x} 0x{free_bytes:08x} 0x00\norg * 0x{free:08x}\nroutine:\n  move.b #5,0x{fixture.MAIL_SFX:08x}\n  rts\n"
injected = package("injected", edit("game_sfx_instruction", call), extra='\n[[source]]\nkind = "guest-asm"\npath = "asm/main.asm"\ncpu = "m68k"\n', extra_files=(("asm/main.asm", asm),))
injected_image = apply(injected).read_bytes()
changed = [i for i in range(len(rom_bytes)) if rom_bytes[i] != injected_image[i]]
cli.check(changed and all(site <= i < site + 8 or free <= i < free + free_bytes for i in changed), "injection changes only the asserted site and the declared free range")
rc, doc, _ = gate(injected)
cli.check(rc == 0 and doc and doc.get("ok") is True and doc.get("pcm_differs") is True and doc["audio"]["modded_status"] == "supported", f"the gate accepts injected selection code: {doc and (doc.get('class'), doc.get('error'))}")
injected_pcm, injected_cap = pcm_of("injected", tmp / "project-injected.img")
cli.check([v for v, _ in distinct(injected_cap["ram_frames"])] == [0, fixture.GAME_MUSIC, 5], "the injected routine selects the chime")
cli.check(injected_pcm != vanilla and first_difference(vanilla, injected_pcm) >= (sfx_at - 2) * BYTES_PER_FRAME, "the injected selection changes only the audio from the effect on (its timing differs from a one-byte edit: the call costs guest cycles)")

# the same data mod as a deterministic BPS payload (its declared target digest is the typed edits' effective image, which the product verifies before building) gives the same audio
patch = tmp / "example.bps"
rc, doc, _ = cli.run("mod", "bps", EXAMPLE / "data", "--rom", rom, "--out", patch)
cli.check(rc == 0 and doc and doc.get("ok") is True, f"the data mod exports a BPS: {doc}")
bps_project = tmp / "project-bps"
bps_project.mkdir()
text = (EXAMPLE / "data/mod.toml").read_text().split("[[source]]")[0]
write(bps_project / "mod.toml", text + f'[[source]]\nkind = "bps-image"\npath = "patch/sound.bps"\ntarget_sha256 = "{sha(example_image)}"\n')
write(bps_project / "patch/sound.bps", patch.read_bytes())
rc, doc, _ = gate(bps_project)
rc2, doc2, _ = gate(EXAMPLE / "data")
cli.check(rc == 0 and rc2 == 0 and doc["modded"]["pcm_sha256"] == doc2["modded"]["pcm_sha256"], f"BPS and typed edits give the same PCM digest: {doc and doc.get('error')}")

# ---------------------------------------------------------------- 5. a driver code edit that breaks static materialization
patch_address = L["driver_patch_address"][0]
breaking = package("breaking", edit("driver_scratch_store", bytes([patch_address & 0xFF, patch_address >> 8])))
rc, doc, _ = gate(breaking, extra=())
cli.check(rc == 0 and doc and doc.get("ok") is True, "without the gate the run is only a comparison (an ordinary build isolates the sound CPU and succeeds)")
broken_image = apply(breaking)
p, out, status = programs.build("breaking", broken_image)
cli.check(p.returncode == 0 and status.get("genesis_audio") == "degraded" and status.get("z80_audio_outcome") == "structural_code_mismatch", f"the product isolates the modified driver, never interprets it: {status.get('genesis_audio')}")
rc, doc, _ = gate(breaking)
cli.check(rc == 1 and doc and doc.get("class") == "E_AUDIO_STATIC_MATERIALIZATION" and "structural_code_mismatch" in doc.get("error", ""), f"the gate rejects the driver edit with the documented class: {doc and (doc.get('class'), doc.get('error'))}")

# the same break on a path only the compared run reaches: the DAC routine (played only after button B) writes a run-time value over compiled code. The build's own observation takes no input, so it reports
# `supported`; the gate must judge what the compared run did.
original_source = fixture.DRIVER_SOURCE
base_image, base_labels, _ = fixture.driver()
late_source = original_source.replace("dac:    ld e,0x2B\n        ld d,0x80\n        call ymwrite", "dac:    ld e,0x2B\n        ld d,0x80\n        call tramp")
late_source += "tramp:  ld a,(0x1F20)\n        add a,0x3C\n        ld (%d),a\n        jp %d\n" % (base_labels["patch"], base_labels["patch"])
fixture.DRIVER_SOURCE = late_source
late_image, late_labels, _ = fixture.driver()
fixture.DRIVER_SOURCE = original_source
cli.check(late_source != original_source and len(late_image) == len(base_image) and late_labels["patch"] == base_labels["patch"], "the late-path driver change keeps the image layout")
late_edits, i = select_drum, 0
while i < len(base_image):
    j = i
    while j < len(base_image) and base_image[j] != late_image[j]:
        j += 1
    if j > i:
        offset = fixture.DRIVER_ROM_BASE + i
        late_edits += f"fixture bytes 0x{offset:08x} {rom_bytes[offset:offset + j - i].hex()} {late_image[i:j].hex()}\n"
    i = j + 1
late = package("late-break", late_edits)
p, _, status = programs.build("late-break", apply(late))
cli.check(p.returncode == 0 and status.get("genesis_audio") == "supported", f"the build's input-free observation cannot see the late break: {status.get('genesis_audio')}")
rc, doc, _ = gate(late)
cli.check(rc == 1 and doc and doc.get("class") == "E_AUDIO_STATIC_MATERIALIZATION" and "sound_fault=57" in doc.get("error", ""), f"the gate rejects a sound CPU isolated during the compared run: {doc and (doc.get('class'), doc.get('error'), doc.get('audio'))}")

# a baseline whose own sound is broken cannot verify anything
on_broken = package("on-broken", f"fixture bytes 0x{L['theme'][0]:08x} {rom_bytes[L['theme'][0]:L['theme'][0] + 3].hex()} {fixture.note(0, 0x12C, 2).hex()}\n", source=broken_image.read_bytes())
rc, doc, _ = gate(on_broken, image=broken_image)
cli.check(rc == 1 and doc and doc.get("class") == "E_AUDIO_BASELINE", f"a broken baseline is refused: {doc and (doc.get('class'), doc.get('error'))}")

# the gate is a diff-run comparison only
rc, _, p = cli.run("mod", "test", EXAMPLE / "data", "--rom", rom, "--cc", CC, "--sdk-dir", SDK, "--audio")
cli.check(rc == 2 and "usage" in p.stderr, "--audio outside diff-run is a usage error")

shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_sound_workflow_test")
