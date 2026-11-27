#!/usr/bin/env python3
"""SEG-145-T004 (ADR 0325): original-hardware Master System sound mods of the synthetic PSG cartridge (platforms/master-system/sdk/examples/sound; project-authored driver, FABRICATED data only).

Real generated-native programs (the unmodified producer; the modded ones from `--data-package`), bounded by --frames and a timeout, compared by the headless PCM artifact (`audio.sha256`, s16le mono
44,100 Hz). Proves: each kind of change (command table, tone period data, step volumes, noise, the injected driver routine) changes the PCM digest, deterministically; muting through the same typed edits
yields an all-zero PCM artifact (the edits reach the PSG); the injected routine's effect equals the identical change made as data; a mismatched original fails closed; a driver patch outside the static
admission (an indirect jump, port I/O) is refused before any file exists; the FM probe finds no FM unit, in Vanilla and in the mod.
usage: mod_sms_sound_test.py <segarecomp-cli> <platforms-dir> <C compiler> <repo-root>"""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

cli = Cli(sys.argv[1])
EXE = cli.exe
SDK, CC, ROOT = Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
EXAMPLE = ROOT / "platforms/master-system/sdk/examples/sound"
spec = importlib.util.spec_from_file_location("sms_sound_fixture", EXAMPLE / "fixture_rom.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
tmp = Path(tempfile.mkdtemp(prefix="segarecomp-sms-sound-"))
FRAMES = "40"
PINNED_VANILLA_PCM = "e37854d9897e6b893bd5306b5792852c8f0a809544dc57d2a63af1bd066c2d54"
PINNED_MODDED_PCM = "813d99f3ed7f31df8c63e90a874172b8ca37bdad1455ad8b1f7702befbd12542"

rom = write(tmp / "sound.sms", fixture.build())
cli.check(f'sha256 = "{sha(rom.read_bytes())}"' in (EXAMPLE / "mod.toml").read_text() and fixture.build() == fixture.build(), "mod.toml names the deterministic fixture")
EDITS = [line for line in (EXAMPLE / "edits/main.edits").read_text().splitlines() if line and not line.startswith("#")]
commands, periods, volumes, noise, redirect = EDITS
ASM = (EXAMPLE / "asm/main.asm").read_text()


def project(name, edits, asm=None):
    d = tmp / name
    shutil.copytree(EXAMPLE, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
    (d / "edits/main.edits").write_text("\n".join(edits) + "\n")
    toml = (d / "mod.toml").read_text()
    if asm is None:   # a data-only change: no injected code
        (d / "mod.toml").write_text(toml.split("# Driver:")[0])
        shutil.rmtree(d / "asm")
    else:
        (d / "asm/main.asm").write_text(asm)
    return d


def pcm(name, package=None, identity=None):
    out = tmp / ("p-" + name)
    args = [EXE, "build", "--rom", rom, "--output", out, "--cc", CC, "--runtime-dir", SDK / "master-system", "--platform", "master-system", "--optimize", "0", "--cc-arg", "-Wno-misleading-indentation"]
    args += ["--mapper", "rom_only"] if package is None else ["--data-package", package, "--data-identity", identity]
    p = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=900)
    if p.returncode != 0:
        print(p.stdout[-1500:], (out / "build.log").read_text()[-2000:])
        raise SystemExit("mod_sms_sound_test: build failed: " + name)
    results = []
    for tag in ("a", "b"):
        artifacts = out / tag
        artifacts.mkdir()
        game = out / ("game.exe" if sys.platform == "win32" else "game")
        r = subprocess.run([str(game), "--frames", FRAMES, "--artifacts", str(artifacts)], text=True, capture_output=True, timeout=120)
        cli.check(r.returncode == 0, f"{name}: run {tag}: {r.returncode} {r.stdout[-300:]}")
        sums = (artifacts / "audio.sha256").read_text().splitlines()
        data = (artifacts / "audio.pcm").read_bytes()
        results.append((sums[-1].split()[2], data, (artifacts / "ram.bin").read_bytes()))
        cli.check(sums[-1].split()[2] == sha(data), f"{name}: the digest is the digest of the PCM artifact")
    cli.check(results[0] == results[1], f"{name}: two runs produce identical PCM and state")
    return results[0]


def variant(name, edits, asm=None):
    package = tmp / (name + ".segapatch")
    rc, doc, p = cli.run("mod", "pack", project(name, edits, asm), "--out", package, "--rom", rom)
    cli.check(rc == 0 and doc and doc.get("ok"), f"pack {name}: {doc} {p.stderr[-300:]}")
    return pcm(name, package, doc["package_identity"])


vanilla = pcm("vanilla")
cli.check(vanilla[0] == PINNED_VANILLA_PCM and any(vanilla[1]), f"the unmodified driver sounds, with the pinned PCM digest: {vanilla[0]}")
digests = {"vanilla": vanilla[0]}
for name, line in (("commands", commands), ("periods", periods), ("volumes", volumes), ("noise", noise)):
    digests[name] = variant(name, [line])[0]
patched = variant("driver", [redirect], ASM)
digests["driver"] = patched[0]
whole = variant("whole", EDITS, ASM)
digests["whole"] = whole[0]
cli.check(len(set(digests.values())) == len(digests), f"every kind of change gives its own PCM digest: {digests}")
cli.check(whole[0] == PINNED_MODDED_PCM and len(whole[1]) == len(vanilla[1]), f"the whole example has the pinned PCM digest over the same number of samples: {whole[0]}")

# muting the tone steps and the noise channel through the SAME kinds of typed edits yields silence (a constant sample: the artifact's rest level is not zero): independent evidence the edits are what the PSG plays
silent = variant("silent", ["fixture bytes 0x00001020 0203040506070809 0f0f0f0f0f0f0f0f", "fixture bytes 0x00001030 040c 040f"])
rest = {silent[1][i:i + 2] for i in range(0, len(silent[1]), 2)}
cli.check(len(silent[1]) == len(vanilla[1]) and len(rest) == 1 and len({vanilla[1][i:i + 2] for i in range(0, len(vanilla[1]), 2)}) > 1, f"all attenuations at 15: one constant sample over the same length: {sorted(rest)[:4]}")
# The injected routine adds 2 to the attenuation while bit 4 of the frame counter is set. With a constant volume table the patched driver equals the UNPATCHED driver playing, in those steps, a table two
# units quieter: frames 16-31 are steps 4-7 (a step lasts 4 frames and the table index is the step).
flat = "fixture bytes 0x00001020 0203040506070809 0404040404040404"
patched_flat = variant("driver-flat", [flat, redirect], ASM)
by_data = variant("data-equivalent", ["fixture bytes 0x00001020 0203040506070809 0404040406060606"])
cli.check(patched_flat[0] != variant("flat", [flat])[0], "the injected routine is audible")
cli.check(patched[2][0:3] == vanilla[2][0:3] and patched_flat[2][0:3] == by_data[2][0:3], "the driver patch changes what is played, not the driver's frame and step bookkeeping")

# the FM unit is absent: with an FM unit the two probe reads echo the 1 and the 0 written to port $F2
for name, result in (("vanilla", vanilla), ("whole", whole)):
    first, second = result[2][0x20], result[2][0x21]
    cli.check(first == second and not (first & 1 == 1 and second & 1 == 0), f"{name}: no YM2413 FM unit answers the probe ({first:#x}, {second:#x})")

# fail closed: a mismatched original, and a driver patch the Z80 owner does not admit
wrong = commands.replace("0001020302010003", "0001020302010000")
bad = project("mismatch", [wrong])
rc, doc, p = cli.run("mod", "apply", bad, "--rom", rom, "--out", tmp / "never.sms")
cli.check(rc == 1 and doc and doc.get("class") == "E_EDIT_EXPECTED_MISMATCH" and not (tmp / "never.sms").exists(), f"a mismatched original builds nothing: {doc}")
for name, asm, cls in (("indirect", "free * 0x2000 0x40 0xff\norg * 0x2000\n  jp (hl)\n", "E_GUEST_NOT_ADMITTED"),
                       ("port-io", "free * 0x2000 0x40 0xff\norg * 0x2000\n  out (0x7f),a\n  ret\n", "E_GUEST_FORBIDDEN_INSTRUCTION"),
                       ("falls-off", "free * 0x2000 0x40 0xff\norg * 0x2000\n  add a,2\n", "E_GUEST_NOT_ADMITTED")):
    never = tmp / (name + ".segapatch")
    rc, doc, p = cli.run("mod", "pack", project(name, [redirect], asm), "--out", never)
    cli.check(rc == 1 and doc and doc.get("class") == cls and not never.exists(), f"driver patch {name}: {cls}, got {doc and doc.get('class')}")

# the developer workflow: `mod test` compares the PCM digest of the modded program
t_dir = tmp / "workflow"
shutil.copytree(EXAMPLE, t_dir, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
if sys.platform != "win32":
    rc, doc, p = cli.run("mod", "test", t_dir, "--rom", rom, "--cc", CC, "--sdk-dir", SDK)
    cli.check(rc == 0 and doc and doc.get("ok") and doc["scenarios"][0]["observed"]["pcm_sha256"] == PINNED_MODDED_PCM, f"mod test verifies the pinned PCM digest: {doc}")

tree = sorted(str(f.relative_to(EXAMPLE)) for f in EXAMPLE.rglob("*") if f.is_file() and "__pycache__" not in f.parts)
cli.check(tree == ["README.md", "asm/main.asm", "edits/main.edits", "fixture_rom.py", "mod.toml", "tests.toml"], f"the sound example is the published set of files: {tree}")
shutil.rmtree(tmp, ignore_errors=True)
cli.done("mod_sms_sound_test")
