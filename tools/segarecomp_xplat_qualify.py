#!/usr/bin/env python3
"""SEG-075-T001 (ADR 0159): reproducible cross-platform MC68000 source qualification (OFFLINE research tooling).

  fetch       shallow-fetch every pinned candidate revision into the private cache and verify HEAD equals the pinned commit (an
              archive candidate is fetched blobless and only a seeded pilot of its subprojects is checked out; never copied wholesale)
  toolchains  download the pinned toolchain inputs (SHA-256 verified) and build the toolchain images from
              tools/segarecomp_xplat_toolchains/*.Dockerfile; record the observed tool versions and image identities
  build       run every recipe of the frozen plan twice, each time in a fresh copy of the pinned source inside an offline container
              (--network none), after re-verifying that the cached checkout is exactly the pinned commit and unmodified. The two runs
              see different wall-clock dates through a `date` wrapper (neither the real date), and every output is scanned for the
              real build day, which catches dates the wrapper cannot control (a compiler's __DATE__). Record the outcome, output digests/sizes/executable format, determinism, author-binary comparisons,
              the source CPU-variant scan and the archive pilot counts in a private record
  verify      check that freshly built records reproduce a manifest (identities, classes, outcomes, code digests); the manifest is
              --manifest (default: the committed one); --out is rejected. This compares RECORDED evidence only: the physical private
              files are checked by tools/segarecomp_xplat_handoff.py (SEG-075 C1)
  identity    write (or --check) the versioned v2 recipe identity (ADR 0160): the frozen v1 digest plus Dockerfile contents, embedded
              helper scripts and the recorded image ids. The v1 digests and T001 records are never rewritten
  report      combine the plan and the records into the sanitized manifest. It fails when any executed evidence contradicts the
              plan's declared expectation: a recipe declared to succeed fails, an expected failure does not reproduce its signature,
              or a declared author comparison is not byte-identical

The plan (tools/segarecomp_xplat_qualify.plan.json) freezes the candidate inventory, revisions, toolchain inputs, recipes and the declared
qualification class of every candidate. Classes are assigned only from executed attempts or documented inspection recorded there, and this
tool checks every executed claim. Durable output is identity/aggregate-only: repository URLs, pinned commits, licences, tool identities,
output digests and sizes of public open-source builds, formats, classes and counts. Sources, build trees, listings and binaries stay in the
private cache, which must be outside the product repository or git-ignored. No commercial input is fetched or used.
"""
from __future__ import annotations

import argparse
import datetime
import fnmatch
import glob
import hashlib
import json
import os
import pathlib
import random
import re
import shutil
import struct
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLAN = ROOT / "tools" / "segarecomp_xplat_qualify.plan.json"
MANIFEST = ROOT / "tools" / "segarecomp_xplat_qualify.manifest.json"
IMAGE_PREFIX = "segarecomp-xplat/"
TECHNICAL_EXTRA = ("NOT_ATTEMPTED",)
PROVENANCE = ("authored", "commercial-derived", "reconstruction")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
# HEURISTIC (not evidence): assembly source lines that select a non-68000 CPU or use a 68010+/FPU/MMU-only instruction or addressing form.
# Instructions hand-encoded as dc.w are invisible to it; real CPU-variant typing is T002's decoder-backed job.
_MNEM = (r"movec|moves|pmove|pflush\w*|ptest\w*|cinv[lpa]|cpush[lpa]|cas2?|chk2|cmp2|bf(?:chg|clr|ext[su]|ffo|ins|set|tst)|rtd|bkpt|pack|"
         r"unpk|extb|trap(?:t|f|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)|(?:mul|div)[su]\.l|divs?l|f(?:move|add|sub|mul|div|cmp|sqrt|tst|"
         r"save|restore|b\w\w?|s\w\w?|db\w\w?)")
CPU_VARIANT_RE = re.compile(
    r"^(?:[A-Za-z_.$][\w.$]*:|[A-Za-z_.$][\w.$]*(?=[ \t]))?[ \t]+(?:\.(?:chip|arch|cpu)\s+\S*680[1-6]0|\.?machine\s+680[1-6]0|"
    r"opt\s+[^;\n]*\bp=680[1-6]0|(?:" + _MNEM + r"|link\.l|chk\.l)(?:\.[bwlsdxp])?(?=\s|$)(?!\s*=)|bra\.l\b|b\w\w\.l\b|"
    r"move(?:\.w)?\s+ccr\s*,)"
    r"|^[^;*\n]*?(?:\(\s*\[|\.[wl]\s*\*\s*[248]\s*[),])",
    re.IGNORECASE | re.MULTILINE)
SOURCE_EXTS = (".s", ".asm", ".68k", ".i", ".inc")

CI_ALIAS = '''import os, sys
# Lower-case alias symlinks so case-insensitive (TOS/AmigaDOS) include names resolve; sources are never edited.
for dp, dns, fns in os.walk(sys.argv[1], topdown=True):
    for n in list(dns) + fns:
        low = n.lower()
        if low != n and not os.path.lexists(os.path.join(dp, low)):
            os.symlink(n, os.path.join(dp, low))
'''

# `date` wrapper: each run fixes the wall-clock date seen by build scripts, and the two runs use dates years apart, so date-dependent
# output shows up as nondeterminism. A recipe may pin XPLAT_DATE itself (then its output must be reproducible on any day).
DATE_SHIM = '''#!/bin/sh
for a in "$@"; do case "$a" in -d|-d*|--date|--date=*|-r|-r*|--reference|--reference=*|-f|-f*|--file|--file=*) exec /bin/date "$@";; esac; done
if [ -n "$XPLAT_DATE" ]; then exec /bin/date -u -d "$XPLAT_DATE" "$@"; fi
exec /bin/date "$@"
'''
# Neither run date is the real date, so a date that reaches an output without passing through the wrapper (a compiler's __DATE__, awk
# systime(), time()) appears as the real build day and is caught by clock_leaks().
RUN_DATES = {"a": "2029-06-01 12:00:00 UTC", "b": "2031-03-04 07:30:00 UTC"}


def date_patterns(day: datetime.date) -> list[bytes]:
    """Encodings of one calendar day as build tools embed it: C __DATE__ ("Oct 19 2026", "Oct  9 2026"), YYYYMMDD, YYYY-MM-DD, DD.MM.YYYY."""
    mon = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")[day.month - 1]
    return [f"{mon} {day.day:2d} {day.year}".encode(), f"{day:%Y%m%d}".encode(), f"{day:%Y-%m-%d}".encode(), f"{day:%d.%m.%Y}".encode()]


def clock_leaks(outputs: dict[str, bytes], days: list[datetime.date]) -> list[str]:
    """Outputs containing any of the real build days: a clock source the date wrapper did not control (pure; unit-tested)."""
    pats = [pat for d in days for pat in date_patterns(d)]
    return sorted(path for path, data in outputs.items() if any(pat in data for pat in pats))

ASTS_PILOT = r'''import collections, os, shutil, struct, subprocess, sys
# Assemble each pilot subproject's single source with vasm (-devpac) and compare with the subproject's shipped TOS executable.
root, listing = sys.argv[1], sys.argv[2]
def prg(b):
    if len(b) < 28 or b[:2] != b'\x60\x1a': return None
    t, d = struct.unpack('>II', b[2:10]); return b[28:28 + t], b[28 + t:28 + t + d]
res = collections.Counter()
for i, line in enumerate(open(listing)):
    d, s, e = line.rstrip('\n').split('\t')
    w = '/tmp/pilot/%d' % i; shutil.rmtree(w, ignore_errors=True); shutil.copytree(os.path.join(root, d), w)
    subprocess.run(['python3', '/xplat/ci_alias.py', w])
    out = os.path.join(w, '_ours.prg')
    r = subprocess.run(['vasmm68k_mot', '-quiet', '-devpac', '-spaces', '-m68000', '-no-fpu', '-Ftos', '-o', out, os.path.basename(s)],
                       cwd=w, capture_output=True, text=True, errors='replace', timeout=120)
    orig = open(os.path.join(root, e), 'rb').read()
    if r.returncode != 0 or not os.path.exists(out):
        err = r.stderr
        cls = 'ASSEMBLY_FAILED_CPU' if 'not supported on selected' in err else 'ASSEMBLY_FAILED_MISSING_INPUT' if 'could not open' in err else 'ASSEMBLY_FAILED_SYNTAX'
    else:
        ours = open(out, 'rb').read(); a, b = prg(ours), prg(orig)
        if ours == orig: cls = 'BYTE_IDENTICAL'
        elif b is None: cls = 'SHIPPED_NOT_PRG'
        elif a and a == b: cls = 'TEXT_DATA_IDENTICAL'
        elif a and a[0] == b[0]: cls = 'TEXT_IDENTICAL'
        else: cls = 'DIFFERENT'
    res[cls] += 1
for k in sorted(res): print('%s\t%d' % (k, res[k]))
'''


class QualifyError(Exception):
    pass


# ---------------------------------------------------------------- plan

def load_plan(path: pathlib.Path = PLAN) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    validate_plan(plan)
    return plan


def validate_plan(plan: dict) -> None:
    """Structural invariants of the frozen plan (no I/O)."""
    if plan.get("schema") != "segarecomp.xplat_qualify.plan.v1":
        raise QualifyError("unexpected plan schema")
    classes = set(plan["classes"])
    toolchains = plan["toolchains"]
    for tid, tc in toolchains.items():
        if tc.get("base") and tc["base"] not in toolchains:
            raise QualifyError(f"toolchain {tid}: unknown base {tc['base']}")
        for item in tc.get("inputs", []):
            if not HEX64.match(item["sha256"]) or not item["url"].startswith(("http://", "https://")):
                raise QualifyError(f"toolchain {tid}: input {item.get('name')} lacks a pinned URL/SHA-256")
        if "dockerfile" not in tc and "context_candidate" not in tc:
            raise QualifyError(f"toolchain {tid}: no build definition")
    ids = set()
    for c in plan["candidates"]:
        cid = c["id"]
        if cid in ids:
            raise QualifyError(f"duplicate candidate {cid}")
        ids.add(cid)
        if not HEX40.match(c["commit"]):
            raise QualifyError(f"{cid}: commit is not a full 40-hex SHA")
        if c["tier"] not in ("A", "B", "C"):
            raise QualifyError(f"{cid}: bad tier")
        if c["class"] not in classes:
            raise QualifyError(f"{cid}: unknown class {c['class']}")
        if c["technical_class"] not in classes and c["technical_class"] not in TECHNICAL_EXTRA:
            raise QualifyError(f"{cid}: unknown technical class {c['technical_class']}")
        if c.get("provenance") not in PROVENANCE:
            raise QualifyError(f"{cid}: provenance must be one of {PROVENANCE}")
        # Commercial-derived code and reconstructions are never clean truth, whatever their licence or build outcome.
        if c["provenance"] != "authored" and c["class"] in ("QUALIFIED", "PARTIALLY_QUALIFIED"):
            raise QualifyError(f"{cid}: {c['provenance']} source cannot be {c['class']}")
        if c["licence_status"] not in ("declared", "undeclared"):
            raise QualifyError(f"{cid}: bad licence status")
        # An undeclared licence never silently passes as usable: a technically usable candidate must then carry the review class.
        if (c["licence_status"] == "undeclared" and c["technical_class"] in ("QUALIFIED", "PARTIALLY_QUALIFIED")
                and c["class"] != "LICENSING_REVIEW_REQUIRED"):
            raise QualifyError(f"{cid}: undeclared licence but class {c['class']}")
        if c["class"] in ("QUALIFIED", "PARTIALLY_QUALIFIED") and not any(r.get("expect", "success") == "success" for r in c["recipes"]):
            raise QualifyError(f"{cid}: usable class without a successful recipe")
        if not c.get("rationale"):
            raise QualifyError(f"{cid}: missing rationale")
        for r in c["recipes"]:
            if r["toolchain"] not in toolchains:
                raise QualifyError(f"{cid}/{r['name']}: unknown toolchain {r['toolchain']}")
            if r.get("expect", "success") not in ("success", "failure"):
                raise QualifyError(f"{cid}/{r['name']}: bad expectation")
            if r.get("expect") == "failure" and not r.get("failure_signature"):
                raise QualifyError(f"{cid}/{r['name']}: expected failure without signature")
    tier_a = [c for c in plan["candidates"] if c["tier"] == "A"]
    if len(tier_a) != 11:
        raise QualifyError("the frozen Tier A inventory has eleven candidates")


# ---------------------------------------------------------------- privacy guard

def require_private(path: pathlib.Path) -> pathlib.Path:
    """The cache holds third-party sources and binaries: it must be outside the product repository or git-ignored by it."""
    p = path.resolve()
    try:
        p.relative_to(ROOT)
    except ValueError:
        inside = False
    else:
        inside = True
    probe = p
    while not probe.exists():
        probe = probe.parent
    top = subprocess.run(["git", "-C", str(probe if probe.is_dir() else probe.parent), "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if top.returncode == 0 or inside:
        repo = pathlib.Path(top.stdout.strip()) if top.returncode == 0 else ROOT
        ign = subprocess.run(["git", "-C", str(repo), "check-ignore", "-q", str(p)], capture_output=True)
        if ign.returncode != 0:
            raise QualifyError(f"private cache {p} is inside git work tree {repo} and not ignored")
    return p


# ---------------------------------------------------------------- executable probes

def probe_format(data: bytes) -> dict:
    """Identify the executable/object container of an output (header-level only)."""
    if len(data) >= 28 and data[:2] == b"\x60\x1a":
        t, d, b, s = struct.unpack(">IIII", data[2:18])
        return {"format": "tos-prg", "text": t, "data": d, "bss": b, "symbols": s}
    if len(data) >= 4 and data[:4] == b"\x00\x00\x03\xf3":
        return {"format": "amiga-hunk-exe"}
    if len(data) >= 4 and data[:4] == b"\x00\x00\x03\xe7":
        return {"format": "amiga-hunk-object"}
    if len(data) >= 20 and data[:4] == b"\x7fELF":
        if data[4] != 1 or data[5] != 2:
            return {"format": "elf-other"}
        etype, machine = struct.unpack(">HH", data[16:20])
        kind = {1: "relocatable", 2: "executable"}.get(etype, "other")
        return {"format": "elf-m68k-" + kind} if machine == 4 else {"format": "elf-other"}
    if data[:8] == b"!<arch>\n":
        return {"format": "ar-archive"}
    if len(data) >= 2 and data[:2] == b"HU":
        return {"format": "x68k-x"}
    if len(data) >= 4 and (struct.unpack(">I", data[:4])[0] & 0xFFFF) in (0x0107, 0x0108, 0x010B):
        return {"format": "aout"}
    if data[:1] == b":" and re.match(rb"^:[0-9A-Fa-f]{10}", data[:11] or b""):
        return {"format": "intel-hex"}
    return {"format": "raw"}


def code_digest(data: bytes) -> str:
    """Digest of everything loadable: for a TOS PRG every byte except the symbol table (header sizes and flags, text, data and the
    relocation table are covered; symbols may carry build-time names); otherwise every byte."""
    fmt = probe_format(data)
    if fmt["format"] == "tos-prg":
        start = 28 + fmt["text"] + fmt["data"]
        return hashlib.sha256(data[:start] + data[start + fmt["symbols"]:]).hexdigest()
    return hashlib.sha256(data).hexdigest()


def cpu_variant_files(root: pathlib.Path) -> int:
    n = 0
    for dp, _dns, fns in os.walk(root):
        if ".git" in dp.split(os.sep):
            continue
        for fn in fns:
            if fn.lower().endswith(SOURCE_EXTS):
                try:
                    text = (pathlib.Path(dp) / fn).read_text(encoding="latin-1")
                except OSError:
                    continue
                if CPU_VARIANT_RE.search(text):
                    n += 1
    return n


def select_pilot(tree: list[str], root: str, size: int, seed: int, max_files: int) -> list[tuple[str, str, str]]:
    """Deterministic pilot: subprojects (directories) under `root` holding one assembly source and one shipped TOS executable."""
    dirs: dict[str, list[str]] = {}
    for path in tree:
        parts = path.split("/")
        if parts[0] != root or len(parts) < 4:
            continue
        dirs.setdefault("/".join(parts[:-1]), []).append(path)
    sel = []
    for d, files in sorted(dirs.items()):
        src = [f for f in files if f.lower().endswith(".s")]
        exe = [f for f in files if f.lower().rsplit(".", 1)[-1] in ("prg", "tos", "ttp")]
        if len(src) == 1 and len(exe) == 1 and len(files) <= max_files:
            sel.append((d, src[0], exe[0]))
    random.Random(seed).shuffle(sel)
    return sel[:size]


# ---------------------------------------------------------------- fetch

def _git(*args: str, cwd: pathlib.Path) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def fetch(plan: dict, cache: pathlib.Path, only: set[str] | None) -> None:
    for c in plan["candidates"]:
        if only and c["id"] not in only:
            continue
        dst = cache / "src" / c["id"]
        dst.mkdir(parents=True, exist_ok=True)
        if not (dst / ".git").exists():
            _git("init", "-q", cwd=dst)
            _git("remote", "add", "origin", f"https://github.com/{c['repo']}", cwd=dst)
        if c.get("fetch") == "archive-pilot":
            _git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", c["commit"], cwd=dst)
            tree = _git("ls-tree", "-r", "--name-only", "FETCH_HEAD", cwd=dst).splitlines()
            p = c["pilot"]
            pilot = select_pilot(tree, p["root"], p["size"], p["seed"], p["max_files"])
            _git("sparse-checkout", "init", "--no-cone", cwd=dst)
            subprocess.run(["git", "sparse-checkout", "set", "--no-cone", "--stdin"], cwd=dst, check=True, text=True,
                           input="".join(f"/{d}/\n" for d, _s, _e in pilot))
            (cache / "pilot").mkdir(exist_ok=True)
            (cache / "pilot" / f"{c['id']}.tsv").write_text("".join("\t".join(x) + "\n" for x in pilot), encoding="utf-8")
        else:
            _git("fetch", "-q", "--depth", "1", "origin", c["commit"], cwd=dst)
        _git("-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD", cwd=dst)
        head = _git("rev-parse", "HEAD", cwd=dst).strip()
        if head != c["commit"]:
            raise QualifyError(f"{c['id']}: HEAD {head} != pinned {c['commit']}")
        print(f"fetch {c['id']}: {head}")


# ---------------------------------------------------------------- toolchains

def _sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def image(tid: str) -> str:
    return f"{IMAGE_PREFIX}{tid}:1"


VERSION_PROBES = {
    "tc-gnu": "dpkg-query -W binutils-m68k-linux-gnu; m68k-linux-gnu-as --version | head -1; vasmm68k_mot 2>&1 | head -1; vlink 2>&1 | head -1",
    "tc-elf": "m68k-elf-gcc --version | head -1; m68k-elf-as --version | head -1",
    "tc-vbcc": "echo 'int x;' > /tmp/v.c; vbccm68k /tmp/v.c -o=/tmp/v.s 2>&1 | head -2; vasmm68k_mot 2>&1 | head -1",
    "tc-a68k": "a68k 2>&1 | grep -i version | head -1",
    "tc-mint": "m68k-atari-mint-gcc --version | head -1; m68k-atari-mint-as --version | head -1; vasmm68k_mot 2>&1 | head -1",
}


def toolchains(plan: dict, cache: pathlib.Path) -> None:
    out = {}
    order = list(plan["toolchains"])
    order.sort(key=lambda t: (plan["toolchains"][t].get("base") is not None, plan["toolchains"][t].get("base") or "", t))
    done: set[str] = set()
    while len(done) < len(order):
        for tid in order:
            tc = plan["toolchains"][tid]
            if tid in done or (tc.get("base") and tc["base"] not in done):
                continue
            if "context_candidate" in tc:
                ctx = cache / "src" / tc["context_candidate"]
                cmd = ["docker", "build", "-q", "-t", image(tid), str(ctx)]
            else:
                ctx = cache / "toolchains" / tid
                ctx.mkdir(parents=True, exist_ok=True)
                for item in tc.get("inputs", []):
                    f = ctx / item["name"]
                    if not f.exists() or _sha256(f) != item["sha256"]:
                        urllib.request.urlretrieve(item["url"], f)
                    if _sha256(f) != item["sha256"]:
                        raise QualifyError(f"{tid}: {item['name']} SHA-256 mismatch")
                shutil.copy(ROOT / tc["dockerfile"], ctx / "Dockerfile")
                cmd = ["docker", "build", "-q", "-t", image(tid), str(ctx)]
                if tc.get("base"):
                    cmd[3:3] = ["--build-arg", f"BASE={image(tc['base'])}"]
            subprocess.run(cmd, check=True, capture_output=True, text=True)
            ident = subprocess.run(["docker", "image", "inspect", image(tid), "--format", "{{.Id}}"], check=True,
                                   capture_output=True, text=True).stdout.strip()
            ver = subprocess.run(["docker", "run", "--rm", "--network", "none", "--entrypoint", "/bin/bash", image(tid), "-c",
                                  VERSION_PROBES.get(tid, "true")], capture_output=True, text=True).stdout
            out[tid] = {"image_id": ident, "versions": [v.strip() for v in ver.splitlines() if v.strip()][:8]}
            done.add(tid)
            print(f"toolchain {tid}: {ident[:19]}")
    (cache / "toolchains.json").write_text(json.dumps(out, indent=1, sort_keys=True) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- build

def _run_recipe(cache: pathlib.Path, cand: dict, recipe: dict, run: str) -> dict:
    src = cache / "src" / cand["id"]
    work = cache / "build" / cand["id"] / recipe["name"] / run
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(src, work, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    if cand.get("fetch") == "archive-pilot":
        shutil.copy(cache / "pilot" / f"{cand['id']}.tsv", work / "pilot.tsv")
    xplat = cache / "xplat"
    xplat.mkdir(exist_ok=True)
    (xplat / "ci_alias.py").write_text(CI_ALIAS, encoding="utf-8")
    (xplat / "asts_pilot.py").write_text(ASTS_PILOT, encoding="utf-8")
    (xplat / "bin").mkdir(exist_ok=True)
    (xplat / "bin" / "date").write_text(DATE_SHIM, encoding="utf-8")
    os.chmod(xplat / "bin" / "date", 0o755)
    mount = cand.get("mount", "/w")
    script = recipe["script"]
    if cand.get("shim") == "ci_alias":
        script = f"python3 /xplat/ci_alias.py . && {{ {script}; }}"
    script = f"export PATH=/xplat/bin:$PATH XPLAT_DATE='{RUN_DATES[run]}'; {script}"
    cmd = ["docker", "run", "--rm", "--network", "none", "-v", f"{work}:{mount}", "-w", mount, "-v", f"{xplat}:/xplat:ro",
           "--entrypoint", "/bin/bash", image(recipe["toolchain"]), "-c", script]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=1800)
        rc, log = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        rc, log = 124, "TIMEOUT"
    (work.parent / f"{run}.log").write_text(log, encoding="utf-8")
    outputs = []
    raw: dict[str, bytes] = {}
    for pattern in recipe.get("outputs", []):
        for f in sorted(glob.glob(str(work / pattern))):
            p = pathlib.Path(f)
            if p.is_file() and not p.is_symlink():
                data = p.read_bytes()
                raw[str(p.relative_to(work))] = data
                outputs.append({"path": str(p.relative_to(work)), "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                                "code_sha256": code_digest(data), **probe_format(data)})
    compares = []
    for ours, theirs in recipe.get("author_compare", []):
        a, b = work / ours, work / theirs
        compares.append({"ours": ours, "author": theirs, "identical": a.is_file() and b.is_file() and a.read_bytes() == b.read_bytes()})
    pilot = {}
    if (work / "pilot.result.tsv").exists():
        for line in (work / "pilot.result.tsv").read_text().splitlines():
            k, v = line.split("\t")
            pilot[k] = int(v)
    today = datetime.datetime.now(datetime.timezone.utc).date()
    days = sorted({today, datetime.date.today(), today - datetime.timedelta(days=1)})
    return {"rc": rc, "log": log, "outputs": outputs, "author_compare": compares, "pilot": pilot, "clock_leaks": clock_leaks(raw, days)}


RECIPE_KEYS = ("name", "toolchain", "script", "outputs", "expect", "failure_signature", "min_outputs", "volatile_outputs", "author_compare")


def recipe_digest(plan: dict, cand: dict, recipe: dict) -> str:
    """Binds a record to the exact recipe, candidate pin, mount/shim and toolchain definition that produced it."""
    tc = plan["toolchains"][recipe["toolchain"]]
    body = {"commit": cand["commit"], "mount": cand.get("mount"), "shim": cand.get("shim"), "pilot": cand.get("pilot"),
            "recipe": {k: recipe.get(k) for k in RECIPE_KEYS},
            "toolchain": {k: tc.get(k) for k in ("dockerfile", "inputs", "base", "context_candidate")},
            "run_dates": RUN_DATES}
    return hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()


IDENTITY_V2_SCHEMA = "segarecomp.xplat_qualify.recipe_identity.v2"
IDENTITY_V2 = ROOT / "tools" / "segarecomp_xplat_qualify.identity_v2.json"
HELPERS = {"ci_alias.py": CI_ALIAS, "asts_pilot.py": ASTS_PILOT, "bin/date": DATE_SHIM}


def toolchain_definition_v2(plan: dict, tid: str, product_root: pathlib.Path, cache: pathlib.Path | None, image_ids: dict) -> dict:
    """SEG-075 C1 (ADR 0160): the toolchain inputs v1 did not bind - Dockerfile CONTENTS (the committed file, or the candidate's own
    Dockerfile for a context-candidate image), the base chain, and the actual image id recorded when the image was built."""
    tc = plan["toolchains"][tid]
    d = {"inputs": tc.get("inputs"), "image_id": image_ids.get(tid, {}).get("image_id")}
    if "dockerfile" in tc:
        d["dockerfile_sha256"] = hashlib.sha256((product_root / tc["dockerfile"]).read_bytes()).hexdigest()
    else:
        cand = next(c for c in plan["candidates"] if c["id"] == tc["context_candidate"])
        df = (cache / "src" / cand["id"] / "Dockerfile") if cache else None
        d["context_candidate"] = {"id": cand["id"], "commit": cand["commit"],
                                  "dockerfile_sha256": hashlib.sha256(df.read_bytes()).hexdigest() if df and df.is_file() else None}
    if tc.get("base"):
        d["base"] = toolchain_definition_v2(plan, tc["base"], product_root, cache, image_ids)
    return d


def recipe_identity_v2(plan: dict, cand: dict, recipe: dict, product_root: pathlib.Path = ROOT, cache: pathlib.Path | None = None,
                       image_ids: dict | None = None, helpers: dict | None = None) -> dict:
    """Versioned v2 identity of one recipe (pure given its inputs; unit-tested). It keeps the frozen v1 digest unchanged and adds the
    Dockerfile contents, the embedded helper scripts and the actual image identity. A v1 digest is never recomputed with v2 inputs."""
    helpers = HELPERS if helpers is None else helpers
    image_ids = image_ids or {}
    body = {"v1_recipe_sha256": recipe_digest(plan, cand, recipe),
            "toolchain": toolchain_definition_v2(plan, recipe["toolchain"], product_root, cache, image_ids),
            "helpers_sha256": {k: hashlib.sha256(v.encode()).hexdigest() for k, v in sorted(helpers.items())}}
    return {"schema": IDENTITY_V2_SCHEMA, "sha256": hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest(), **body}


def identity_document(plan: dict, cache: pathlib.Path | None, image_ids: dict) -> dict:
    """Sanitized v2 identity of every recipe (identities only), bound to the frozen v1 digests."""
    rows = {}
    for c in plan["candidates"]:
        for r in c["recipes"]:
            ident = recipe_identity_v2(plan, c, r, ROOT, cache, image_ids)
            rows[f"{c['id']}/{r['name']}"] = {"v1_recipe_sha256": ident["v1_recipe_sha256"], "v2_identity_sha256": ident["sha256"],
                                              "toolchain": r["toolchain"]}
    return {"schema": IDENTITY_V2_SCHEMA, "task": "SEG-075-T002 (C1)", "adr": "0160",
            "note": "v1 recipe digests are the frozen T001 values (ADR 0159) and are not rewritten; v2 additionally binds Dockerfile contents, "
                    "embedded helpers and image ids, and was introduced after T001 (T001 did not use it)",
            "helpers_sha256": {k: hashlib.sha256(v.encode()).hexdigest() for k, v in sorted(HELPERS.items())},
            "image_ids": {t: v.get("image_id") for t, v in sorted(image_ids.items())},
            "recipes": rows}


def compare_identity(committed: dict, fresh: dict) -> list[str]:
    """Drift between two v2 identity documents (pure; unit-tested)."""
    diffs = []
    for k in ("helpers_sha256", "image_ids"):
        if committed.get(k) != fresh.get(k):
            diffs.append(f"{k} differs")
    a, b = committed.get("recipes", {}), fresh.get("recipes", {})
    for key in sorted(set(a) | set(b)):
        if key not in a or key not in b:
            diffs.append(f"{key}: present in only one identity document")
        elif a[key] != b[key]:
            diffs.append(f"{key}: " + ", ".join(f for f in ("v1_recipe_sha256", "v2_identity_sha256", "toolchain") if a[key].get(f) != b[key].get(f))
                         + " differs")
    return diffs


def evaluate_recipe(recipe: dict, a: dict, b: dict | None) -> dict:
    """Outcome of one recipe from its two runs (pure; unit-tested)."""
    expect = recipe.get("expect", "success")
    patterns = recipe.get("outputs", [])
    if expect == "failure":
        sig = re.compile(recipe["failure_signature"])
        ok = a["rc"] != 0 and bool(sig.search(a["log"]))
        return {"name": recipe["name"], "toolchain": recipe["toolchain"], "expect": expect, "observed": "failure" if a["rc"] else "success",
                "matches_expectation": ok}
    matched = all(any(fnmatch.fnmatch(o["path"], pat) for o in a["outputs"]) for pat in patterns)
    ok = a["rc"] == 0 and matched and len(a["outputs"]) >= recipe.get("min_outputs", 1 if patterns else 0)
    if recipe.get("author_compare"):
        ok = ok and all(c["identical"] for c in a["author_compare"])
    volatile = recipe.get("volatile_outputs", {})

    def stable(run: dict) -> list[tuple[str, str]]:
        return [(o["path"], o.get("code_sha256", o["sha256"])) for o in run["outputs"] if o["path"] not in volatile]

    deterministic = b is not None and stable(a) == stable(b)
    byte_identical_runs = b is not None and [(o["path"], o["sha256"]) for o in a["outputs"]] == [(o["path"], o["sha256"]) for o in b["outputs"]]
    formats: dict[str, int] = {}
    for o in a["outputs"]:
        formats[o["format"]] = formats.get(o["format"], 0) + 1
    res = {"name": recipe["name"], "toolchain": recipe["toolchain"], "expect": expect, "observed": "success" if a["rc"] == 0 else "failure",
           "matches_expectation": ok, "deterministic": deterministic, "byte_identical_runs": byte_identical_runs,
           "output_count": len(a["outputs"]),
           "output_bytes": sum(o["size"] for o in a["outputs"]), "formats": formats,
           "outputs": [{k: o[k] for k in ("path", "size", "sha256", "code_sha256", "format") if k in o} for o in a["outputs"]]}
    if volatile:
        res["volatile_outputs"] = volatile
    leaks = sorted(set(a.get("clock_leaks", [])) | set((b or {}).get("clock_leaks", [])))
    res["undeclared_clock_leaks"] = [x for x in leaks if x not in volatile]
    if a["author_compare"]:
        res["author_identical"] = sum(1 for c in a["author_compare"] if c["identical"])
        res["author_compared"] = len(a["author_compare"])
    if a["pilot"]:
        res["pilot"] = a["pilot"]
    return res


def observed_source(src: pathlib.Path, pinned: str) -> str:
    """The cached checkout must be exactly the pinned commit with no tracked modification; returns the observed HEAD."""
    head = _git("rev-parse", "HEAD", cwd=src).strip()
    dirty = _git("status", "--porcelain", "--ignored", "--untracked-files=all", cwd=src).strip()
    if head != pinned or dirty:
        raise QualifyError(f"{src.name}: cached source is {head}{' (modified)' if dirty else ''}, plan pins {pinned}")
    return head


def build(plan: dict, cache: pathlib.Path, only: set[str] | None) -> None:
    (cache / "records").mkdir(parents=True, exist_ok=True)
    for c in plan["candidates"]:
        if only and c["id"] not in only:
            continue
        head = observed_source(cache / "src" / c["id"], c["commit"])
        recs = []
        for r in c["recipes"]:
            a = _run_recipe(cache, c, r, "a")
            b = _run_recipe(cache, c, r, "b") if r.get("expect", "success") == "success" else None
            res = evaluate_recipe(r, a, b)
            res["recipe_sha256"] = recipe_digest(plan, c, r)
            recs.append(res)
            print(f"build {c['id']}/{r['name']}: expect={res['expect']} observed={res['observed']} ok={res['matches_expectation']}"
                  + (f" deterministic={res.get('deterministic')} outputs={res.get('output_count')}" if res["expect"] == "success" else ""))
        rec = {"id": c["id"], "commit": head, "recipes": recs}
        if c.get("cpu_scan"):
            rec["cpu_variant_source_files"] = cpu_variant_files(cache / "src" / c["id"])
        (cache / "records" / f"{c['id']}.json").write_text(json.dumps(rec, indent=1, sort_keys=True) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- report

def make_manifest(plan: dict, records: dict[str, dict], toolchain_ids: dict) -> dict:
    """Sanitized manifest + contradiction list (pure; unit-tested)."""
    contradictions = []
    cands = []
    for c in plan["candidates"]:
        rec = records.get(c["id"])
        if c["recipes"] and rec is None:
            contradictions.append(f"{c['id']}: no build record")
            rec = {"recipes": []}
        rec = rec or {"recipes": []}
        if "commit" in rec and rec["commit"] != c["commit"]:
            contradictions.append(f"{c['id']}: built from {rec['commit']}, plan pins {c['commit']}")
        planned = {r["name"]: recipe_digest(plan, c, r) for r in c["recipes"]}
        got = {r["name"]: r.get("recipe_sha256") for r in rec["recipes"]}
        for name in sorted(set(planned) - set(got)):
            contradictions.append(f"{c['id']}/{name}: plan recipe has no build record")
        for name in sorted(set(got) - set(planned)):
            contradictions.append(f"{c['id']}/{name}: record of a recipe not in the plan")
        for name in sorted(set(got) & set(planned)):
            if got[name] != planned[name]:
                contradictions.append(f"{c['id']}/{name}: record was built from a different recipe/toolchain definition")
        for r in rec["recipes"]:
            if r.get("undeclared_clock_leaks"):
                contradictions.append(f"{c['id']}/{r['name']}: outputs embed the real build date: {', '.join(r['undeclared_clock_leaks'])}")
            if not r["matches_expectation"]:
                contradictions.append(f"{c['id']}/{r['name']}: expected {r['expect']}, observed {r['observed']}")
            elif r["expect"] == "success" and not r.get("deterministic", False):
                contradictions.append(f"{c['id']}/{r['name']}: outputs differ between the two runs")
        entry = {k: c[k] for k in ("id", "tier", "repo", "commit", "platform", "workload", "provenance", "licence", "licence_status", "class",
                                   "technical_class", "rationale", "label_evidence", "lineage_hypothesis")}
        for k in ("role", "prerequisites", "operator_inputs"):
            if k in c:
                entry[k] = c[k]
        entry["recipes"] = rec["recipes"]
        if "cpu_variant_source_files" in rec:
            entry["cpu_variant_source_files"] = rec["cpu_variant_source_files"]
        cands.append(entry)
    by_class: dict[str, int] = {}
    by_platform: dict[str, dict[str, int]] = {}
    for e in cands:
        by_class[e["class"]] = by_class.get(e["class"], 0) + 1
        plat = by_platform.setdefault(e["platform"], {})
        plat[e["class"]] = plat.get(e["class"], 0) + 1
    usable = [e for e in cands if e["class"] in ("QUALIFIED", "PARTIALLY_QUALIFIED") and e.get("role") != "toolchain-infrastructure"
              and e["provenance"] == "authored"]
    return {
        "schema": "segarecomp.xplat_qualify.manifest.v1",
        "task": plan["task"], "adr": plan["adr"], "frozen": plan["frozen"],
        "toolchains": {t: {"description": tc["description"], **({"inputs": tc["inputs"]} if "inputs" in tc else {}),
                           **toolchain_ids.get(t, {})} for t, tc in plan["toolchains"].items()},
        "summary": {"candidates": len(cands), "by_class": dict(sorted(by_class.items())),
                    "by_platform": {k: dict(sorted(v.items())) for k, v in sorted(by_platform.items())},
                    "usable_without_review": sorted(e["id"] for e in usable),
                    "contradictions": contradictions},
        "candidates": cands,
    }


def compare_manifests(committed: dict, fresh: dict) -> list[str]:
    """Differences of identities, classes, outcomes and output digests between two manifests (pure; unit-tested)."""
    diffs = []
    old = {c["id"]: c for c in committed["candidates"]}
    new = {c["id"]: c for c in fresh["candidates"]}
    for cid in sorted(set(old) | set(new)):
        a, b = old.get(cid), new.get(cid)
        if a is None or b is None:
            diffs.append(f"{cid}: present in only one manifest")
            continue
        for k in ("commit", "class", "technical_class", "provenance"):
            if a.get(k) != b.get(k):
                diffs.append(f"{cid}: {k} {a.get(k)} != {b.get(k)}")
        ra = {r["name"]: r for r in a["recipes"]}
        rb = {r["name"]: r for r in b["recipes"]}
        for name in sorted(set(ra) | set(rb)):
            x, y = ra.get(name), rb.get(name)
            if x is None or y is None:
                diffs.append(f"{cid}/{name}: recipe present in only one manifest")
                continue
            if x.get("recipe_sha256") != y.get("recipe_sha256"):
                diffs.append(f"{cid}/{name}: recipe definition differs")
            if x.get("observed") != y.get("observed"):
                diffs.append(f"{cid}/{name}: outcome {x.get('observed')} != {y.get('observed')}")
            vol = set(x.get("volatile_outputs", {}))
            dx = {(o["path"], o.get("code_sha256", o["sha256"])) for o in x.get("outputs", []) if o["path"] not in vol}
            dy = {(o["path"], o.get("code_sha256", o["sha256"])) for o in y.get("outputs", []) if o["path"] not in vol}
            for path, _d in sorted(dx ^ dy):
                diffs.append(f"{cid}/{name}: output {path} differs")
    return diffs


def verify(plan: dict, cache: pathlib.Path, committed_path: pathlib.Path) -> int:
    """Rebuilt records (`build`) must reproduce the committed manifest's identities, classes, outcomes and code digests."""
    fresh = _fresh_manifest(plan, cache)
    diffs = fresh["summary"]["contradictions"] + compare_manifests(json.loads(committed_path.read_text(encoding="utf-8")), fresh)
    for d in diffs:
        print("DIFF:", d)
    print("verify:", "PASS" if not diffs else f"FAIL ({len(diffs)})")
    return 1 if diffs else 0


def _fresh_manifest(plan: dict, cache: pathlib.Path) -> dict:
    records = {}
    for f in sorted((cache / "records").glob("*.json")):
        rec = json.loads(f.read_text(encoding="utf-8"))
        records[rec["id"]] = rec
    tcs = json.loads((cache / "toolchains.json").read_text(encoding="utf-8")) if (cache / "toolchains.json").exists() else {}
    return make_manifest(plan, records, tcs)


def report(plan: dict, cache: pathlib.Path, out: pathlib.Path) -> int:
    manifest = _fresh_manifest(plan, cache)
    out.write_text(json.dumps(manifest, indent=1, sort_keys=False) + "\n", encoding="utf-8")
    for c in manifest["summary"]["contradictions"]:
        print("CONTRADICTION:", c)
    print(json.dumps(manifest["summary"]["by_class"]))
    return 1 if manifest["summary"]["contradictions"] else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("fetch", "toolchains", "build", "report", "verify", "validate", "identity"))
    ap.add_argument("--cache", type=pathlib.Path, help="private ignored cache directory")
    ap.add_argument("--only", action="append", help="restrict to candidate id (repeatable)")
    ap.add_argument("--plan", type=pathlib.Path, default=PLAN)
    ap.add_argument("--out", type=pathlib.Path, default=None,
                    help="report: manifest output path (default: the committed manifest); identity: v2 identity output path")
    ap.add_argument("--manifest", type=pathlib.Path, default=None,
                    help="verify: the manifest the rebuilt records must reproduce (default: the committed manifest)")
    ap.add_argument("--check", action="store_true", help="identity: compare with the committed v2 identity instead of writing")
    args = ap.parse_args(argv)
    try:
        if args.command == "verify" and args.out is not None:
            raise QualifyError("verify reads --manifest; --out is an output path of report/identity and is not accepted by verify")
        if args.command != "verify" and args.manifest is not None:
            raise QualifyError("--manifest is accepted by verify only")
        plan = load_plan(args.plan)
        if args.command == "validate":
            print(f"plan OK: {len(plan['candidates'])} candidates, {len(plan['toolchains'])} toolchains")
            return 0
        if not args.cache:
            raise QualifyError("--cache is required")
        cache = require_private(args.cache)
        cache.mkdir(parents=True, exist_ok=True)
        only = set(args.only) if args.only else None
        if args.command == "fetch":
            fetch(plan, cache, only)
        elif args.command == "toolchains":
            toolchains(plan, cache)
        elif args.command == "build":
            build(plan, cache, only)
        elif args.command == "verify":
            committed = args.manifest or MANIFEST
            if not committed.is_file():
                raise QualifyError(f"verify: manifest {committed} does not exist")
            return verify(plan, cache, committed)
        elif args.command == "identity":
            ids = json.loads((cache / "toolchains.json").read_text(encoding="utf-8")) if (cache / "toolchains.json").exists() else {}
            doc = identity_document(plan, cache, ids)
            target = args.out or IDENTITY_V2
            if args.check:
                diffs = compare_identity(json.loads(target.read_text(encoding="utf-8")), doc)
                for d in diffs:
                    print("DRIFT:", d)
                print("identity:", "PASS" if not diffs else f"FAIL ({len(diffs)})")
                return 1 if diffs else 0
            target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            print(f"identity: {len(doc['recipes'])} recipes -> {target}")
        else:
            return report(plan, cache, args.out or MANIFEST)
        return 0
    except QualifyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
