#!/usr/bin/env python3
"""SEG-138-T004 (ADR 0298): the function-hook declaration model through the product CLI (`segarecomp mod check|pack|inspect|install|variant`). A project declares hooks in `[[hook]]` tables
(before | after | before-after | replace, with a replacement contract) on VERIFIED per-revision symbols; the package carries them canonically in `hooks`; every declaration-level rejection has a stable
class; the hook identity (ids, addresses, modes, register masks, costs) is part of the package identity, the content digest and the variant key. The expected manifest is rebuilt here independently.
Synthetic data only. usage: mod_hooks_test.py <segarecomp-cli> <platforms-dir> <C compiler>"""
import json
import os
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
SDK, CC = os.path.abspath(sys.argv[2]), sys.argv[3]
tmp = temp()
IMG_A, IMG_B = b"hook-image-A", b"hook-image-B"
SHA_A, SHA_B = sha(IMG_A), sha(IMG_B)
rom_a = write(tmp / "rom_a.bin", IMG_A)
NATIVE = b'#include <segarecomp_mod_genesis.h>\nconst SegaModProvider *segarecomp_mod_provider(void **s) { (void)s; return 0; }\n'

HEAD = f'''[mod]
id = "hook-mod"
version = "1.0.0"
name = "Hook Mod"
author = "Tester"
license = "MIT"

[target]
platform = "%(platform)s"

[[target.revision]]
label = "rev-a"
sha256 = "{SHA_A}"

[[target.revision]]
label = "rev-b"
sha256 = "{SHA_B}"

[capabilities]
required = ["hook.function", "host.log"]

[abi]
native = 2

[[source]]
kind = "native-c11"
path = "src/provider.c"
'''
SYMBOLS = '''
[[symbol]]
name = "fn-a"
revision = "rev-a"
address = "0x00000220"
provenance = "verified"

[[symbol]]
name = "fn-a"
revision = "rev-b"
address = "0x00000320"
provenance = "verified"

[[symbol]]
name = "fn-b"
revision = "rev-a"
address = "0x00000240"
provenance = "verified"

[[symbol]]
name = "fn-b"
revision = "rev-b"
address = "0x00000340"
provenance = "verified"
'''
HOOKS = '''
[[hook]]
id = "b-replace"
symbol = "fn-b"
mode = "replace"
reads = ["d0", "a7", "stack0"]
writes = ["d0", "ccr"]
cost = 24

[[hook]]
id = "a-watch"
symbol = "fn-a"
mode = "before-after"
reads = ["d0", "d1", "cycles"]
'''
_n = [0]


def project(text, name=None, platform="genesis"):
    _n[0] += 1
    d = tmp / (name or f"p{_n[0]}")
    write(d / "mod.toml", text.replace("%(platform)s", platform))
    write(d / "src" / "provider.c", NATIVE)
    return d


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


# ------------------------------------------------------------------ a valid project: canonical manifest, round trip, identity
good = project(HEAD + SYMBOLS + HOOKS, "good")
doc = ok("check", "mod", "check", good)
built = tmp / "good.segapatch"
ok("pack", "mod", "pack", good, "--out", built)
import zipfile
manifest = json.loads(zipfile.ZipFile(built).read("manifest.json"))
want_hooks = [
    {"id": "a-watch", "symbol": "fn-a", "cpu": "m68k", "mode": "before-after", "reads": ["d0", "d1", "cycles"]},
    {"id": "b-replace", "symbol": "fn-b", "cpu": "m68k", "mode": "replace", "reads": ["d0", "a7", "stack0"], "replace": {"writes": ["d0", "ccr"], "cost": 24}},
]
cli.check(manifest["hooks"] == want_hooks, f"canonical hooks (sorted by id, registers in view order): {manifest['hooks']}")
raw = zipfile.ZipFile(built).read("manifest.json")
cli.check(raw == canon(manifest), "the manifest is written in canonical form (no whitespace, insertion order)")
cli.check([k for k in manifest["hooks"][1]] == ["id", "symbol", "cpu", "mode", "reads", "replace"], "canonical key order of a hook")
ins = ok("inspect the package", "mod", "inspect", built)
pro = ok("inspect the project", "mod", "inspect", good)
cli.check(ins["hooks"] == pro["hooks"] and [h["id"] for h in ins["hooks"]] == ["a-watch", "b-replace"], f"inspect shows the hooks, project and package agree: {ins.get('hooks')}")
cli.check(ins["hooks"][1]["writes"] == ["d0", "ccr"] and ins["hooks"][1]["cost"] == 24, "inspect shows the replacement contract")
cli.check(ins["package_identity"] == pro["package_identity"], "project and package identity agree")
again = tmp / "again.segapatch"
ok("pack again", "mod", "pack", project(HEAD + SYMBOLS + HOOKS, "good2"), "--out", again)
cli.check(again.read_bytes() == built.read_bytes(), "the same declaration packs to the same bytes (deterministic)")
reordered = HOOKS.split("[[hook]]")  # ['', b-replace, a-watch]
reorder_text = HEAD + SYMBOLS + "[[hook]]" + reordered[2] + "[[hook]]" + reordered[1]
other = tmp / "reordered.segapatch"
ok("pack with the declarations in another source order", "mod", "pack", project(reorder_text, "reordered"), "--out", other)
cli.check(other.read_bytes() == built.read_bytes(), "declaration order in mod.toml does not change the package")

# ------------------------------------------------------------------ identity: any hook field changes the package identity and the content digest
base_id, base_content = pro["package_identity"], pro["content_digest"]
for what, mutate in (("cost", lambda t: t.replace("cost = 24", "cost = 28")), ("mode", lambda t: t.replace('mode = "before-after"', 'mode = "before"')),
                     ("reads", lambda t: t.replace('"d0", "d1", "cycles"', '"d0", "d1"')), ("writes", lambda t: t.replace('writes = ["d0", "ccr"]', 'writes = ["d0", "d1", "ccr"]')),
                     ("symbol", lambda t: t.replace('symbol = "fn-b"', 'symbol = "fn-a"\nrevision = "rev-a"').replace('"fn-b"', '"fn-b"')),
                     ("id", lambda t: t.replace('id = "a-watch"', 'id = "a-watch2"'))):
    text = mutate(HEAD + SYMBOLS + HOOKS)
    rc, d, _ = cli.run("mod", "inspect", project(text))
    if rc == 0:
        cli.check(d["package_identity"] != base_id and d["content_digest"] != base_content, f"changing the hook {what} changes the package identity and the content digest")
    else:
        cli.check(what == "symbol", f"unexpected rejection while mutating {what}: {d}")
# a package without hooks keeps its exact pre-hook digest rule (no `hook` line without hooks)
plain = ok("project without hooks", "mod", "inspect", project(HEAD + SYMBOLS, "plain"))
cli.check("hooks" not in plain, "a package without hooks reports none")

# ------------------------------------------------------------------ rejections: stable classes
def broken(what, code, text, platform="genesis"):
    refused(what, code, "mod", "check", project(text, platform=platform))


broken("unknown symbol", "E_SYMBOL_UNKNOWN", HEAD + SYMBOLS + HOOKS.replace('"fn-a"', '"nope"'))
manual = SYMBOLS.replace('address = "0x00000220"\nprovenance = "verified"', 'address = "0x00000220"\nprovenance = "manual"')
broken("manual symbol cannot be a hook site", "E_SYMBOL_NOT_VERIFIED", HEAD + manual + HOOKS)
hint = SYMBOLS.replace('address = "0x00000240"\nprovenance = "verified"', 'address = "0x00000240"\nprovenance = "imported-hint"')
broken("imported hint cannot be a hook site", "E_SYMBOL_NOT_VERIFIED", HEAD + hint + HOOKS)
only_rev_a = SYMBOLS.split("[[symbol]]")
partial = "[[symbol]]".join([only_rev_a[0]] + [only_rev_a[1], only_rev_a[3]])
broken("a hook that applies to every revision needs a verified symbol in each", "E_SYMBOL_UNKNOWN", HEAD + partial + HOOKS)
ok("a hook limited to one revision resolves only there", "mod", "check", project(HEAD + partial + HOOKS.replace('symbol = "fn-b"', 'symbol = "fn-b"\nrevision = "rev-a"').replace('symbol = "fn-a"', 'symbol = "fn-a"\nrevision = "rev-a"'), "limited"))
broken("replace without contract", "E_HOOK_REPLACE_CONTRACT", HEAD + SYMBOLS + HOOKS.replace('writes = ["d0", "ccr"]\ncost = 24\n', ""))
broken("replace cost zero", "E_HOOK_REPLACE_CONTRACT", HEAD + SYMBOLS + HOOKS.replace("cost = 24", "cost = 0"))
broken("replace cost too large", "E_HOOK_REPLACE_CONTRACT", HEAD + SYMBOLS + HOOKS.replace("cost = 24", "cost = 70000"))
broken("a before hook may not declare a contract", "E_HOOK_REPLACE_CONTRACT", HEAD + SYMBOLS + HOOKS.replace('reads = ["d0", "d1", "cycles"]', 'reads = ["d0"]\ncost = 4'))
broken("A7 is never writable", "E_HOOK_REGISTER", HEAD + SYMBOLS + HOOKS.replace('writes = ["d0", "ccr"]', 'writes = ["d0", "a7"]'))
broken("an unknown register", "E_HOOK_REGISTER", HEAD + SYMBOLS + HOOKS.replace('"d0", "d1", "cycles"', '"d0", "d9"'))
broken("cycles is read only", "E_HOOK_REGISTER", HEAD + SYMBOLS + HOOKS.replace('writes = ["d0", "ccr"]', 'writes = ["cycles"]'))
broken("an unknown mode", "E_HOOK_MODE", HEAD + SYMBOLS + HOOKS.replace('"before-after"', '"around"'))
broken("a bad id", "E_HOOK_ID", HEAD + SYMBOLS + HOOKS.replace('"a-watch"', '"A Watch"'))
broken("a repeated id", "E_HOOK_OVERLAP", HEAD + SYMBOLS + HOOKS.replace('"a-watch"', '"b-replace"'))
broken("two hooks on one site", "E_HOOK_OVERLAP", HEAD + SYMBOLS + HOOKS.replace('symbol = "fn-b"', 'symbol = "fn-a"'))
broken("an undeclared revision", "E_HOOK_REVISION", HEAD + SYMBOLS + HOOKS.replace('symbol = "fn-b"', 'symbol = "fn-b"\nrevision = "rev-z"'))
broken("the Z80 has no succeeded spike", "E_HOOK_CPU_UNSUPPORTED", HEAD + SYMBOLS + HOOKS.replace('symbol = "fn-b"', 'symbol = "fn-b"\ncpu = "z80"'))
sms = HEAD.replace('sha256 = "' + SHA_A + '"', 'sha256 = "' + SHA_A + '"\nmapper = "sega"').replace('sha256 = "' + SHA_B + '"', 'sha256 = "' + SHA_B + '"\nmapper = "sega"')
broken("Master System has no hook promise", "E_HOOK_CPU_UNSUPPORTED", sms + SYMBOLS + HOOKS, "master-system")
odd = SYMBOLS.replace("0x00000220", "0x00000221")
broken("an odd address cannot start an M68K instruction", "E_HOOK_MID_INSTRUCTION", HEAD + odd + HOOKS)
broken("a hook needs the capability", "E_CAPABILITY_INCONSISTENT", (HEAD + SYMBOLS + HOOKS).replace('required = ["hook.function", "host.log"]', 'required = ["host.log"]'))
many = "".join(f'\n[[hook]]\nid = "h{i:02d}"\nsymbol = "fn-a"\nmode = "before"\n' for i in range(65))
broken("more than 64 hooks", "E_HOOK_COUNT", HEAD + SYMBOLS + many)
# Sega CD: the disc source identity has no verified module/overlay symbols
scd = '''[mod]
id = "scd-hook"
version = "1.0.0"
name = "Scd"
license = "MIT"

[target]
platform = "sega-cd"
disc_identity = "%s"

[capabilities]
required = ["host.log"]

[[hook]]
id = "h"
symbol = "fn-a"
mode = "before"
''' % ("b" * 64)
d = tmp / "scd"
write(d / "mod.toml", scd)
rc, doc, _ = cli.run("mod", "check", d)
cli.check(rc == 1 and doc and doc.get("class") in ("E_HOOK_OVERLAY_UNRESOLVED", "E_SOURCE_KIND_RESERVED"), f"Sega CD hooks are refused: {doc}")

# ------------------------------------------------------------------ the register names the manifest accepts equal the runtime masks (function_hooks.h)
header = open(os.path.join(SDK, "genesis", "runtime", "function_hooks.h")).read()
cli.check("GENESIS_HOOK_READABLE_MASK UINT32_C(0xFF03FFFF)" in header and "GENESIS_HOOK_WRITABLE_MASK UINT32_C(0x00017FFF)" in header, "the manifest register view matches the runtime masks")

# ------------------------------------------------------------------ the hook table keys the variant: a changed hook misses the cache and names `hooks`
lib = tmp / "library"
pkg = tmp / "install.segapatch"
ok("pack for install", "mod", "pack", good, "--out", pkg)
ident = ok("inspect", "mod", "inspect", pkg)["package_identity"]
ok("install and approve", "mod", "install", pkg, "--rom", rom_a, "--library", lib, "--approve", ident)
first = ok("variant", "mod", "variant", "--library", lib, "--identity", ident, "--rom", rom_a, "--cc", CC, "--sdk-dir", SDK)
cli.check(first["cache"] == "miss" and "hooks" in first["components"], f"the variant names the hook table as a component: {first['components']}")
hit = ok("variant again", "mod", "variant", "--library", lib, "--identity", ident, "--rom", rom_a, "--cc", CC, "--sdk-dir", SDK)
cli.check(hit["cache"] == "hit" and hit["variant_key"] == first["variant_key"], "unchanged hooks hit the cache")
cost_pkg = tmp / "cost.segapatch"
ok("pack changed cost", "mod", "pack", project((HEAD + SYMBOLS + HOOKS).replace("cost = 24", "cost = 32"), "cost"), "--out", cost_pkg)
cost_id = ok("inspect changed cost", "mod", "inspect", cost_pkg)["package_identity"]
ok("install changed cost", "mod", "install", cost_pkg, "--rom", rom_a, "--library", lib, "--approve", cost_id)
second = ok("variant of the changed hook", "mod", "variant", "--library", lib, "--identity", cost_id, "--rom", rom_a, "--cc", CC, "--sdk-dir", SDK)
cli.check(second["variant_key"] != first["variant_key"] and second["components"]["hooks"] != first["components"]["hooks"], "a changed hook cost changes the hooks component and the variant key")

cli.done("mod_hooks_test")
