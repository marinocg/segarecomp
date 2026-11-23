#!/usr/bin/env python3
"""SEG-152-T003 (ADR 0320 decision 10): the additive manifest field `abi.minor`, the header-level minor guard and the required-only rule of `frame.vdp`.
usage: segapatch_abi_minor_test.py <segarecomp-cli> <cc> <PROJECT_SOURCE_DIR>
Checks: `abi.minor` 1 is accepted (inspect reports it, the package identity covers it), 0 and above-the-release values are refused (E_FIELD_VALUE / E_ABI_MINOR_TOO_NEW), a reader
that does not know the key refuses it with E_FIELD_UNKNOWN (simulated by the strict key set: an unknown sibling key is refused with the same class), an older manifest
(no minor) keeps its pinned identity, an optional `frame.vdp` is E_CAPABILITY_INCONSISTENT, and the SDK header hides the minor-1 members for a source that targets minor 0 and refuses
a minor above its own with E_ABI_MINOR_TOO_NEW. Synthetic data only."""
import subprocess
import sys
import tempfile
from pathlib import Path

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
cc = sys.argv[2]
root = Path(sys.argv[3])
tmp = temp()


def inspect(manifest, payloads):
    path = write(tmp / "p.segapatch", build(manifest, payloads))
    rc, doc, _ = cli.run("mod", "inspect", path)
    return rc, doc


def expect_ok(what, manifest, payloads):
    rc, doc = inspect(manifest, payloads)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: accepted ({doc})")
    return doc


def expect_class(what, manifest, payloads, code):
    rc, doc = inspect(manifest, payloads)
    cli.check(rc == 1 and doc and doc.get("class") == code, f"{what}: expected {code}, got {doc and doc.get('class')}")


nat = native()
head = {"author": "A"}
base = base_manifest("genesis", [nat], head=head, abi={"native": 2})
old = expect_ok("an ABI 2 manifest without minor (an older provider)", base, [nat])
cli.check(old["package_identity"] == sha(canon(base)), "the identity of a manifest without minor is the digest of its exact bytes (canonical bytes unchanged)")
v1 = base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": 1})
doc = expect_ok("abi.minor 1", v1, [nat])
cli.check(doc["package_identity"] == sha(canon(v1)) and doc["package_identity"] != old["package_identity"], "the declared minor is part of the package identity")
expect_class("abi.minor 0 (omitted for minor 0)", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": 0}), [nat], "E_FIELD_VALUE")
expect_class("abi.minor newer than the release (E_ABI_MINOR_TOO_NEW)", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": 2}), [nat], "E_ABI_MINOR_TOO_NEW")
expect_class("a reader that does not know a key refuses it (the strict set that older products apply to `minor`)", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor_next": 1}), [nat], "E_FIELD_UNKNOWN")
expect_class("abi.minor must be an integer", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": "1"}), [nat], "E_FIELD_VALUE")

# frame.vdp selects the callback timing: required is fine, optional is inconsistent (it could silently change timing between hosts)
expect_ok("frame.vdp required", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": 1}, capabilities=["frame.vdp", "host.log"]), [nat])
expect_class("frame.vdp optional", base_manifest("genesis", [nat], head=head, abi={"native": 2, "minor": 1}, capabilities=["host.log"], capabilities_optional=["frame.vdp"]), [nat], "E_CAPABILITY_INCONSISTENT")

# the header guard: a source targeting minor 0 does not see the minor-1 members; minor 1 sees them; a minor above the header's is a stable #error
sdk = [str(root / "platforms" / "sdk"), str(root / "platforms" / "genesis" / "sdk")]
src_use = '#include <segarecomp_mod_genesis.h>\nconst void *use(const SegaGenesisFeatures *f) { return f->frame; }\n'
src_plain = '#include <segarecomp_mod_genesis.h>\nunsigned size(void) { return (unsigned)sizeof(SegaGenesisFeatures); }\n'


def compile_source(source, minor):
    d = Path(tempfile.mkdtemp(dir=tmp))
    c = write(d / "p.c", source)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-c", "-o", str(d / "p.o"), "-DSEGA_MOD_TARGET_PLATFORM=1", "-DSEGA_MOD_TARGET_ABI_MAJOR=2U"]
    if minor is not None:
        flags.append(f"-DSEGA_MOD_TARGET_ABI_MINOR={minor}U")
    p = subprocess.run([cc, *flags, *[f"-I{i}" for i in sdk], str(c)], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stderr


rc, err = compile_source(src_use, 1)
cli.check(rc == 0, f"a source targeting minor 1 sees SegaGenesisFeatures::frame ({err[:200]})")
rc, err = compile_source(src_use, 0)
cli.check(rc != 0 and "frame" in err, "a source targeting minor 0 does not see the minor-1 members (a readable compile error, not a silent change)")
# SEG-152-T005/T006: the appended render-API members (bitmap, layer bitmap/clip, fallback, decline) follow the same minor-1 rule; the table keeps its size for a source that declares no minor
for member in ("overlay_bitmap", "overlay_layer_bitmap", "overlay_layer_clip", "overlay_fallback", "decline"):
    use = f'#include <segarecomp_mod_genesis.h>\nconst void *use(const SegaGenesisRenderApi *r) {{ return (const void *)r->{member}; }}\n'
    rc, err = compile_source(use, 1)
    cli.check(rc == 0, f"a source targeting minor 1 sees SegaGenesisRenderApi::{member} ({err[:200]})")
    rc, err = compile_source(use, 0)
    cli.check(rc != 0 and member in err, f"a source targeting minor 0 does not see SegaGenesisRenderApi::{member}")
size_src = '#include <segarecomp_mod_genesis.h>\nunsigned size(void) { return (unsigned)sizeof(SegaGenesisRenderApi); }\n'
cli.check(compile_source(size_src, 0)[0] == 0 and compile_source(size_src, 1)[0] == 0, "SegaGenesisRenderApi compiles at both minors (the minor-0 placeholder keeps the layout)")
rc, err = compile_source(src_plain, 0)
cli.check(rc == 0, "a source targeting minor 0 still compiles against the extended header and the table size is unchanged for it")
rc, err = compile_source(src_plain, 2)
cli.check(rc != 0 and "E_ABI_MINOR_TOO_NEW" in err, "a source targeting a minor above the header's fails with E_ABI_MINOR_TOO_NEW")
rc, err = compile_source(src_use, None)
cli.check(rc == 0, "a hand-built source that declares no target minor sees the whole table (the host build always declares it)")

# the host's own diagnostic for a source that uses a minor-1 member without declaring the minor: a stable class naming abi.minor (a struct member cannot carry a custom #error, ADR 0320 decision 10 as amended)
HEAD = f'''[mod]
id = "minor-diag"
author = "segarecomp tests"
version = "0.1.0"
name = "Minor diagnostic"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "rev-a"
sha256 = "{SRC_SHA}"

[[source]]
kind = "native-c11"
path = "src/provider.c"

[abi]
native = 2
{{minor}}
[capabilities]
required = ["frame.vdp", "frame.work_ram"]
'''
PROVIDER = b'#include <segarecomp_mod_genesis.h>\nstatic const SegaModProvider p = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 1U, SEGA_MOD_PLATFORM, "d", 1U, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};\nconst SegaModProvider *segarecomp_mod_provider(void **s) { const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)0; if (s != 0 && f != 0 && f->frame != 0 && f->vdp != 0) return 0; return &p; }\n'


def build_project(name, minor_line):
    d = tmp / name
    write(d / "mod.toml", HEAD.replace("{minor}", minor_line))
    write(d / "src/provider.c", PROVIDER)
    return cli.run("mod", "build", d, "--out", tmp / (name + ".segapatch"), "--cc", cc, "--sdk-dir", root / "platforms")


rc, doc, _ = build_project("undeclared", "")
cli.check(rc == 1 and doc and doc.get("class") == "E_ABI_MINOR_UNDECLARED" and "abi.minor" in (doc.get("error") or ""), f"a source using frame/vdp without abi.minor is E_ABI_MINOR_UNDECLARED naming abi.minor: {doc and (doc.get('class'), (doc.get('error') or '')[:160])}")
rc, doc, _ = build_project("declared", "minor = 1")
cli.check(rc == 0 and doc and doc.get("ok") is True, f"the same source with minor = 1 builds ({doc and (doc.get('class'), (doc.get('error') or '')[:200])})")
rc, doc, _ = build_project("too-new", "minor = 2")
cli.check(rc == 1 and doc and doc.get("class") == "E_ABI_MINOR_TOO_NEW", f"minor = 2 is E_ABI_MINOR_TOO_NEW from the project reader: {doc and doc.get('class')}")

cli.done("segapatch_abi_minor_test")
