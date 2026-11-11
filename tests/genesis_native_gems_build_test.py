#!/usr/bin/env python3
"""ADR 0291: the build-time sound-implementation contract of `segarecomp build`, on project-authored synthetic Genesis fixtures.

  * every Genesis build records WHICH audio implementation the executable has (`audio_implementation`, schema 2, in status.json): by default (`--sound-driver auto`) the
    original generated-native Z80 ("original_z80_aot", supported, or degraded by a structural code mutation with no verified replacement) - selection `automatic`;
  * `--sound-driver gems` (developer override) is refused (exit 6, stage `sound-driver`, `sound_driver_unverified`, no executable) when no uploaded Z80 image is a verified
    GEMS image - an unknown driver identity is never accepted silently, and there is no fallback build; `--sound-driver original` is recorded as a developer override;
  * the AUTOMATIC selection of the replacement: a self-modifying driver whose image signature is in the catalog is built a second time with the replacement and recorded as
    `native_driver` / `automatic` / `original_aot_failure_verified_replacement` with the original AOT's failure evidence (the catalog of a temporary copy of the runtime tree is
    given the fixture's signature; the product catalog is untouched), while the same driver without a catalog entry keeps the original path;
  * `--z80-alternatives 1` (compiled structural alternatives of the original path) is recorded as requested and with its statistics, and is not a replacement;
  * malformed selections are rejected, and the record is deterministic across repeated builds.
The positive case on a real driver image (replaced by the native implementation and compared with an independent reference) needs a commercial image and is
validated locally: docs/testing/genesis-native-gems-validation.md.

usage: genesis_native_gems_build_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import concurrent.futures
import json
import pathlib
import re
import shutil
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


def build(rom_bytes, out, *extra, runtime=None):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir", str(runtime or root / "platforms" / "genesis"),
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
        check(done.returncode == 0 and audio.get("schema") == 2 and audio.get("kind") == "original_z80_aot" and audio.get("requested") == "auto" and
              audio.get("selection") == "automatic" and audio.get("reason") == "original_supported" and audio.get("audio_status") == "supported" and "driver" not in audio,
              "a default build records the original generated-native Z80 as its audio implementation, selected automatically, not a replacement")
        check(audio.get("save_states") == "supported" and audio.get("native_implementation_version", 0) >= 1 and audio.get("catalog_revision", 0) >= 1,
              "the record names the shipped replacement's implementation version, the catalog revision and the save-state capability")
        check(audio.get("structural_alternatives", {}).get("requested") is False and audio["structural_alternatives"].get("forms") == 0, "no structural alternatives were compiled")
        check(status.get("genesis_audio") == "supported", "the pre-existing genesis_audio status is unchanged for the original path")
        check(json.dumps(audio, sort_keys=True) == json.dumps(results["multi_again"][1].get("audio_implementation"), sort_keys=True), "the record is identical across repeated builds")

        done, status = results["smc"]
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("audio_status") == "degraded" and audio.get("degraded_reason") == "structural_code_mismatch" and
              audio.get("reason") == "no_verified_replacement" and audio.get("selection") == "automatic" and status.get("genesis_audio") == "degraded" and
              status.get("z80_audio_outcome") == "structural_code_mismatch",
              "a structurally self-modifying driver without a verified replacement stays the original Z80, degraded, and says why")
        done_original, status_original = results["smc_original"]
        original = dict(status_original.get("audio_implementation", {}))
        check(done_original.returncode == 0 and original.get("selection") == "developer_override" and original.get("reason") == "developer_override" and original.get("requested") == "original" and
              original.get("kind") == "original_z80_aot", "--sound-driver original is recorded as a developer override")
        for key in ("selection", "reason", "requested"):
            original.pop(key, None)
            audio.pop(key, None)
        check(original == audio, "and is otherwise exactly the default")

        for name in ("smc_gems", "multi_gems"):
            done, status = results[name]
            audio = status.get("audio_implementation", {})
            check(done.returncode == 6 and "stage=sound-driver" in done.stdout and "sound_driver_unverified" in done.stdout and status.get("status") == "failed" and
                  status.get("diagnostic") == "sound_driver_unverified" and executable(tmp / name) is None,
                  "%s: an unverified driver identity is refused with a typed failure and no executable" % name)
            check(audio.get("requested") == "gems" and audio.get("kind") == "none" and audio.get("reason") == "sound_driver_unverified" and audio.get("selection") == "developer_override",
                  "%s: the failure record says gems was required by a developer override and nothing was built" % name)

        done, status = results["multi_alt"]
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("structural_alternatives", {}).get("requested") is True and
              "driver" not in audio, "--z80-alternatives is recorded as requested on the original path and is not a replacement driver")

        # the automatic selection of the replacement, on a temporary copy of the runtime tree whose catalog lists the self-modifying fixture's image signature
        registry = (tmp / "smc" / "generated-z80" / "genesis_z80_registry.c").read_text()
        block = registry[registry.index("genesis_z80_image_signatures"):registry.index("const uint8_t genesis_z80_image_content_hashes")]
        signatures = ["".join("%02x" % int(b, 16) for b in row.split(",")) for row in re.findall(r"\{((?:0x[0-9A-F]{2},?){32})\}", block)]
        check(len(signatures) >= 1 and all(len(sig) == 64 for sig in signatures), "the fixture's image signatures were read from its generated registry")
        tree = tmp / "tree"
        shutil.copytree(root / "libs", tree / "libs")
        shutil.copytree(root / "platforms" / "genesis", tree / "platforms" / "genesis")
        catalog = tree / "platforms" / "genesis" / "runtime" / "gems_driver_catalog.inc"
        catalog.write_text(catalog.read_text() + "".join('GEMS_VERIFIED_IMAGE("gems", "2.5", "build_qualified", "%s")\n' % sig for sig in signatures))
        done, status = build(smc, tmp / "auto_native", runtime=tree / "platforms" / "genesis")
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "native_driver" and audio.get("requested") == "auto" and audio.get("selection") == "automatic" and
              audio.get("reason") == "original_aot_failure_verified_replacement" and audio.get("driver") == "gems" and audio.get("save_states") == "supported" and
              audio.get("original_aot", {}).get("outcome") == "structural_code_mismatch" and status.get("genesis_audio") in ("native", "degraded"),
              "a self-modifying driver with a verified image identity is built a second time with the replacement, automatically and with the original's failure as evidence")
        check("sound_driver=gems" in done.stdout and executable(tmp / "auto_native") is not None, "the build result names the replacement and produced an executable")
        # two images, the SECOND one faults: only a catalog line for the FAULTING image selects the replacement
        second = fx.build("sound_smc_second_image")
        done, status = build(second, tmp / "two_default")
        audio = status.get("audio_implementation", {})
        registry = (tmp / "two_default" / "generated-z80" / "genesis_z80_registry.c").read_text()
        block = registry[registry.index("genesis_z80_image_signatures"):registry.index("const uint8_t genesis_z80_image_content_hashes")]
        pair = ["".join("%02x" % int(b, 16) for b in row.split(",")) for row in re.findall(r"\{((?:0x[0-9A-F]{2},?){32})\}", block)]
        check(done.returncode == 0 and len(pair) == 2 and audio.get("audio_status") == "degraded" and audio.get("kind") == "original_z80_aot",
              "the two-image fixture faults the sound CPU on the original path and registers both images")
        def with_catalog(name, signature):
            copy = tmp / name
            shutil.copytree(tree, copy)
            file = copy / "platforms" / "genesis" / "runtime" / "gems_driver_catalog.inc"
            lines = [l for l in file.read_text().splitlines() if not l.startswith('GEMS_VERIFIED_IMAGE("gems", "2.5", "build_qualified"')]
            file.write_text("\n".join(lines) + '\nGEMS_VERIFIED_IMAGE("gems", "2.5", "build_qualified", "%s")\n' % signature)
            return copy / "platforms" / "genesis"
        done, status = build(second, tmp / "two_other", runtime=with_catalog("tree_other", pair[0]))
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("reason") == "no_verified_replacement" and audio.get("audio_status") == "degraded",
              "a verified image that did NOT fault is no evidence: a different image's fault keeps the original path (degraded, no verified replacement)")
        done, status = build(second, tmp / "two_faulting", runtime=with_catalog("tree_faulting", pair[1]))
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "native_driver" and audio.get("reason") == "original_aot_failure_verified_replacement" and audio.get("identity") == pair[1][:16],
              "the replacement is selected when the verified image is the one that faulted, and the record names that image")

        done, status = build(multi, tmp / "auto_original", runtime=tree / "platforms" / "genesis")
        audio = status.get("audio_implementation", {})
        check(done.returncode == 0 and audio.get("kind") == "original_z80_aot" and audio.get("reason") == "original_supported",
              "a verified identity alone changes nothing: the original path that works stays (the replacement needs the original's recorded failure)")

        bad = tmp / "bad"
        for argument in (("--sound-driver", "other"), ("--sound-driver", ""), ("--platform", "master-system", "--sound-driver", "gems")):
            done, _ = build(multi, bad, *argument)
            check(done.returncode != 0 and executable(bad) is None, "rejected: %s" % " ".join(argument))

    print("failures: %d" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
