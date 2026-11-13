#!/usr/bin/env python3
"""SEG-135-T002 + SEG-146-T002: the `mod.toml` project model and the platform-adapter registry through the product CLI (`segarecomp mod init|check|build|inspect`).
A project maps 1:1 onto a schema 3 manifest: the expected manifest is rebuilt here independently (Python) and compared byte for byte; loads are deterministic across working
directories; every documented rejection has a stable class; the adapter table reports platform, source-identity kind and per-capability support; unknown platforms are never guessed.
usage: mod_project_test.py <segarecomp-cli> <platforms-dir> <C compiler>"""
import json
import os
import sys
import zipfile

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
SDK, CC = os.path.abspath(sys.argv[2]), sys.argv[3]
tmp = temp()
REV_B = sha(b"synthetic-source-image-B")

LUAU = b"-- hello"
NATIVE = b'#include <segarecomp_mod_genesis.h>\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n'
DATA = b"\x01\x02\x03\x04"

FULL = f'''# a complete Genesis project
[mod]
id = "demo-mod"
version = "1.2.3"
name = "Demo Mod"
author = "Tester"
license = "MIT"
purpose = "demo"

[target]
platform = "genesis"

[[target.revision]]
label = "rev-a"
sha256 = "{SRC_SHA}"

[[target.revision]]
label = "rev-b"
sha256 = "{REV_B}"

[capabilities]
required = ["state.mod", "host.log"]
optional = ["input.read"]

[abi]
native = 2

[state]
schema = 3

[presentation]
widescreen = "original"

[[source]]
kind = "native-c11"
path = "src/provider.c"

[[source]]
kind = "luau-source"
path = "scripts/main.luau"

[[source]]
kind = "asset"
path = "assets/table.bin"
asset_class = "data"
format = "raw"
'''


def project(name, toml=FULL, files=None):
    root = tmp / name
    write(root / "mod.toml", toml)
    for path, data in (files if files is not None else {"src/provider.c": NATIVE, "scripts/main.luau": LUAU, "assets/table.bin": DATA}).items():
        write(root / path, data)
    return root


def expect(what, args, code, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got {doc and (doc.get('class'), doc.get('error'))}")


def expect_ok(what, args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got {doc} {p.stderr[-200:]}")
    return doc


# ------------------------------------------------------------------ init / check
init = tmp / "init-proj"
for platform in ("genesis", "master-system"):
    d = tmp / f"init-{platform}"
    expect_ok(f"init {platform}", ("mod", "init", d, "--platform", platform, "--id", "fresh-mod"))
    expect_ok(f"the {platform} template checks", ("mod", "check", d))
expect("init refuses to overwrite", ("mod", "init", tmp / "init-genesis", "--platform", "genesis", "--id", "x"), "E_ARCHIVE_ENTRY")
expect("init unknown platform", ("mod", "init", tmp / "init-bad", "--platform", "saturn", "--id", "x"), "E_PLATFORM_UNKNOWN")

# ------------------------------------------------------------------ project -> manifest is exactly the documented mapping (independent rebuild)
root = project("full")
built = tmp / "full.segapatch"
doc = expect_ok("build the full project (host compile of the native unit)", ("mod", "build", root, "--out", built, "--cc", CC, "--sdk-dir", SDK))
expected = {
    "format": "segarecomp-patch", "schema_version": 3, "id": "demo-mod", "version": "1.2.3", "name": "Demo Mod", "platform": "genesis", "author": "Tester", "license": "MIT", "purpose": "demo",
    "source": {"kind": "cartridge-image", "revisions": [{"label": "rev-a", "sha256": SRC_SHA}, {"label": "rev-b", "sha256": REV_B}]},
    "capabilities": ["host.log", "state.mod"], "capabilities_optional": ["input.read"], "abi": {"native": 2}, "state_schema": 3, "presentation_defaults": {"widescreen": "original"},
    "payloads": [payload("asset", "assets/table.bin", DATA, asset_class="data", format="raw"), payload("luau-source", "scripts/main.luau", LUAU, role="script"),
                 payload("native-c11", "src/provider.c", NATIVE, role="provider")],
}
with zipfile.ZipFile(built) as z:
    names = [i.filename for i in z.infolist()]
    manifest_bytes = z.read("manifest.json")
    cli.check(manifest_bytes == canon(expected), f"the manifest is exactly the documented mapping of the project:\n{manifest_bytes}\n{canon(expected)}")
    cli.check(names == ["manifest.json", "assets/table.bin", "scripts/main.luau", "src/provider.c"], f"canonical entry order (manifest first, then path bytes): {names}")
    for info in z.infolist():
        cli.check(info.compress_type == zipfile.ZIP_STORED and info.date_time == (1980, 1, 1, 0, 0, 0) and not info.extra and not info.comment and info.flag_bits == 0 and info.create_version == 20
                  and info.external_attr == 0, f"canonical entry attributes of {info.filename}")
cli.check(doc["package_identity"] == sha(manifest_bytes), "the reported identity is SHA-256 of the manifest bytes")

# round trip: project -> manifest -> (inspect) -> project fields equal
rt = expect_ok("inspect the built package", ("mod", "inspect", built))
pj = expect_ok("inspect the project directory", ("mod", "inspect", root))
for key in ("id", "version", "name", "platform", "source_identity_kind", "revisions", "trust_class", "capabilities", "capabilities_optional", "abi_native", "payloads", "package_identity", "content_digest"):
    cli.check(rt[key] == pj[key], f"round trip: package and project agree on {key}")
cli.check(rt["revisions"] == [{"label": "rev-a", "sha256": SRC_SHA}, {"label": "rev-b", "sha256": REV_B}], "multiple revisions by exact digest with labels")
cli.check(rt["capabilities"] == ["host.log", "state.mod"] and rt["capabilities_optional"] == ["input.read"], "capabilities are canonicalised (sorted)")

# determinism: a different working directory / path spelling / location yields identical bytes
copy_root = project("elsewhere/nested/full-copy")
a = tmp / "a.segapatch"; b = tmp / "b.segapatch"; c = tmp / "c.segapatch"
expect_ok("build from the project's parent with a relative path", ("mod", "build", "full", "--out", a, "--cc", CC, "--sdk-dir", SDK), cwd=tmp)
expect_ok("build a copy at another absolute path", ("mod", "build", copy_root, "--out", b, "--cc", CC, "--sdk-dir", SDK), cwd=tmp / "elsewhere")
expect_ok("build again", ("mod", "build", root, "--out", c, "--cc", CC, "--sdk-dir", SDK))
cli.check(a.read_bytes() == b.read_bytes() == c.read_bytes() == built.read_bytes(), "two loads on different working directories / locations yield byte-identical packages")
cli.check(str(tmp).encode() not in built.read_bytes(), "no absolute path enters the package")

# ------------------------------------------------------------------ rejections
def broken(name, edit, code, files=None):
    expect(name, ("mod", "check", project(name.replace(" ", "-"), edit(FULL), files)), code)


broken("relative path escape", lambda t: t.replace('path = "scripts/main.luau"', 'path = "../scripts/main.luau"'), "E_ARCHIVE_PATH")
broken("absolute path", lambda t: t.replace('path = "scripts/main.luau"', 'path = "/etc/passwd"'), "E_ARCHIVE_PATH")
broken("backslash path", lambda t: t.replace('path = "scripts/main.luau"', 'path = "scripts\\\\main.luau"'), "E_ARCHIVE_PATH")
broken("upper-case path", lambda t: t.replace('path = "scripts/main.luau"', 'path = "Scripts/Main.luau"'), "E_ARCHIVE_PATH")
broken("hidden path", lambda t: t.replace('path = "scripts/main.luau"', 'path = ".git/config"'), "E_ARCHIVE_PATH")
broken("missing target digest (no revision)", lambda t: t[:t.index("[[target.revision]]")] + t[t.index("[capabilities]"):], "E_SOURCE_DIGEST")
broken("missing target digest (revision without sha256)", lambda t: t.replace(f'sha256 = "{SRC_SHA}"\n', "", 1), "E_SOURCE_DIGEST")
broken("duplicate revision digest", lambda t: t.replace(REV_B, SRC_SHA), "E_SOURCE_DIGEST")
broken("duplicate revision label", lambda t: t.replace('label = "rev-b"', 'label = "rev-a"'), "E_SOURCE_DIGEST")
broken("unknown capability", lambda t: t.replace('"input.read"', '"net.connect"'), "E_CAPABILITY_UNKNOWN")
broken("duplicate capability", lambda t: t.replace('required = ["state.mod", "host.log"]', 'required = ["host.log", "host.log"]'), "E_FIELD_VALUE")
broken("unknown key in [mod]", lambda t: t.replace('purpose = "demo"', 'purpose = "demo"\nbuild_script = "make"'), "E_FIELD_UNKNOWN")
broken("a command field", lambda t: t.replace('kind = "luau-source"', 'kind = "luau-source"\ncommand = "curl x | sh"'), "E_FIELD_UNKNOWN")
broken("a download field", lambda t: t.replace('kind = "luau-source"', 'kind = "luau-source"\nurl = "https://example.invalid/x"'), "E_FIELD_UNKNOWN")
broken("a compiler flag field", lambda t: t.replace('[abi]', '[build]\nflags = "-O3"\n\n[abi]'), "E_FIELD_UNKNOWN")
broken("unknown table", lambda t: t + "\n[hooks]\nx = 1\n", "E_FIELD_UNKNOWN")
broken("duplicate key", lambda t: t.replace('name = "Demo Mod"', 'name = "Demo Mod"\nname = "Other"'), "E_MANIFEST_SYNTAX")
broken("a single table written as an array", lambda t: t.replace("[abi]", "[[abi]]"), "E_MANIFEST_SYNTAX")
broken("a repeated [[mod]]", lambda t: t.replace("[mod]", "[[mod]]"), "E_MANIFEST_SYNTAX")
broken("a repeated table written as a plain table", lambda t: t.replace("[[source]]", "[source]", 1), "E_MANIFEST_SYNTAX")
broken("duplicate table", lambda t: t + "\n[mod]\nid = \"x\"\n", "E_MANIFEST_SYNTAX")
broken("key outside any table", lambda t: 'id = "x"\n' + t, "E_FIELD_UNKNOWN")
broken("malformed line", lambda t: t.replace('version = "1.2.3"', 'version 1.2.3'), "E_MANIFEST_SYNTAX")
broken("float is not in the subset", lambda t: t.replace("schema = 3", "schema = 3.5"), "E_MANIFEST_SYNTAX")
broken("missing [mod]", lambda t: t.replace("[mod]", "[modx]"), "E_FIELD_UNKNOWN")
broken("missing source file", lambda t: t, "E_ARCHIVE_ENTRY", files={"src/provider.c": NATIVE, "scripts/main.luau": LUAU})
broken("abi missing for a native source", lambda t: t.replace("[abi]\nnative = 2\n", ""), "E_TRUST_UNDECLARED")
broken("author missing for a native source", lambda t: t.replace('author = "Tester"\n', ""), "E_TRUST_UNDECLARED")
broken("native source over its cap", lambda t: t, "E_PAYLOAD_CAP", files={"src/provider.c": b"/" * ((256 << 10) + 1), "scripts/main.luau": LUAU, "assets/table.bin": DATA})
broken("luau source with bytecode", lambda t: t, "E_LUAU_BYTECODE", files={"src/provider.c": NATIVE, "scripts/main.luau": b"\x03bytecode\x00", "assets/table.bin": DATA})
broken("limits above policy", lambda t: t + "\n[limits]\nluau_memory_bytes = 9999999\n", "E_LIMIT_EXCEEDS_POLICY")
expect("no mod.toml", ("mod", "check", tmp / "does-not-exist"), "E_ARCHIVE_ENTRY")
try:
    linked = project("symlinked-source")
    (linked / "scripts" / "main.luau").unlink()
    os.symlink(str(root / "scripts" / "main.luau"), linked / "scripts" / "main.luau")
except (OSError, NotImplementedError, AttributeError):
    linked = None   # links are not creatable here (unprivileged Windows)
if linked is not None:
    expect("a source reached through a link is refused", ("mod", "check", linked), "E_ARCHIVE_ENTRY")

# geometry (SEG-139-T004, ADR 0302): the empty declaration still round-trips; an aspect range and [[scene]] tables are declared, sorted into canonical order and carried into the manifest
GEO = FULL.replace("[abi]", "[geometry]\nscenes = []\n\n[abi]")
gp = project("geo", GEO)
gout = tmp / "geo.segapatch"
expect_ok("an empty geometry declaration builds", ("mod", "build", gp, "--out", gout, "--cc", CC, "--sdk-dir", SDK))
with zipfile.ZipFile(gout) as z:
    cli.check(b'"geometry":{"scenes":[]}' in z.read("manifest.json"), "the empty geometry declaration is carried into the manifest")
expect_ok("the geometry package re-reads (round trip)", ("mod", "inspect", gout))
RANGE = FULL.replace("[abi]", '[geometry]\naspect_max = "21:9"\n\n[[scene]]\nid = "title"\ncomponents = ["scenery"]\n\n[[scene]]\nid = "stage-1"\ncomponents = ["scenery", "hud", "actors"]\n\n[abi]')
rp = project("geo-range", RANGE)
rout = tmp / "geo-range.segapatch"
expect_ok("a range with scenes builds", ("mod", "build", rp, "--out", rout, "--cc", CC, "--sdk-dir", SDK))
with zipfile.ZipFile(rout) as z:
    mtext = z.read("manifest.json")
    cli.check(b'"geometry":{"aspect_max":"21:9","scenes":[{"id":"stage-1","components":["actors","hud","scenery"]},{"id":"title","components":["scenery"]}]}' in mtext, "scenes and components are written in canonical (sorted) order")
rc, d, _ = cli.run("mod", "inspect", rout)
cli.check(rc == 0 and d["geometry"]["declaration"] == "max=21:9;scene=stage-1:actors+hud+scenery;scene=title:scenery", f"inspect shows the host text ({d.get('geometry')})")
broken("geometry with a scenes entry", lambda t: GEO.replace("scenes = []", 'scenes = ["main"]'), "E_FIELD_VALUE")
broken("geometry with another key", lambda t: GEO.replace("scenes = []", "width = 4"), "E_FIELD_UNKNOWN")
broken("geometry written as an array table", lambda t: GEO.replace("[geometry]", "[[geometry]]"), "E_MANIFEST_SYNTAX")
broken("a scene without a range", lambda t: RANGE.replace('aspect_max = "21:9"\n', ""), "E_GEOMETRY_ASPECT")
broken("an aspect outside 4:3 to 32:9", lambda t: RANGE.replace("21:9", "64:9"), "E_GEOMETRY_ASPECT")
broken("a duplicate scene id", lambda t: RANGE.replace('id = "title"', 'id = "stage-1"'), "E_GEOMETRY_SCENE")
broken("an unknown component", lambda t: RANGE.replace('["scenery"]', '["sky"]'), "E_GEOMETRY_SCENE")
broken("a scene written as a single table", lambda t: RANGE.replace("[[scene]]", "[scene]", 1), "E_MANIFEST_SYNTAX")
broken("a scene with another key", lambda t: RANGE.replace('id = "title"', 'id = "title"\nname = "x"'), "E_FIELD_UNKNOWN")

# target platform fields: platform-specific target fields (cartridge digest + mapper config; disc-image reserved)
SMS = FULL.split("[capabilities]")[0].replace('platform = "genesis"', 'platform = "master-system"').replace(f'sha256 = "{REV_B}"', f'sha256 = "{REV_B}"\nmapper = "rom_only"').replace(f'sha256 = "{SRC_SHA}"', f'sha256 = "{SRC_SHA}"\nmapper = "sega"')
expect_ok("a master-system project with mappers", ("mod", "check", project("sms-ok", SMS + '[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n', {"scripts/main.luau": LUAU})))
expect("master-system without mapper", ("mod", "check", project("sms-nomapper", SMS.replace('mapper = "sega"\n', "").replace('mapper = "rom_only"\n', "") + '[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n', {"scripts/main.luau": LUAU})), "E_FIELD_MISSING")
expect("mapper on genesis", ("mod", "check", project("gen-mapper", FULL.replace(f'sha256 = "{REV_B}"', f'sha256 = "{REV_B}"\nmapper = "sega"'))), "E_FIELD_UNKNOWN")
DISC = '''[mod]
id = "cd-mod"
version = "1.0.0"
name = "CD"

[target]
platform = "sega-cd"
disc_identity = "%s"

[[source]]
kind = "luau-source"
path = "scripts/main.luau"
''' % ("a" * 64)
expect("disc-image targets are reserved, parsed and rejected", ("mod", "check", project("cd-reserved", DISC, {"scripts/main.luau": LUAU})), "E_SOURCE_KIND_RESERVED")
expect("a sega-cd target cannot use a cartridge-image source", ("mod", "check", project("cd-cart", DISC.split("disc_identity")[0] + 'kind = "cartridge-image"\n\n[[target.revision]]\nlabel = "r"\nsha256 = "%s"\n\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n' % SRC_SHA, {"scripts/main.luau": LUAU})), "E_SOURCE_PLATFORM")
expect("unknown platform is never guessed", ("mod", "check", project("unk", FULL.replace('platform = "genesis"', 'platform = "saturn"'))), "E_PLATFORM_UNKNOWN")
expect("missing platform is never guessed", ("mod", "check", project("noplat", FULL.replace('platform = "genesis"\n', ""))), "E_FIELD_MISSING")

# ------------------------------------------------------------------ platform adapters (SEG-146-T002)
LUAU_PROJECT = '''[mod]
id = "adapt"
version = "1.0.0"
name = "Adapt"

[target]
platform = "%s"
%s
[capabilities]
required = ["%s"]

[[source]]
kind = "luau-source"
path = "scripts/main.luau"
'''
REV = f'\n[[target.revision]]\nlabel = "rev-a"\nsha256 = "{SRC_SHA}"\n%s'
sms_rev = REV % 'mapper = "sega"\n'
gen_rev = REV % ""
for platform, rev, cap in (("master-system", sms_rev, "frame.vdp"), ("master-system", sms_rev, "memory.write.z80.work_ram"), ("master-system", sms_rev, "frame.sms_vdp"),
                           ("genesis", gen_rev, "audio.host.play"), ("genesis", gen_rev, "frame.sms_vdp")):
    expect(f"{platform} with {cap}", ("mod", "check", project(f"cap-{platform}-{cap}", LUAU_PROJECT % (platform, rev, cap), {"scripts/main.luau": LUAU})), "E_CAPABILITY_UNSUPPORTED")
d = '[target]\nplatform = "sega-cd"\ndisc_identity = "%s"\n' % ("b" * 64)
expect("sega-cd with an unsupported capability", ("mod", "check", project("cap-scd", LUAU_PROJECT.replace('[target]\nplatform = "%s"\n%s', d).replace("%s", "frame.vdp"), {"scripts/main.luau": LUAU})), "E_CAPABILITY_UNSUPPORTED")
expect("sega-cd with a supported capability reaches the reserved source rejection", ("mod", "check", project("cap-scd-log", LUAU_PROJECT.replace('[target]\nplatform = "%s"\n%s', d).replace("%s", "host.log"), {"scripts/main.luau": LUAU})), "E_SOURCE_KIND_RESERVED")
expect_ok("genesis keeps its ABI 1 capability names", ("mod", "check", project("cap-gen", LUAU_PROJECT % ("genesis", gen_rev, "frame.vdp"), {"scripts/main.luau": LUAU})))
expect_ok("an optional unsupported capability is simply not granted", ("mod", "check", project("cap-opt", (LUAU_PROJECT % ("master-system", sms_rev, "host.log")) + '\n', {"scripts/main.luau": LUAU})))
optional = (LUAU_PROJECT % ("master-system", sms_rev, "host.log")).replace('required = ["host.log"]', 'required = ["host.log"]\noptional = ["frame.sms_vdp"]')
expect_ok("optional capability on SMS (known, unsupported) is accepted", ("mod", "check", project("cap-opt2", optional, {"scripts/main.luau": LUAU})))
expect("an unknown optional capability is refused", ("mod", "check", project("cap-opt3", optional.replace("frame.sms_vdp", "nope.nope"), {"scripts/main.luau": LUAU})), "E_CAPABILITY_UNKNOWN")

# `mod inspect --json`: platform, source-identity kind and capability support per platform
rc, adapters, _ = cli.run("mod", "inspect", "--json")
cli.check(rc == 0 and adapters["ok"], "mod inspect --json succeeds")
by = {a["platform"]: a for a in adapters["adapters"]}
cli.check(sorted(by) == ["genesis", "master-system", "sega-cd"], "all three adapters are registered")
cli.check(by["genesis"]["source_identity_kind"] == "cartridge-image" and by["master-system"]["source_identity_kind"] == "cartridge-image" and by["sega-cd"]["source_identity_kind"] == "disc-image", "source-identity kind per platform")
cli.check(by["genesis"]["capabilities"]["frame.vdp"] == "supported-with-limitations" and by["genesis"]["capabilities"]["hook.function"] == "experimental" and by["master-system"]["capabilities"]["hook.function"] == "unsupported", "genesis capability levels (hook.function is experimental from SEG-138-T004)")
cli.check(all(v == "unsupported" for k, v in by["master-system"]["capabilities"].items() if k != "host.log") and by["master-system"]["capabilities"]["host.log"] == "experimental", "master-system registers a mostly unsupported set")
cli.check(all(v == "unsupported" for k, v in by["sega-cd"]["capabilities"].items() if k != "host.log") and by["sega-cd"]["capabilities"]["host.log"] == "experimental", "sega-cd registers a mostly unsupported set")
cli.check(by["genesis"]["sdk_header"] == "segarecomp_mod_genesis.h" and by["master-system"]["sdk_header"] == "segarecomp_mod_sms.h" and by["sega-cd"]["sdk_header"] == "segarecomp_mod_scd.h", "per-platform SDK header names")
pj = expect_ok("inspect a project reports its platform adapter", ("mod", "inspect", project("inspect-gen", LUAU_PROJECT % ("genesis", gen_rev, "frame.vdp"), {"scripts/main.luau": LUAU})))
cli.check(pj["platform"] == "genesis" and pj["adapter"]["platform"] == "genesis" and pj["adapter"]["capabilities"]["frame.vdp"] == "supported-with-limitations", "inspect <project> carries the adapter report")

# Genesis golden output: a fixed genesis project packs to a pinned identity and package digest (changes here mean the Genesis path changed)
gold = project("golden", LUAU_PROJECT % ("genesis", gen_rev, "frame.vdp"), {"scripts/main.luau": LUAU})
gp = tmp / "golden.segapatch"
g = expect_ok("build the golden genesis project", ("mod", "build", gold, "--out", gp))
GOLDEN_IDENTITY, GOLDEN_PACKAGE = "8e56d7f849ada92976f1bc3b4287ff8addca49f35a1437ff0f54fb0cf8887872", "62e680e8179802323f58a6a6ab75da75b368d9b4e6c137396fdd3284929b74c5"
cli.check(g["package_identity"] == GOLDEN_IDENTITY, f"golden Genesis package identity is pinned: {g['package_identity']}")
cli.check(sha(gp.read_bytes()) == GOLDEN_PACKAGE, f"golden Genesis package bytes are pinned: {sha(gp.read_bytes())}")

cli.done("mod_project_test")
