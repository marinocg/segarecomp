#!/usr/bin/env python3
"""SEG-009-T010: `segarecomp build` for Master System images (hermetic, project-authored fixtures only).

usage: sms_native_build_test.py <segarecomp-cli> <cc> <product-root> <sms_image_emitter> [group ...]

Proves the consumer build route (ROM -> platform/profile -> mapper declaration -> SMS generation route -> runtime + PSG +
headless driver -> strict C11 -> executable) reproduces the T008 artifact digests of the `machine_e2e` fixture, that it emits
exactly what the one SMS emitter (tests/tools/sms_image_emitter) emits, and that every ambiguous or unsupported input fails
closed, before any generated C exists, with the typed diagnostic in status.json and the existing exit-code conventions.

CI wall-clock: the cases are independent groups, each with its own builds, so CTest can run them as separate entries in parallel
(tests/CMakeLists.txt registers one per group; the union is the whole matrix):
  build       the success route (T008 digests, provenance, emitter equality) and the manifest declaration form
  policy      the production compile policy (-O2 default, bounded jobs, explicit --optimize / --jobs, rejected --optimize)
  failclosed  every ambiguous or unsupported input fails closed before generated C exists (no compile except the missing-runtime case)
Without a group argument every group runs.
"""
import hashlib
import os
import json
import pathlib
import subprocess
import sys
import tempfile

CLI, CC, ROOT, EMITTER = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
sys.path.insert(0, str(ROOT / "tests"))
import sms_e2e_common as c  # noqa: E402
from sms_e2e_common import builder  # noqa: E402

ALL_GROUPS = ("build", "policy", "failclosed")
GROUPS = tuple(sys.argv[5:]) or ALL_GROUPS
assert all(g in ALL_GROUPS for g in GROUPS), GROUPS
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def build(rom, out, *extra, platform_dir=None):
    return subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--optimize", "0",
                           "--runtime-dir", str(platform_dir or ROOT / "platforms" / "master-system"), *extra],
                          text=True, capture_output=True, timeout=900)


def build_default_policy(rom, out, *extra):
    """The consumer defaults: no --optimize (the target's policy applies); the fixture's -Wmisleading-indentation relaxation only."""
    return subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--mapper", "sega",
                           "--runtime-dir", str(ROOT / "platforms" / "master-system"), "--cc-arg", "-Wno-misleading-indentation", *extra],
                          text=True, capture_output=True, timeout=900)


def compile_levels(out):
    """{source file name: last -O level} of every compile command recorded in build.log (the last -O flag wins)."""
    levels = {}
    for line in (out / "build.log").read_text().splitlines():
        if not line.startswith("$ ") or " -c " not in line:
            continue
        words = line.split()
        flags = [w for w in words if w in ("-O0", "-O1", "-O2")]
        levels[pathlib.Path(words[-1]).name] = flags[-1] if flags else None
    return levels


def status(out):
    return json.loads((out / "status.json").read_text())


def tree(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.iterdir())
            if p.suffix in (".c", ".h", ".json", ".units")}


def expect_failure(tmp, name, rom, *extra, stage="analyze", code=1, diagnostic=None, words=()):
    out = tmp / name
    r = build(rom, out, *extra)
    st = status(out) if (out / "status.json").exists() else {}
    check(r.returncode == code, "%s: exit %d, expected %d (%s)" % (name, r.returncode, code, r.stdout[-200:]))
    check("@result failed stage=%s" % stage in r.stdout and st.get("status") == "failed" and st.get("stage") == stage,
          "%s: failure not attributed to stage %s: %s" % (name, stage, st))
    if diagnostic:
        check(st.get("diagnostic") == diagnostic, "%s: diagnostic %s, expected %s" % (name, st.get("diagnostic"), diagnostic))
    check(all(w in st.get("message", "") for w in words), "%s: message lacks %s: %s" % (name, words, st.get("message")))
    check(not (out / "generated").exists() and not (out / "game").exists() and not (out / "game.exe").exists(), "%s: generated C or an executable exists" % name)
    check((out / "build.log").is_file() and "FAILED" in (out / "build.log").read_text(), "%s: build.log lacks the failure" % name)


def main():
    manifest = json.loads(c.MANIFEST.read_text())["native"]
    with tempfile.TemporaryDirectory(prefix="sms-native-build-") as directory:
        tmp = pathlib.Path(directory)
        builder.main(["--out", str(tmp / "roms"), "--fixture", c.FIXTURE])
        rom = tmp / "roms" / (c.FIXTURE + ".sms")
        manifest_file = tmp / "roms" / (c.FIXTURE + ".mapper.json")
        data = rom.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        (tmp / "input.txt").write_text(builder.E2E_SCRIPT, encoding="utf-8")

        if "build" in GROUPS:
            # ---- success: header selects the platform/profile, the build option declares the mapper --------------------
            out = tmp / "ok"
            r = build(rom, out, "--mapper", "sega")
            check(r.returncode == 0, "build failed: %s" % (r.stdout + r.stderr)[-600:])
            lines = r.stdout.splitlines()
            check(all(("@stage %s begin" % s in lines and "@stage %s done" % s in lines) for s in ("analyze", "generate", "compile", "link")),
                  "stage lines missing")
            check(lines and lines[-1].startswith("@result ok executable="), "no @result ok line")
            st = status(out)
            check(st.get("status") == "ok" and st.get("rom_sha256") == digest and st.get("platform") == "master-system" and
                  st.get("profile") == "sms2_ntsc_export" and st.get("mapper") == "sega" and st.get("mapper_source") == "build_option"
                  and st.get("toolchain") == CC, "status.json provenance: %s" % st)
            log = (out / "build.log").read_text()
            check("platform=master-system" in log and "mapper=sega" in log and "mapper_declaration_source=build_option" in log and
                  "profile=sms2_ntsc_export" in log and "rom_sha256=" + digest in log, "build.log provenance")
            check(not (out / "obj").exists(), "intermediate objects must be removed")
            exe = out / ("game.exe" if sys.platform == "win32" else "game")
            check(exe.is_file(), "no executable")

            native = c.Native(EMITTER, CC, ROOT, tmp)
            res = c.run_native(native, exe, "built")
            check(res["code"] == 0, "run exited %s: %s" % (res["code"], res["stdout"][:200]))
            if res["code"] == 0:
                d = c.artifact_digests(res)
                for key in ("artifact_sha256", "state_digest", "cycles", "frames_completed", "irq_trace_entries", "mapper_trace_entries",
                            "vdp_trace_entries", "frame_record_hashes_sha256", "pcm_samples", "pcm_sha256"):
                    check(d[key] == manifest[key], "artifact %s differs from the T008 validation manifest" % key)

            # the build emits exactly what the one SMS emitter emits (no second emitter)
            ref = tmp / "ref"
            e = subprocess.run([EMITTER, str(rom), str(ref), "sms", "--mapper", "sega"], capture_output=True, text=True, timeout=300)
            check(e.returncode == 0, "reference emission failed")
            check(tree(out / "generated") == tree(ref) and tree(ref), "build output differs from sms_image_emitter output")

            # the manifest declaration form (same image, declared through the manifest)
            out2 = tmp / "manifest"
            # the unrelaxed -Wall build above is the generated-code strict-warning evidence; this repeat of the same image drops
            # GCC's expensive -Wmisleading-indentation (see sms_cc_cache.GENERATED_UNIT_FLAGS) without changing the emitted C
            r = build(rom, out2, "--mapper-manifest", str(manifest_file), "--platform", "master-system", "--cc-arg", "-Wno-misleading-indentation")
            check(r.returncode == 0, "manifest build failed: %s" % (r.stdout + r.stderr)[-400:])
            if r.returncode == 0:
                check(status(out2).get("mapper_source") == "fixture_builder", "manifest source not recorded: %s" % status(out2))

        if "policy" in GROUPS:
            # ---- SEG-033-T004 production compile policy: one optimization level for every unit (-O2 by default), bounded jobs ----
            out3 = tmp / "policy"
            r = build_default_policy(rom, out3)
            check(r.returncode == 0, "default-policy build failed: %s" % (r.stdout + r.stderr)[-400:])
            if r.returncode == 0:
                levels = compile_levels(out3)
                check(levels and set(levels.values()) == {"-O2"}, "the default build must compile every unit at -O2: %s" % levels)
                check("# compile jobs: %d\n" % min(8, os.cpu_count() or 1) in (out3 / "build.log").read_text() + "\n",
                      "the default worker count is not min(8, cpus)")
            out4 = tmp / "policy_jobs"
            r = build_default_policy(rom, out4, "--jobs", "3", "--optimize", "1")
            check(r.returncode == 0, "explicit-policy build failed")
            if r.returncode == 0:
                check("# compile jobs: 3" in (out4 / "build.log").read_text(), "--jobs override not honored")
                check(set(compile_levels(out4).values()) == {"-O1"}, "an explicit --optimize applies to every unit")
            bad = subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(tmp / "badopt"), "--cc", CC, "--optimize", "3",
                                  "--runtime-dir", str(ROOT / "platforms" / "master-system")], text=True, capture_output=True)
            check(bad.returncode != 0 and not (tmp / "badopt" / "game").exists(), "--optimize 3 must be rejected")

        if "failclosed" in GROUPS:
            # ---- fail closed ---------------------------------------------------------------------------------------------
            # header-valid image, no mapper declaration: typed failure before any emission
            expect_failure(tmp, "undeclared", rom, diagnostic="SMS_ERROR_MAPPER_UNDECLARED", words=("must be declared",))
            expect_failure(tmp, "undeclared_explicit", rom, "--platform", "master-system", diagnostic="SMS_ERROR_MAPPER_UNDECLARED")
            expect_failure(tmp, "unknown_mapper", rom, "--mapper", "banana", diagnostic="SMS_ERROR_MAPPER_UNDECLARED")
            expect_failure(tmp, "unsupported_mapper", rom, "--mapper", "codemasters", diagnostic="SMS_ERROR_MAPPER_UNSUPPORTED")
            expect_failure(tmp, "conflict", rom, "--mapper", "rom_only", "--mapper-manifest", str(manifest_file),
                           diagnostic="SMS_ERROR_MAPPER_UNDECLARED")
            expect_failure(tmp, "rom_only_size", rom, "--mapper", "rom_only", diagnostic="SMS_ERROR_ROM_SIZE_UNSUPPORTED")
            wrong = tmp / "wrong.mapper.json"
            wrong.write_text(json.dumps({"mapper": "sega", "sha256": "0" * 64}), encoding="utf-8")
            expect_failure(tmp, "manifest_hash", rom, "--mapper-manifest", str(wrong), diagnostic="SMS_ERROR_MAPPER_UNDECLARED")
            expect_failure(tmp, "manifest_missing", rom, "--mapper-manifest", str(tmp / "absent.json"), diagnostic="SMS_ERROR_MAPPER_UNDECLARED")

            # header ambiguity/other consoles: never guessed
            for name, region, platform_flag, diagnostic in (("gg", 0x6, None, "PLATFORM_UNSUPPORTED"),
                                                             ("gg_explicit", 0x6, "master-system", "SMS_ERROR_PROFILE_UNSUPPORTED"),
                                                             ("japan", 0x3, None, "SMS_ERROR_PROFILE_UNSUPPORTED")):
                image = bytearray(data)
                image[0x7FFF] = (region << 4) | (image[0x7FFF] & 0x0F)
                path = tmp / (name + ".sms")
                path.write_bytes(bytes(image))
                extra = ["--mapper", "sega"] + (["--platform", platform_flag] if platform_flag else [])
                expect_failure(tmp, name, path, *extra, diagnostic=diagnostic)
            headerless = bytearray(data)
            headerless[0x7FF0:0x7FF8] = bytes(8)
            path = tmp / "headerless.sms"
            path.write_bytes(bytes(headerless))
            r = build(path, tmp / "headerless_implicit", "--mapper", "sega")  # no header, no --platform: never selected as SMS
            check(r.returncode != 0 and "platform=master-system" not in (tmp / "headerless_implicit" / "build.log").read_text(),
                  "a headerless image must not select the Master System route implicitly")
            short = tmp / "short.sms"
            short.write_bytes(bytes(headerless)[:40000])
            expect_failure(tmp, "headerless_bad_size", short, "--platform", "master-system", "--mapper", "sega",
                           diagnostic="SMS_ERROR_ROM_SIZE_UNSUPPORTED")

            # a mapper declaration never applies to another platform; bad usage
            genesis = tmp / "genesis.bin"
            genesis.write_bytes(bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70)))
            expect_failure(tmp, "genesis_mapper", genesis, "--mapper", "sega", diagnostic="MAPPER_NOT_APPLICABLE")
            bad_usage = subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(tmp / "u"), "--cc", CC, "--runtime-dir", str(tmp),
                                        "--platform", "saturn"], capture_output=True, text=True)
            check(bad_usage.returncode == 2, "unknown --platform must be a usage error")
            # missing runtime sources: compile-stage failure with the existing exit code
            r = build(rom, tmp / "norun", "--mapper", "sega", platform_dir=tmp / "platforms" / "master-system")
            check(r.returncode == 2 and "@result failed stage=compile" in r.stdout, "missing runtime files must fail at compile with exit 2")
    print("sms_native_build_test:", "FAILED" if FAILED else "all passed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
