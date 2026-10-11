#!/usr/bin/env python3
"""SEG-049-T004/T005 (ADR 0103/0104): CLI-level authority order, model lifecycle and machine-readable policy report.

Project-authored synthetic Genesis images only. Usage: <test.py> <segarecomp> <c-compiler> <source-root>.

1. `emit-general-startup-bridge-c --immutable-rom-aot-ml-gen2-admission`: one sanitized report line, deterministic, mutually exclusive with the
   exact-map option and the v1 option, requires --immutable-rom-aot; a fallback emission is byte-identical to broad.
2. `segarecomp build --aot-policy optimized --aot-model gen2`: status.json `aot_policy` carries requested/effective/producer/model identity/fractions/validator/
   fallback class/identity; gen2 is never selected implicitly (`optimized` alone is still v1); gen2 outside `optimized` is a usage error; an exact map outranks it.
3. Identities: Compatibility, v1 and gen2 never alias; the v1 and Compatibility identities equal their frozen SEG-047 text; identity is deterministic across runs
   and the generated program of a fallback is identical to the Compatibility one; `aot-policy-identity` is stable and distinguishes the requests.
"""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from genesis_ml_admission_cli_test import rom_image  # noqa: E402

KNOWN_REASONS = {"none", "model_identity", "rom_size", "token_range", "non_finite_score", "empty_proposal", "prune_rejected", "validator_rejected", "no_analysis",
                 "plan_alias_set_mismatch", "exact_plan_precedence", "platform_not_applicable", "no_report"}
KNOWN_CLASSES = {"none", "producer_incompatible", "producer_input", "proposal_rejected", "validator_rejected", "not_applicable", "exact_map"}
IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def build(cli, compiler, root, rom, out, extra=()):
    return run([cli, "build", "--rom", str(rom), "--output", str(out), "--cc", compiler, "--runtime-dir", str(root / "platforms" / "genesis"),
                "--optimize", "0", "--runtime-optimize", "1", *extra])


def status(out):
    return json.loads((out / "status.json").read_text())["aot_policy"]


def h(text):
    return hashlib.sha256(text.encode()).hexdigest()


def main():
    cli, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
    with tempfile.TemporaryDirectory(prefix="segarecomp-aot-model-") as directory:
        tmp = pathlib.Path(directory)
        data = rom_image()
        rom = tmp / "rom.bin"
        rom.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        emit = [cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"]

        # ---- 1. emit route ----
        plain = run(emit)
        first = run(emit + ["--immutable-rom-aot-ml-gen2-admission"])
        second = run(emit + ["--immutable-rom-aot-ml-gen2-admission"])
        require(plain.returncode == 0 and first.returncode == 0, "emission must succeed: " + first.stderr[-300:])
        require(first.stdout == second.stdout and first.stderr == second.stderr, "gen2 admission is deterministic")
        line = next((l for l in first.stderr.splitlines() if l.startswith("segarecomp: m68k admission: requested=optimized")), None)
        require(line is not None, first.stderr)
        fields = dict(token.split("=", 1) for token in line.split()[3:])
        require(fields["model_family"] == "gen2" and fields["fallback_class"] in KNOWN_CLASSES and fields["reason"] in KNOWN_REASONS, line)
        if fields["producer"] == "ml_region_gen2":
            require(fields["fallback"] == "0" and fields["validator"] == "accepted" and fields["rank_policy"] == "B35", line)
            require(fields["model"] == "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1", line)
            require(re.fullmatch(r"[0-9a-f]{64}", fields["token_contract"]) and re.fullmatch(r"[0-9a-f]{64}", fields["weights"]), line)
            require(int(fields["cells"]) == (len(data) + 127) // 128 and int(fields["ml_selected"]) == -(-int(fields["cells"]) * 35 // 100), line)
            require(int(fields["admitted"]) <= int(fields["broad"]) and fields["contract"] == "segarecomp.admission_contract.v1", line)
        else:
            require(fields["producer"] == "broad" and fields["fallback"] == "1" and fields["validator"] in ("not_run", "rejected"), line)
            require(first.stdout == plain.stdout, "a fallback emission must be byte-identical to broad")
        require("address" not in line and "0x" not in line, "sanitized")
        v1 = run(emit + ["--immutable-rom-aot-ml-admission"])
        require("model_family=v1" in v1.stderr, "v1 line carries its family: " + v1.stderr[-300:])
        both = run(emit + ["--immutable-rom-aot-ml-admission", "--immutable-rom-aot-ml-gen2-admission"])
        require(both.returncode == 2 and "mutually exclusive" in both.stderr, both.stderr)
        planfile = tmp / "x.plan"
        planfile.write_text(f"segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 {sha}\nuniverse_sha256 {'0' * 64}\nstrategy broad\nend\n")
        excl = run(emit + ["--immutable-rom-aot-admission", str(planfile), "--immutable-rom-aot-ml-gen2-admission"])
        require(excl.returncode == 2 and "excludes" in excl.stderr, "exact map and gen2 are mutually exclusive: " + excl.stderr)
        require(run(emit[:-1] + ["--immutable-rom-aot-ml-gen2-admission"]).returncode == 2, "gen2 admission requires --immutable-rom-aot")

        # ---- policy identity command ----
        def ident(*args):
            out = run([cli, "aot-policy-identity", *args])
            return out.returncode, out.stdout.strip()
        rc_c, compat_id = ident("--aot-policy", "compatibility")
        rc_1, v1_id = ident("--aot-policy", "optimized")
        rc_2, g2_id = ident("--aot-policy", "optimized", "--aot-model", "gen2")
        require(rc_c == rc_1 == rc_2 == 0, "identity command")
        pid = lambda s: re.search(r"policy_identity=([0-9a-f]{64})", s).group(1)
        require(len({pid(compat_id), pid(v1_id), pid(g2_id)}) == 3, "policy identities of Compatibility, v1 and gen2 differ")
        require(ident("--aot-policy", "optimized", "--aot-model", "gen2") == (0, g2_id), "stable")
        require(pid(ident("--aot-policy", "compatibility", "--aot-model", "gen2")[1]) == pid(compat_id), "an unused model never changes the Compatibility identity")
        require("requested_model=gen2" in g2_id and "requested_model=v1" in v1_id and "requested_model=none" in compat_id, "identity fields")
        require(ident("--aot-policy", "turbo")[0] == 2 and ident()[0] == 2 and ident("--aot-policy", "optimized", "--aot-model", "v3")[0] == 2, "invalid arguments")

        # ---- 2. build route ----
        compat_out, v1_out, g2_out = tmp / "compat", tmp / "v1", tmp / "g2"
        require(build(cli, compiler, root, rom, compat_out).returncode == 0, "compat build")
        r1 = build(cli, compiler, root, rom, v1_out, ("--aot-policy", "optimized"))
        r2 = build(cli, compiler, root, rom, g2_out, ("--aot-policy", "optimized", "--aot-model", "gen2"))
        require(r1.returncode == 0 and r2.returncode == 0, "builds: " + r1.stderr[-200:] + r2.stderr[-200:])
        sc, s1, s2 = status(compat_out), status(v1_out), status(g2_out)
        require(sc["requested_model"] == "none" and sc["model_family"] == "none" and sc["fallback_class"] == "none" and sc["validator"] == "not_run", str(sc))
        require(s1["requested_model"] == "v1" and s1["effective"] in ("ml_region", "broad"), "optimized alone is still v1: " + str(s1))
        require(s2["requested_model"] == "gen2" and s2["effective"] in ("ml_region_gen2", "broad") and s2["analysis_contract"] == "segarecomp.admission_contract.v1", str(s2))
        require(s2["fallback_class"] in KNOWN_CLASSES and s2["reason"] in KNOWN_REASONS and s2["fallback"] == (s2["effective"] == "broad"), str(s2))
        require(len({sc["identity"], s1["identity"], s2["identity"]}) == 3, "identities never alias")
        require(s2["policy_identity"] == pid(g2_id) and s1["policy_identity"] == pid(v1_id) and sc["policy_identity"] == pid(compat_id),
                "status carries the product policy identity")
        require(s2["cache_reuse"] == "none", "the CLI never reuses an output")
        # Frozen SEG-047 text for Compatibility / v1 identities.
        require(sc["identity"] == h(f"segarecomp.aot_policy.v1|rom={sha}|requested=compatibility|effective=broad|model=-|schema=-|k=-"), "Compatibility identity frozen")
        if s1["effective"] == "ml_region":
            require(s1["identity"] == h(f"segarecomp.aot_policy.v1|rom={sha}|requested=optimized|effective=ml_region|model=seg046-features-v1|schema={s1['schema_sha256']}|k={s1['k_sha256']}"),
                    "v1 identity frozen")
        else:
            require(s1["identity"] == h(f"segarecomp.aot_policy.v1|rom={sha}|requested=optimized|effective=broad|model=-|schema=-|k=-"), "v1 fallback identity frozen")
        if s2["effective"] == "ml_region_gen2":
            require(s2["model"] == "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1" and s2["rank_policy"] == "B35" and s2["validator"] == "accepted" and
                    len(s2["token_contract_sha256"]) == 64 and len(s2["rank_plan_sha256"]) == 64 and len(s2["weight_table_sha256"]) == 64 and len(s2["schema_sha256"]) == 64, str(s2))
            require(0 < s2["selected_ppm"] <= 1000000 and 0 < s2["admitted_ppm"] <= 1000000 and s2["k"] <= s2["universe"] and s2["cells"] == (len(data) + 127) // 128, str(s2))
        else:
            require(s2["validator"] in ("not_run", "rejected"), str(s2))
            gen_c = next(p for p in compat_out.iterdir() if p.suffix in ("", ".exe") and p.stem == "game")
            require(gen_c.exists(), "program exists")
        deterministic = tmp / "g2b"
        require(build(cli, compiler, root, rom, deterministic, ("--aot-policy", "optimized", "--aot-model", "gen2")).returncode == 0, "rebuild")
        require(status(deterministic)["identity"] == s2["identity"], "identity is deterministic across builds")
        # usage errors
        bad = run([cli, "build", "--rom", str(rom), "--output", str(tmp / "bad1"), "--cc", compiler, "--runtime-dir", str(root / "platforms" / "genesis"), "--aot-model", "gen2"])
        require(bad.returncode == 2, "gen2 without optimized is a usage error")
        bad = run([cli, "build", "--rom", str(rom), "--output", str(tmp / "bad2"), "--cc", compiler, "--runtime-dir", str(root / "platforms" / "genesis"),
                   "--aot-policy", "optimized", "--aot-model", "v9"])
        require(bad.returncode == 2, "unknown model is a usage error")

        # Exact-map precedence over gen2: no ML producer runs and the report says so.
        tiny = tmp / "tiny.bin"
        tiny.write_bytes(IMAGE)
        tsha = hashlib.sha256(IMAGE).hexdigest()
        regions = tmp / "x.regions"
        regions.write_text(f"segarecomp.m68k_executable_regions.v1\nrom_sha256 {tsha}\nrange 00000000 0000000c\nend\n")
        plan = tmp / "t.plan"
        planned = run([cli, "emit-general-startup-bridge-c", "--rom", str(tiny), "--reset-entry", "--rom-sha256", tsha, "--immutable-rom-aot",
                       "--immutable-aot-region-proposal", str(regions), "--region-admission-plan-output", str(plan)])
        require(planned.returncode == 0, planned.stderr)
        won = build(cli, compiler, root, tiny, tmp / "prec", ("--aot-policy", "optimized", "--aot-model", "gen2", "--admission-plan", str(plan)))
        require(won.returncode == 0, won.stdout + won.stderr)
        sp = status(tmp / "prec")
        require(sp["effective"] == "admission_plan" and sp["reason"] == "exact_plan_precedence" and sp["fallback"] is False and sp["fallback_class"] == "exact_map" and
                sp["model_family"] == "none" and sp["requested_model"] == "gen2", "an exact map outranks the gen2 producer: " + str(sp))
        require("m68k admission: requested=optimized" not in (tmp / "prec" / "build.log").read_text(), "no ML producer ran under an exact map")
    print("segarecomp_aot_model_lifecycle_test: ok")


if __name__ == "__main__":
    main()
