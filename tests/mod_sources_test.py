#!/usr/bin/env python3
"""SEG-135-T003 + SEG-146-T003: bounded multi-file C11/Luau/asset sources and the typed per-platform ABI 2 feature tables.
(1) A two/three-file synthetic provider builds through `segarecomp mod build` (host-owned strict-C11 flags, deterministic order) and its identity changes with any file digest; include escapes are
rejected; Luau and assets are stored as opaque digest-listed payloads; (2) the per-platform headers compile as strict C11 with warnings, a synthetic provider per platform is compiled and LOADED
(negotiation harness), and a wrong platform/ABI/capability is rejected with a stable class. ABI 1 coexists unchanged.
usage: mod_sources_test.py <segarecomp-cli> <platforms-dir> <C compiler> <fixtures-dir> <ABI 1 sample provider.c>"""
import os
import subprocess
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
PLATFORMS, CC, FIXTURES, ABI1_SAMPLE = os.path.abspath(sys.argv[2]), sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5])
tmp = temp()
STRICT = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror"]

HEAD = '''[mod]
id = "src-mod"
version = "1.0.0"
name = "Sources"
author = "T"

[target]
platform = "%s"

[[target.revision]]
label = "rev-a"
sha256 = "%s"
%s
[abi]
native = 2
'''


def project(name, files, platform="genesis", natives=None, extra="", caps=""):
    root = tmp / name
    mapper = 'mapper = "sega"\n' if platform == "master-system" else ""
    toml = HEAD % (platform, SRC_SHA, mapper) + (caps and f"\n[capabilities]\nrequired = [{caps}]\n") + extra
    if platform == "sega-cd":   # a disc-image target (reserved): no revisions, a disc identity instead
        toml = toml.replace(f'[[target.revision]]\nlabel = "rev-a"\nsha256 = "{SRC_SHA}"\n', f'disc_identity = "{SRC_SHA}"\n')
    for path in natives if natives is not None else sorted(p for p in files if p.endswith((".c", ".h"))):
        toml += f'\n[[source]]\nkind = "native-c11"\npath = "{path}"\n'
    write(root / "mod.toml", toml)
    for path, data in files.items():
        write(root / path, data)
    return root


def build(root, name="out.segapatch", platform_dir=PLATFORMS):
    out = tmp / name
    return cli.run("mod", "build", root, "--out", out, "--cc", CC, "--sdk-dir", platform_dir)


def ok(what, root, name="out.segapatch"):
    rc, doc, p = build(root, name)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected a build, got {doc}")
    return doc


def refused(what, root, code, name="out.segapatch"):
    rc, doc, p = build(root, name)
    cli.check(rc == 1 and doc and doc.get("class") == code, f"{what}: expected {code}, got {doc and (doc.get('class'), (doc.get('error') or '')[:200])}")


PROVIDER = b'#include <segarecomp_mod_genesis.h>\n#include "src/util.h"\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return util_zero() ? (const SegaModProvider *)s : (const SegaModProvider *)0; }\n'
UTIL_H = b"int util_zero(void);\n"
UTIL_C = b'#include "src/util.h"\nint util_zero(void) { return 0; }\n'
FILES = {"src/provider.c": PROVIDER, "src/util.h": UTIL_H, "src/util.c": UTIL_C}

# ------------------------------------------------------------------ (1) multi-file sources
first = ok("a three-file synthetic provider builds", project("multi", FILES))
cli.check(first["payloads"]["native"] == 3 and first["trust_class"] == "native", "three native-c11 payloads, trust class native")
ids = {first["package_identity"]}
contents = {first["content_digest"]}
for victim in FILES:
    changed = dict(FILES)
    changed[victim] = FILES[victim] + b"\n/* changed */\n"
    doc = ok(f"changed {victim}", project("multi-" + victim.replace("/", "-"), changed))
    cli.check(doc["package_identity"] not in ids and doc["content_digest"] not in contents, f"identity and content digest change when {victim} changes")
    ids.add(doc["package_identity"]); contents.add(doc["content_digest"])
again = ok("rebuild is stable", project("multi-again", FILES))
cli.check(again["package_identity"] == first["package_identity"], "the same files give the same identity (deterministic)")

# include allowlist
def with_include(line):
    return {"src/provider.c": b'#include <segarecomp_mod_genesis.h>\n' + line + b'\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n', "src/util.h": UTIL_H}


for what, line in (("parent directory", b'#include "../secret.h"'), ("absolute path", b'#include "/etc/passwd"'), ("system header", b"#include <stdlib.h>"), ("nested system header", b"#include <sys/types.h>"),
                   ("a file that is not in the package", b'#include "src/missing.h"'), ("a bare local name", b'#include "util.h"'), ("computed include", b"#define H <stdlib.h>\n#include H"),
                   ("#import", b"#import <stdlib.h>"), ("#include_next", b"#include_next <stdlib.h>"), ("#embed", b'#embed "src/util.h"'), ("digraph", b"%:include <stdlib.h>"),
                   ("trigraph hash", b"??=include <stdlib.h>"), ("line continuation", b"#inc\\\nlude <stdlib.h>"), ("comment between # and include", b"# /* x */ include <stdlib.h>"),
                   ("include after a block comment", b"/* x */ #include <stdlib.h>"), ("lone CR splice", b"#inc\\\rlude <stdlib.h>"), ("splice with trailing space", b"#inc\\ \nlude <stdlib.h>"), ("form feed", b"#\x0cinclude <stdlib.h>"), ("vertical tab", b"#\x0binclude <stdlib.h>"), ("NUL byte", b"#\x00include <stdlib.h>"),
                   ("incbin", b'__asm__(".incbin \\"x\\"");'), ("_Pragma", b'_Pragma("GCC dependency \\"x\\"")'), ("windows backslash", b'#include "src\\\\util.h"'),
                   ("another platform's header", b"#include <segarecomp_mod_sms.h>"), ("the ABI 1 header", b"#include <segarecomp_genesis_patch.h>")):
    expected = "E_ABI_PLATFORM" if "platform's header" in what else "E_ABI_MIXED" if "ABI 1" in what else "E_INCLUDE_FORBIDDEN"
    refused(f"include escape: {what}", project("inc-" + what.replace(" ", "-").replace("#", "").replace("'", ""), with_include(line)), expected)
ok("allowlisted includes (SDK headers, <stdint.h>, <stddef.h>, a package-local file)", project("inc-ok", {"src/provider.c": b'#include <stdint.h>\n#include <stddef.h>\n#include <segarecomp_mod.h>\n#include <segarecomp_mod_genesis.h>\n#include "src/util.h"\n'
                                                           b'const SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; (void)util_zero; return 0; }\n', "src/util.h": UTIL_H, "src/util.c": UTIL_C}))

# the host compiler is host-owned: strict flags catch warnings; a failing unit is reported with the first failing path
refused("a warning is an error", project("warn", {"src/a.c": b"#include <segarecomp_mod_genesis.h>\nstatic int unused_function(void) { return 1; }\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n"}), "E_COMPILE_FAILED")
rc, doc, _ = build(project("order", {"src/b.c": b"int b(void) { return ; }\n", "src/a.c": b"int a(void) { return ; }\n"}), "order.segapatch")
cli.check(rc == 1 and doc["class"] == "E_COMPILE_FAILED" and doc["error"].startswith("src/a.c"), f"units are compiled in path order ({doc})")
# link validation: every unit is kept and linked together with a probe that requires segarecomp_mod_provider (linked, never run)
ENTRY = b'#include <segarecomp_mod_genesis.h>\n'
refused("an unresolved cross-unit reference", project("link-unres", {"src/provider.c": ENTRY + b"int missing_elsewhere(void);\nconst SegaModProvider *segarecomp_mod_provider(void **s) { return missing_elsewhere() ? (const SegaModProvider *)s : (const SegaModProvider *)0; }\n"}), "E_LINK_FAILED")
refused("a symbol defined in two units", project("link-dup", {"src/provider.c": ENTRY + b"int dup(void) { return 1; }\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n", "src/other.c": b"int dup(void) { return 2; }\n"}), "E_LINK_FAILED")
refused("two entry symbols", project("link-twoentry", {"src/provider.c": ENTRY + b"const SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n", "src/other.c": ENTRY + b"const SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n"}), "E_LINK_FAILED")
refused("no provider entry point", project("link-noentry", {"src/provider.c": ENTRY + b"int helper(void) { return 0; }\n"}), "E_ABI_ENTRY_MISSING")
ok("a cross-unit reference that resolves links", project("link-ok", {"src/provider.c": ENTRY + b"int helper(void);\nconst SegaModProvider *segarecomp_mod_provider(void **s) { return helper() ? (const SegaModProvider *)s : (const SegaModProvider *)0; }\n", "src/helper.c": b"int helper(void) { return 0; }\n"}))
refused("header-only native package", project("hdr", {"src/util.h": UTIL_H}), "E_FIELD_VALUE")
rc, doc, _ = cli.run("mod", "build", project("nocc", FILES), "--out", tmp / "nocc.segapatch")
cli.check(rc == 1 and doc["class"] == "E_COMPILE_FAILED", "a native package cannot be built without a compiler and SDK directory (no silent skip)")
cli.check(not (tmp / "nocc.segapatch").exists(), "a failed build leaves no package behind")
many = {f"src/f{i:02d}.c": b"int f%d(void) { return 0; }\n" % i for i in range(65)}
refused("65 native files", project("many", many), "E_PAYLOAD_CAP")

# Luau and assets are stored as opaque, digest-listed payloads (never compiled or executed here)
mixed_files = {"src/provider.c": b'#include <segarecomp_mod_genesis.h>\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n', "scripts/a.luau": b"this is not valid luau at all ((( \n",
               "assets/img.bin": bytes(range(64)) * 4, "assets/pic.rgba": b"\x00" * (2 * 2 * 4)}
MIXED_EXTRA = '''
[[source]]
kind = "luau-source"
path = "scripts/a.luau"

[[source]]
kind = "asset"
path = "assets/img.bin"
asset_class = "data"
format = "raw"

[[source]]
kind = "asset"
path = "assets/pic.rgba"
asset_class = "image"
width = 2
height = 2
format = "rgba8"
'''
mixed = project("mixed", mixed_files, natives=["src/provider.c"], extra=MIXED_EXTRA)
doc = ok("native + Luau + assets in one package", mixed)
cli.check(doc["payloads"] == {"native": 1, "luau": 1, "assets": 2, "other": 0}, f"payload counts ({doc['payloads']})")
refused("image asset with the wrong size", project("badimg", {**mixed_files, "assets/pic.rgba": b"\x00" * 7}, natives=["src/provider.c"], extra=MIXED_EXTRA), "E_ARCHIVE_SIZE")

# ABI 1 coexists unchanged: the ABI 1 sample still compiles against its own header, and the two headers share no name
r = subprocess.run([CC, *STRICT, "-I", f"{PLATFORMS}/genesis/sdk", "-c", str(ABI1_SAMPLE), "-o", str(tmp / "abi1.o")], capture_output=True, text=True)
cli.check(r.returncode == 0, f"the ABI 1 sample provider compiles unchanged against platforms/genesis/sdk: {r.stderr[-300:]}")
both = tmp / "both.c"
both.write_text('#include <segarecomp_genesis_patch.h>\n#include <segarecomp_mod_genesis.h>\nint main(void) { return 0; }\n')
r = subprocess.run([CC, *STRICT, "-I", f"{PLATFORMS}/genesis/sdk", "-I", f"{PLATFORMS}/sdk", "-c", str(both), "-o", str(tmp / "both.o")], capture_output=True, text=True)
cli.check(r.returncode == 0, f"ABI 1 and ABI 2 headers coexist in one translation unit: {r.stderr[-300:]}")

# ------------------------------------------------------------------ (2) typed per-platform feature tables
platforms = {"genesis": ("genesis", "segarecomp_mod_genesis.h", "SegaGenesisFeatures", 1, "provider_genesis.c"),
             "master-system": ("master-system", "segarecomp_mod_sms.h", "SegaMasterSystemFeatures", 2, "provider_sms.c"),
             "sega-cd": ("sega-cd", "segarecomp_mod_scd.h", "SegaCdFeatures", 3, "provider_scd.c")}
for platform, (directory, header, features, pid, provider) in platforms.items():
    inc = ["-I", f"{PLATFORMS}/sdk", "-I", f"{PLATFORMS}/{directory}/sdk"]
    hc = tmp / f"hdr-{pid}.c"
    hc.write_text(f"#include <{header}>\nint main(void) {{ {features} f; return (int)sizeof f - (int)sizeof f; }}\n")
    r = subprocess.run([CC, *STRICT, "-Wconversion", "-Wshadow", "-DSEGA_MOD_TARGET_PLATFORM=%d" % pid, "-DSEGA_MOD_TARGET_ABI_MAJOR=2U", *inc, "-c", str(hc), "-o", str(tmp / f"hdr-{pid}.o")], capture_output=True, text=True)
    cli.check(r.returncode == 0, f"{header} compiles as strict C11 with warnings: {r.stderr[-300:]}")
    # synthetic provider compiled and loaded through the negotiation harness
    exe = tmp / f"negotiate-{pid}"
    r = subprocess.run([CC, *STRICT, "-Wconversion", f"-DHARNESS_HEADER=\"{header}\"", f"-DHARNESS_FEATURES={features}", f"-DHARNESS_PLATFORM_ID={pid}", f"-DSEGA_MOD_TARGET_PLATFORM={pid}",
                        "-DSEGA_MOD_TARGET_ABI_MAJOR=2U", *inc, str(FIXTURES / "negotiate_harness.c"), str(FIXTURES / provider), "-o", str(exe)], capture_output=True, text=True)
    cli.check(r.returncode == 0, f"{platform}: synthetic provider + harness compile as strict C11: {r.stderr[-500:]}")
    if r.returncode == 0:
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        cli.check(run.returncode == 0 and run.stdout.strip() == "ok", f"{platform}: negotiation harness: {run.stdout}")
    # the same provider builds through the CLI for its platform (no behaviour: lifecycle and capability reporting only)
    src = (FIXTURES / provider).read_bytes()
    if platform == "sega-cd":   # compiled and negotiated above; packaging a disc target stays reserved until SEG-148
        refused("sega-cd: the disc-image source is reserved", project(f"sdk-{pid}", {"src/provider.c": src}, platform=platform, caps='"host.log"'), "E_SOURCE_KIND_RESERVED")
    else:
        doc = ok(f"{platform}: provider builds through mod build", project(f"sdk-{pid}", {"src/provider.c": src}, platform=platform, caps='"host.log"'))
        cli.check(doc["platform"] == platform and doc["abi_native"] == 2, f"{platform}: package declares platform and ABI 2")
    # compile-time pinning: the host's -D flags make a provider written for another platform / ABI fail with the stable class in the message
    for flag, text, klass in ((f"-DSEGA_MOD_TARGET_PLATFORM={pid % 3 + 1}", "other platform", "E_ABI_PLATFORM"), ("-DSEGA_MOD_TARGET_ABI_MAJOR=3U", "ABI 3", "E_ABI_HEADER_MISMATCH")):
        r = subprocess.run([CC, *STRICT, flag, *inc, "-c", str(FIXTURES / provider), "-o", str(tmp / "x.o")], capture_output=True, text=True)
        cli.check(r.returncode != 0 and klass in r.stderr, f"{platform}: a provider built against {text} fails with {klass}")

# wrong platform / missing capability are rejected at build with stable classes
sms_for_genesis = (FIXTURES / "provider_sms.c").read_bytes()
refused("an SMS provider in a genesis package", project("wrong-platform", {"src/provider.c": sms_for_genesis}), "E_ABI_PLATFORM")
refused("a genesis provider in a master-system package", project("wrong-platform2", {"src/provider.c": (FIXTURES / "provider_genesis.c").read_bytes()}, platform="master-system"), "E_ABI_PLATFORM")
refused("master-system declaring a capability it lacks", project("sms-cap", {"src/provider.c": sms_for_genesis}, platform="master-system", caps='"frame.vdp"'), "E_CAPABILITY_UNSUPPORTED")
refused("master-system declaring memory.write", project("sms-cap2", {"src/provider.c": sms_for_genesis}, platform="master-system", caps='"memory.write.z80.work_ram"'), "E_CAPABILITY_UNSUPPORTED")
def with_abi(name, value):
    root = project(name, {"src/provider.c": (FIXTURES / "provider_genesis.c").read_bytes()})
    write(root / "mod.toml", (root / "mod.toml").read_text().replace("native = 2", f"native = {value}"))
    return root


refused("a package declaring ABI 3", with_abi("abi3", 3), "E_ABI_UNSUPPORTED")
refused("a package declaring ABI 1", with_abi("abi1", 1), "E_FIELD_VALUE")

cli.done("mod_sources_test")
