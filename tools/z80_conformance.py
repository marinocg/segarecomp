#!/usr/bin/env python3
"""SEG-008-T003: Z80 generated-native differential conformance harness (driver).

One reusable harness for every legal Z80 form. Deterministic synthetic vectors (full architectural and internal
initial state, memory image, I/O input script, interrupt-acknowledge script, interrupt-line script) go through
decode -> lower -> emitted strict-C11 -> compile -> generated-native execution (tests/tools/z80_conformance_runner.c),
and the identical vector text runs on the pinned redcode/Z80 oracle (tests/z80_oracle/z80_conformance_oracle.c,
ADR 0057). The two result streams are compared per step: full state, ordered memory-write log, ordered I/O log
(IN, OUT and interrupt-acknowledge transactions) and T-states; the first divergence is reported deterministically
(SEG-020 / ADR 0042 pattern, Z80-local comparator).

Validating a family means adding table rows (tests/fixtures/z80-conformance-vectors/<family>.json), not changing this
file: a row names a legal-form id of the independent dataset (tests/fixtures/z80-legal-forms.json), which supplies
the exact opcode bytes. Scenario vectors (scenarios.json) carry explicit images and interrupt/HALT/prefix scripts.

Oracle policy (ADR 0057, docs/testing/z80-conformance-harness.md): without SEGARECOMP_Z80_ORACLE_CHECKOUT the oracle
side is skipped ("skipped: ..."); credit for `oracle_*` and `timing_validated` comes only from the committed
validation manifests (tests/fixtures/z80-validation-manifest/<family>.json), written by `--update-manifest`, which
refuses to run without the pinned oracle and only ever adds credit. A hermetic test checks manifest freshness.
"""
import argparse
import concurrent.futures
import hashlib
import itertools
import json
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import z80_conformance_sweeps as sweeps  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATASET = ROOT / "tests" / "fixtures" / "z80-legal-forms.json"
VECTOR_DIR = ROOT / "tests" / "fixtures" / "z80-conformance-vectors"
MANIFEST_DIR = ROOT / "tests" / "fixtures" / "z80-validation-manifest"
INCLUDE_DIR = ROOT / "libs" / "codegen" / "c11" / "include"
TOOLS_DIR = ROOT / "tests" / "tools"
ORACLE_DIR = ROOT / "tests" / "z80_oracle"
CHECKOUT_ENV = "SEGARECOMP_Z80_ORACLE_CHECKOUT"
PINS = {"redcode_Z80": "6bb4166317108b8d1a4b5934df15761089bdea9e",
        "redcode_Zeta": "93ba5ab967eef00f074d21bb760fe9dc48afd2d3"}
SECONDARY_PIN = {"kosarev_z80": "4c56dc514b37087751c5d9de29126a0d5c6ec731"}
# Fields the secondary oracle (kosarev/z80, ADR 0057 decision 2) does not model at all: the NMOS Q register, the LD A,I/R
# marker, the in-prefix-run state and the NMI reject latch print as 0 and are never compared against it.
SECONDARY_UNMODELLED = {"q", "ldair", "prefix_run", "nmireject"}
# Committed deviation mask (ADR 0057 decision 2; the corpus and redcode agree with the generated side on every one of
# these): {opcode-file stem or stem prefix -> (family, masked fields)}. Anything else that differs is unexplained.
SECONDARY_MASK = {
    "scf_ccf": ({"37", "3f", "dd_37", "dd_3f", "fd_37", "fd_3f"}, {"f"}),          # kosarev has no Zilog NMOS Q rule
    "block_io_memptr_flags": ({"ed_a2", "ed_aa", "ed_b2", "ed_ba", "ed_b3", "ed_bb"}, {"f", "wz"}),  # INI/IND/OTIR/OTDR family
    "repeating_block": ({"ed_b0", "ed_b1", "ed_b8", "ed_b9"}, {"f", "wz"}),        # repeating-step LDxR/CPxR flags, MEMPTR
}
# The same deviations by legal-form id for the synthetic form vectors: {form-id prefix -> masked cpu fields}.
SECONDARY_FORM_MASK = {"ccf.": {"f"}, "scf.": {"f"}, "ldir.": {"f", "wz"}, "lddr.": {"f", "wz"}, "cpir.": {"f", "wz"},
                       "cpdr.": {"f", "wz"}, "ini.": {"f", "wz"}, "ind.": {"f", "wz"}, "inir.": {"f", "wz"},
                       "indr.": {"f", "wz"}, "otir.": {"f", "wz"}, "otdr.": {"f", "wz"},
                       # kosarev models no RETI/RETN deferral bit (ADR 0057 unresolved item 6)
                       "reti.": {"deferral"}, "retn.": {"deferral"}}
ORACLE_DEFINES = ["-DZ80_STATIC", "-DZ80_WITH_EXECUTE", "-DZ80_WITH_Q", "-DZ80_WITH_FULL_IM0", "-DZ80_WITH_SPECIAL_RESET",
                  "-DZ80_WITH_UNOFFICIAL_RETI", "-DZ80_WITH_ZILOG_NMOS_LD_A_IR_BUG"]
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
SCHEMA = 1
SLOT_BASE = 0x0100
MAX_CODE = 0x2000          # code bytes per slot batch (bounded emitted C per batch)
MAX_STEPS = 64
FAMILIES = ("data_alu", "control_stack", "cb_bit_prefix", "ed_io_interrupt")

CPU_FIELDS = ["a", "f", "b", "c", "d", "e", "h", "l", "a2", "f2", "b2", "c2", "d2", "e2", "h2", "l2", "ix", "iy", "sp",
              "pc", "wz", "i", "r", "im", "iff1", "iff2", "q", "halted", "deferral", "ldair", "prefix_run", "nmireject"]
DECIMAL = {"im", "iff1", "iff2", "halted", "deferral", "ldair", "prefix_run", "nmireject"}
REG_ORDER = ["a", "f", "b", "c", "d", "e", "h", "l", "a2", "f2", "b2", "c2", "d2", "e2", "h2", "l2"]
PAIR_ORDER = ["ix", "iy", "sp", "pc", "wz"]
X_ORDER = ["i", "r", "im", "iff1", "iff2", "q", "halted", "deferral", "ldair", "prefix_run", "nmireject"]
WIDTH = {k: 4 if k in PAIR_ORDER else 2 for k in CPU_FIELDS}


# ------------------------------------------------------------------------------------------------ dataset / rows
def load_dataset():
    data = json.loads(DATASET.read_text(encoding="utf-8"))
    forms = {}
    for row in data["forms"]:
        f = dict(zip(data["form_columns"], row))
        forms[f["id"]] = f
    return data, forms


def expand_ranges(ranges):
    for lo, hi in ranges:
        yield from range(lo, hi + 1)


def rng(*parts):
    """Deterministic value source: 64 bits from sha256 of the parts."""
    digest = hashlib.sha256("|".join(str(p) for p in parts).encode()).digest()
    return int.from_bytes(digest[:8], "big")


def profile_state(profile, key):
    """Initial architectural and internal state of a value profile (all values within their width)."""
    s = {k: 0 for k in CPU_FIELDS}
    if profile == "zero":
        s["sp"] = 0xE000
    elif profile == "ones":
        for k in CPU_FIELDS:
            s[k] = (1 << (4 * WIDTH[k])) - 1
        s.update(im=2, iff1=1, iff2=1, halted=0, deferral=0, ldair=0, prefix_run=0, nmireject=0)
    elif profile == "edge":
        pattern = [0x80, 0x7F, 0x01, 0xFE, 0x55, 0xAA, 0x00, 0xFF]
        for n, k in enumerate(REG_ORDER):
            s[k] = pattern[(n + rng(key, "edge") % 8) % 8]
        s.update(ix=0x7FFF, iy=0x8000, sp=0x8000, wz=0x0100, i=0x80, r=0x7F, q=0x80, im=1, iff1=1, iff2=0)
    elif profile == "mixed":
        for k in CPU_FIELDS:
            s[k] = rng(key, profile, k) % (1 << (4 * WIDTH[k]))
        s["im"] = rng(key, "im") % 3
        s.update(iff1=rng(key, "iff1") & 1, iff2=rng(key, "iff2") & 1, halted=0, deferral=0, ldair=0, prefix_run=0,
                 nmireject=0)
    else:
        raise SystemExit("unknown value profile %r" % profile)
    return s


def touch_points(state):
    """Addresses whose memory a vector initialises: BC, DE, HL, IX, IY and SP (four bytes each)."""
    bc = state["b"] << 8 | state["c"]
    de = state["d"] << 8 | state["e"]
    hl = state["h"] << 8 | state["l"]
    return [bc, de, hl, state["ix"], state["iy"], state["sp"]]


class Vec:
    """One vector definition, independent of placement (its digest excludes the slot address)."""

    def __init__(self, name, code, state, steps, form=None, patches=(), inb=b"", ack=b"", maps=None, expect=None,
                 oracle=True, image=None, load=(), same_final_as=None, family=None):
        self.name, self.code, self.state, self.steps = name, bytes(code), dict(state), list(steps)
        self.form, self.patches, self.inb, self.ack = form, [(a, bytes(b)) for a, b in patches], bytes(inb), bytes(ack)
        self.maps, self.expect, self.oracle = maps or {}, expect, oracle
        self.image, self.load, self.same_final_as, self.family = image, list(load), same_final_as, family

    def digest_material(self):
        state = {k: v for k, v in self.state.items() if k != "pc"} if self.image is None else dict(self.state)
        return {"code": self.code.hex(), "state": state, "steps": self.steps, "form": self.form,
                "patches": [[a, b.hex()] for a, b in self.patches], "in": self.inb.hex(), "ack": self.ack.hex(),
                "maps": self.maps, "load": self.load}


def encoding_code(form, opcode_byte, operands):
    """Concrete instruction bytes of a form from the dataset layout tokens."""
    prefix = {"CB": 0xCB, "ED": 0xED, "DD": 0xDD, "FD": 0xFD}
    out, ops = [], list(operands)
    for token in form["layout"].split():
        if token in prefix:
            out.append(prefix[token])
        elif token == "op":
            out.append(opcode_byte)
        else:  # d, n, e, nn_lo, nn_hi
            out.append(ops.pop(0))
    return bytes(out)


def operand_count(form):
    return sum(1 for t in form["layout"].split() if t not in ("CB", "ED", "DD", "FD", "op"))


def expand_rows(doc, data, forms):
    """Vector definitions of one table document (`rows`)."""
    vecs = []
    partition = data["space_partition"]["spaces"]
    for row_index, row in enumerate(doc["rows"]):
        form = forms[row["form"]]
        n = operand_count(form)
        default_ops = [bytes([0xA5] * n).hex(), bytes([0x5A] * n).hex()] if n else [""]
        operand_sets = row.get("operands", default_ops)
        byte_filter = {int(b, 16) for b in row["bytes"]} if "bytes" in row else None
        step_spec = row.get("steps", [{"mode": "i", "budget": 1, "int": 0, "nmi": 0}])
        cases = sweeps.sweep_cases(row["sweep"]) if "sweep" in row else [None]
        steps = expand_steps(step_spec)
        for prefix in row.get("prefixes", [""]):
            pbytes = bytes.fromhex(prefix)
            for b in expand_ranges(form["byte_ranges"]):
                if byte_filter is not None and b not in byte_filter:
                    continue
                if pbytes:  # a DD/FD chain before a base-space byte: only prefix-ignored bytes keep the base form
                    if form["space"] in ("ddcb", "fdcb"):  # the form's own DD/FD supersedes every chained prefix
                        pass
                    elif form["space"] != "base" or partition["dd" if pbytes[-1] == 0xDD else "fd"][b] != "P":
                        continue
                for pre, ops, profile, case in itertools.product(row.get("pre", [""]), operand_sets,
                                                                  row.get("profiles", ["zero", "mixed"]), cases):
                    if case is not None and case.get("v") is not None and row.get("sweep_v") == "imm":
                        ops = "%02X" % case["v"]
                    code = bytes.fromhex(pre) + pbytes + encoding_code(form, b, bytes.fromhex(ops))
                    tag = "" if case is None else ":" + sweeps.case_tag(case)
                    key = "%s:%s:%s:%s%s" % (row["form"], prefix, code.hex(), profile, tag)
                    name = "%s/%s/%s/r%d%s" % (row["form"], code.hex(), profile, row_index, tag)
                    state = profile_state(profile, key)
                    mem = None if case is None else sweeps.apply_case(case, state, row.get("sweep_v"))
                    patches = [((p + d) & 0xFFFF, bytes([rng(key, "mem", p, d) & 0xFF if mem is None else mem]))
                               for p in touch_points(state) for d in range(4)]
                    vecs.append(Vec(name, code, state, steps, form=row["form"], patches=patches,
                                    family=form["family"], oracle=row.get("oracle", True)))
    return vecs


def expand_steps(spec):
    steps = []
    for item in spec:
        for _ in range(item.get("repeat", 1)):
            steps.append({"mode": item.get("mode", "i"), "budget": item.get("budget", 1), "int": item.get("int", 0),
                          "nmi": item.get("nmi", 0), "map": item.get("map", 0)})
    if not 1 <= len(steps) <= MAX_STEPS:
        raise SystemExit("a vector has 1..%d steps" % MAX_STEPS)
    return steps


def load_row_documents(directory=VECTOR_DIR):
    docs = {}
    for family in FAMILIES:
        path = directory / (family + ".json")
        if path.is_file():
            doc = json.loads(path.read_text(encoding="utf-8"))
            assert doc["schema"] == SCHEMA and doc["family"] == family, path
            docs[family] = doc
    return docs


def form_vectors(docs=None):
    """All row-expanded vector definitions, grouped by form id, and a check that each row sits in its family."""
    data, forms = load_dataset()
    docs = docs if docs is not None else load_row_documents()
    by_form = {}
    for family, doc in docs.items():
        for vec in expand_rows(doc, data, forms):
            if vec.family != family:
                raise SystemExit("row for %s is in the %s table but belongs to %s" % (vec.form, family, vec.family))
            by_form.setdefault(vec.form, []).append(vec)
    return by_form


def form_digest(vecs):
    text = json.dumps([v.digest_material() for v in vecs], sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode()).hexdigest()


# ------------------------------------------------------------------------------------------------ placement / text
def hex_field(k, v):
    return "%0*X" % (WIDTH[k], v) if k not in DECIMAL else "%X" % v


def vector_text(vec, pc, memory_lines):
    s = dict(vec.state)
    s["pc"] = pc
    lines = ["V " + vec.name,
             "R " + " ".join(hex_field(k, s[k]) for k in REG_ORDER),
             "P " + " ".join(hex_field(k, s[k]) for k in PAIR_ORDER),
             "X " + " ".join("%X" % s[k] for k in X_ORDER)]
    lines += memory_lines
    for addr, data in vec.patches:
        lines.append("M %04X %s" % (addr & 0xFFFF, data.hex()))
    if vec.image is None:
        lines.append("M %04X %s" % (pc, vec.code.hex()))  # code last: it wins over data patches
    if vec.inb:
        lines.append("IN " + vec.inb.hex())
    if vec.ack:
        lines.append("ACK " + vec.ack.hex())
    for set_id, ranges in sorted(vec.maps.items()):
        for lo, hi, ident, base in ranges:
            lines.append("K %s %X %X %d %X" % (set_id, lo, hi, ident, base))
    for st in vec.steps:
        lines.append("S %s %d %d %d map=%d" % (st["mode"], st["budget"], st["int"], st["nmi"], st["map"]))
    lines.append("E")
    return "\n".join(lines) + "\n"


class Batch:
    def __init__(self, name, spec_text, vectors, slots=None):
        self.name, self.spec_text, self.vectors = name, spec_text, vectors  # vectors: [(Vec, text)]
        self.slots = slots or {}  # code bytes -> slot address (slot batches only)

    @property
    def text(self):
        return "".join(t for _, t in self.vectors)


def slot_batches(vecs):
    """Places form-row vectors into invariant-image slots (identity 1, window [0, code_end)); bounded per batch."""
    batches, cur_slots, cur_vecs, end = [], {}, [], SLOT_BASE

    def flush():
        nonlocal cur_slots, cur_vecs, end
        if not cur_vecs:
            return
        code_end = (end + 0xFF) & ~0xFF
        image = bytearray(code_end)
        for code, addr in cur_slots.items():
            image[addr:addr + len(code)] = code
        spec = "image 1 invariant\nwindow 1 0 0 %X\nbytes 1 %s\n" % (code_end, bytes(image).hex())
        texts = [(v, vector_text(v, cur_slots[v.code], [])) for v in cur_vecs]
        batches.append(Batch("slots%02d" % len(batches), spec, texts, dict(cur_slots)))
        cur_slots, cur_vecs, end = {}, [], SLOT_BASE

    for vec in vecs:
        if vec.code not in cur_slots:
            size = (len(vec.code) + 7) & ~7
            if end + size > SLOT_BASE + MAX_CODE:
                flush()
            cur_slots[vec.code] = end
            end += size
        cur_vecs.append(vec)
    flush()
    return batches


# ------------------------------------------------------------------------------------------------ scenarios
def hexint(text):
    return int(text, 16)


def scenario_image_bytes(spec):
    data = bytearray(bytes([hexint(spec.get("fill", "00"))]) * spec.get("size", 0x10000))
    for entry in spec.get("patch", []):  # [address, hex bytes, optional repeat count]
        raw = bytes.fromhex(entry[1]) * (entry[2] if len(entry) > 2 else 1)
        a = hexint(entry[0])
        data[a:a + len(raw)] = raw
    return bytes(data)


def image_spec_text(images):
    lines = []
    for img in images:
        lines.append("image %d %s" % (img["identity"], img["kind"]))
        for base, first, length in img["windows"]:
            lines.append("window %d %s %s %s" % (img["identity"], base, first, length))
        lines.append("bytes %d %s" % (img["identity"], scenario_image_bytes(img).hex()))
    return "\n".join(lines) + "\n"


def memory_lines_for_load(image_doc, load):
    """`F`/`M` lines that place the exposed range of image `identity` at `window_base`."""
    lines = []
    by_id = {i["identity"]: i for i in image_doc["images"]}
    for identity, base_hex in load:
        img = by_id[identity]
        raw = scenario_image_bytes(img)
        base = hexint(base_hex)
        first = hexint(img["windows"][0][1])
        length = hexint(img["windows"][0][2])
        fill = hexint(img.get("fill", "00"))
        lines.append("F %04X %X %02X" % ((base + first) & 0xFFFF, length, fill))
        run = None
        for off in range(first, first + length):
            if raw[off] != fill:
                if run is None:
                    run = [off, bytearray()]
                run[1].append(raw[off])
            elif run is not None:
                lines.append("M %04X %s" % ((base + run[0]) & 0xFFFF, bytes(run[1]).hex()))
                run = None
        if run is not None:
            lines.append("M %04X %s" % ((base + run[0]) & 0xFFFF, bytes(run[1]).hex()))
    return lines


def load_scenarios(path=None):
    path = path or VECTOR_DIR / "scenarios.json"
    doc = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    assert doc["schema"] == SCHEMA
    return doc


def scenario_batches(doc):
    """One batch per named image; scenarios keep declaration order."""
    batches = []
    for image_name, image_doc in doc["images"].items():
        texts = []
        for sc in doc["scenarios"]:
            if sc["image"] != image_name:
                continue
            state = profile_state(sc.get("profile", "zero"), sc["name"])
            for k, v in sc.get("state", {}).items():
                state[k] = hexint(v) if k not in DECIMAL else int(v)
            maps = {str(set_id): [[hexint(lo), hexint(hi), ident, hexint(base)] for lo, hi, ident, base in ranges]
                    for set_id, ranges in sc.get("maps", {}).items()}
            vec = Vec(sc["name"], b"", state, expand_steps(sc["steps"]), inb=bytes.fromhex(sc.get("in", "")),
                      ack=bytes.fromhex(sc.get("ack", "")), maps=maps, expect=sc.get("expect"),
                      oracle=sc.get("oracle", True), image=image_name, load=sc.get("load", []),
                      same_final_as=sc.get("same_final_as"))
            vec.patches = [(hexint(a), bytes.fromhex(b)) for a, b in sc.get("memory", [])]
            texts.append((vec, vector_text(vec, state["pc"], memory_lines_for_load(image_doc, sc.get("load", [])))))
        if texts:
            batches.append(Batch(image_name, image_spec_text(image_doc["images"]), texts))
    return batches


# ------------------------------------------------------------------------------------------------ build / run
class Toolchain:
    def __init__(self, cc, emitter, oracle_checkout=None, opt="-O0", include_dir=None):
        self.cc, self.emitter, self.oracle_checkout, self.opt = cc, emitter, oracle_checkout, opt
        self.include_dir = include_dir or INCLUDE_DIR  # a test may shadow the ABI header with a mutated copy


def run(cmd, **kw):
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def emit_image(tc, spec_text, workdir, stem="z80_image", list_owners=False):
    """Runs the public emitter over an image spec. Returns (stats dict, owner records, error)."""
    workdir = pathlib.Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    spec = workdir / (stem + ".spec")
    spec.write_text(spec_text, encoding="utf-8")
    cmd = [str(tc.emitter), str(spec), str(workdir), stem] + (["--list"] if list_owners else [])
    out = run(cmd)
    if out.returncode != 0:
        return None, [], (out.stdout + out.stderr).strip() or "emitter failed"
    stats, owners = {}, []
    for line in out.stdout.splitlines():
        if line.startswith("stats "):
            stats = {k: int(v) for k, v in (t.split("=") for t in line.split()[1:])}
        elif line.startswith("owner "):
            _, ident, key, kind, variants, form, rel, bound = line.split()
            owners.append({"identity": int(ident), "key": int(key, 16), "kind": kind, "variants": int(variants),
                           "form": form, "relative": rel == "1", "bound": bound == "1"})
    return stats, owners, None


def compile_units(tc, workdir, stem, extra_sources=(), extra_flags=()):
    """Compiles every generated TU plus extra sources as strict C11 in parallel and links an executable."""
    workdir = pathlib.Path(workdir)
    units = (workdir / (stem + ".units")).read_text().split()
    sources = [workdir / u for u in units] + [pathlib.Path(s) for s in extra_sources]
    objs = [workdir / (s.stem + ".o") for s in sources]
    common = [tc.cc, *STRICT, tc.opt, "-I", str(tc.include_dir), "-I", str(INCLUDE_DIR), "-I", str(workdir), "-I", str(TOOLS_DIR), *extra_flags]

    def one(pair):
        src, obj = pair
        r = run(common + ["-c", str(src), "-o", str(obj)])
        return r.returncode, (r.stderr or r.stdout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(one, zip(sources, objs)))
    for code, message in results:
        if code != 0:
            return None, message
    exe = workdir / (stem + ".exe")
    linked = run([tc.cc, *[str(o) for o in objs], "-o", str(exe)])
    if linked.returncode != 0:
        return None, linked.stderr
    return exe, None


def build_generated(tc, spec_text, workdir, stem="z80_image", list_owners=False):
    stats, owners, error = emit_image(tc, spec_text, workdir, stem, list_owners)
    if error:
        return {"error": "emit: " + error}
    exe, error = compile_units(tc, workdir, stem, extra_sources=[TOOLS_DIR / "z80_conformance_runner.c"])
    if error:
        return {"error": "compile: " + error, "stats": stats, "owners": owners}
    return {"exe": exe, "stats": stats, "owners": owners, "error": None}


def oracle_checkout():
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return None
    root = pathlib.Path(configured)
    if not (root / "redcode_Z80" / "sources" / "Z80.c").is_file():
        return None
    for name, pin in PINS.items():
        head = run(["git", "-C", str(root / name), "rev-parse", "HEAD"])
        if head.returncode != 0 or head.stdout.strip() != pin:
            raise AssertionError("%s checkout is not the pinned revision %s" % (name, pin))
        if run(["git", "-C", str(root / name), "diff", "--quiet", "HEAD", "--"]).returncode != 0:
            raise AssertionError("%s pinned checkout has local modifications" % name)
    return root


def secondary_checkout(root):
    """The pinned kosarev/z80 checkout under `root`, or None when it is absent. A wrong HEAD or a dirty tree is a hard failure."""
    if root is None or not (pathlib.Path(root) / "kosarev_z80" / "z80.h").is_file():
        return None
    for name, pin in SECONDARY_PIN.items():
        head = run(["git", "-C", str(pathlib.Path(root) / name), "rev-parse", "HEAD"])
        if head.returncode != 0 or head.stdout.strip() != pin:
            raise AssertionError("%s checkout is not the pinned revision %s" % (name, pin))
        if run(["git", "-C", str(pathlib.Path(root) / name), "diff", "--quiet", "HEAD", "--"]).returncode != 0:
            raise AssertionError("%s pinned checkout has local modifications" % name)
    return pathlib.Path(root) / "kosarev_z80"


def build_secondary(root, workdir, cxx="c++"):
    exe = pathlib.Path(workdir) / "z80_conformance_kosarev"
    r = run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(root), "-I", str(TOOLS_DIR),
             str(ORACLE_DIR / "z80_conformance_kosarev.cpp"), "-o", str(exe)])
    if r.returncode != 0:
        raise AssertionError("secondary oracle build failed: " + r.stderr)
    return exe


def skip_reason():
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return "pinned Z80 oracle checkout unavailable (%s unset)" % CHECKOUT_ENV
    return "pinned Z80 oracle checkout unavailable (%s=%s has no redcode_Z80/sources/Z80.c)" % (CHECKOUT_ENV, configured)


def build_oracle(tc, workdir):
    root = tc.oracle_checkout
    exe = pathlib.Path(workdir) / "z80_conformance_oracle"
    cmd = [tc.cc, *STRICT, "-O1", *ORACLE_DEFINES, "-I", str(root / "redcode_Z80" / "API"),
           "-I", str(root / "redcode_Zeta" / "API"), "-I", str(TOOLS_DIR),
           str(ORACLE_DIR / "z80_conformance_oracle.c"), str(root / "redcode_Z80" / "sources" / "Z80.c"), "-o", str(exe)]
    r = run(cmd)
    if r.returncode != 0:
        raise AssertionError("oracle build failed: " + r.stderr)
    return exe


def secondary_unexplained(generated, secondary, vectors):
    """Generated-vs-kosarev differences of `vectors` [(Vec, ...)] outside the committed deviation mask, as
    [(vector, step, domain, fields)]. Q, the LD A,I marker, the prefix-run state and the NMI latch are never compared."""
    out = []
    for vec in vectors:
        allowed = next((m for prefix, m in SECONDARY_FORM_MASK.items() if (vec.form or "").startswith(prefix)), set())
        for k, (g, o) in enumerate(zip(generated.get(vec.name, []), secondary.get(vec.name, []))):
            fields = [f for f in CPU_FIELDS if f not in SECONDARY_UNMODELLED and f not in allowed and g["cpu"][f] != o["cpu"][f]]
            fields += [d for d, a, b in (("memory", g["w"], o["w"]), ("io", g["io"], o["io"]), ("timing", g["t"], o["t"]))
                       if a != b]
            if fields:
                out.append((vec.name, k, fields))
    return out


def run_exe(exe, text, workdir, timeout=600):
    path = pathlib.Path(workdir) / "vectors.txt"
    path.write_text(text, encoding="utf-8")
    r = run([str(exe), str(path)], timeout=timeout)
    if r.returncode != 0:
        raise AssertionError("harness executable failed (%d): %s" % (r.returncode, r.stderr))
    return parse_results(r.stdout)


def parse_results(text):
    out = {}
    for line in text.splitlines():
        if not line.startswith("S "):
            continue
        parts = line.split(" ")
        kv = dict(p.split("=", 1) for p in parts[3:])
        step = {"t": int(kv["t"]), "out": kv["out"], "cpu": {k: kv[k] for k in CPU_FIELDS},
                "w": [] if kv["w"] == "-" else kv["w"].split(","), "io": [] if kv["io"] == "-" else kv["io"].split(",")}
        out.setdefault(parts[1], []).append(step)
    return out


# ------------------------------------------------------------------------------------------------ comparison
def compare_step(g, o):
    """First differing domain of one step: cpu, memory, io, timing (SEG-020 rule), with the differing fields."""
    cpu = [{"field": k, "generated": g["cpu"][k], "oracle": o["cpu"][k]} for k in CPU_FIELDS if g["cpu"][k] != o["cpu"][k]]
    if cpu:
        return "cpu", cpu
    if g["w"] != o["w"]:
        return "memory", [{"field": "writes", "generated": g["w"], "oracle": o["w"]}]
    if g["io"] != o["io"]:
        return "io", [{"field": "io", "generated": g["io"], "oracle": o["io"]}]
    if g["t"] != o["t"]:
        return "timing", [{"field": "t_states", "generated": g["t"], "oracle": o["t"]}]
    return None, []


def first_divergence(generated, oracle, order=None, limit=MAX_STEPS):
    """Bounded lockstep comparison. Returns a deterministic report dict for the first diverging vector/step, or
    None when every step of every vector agrees. `limit` bounds the steps compared per vector."""
    for name in order if order is not None else sorted(oracle):
        g_steps, o_steps = generated.get(name, []), oracle.get(name, [])
        if not g_steps:
            return {"schema": SCHEMA, "vector": name, "step": 0, "domain": "outcome", "fields": [
                {"field": "generated_steps", "generated": 0, "oracle": len(o_steps)}]}
        for k in range(min(len(o_steps), limit)):
            if k >= len(g_steps):
                return {"schema": SCHEMA, "vector": name, "step": k, "domain": "outcome", "fields": [
                    {"field": "generated_outcome", "generated": g_steps[-1]["out"], "oracle": "continued"}]}
            domain, fields = compare_step(g_steps[k], o_steps[k])
            if domain:
                return {"schema": SCHEMA, "vector": name, "step": k, "domain": domain, "fields": fields}
    return None


def check_expectations(vec, steps):
    """Generated-only assertions of a scenario (`expect`): outcome, T-states and any cpu field, per step."""
    problems = []
    for k, want in enumerate(vec.expect or []):
        if k >= len(steps):
            problems.append("%s step %d missing" % (vec.name, k))
            continue
        got = steps[k]
        for key, value in want.items():
            actual = got["out"] if key == "out" else got["t"] if key == "t" else got["cpu"].get(key)
            wanted = value if key in ("out", "t") else str(value).upper()
            if key in ("t",):
                ok = actual == int(wanted)
            elif key == "out":
                ok = actual == wanted
            elif key in DECIMAL:
                ok = int(actual) == int(wanted)
            else:
                ok = int(actual, 16) == int(wanted, 16)
            if not ok:
                problems.append("%s step %d: %s is %s, expected %s" % (vec.name, k, key, actual, wanted))
    return problems


def final_signature(steps):
    return {"cpu": steps[-1]["cpu"], "t": sum(s["t"] for s in steps), "w": [w for s in steps for w in s["w"]],
            "io": [i for s in steps for i in s["io"]]}


# ------------------------------------------------------------------------------------------------ manifest
def load_manifests():
    manifests = {}
    for family in FAMILIES:
        path = MANIFEST_DIR / (family + ".json")
        if path.is_file():
            manifests[family] = json.loads(path.read_text(encoding="utf-8"))
    return manifests


def manifest_credit(by_form, manifests):
    """{form id: set of credited stage names} from fresh manifest entries (digest equals the current rows)."""
    _, forms = load_dataset()
    credit = {}
    for family, manifest in manifests.items():
        for form_id, entry in manifest["forms"].items():
            vecs = by_form.get(form_id)
            if vecs is None or forms[form_id]["family"] != family or entry["digest"] != form_digest(vecs):
                continue  # stale or unattributed credit is not credit
            credit[form_id] = set(entry["stages"])
    return credit


def applicable_oracle_stages(form):
    stages = ["oracle_state", "timing_validated"]
    if "memory" in form["observables"]:
        stages.append("oracle_memory")
    if "io" in form["observables"]:
        stages.append("oracle_io")
    return stages


def write_manifest(family, passing, digests, previous):
    """Only adds credit: an already credited form that no longer passes is an error."""
    forms = dict(previous.get("forms", {}))
    for form_id, entry in forms.items():
        if entry["digest"] == digests.get(form_id) and form_id not in passing:
            raise SystemExit("refusing to remove oracle credit of %s" % form_id)
    for form_id in passing:
        forms[form_id] = {"digest": digests[form_id], "stages": passing[form_id]}
    manifest = {"schema": SCHEMA, "family": family, "oracle": PINS, "forms": dict(sorted(forms.items()))}
    (MANIFEST_DIR).mkdir(parents=True, exist_ok=True)
    (MANIFEST_DIR / (family + ".json")).write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")


# ------------------------------------------------------------------------------------------------ driver
def check_same_final(vecs, results):
    """`same_final_as: [other, [ignored cpu fields]]`: two scenarios (e.g. a split run and a whole run, or a wrapped and an
    unwrapped placement) must end in the same state, T-states and write/I/O logs."""
    problems = []
    for vec in vecs:
        if not vec.same_final_as:
            continue
        other, ignore = vec.same_final_as
        mine, theirs = final_signature(results[vec.name]), final_signature(results[other])
        for k in CPU_FIELDS:
            if k not in ignore and mine["cpu"][k] != theirs["cpu"][k]:
                problems.append("%s vs %s: %s %s != %s" % (vec.name, other, k, mine["cpu"][k], theirs["cpu"][k]))
        for k in ("t", "w", "io"):
            if mine[k] != theirs[k]:
                problems.append("%s vs %s: %s differs" % (vec.name, other, k))
    return problems


def run_batches(tc, batches, workdir, with_oracle, list_owners=False):
    """Builds and runs every batch. Returns {batch: {"generated": results, "oracle": results|None, ...}}."""
    workdir = pathlib.Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    oracle_exe = build_oracle(tc, workdir) if with_oracle else None
    report = {}
    for batch in batches:
        bdir = workdir / batch.name
        built = build_generated(tc, batch.spec_text, bdir, stem="z80_" + batch.name, list_owners=list_owners)
        if built["error"]:
            report[batch.name] = {"error": built["error"]}
            continue
        generated = run_exe(built["exe"], batch.text, bdir)
        oracle = None
        if oracle_exe is not None:
            oracle_text = "".join(t for v, t in batch.vectors if v.oracle)
            oracle = run_exe(oracle_exe, oracle_text, bdir)
        report[batch.name] = {"generated": generated, "oracle": oracle, "stats": built["stats"], "owners": built["owners"],
                              "exe": built["exe"]}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--emitter", required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--work", help="work directory (default: a fresh temporary directory)")
    parser.add_argument("--update-manifest", action="store_true")
    parser.add_argument("--json", help="write the first-divergence report here")
    parser.add_argument("--secondary", action="store_true",
                        help="also compare every form vector with the pinned kosarev/z80 secondary oracle under the deviation mask")
    args = parser.parse_args()
    root = oracle_checkout()
    if root is None:
        print("skipped: " + skip_reason())
        return 0
    tc = Toolchain(args.cc, args.emitter, root)
    by_form = form_vectors()
    all_vecs = [v for f in sorted(by_form) for v in by_form[f]]
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(args.work) if args.work else pathlib.Path(tmp)
        batches = slot_batches(all_vecs)
        report = run_batches(tc, batches, work, with_oracle=True)
        failures, passing_by_form = [], {}
        divergence = None
        for batch in batches:
            r = report[batch.name]
            if r.get("error"):
                failures.append(r["error"])
                continue
            order = [v.name for v, _ in batch.vectors]
            d = first_divergence(r["generated"], r["oracle"], order)
            if d and divergence is None:
                divergence = d
            bad = {d["vector"]} if d else set()
            for name in order:
                if name not in bad and first_divergence(r["generated"], r["oracle"], [name]):
                    bad.add(name)
            for vec, _ in batch.vectors:
                passing_by_form.setdefault(vec.form, True)
                if vec.name in bad:
                    passing_by_form[vec.form] = False
        if args.secondary:
            sec_root = secondary_checkout(os.environ.get(CHECKOUT_ENV))
            if sec_root is None:
                print("skipped: pinned kosarev/z80 checkout unavailable")
                return 0
            sec_exe = build_secondary(sec_root, work)
            unexplained = []
            for batch in batches:
                bdir = work / batch.name
                sec = run_exe(sec_exe, batch.text, bdir)
                unexplained += secondary_unexplained(report[batch.name]["generated"], sec, [v for v, _ in batch.vectors])
            print("z80 secondary (kosarev): %d unexplained differences over %d vectors" % (len(unexplained), len(all_vecs)))
            if unexplained:
                print(unexplained[:3])
                return 1
        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(divergence, indent=1, sort_keys=True) + "\n")
        if failures or divergence:
            print("z80 conformance: FAIL", failures[:1], divergence)
            return 1
        _, forms = load_dataset()
        if args.update_manifest:
            for family in FAMILIES:
                passing = {f: applicable_oracle_stages(forms[f]) for f in passing_by_form
                           if passing_by_form[f] and forms[f]["family"] == family}
                if passing:
                    digests = {f: form_digest(by_form[f]) for f in passing}
                    write_manifest(family, passing, digests, load_manifests().get(family, {}))
        print("z80 conformance: %d forms, %d vectors, no divergence" % (len(by_form), len(all_vecs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
