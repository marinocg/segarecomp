#!/usr/bin/env python3
"""SEG-063-T001 Checkpoint B (ADR 0132): bounded ISO9660 extent lookup and digest-checked module admission at build time.

On the synthetic `modules` fixture disc (cooked and raw) this checks:
  * `sega-cd inspect --iso-tree` equals the independent spec-derived ISO9660 oracle (paths, blocks, sizes), and the module digests
    the producer verifies equal the oracle's SHA-256 of those extents;
  * every malformed or unsupported ISO9660 structure the fixture builder can inject ends in its own typed error (inspect and emit);
  * a module is admitted only by an explicit list entry whose disc identity, extent and digest match; a filename, an unlisted file
    or a path whose bytes changed admit nothing; every malformed list or mapping is a typed error;
  * an admitted list compiles each module once per base, in overlay groups, with no decoder, into strict C11; the program identity
    depends on the list, and a disc emitted without a list keeps the SEG-062 shape.

usage: sega_cd_modules_build_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402
import sega_cd_disc_oracle as disc_oracle  # noqa: E402

failures = []
FORBIDDEN = re.compile(r"(?i)m68k_execute|m68ki_|decode_m68k|disassembl|interpret|jit|opcode")

ISO_DEFECT_ERRORS = {
    "bad_identifier": "descriptor_invalid", "no_terminator": "descriptor_invalid", "duplicate_primary": "duplicate_primary",
    "endian": "both_endian_mismatch", "block_size": "unsupported_block_size", "volume_space": "volume_space_invalid",
    "xattr": "unsupported_feature", "multi_extent": "unsupported_feature", "interleave": "unsupported_feature",
    "associated": "unsupported_feature", "cross_sector": "record_crosses_sector", "duplicate_name": "path_ambiguous",
    "extent_range": "extent_out_of_range", "dot_record": "record_invalid",
}


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def tool(*args):
    out = subprocess.run([SEGARECOMP, "sega-cd", *args], capture_output=True, text=True, timeout=300)
    return out.returncode, json.loads(out.stdout) if out.stdout.strip() else {}


def main():
    with tempfile.TemporaryDirectory(prefix="scd-mod-build-") as tmp:
        tmp = pathlib.Path(tmp)
        for variant in ("cooked", "raw"):
            disc = tmp / ("disc-" + variant)
            described = common.build_disc(disc, "modules", variant)
            identity, files, tree = common.disc_facts(disc)
            code, inspected = tool("inspect", "--cue", str(disc / "disc.cue"), "--iso-tree")
            iso = inspected.get("iso9660", {})
            ours = [(e["path"], e["directory"], e["block"], e["size"]) for e in iso.get("entries", [])]
            theirs = [(e["path"], e["directory"], e["block"], e["size"]) for e in tree["entries"]]
            check(code == 0 and iso.get("ok") and ours == theirs and iso["volume_space"] == tree["volume_space"],
                  variant + ": ISO9660 tree equals the independent oracle (%d entries)" % len(theirs))
            check(all(files["/OVL/" + n]["sha256"] == v["sha256"] and files["/OVL/" + n]["block"] == v["block"]
                      for n, v in described["files"].items() if n != "README.TXT;1"),
                  variant + ": oracle extents and digests equal the builder's module files")
            manifest = tmp / ("modules-" + variant + ".txt")
            manifest.write_text(common.standard_manifest(disc), newline="\n")
            code, inspected = tool("inspect", "--cue", str(disc / "disc.cue"), "--modules", str(manifest))
            mods = inspected.get("modules", {}).get("modules", [])
            check(code == 0 and [m["name"] for m in mods] == ["moda", "modb"] and
                  all(m["sha256"] == files[m["path"]]["sha256"] and m["block"] == files[m["path"]]["block"] for m in mods) and
                  [m["image_id"] for m in mods] == [2, 3],
                  variant + ": both listed modules are admitted with the oracle's extents and digests, as sub images 2 and 3")

        disc = tmp / "disc-cooked"
        cue = disc / "disc.cue"
        identity, files, _ = common.disc_facts(disc)

        # --- ISO9660 defects --------------------------------------------------------------------------------------------------
        for defect, error in ISO_DEFECT_ERRORS.items():
            bad = tmp / ("iso-" + defect)
            common.build_disc(bad, "modules", "cooked", "--iso-defect", defect)
            code, inspected = tool("inspect", "--cue", str(bad / "disc.cue"), "--iso-tree")
            tree_error = inspected.get("iso9660", {}).get("error")
            bad_identity = disc_oracle.read_disc(bad / "disc.cue")["identity"]
            text = common.module_list(bad_identity, common.standard_modules(files, ("MODA.BIN;1",)))
            (bad / "modules.txt").write_text(text, newline="\n")
            code2, emitted = tool("emit", "--cue", str(bad / "disc.cue"), "--out", str(bad / "p.c"), "--modules", str(bad / "modules.txt"))
            tree_expected = None if defect == "duplicate_name" else error  # listing is fine; only the lookup is ambiguous
            check(tree_error == tree_expected and code2 == 5 and emitted.get("error") == "iso9660" and emitted.get("iso9660") == error,
                  "ISO9660 %s: inspect and emit stop with %s (got %s / %s)" % (defect, error, tree_error, emitted.get("iso9660")))

        # --- module list admission and its negatives --------------------------------------------------------------------------
        def emit_with(text, name):
            path = tmp / (name + ".txt")
            path.write_text(text, newline="\n")
            return tool("emit", "--cue", str(cue), "--out", str(tmp / (name + ".c")), "--modules", str(path))

        base = common.standard_modules(files)

        def variant_of(index=0, **changes):
            mods = [dict(m) for m in base]
            mods[index].update(changes)
            return common.module_list(identity, mods)

        cases = [
            ("disc identity of another disc", common.module_list("0" * 64, base), "disc_identity_mismatch"),
            ("digest of other bytes (a filename alone admits nothing)", variant_of(sha256=files["/OVL/MODC.BIN;1"]["sha256"]),
             "digest_mismatch"),
            ("unlisted path", variant_of(path="/OVL/MODX.BIN;1"), "iso9660"),
            ("path naming a directory", variant_of(path="/OVL"), "iso9660"),
            ("path through a file", variant_of(path="/README.TXT;1/MODA.BIN;1"), "iso9660"),
            ("path without its version", variant_of(path="/OVL/MODA.BIN"), "iso9660"),
            ("relative path", variant_of(path="OVL/MODA.BIN;1"), "iso9660"),
            ("extent disagreeing with the path", variant_of(extent=(files["/OVL/MODA.BIN;1"]["block"] + 1, 192)), "extent_mismatch"),
            ("neither path nor extent", variant_of(path=None), "source_missing"),
            ("extent in the system area", variant_of(path=None, extent=(2, 192)), "extent_outside_track"),
            ("extent past the track", variant_of(path=None, extent=(39, 4096)), "extent_outside_track"),
            ("odd base", variant_of(bases=[0x020001]), "mapping_invalid"),
            ("base outside executable RAM (PCM)", variant_of(bases=[0x0F0000]), "mapping_outside_executable_ram"),
            ("range straddling PRG RAM and Word RAM", variant_of(bases=[0x07FFC0]), "mapping_outside_executable_ram"),
            ("main base outside its RAM", variant_of(processor="main", bases=[0x080000]), "mapping_outside_executable_ram"),
            ("no base", variant_of(bases=[]), "mapping_missing"),
            ("odd entry", variant_of(entries=[1]), "entry_invalid"),
            ("entry outside the module", variant_of(entries=[192]), "entry_invalid"),
            ("no entry", variant_of(entries=[]), "entry_missing"),
            ("structural verification for RAM code", variant_of(verify="structural"), "verification_unsupported"),
            ("one module mapped over itself", variant_of(bases=[0x020000, 0x020040]), "image_validation"),
            ("the same bytes listed twice at one base", common.module_list(identity, [base[0], dict(base[0], name="moda_again")]),
             "duplicate_module"),
            ("a repeated name", common.module_list(identity, [base[0], dict(base[1], name="moda")]), "duplicate_name"),
            ("an invalid name", variant_of(name="ModA"), "name_invalid"),
            ("an unknown processor", variant_of(processor="z80"), "processor_invalid"),
        ]
        for label, text, error in cases:
            code, out = emit_with(text, "neg-" + re.sub(r"\W+", "_", label))
            check(code == 5 and out.get("error") == error, "module list: %s -> %s (got %s)" % (label, error, out.get("error")))
        syntax = [
            ("no version line", "disc %s\n" % identity, "manifest_version"),
            ("unknown directive", common.standard_manifest(disc).replace("verify byte_identity", "verify byte_identity\ntrust yes", 1),
             "manifest_syntax"),
            ("missing end", common.standard_manifest(disc).rsplit("end\n", 1)[0], "manifest_syntax"),
            ("repeated sha256", common.standard_manifest(disc).replace("processor sub", "processor sub\nsha256 " + "0" * 64, 1),
             "manifest_syntax"),
            ("bad number", common.standard_manifest(disc).replace("map 0x80000", "map 0xZZ", 1), "manifest_syntax"),
        ]
        for label, text, error in syntax:
            code, out = emit_with(text, "syn-" + re.sub(r"\W+", "_", label))
            check(code == 5 and out.get("error") == error, "module list syntax: %s -> %s (got %s)" % (label, error, out.get("error")))
        code, out = tool("emit", "--cue", str(cue), "--out", str(tmp / "x.c"), "--modules", str(tmp / "missing.txt"))
        check(code == 5 and out.get("error") == "manifest_unreadable", "an unreadable module list is refused")

        # --- emission ---------------------------------------------------------------------------------------------------------
        code, plain = tool("emit", "--cue", str(cue), "--out", str(tmp / "plain.c"))
        code2, listed = emit_with(common.standard_manifest(disc), "listed")
        source = (tmp / "listed.c").read_text()
        plain_source = (tmp / "plain.c").read_text()
        check(code == 0 and plain["overlay_groups"] == 0 and "scd_overlay" not in plain_source and
              "scd_program_main(&scd_program" in plain_source, "without a list the modules disc emits the SEG-062 shape")
        check(code2 == 0 and listed["overlay_groups"] == 2 and listed["program_identity"] != plain["program_identity"],
              "with the list: two overlay groups ($080000, $020000) and a different program identity")
        images = [i for i in listed["images"] if i["producer"] == "sega_cd.disc_module"]
        check(sorted((i["name"], i["start"]) for i in images) ==
              sorted([("moda", 0x080000), ("moda", 0x020000), ("modb", 0x080000), ("modb", 0x020000)]) and
              all(i["compiled"] > 0 and i["unsupported"] == 0 and i["overlay_group"] >= 0 for i in images),
              "each module is compiled once per base, every reached instruction lowered")
        for fn in ("scd_p1_m2_map0_i_00080000", "scd_p1_m3_map0_i_00080000", "scd_p1_m2_map1_i_00020000", "scd_p1_m3_map1_i_00020000"):
            check(("static int %s(ScdMachine *m)" % fn) in source, "compiled entry " + fn)
        check(not FORBIDDEN.search(source), "the generated program names no decoder, interpreter or JIT")
        check("MODA.BIN" not in source and "scd_overlay_groups[]" in source and "scd_program_main_overlays" in source,
              "the registry is emitted; module bytes are not (only instruction verification bytes and granule digests)")
        obj = tmp / "listed.o"
        compiled = subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME), "-c",
                                   str(tmp / "listed.c"), "-o", str(obj)], capture_output=True, text=True)
        check(compiled.returncode == 0, "the generated program compiles as strict C11 with warnings as errors" +
              ("" if compiled.returncode == 0 else ": " + compiled.stderr[:400]))
        code3, again = emit_with(common.standard_manifest(disc), "listed2")
        check(code3 == 0 and (tmp / "listed2.c").read_bytes() == (tmp / "listed.c").read_bytes() and
              again["program_identity"] == listed["program_identity"], "emission is deterministic")
        commented = common.standard_manifest(disc).replace("module modb", "# another comment\nmodule modb")
        code4, comment_run = emit_with(commented, "listed3")
        check(code4 == 0 and comment_run["program_identity"] == listed["program_identity"],
              "the program identity depends on the admitted modules, not on list formatting")

    print("PASS" if not failures else "FAIL (%d)" % len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
