#!/usr/bin/env python3
"""ADR 0291: the build-time sound-implementation contract of `segarecomp build`, on project-authored synthetic Genesis fixtures.

  * every Genesis build records WHICH audio implementation the executable has (`audio_implementation` in status.json): the default build is the original
    generated-native Z80 ("original_z80_aot", supported or degraded by a structural code mutation); nothing is ever a replacement unless it was asked for;
  * `--sound-driver gems` is refused (exit 6, stage `sound-driver`, `sound_driver_unverified`, no executable) when no uploaded Z80 image is a verified GEMS
    image - an unknown driver identity is never accepted silently, and there is no fallback build;
  * `--z80-alternatives 1` (compiled structural alternatives of the original path) is recorded as requested and with its statistics, and is not a replacement;
  * malformed selections are rejected, and the record is deterministic across repeated builds.
The positive case (a verified driver image replaced by the native implementation, compared with an independent reference) needs a commercial image and is
validated locally: docs/testing/genesis-native-gems-validation.md.

usage: genesis_native_gems_build_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import concurrent.futures
import json
import pathlib
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def build(rom_bytes, out, *extra):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir", str(root / "platforms" / "genesis"),
               "--optimize", "0", "--runtime-optimize", "1", *extra]
    done = subprocess.run(command, text=True, capture_output=True, timeout=1800)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return done, status


def executable(out):
    return next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-native-gems-") as directory:
        tmp = pathlib.Path(directory)
        multi, smc = fx.build("sound_multi_epoch"), fx.build("sound_smc")
        jobs = {"multi": (multi, ()), "multi_again": (multi, ()), "smc": (smc, ()), "smc_gems": (smc, ("--sound-driver", "gems")),
                "multi_gems": (multi, ("--sound-driver", "gems")), "multi_alt": (multi, ("--z80-alternatives", "1")),
                "smc_original": (smc, ("--sound-driver", "original"))}
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            futures = {name: pool.submit(build, rom, tmp / name, *extra) for name, (rom, extra) in jobs.items()}
            results = {name: f.result() for name, f in futures.items()}

        done, status = results["multi"]
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("schema") == 1 and audio.get("kind") == "original_z80_aot" and audio.get("requested") == "original" and
              audio.get("selection") == "default" and audio.get("audio_status") == "supported" and "driver" not in audio,
              "a default build records the original generated-native Z80 as its audio implementation, not a replacement")
        check(audio.get("structural_alternatives", {}).get("requested") is False and audio["structural_alternatives"].get("forms") == 0, "no structural alternatives were compiled")
        check(status.get("genesis_audio") == "supported", "the pre-existing genesis_audio status is unchanged for the original path")
        check(json.dumps(audio, sort_keys=True) == json.dumps(results["multi_again"][1].get("audio_implementation"), sort_keys=True), "the record is identical across repeated builds")

        done, status = results["smc"]
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("audio_status") == "degraded" and audio.get("reason") == "structural_code_mismatch" and
              status.get("genesis_audio") == "degraded" and status.get("z80_audio_outcome") == "structural_code_mismatch",
              "a structurally self-modifying driver is recorded as the original Z80 with degraded audio and its reason")
        done_original, status_original = results["smc_original"]
        check(done_original.returncode == 0 and status_original.get("audio_implementation") == status.get("audio_implementation"),
              "--sound-driver original is exactly the default")

        for name in ("smc_gems", "multi_gems"):
            done, status = results[name]
            audio = status.get("audio_implementation", {})
            check(done.returncode == 6 and "stage=sound-driver" in done.stdout and "sound_driver_unverified" in done.stdout and status.get("status") == "failed" and
                  status.get("diagnostic") == "sound_driver_unverified" and executable(tmp / name) is None,
                  "%s: an unverified driver identity is refused with a typed failure and no executable" % name)
            check(audio.get("requested") == "gems" and audio.get("kind") == "none" and audio.get("reason") == "sound_driver_unverified",
                  "%s: the failure record says gems was requested and nothing was built" % name)

        done, status = results["multi_alt"]
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("structural_alternatives", {}).get("requested") is True and
              "driver" not in audio, "--z80-alternatives is recorded as requested on the original path and is not a replacement driver")

        bad = tmp / "bad"
        for argument in (("--sound-driver", "other"), ("--sound-driver", ""), ("--platform", "master-system", "--sound-driver", "gems")):
            done, _ = build(multi, bad, *argument)
            check(done.returncode != 0 and executable(bad) is None, "rejected: %s" % " ".join(argument))

    print("failures: %d" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
