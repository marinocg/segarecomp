#!/usr/bin/env python3
"""SEG-137-T003 (ADR 0294): the trusted Luau compile step through the product CLI, with a FABRICATED fixture image and synthetic scripts only.
`mod build` compiles Luau sources with the pinned compiler (syntax errors are stable `file:line`, a failed build writes nothing), the compile output is deterministic across runs and working
directories, packages carrying bytecode are rejected by schema validation, and the variant records the bytecode digest and refuses (rebuilds) any compiled output modified after build.
The compiled-artifact format and digest are re-derived here by an independent Python implementation.
usage: mod_luau_test.py <segarecomp-cli>"""
import hashlib
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
tmp = temp()
rom = write(tmp / "fixture" / "a.bin", SRC_IMAGE)
PIN = "0.746"
OPTIONS = "O1 g0 no-fastcall:getmetatable,math.random,math.randomseed"


def ok(what, *args, cwd=None):
    rc, doc, p = cli.run(*args, cwd=cwd)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc


def project(name, scripts, template="luau"):
    d = tmp / name
    ok(f"init {name}", "mod", "init", d, "--platform", "genesis", "--id", "lt", "--template", template, "--rom-sha256", SRC_SHA)
    for rel, text in scripts.items():
        write(d / rel, text)
    return d


GOOD = "counter = 0\nfunction on_frame()\n  counter = counter + 1\nend\n"

# ---- build: deterministic compile, digest reported, package unchanged by the compile
proj = project("good", {"scripts/main.luau": GOOD})
out1, out2 = tmp / "one.segapatch", tmp / "two.segapatch"
b1 = ok("build", "mod", "build", proj, "--out", out1)
b2 = ok("build again from another working directory", "mod", "build", proj, "--out", out2, cwd=tmp)
cli.check(b1["luau"] == b2["luau"] and b1["luau"]["compiler"] == f"luau-{PIN}" and b1["luau"]["scripts"] == 1, f"build reports the compiler and is deterministic: {b1.get('luau')} {b2.get('luau')}")
cli.check(len(b1["luau"]["bytecode_digest"]) == 64, "the bytecode digest is a SHA-256")
cli.check(out1.read_bytes() == out2.read_bytes(), "the package carries source only and is bit-identical")
packed = ok("pack (no compile)", "mod", "pack", proj, "--out", tmp / "p.segapatch")
cli.check("luau" not in packed and (tmp / "p.segapatch").read_bytes() == out1.read_bytes(), "pack does not compile and emits the same package")
check = ok("check", "mod", "check", proj)
cli.check("luau" not in check, "check never compiles")

# ---- compile errors: stable file:line, nothing written
bad = project("bad", {"scripts/main.luau": "local a = 1\n\nlocal b = = 2\n"})
for attempt in range(2):
    out = tmp / f"bad{attempt}.segapatch"
    rc, doc, _ = cli.run("mod", "build", bad, "--out", out)
    cli.check(rc == 1 and doc["class"] == "E_LUAU_SYNTAX" and doc["error"].startswith("scripts/main.luau:3: "), f"syntax error reports file:line (attempt {attempt}): {doc}")
    cli.check(not out.exists(), "a failed build writes no package")
msgs = set()
for n, src in enumerate(("x = (\n", "if true then\n\nx = 1\n", "local function f(\n  a,\nend\n")):
    p = project(f"err{n}", {"scripts/main.luau": src})
    rc, doc, _ = cli.run("mod", "build", p, "--out", tmp / "err.segapatch")
    cli.check(rc == 1 and doc["class"] == "E_LUAU_SYNTAX" and doc["error"].split(":")[1].isdigit(), f"error carries a numeric line: {doc}")
    msgs.add(doc["error"])
two = project("two", {"scripts/main.luau": GOOD})
write(two / "scripts" / "extra.luau", "x = = 1\n")
write(two / "mod.toml", (two / "mod.toml").read_text() + '\n[[source]]\nkind = "luau-source"\npath = "scripts/extra.luau"\n')
rc, doc, _ = cli.run("mod", "build", two, "--out", tmp / "two-bad.segapatch")
cli.check(rc == 1 and doc["class"] == "E_LUAU_SYNTAX" and doc["error"].startswith("scripts/extra.luau:1: "), f"the failing file is named: {doc}")

# ---- bytecode is never accepted from a package
for name, blob in (("version byte", b"\x06\x01\x00crafted"), ("lead 0x1b", b"\x1bLua\x51\x00"), ("embedded NUL", b"x = 1\x00"), ("control byte", b"x = 1\x03\n")):
    p = project("bc-" + name.split()[0], {"scripts/main.luau": b"x = 1\n"})
    write(p / "scripts" / "main.luau", blob)
    rc, doc, _ = cli.run("mod", "build", p, "--out", tmp / "bc.segapatch")
    cli.check(rc == 1 and doc["class"] == "E_LUAU_BYTECODE", f"a project whose Luau file is not text is rejected before the compiler ({name}): {doc}")
lp, lf = luau("scripts/main.luau", b"\x06\x01crafted-bytecode")
pkg = write(tmp / "crafted.segapatch", build(base_manifest(payloads=[(lp, lf)]), [(lp, lf)]))
rc, doc, _ = cli.run("mod", "inspect", pkg)
cli.check(rc == 1 and doc["class"] == "E_LUAU_BYTECODE", f"a package carrying a crafted bytecode blob is rejected by schema validation: {doc}")
rc, doc, _ = cli.run("mod", "install", pkg, "--rom", rom, "--library", tmp / "lib-crafted")
cli.check(rc == 1 and doc["class"] == "E_LUAU_BYTECODE", f"and cannot be installed: {doc}")
bp = payload("luau-bytecode", "scripts/main.luab", b"\x06\x01x")
rc, doc, _ = cli.run("mod", "inspect", write(tmp / "kind.segapatch", build(base_manifest(payloads=[(bp, ("scripts/main.luab", b"\x06\x01x"))]), [(bp, ("scripts/main.luab", b"\x06\x01x"))])))
cli.check(rc == 1 and doc["ok"] is False, f"an unknown bytecode payload kind is rejected: {doc}")

# ---- the variant records the digest; the stored compiled output is verified before reuse
lib = tmp / "lib"
inst = ok("install", "mod", "install", out1, "--rom", rom, "--library", lib)
ident = inst["package_identity"]
VAR = ("mod", "variant", "--library", lib, "--identity", ident, "--rom", rom)
first = ok("first variant", *VAR)
cli.check(first["cache"] == "miss" and first["components"]["luau_bytecode"] == b1["luau"]["bytecode_digest"], f"the variant identity carries the bytecode digest: {first}")
entry = lib / "cache" / "variants" / first["variant_key"]
stored = (entry / "luau.bin").read_bytes()


def parse_artifact(data):
    """Independent reader of the stored compiled form and of its digest rule (luau-trust-policy.md section 2.5)."""
    assert data.startswith(b"SEGLUAUC1\n")
    pos = len(b"SEGLUAUC1\n")

    def line():
        nonlocal pos
        end = data.index(b"\n", pos)
        text = data[pos:end].decode("ascii")
        pos = end + 1
        return text

    identity = line().split(" ", 1)[1]
    digest = line().split(" ", 1)[1]
    count = int(line().split(" ", 1)[1])
    scripts = []
    for _ in range(count):
        _, path, src, size = line().split(" ")
        body = data[pos:pos + int(size)]
        pos += int(size)
        assert data[pos:pos + 1] == b"\n"
        pos += 1
        scripts.append((path, src, hashlib.sha256(body).hexdigest(), body))
    assert pos == len(data)
    return identity, digest, scripts


identity, digest, scripts = parse_artifact(stored)
want = "segarecomp-luau-bytecode-1\ncompiler luau %s %s\nidentity %s\n" % (PIN, OPTIONS, ident) + "".join(f"script {p} {s} {b}\n" for p, s, b, _ in sorted(scripts))
cli.check(identity == ident and digest == hashlib.sha256(want.encode()).hexdigest() == first["components"]["luau_bytecode"], "the stored artifact and its digest match the independent derivation")
cli.check(scripts[0][1] == hashlib.sha256(GOOD.encode()).hexdigest() and scripts[0][3][:1] not in (b"", b"\x00") and scripts[0][3] != GOOD.encode(), "bound to the source digest; the bytecode is not the source text")
hit = ok("unchanged variant", *VAR)
cli.check(hit["cache"] == "hit" and hit["variant_key"] == first["variant_key"], f"unchanged compiled output hits the cache: {hit}")
for what, tamper in (("a flipped bytecode byte", lambda b: b[:-4] + bytes([b[-4] ^ 1]) + b[-3:]), ("a crafted blob", lambda b: b"\x06\x01crafted"), ("a truncated file", lambda b: b[:-5]), ("an emptied file", lambda b: b"")):
    (entry / "luau.bin").write_bytes(tamper(stored))
    again = ok(f"variant after {what}", *VAR)
    cli.check(again["cache"] == "miss" and (entry / "luau.bin").read_bytes() == stored, f"{what}: the modified compiled output is not reused; it is rebuilt from source: {again}")
(entry / "luau.bin").unlink()
again = ok("variant with the compiled output removed", *VAR)
cli.check(again["cache"] == "miss" and (entry / "luau.bin").read_bytes() == stored, "a missing compiled output is rebuilt")

# ---- a changed script changes the bytecode digest and names it
edited = project("edited", {"scripts/main.luau": GOOD.replace("+ 1", "+ 2")})
out3 = tmp / "three.segapatch"
b3 = ok("build edited", "mod", "build", edited, "--out", out3)
cli.check(b3["luau"]["bytecode_digest"] != b1["luau"]["bytecode_digest"], "the digest depends on the script")
ok("install edited", "mod", "install", out3, "--rom", rom, "--library", lib)
v3 = ok("variant edited", "mod", "variant", "--library", lib, "--identity", b3["package_identity"], "--rom", rom)
cli.check("luau_bytecode" in v3["components"] and v3["variant_key"] != first["variant_key"], "an edited script is its own variant")

# ---- a package without Luau has no luau component and no output
plain = project("plain", {}, template="rom-edit")
toml = (plain / "mod.toml").read_text()
cut = toml.index('[[source]]\nkind = "luau-source"')
nxt = toml.find("[[source]]", cut + 5)
write(plain / "mod.toml", toml[:cut] + (toml[nxt:] if nxt != -1 else ""))
pb = ok("build without luau", "mod", "build", plain, "--out", tmp / "plain.segapatch")
cli.check("luau" not in pb, "no Luau, no compile step output")

cli.done("mod_luau_test")
