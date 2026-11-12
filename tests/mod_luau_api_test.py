#!/usr/bin/env python3
"""SEG-137-T004..T007 (ADR 0295, delivery F07): the Luau host API through the product CLI, with a FABRICATED fixture image and synthetic scripts only.
  T004  the API list (`mod api`) is committed and cannot drift; an undeclared capability is rejected by check/pack/build; the install sheet distinguishes data, Luau-only (consent) and native.
  T005  the generated `.d.luau`, the template's editor files, `mod check` flagging a type error / an undeclared / an unknown API use in fixture scripts.
  T006  a failing script never stops the host (fixture session), the diagnostic has its stable shape, the golden session hash is identical on every platform that runs this test.
  T007  the committed SDK example is built, packed and installed through the CLI (Luau-only trust, consent, no native approval). The `mod session` runs here are the DIAGNOSTIC synthetic fixture
        session (a synthetic picture, NOT the Genesis machine): they pin the script -> overlay -> raster -> composite chain (an independent Python oracle re-derives a rect-only frame hash). They are NOT the
        T007 end-to-end proof: that is tests/mod_luau_executable_test.py, which builds and runs a real generated-native program.
usage: mod_luau_api_test.py <segarecomp-cli>"""
import hashlib
import json
import platform
import shutil
import sys
from pathlib import Path

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
repo = Path(__file__).resolve().parent.parent
tmp = temp()


import importlib.util

_spec = importlib.util.spec_from_file_location("luau_hud_fixture_rom", repo / "platforms/genesis/sdk/examples/luau-hud/fixture_rom.py")
_fixture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_fixture)
ROM = _fixture.make_rom()   # the example's runnable fixture image
ROM_SHA = hashlib.sha256(ROM).hexdigest()
rom = write(tmp / "fixture.rom", ROM)


def ok(what, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc or {}


def refused(what, cls, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == cls, f"{what}: expected {cls}, got rc={rc} {doc}")
    return doc or {}


def project(name, script, caps=("host.log", "frame.work_ram", "overlay.rgba"), platform_id="genesis"):
    d = tmp / name
    ok(f"init {name}", "mod", "init", d, "--platform", platform_id, "--id", "api-test", "--template", "luau", "--rom-sha256", ROM_SHA)
    toml = (d / "mod.toml").read_text()
    head, _, _ = toml.partition("[capabilities]")
    (d / "mod.toml").write_text(head + "[capabilities]\nrequired = [" + ", ".join(f'"{c}"' for c in sorted(caps)) + "]\n")
    write(d / "scripts" / "main.luau", script)
    return d


# ---- T004/T005: the committed API list and declarations cannot drift ----
rc, _, p = cli.run("mod", "api", "--declarations")
cli.check(rc == 0 and p.stdout == (repo / "docs/product/luau/segarecomp.d.luau").read_text(), "docs/product/luau/segarecomp.d.luau equals `mod api --declarations` (regenerate it when the API list changes)")
rc, api, p = cli.run("mod", "api")
cli.check(rc == 0 and p.stdout == (repo / "docs/product/luau/api.json").read_text(), "docs/product/luau/api.json equals `mod api` (regenerate it when the API list changes)")
cli.check(cli.run("mod", "api")[2].stdout == p.stdout and cli.run("mod", "api", "--declarations")[2].stdout.count("\n") > 10, "the generated forms are deterministic")
entries = (api or {}).get("api", [])
rc, inspect, _ = cli.run("mod", "inspect", "--json")
genesis = next(a for a in inspect["adapters"] if a["platform"] == "genesis")
cap_names = set(genesis["capabilities"])
cli.check(entries and all(e["kind"] == "callback" or e["capability"] in cap_names for e in entries), "every API capability is a catalogue capability the Genesis adapter reports")
cli.check({e["name"] for e in entries} >= {"sega.log", "sega.frame.value", "sega.overlay.rect", "sega.overlay.text", "reset", "render"}, "the API list names the T004 surface")

# ---- T005: the template, editor configuration, and `mod check` on fixture scripts ----
tpl = tmp / "tpl"
created = ok("init luau template", "mod", "init", tpl, "--platform", "genesis", "--id", "tpl", "--template", "luau", "--rom-sha256", ROM_SHA)["created"]
cli.check(set(created) == {"mod.toml", "scripts/main.luau", "types/segarecomp.d.luau", ".luaurc", ".vscode/settings.json"}, f"template files: {created}")
cli.check((tpl / "types/segarecomp.d.luau").read_text() == (repo / "docs/product/luau/segarecomp.d.luau").read_text(), "the template's declaration file is the generated one")
settings = json.loads((tpl / ".vscode/settings.json").read_text())
cli.check(settings.get("luau-lsp.types.definitionFiles") == ["types/segarecomp.d.luau"], f"luau-lsp is pointed at the declarations: {settings}")
cli.check(json.loads((tpl / ".luaurc").read_text()) == {"languageMode": "strict"}, "strict mode in .luaurc")
cli.check((tpl / "scripts/main.luau").read_text() == (repo / "platforms/genesis/sdk/examples/luau-hud/scripts/main.luau").read_text(), "the Genesis template script is the committed SDK example")
doc = ok("the documented editor-setup command: init then check", "mod", "check", tpl)
cli.check(doc["trust_class"] == "luau" and doc["capabilities"] == ["host.log", "overlay.rgba"], "a fresh template checks clean")
ms = tmp / "ms"
ok("init luau template (master-system)", "mod", "init", ms, "--platform", "master-system", "--id", "ms", "--template", "luau", "--rom-sha256", ROM_SHA)
ok("the master-system template checks (log only)", "mod", "check", ms)
# luau-analyze (0.746) cannot load definition files and luau-lsp is not installed in CI: the external editor tooling is documented configuration (docs/product/luau-scripting.md says exactly what was run once by hand).

bad_cases = {
    "undeclared capability": ("function render()\n  sega.overlay.rect(0, 0, 1, 1, 1)\nend\n", ("host.log",), "E_LUAU_CAPABILITY", 2),
    "type error": ("function render()\n  sega.log(42)\nend\n", ("host.log",), "E_LUAU_TYPE", 2),
    "wrong arity": ("sega.overlay.rect(1, 2, 3)\n", ("overlay.rgba",), "E_LUAU_TYPE", 1),
    "unknown API": ("local x = 1\nsega.teleport(x)\n", ("host.log",), "E_LUAU_API", 2),
}
for name, (src, caps, cls, line) in bad_cases.items():
    d = project("bad-" + name.replace(" ", "-"), src, caps)
    for sub in ("check", "pack", "build"):
        extra = ["--out", tmp / f"{d.name}-{sub}.segapatch"] if sub != "check" else []
        doc = refused(f"{name} ({sub})", cls, "mod", sub, d, *extra)
        cli.check(doc.get("error", "").startswith(f"scripts/main.luau:{line}: ") and doc["issues"][0]["line"] == line, f"{name}: the first issue is scripts/main.luau:{line}: {doc.get('error')}")
    cli.check(not (tmp / f"{d.name}-build.segapatch").exists() and not (tmp / f"{d.name}-pack.segapatch").exists(), f"{name}: nothing is written")
multi = refused("two issues", "E_LUAU_TYPE", "mod", "check", project("multi", "sega.log(1)\nsega.log(2)\n", ("host.log",)))
cli.check([i["line"] for i in multi["issues"]] == [1, 2], "every issue is listed in line order")
ok("a declared use works", "mod", "check", project("fine", "function render(f)\n  sega.log('x')\n  sega.overlay.text(0, 0, 'hi', 0xFFFFFFFF)\nend\n"))
d = project("syntax", "local = 1\n")
ok("a syntax error is left to the compile step: check passes", "mod", "check", d)
refused("a syntax error fails the build", "E_LUAU_SYNTAX", "mod", "build", d, "--out", tmp / "syntax.segapatch")

# ---- T007: the committed SDK example, end to end ----
example = tmp / "luau-hud"
shutil.copytree(repo / "platforms/genesis/sdk/examples/luau-hud", example)
cli.check('sha256 = "' + ROM_SHA + '"' in (example / "mod.toml").read_text(), "the example's revision is the deterministic runnable fixture image")
ok("check the example", "mod", "check", example)
built = ok("build the example", "mod", "build", example, "--out", tmp / "hud.segapatch")
cli.check(built["trust_class"] == "luau" and built["payloads"]["native"] == 0 and built["luau"]["scripts"] == 1, "the example is Luau-only")
ok("pack the example", "mod", "pack", example, "--out", tmp / "hud-packed.segapatch")
cli.check((tmp / "hud-packed.segapatch").read_bytes() == (tmp / "hud.segapatch").read_bytes(), "pack and build give the same package")
lib = tmp / "lib"
inst = ok("install", "mod", "install", tmp / "hud.segapatch", "--rom", rom, "--library", lib)
cli.check(inst["trust_class"] == "luau" and inst["approval_kind"] == "consent" and inst["approved"] is False, f"Luau-only: consent, no native approval: {inst}")
sheet = inst["install_sheet"]
cli.check("scripts that run while you play" in sheet and "not a security guarantee" in sheet and "cannot access your files or network" in sheet and "overlay.rgba" in sheet, f"the install sheet states the policy text and the capabilities: {sheet}")
cli.check("native code" not in sheet, "the Luau-only sheet is not the native-code approval")
ident = inst["package_identity"]
var = ok("variant without any approval", "mod", "variant", "--library", lib, "--identity", ident, "--rom", rom)
cli.check(var["components"].get("luau_bytecode"), "the variant records the trusted compile digest")
refused("another image is not accepted", "E_IMAGE_MISMATCH", "mod", "install", tmp / "hud.segapatch", "--rom", write(tmp / "other.rom", ROM + b"x"), "--library", tmp / "lib2")
installed = lib / "mods" / f"{ident}.segapatch"

golden = json.loads((repo / "tests/fixtures/mod_luau/hud_session.json").read_text())
bmp = tmp / "last.bmp"
s1 = ok("session on the installed package", "mod", "session", installed, "--rom", rom, "--frames", golden["frames"], "--bmp", bmp)
s2 = ok("session on the project directory", "mod", "session", example, "--rom", rom, "--frames", golden["frames"])
for key in ("first_frame_sha256", "last_frame_sha256", "session_sha256"):
    cli.check(s1[key] == golden[key], f"golden {key} on {platform.system()} {platform.machine()}: {s1[key]} != {golden[key]}")
cli.check(s1["session_sha256"] == s2["session_sha256"] and s1["package_identity"] == ident, "package and project give the same session")
cli.check(s1["frames"] == golden["frames"] == s1["overlay_frames"] and s1["faulted"] is False and s1["leaked_bytes"] == 0 and s1["nondeterministic"] is False and s1["log"] == ["hud example ready"] and s1["synthetic_picture"] is True, f"clean session: {s1}")
data = bmp.read_bytes()
cli.check(data[:2] == b"BM" and int.from_bytes(data[18:22], "little") == 320 and int.from_bytes(data[22:26], "little") == 224 and len(data) == 54 + 320 * 224 * 3, "the last frame is a 320x224 24-bit BMP")
other = ok("a different frame count", "mod", "session", installed, "--rom", rom, "--frames", golden["frames"] - 1)
cli.check(other["session_sha256"] != s1["session_sha256"] and other["first_frame_sha256"] == s1["first_frame_sha256"], "frame 0 is the same, the session hash is not")
refused("zero frames", "E_SESSION", "mod", "session", installed, "--rom", rom, "--frames", "0")
refused("too many frames", "E_SESSION", "mod", "session", installed, "--rom", rom, "--frames", "3601")
refused("the wrong image", "E_IMAGE_MISMATCH", "mod", "session", installed, "--rom", write(tmp / "wrong.rom", b"not the fixture"), "--frames", "1")
print(f"portability (diagnostic synthetic session only): golden HUD session hash {s1['session_sha256'][:16]}... matched on {platform.system()} {platform.machine()}")


# ---- independent oracle for a rect-only script: picture pattern, overlay raster and composite re-derived in Python ----
def oracle_frame(f, rect, seed):
    x0, y0, w, h, rgba = rect
    sr, sg, sb, sa = (rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255
    out = bytearray()
    for y in range(224):
        for x in range(320):
            px = [(x * 3 + f + seed[0]) & 255, (y * 5 + 2 * f + seed[1]) & 255, ((x ^ y) + 7 * f + seed[2]) & 255]
            if x0 <= x < x0 + w and y0 <= y < y0 + h and sa:
                px = [(c * sa + d * (255 - sa) + 127) // 255 for c, d in zip((sr, sg, sb), px)]
            out += bytes(px)
    return hashlib.sha256(out).hexdigest()


rect = (10, 20, 30, 12, 0xFF8040C0)
d = project("oracle", "function render(frame)\n  sega.overlay.rect(10, 20, 30, 12, 0xFF8040C0)\nend\n")
seed = bytes.fromhex(ROM_SHA)[:3]
o1 = ok("oracle session, 1 frame", "mod", "session", d, "--rom", rom, "--frames", "1")
o8 = ok("oracle session, 8 frames", "mod", "session", d, "--rom", rom, "--frames", "8")
cli.check(o1["first_frame_sha256"] == oracle_frame(0, rect, seed), "frame 0 equals the independent Python derivation")
cli.check(o8["last_frame_sha256"] == oracle_frame(7, rect, seed), "frame 7 equals the independent Python derivation")
chain = "".join(oracle_frame(f, rect, seed) + "\n" for f in range(8))
cli.check(o8["session_sha256"] == hashlib.sha256(chain.encode()).hexdigest(), "the session hash is the SHA-256 of the frame digests, one per line")

# ---- T006: a failing script disables only its mod and the host keeps presenting frames ----
bad = project("faulting", "function render(frame)\n  if frame == 5 then error('boom') end\n  sega.overlay.rect(0, 0, 8, 8, 0xFF0000FF)\nend\n")
f = ok("faulting session still succeeds", "mod", "session", bad, "--rom", rom, "--frames", "12")
cli.check(f["faulted"] is True and f["fault"] == "error" and f["frames"] == 12 and f["overlay_frames"] == 5, f"the host presented every frame: {f}")
cli.check(f["diagnostic"].startswith("E_MOD_FAULT cause=error mod=api-test callback=render frame=5: "), f"stable diagnostic: {f['diagnostic']}")
for needle in ("/", "0x", "\\", ".cpp"):
    cli.check(needle not in f["diagnostic"].split(": ", 1)[1], f"the fault text carries no host detail ({needle})")
causes = {
    "budget": "function render() while true do end end",
    "memory": "function render() local s = string.rep('ab', 2^30) end",
    "capability": "local s = sega\nfunction render() s.log('x') end",   # an alias is not followed by the static check, so the run-time gate decides
    "range": "function render() sega.overlay.rect(0, 0, -1, 1, 1) end",
    "stack": "function render() local function f() return 1 + f() end f() end",
    "phase": "function reset() local v = sega.frame.value('score') end",
}
for cause, src in causes.items():
    caps = ("frame.work_ram", "overlay.rgba") if cause == "capability" else ("host.log", "frame.work_ram", "overlay.rgba")
    d = project("cause-" + cause, src, caps)
    r = ok(f"{cause} fault", "mod", "session", d, "--rom", rom, "--frames", "4")
    got = r["fault"]   # the 4 ms hang watchdog may win a budget case on a slow host (it marks the run non-deterministic)
    cli.check(r["faulted"] and r["frames"] == 4 and (got == cause or (cause == "memory" and got == "error") or (cause == "budget" and got == "watchdog")), f"{cause}: reported as {got}: {r['diagnostic']}")

# ---- approval-semantics: data, Luau-only and native/mixed are different classes ----
data_dir = tmp / "data"
ok("init data package", "mod", "init", data_dir, "--platform", "genesis", "--id", "data-only", "--template", "rom-edit", "--rom-sha256", ROM_SHA)
text = (data_dir / "mod.toml").read_text().replace('[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n', "")
(data_dir / "mod.toml").write_text(text)
write(data_dir / "edits" / "main.edits", f"rev-a u8 0x00000010 0x{ROM[0x10]:02x} 0x{(ROM[0x10] ^ 1):02x}\n")
shutil.rmtree(data_dir / "scripts")
ok("pack data", "mod", "pack", data_dir, "--out", tmp / "data.segapatch")
di = ok("install data", "mod", "install", tmp / "data.segapatch", "--rom", rom, "--library", tmp / "libd")
cli.check(di["trust_class"] == "data" and di["approval_kind"] == "none" and "no code runs" in di["install_sheet"], f"data class: {di}")
nat = tmp / "native"
ok("init native", "mod", "init", nat, "--platform", "genesis", "--id", "nat", "--template", "c11-provider", "--rom-sha256", ROM_SHA)
ok("pack native", "mod", "pack", nat, "--out", tmp / "nat.segapatch")
ni = ok("install mixed (native + Luau)", "mod", "install", tmp / "nat.segapatch", "--rom", rom, "--library", tmp / "libn")
cli.check(ni["trust_class"] == "native" and ni["approval_kind"] == "native" and "native code that can do anything" in ni["install_sheet"], f"native/mixed class needs native approval: {ni}")
refused("a package with native code is not run by the fixture session", "E_TRUST_CLASS", "mod", "session", tmp / "nat.segapatch", "--rom", rom, "--frames", "1")
refused("a data package is not a Luau session", "E_TRUST_CLASS", "mod", "session", tmp / "data.segapatch", "--rom", rom, "--frames", "1")

cli.done("mod_luau_api_test")
