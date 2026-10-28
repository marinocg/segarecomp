#!/usr/bin/env python3
"""SEG-097-T003 (ADR 0208): the experimental frozen M3 + B35 path for Sega CD, end to end with the REAL native model on project-authored fixtures.

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
        rc, compat_out, _ = cli("aot-policy-identity", "--platform", "sega-cd", "--aot-policy", "compatibility")
        check(rc == 0 and "policy_identity=773742c0e113b3dc5754bb759513502cc5e519f4094ee1fe0755ad247c902211" in compat_out and "selective_admission.v1" in compat_out,
              "the Compatibility policy identity is unchanged by the changed experimental contract")
        check(fields["policy_identity"] != "dcfc165125640c9d0b26a4a61fa5bc5c417e3b2c616dbf30a1c067e489ac7ca8" and "selective_admission.v2" in scd_out,
              "the experimental policy identity changed (contract v2): a build made under the previous eligibility can never be reused")
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
        check(set(plain["aot_policy"]) == {"requested", "experimental", "effective", "fallback", "reason", "failed_image", "policy_identity", "contract", "k_sha256", "broad_entries",
                                           "admitted_entries", "images"} and plain["aot_policy"]["contract"].endswith("selective_admission.v1"),
              "the Compatibility report keeps exactly its earlier keys and contract name")
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
        check(all(sel_off[k] <= broad_off[k] for k in broad_off) and sum(len(v) for v in sel_off.values()) <= sum(len(v) for v in broad_off.values()),
              "every offset the experimental emission compiles is also compiled by the broad one: a proposal can only remove entries")
        hist_rc, hist, _ = emit("hist.c", "--aot-policy", "optimized", "--aot-model", "gen2", "--aot-scope", "all-images")
        check(hist_rc == 0 and hist["program_identity"] not in (selective["program_identity"], plain["program_identity"]) and hist["aot_policy"]["scope"] == "all_images" and
              selective["aot_policy"]["scope"] == "game_modules" and hist["aot_policy"]["m3_images"] == 5 and selective["aot_policy"]["m3_images"] == 3,
              "the scope is part of the identity: the historical all-image arm and the shipped game-module arm are different programs (scored images 5 vs 3)")
        proc = subprocess.run([*base, "--out", str(tmp / "bad.c"), "--aot-scope", "all-images"], capture_output=True, text=True)
        check(proc.returncode == 2, "usage error: --aot-scope without the experimental policy")
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
        A, B, H, C = arms["A"], arms["B"], arms["H"], arms["C"]
        check(A["inventory"] == B["inventory"] == H["inventory"] == C["inventory"], "S1: the inventories of A, B, H and C are identical (authority and mappings untouched)")
        check(A["admitted_offsets"] == B["admitted_offsets"], "S1: B (source-relative bodies) admits exactly A's source-relative offsets")
        check(all(C["admitted_offsets"].get(k, 0) <= v for k, v in A["admitted_offsets"].items()), "S1: C admits no more offsets than A in any image")
        check(A["deterministic"] and B["deterministic"] and H["deterministic"] and C["deterministic"], "S1: every arm is deterministic")
        check(len({A["program_identity"], B["program_identity"], H["program_identity"], C["program_identity"]}) == 4, "S1: four arms, four program identities (scope is in the experimental identity)")
        check(A["routes"] == B["routes"], "S1: arms A and B run every route identically")
        # the policy: boot code is broad, the modules are scored
        sel = {i["name"]: i for i in C["images_policy"]}
        check(sel["boot_ip"]["selection"] == "broad_required" and sel["boot_sp"]["selection"] == "broad_required" and
              sel["boot_ip"]["selection_reason"] == "sega_cd.boot_ip" and sel["boot_sp"]["selection_reason"] == "sega_cd.boot_sp",
              "S1/C: the boot IP and SP are broad_required (reason: their producer) and not scored")
        mods = [i for n, i in sel.items() if n.startswith("lba")]
        check(len(mods) == 3 and all(m["selection"] == "m3_selected" and m["selection_reason"] == "sega_cd.disc_module" and m["units"] > 0 and m["selected"] > 0 for m in mods),
              "S1/C: the three disc game modules are genuinely scored by M3 (cells %s, selected %s)" % ([m["units"] for m in mods], [m["selected"] for m in mods]))
        check(C["admitted_digests"]["main:boot_ip"] == A["admitted_digests"]["main:boot_ip"] and C["admitted_digests"]["sub:boot_sp"] == A["admitted_digests"]["sub:boot_sp"],
              "S1/C: the admitted instruction sets of boot IP and SP equal Compatibility exactly")
        ca = C["aot_policy"]
        check(ca["effective"] == "selective" and not ca["fallback"] and ca["m3_images"] == 3 and ca["m3_admitted_entries"] <= ca["m3_broad_entries"] and
              ca["admitted_entries"] == C["admitted_total"] and ca["broad_entries"] == A["admitted_total"],
              "S1/C: report - effective selective, 3 scored images (%d of %d entries admitted), totals count the broad images as broad-admitted" % (ca["m3_admitted_entries"], ca["m3_broad_entries"]))
        for route in C["routes"]:
            check(C["routes"][route] == A["routes"][route] and A["routes"][route]["stop"] == "none",
                  "S1/C route %s completes with exactly Compatibility's results (words %s, %d instructions)" % (route, C["routes"][route]["words"], C["routes"][route]["retired"]))
        check(C["retention"]["retained"] == A["retention"]["executed_starts"], "S1/C: every executed start of the three routes is retained (%d/%d)" % (C["retention"]["retained"], A["retention"]["executed_starts"]))
        check(any(r["stop"] == "entry_not_compiled" for r in H["routes"].values()) and H["retention"]["retained"] < A["retention"]["executed_starts"],
              "S1/H (historical global scope): stops at uncompiled boot code on %s routes, retains %d/%d - the reason for the change" % (
                  [k for k, r in H["routes"].items() if r["stop"] != "none"], H["retention"]["retained"], A["retention"]["executed_starts"]))
        check(C["bodies"] <= B["bodies"] <= A["bodies"], "S1: native bodies A %d / B %d / H %d / C %d, C bytes %d / %d / %d / %d" % (A["bodies"], B["bodies"], H["bodies"], C["bodies"],
                                                                                           A["c_bytes"], B["c_bytes"], H["c_bytes"], C["c_bytes"]))
        print("      label recall (independent encoder starts), A: %s" % {k: "%d/%d" % (v["admitted"], v["labels"]) for k, v in A["recall"].items()})
        print("      label recall (independent encoder starts), C: %s" % {k: "%d/%d" % (v["admitted"], v["labels"]) for k, v in C["recall"].items()})
        check(any(v["missed"] > 0 for k, v in C["recall"].items() if k.startswith("lba")) or all(C["recall"][k]["admitted"] == A["recall"][k]["admitted"] for k in C["recall"]),
              "S1: module recall under M3 is reported, not claimed (missed per image %s; full recall is not claimed - ADR 0140's 306 starts remain a known limitation)" % {k: v["missed"] for k, v in C["recall"].items()})

        # BIOS-profile eligibility by provenance (negative and positive, project-authored fixture): firmware, a user-declared extent and a boot-area extent stay broad;
        # a discovered extent outside the boot area is scored.
        fx = tmp / "bf"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "direct", "--out", str(fx)], check=True, capture_output=True)
        bios_base = [SEGARECOMP, "sega-cd", "emit", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--aot-policy", "optimized", "--aot-model", "gen2"]

        def bios_emit(name, *extra):
            proc = subprocess.run([*bios_base, "--out", str(tmp / name), *extra], capture_output=True, text=True, timeout=600)
            return json.loads(proc.stdout)
        first = bios_emit("e0.c")
        pid = first["program_identity"]
        manual = bios_emit("e1.c", "--disc-image", "sub:81920:524288:4096")
        boot_area = bios_emit("e2.c", "--admit", "extent:%s:sub:2048:24576:1024" % pid)
        game_extent = bios_emit("e3.c", "--admit", "extent:%s:sub:81920:524288:4096" % pid)
        pol = lambda r: {i["name"]: (i["selection"], i["selection_reason"]) for i in r["aot_policy"]["images"]}
        check(pol(first)["bios"] == ("broad_required", "bios_firmware"),
              "BIOS profile: the BIOS image is broad_required (bios_firmware)")
        check(first["aot_policy"]["effective"] == "broad" and first["aot_policy"]["reason"] == "no_eligible_images", "BIOS profile: nothing but firmware admitted -> no eligible image, a broad build with that reason")
        check(pol(manual)["disc0"] == ("broad_required", "manual_disc_image"), "BIOS profile: a user-declared disc image has no established role and stays broad (manual_disc_image)")
        check(pol(boot_area)["disc0"] == ("broad_required", "boot_area_extent"), "BIOS profile: a discovered extent inside the disc's system area (boot code) stays broad (boot_area_extent)")
        check(pol(game_extent)["disc0"] == ("m3_selected", "disc_extent") and game_extent["aot_policy"]["effective"] == "selective" and game_extent["aot_policy"]["m3_images"] == 1 and
              game_extent["aot_policy"]["admitted_entries"] > 0, "BIOS profile: a discovered game extent outside the boot area is scored (m3_selected, disc_extent)")
        check(game_extent["program_identity"] != first["program_identity"], "BIOS profile: the admission chain keeps working under the experimental identity")

        s2 = data["subjects"]["S2"]["arms"]
        check(all(s2[a]["status"] == "ok" for a in "ACH") and s2["A"]["aot_policy"]["effective"] == "broad", "S2: all builds complete; the default is the broad policy")
        c2 = s2["C"]["aot_policy"]
        check(c2["requested"] == "optimized" and c2["effective"] == "selective" and not c2["fallback"] and c2["m3_images"] == 1 and c2["m3_admitted_entries"] < c2["m3_broad_entries"],
              "S2/C: status.json records the experimental request and the product's effective policy (%s; reason %s; scored images %s)" % (c2["effective"], c2["reason"], c2.get("m3_images")))
        names = {i["name"]: i for i in s2["C"]["images"]}
        check(names["bios"]["selection"] == "broad_required" and names["bios"]["selection_reason"] == "bios_firmware" and
              all(i["selection"] in ("broad_required", "m3_selected", "broad_fallback") for i in names.values()), "S2/C: the BIOS image is broad_required (bios_firmware); every image reports its selection")
        check(all(i["selection"] != "m3_selected" or i["selection_reason"] == "disc_extent" for i in names.values()), "S2/C: only discovered disc extents outside the boot area are ever scored")
        check(s2["A"]["program_identity"] != s2["C"]["program_identity"], "S2: the experimental request has its own program identity")
        print(json.dumps({"S1": {k: {kk: vv for kk, vv in v.items() if kk in ("bodies", "admitted_total", "c_bytes", "compile_cpu_s", "compile_peak_kib", "object_bytes", "retention")}
                                    for k, v in arms.items()}, "S2": {k: {kk: vv for kk, vv in v.items() if kk in ("build_cpu_s", "build_peak_kib", "c_bytes", "result", "stop", "frontier", "rounds", "extents", "aot_policy")}
                                                                      for k, v in s2.items()}}, sort_keys=True))
    print("%d checks, %d failures" % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
