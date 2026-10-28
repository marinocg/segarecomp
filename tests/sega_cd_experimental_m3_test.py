#!/usr/bin/env python3
"""SEG-096-T003 (ADR 0208): the experimental frozen M3 + B35 path for Sega CD, end to end with the REAL native model on project-authored fixtures.

Runs the pre-registered three-arm comparison (tools/sega_cd_m3_compare.py) and checks the invariants that make it an experiment and not an authority change:
  * selectable only by `--aot-policy optimized [--aot-model gen2]`; Compatibility (default) is byte-identical to no option; `--aot-model` alone, a model other than
    gen2, a repeated model and unknown policies are usage errors; `aot-policy-identity` states the platform's identity and leaves the Genesis one untouched;
  * the image/mapping/root/producer inventory is identical in arms A, B, C; C's compiled source-relative offsets are a subset of A's (it never adds);
  * the report is truthful: requested/effective/fallback/reason, the frozen model identities equal the ones the product prints, a digest of the admitted set;
  * deterministic: every arm emits byte-identical text twice; program identities of the arms differ (the request enters the identity);
  * arms A and B run every route identically; arm C either does the same or stops at the typed uncompiled entry - never anywhere else, never with more work;
  * the BIOS-image profile end to end through `sega-cd build`: the experimental build is smaller, completes, records `aot_policy` in status.json, and a stop on an
    omitted entry is a typed frontier;
  * independent labels (the encoder's own instruction starts) are REPORTED: the gate "complete recall" is not claimed.

usage: sega_cd_experimental_m3_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
checks = [0]
GENESIS_GEN2_IDENTITY = "policy_identity=3f494353182d1d3efda42f933701b50d338fed28df85548d79b0bbf2382ad7cf requested=optimized requested_model=gen2 contract=segarecomp.admission_contract.v1"


def check(ok, label):
    checks[0] += 1
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def cli(*args):
    out = subprocess.run([SEGARECOMP, *args], capture_output=True, text=True, timeout=300)
    return out.returncode, out.stdout, out.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="scd-m3-test-") as tmp:
        tmp = pathlib.Path(tmp)
        # ---- vocabulary and identities -------------------------------------------------------------------------------------------
        rc, out, _ = cli("aot-policy-identity", "--aot-policy", "optimized", "--aot-model", "gen2")
        check(rc == 0 and out.strip() == GENESIS_GEN2_IDENTITY, "the Genesis gen2 policy identity is unchanged")
        rc, scd_out, _ = cli("aot-policy-identity", "--platform", "sega-cd", "--aot-policy", "optimized", "--aot-model", "gen2")
        fields = dict(f.split("=", 1) for f in scd_out.split())
        check(rc == 0 and fields.get("platform") == "sega-cd" and fields.get("experimental") == "1" and fields["policy_identity"] != GENESIS_GEN2_IDENTITY.split()[0].split("=")[1],
              "the Sega CD experimental policy has its own identity, labelled experimental")
        rc, out, _ = cli("aot-policy-identity", "--platform", "sega-cd", "--aot-policy", "optimized", "--aot-model", "v1")
        check(rc == 2, "Sega CD accepts only the frozen gen2 model (v1 is a usage error)")
        rc, out, _ = cli("aot-policy-identity", "--platform", "sega-cd", "--aot-policy", "compatibility", "--aot-model", "gen2")
        check(rc == 2, "a model with Compatibility is a usage error")

        disc = tmp / "d" / "disc"
        common.build_disc(disc, "unvisited", "cooked", "--unvisited-case", "pi")
        subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(disc / "disc.cue"), "--out", str(tmp / "m.txt")], capture_output=True, check=True)
        base = [SEGARECOMP, "sega-cd", "emit", "--cue", str(disc / "disc.cue"), "--modules", str(tmp / "m.txt")]

        def emit(name, *flags):
            proc = subprocess.run([*base, "--out", str(tmp / name), "--report-offsets", *flags], capture_output=True, text=True, timeout=300)
            return proc.returncode, (json.loads(proc.stdout) if proc.stdout.strip().startswith("{") else {}), proc.stderr

        rc, plain, _ = emit("plain.c")
        rc2, explicit, _ = emit("explicit.c", "--aot-policy", "compatibility")
        check(rc == 0 and rc2 == 0 and (tmp / "plain.c").read_bytes() == (tmp / "explicit.c").read_bytes() and plain["program_identity"] == explicit["program_identity"],
              "Compatibility is the default: `--aot-policy compatibility` is byte-identical to no option, same program identity")
        check(plain["aot_policy"]["requested"] == "compatibility" and plain["aot_policy"]["effective"] == "broad" and not plain["aot_policy"]["fallback"] and
              not plain["aot_policy"]["experimental"], "the default report states Compatibility / broad / no fallback / not experimental")
        for bad in (["--aot-model", "gen2"], ["--aot-policy", "optimized", "--aot-model", "v1"], ["--aot-policy", "fast"], ["--aot-policy", "optimized", "--aot-model", "gen2", "--aot-model", "gen2"]):
            proc = subprocess.run([*base, "--out", str(tmp / "bad.c"), *bad], capture_output=True, text=True)
            check(proc.returncode == 2, "usage error: %s" % " ".join(bad))

        # ---- experimental emission vs the broad reference ---------------------------------------------------------------------------
        rc, selective, _ = emit("sel.c", "--aot-policy", "optimized", "--aot-model", "gen2")
        rc2, selective2, _ = emit("sel2.c", "--aot-policy", "optimized", "--aot-model", "gen2")
        a = selective["aot_policy"]
        check(rc == 0 and a["requested"] == "optimized" and a["experimental"] and a["effective"] == "selective" and not a["fallback"] and a["reason"] == "none" and a["k_sha256"],
              "the experimental request is honoured and reported (requested optimized, experimental, effective selective, no fallback, an admitted-set digest)")
        check((tmp / "sel.c").read_bytes() == (tmp / "sel2.c").read_bytes() and selective == selective2, "two experimental emissions are byte-identical (text and report)")
        check(selective["program_identity"] != plain["program_identity"], "the request enters the program identity")
        check(a["rank_policy"] == "B35" and len(a["model"]) == 64 and len(a["weights"]) == 64 and a["model"] == "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1",
              "the frozen gen2 model, rank policy B35 and weight table are the ones ADR 0102/0140 pin")
        inv = lambda r: [{k: im[k] for k in ("processor", "image_id", "name", "producer", "mapping", "start", "length", "roots")} for im in r["images"]]
        check(inv(selective) == inv(plain), "the image / mapping / root / producer inventory is identical to the broad emission: the proposal never touches authority")
        broad_off, sel_off = {}, {}
        for r, target in ((plain, broad_off), (selective, sel_off)):
            for im in r["images"]:
                target.setdefault((im["processor"], im["name"], im["mapping"]), set()).update(im["source_offsets"])
        check(all(sel_off[k] <= broad_off[k] for k in broad_off) and sum(len(v) for v in sel_off.values()) < sum(len(v) for v in broad_off.values()),
              "every offset the experimental emission compiles is also compiled by the broad one, and it omits some: a proposal can only remove entries")
        # one scoring per image: module C has two mappings and ONE report row
        names = [i["name"] for i in a["images"]]
        check(names.count("lba30") == 1 and len(names) == len(set((i["processor"], i["name"]) for i in a["images"])),
              "scoring is per image: the module mapped at two bases appears once in the policy report")
        check(sel_off[("sub", "lba30", 0)] == sel_off[("sub", "lba30", 1)], "the admitted source-relative offsets are the same for both mappings of the module (shared by every mapping)")
        check(selective["processors"][1]["bodies"] <= plain["processors"][1]["bodies"], "fewer native bodies than the broad emission")

        # ---- the whole three-arm comparison, with the real model -------------------------------------------------------------------
        proc = subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_m3_compare.py"), SEGARECOMP, CC, str(ROOT)], capture_output=True, text=True, timeout=1500)
        check(proc.returncode == 0, "the pre-registered comparison runs (%s)" % proc.stderr[-200:])
        data = json.loads(proc.stdout)
        (tmp / "comparison.json").write_text(proc.stdout)
        arms = data["subjects"]["S1"]["arms"]
        A, B, C = arms["A"], arms["B"], arms["C"]
        check(A["inventory"] == B["inventory"] == C["inventory"], "S1: the inventories of A, B and C are identical")
        check(A["admitted_offsets"] == B["admitted_offsets"] and A["admitted_total"] == B["admitted_total"], "S1: B (position-independent) admits exactly A's source-relative offsets")
        check(all(C["admitted_offsets"].get(k, 0) <= v for k, v in A["admitted_offsets"].items()), "S1: C admits no more offsets than A in any image")
        check(A["deterministic"] and B["deterministic"] and C["deterministic"], "S1: every arm is deterministic")
        check(len({A["program_identity"], B["program_identity"], C["program_identity"]}) == 3, "S1: three arms, three program identities")
        check(A["routes"] == B["routes"], "S1: arms A and B run every route identically (result words, retired, stop)")
        for route, r in C["routes"].items():
            same = r == A["routes"][route]
            check(same or (r["stop"] == "entry_not_compiled" and r["retired"] <= A["routes"][route]["retired"]),
                  "S1/C route %s: identical to A or a typed uncompiled-entry stop after no more work (%s, %d instructions)" % (route, r["stop"], r["retired"]))
        check(A["retention"]["retained"] == A["retention"]["executed_starts"] and C["retention"]["retained"] <= A["retention"]["executed_starts"],
              "S1: retention of executed starts - A %d/%d, C %d/%d (reported, not claimed complete)" % (A["retention"]["retained"], A["retention"]["executed_starts"],
                                                                                              C["retention"]["retained"], A["retention"]["executed_starts"]))
        ca = C["aot_policy"]
        check(ca["effective"] == "selective" and ca["admitted_entries"] == C["admitted_total"] and ca["broad_entries"] == A["admitted_total"] and not ca["fallback"],
              "S1/C: the policy report's counts equal the independently measured offsets (%d of %d)" % (ca["admitted_entries"], ca["broad_entries"]))
        check(C["bodies"] < B["bodies"] <= A["bodies"] and C["c_bytes"] < B["c_bytes"] < A["c_bytes"], "S1: native bodies %d / %d / %d and C bytes %d / %d / %d (A / B / C)" % (
            A["bodies"], B["bodies"], C["bodies"], A["c_bytes"], B["c_bytes"], C["c_bytes"]))
        missed = {k: v["missed"] for k, v in C["recall"].items()}
        print("      label recall (independent encoder starts), A: %s" % {k: "%d/%d" % (v["admitted"], v["labels"]) for k, v in A["recall"].items()})
        print("      label recall (independent encoder starts), C: %s" % {k: "%d/%d" % (v["admitted"], v["labels"]) for k, v in C["recall"].items()})
        check(any(v["missed"] > 0 for v in C["recall"].values()), "S1: the complete-recall gate is NOT met by M3+B35 (missed labelled starts per image: %s) - reported, not hidden" % missed)

        s2 = data["subjects"]["S2"]["arms"]
        check(s2["A"]["status"] == "ok" and s2["C"]["status"] == "ok" and s2["A"]["aot_policy"]["effective"] == "broad", "S2: both builds complete; the default is the broad policy")
        c2 = s2["C"]["aot_policy"]
        check(c2["requested"] == "optimized" and c2["effective"] == "selective" and not c2["fallback"] and c2["admitted_entries"] < c2["broad_entries"],
              "S2: status.json records the experimental policy (admitted %d of %d aligned starts)" % (c2["admitted_entries"], c2["broad_entries"]))
        check(s2["C"]["c_bytes"] < s2["A"]["c_bytes"] and s2["C"]["build_cpu_s"] < s2["A"]["build_cpu_s"], "S2: generated C %d -> %d bytes, build CPU %.1f -> %.1f s" % (
            s2["A"]["c_bytes"], s2["C"]["c_bytes"], s2["A"]["build_cpu_s"], s2["C"]["build_cpu_s"]))
        check(s2["C"]["stop"] in ("none", "entry_not_compiled") and (s2["C"]["stop"] == "none" or s2["C"]["frontier"]), "S2: the experimental program ends normally or at a typed uncompiled entry with a recorded frontier (%s / %s)" % (
            s2["C"]["stop"], s2["C"]["frontier"]))
        check(s2["A"]["program_identity"] != s2["C"]["program_identity"], "S2: the experimental request has its own program identity")
        print(json.dumps({"S1": {k: {kk: vv for kk, vv in v.items() if kk in ("bodies", "admitted_total", "c_bytes", "compile_cpu_s", "compile_peak_kib", "object_bytes", "retention")}
                                    for k, v in arms.items()}, "S2": {k: {kk: vv for kk, vv in v.items() if kk in ("build_cpu_s", "build_peak_kib", "c_bytes", "result", "stop", "frontier", "rounds")}
                                                                      for k, v in s2.items()}}, sort_keys=True))
    print("%d checks, %d failures" % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
