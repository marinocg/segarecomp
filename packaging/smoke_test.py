#!/usr/bin/env python3
"""Package smoke test: exercise an EXTRACTED release package with a project-authored synthetic ROM.

usage: smoke_test.py <path-to-extracted-launcher-executable>

The package is driven through the launcher's non-interactive mode (`Segarecomp --build <rom> --run`), the same
core the GUI uses. The child environment is scrubbed: an empty PATH (no cc/clang/gcc/zig/python/cmake/ninja
can be found), no CC/CXX, a throw-away HOME and cache. Nothing from the build tree is referenced.
This script itself is CI glue and is not part of the shipped package.
"""
import re
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile

# Reset SSP, reset PC=8, MOVEQ #0,D0 then RESET: a deterministic unsupported-instruction stop
# (the sanitized checkpoint the synthetic route is expected to end in).
IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))


CACHE_FOR_DIAGNOSTICS = []


# `segarecomp build` logs a link step as the pseudo-command `$ (link)` (target detail is on a `# link target=` comment line).
LINK_LINE = re.compile(r"^\$ \(link\)$")

def hermetic_compile_lines(build_log, compiler):
    """True when the build log has command lines and every `$ ` line is the bundled compiler or the `$ (link)` pseudo-marker.
    `#` comment lines (e.g. `# link target=`) are not commands and are ignored."""
    lines = [line for line in build_log.splitlines() if line.startswith("$ ")]
    return bool(lines) and all(line.startswith(f"$ {compiler}") or LINK_LINE.match(line) for line in lines)


def require(condition, message):
    if not condition:
        print("SMOKE FAIL:", message, file=sys.stderr)
        for cache in CACHE_FOR_DIAGNOSTICS:  # show the preserved diagnostics of any failed build
            for log in sorted(cache.glob("games/*/build.log")):
                print(f"---- {log} ----\n" + log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        sys.exit(1)


def parse(report):
    return dict(line.split("=", 1) for line in report.read_text(encoding="utf-8").splitlines() if "=" in line)


def smoke_master_system(tmp, launch):
    """Master System through the same launcher core, with the project-authored `machine_e2e` fixture (built from
    tools/sms_fixture_rom.py, never committed). The mapper is declared, never defaulted; the viewer run is finite."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
    import sms_fixture_rom as builder
    roms = tmp / "sms-roms"
    builder.main(["--out", str(roms), "--fixture", "machine_e2e"])
    sidecar_rom = roms / "machine_e2e.sms"  # the builder also wrote machine_e2e.mapper.json beside it
    (tmp / "sms-plain").mkdir()
    rom = tmp / "sms-plain" / "machine_e2e.sms"  # the same image with no manifest beside it
    rom.write_bytes(sidecar_rom.read_bytes())
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()

    # No mapper declaration: typed fail-closed failure (clear message), diagnostics preserved, nothing run.
    code, undeclared, _ = launch(rom, "sms-undeclared.txt", run=False)
    require(code == 3 and undeclared.get("build") == "failed" and "must be declared" in undeclared.get("message", ""),
            f"an undeclared SMS mapper must fail closed: {undeclared}")
    require(undeclared.get("platform") == "master-system" and undeclared.get("mapper") == "undeclared", f"platform not shown: {undeclared}")
    log = pathlib.Path(undeclared["entry"] + ".failed") / "build.log"
    require('"diagnostic":"SMS_ERROR_MAPPER_UNDECLARED"' in (log.parent / "status.json").read_text(encoding="utf-8"),
            "the typed diagnostic must be preserved in status.json")
    require(not (log.parent / "generated").exists(), "nothing may be emitted before the mapper is declared")

    # Declared mapper: build with the bundled compiler and run the viewer for a finite number of frames (dummy video/audio).
    frames = ["--game-arg", "--viewer-frames", "--game-arg", "60", "--game-arg", "--viewer-unthrottled", "--game-arg", "--viewer-mute"]
    code, built, _ = launch(rom, "sms-first.txt", extra=["--mapper", "sega"] + frames)
    require(code == 0 and built["build"] == "ok" and built["run"] == "exited code=0", f"SMS build+run failed rc={code}: {built}")
    require(built["rom_sha256"] == digest and built["platform"] == "master-system" and built["mapper"] == "sega", f"SMS report: {built}")
    entry = pathlib.Path(built["entry"])
    compiler = pathlib.Path(built["compiler"]).resolve()
    build_log = (entry / "build.log").read_text(encoding="utf-8")
    require(f"cc={compiler}" in build_log and "mapper=sega" in build_log and "mapper_declaration_source=build_option" in build_log,
            "SMS build.log must record the toolchain, mapper and declaration source")
    require(hermetic_compile_lines(build_log, compiler), "every SMS compile command must invoke the bundled compiler")
    require("viewer outcome 1 frames 60" in (entry / "run.log").read_text(encoding="utf-8"), "the SMS viewer must run its 60 frames")
    meta = (entry / "metadata.json").read_text(encoding="utf-8")
    require('"platform":"master-system"' in meta and '"mapper":"sega"' in meta and '"profile":"sms2_ntsc_export"' in meta and str(rom) not in meta,
            f"SMS metadata provenance: {meta}")
    require(not list(entry.rglob("machine_e2e.sms")), "the ROM must not be copied into the cache")

    # Same declaration: cache hit. A sidecar manifest (another declaration form) selects another cache entry.
    code, again, _ = launch(rom, "sms-second.txt", run=False, extra=["--mapper", "sega"])
    require(code == 0 and again["cache"] == "hit", f"expected an SMS cache hit: {again}")
    code, other, _ = launch(sidecar_rom, "sms-third.txt", run=False)
    require(code == 0 and other["cache"] == "miss" and other["entry"] != built["entry"] and other["mapper"] == "manifest",
            f"the sidecar manifest declaration must key a separate entry: {other}")


# ---- SEG-049-T007 (ADR 0106): Optimized (v1) and the experimental gen2 model through the PACKAGED launcher and CLI ------------------------------
# Golden values of the 512-byte synthetic cartridge below, captured from the Linux package of this checkpoint (and reproduced by the macOS-arm64 and
# Windows package jobs of the same change). Every platform's package must reproduce them exactly: same model, schema and analysis contract, and
# bit-identical native inference (a divergence of the native model on any platform fails the package job).
OPT_GOLDEN = {
    "v1": {"effective": "ml_region", "model_family": "v1", "selected_ppm": "0", "admitted_ppm": "1000000",
           "k_sha256": "5f299d8b8999df7b8f2917e650cc39c768ddf71a9532a5ed553846602ca6a2b2"},
    "gen2": {"effective": "ml_region_gen2", "model_family": "gen2", "selected_ppm": "500000", "admitted_ppm": "19685",
             "k_sha256": "882a43982be3ab33e116cfda125f5f389cb8374fdaa123b75ca967acd848bbb3"},
}


def optimized_image():
    """The synthetic stop image (IMAGE) in a 512-byte cartridge with a Genesis header, so the launcher offers Optimized for it. Project-authored."""
    image = bytearray(0x200)
    image[:len(IMAGE)] = IMAGE
    image[0x100:0x110] = b"SEGA GENESIS    "
    return bytes(image)


def expected_policy_identity(policy, model):
    """Independent (Python, from the committed frozen records) re-derivation of `segarecomp aot-policy-identity`."""
    import json
    tools = pathlib.Path(__file__).resolve().parent.parent / "tools"
    frozen = json.loads((tools / "segarecomp_ml_region_gen2.frozen.json").read_text())
    inc = (pathlib.Path(__file__).resolve().parent.parent / "platforms/genesis/machine/src/ml_region_gen2_model_data.inc").read_text()
    weights = re.search(r'g2_weight_table_sha256 = "([0-9a-f]{64})"', inc).group(1)
    text = f"segarecomp.aot_policy_identity.v1|contract=segarecomp.admission_contract.v1|requested={policy}"
    if policy == "optimized" and model == "gen2":
        text += (f"|family=gen2|producer=ml_region_gen2|model={frozen['model_artifact']['sha256']}|schema={frozen['representation_sha256']}"
                 f"|tokens={frozen['token_schema_sha256']}|rank=B35:{frozen['rank_plan_sha256']}|weights={weights}")
    elif policy == "optimized":
        text += ("|family=v1|producer=ml_region|model=seg046-features-v1|schema=d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570"
                 "|tokens=|rank=:|weights=")
    return hashlib.sha256(text.encode()).hexdigest()


def smoke_optimized(tmp, launch):
    import json
    rom = tmp / "optimized.bin"
    rom.write_bytes(optimized_image())
    modes = {"compatibility": [], "v1": ["--aot-policy", "optimized"], "gen2": ["--aot-policy", "optimized", "--aot-model", "gen2"]}
    entries, reports = {}, {}
    for name, extra in modes.items():
        code, first, _ = launch(rom, f"opt-{name}-1.txt", extra=extra)
        require(code == 0 and first["build"] == "ok" and first["cache"] == "miss" and first["run"] == "exited code=0", f"{name}: build+run failed rc={code}: {first}")
        code, second, _ = launch(rom, f"opt-{name}-2.txt", run=False, extra=extra)
        require(code == 0 and second["cache"] == "hit" and "build" not in second and second["entry"] == first["entry"], f"{name}: expected a cache hit: {second}")
        entries[name], reports[name] = first["entry"], second
    require(len(set(entries.values())) == 3, f"Compatibility, Optimized (v1) and the experimental model must use separate cache entries: {entries}")
    comp = reports["compatibility"]
    require(comp["aot_policy_effective"] == "broad" and comp["aot_policy_fallback"] == "no" and comp["aot_policy_model_family"] == "none" and
            comp["aot_policy_policy_identity"] == expected_policy_identity("compatibility", "none"), f"Compatibility report: {comp}")
    for name in ("v1", "gen2"):
        got, want = reports[name], OPT_GOLDEN[name]
        require(got["aot_policy_requested"] == "optimized" and got["aot_policy_requested_model"] == name, f"{name}: request: {got}")
        require(got["aot_policy_effective"] == want["effective"] and got["aot_policy_model_family"] == want["model_family"] and
                got["aot_policy_fallback"] == "no" and got["aot_policy_reason"] == "none" and got["aot_policy_fallback_class"] == "none" and
                got["aot_policy_validator"] == "accepted", f"{name}: the packaged build must be selective, not a fallback: {got}")
        require(got["aot_policy_selected_ppm"] == want["selected_ppm"] and got["aot_policy_admitted_ppm"] == want["admitted_ppm"],
                f"{name}: selected/admitted fractions differ from the development build: {got}")
        require(got["aot_policy_policy_identity"] == expected_policy_identity("optimized", name), f"{name}: policy identity differs from the development contract: {got}")
        status = json.loads((pathlib.Path(entries[name]) / "status.json").read_text())["aot_policy"]
        require(status["k_sha256"] == want["k_sha256"], f"{name}: admitted-set digest differs from the development build (cross-platform determinism)")
        require(status["analysis_contract"] == "segarecomp.admission_contract.v1" and status["cache_reuse"] == "none", f"{name}: status: {status}")
    gen2 = json.loads((pathlib.Path(entries["gen2"]) / "status.json").read_text())["aot_policy"]
    frozen = json.loads((pathlib.Path(__file__).resolve().parent.parent / "tools/segarecomp_ml_region_gen2.frozen.json").read_text())
    require(gen2["model"] == frozen["model_artifact"]["sha256"] and gen2["schema_sha256"] == frozen["representation_sha256"] and
            gen2["token_contract_sha256"] == frozen["token_schema_sha256"] and gen2["rank_plan_sha256"] == frozen["rank_plan_sha256"] and gen2["rank_policy"] == "B35",
            f"the packaged model identity must equal the frozen research record: {gen2}")
    # The packaged CLI reports the same product identity (it is what the launcher keyed on).
    for policy, model, name in (("compatibility", "v1", "compatibility"), ("optimized", "v1", "v1"), ("optimized", "gen2", "gen2")):
        require(reports[name]["aot_policy_policy_identity"] == expected_policy_identity(policy, model if policy == "optimized" else "none"), f"{name}: identity parity")


# ---- SEG-057-T002 (ADR 0119): the experimental Master System Optimized policy through the PACKAGED launcher and CLI ---------------------------
# Golden values of the 32 KiB synthetic cartridge below (captured from the Linux package of this checkpoint): the native D512HP inference is
# bit-identical on every IEEE-754 host, so every platform's package must select exactly the same cells and plan (a divergence fails the job).
SMS_OPT_GOLDEN = {"cells": "1024", "ml_selected": "666", "selected_ppm": "650390", "admitted_ppm": "657196",
                  "plan_sha256": "ad4c411a1e377949820cb837081fa4fa85e5f0ce7db523674a9913b927ed178d"}


def sms_image():
    """A synthetic 32 KiB Master System cartridge: deterministic bytes and a `TMR SEGA` header (project-authored, no game data)."""
    image = bytearray((i * 31 + 7) & 0xFF for i in range(0x8000))
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C  # SMS export, 32 KiB
    return bytes(image)


def expected_sms_policy_identity(policy):
    """Independent (Python, from the committed weight record) re-derivation of `segarecomp aot-policy-identity --platform master-system`."""
    import json
    meta = json.loads((pathlib.Path(__file__).resolve().parent.parent / "tools/segarecomp_ml_region_z80_gen2.weights.json").read_text())
    text = f"segarecomp.sms_aot_policy_identity.v1|requested={policy}"
    if policy == "optimized":
        text += ("|sms_z80_d512hp:6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11:D512HP/3seeds:"
                 "cell32-top2mean-roots(0000,0038,0066)-budget65-ceil-tie:lower-index:sgz80w01/tokens256+pad/logit-per-byte/context512:"
                 f"segarecomp.z80_admission.v1|artifact={meta['blob_sha256']}")
    return hashlib.sha256(text.encode()).hexdigest()


def smoke_sms_optimized(tmp, launch):
    import json
    rom = tmp / "sms-optimized.sms"
    rom.write_bytes(sms_image())
    modes = {"compatibility": [], "optimized": ["--aot-policy", "optimized"]}
    entries, reports = {}, {}
    for name, extra in modes.items():
        code, first, _ = launch(rom, f"sms-{name}-1.txt", run=False, extra=["--mapper", "sega", *extra])
        require(code == 0 and first["build"] == "ok" and first["cache"] == "miss", f"SMS {name}: build failed rc={code}: {first}")
        code, second, _ = launch(rom, f"sms-{name}-2.txt", run=False, extra=["--mapper", "sega", *extra])
        require(code == 0 and second["cache"] == "hit" and "build" not in second and second["entry"] == first["entry"], f"SMS {name}: expected a cache hit: {second}")
        entries[name], reports[name] = first["entry"], second
    require(entries["compatibility"] != entries["optimized"], f"SMS Compatibility and Optimized must use separate cache entries: {entries}")
    comp, opt = reports["compatibility"], reports["optimized"]
    require(comp["aot_policy_requested"] == "compatibility" and comp["aot_policy_effective"] == "broad" and comp["aot_policy_fallback"] == "no" and
            comp["aot_policy_policy_identity"] == expected_sms_policy_identity("compatibility"), f"SMS Compatibility report: {comp}")
    require(opt["aot_policy_requested"] == "optimized" and opt["aot_policy_effective"] == "ml_region_z80" and opt["aot_policy_model_family"] == "sms_z80_d512hp" and
            opt["aot_policy_fallback"] == "no" and opt["aot_policy_reason"] == "none" and opt["aot_policy_validator"] == "accepted",
            f"SMS: the packaged build must be selective, not a fallback: {opt}")
    require(opt["aot_policy_policy_identity"] == expected_sms_policy_identity("optimized"), f"SMS: policy identity differs from the development contract: {opt}")
    status = json.loads((pathlib.Path(entries["optimized"]) / "status.json").read_text())["aot_policy"]
    require(status["model"] == "6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11" and
            status["artifact_sha256"] == "81434de0da4755654e14c370fc32de7898532193cbc5a514033268ba6669c2ac", f"SMS: embedded model identity: {status}")
    for key, want in SMS_OPT_GOLDEN.items():
        require(str(status[key]) == want, f"SMS: {key} differs from the development build (cross-platform determinism): {status[key]} != {want}")
    require(status["analysis_contract"] == "segarecomp.z80_admission.v1" and status["cache_reuse"] == "none", f"SMS: status: {status}")


def main():
    launcher = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="segarecomp-smoke-") as directory:
        tmp = pathlib.Path(directory)
        (tmp / "emptybin").mkdir()
        (tmp / "home").mkdir()
        # Decoy host tools: if the build ever resolved a compiler through PATH it would trip these.
        decoys = ("cc", "gcc", "clang", "zig", "c99") if os.name != "nt" else ()
        for name in decoys:
            script = tmp / "emptybin" / name
            script.write_text(f"#!/bin/sh\necho {name} >> '{tmp}/decoy-used'\nexit 99\n")
            script.chmod(0o755)
        env = {"PATH": str(tmp / "emptybin"), "HOME": str(tmp / "home"), "SEGARECOMP_CACHE_DIR": str(tmp / "cache"),
               "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
        if os.name == "nt":
            system = os.environ.get("SystemRoot", r"C:\Windows")
            env.update(SystemRoot=system, PATH=str(tmp / "emptybin") + ";" + system + r"\System32",
                       LOCALAPPDATA=str(tmp / "home"), TEMP=str(tmp), TMP=str(tmp))
            env["SEGARECOMP_CACHE_DIR"] = str(tmp / "cache")
        CACHE_FOR_DIAGNOSTICS.append(tmp / "cache")
        rom = tmp / "synthetic.bin"
        rom.write_bytes(IMAGE)
        digest = hashlib.sha256(IMAGE).hexdigest()

        def launch(rom_path, report_name, run=True, extra=()):
            report = tmp / report_name
            command = [str(launcher), "--build", str(rom_path), "--report", str(report)] + (["--run"] if run else []) + list(extra)
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=900)
            return result.returncode, (parse(report) if report.exists() else {}), result

        # 1-3, 6-9: build with the bundled compiler, run the produced native program.
        code, first, result = launch(rom, "first.txt")
        require(code == 0, f"first build+run failed rc={code}: {first}")
        package_root = pathlib.Path(first["layout_root"]).resolve()
        compiler = pathlib.Path(first["compiler"]).resolve()
        require(package_root in compiler.parents, "compiler must live inside the package")
        require(first["cache"] == "miss" and first["build"] == "ok", f"unexpected first result {first}")
        require(first["rom_sha256"] == digest, "ROM digest mismatch")
        require(first["run"] == "exited code=0", f"native program did not run cleanly: {first['run']}")
        entry = pathlib.Path(first["entry"])
        build_log = (entry / "build.log").read_text(encoding="utf-8")
        require(f"cc={compiler}" in build_log, "build.log must show the bundled compiler was configured")
        require(hermetic_compile_lines(build_log, compiler), "every compile command must invoke the bundled compiler")
        # SEG-058: the launcher's headless `--import` adds the ROM to the persistent library (a reference, never a copy), twice is a duplicate,
        # and the build already made is adopted for the same identity.
        imp = subprocess.run([str(launcher), "--import", str(rom)], env=env, text=True, capture_output=True, timeout=120)
        require(imp.returncode == 0 and "import=added" in imp.stdout and digest in imp.stdout, f"--import must add the ROM: {imp.stdout} {imp.stderr}")
        imp2 = subprocess.run([str(launcher), "--import", str(rom)], env=env, text=True, capture_output=True, timeout=120)
        require("import=duplicate" in imp2.stdout, f"importing the same content again must be a duplicate: {imp2.stdout}")
        catalog_file = next((tmp / "cache" / "library").glob("library.json"), None)
        require(catalog_file is not None and digest in catalog_file.read_text(encoding="utf-8"), "library.json must record the game")
        require(str(rom) in catalog_file.read_text(encoding="utf-8") and not list((tmp / "cache").rglob("synthetic.bin")), "the ROM is referenced by path and never copied into the cache")
        run_log = (entry / "run.log").read_text(encoding="utf-8")
        require("VIEWER_SUMMARY" in run_log and '"result":"stop"' in run_log and digest in run_log,
                "the launched program must reach the synthetic checkpoint (sanitized stop report)")
        require("rom_path" in (entry / "metadata.json").read_text() and str(rom) not in (entry / "metadata.json").read_text(),
                "the cache must not record or copy the ROM")
        require(not list(entry.rglob("synthetic.bin")), "the ROM must not be copied into the cache")

        # cache hit: no rebuild, entry untouched.
        stamp = (entry / "status.json").stat().st_mtime_ns
        code, second, _ = launch(rom, "second.txt")
        require(code == 0 and second["cache"] == "hit" and "build" not in second, f"expected a cache hit: {second}")
        require((entry / "status.json").stat().st_mtime_ns == stamp, "a cache hit must not rebuild")

        # stale/incomplete entry rebuilds safely.
        (entry / "status.json").unlink()
        code, third, _ = launch(rom, "third.txt", run=False)
        require(code == 0 and third["cache"] == "miss" and third["build"] == "ok", f"stale entry must rebuild: {third}")

        # 10: failure is reported (non-zero, human message) with preserved diagnostics.
        bad = tmp / "notarom.bin"
        bad.write_bytes(bytes(64))
        code, failed, _ = launch(bad, "failed.txt", run=False)
        require(code == 3 and failed.get("build") == "failed" and failed.get("message"), f"failure must be reported: {failed}")
        failed_log = pathlib.Path(failed["entry"] + ".failed") / "build.log"
        require(failed_log.is_file() and "FAILED" in failed_log.read_text(encoding="utf-8"), "diagnostics must be preserved")
        code, unreadable, _ = launch(tmp / "missing.bin", "missing.txt", run=False)
        require(code == 1 and "problem" in "".join(unreadable.values()) + str(unreadable), "unreadable ROM must fail cleanly")
        smoke_master_system(tmp, launch)
        smoke_optimized(tmp, launch)
        smoke_sms_optimized(tmp, launch)
        require(not (tmp / "decoy-used").exists(), "a host compiler on PATH was used")
    print("package smoke test: OK")


if __name__ == "__main__":
    main()
