#!/usr/bin/env python3
"""SEG-135-T004/T005/T006: the `segarecomp mod` workflow end to end with a FABRICATED fixture image (no commercial input).
T004  golden output of every subcommand, stable failure classes + exit codes, `pack` bit-reproducible across runs and working directories, `inspect` stable JSON.
T005  the single-mod variant identity (original image, effective image, package identity, sources, ABI, capabilities): unchanged inputs hit the cache, each component change misses and
      is NAMED, a broken C11 unit fails only its own variant and leaves Vanilla, saves and earlier variants byte-identical.
T006  init (each template) -> check -> build -> pack -> install -> approve -> variant, with pinned package and variant digests; templates compile as strict C11.
usage: mod_workflow_test.py <segarecomp-cli> <platforms-dir> <C compiler>"""
import hashlib
import json
import os
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
SDK, CC = os.path.abspath(sys.argv[2]), sys.argv[3]
tmp = temp()
ROM_B = b"synthetic-source-image-B"
SHA_B = sha(ROM_B)
rom_a = write(tmp / "fixture" / "a.bin", SRC_IMAGE)
rom_b = write(tmp / "fixture" / "b.bin", ROM_B)

# Pinned digests of the c11-provider template project below (id "tpl", fixture revision A). A change to a template, the manifest writer or the packer changes these on purpose.
PINNED_IDENTITY = "845ace166357d5fc6faf1c9bfd268a5d68fe7458a07850a9bebfa2116d8915db"
PINNED_CONTENT = "af196077cfa7d62176a5a8db5ca881d475f0375919e57b5c2e3efdcddb1677ab"
# SEG-137-T003: a package with Luau adds the trusted compiler's bytecode digest (pinned Luau 0.746, optimization 1, debug 0) as the `luau_bytecode` identity component, so the variant key changed on purpose.
PINNED_LUAU_DIGEST = "cf6722a198a4912ecf5e253c5727cb6e102b254baee46079e4f4badd542e239a"
PINNED_VARIANT_KEY = "c83ae6ee347a9975514d959f54a8c5f58d812580eb16c700f2415bf05db68f37"


def ok(what, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc


def refused(what, code, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


def tree(root):
    root = Path(root)
    return {str(p.relative_to(root)): sha(p.read_bytes()) for p in sorted(root.rglob("*")) if p.is_file()}


def template(name, kind, platform="genesis", sha_hex=SRC_SHA):
    d = tmp / name
    doc = ok(f"init {kind}", "mod", "init", d, "--platform", platform, "--id", "tpl", "--template", kind, "--rom-sha256", sha_hex)
    return d, doc


# ------------------------------------------------------------------ T004: golden output per subcommand
proj, init_doc = template("proj", "c11-provider")
cli.check(init_doc == {"ok": True, "created": ["mod.toml", "scripts/main.luau", "src/provider.c"]}, f"init golden: {init_doc}")
GOLDEN_FIELDS = '"id":"tpl","version":"0.1.0","name":"tpl","platform":"genesis","source_identity_kind":"cartridge-image","revisions":[{"label":"rev-a","sha256":"%s"}],"trust_class":"native","capabilities":[],"capabilities_optional":[],"abi_native":2,"payloads":{"native":1,"luau":1,"assets":0,"other":0},"rom_edits":0,"symbols":[],"package_identity":"%s","content_digest":"%s"' % (SRC_SHA, PINNED_IDENTITY, PINNED_CONTENT)


def raw(*args, cwd=None):
    return cli.run(*args, cwd=cwd)[2].stdout


pkg_a, pkg_b, pkg_c = tmp / "a.segapatch", tmp / "b.segapatch", tmp / "c.segapatch"
cli.check(raw("mod", "check", proj) == '{"ok":true,' + GOLDEN_FIELDS + ',"package_bytes":1829}\n', "check golden output")
cli.check(raw("mod", "pack", proj, "--out", pkg_a) == '{"ok":true,' + GOLDEN_FIELDS + ',"package_bytes":1829,"packed":true}\n', "pack golden output")
cli.check(raw("mod", "build", proj, "--out", pkg_b, "--cc", CC, "--sdk-dir", SDK) == '{"ok":true,' + GOLDEN_FIELDS + ',"package_bytes":1829,"built":true,"luau":{"compiler":"luau-0.746","scripts":1,"bytecode_digest":"%s"}}\n' % PINNED_LUAU_DIGEST, "build golden output")
cli.check(pkg_a.read_bytes() == pkg_b.read_bytes(), "pack and build emit the identical package")
inspect_golden = raw("mod", "inspect", pkg_a)
cli.check(inspect_golden.startswith('{"ok":true,' + GOLDEN_FIELDS + ',"adapter":{"platform":"genesis","source_identity_kind":"cartridge-image"'), "inspect golden prefix")
cli.check(raw("mod", "inspect", "--json", pkg_a) == inspect_golden and raw("mod", "inspect", proj).replace('"checked":true,', "") == inspect_golden, "inspect is stable across --json, package and project forms")
adapters = ok("inspect without argument", "mod", "inspect")
cli.check([a["platform"] for a in adapters["adapters"]] == ["genesis", "master-system", "sega-cd"], "adapter listing order is stable")

# pack is bit-reproducible: two runs, two working directories, a copy at another path
proj2 = tmp / "elsewhere" / "deep" / "copy"
for rel, data in tree(proj).items():
    write(proj2 / rel, (proj / rel).read_bytes())
ok("pack from the parent directory (relative paths)", "mod", "pack", "proj", "--out", "rel.segapatch", cwd=tmp)
ok("pack a copy from another directory", "mod", "pack", proj2, "--out", pkg_c, cwd=tmp / "elsewhere")
cli.check(pkg_a.read_bytes() == pkg_c.read_bytes() == (tmp / "rel.segapatch").read_bytes(), "pack is byte-identical across runs and working directories")
cli.check(sha(pkg_a.read_bytes()) == sha(pkg_c.read_bytes()), "package file digest repeats")

# failures: non-zero exit and a stable class on every path
rc, doc, _ = cli.run("mod", "check", tmp / "no-such-dir")
cli.check(rc == 1 and doc and doc["ok"] is False and doc["class"].startswith("E_"), f"check of a missing project fails with a class: {doc}")
refused("init unknown template", "E_FIELD_VALUE", "mod", "init", tmp / "t1", "--platform", "genesis", "--id", "x", "--template", "nope")
refused("init sms c11 template", "E_PLATFORM_UNKNOWN", "mod", "init", tmp / "t2", "--platform", "master-system", "--id", "x", "--template", "c11-provider")
refused("init bad digest", "E_FIELD_VALUE", "mod", "init", tmp / "t3", "--platform", "genesis", "--id", "x", "--rom-sha256", "XYZ")
rc, doc, _ = cli.run("mod", "inspect", write(tmp / "junk.segapatch", b"not a zip"))
cli.check(rc == 1 and doc and doc["ok"] is False and doc["class"].startswith("E_"), f"inspect of a garbage package fails with a class: {doc}")
refused("build native sources without a compiler", "E_COMPILE_FAILED", "mod", "build", proj, "--out", tmp / "nocc.segapatch")
cli.check(not (tmp / "nocc.segapatch").exists(), "a failed build writes no package")
for bad in (("mod",), ("mod", "frobnicate"), ("mod", "pack", proj), ("mod", "install", pkg_a)):
    rc, doc, p = cli.run(*bad)
    cli.check(rc == 2 and "usage:" in p.stderr, f"usage error exits 2: {bad}")

# ------------------------------------------------------------------ T006: templates (strict C11 build) and the other templates
luau_dir, _ = template("luau-proj", "luau")
ok("luau template checks", "mod", "check", luau_dir)
edit_dir, doc = template("edit-proj", "rom-edit")
cli.check("edits/main.edits" in doc["created"], "rom-edit template creates a commented edit file")
ok("rom-edit template checks", "mod", "check", edit_dir)
sms_dir, _ = template("sms-proj", "luau", platform="master-system")
ok("master-system luau template checks", "mod", "check", sms_dir)
# a template with a deliberate warning fails the strict host flags (the flags are real)
warn = tmp / "warn-proj"
for rel in tree(proj):
    write(warn / rel, (proj / rel).read_bytes())
write(warn / "src" / "provider.c", (proj / "src" / "provider.c").read_text() + "\nstatic int unused_helper(void) { return 1; }\n")
refused("a warning is an error", "E_COMPILE_FAILED", "mod", "build", warn, "--out", tmp / "warn.segapatch", "--cc", CC, "--sdk-dir", SDK)

# ------------------------------------------------------------------ install / approve / variant (T005 + T006)
lib = tmp / "library"
write(lib / "vanilla" / "game-build" / "game.bin", b"vanilla build output")
write(lib / "saves" / "slot0.sav", b"user save")
vanilla_before = tree(lib)
ident = PINNED_IDENTITY
VARIANT = ("mod", "variant", "--library", lib, "--identity", ident, "--rom", rom_a, "--cc", CC, "--sdk-dir", SDK)

refused("variant before install", "E_NOT_INSTALLED", *VARIANT)
inst = ok("install (not yet approved)", "mod", "install", pkg_a, "--rom", rom_a, "--library", lib)
cli.check(inst["package_identity"] == PINNED_IDENTITY and inst["added"] is True and inst["approved"] is False and inst["trust_class"] == "native", f"install result: {inst}")
refused("native variant without approval", "E_NOT_APPROVED", *VARIANT)
refused("approval of another identity", "E_APPROVAL_MISMATCH", "mod", "install", pkg_a, "--rom", rom_a, "--library", lib, "--approve", "0" * 64)
refused("install against a different image", "E_IMAGE_MISMATCH", "mod", "install", pkg_a, "--rom", rom_b, "--library", lib)
inst = ok("approve the exact identity", "mod", "install", pkg_a, "--rom", rom_a, "--library", lib, "--approve", ident)
cli.check(inst["added"] is False and inst["approved"] is True, f"re-install approves without duplicating: {inst}")

first = ok("first variant build", *VARIANT)
cli.check(first["cache"] == "miss" and first["reasons"] == ["no_previous_build"] and first["variant_key"] == PINNED_VARIANT_KEY, f"first build is a miss with a pinned key: {first}")
cli.check(first["components"]["original_image"] == SRC_SHA and first["components"]["effective_image"] == SRC_SHA and first["components"]["abi"] == "2", "components name the original and effective image and the ABI")
again = ok("unchanged inputs", *VARIANT)
cli.check(again["cache"] == "hit" and again["reasons"] == [] and again["variant_key"] == first["variant_key"], f"unchanged inputs hit the cache: {again}")
refused("variant with a different image", "E_IMAGE_MISMATCH", "mod", "variant", "--library", lib, "--identity", ident, "--rom", rom_b, "--cc", CC, "--sdk-dir", SDK)
cli.check(first["package_identity"] == ident, "variant reports the package identity")
cache_snapshot = tree(lib / "cache")


def edited(name, mutate):
    """A copy of the template project mutated by `mutate(dir)`; packed, installed and approved; returns (identity, package)."""
    d = tmp / name
    for rel in tree(proj):
        write(d / rel, (proj / rel).read_bytes())
    mutate(d)
    out = tmp / f"{name}.segapatch"
    doc = ok(f"pack {name}", "mod", "pack", d, "--out", out)
    ok(f"install {name}", "mod", "install", out, "--rom", rom_a, "--library", lib, "--approve", doc["package_identity"])
    return doc["package_identity"]


def variant_of(identity, rom=rom_a):
    return cli.run("mod", "variant", "--library", lib, "--identity", identity, "--rom", rom, "--cc", CC, "--sdk-dir", SDK)


# each identity component change misses, and the reason NAMES it
luau_id = edited("v-luau", lambda d: write(d / "scripts" / "main.luau", "-- changed\n"))
rc, doc, _ = variant_of(luau_id)
cli.check(rc == 0 and doc["cache"] == "miss" and sorted(doc["reasons"]) == ["luau_bytecode", "package_identity", "source_digest"], f"a source change misses naming package_identity + source_digest + luau_bytecode: {doc}")
cap_id = edited("v-cap", lambda d: write(d / "mod.toml", (d / "mod.toml").read_text() + '\n[capabilities]\nrequired = ["host.log"]\n'))
rc, doc, _ = variant_of(cap_id)
cli.check(rc == 0 and doc["cache"] == "miss" and "capabilities" in doc["reasons"] and "package_identity" in doc["reasons"], f"a capability change misses naming capabilities: {doc}")
rev_id = edited("v-rev", lambda d: write(d / "mod.toml", (d / "mod.toml").read_text().replace('[[source]]\nkind = "luau-source"', f'[[target.revision]]\nlabel = "rev-b"\nsha256 = "{SHA_B}"\n\n[[source]]\nkind = "luau-source"', 1)))
rc, doc, _ = variant_of(rev_id, rom_b)
cli.check(rc == 0 and doc["cache"] == "miss" and doc["components"]["original_image"] == SHA_B, f"a different original image keys a different variant: {doc}")
rc, doc2, _ = variant_of(rev_id, rom_a)
cli.check(rc == 0 and doc2["cache"] == "miss" and doc2["variant_key"] != doc["variant_key"], "the same package on another declared revision is its own variant")
rc, doc3, _ = variant_of(rev_id, rom_b)
cli.check(rc == 0 and doc3["cache"] == "hit" and doc3["variant_key"] == doc["variant_key"], "and that variant is reused")
rc, doc, _ = variant_of(ident)
cli.check(rc == 0 and doc["cache"] == "hit" and doc["variant_key"] == PINNED_VARIANT_KEY, f"returning to the first package reuses its still-cached variant: {doc}")

# ------------------------------------------------------------------ failure injection: a broken C11 unit fails only that variant
bad_id = edited("v-broken", lambda d: write(d / "src" / "provider.c", "int broken( {\n"))
after_install = tree(lib)
before_cache = {k: v for k, v in after_install.items() if k.startswith("cache/")}
rc, doc, _ = variant_of(bad_id)
cli.check(rc == 1 and doc["ok"] is False and doc["class"] == "E_COMPILE_FAILED", f"a broken unit fails with E_COMPILE_FAILED: {doc}")
after = tree(lib)
cli.check({k: v for k, v in after.items() if k.startswith("cache/")} == before_cache, "a failed build publishes nothing and leaves every earlier variant and index byte-identical")
cli.check(all(after[k] == v for k, v in vanilla_before.items()), "Vanilla build output and saves are byte-identical")
cli.check(not any(".partial" in k for k in after), "no partial variant remains")
rc, doc, _ = variant_of(ident)
cli.check(rc == 0 and doc["variant_key"] == PINNED_VARIANT_KEY, "the earlier variant is still addressable after the failure")

# ------------------------------------------------------------------ regression: variant identity follows the exact received manifest bytes, not a re-serialisation
lp, lf = luau()
m1 = base_manifest(payloads=[(lp, lf)])
m2 = {k: m1[k] for k in ("format", "schema_version", "version", "id", "name", "platform", "source", "payloads")}   # same semantics, different key order
cli.check(m1 != list(m2.items()) and canon(m1) != canon(m2) and json.loads(canon(m1)) == json.loads(canon(m2)), "fixture: semantically equal manifests with different key order")
order_pkgs = []
for tag, m in (("canonical", m1), ("reordered", m2)):
    path = tmp / f"order-{tag}.segapatch"
    path.write_bytes(build(m, [(lp, lf)]))
    doc = ok(f"inspect the {tag} manifest order", "mod", "inspect", path)
    cli.check(doc["package_identity"] == sha(canon(m)), f"{tag}: identity is the SHA-256 of the exact received manifest bytes")
    inst = ok(f"install the {tag} order", "mod", "install", path, "--rom", rom_a, "--library", lib)
    order_pkgs.append(inst["package_identity"])
cli.check(order_pkgs[0] != order_pkgs[1], "different key order -> different package identities")
results = []
for identity in order_pkgs:
    results.append(ok("variant of a luau-only package", "mod", "variant", "--library", lib, "--identity", identity, "--rom", rom_a))
cli.check(results[0]["variant_key"] != results[1]["variant_key"] and all(r["cache"] == "miss" for r in results), f"different identities -> different variant keys and cache misses: {[r['variant_key'] for r in results]}")
cli.check(results[0]["components"]["package_identity"] == order_pkgs[0] and results[1]["components"]["package_identity"] == order_pkgs[1], "the package_identity component is the verified Package.identity")
cli.check(results[0]["components"]["source_digest"] == results[1]["components"]["source_digest"], "identical payloads keep an identical source_digest")
repeat = ok("repeat the reordered variant", "mod", "variant", "--library", lib, "--identity", order_pkgs[1], "--rom", rom_a)
cli.check(repeat["cache"] == "hit", "each order is reused on its own")

cli.done("mod_workflow_test")
