#!/usr/bin/env python3
"""SEG-057-T002 (ADR 0119): the experimental Master System Optimized policy through the real product CLI and real generated programs.

usage: sms_optimized_policy_test.py <segarecomp-cli> <cc> <product-root> <sms_admission_emit> [cli|runtime|viewer <sdl3-include> <sdl3-lib>]

  cli      `aot-policy-identity` per platform, the machine-readable `aot_policy` status of Compatibility and Optimized builds (requested /
           effective / fallback / reason / model and plan identities / selected fraction), determinism, cache identities that never collide,
           the Genesis model names refused for Master System, and that the default build is the Compatibility build byte for byte.
  runtime  a real generated program built from an explicit plan: an adequate plan reproduces the broad run's machine-state digest; an
           inadequate plan stops with the typed Z80_ERROR_UNADMITTED_PC, printed as `z80_outcome unadmitted_pc` and exiting with the
           dedicated status 7 (not 3), before the omitted instruction has any effect.

  viewer   (SDL3 job only) the same typed stop through the real SDL3 VIEWER program under SDL's dummy video/audio drivers: exit status 7 for an
           omitted start and 0 for an adequate plan, so the program the launcher actually starts speaks the same contract.

Hermetic: the project-authored `machine_e2e` fixture only; no commercial ROM, no model file, no PyTorch.
"""
import concurrent.futures
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

CLI, CC, ROOT, PLAN_EMIT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
GROUPS = tuple(a for a in sys.argv[5:5 + 1]) or ("cli", "runtime")
SDL_INCLUDE, SDL_LIB = (sys.argv[6], sys.argv[7]) if len(sys.argv) > 7 else (None, None)
sys.path.insert(0, str(ROOT / "tests"))
import sms_e2e_common as c  # noqa: E402
from sms_e2e_common import builder  # noqa: E402

FAILED = []
EXIT_UNADMITTED_PC = 7


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def build(rom, out, *extra):
    return subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--optimize", "0", "--mapper", "sega",
                           "--cc-arg", "-Wno-misleading-indentation", "--runtime-dir", str(ROOT / "platforms" / "master-system"), *extra],
                          text=True, capture_output=True, timeout=1800)


def status(out):
    return json.loads((out / "status.json").read_text())


def tree(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.iterdir()) if p.suffix in (".c", ".h", ".json", ".units")}


def identity(*args):
    r = subprocess.run([CLI, "aot-policy-identity", *args], text=True, capture_output=True, timeout=60)
    m = re.search(r"policy_identity=([0-9a-f]{64})", r.stdout)
    return r.returncode, (m.group(1) if m else None), r.stdout


def run_program(exe, frames, artifacts):
    shutil.rmtree(artifacts, ignore_errors=True)
    artifacts.mkdir(parents=True)
    r = subprocess.run([str(exe), "--frames", str(frames), "--artifacts", str(artifacts)], text=True, capture_output=True, timeout=300)
    digest = (artifacts / "state.sha256").read_text().strip() if (artifacts / "state.sha256").exists() else ""
    return r.returncode, r.stdout, digest


def cli_group(tmp, rom):
    # ---- identities: per platform, per policy ----
    rc_c, compat, _ = identity("--platform", "master-system", "--aot-policy", "compatibility")
    rc_o, opt, text = identity("--platform", "master-system", "--aot-policy", "optimized")
    check(rc_c == 0 and rc_o == 0 and compat and opt and compat != opt, "SMS Compatibility and Optimized identities exist and differ")
    check("requested_model=d512hp" in text and "platform=master-system" in text, "SMS identity line names its model and platform: " + text)
    _, g_opt, _ = identity("--aot-policy", "optimized")
    _, g_opt_explicit, _ = identity("--platform", "genesis", "--aot-policy", "optimized")
    check(g_opt and g_opt == g_opt_explicit and g_opt != opt, "the Genesis identity is untouched and differs from the SMS one")
    check(identity("--platform", "master-system", "--aot-policy", "optimized", "--aot-model", "gen2")[0] == 2, "a Genesis model name is refused for the SMS identity")
    check(identity("--platform", "master-system", "--aot-policy", "optimized", "--aot-model", "v1")[0] == 2, "even 'v1' is not a Z80 model name")
    check(identity("--platform", "nes", "--aot-policy", "optimized")[0] == 2, "an unknown platform is a usage error")

    # ---- builds ----
    out_default, out_compat, out_opt, out_opt2 = (tmp / n for n in ("default", "compat", "opt", "opt2"))
    runs = {n: build(rom, d, *x) for n, d, x in (("default", out_default, ()), ("compat", out_compat, ("--aot-policy", "compatibility")),
                                                   ("opt", out_opt, ("--aot-policy", "optimized")), ("opt2", out_opt2, ("--aot-policy", "optimized")))}
    for name, r in runs.items():
        check(r.returncode == 0, "%s build failed: %s" % (name, (r.stdout + r.stderr)[-500:]))
    if any(r.returncode != 0 for r in runs.values()):
        return
    sd, sc, so, so2 = (status(d) for d in (out_default, out_compat, out_opt, out_opt2))
    check(tree(out_default / "generated") == tree(out_compat / "generated"), "the default build is the Compatibility build byte for byte")
    # The launcher reads the aot_policy object WITHOUT a JSON parser (it ends the object at the first '}'): no string value may contain a brace, and a
    # flat read must still find the identities that decide cache reuse (a regression of exactly this broke package cache reuse).
    for name, d in (("compat", out_compat), ("opt", out_opt)):
        raw = (d / "status.json").read_text()
        start = raw.index('"aot_policy":{')
        flat = raw[start:raw.index("}", start) + 1]
        parsed = json.loads("{" + flat + "}")["aot_policy"]
        check(parsed == json.loads(raw)["aot_policy"], "%s: the flat object (to the first brace) must be the whole aot_policy object" % name)
        check('"policy_identity":"' in flat and '"identity":"' in flat, "%s: a flat read must reach the identities" % name)
    pd, pc, po = sd["aot_policy"], sc["aot_policy"], so["aot_policy"]
    check(pd["requested"] == "compatibility" and pd["effective"] == "broad" and pd["fallback"] is False and pd["reason"] == "none", "default request: %s" % pd)
    check(pc["policy_identity"] == compat and po["policy_identity"] == opt, "the status carries the CLI's policy identities")
    check(po["requested"] == "optimized" and po["requested_model"] == "d512hp", "Optimized request recorded: %s" % po)
    check(pc["identity"] != po["identity"] and pc["policy_identity"] != po["policy_identity"], "Compatibility and Optimized builds never share an identity")
    if po["effective"] == "ml_region_z80":
        check(po["fallback"] is False and po["reason"] == "none" and po["fallback_class"] == "none" and po["model_family"] == "sms_z80_d512hp" and po["validator"] == "accepted",
              "an applied Optimized build reports no fallback: %s" % po)
        check(po["model"] == "6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11" and
              po["artifact_sha256"] == "81434de0da4755654e14c370fc32de7898532193cbc5a514033268ba6669c2ac" and po["architecture"] == "D512HP/3seeds", "model identity: %s" % po)
        check(re.fullmatch(r"[0-9a-f]{64}", po["plan_sha256"]) is not None and po["cells"] == rom.stat().st_size // 32, "plan identity and cell count: %s" % po)
        check(640000 <= po["selected_ppm"] <= 700000 and 0 < po["admitted_ppm"] < 1000000 and po["admitted_offsets"] < po["image_offsets"],
              "about 65%% of the cells are selected and the admitted set is a strict subset: %s" % po)
        check(tree(out_opt / "generated") == tree(out_opt2 / "generated") and po["plan_sha256"] == so2["aot_policy"]["plan_sha256"],
              "Optimized builds are deterministic (generated tree and plan identity)")
        check(tree(out_opt / "generated") != tree(out_compat / "generated"), "the Optimized program differs from the Compatibility program")
        exe = out_opt / "game"
        rc, stdout, _digest = run_program(exe, 12, tmp / "run_opt")
        check(rc in (0, EXIT_UNADMITTED_PC), "the Optimized program ends cleanly or with the typed status, not %d: %s" % (rc, stdout[-200:]))
        if rc == EXIT_UNADMITTED_PC:
            check("z80_outcome unadmitted_pc" in stdout, "the typed stop is printed as a structured line: %s" % stdout[-300:])
    else:
        check(po["fallback"] is True and po["effective"] == "broad" and po["reason"] != "none" and po["fallback_class"] != "none", "a fallback is explicit: %s" % po)
        check(tree(out_opt / "generated") == tree(out_compat / "generated"), "a fallback program is the Compatibility program")

    # ---- Genesis model names are not Z80 model names ----
    out_bad = tmp / "bad"
    r = build(rom, out_bad, "--aot-policy", "optimized", "--aot-model", "gen2")
    st = status(out_bad) if (out_bad / "status.json").exists() else {}
    check(r.returncode == 1 and st.get("diagnostic") == "MODEL_NOT_APPLICABLE" and not (out_bad / "generated").exists() and not (out_bad / "game").exists(),
          "--aot-model gen2 is refused for Master System before any generated C: rc=%d %s" % (r.returncode, st))
    check(build(rom, tmp / "bad2", "--aot-model", "gen2").returncode != 0, "--aot-model without --aot-policy optimized is still a usage error")


def compile_program(emit_dir, work, viewer=False):
    units = [emit_dir / n for n in (emit_dir / "sms.units").read_text().split()]
    rt, hl, psg = ROOT / "platforms/master-system/runtime", ROOT / "platforms/master-system/headless", ROOT / "libs/device/sega/psg"
    vw = ROOT / "platforms/master-system/viewer"
    sources = units + [rt / n for n in ("sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c", "sms_vdp.c", "sms_render.c")] + \
        [psg / "src/sn76489.c"] + ([vw / n for n in ("sms_viewer.c", "sms_viewer_sdl3.c", "sms_viewer_main.c")] if viewer else
                                   [hl / n for n in ("sms_audio.c", "sms_headless.c", "sms_devices_vdp.c")])
    base = [CC, "-std=c11", "-O0", "-w", "-I", str(ROOT / "libs/codegen/c11/include"), "-I", str(rt), "-I", str(psg / "include"), "-I", str(emit_dir),
            *(["-I", str(vw), "-I", SDL_INCLUDE] if viewer else ["-I", str(hl)])]
    objects = [work / ("%d.o" % i) for i in range(len(sources))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        codes = list(pool.map(lambda p: subprocess.run(base + ["-c", "-o", str(p[1]), str(p[0])], capture_output=True).returncode, zip(sources, objects)))
    check(all(code == 0 for code in codes), "the plan-built program must compile")
    exe = work / "program"
    link = subprocess.run([CC, *map(str, objects), "-o", str(exe), *(["-L" + SDL_LIB, "-lSDL3", "-Wl,-rpath," + SDL_LIB] if viewer else [])], capture_output=True)
    check(link.returncode == 0, "the plan-built program must link")
    return exe


def runtime_group(tmp, rom):
    broad_out = tmp / "broad"
    r = build(rom, broad_out)
    check(r.returncode == 0, "broad build failed: " + (r.stdout + r.stderr)[-400:])
    if r.returncode != 0:
        return
    rc_b, out_b, digest_b = run_program(broad_out / "game", 12, tmp / "run_broad")
    check(rc_b == 0 and digest_b, "the broad program runs: %s" % out_b[-200:])

    def plan_build(name, lines):
        plan = tmp / (name + ".plan")
        plan.write_text("\n".join(lines) + "\n")
        directory = tmp / name
        directory.mkdir()
        r = subprocess.run([PLAN_EMIT, str(rom), str(directory), "sms", "--plan", str(plan)], text=True, capture_output=True)
        check(r.returncode == 0, "%s emission failed: %s" % (name, r.stdout + r.stderr))
        return compile_program(directory, directory) if r.returncode == 0 else None

    # image 1 = the fixed first 1 KiB, image 2 + n = bank n of the 128 KiB fixture; its program calls into several banks, so the adequate plan
    # admits the first 2 KiB of every bank (where the fixture keeps its code) and the inadequate one only the reset path.
    adequate = plan_build("adequate", ["1 0 1024"] + ["%d 0 2048" % (2 + bank) for bank in range(8)])
    if adequate:
        rc, out, digest = run_program(adequate, 12, tmp / "run_adequate")
        check(rc == 0 and digest == digest_b, "an adequate plan reproduces the broad run exactly (digest %s vs %s, rc %d): %s" % (digest[:12], digest_b[:12], rc, out[-160:]))
    inadequate = plan_build("inadequate", ["1 0 256"])  # the reset path only: the first jump leaves the plan
    if inadequate:
        rc, out, digest = run_program(inadequate, 12, tmp / "run_inadequate")
        check(rc == EXIT_UNADMITTED_PC, "an omitted start exits with the dedicated status %d, not %d: %s" % (EXIT_UNADMITTED_PC, rc, out[-200:]))
        check(re.search(r"^z80_outcome unadmitted_pc pc [0-9A-F]{4} identity \d+$", out, re.M) is not None, "the typed outcome is a structured line: %s" % out[-300:])
        check(digest != digest_b, "the stopped run is not mistaken for the complete run")
    # A different kind of stop keeps its own status (the broad program with a cycle budget too small to finish is status 2, never 7).
    r = subprocess.run([str(broad_out / "game"), "--cycle-budget", "1000"], text=True, capture_output=True, timeout=120)
    check(r.returncode == 2, "a cycle-budget stop keeps status 2 (got %d)" % r.returncode)


def viewer_group(tmp, rom):
    env = {**__import__("os").environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software"}

    def plan_viewer(name, lines):
        plan = tmp / (name + ".plan")
        plan.write_text("\n".join(lines) + "\n")
        directory = tmp / name
        directory.mkdir()
        r = subprocess.run([PLAN_EMIT, str(rom), str(directory), "sms", "--plan", str(plan)], text=True, capture_output=True)
        check(r.returncode == 0, "%s emission failed: %s" % (name, r.stdout + r.stderr))
        return compile_program(directory, directory, viewer=True) if r.returncode == 0 else None

    adequate = plan_viewer("v_adequate", ["1 0 1024"] + ["%d 0 2048" % (2 + bank) for bank in range(8)])
    if adequate:
        r = subprocess.run([str(adequate), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", "8"], text=True, capture_output=True, timeout=300, env=env)
        check(r.returncode == 0, "the viewer with an adequate plan ends cleanly (rc %d): %s" % (r.returncode, (r.stdout + r.stderr)[-300:]))
    inadequate = plan_viewer("v_inadequate", ["1 0 256"])
    if inadequate:
        r = subprocess.run([str(inadequate), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", "8"], text=True, capture_output=True, timeout=300, env=env)
        check(r.returncode == EXIT_UNADMITTED_PC, "the viewer exits with the dedicated status %d for an omitted start, not %d: %s" % (EXIT_UNADMITTED_PC, r.returncode, (r.stdout + r.stderr)[-300:]))
        check(re.search(r"^z80_outcome unadmitted_pc pc [0-9A-F]{4} identity \d+$", r.stdout, re.M) is not None, "the viewer prints the structured outcome line: %s" % r.stdout[-300:])


def main():
    with tempfile.TemporaryDirectory(prefix="sms-optimized-policy-") as directory:
        tmp = pathlib.Path(directory)
        builder.main(["--out", str(tmp / "roms"), "--fixture", c.FIXTURE])
        rom = tmp / "roms" / (c.FIXTURE + ".sms")
        if "cli" in GROUPS:
            cli_group(tmp, rom)
        if "runtime" in GROUPS:
            runtime_group(tmp, rom)
        if "viewer" in GROUPS:
            viewer_group(tmp, rom)
    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("sms optimized policy (%s): ok" % ",".join(GROUPS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
