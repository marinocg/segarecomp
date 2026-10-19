#!/usr/bin/env python3
"""SEG-072 (ADR 0151/0152): reproducible SGDK homebrew builds and fail-closed per-byte MC68000 truth (OFFLINE research tooling).

  build   archive the pinned project revision into fresh trees, run the project's SGDK build twice (plain) plus once instrumented
          (a patched copy of the SAME makefile.gen that only adds `-save-temps -Wl,-Map` to the link and `-Wa,-almn` listings to the
          assembler rules), require all three ROMs byte-identical, then re-assemble every kept LTO partition (`*.ltransN.s`) with a
          listing and collect symbol tables (`nm`) of the application objects and of the SDK libraries.
  truth   map + listings -> per-byte classes (I instruction start / c continuation / D known data / U masked) bound to the ROM digest.

Truth rules (ADR 0151 `labels.sgdk`): sections are placed with the linker map; code rows come only from assembler listings whose every
non-relocated byte must equal the ROM at its mapped address and whose section sizes must equal the map; application functions/objects
(by symbol name, LTO suffixes stripped) are I/c/D; SGDK (libmd, sega.s/rom_head), libgcc and names that are ambiguous (defined by both the
application and the SDK, or by neither) are U; application resources (rescomp objects) are D; the uniform tail fill is D. Anything
unexplained fails closed. Toolchains: `wine` (the Windows toolchain bundled in the SGDK release tree) or `docker` (a pinned SGDK image).
Private outputs only (ignored cache); nothing commercial is involved - every input is a project-authored public source.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

M68K_MNEMONICS = frozenset("""
abcd add adda addi addq addx and andi asl asr bcc bcs beq bge bgt bhi bhs ble blo bls blt bmi bne bpl bvc bvs bra bsr bchg bclr bset btst
chk clr cmp cmpa cmpi cmpm dbcc dbcs dbeq dbf dbge dbgt dbhi dble dbls dblt dbmi dbne dbpl dbra dbt dbvc dbvs dbhs dblo divs divu eor eori
exg ext illegal jmp jsr lea link lsl lsr move movea movem movep moveq muls mulu nbcd neg negx nop not or ori pea reset rol ror roxl roxr rte
rtr rts sbcd scc scs seq sf sge sgt shi sle sls slt smi sne spl st svc svs shs slo stop sub suba subi subq subx swap tas trap trapv tst unlk
jra jbra jbsr jcc jcs jeq jge jgt jhi jhs jle jlo jls jlt jmi jne jpl jvc jvs dbeq dbne jbeq jbne movm jbcc jbcs jbge jbgt jbhi jbhs jble jblo jbls jblt jbmi jbpl jbvc jbvs
""".split())
DATA_DIRECTIVES = frozenset(""".swbeg .byte .word .short .long .int .ascii .asciz .string .skip .space .zero .fill .align .balign .p2align .even
.incbin .dc .dc.b .dc.w .dc.l .ds .ds.b .ds.w .ds.l .dcb .dcb.b .dcb.w .dcb.l .2byte .4byte .octa .quad .uaword .ualong""".split())
SUFFIX = re.compile(r"(\.(?:lto_priv|constprop|isra|part|cold|clone|localalias)(?:\.\d+)?)+$")
DEFAULT_IMAGE = "ghcr.io/stephane-d/sgdk@sha256:b0bb8a7171f8dcbf1625685b3a839d23c5c32000019a2023d0e039dd6eadcf31"


class TruthError(Exception):
    pass


def fail(code: str):
    raise TruthError(code)


def sha256(path) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def base_name(sym: str) -> str:
    return SUFFIX.sub("", sym)


# ------------------------------------------------------------------------------------------------------------------- toolchains
class Toolchain:
    def __init__(self, mode: str, tree: str | None, image: str = DEFAULT_IMAGE, jre: str | None = None, wineprefix: str | None = None):
        if mode not in ("wine", "docker"):
            raise SystemExit("mode must be wine or docker")
        self.mode, self.tree, self.image, self.jre, self.wineprefix = mode, tree, image, jre, wineprefix

    def identity(self) -> dict:
        out = {"mode": self.mode}
        if self.tree:
            rev = subprocess.run(["git", "-C", self.tree, "rev-parse", "HEAD"], capture_output=True, text=True)
            out["sgdk_tree_revision"] = rev.stdout.strip() if rev.returncode == 0 else "unknown"
        if self.mode == "docker":
            out["image"] = self.image
        return out

    def run(self, cwd, argv, log=None, timeout=3600) -> int:
        cwd = str(cwd)
        if self.mode == "wine":
            env = dict(os.environ, WINEDEBUG="-all", GDK="Z:" + self.tree, GDK_WIN="Z:" + self.tree.replace("/", "\\"),
                       WINEPATH=";".join(["Z:" + self.tree + "/bin"] + (["Z:" + self.jre + "/bin"] if self.jre else [])))
            if self.wineprefix:
                env["WINEPREFIX"] = self.wineprefix
                tmp = pathlib.Path(self.wineprefix) / "build-tmp"
                tmp.mkdir(exist_ok=True)
                env["TEMP"] = env["TMP"] = "Z:" + str(tmp).replace("/", "\\")
            tool = argv[0]
            exe = f"Z:{self.tree}/bin/{tool}.exe"
            cmd = ["wine", exe] + argv[1:]
        else:
            env = dict(os.environ)
            mounts = ["-v", f"{cwd}:/src"]
            gdk = "/sgdk"
            if self.tree:
                mounts += ["-v", f"{self.tree}:/sgdkv"]
                gdk = "/sgdkv"
            tool = argv[0]
            prefixed = {"make": "make", "java": "java"}.get(tool, "m68k-elf-" + tool)
            cmd = ["docker", "run", "--rm", "-u", "0:0", "-e", f"GDK={gdk}"] + mounts + ["-w", "/src", "--entrypoint", prefixed, self.image] + \
                  [a.replace("{GDK}", gdk) for a in argv[1:]]
        with open(log, "ab") if log else open(os.devnull, "wb") as sink:
            return subprocess.run(cmd, cwd=cwd, env=env, stdout=sink, stderr=subprocess.STDOUT, timeout=timeout).returncode

    def script(self, cwd, cmds, log=None) -> None:
        """Run argv lists in order (`>` redirects stdout to a file relative to cwd); one container for docker, sequential wine calls."""
        cwd = pathlib.Path(cwd)
        if self.mode == "docker":
            def q(x):
                return "'" + x.replace("'", "'\\''") + "'"
            lines = ["set -e"]
            for c in cmds:
                if ">" in c:
                    i = c.index(">")
                    lines.append(" ".join(q(("m68k-elf-" + c[0]) if j == 0 else x) for j, x in enumerate(c[:i])) + " > " + q(c[i + 1]))
                else:
                    lines.append(" ".join(q(("m68k-elf-" + c[0]) if j == 0 else x) for j, x in enumerate(c)))
            (cwd / ".v3" / "script.sh").write_text("\n".join(lines) + "\n")
            rc = subprocess.run(["docker", "run", "--rm", "-u", "0:0", "-v", f"{cwd}:/src", "-w", "/src", "--entrypoint", "sh", self.image, ".v3/script.sh"],
                                stdout=open(log, "ab") if log else subprocess.DEVNULL, stderr=subprocess.STDOUT).returncode
            if rc != 0:
                raise SystemExit("toolchain script failed")
            return
        for c in cmds:
            if ">" in c:
                i = c.index(">")
                target = cwd / c[i + 1]
                env = dict(os.environ, WINEDEBUG="-all")
                if self.wineprefix:
                    env["WINEPREFIX"] = self.wineprefix
                tree = self.analysis_tree if (c[0] == "objdump" and getattr(self, "analysis_tree", None)) else self.tree
                with open(target, "wb") as sink:
                    rc = subprocess.run(["wine", f"Z:{tree}/bin/{c[0]}.exe"] + c[1:i], cwd=str(cwd), env=env, stdout=sink,
                                        stderr=subprocess.DEVNULL).returncode
            else:
                rc = self.run(cwd, c, log=log)
            if rc != 0:
                raise SystemExit(f"toolchain step failed: {c[0]}")

    def gdk_ref(self) -> str:
        """How the build refers to the SDK root (`-f <gdk>/makefile.gen`)."""
        if self.mode == "wine":
            return "Z:" + self.tree
        return "{GDK}"


# ---------------------------------------------------------------------------------------------------------- makefile patching
def patch_makefile(text: str, compile_extra: str = " -g -save-temps=obj") -> tuple[str, int, int]:
    """Add `-g -save-temps -Wl,-Map=out/rom.map` to the rom.out link recipe and `-g -save-temps=obj` to every compile/assemble recipe
    that writes `$@` with `-c`. Debug information, kept temporaries and the map never change emitted code (GCC's -g invariance; checked
    for every project: the instrumented ROM must equal the plain ROM byte for byte)."""
    out, target, n_link, n_lst = [], None, 0, 0
    for ln in text.split("\n"):
        m = re.match(r"^(\S[^:=]*):", ln)
        if m and not ln.startswith("\t"):
            target = m.group(1).strip()
        if ln.startswith("\t") and target is not None:
            if re.search(r"(out|\$\(OUT\))/rom\.out$", target) and re.search(r"-o\s+(out|\$\(OUT\))/rom\.out", ln):
                ln += " -g -save-temps -Wl,-Map=out/rom.map"
                n_link += 1
            elif (target.endswith("%.o") or target.endswith("sega.o")) and "$(CC)" in ln and re.search(r"(^|\s)-c(\s|$)", ln) and "-o $@" in ln:
                ln += compile_extra
                n_lst += 1
        out.append(ln)
    return "\n".join(out), n_link, n_lst


# --------------------------------------------------------------------------------------------------------------------- build
def archive(src_repo: str, rev: str, subdir: str | None, dest: pathlib.Path) -> None:
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    tar = subprocess.run(["git", "-C", src_repo, "archive", rev] + ([subdir] if subdir else []), capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", str(dest)], input=tar, check=True)
    if subdir:
        inner = dest / subdir
        tmp = dest.parent / (dest.name + ".tmp")
        inner.rename(tmp)
        shutil.rmtree(dest)
        tmp.rename(dest)
    for stale in ("out",):
        if (dest / stale).exists():
            shutil.rmtree(dest / stale)


def cmd_build(a) -> int:
    tc = Toolchain(a.mode, a.sgdk_tree, a.image, a.jre, a.wineprefix)
    tc.analysis_tree = a.analysis_tree  # passive object reader (objdump) when the build toolchain lacks one
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    rev = subprocess.run(["git", "-C", a.project, "rev-parse", a.rev + "^{commit}"], capture_output=True, text=True, check=True).stdout.strip()
    if rev != a.rev:
        raise SystemExit("revision must be a full 40-hex commit")
    makefile_rel = a.makefile
    gdk = tc.gdk_ref()
    roms = {}
    for stale in ("plain1.log", "plain2.log", "inst.log", "relist.log", "app.nm", "lib.nm"):
        (work / stale).unlink(missing_ok=True)
    for name in ("plain1", "plain2", "inst"):
        tree = work / name
        archive(a.project, rev, a.subdir, tree)
        for extra in a.pre_copy or []:  # e.g. SDK boot files a version expects in src/boot are copied by the makefile itself
            pass
        for pre in a.pre_cmd or []:  # the project's own documented host-side generation steps (e.g. tools/gen_*.py)
            if subprocess.run(pre, shell=True, cwd=str(tree), stdout=open(work / f"{name}.pre.log", "ab"), stderr=subprocess.STDOUT).returncode != 0:
                print(json.dumps({"build": name, "status": "PRE_STEP_FAILED"}))
                return 2
        mk = f"{gdk}/{makefile_rel}" if not a.project_makefile else a.project_makefile
        if name == "inst":
            source = (tree / a.project_makefile) if a.project_makefile else (pathlib.Path(a.sgdk_tree or "") / makefile_rel if a.sgdk_tree else None)
            if source is None or not source.exists():
                if a.mode == "docker" and not a.sgdk_tree:
                    text = subprocess.run(["docker", "run", "--rm", "--entrypoint", "cat", a.image, f"/sgdk/{makefile_rel}"], capture_output=True,
                                          text=True, check=True).stdout
                else:
                    raise SystemExit("makefile not found")
            else:
                text = source.read_text(encoding="latin1")
            patched, n_link, n_lst = patch_makefile(text, " -g -save-temps" if a.save_temps_cwd else " -g -save-temps=obj")
            if a.save_temps_cwd:
                stems = {}
                for c in list(tree.rglob("*.c")) + list(tree.rglob("*.s")):
                    stems.setdefault((c.parent, c.stem), []).append(c.suffix)
                if any(len(v) > 1 for v in stems.values()):
                    raise SystemExit("save-temps in cwd would overwrite an assembly source with the same stem")
            if n_link != 1 or n_lst < 1:
                raise SystemExit(f"makefile patch failed (link {n_link}, listing {n_lst})")
            (tree / ".v3").mkdir()
            (tree / ".v3" / "makefile.gen").write_text(patched, encoding="latin1")
            mk = ".v3/makefile.gen"
        args = ["make", "-f", mk] + (a.make_args or []) + [a.target]
        rc = tc.run(tree, args, log=str(work / f"{name}.log"))
        rom = tree / "out" / "rom.bin"
        if rc != 0 and a.allow_failed_rename and rom.exists():
            tail = [ln for ln in (work / f"{name}.log").read_text(errors="replace").splitlines() if ln.strip()]
            renames = [ln for ln in tail if ln.startswith("mv out/rom.bin ")]
            if renames and "command not found: mv" in "\n".join(tail[-4:]):
                rc = 0  # documented deviation: the toolchain lacks the cosmetic final `mv`; the ROM bytes were complete before it
        if rc != 0 or not rom.exists():
            print(json.dumps({"build": name, "status": "BUILD_FAILED", "rc": rc}))
            return 2
        roms[name] = sha256(rom)
    if len(set(roms.values())) != 1:
        print(json.dumps({"status": "NOT_REPRODUCIBLE", "roms": roms}))
        return 3
    inst = work / "inst"
    units = relist_units(inst, (work / "inst.log").read_text(encoding="latin1", errors="replace"))
    run_relist(tc, inst, units, work)
    ltrans = [u for u in units if u["kind"] == "ltrans"]
    app_objs = [str(p.relative_to(inst)) for p in (inst / "out").rglob("*.o") if p.name not in ("sega.o", "rom_head.o") and "/res/" not in str(p)
                and not str(p.relative_to(inst)).startswith("out/res")]
    nm_plugin = ["--plugin=liblto_plugin-0.dll"] if a.mode == "wine" else []
    nm_tool = "nm" if a.mode == "wine" else "gcc-nm"
    open(work / "app.nm", "w").close()
    for o in app_objs:
        tc.run(inst, [nm_tool] + nm_plugin + [o], log=str(work / "app.nm"))
    libs = a.lib or []
    open(work / "lib.nm", "w").close()
    for lib in libs:
        tc.run(inst, [nm_tool] + nm_plugin + [lib], log=str(work / "lib.nm"))
    meta = {"status": "BUILT", "project_revision": rev, "subdir": a.subdir, "rom_sha256": roms["inst"], "rom_bytes": (inst / "out" / "rom.bin").stat().st_size,
            "toolchain": tc.identity(), "makefile": makefile_rel, "target": a.target, "make_args": a.make_args or [], "ltrans_partitions": len(ltrans),
            "reproducible_builds": 3}
    (work / "build.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
    print(json.dumps(meta, sort_keys=True))
    return 0


# ------------------------------------------------------------------------------------------------- marker relisting (exact rows)
# Gas listings and gas DWARF line tables are both skewed by m68k branch relaxation (bytes/addresses land on later source lines), so
# neither is used. Instead a unique local marker label is inserted before every byte-emitting statement of the assembly source; a label
# emits nothing and cannot change relaxation, and the instrumented object must have section contents byte-identical to the original.
# Each marker's symbol value is then the exact post-relaxation offset of its statement.
MARK = "__v3m_"
DEFINES = (".macro", ".rept", ".irp", ".irpc")
ENDS = (".endm", ".endr")
NONEMIT = frozenset(""".globl .global .local .hidden .type .size .file .ident .section .text .data .bss .previous .pushsection .popsection
.set .equ .equiv .weak .comm .lcomm .cfi_startproc .cfi_endproc .loc .file .p2alignl .end .include .if .ifdef .ifndef .else .endif .elseif
.extern .list .nolist .psize .title .sbttl .eject .org .def .endef .scl .tag .val .dim .line .ln .protected .internal .symver .vtable_entry
.vtable_inherit .stabs .stabn .stabd .uleb128 .sleb128 .attribute .gnu_attribute .option .arch .cpu .mri .lflags .noformat .format
.altmacro .noaltmacro .purgem .exitm .err .warning .print .fail .abort .linkonce .weakref .reloc .cfi_def_cfa .cfi_offset
.cfi_def_cfa_offset .cfi_adjust_cfa_offset .cfi_rel_offset .cfi_restore .cfi_sections""".split())


def split_statement(line: str):
    """(labels, first_token, rest) of one gas statement; comments (`|` anywhere, `#` at line start) removed; strings blanked."""
    text = line.rstrip("\r")
    if text.lstrip().startswith("#") or text.lstrip().startswith("*"):
        return [], "", ""
    text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
    text = text.split("|", 1)[0]
    labels = []
    while True:
        mo = re.match(r"^\s*([A-Za-z_.$][\w.$]*|\d+):(?!:)(.*)$", text)
        if not mo:
            break
        labels.append(mo.group(1))
        text = mo.group(2)
    toks = text.split()
    return labels, (toks[0] if toks else ""), " ".join(toks[1:])


def classify_statement(first: str, macros: set) -> str:
    f = first.lower()
    if not f:
        return "N"
    if f in macros:
        return "M"
    if f.startswith("."):
        if f in DATA_DIRECTIVES or f.startswith((".dc", ".ds", ".dcb", ".align", ".balign", ".p2align", ".incbin")):
            return "D"
        if f in NONEMIT or f.startswith(".cfi_"):
            return "N"
        return "X"
    base = f.split(".")[0]
    if base in M68K_MNEMONICS:
        return "I"
    return "X"


def strip_c_comments(text: str) -> str:
    """Remove /* ... */ comments (outside double-quoted strings), keeping every newline so line numbers are unchanged."""
    out, i, n, in_str, in_com = [], 0, len(text), False, False
    while i < n:
        ch = text[i]
        if in_com:
            if text.startswith("*/", i):
                in_com = False
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"' or ch == "\n":
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
        elif text.startswith("/*", i):
            in_com = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def instrument_source(text: str):
    """Returns (instrumented_text, rows) where rows[k] = dict(kind, line, sym) for marker k. Kinds: I, D, M (macro call), R (repeat
    block). Lines inside macro/repeat definitions get no marker. `sym` is the latest label declared with `.type` (function/object).
    C comments are removed first (the instrumented text is assembled from the stripped source; byte identity is checked)."""
    text = strip_c_comments(text)
    lines = text.split("\n")
    macros: set[str] = set()
    for ln in lines:
        _, first, rest = split_statement(ln)
        if first.lower() == ".macro" and rest:
            macros.add(rest.split()[0].split(",")[0].lower())
    out, rows = [], []
    row_index_sym: list[int] = []
    depth = 0
    types: dict[str, str] = {}
    for ln in lines:
        _, first, rest = split_statement(ln)
        if first.lower() == ".type" and rest:
            parts = rest.replace(" ", "").split(",")
            if len(parts) >= 2:
                types[parts[0]] = "function" if "function" in parts[1] else "object"
    files: dict[str, str] = {}
    owner_file: dict[str, str] = {}
    for ln in lines:
        _, first, rest = split_statement_keep_strings(ln)
        mo = re.match(r'^(\d+)\s+"(.*)"\s*$', rest) if first.lower() == ".file" else None
        if mo:
            files[mo.group(1)] = mo.group(2)
    cur = None
    for i, ln in enumerate(lines):
        labels, first, rest = split_statement(ln)
        f = first.lower()
        for lab in labels:
            if lab in types:
                cur = lab
        if f == ".loc" and cur is not None and cur not in owner_file and rest.split():
            owner_file[cur] = files.get(rest.split()[0], "")
        if depth:
            if f in ENDS:
                depth -= 1
            elif f in DEFINES:
                depth += 1
            out.append(ln)
            continue
        if f == ".macro":
            depth = 1
            out.append(ln)
            continue
        kind = classify_statement(first, macros)
        if f in (".rept", ".irp", ".irpc"):
            kind = "R"
        if kind == "X":
            raise TruthError("unknown_statement")
        if kind in ("I", "D", "M", "R"):
            out.append(f"{MARK}{len(rows)}:")
            rows.append({"kind": kind, "line": i + 1, "sym": cur, "mnemonic": f})
            row_index_sym.append(len(rows) - 1)
        if kind == "R":
            depth = 1
        out.append(ln)
    for r in rows:
        r["file"] = owner_file.get(r["sym"]) if r["sym"] and types.get(r["sym"]) == "function" else None
        r["symtype"] = types.get(r["sym"]) if r["sym"] else None
    return "\n".join(out), rows


def split_statement_keep_strings(line: str):
    text = line.rstrip("\r")
    toks = text.split(None, 1)
    if not toks:
        return [], "", ""
    return [], toks[0], toks[1] if len(toks) > 1 else ""


def objdump_contents(text: str) -> dict:
    """objdump -s output -> {section: bytes}."""
    secs: dict[str, bytearray] = {}
    cur = None
    for ln in text.split("\n"):
        mo = re.match(r"^Contents of section (\S+):", ln)
        if mo:
            cur = mo.group(1)
            secs[cur] = bytearray()
            continue
        mo = re.match(r"^ ([0-9a-f]{4,8}) ((?:[0-9a-f]{2,8} ){1,4})", ln)
        if mo and cur is not None:
            off = int(mo.group(1), 16)
            data = bytes.fromhex(mo.group(2).replace(" ", ""))
            if off != len(secs[cur]):
                raise TruthError("objdump_contents_gap")
            secs[cur] += data
    return {k: bytes(v) for k, v in secs.items()}


def objdump_symbols(text: str) -> dict:
    """objdump -t -> {marker index: (section, value)} for marker symbols only."""
    out = {}
    for ln in text.split("\n"):
        mo = re.match(r"^([0-9a-f]{8})\s.{7}\s(\S+)\s+[0-9a-f]{8}\s+" + MARK + r"(\d+)$", ln)
        if mo:
            out[int(mo.group(3))] = (mo.group(2), int(mo.group(1), 16))
    return out


def relist_units(inst: pathlib.Path, inst_log: str) -> list[dict]:
    """Assembly units whose rows are needed: every kept LTO partition and every application object compiled to real code."""
    units = []
    for s in sorted(inst.rglob("*.s")):
        mo = re.search(r"^(.*\.ltrans\d+)(?:\.ltrans)?\.s$", s.name)
        if mo:
            units.append({"kind": "ltrans", "src": str(s.relative_to(inst)), "map_file_suffix": mo.group(1) + ".ltrans.o", "as_flags": ["-m68000"]})
    for line in inst_log.split("\n"):
        mo = re.search(r"\s-o\s+(out/\S+\.o)(?:\s|$)", line)
        if not mo or " -c " not in line + " ":
            continue
        obj = mo.group(1)
        if obj.endswith(("sega.o", "rom_head.o")) or obj.startswith("out/res/"):
            continue
        src_m = re.search(r"\s-c\s+(\S+)", line)
        src = src_m.group(1) if src_m else ""
        cpp = "assembler-with-cpp" in line or src.endswith(".S")
        candidates = []
        if src.endswith(".c") or cpp:
            # saved temporary: next to the object (-save-temps=obj) or, for compilers that ignore `=obj`, in the working directory
            candidates = [obj[:-2] + ".s", pathlib.Path(src).stem + ".s"]
        elif src.endswith(".s"):
            candidates = [src]  # a plain assembly source is assembled as written
        saved = next((c for c in candidates if (inst / c).exists()), None)
        if saved is None:
            continue
        wa = []
        for tok in line.split():
            if tok.startswith("-Wa,"):
                for opt in tok[4:].split(","):
                    if opt and opt != "--gdwarf2" and not opt.startswith("-a"):
                        wa.append(opt)
        if any(u["map_file_suffix"] == obj for u in units):
            continue
        units.append({"kind": "object", "src": saved, "map_file_suffix": obj, "as_flags": ["-m68000"] + wa})
    return units


def run_relist(tc: Toolchain, inst: pathlib.Path, units: list[dict], work: pathlib.Path) -> None:
    d = inst / ".v3" / "relist"
    d.mkdir(parents=True, exist_ok=True)
    cmds = []
    for k, u in enumerate(units):
        text = (inst / u["src"]).read_text(encoding="latin1")
        u["id"] = k
        if ".gnu.lto_" in text:
            u["skipped"] = "lto_ir"  # an LTO bytecode container: its code is emitted by the LTO partitions
            continue
        try:
            instr, rows = instrument_source(text)
        except TruthError as error:
            u["skipped"] = str(error)  # fails closed later only if the linker map places this unit
            continue
        (d / f"u{k}.s").write_text(text, encoding="latin1")
        (d / f"u{k}.m.s").write_text(instr, encoding="latin1")
        (d / f"u{k}.rows.json").write_text(json.dumps(rows))
        u["rows"] = f".v3/relist/u{k}.rows.json"
        rel = f".v3/relist/u{k}"
        cmds += [["as"] + u["as_flags"] + ["-o", f"{rel}.o", f"{rel}.s"], ["as"] + u["as_flags"] + ["-o", f"{rel}.m.o", f"{rel}.m.s"],
                 ["objdump", "-s", "-h", "-r", f"{rel}.o", ">", f"{rel}.o.dump"], ["objdump", "-s", f"{rel}.m.o", ">", f"{rel}.m.o.dump"],
                 ["objdump", "-t", f"{rel}.m.o", ">", f"{rel}.m.o.sym"]]
    tc.script(inst, cmds, log=str(work / "relist.log"))
    (work / "units.json").write_text(json.dumps(units, indent=1))


# ----------------------------------------------------------------------------------------------------------------- map parsing
def parse_map(text: str):
    """Input-section placements: list of dict(section, addr, size, file, lma). Also fill entries (file = '*fill*')."""
    lines = text.split("\n")
    try:
        start = next(i for i, ln in enumerate(lines) if ln.startswith("Linker script and memory map"))
    except StopIteration:
        fail("map_without_memory_map")
    out = []
    out_sec, out_vma, out_lma = None, 0, None
    pending = None
    for ln in lines[start + 1:]:
        mo = re.match(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?\s*$", ln)
        if mo:
            out_sec, out_vma = mo.group(1), int(mo.group(2), 16)
            out_lma = int(mo.group(4), 16) if mo.group(4) else out_vma
            continue
        mo = re.match(r"^(\.\S+)\s*$", ln)
        if mo and not ln.startswith(" "):
            out_sec = mo.group(1)
            pending = ("out", out_sec)
            continue
        if pending and pending[0] == "out":
            mo = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?\s*$", ln)
            if mo:
                out_vma = int(mo.group(1), 16)
                out_lma = int(mo.group(3), 16) if mo.group(3) else out_vma
            pending = None
            continue
        mo = re.match(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$", ln)
        if mo and not mo.group(1).startswith("*"):
            if out_sec is not None:
                vma = int(mo.group(2), 16)
                out.append({"section": mo.group(1), "addr": vma, "size": int(mo.group(3), 16), "file": mo.group(4).strip(),
                            "lma": out_lma + (vma - out_vma), "out": out_sec})
            continue
        mo = re.match(r"^ (\S+)\s*$", ln)
        if mo and not mo.group(1).startswith("*") and not mo.group(1).startswith("0x"):
            pending = ("in", mo.group(1))
            continue
        if pending and pending[0] == "in":
            mo = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$", ln)
            if mo and out_sec is not None:
                vma = int(mo.group(1), 16)
                out.append({"section": pending[1], "addr": vma, "size": int(mo.group(2), 16), "file": mo.group(3).strip(),
                            "lma": out_lma + (vma - out_vma), "out": out_sec})
            pending = None
            continue
        mo = re.match(r"^ \*fill\*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", ln)
        if mo and out_sec is not None:
            vma = int(mo.group(1), 16)
            out.append({"section": "*fill*", "addr": vma, "size": int(mo.group(2), 16), "file": "*fill*", "lma": out_lma + (vma - out_vma),
                        "out": out_sec})
            continue
        mo = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(LONG|SHORT|BYTE|QUAD)\b", ln)
        if mo and out_sec is not None:
            vma = int(mo.group(1), 16)
            out.append({"section": "*data-stmt*", "addr": vma, "size": int(mo.group(2), 16), "file": "*linker*", "lma": out_lma + (vma - out_vma),
                        "out": out_sec})
    return [e for e in out if e["size"] > 0]


# ------------------------------------------------------------------------------------------------------------- listing parsing
ROW = re.compile(r"^\s*(\d+) ([0-9a-f]{4,8}) ?((?:[0-9A-F]{2,8} ?)*)\s*\t?(.*)$")
CONT = re.compile(r"^\s*(\d+)\s{4,}((?:[0-9A-F]{2,8} ?)+)\s*$")
NOADDR = re.compile(r"^\s*(\d+)\s+\t(.*)$|^\s*(\d+)\s*$")


def _src_tokens(src: str):
    text = src.split("|", 1)[0].split("#", 1)[0] if not src.lstrip().startswith(".") else src
    text = re.sub(r"(?<!\\)\"(?:[^\"\\]|\\.)*\"", "\"\"", text)
    text = text.split(";", 1)[0]
    labels = []
    while True:
        mo = re.match(r"^\s*([A-Za-z_.$][\w.$]*):(.*)$", text)
        if not mo:
            break
        labels.append(mo.group(1))
        text = mo.group(2)
    return labels, text.split()


def parse_listing(text: str):
    """gas listing -> per section: list of rows dict(off, bytes, kind I/D/N, sym_events, text). Section switches tracked from directives.
    Returns {section: rows}, symbol types {name: function|object}."""
    sections: dict[str, list] = {}
    cur = ".text"
    sym_types: dict[str, str] = {}
    last = None
    stack = []
    for ln in text.split("\n"):
        ln = ln.rstrip("\r")
        mo = CONT.match(ln)
        if mo and last is not None and ROW.match(ln) is None:
            last["bytes"] += bytes.fromhex(mo.group(2).replace(" ", ""))
            continue
        mo = ROW.match(ln)
        if mo:
            off = int(mo.group(2), 16)
            data = bytes.fromhex(mo.group(3).replace(" ", "")) if mo.group(3).strip() else b""
            src = mo.group(4)
        else:
            mo2 = NOADDR.match(ln)
            if not mo2:
                continue
            off, data, src = None, b"", mo2.group(2) or ""
        labels, toks = _src_tokens(src)
        first = toks[0].lower() if toks else ""
        if first in (".text", ".data", ".bss"):
            cur = first
        elif first == ".section":
            cur = toks[1].split(",")[0].strip("\"") if len(toks) > 1 else cur
        elif first == ".pushsection":
            stack.append(cur)
            cur = toks[1].split(",")[0].strip("\"")
        elif first == ".popsection":
            cur = stack.pop() if stack else cur
        elif first == ".previous":
            pass
        elif first == ".type" and len(toks) >= 2:
            parts = " ".join(toks[1:]).replace(" ", "").split(",")
            if len(parts) >= 2:
                sym_types[parts[0]] = "function" if "function" in parts[1] else "object"
        rows = sections.setdefault(cur, [])
        kind = "N"
        if data or (off is not None and first and not first.startswith(".") and toks):
            if first.startswith("."):
                kind = "D" if (first in DATA_DIRECTIVES or first.split(".")[1:2] and "." + first.split(".")[1] in DATA_DIRECTIVES) else "X"
            elif first:
                kind = "I"
            else:
                kind = "D"
        if kind == "X":
            fail("unknown_directive_with_bytes")
        row = {"off": off, "bytes": bytearray(data), "kind": kind, "labels": labels, "mnemonic": first}
        rows.append(row)
        last = row if data else None
    return sections, sym_types


def parse_relocs(text: str):
    """objdump -r -h output -> ({section: set(byte offsets covered by relocations)}, {section: size})."""
    rel: dict[str, set[int]] = {}
    sizes: dict[str, int] = {}
    cur = None
    for ln in text.split("\n"):
        mo = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s", ln)
        if mo:
            sizes[mo.group(1)] = int(mo.group(2), 16)
            continue
        mo = re.match(r"^RELOCATION RECORDS FOR \[(.+)\]:", ln)
        if mo:
            cur = mo.group(1)
            rel.setdefault(cur, set())
            continue
        mo = re.match(r"^([0-9a-f]{8})\s+(R_68K_(\w+))\s", ln)
        if mo and cur is not None:
            off = int(mo.group(1), 16)
            kind = mo.group(3)
            width = 4 if "32" in kind else 2 if "16" in kind else 1 if "8" in kind else 4
            rel[cur].update(range(off, off + width))
    return rel, sizes


# ---------------------------------------------------------------------------------------------------------------------- truth
def symbol_set(nm_text: str) -> set[str]:
    out = set()
    for ln in nm_text.split("\n"):
        mo = re.match(r"^(?:[0-9a-f]+|\s+)\s+([A-Za-z])\s+(\S+)$", ln)
        if mo and mo.group(1) not in "Uuw":
            out.add(base_name(mo.group(2)))
    return out


def file_owner(path) -> str:
    """Owner of a function from the source file of its first `.loc` (DWARF, -g): a project-relative path is the application; an
    absolute path (an SDK source path baked into libmd's LTO bytecode, a toolchain header) is the SDK; no location is ambiguous."""
    if not path:
        return "ambiguous"
    p = path.replace("\\", "/")
    if p.startswith("<"):
        return "ambiguous"
    if re.match(r"^[A-Za-z]:/", p) or p.startswith("/"):
        if p.startswith("/src/") and not p.startswith("/src/sgdk"):
            return "app"  # docker: the project tree is mounted at /src
        return "sdk"
    return "app"


def origin_of_file(path: str) -> str:
    p = path.replace("\\", "/")
    if "libgcc" in p or "libmd" in p or "/lib/" in p and p.endswith(")"):
        return "sdk"
    if p.endswith("out/sega.o") or p == "out/sega.o" or p.endswith("/rom_head.o"):
        return "sdk"
    if re.search(r"\.ltrans\d+\.ltrans\.o$", p) or re.search(r"\.ltrans\d+\.o$", p):
        return "ltrans"
    if p.startswith("out/res/") or "/out/res/" in p:
        return "resource"
    if p.startswith("out/") or "/out/" in p:
        return "app_object"
    if p in ("*fill*", "*linker*"):
        return p
    return "unknown"


def build_truth(work: pathlib.Path) -> tuple[bytes, dict]:
    inst = work / "inst"
    rom = (inst / "out" / "rom.bin").read_bytes()
    n = len(rom)
    cls = bytearray(b"\0" * n)  # 0 = not yet explained
    entries = parse_map((inst / "out" / "rom.map").read_text(encoding="latin1"))
    app = symbol_set((work / "app.nm").read_text(errors="replace")) if (work / "app.nm").exists() else set()
    lib = symbol_set((work / "lib.nm").read_text(errors="replace")) if (work / "lib.nm").exists() else set()
    units = json.loads((work / "units.json").read_text())
    stats = {"verified_bytes": 0, "reloc_bytes": 0, "owners": {"app": set(), "sdk": set(), "ambiguous": set()}, "unit_sections_placed": 0,
             "merged_string_sections": 0, "macro_or_repeat_rows": 0}
    loaded: dict[int, dict] = {}

    def unit_for(file: str):
        f = file.replace("\\", "/")
        hits = [u for u in units if f == u["map_file_suffix"] or f.endswith("/" + u["map_file_suffix"]) or f.endswith(u["map_file_suffix"])
                and u["kind"] == "ltrans"]
        if len(hits) != 1:
            fail("unit_for_map_file_ambiguous_or_missing")
        u = hits[0]
        if u.get("skipped"):
            fail("placed_unit_not_relisted:" + u["skipped"])
        if u["id"] not in loaded:
            rel = inst / ".v3" / "relist" / f"u{u['id']}"
            orig = pathlib.Path(str(rel) + ".o.dump").read_text(errors="replace")
            marked = pathlib.Path(str(rel) + ".m.o.dump").read_text(errors="replace")
            c0, c1 = objdump_contents(orig), objdump_contents(marked)
            meta = (".debug", ".comment", ".note", ".stab", ".gnu")  # never loaded; assembler line metadata may differ
            if {k: v for k, v in c0.items() if not k.startswith(meta)} != {k: v for k, v in c1.items() if not k.startswith(meta)}:
                fail("marker_instrumentation_changed_section_bytes")
            rels, sizes = parse_relocs(orig)
            marks = objdump_symbols(pathlib.Path(str(rel) + ".m.o.sym").read_text(errors="replace"))
            rows = json.loads((inst / u["rows"]).read_text())
            if sorted(marks) != list(range(len(rows))):
                fail("marker_symbols_missing")
            by_sec: dict[str, list] = {}
            for k in range(len(rows)):
                sec, val = marks[k]
                by_sec.setdefault(sec, []).append((k, val))
            loaded[u["id"]] = {"contents": c0, "rels": rels, "sizes": sizes, "rows": rows, "by_sec": by_sec, "kind": u["kind"]}
        return loaded[u["id"]]

    def put(lo, hi, c):
        if lo < 0 or hi > n:
            fail("section_outside_rom")
        for i in range(lo, hi):
            if cls[i] and cls[i] != c:
                fail("overlapping_placement")
            cls[i] = c

    for e in entries:
        if e["out"] in (".bss", ".stack") or e["section"].startswith((".bss", ".sbss")) or e["section"] == "COMMON":
            continue
        if e["out"].startswith((".debug", ".comment", ".stab", ".note", ".gnu", ".ARM")):
            continue  # not loaded into the image
        lo = e["lma"]
        if lo >= n:
            continue
        hi = lo + e["size"]
        origin = origin_of_file(e["file"])
        if origin == "sdk":
            put(lo, hi, ord("U"))
            continue
        if origin in ("*fill*", "*linker*", "resource"):
            put(lo, hi, ord("D"))
            continue
        if origin not in ("ltrans", "app_object"):
            fail("unknown_input_file_class")
        sec = e["section"]
        if sec.startswith(".rodata.str") or sec.startswith(".rodata.cst"):
            put(lo, hi, ord("U"))  # mergeable constants: the linker may deduplicate across objects; never labelled
            stats["merged_string_sections"] += 1
            continue
        U = unit_for(e["file"])
        if U["sizes"].get(sec) != e["size"] or sec not in U["contents"]:
            fail("section_size_or_name_differs_from_map")
        data, relset = U["contents"][sec], U["rels"].get(sec, set())
        for k in range(e["size"]):
            if k in relset:
                stats["reloc_bytes"] += 1
                continue
            if rom[lo + k] != data[k]:
                fail("object_byte_differs_from_rom")
            stats["verified_bytes"] += 1
        marks = U["by_sec"].get(sec, [])
        if not marks:
            fail("placed_section_without_rows")
        covered = 0
        stats["unit_sections_placed"] += 1
        for j, (k, val) in enumerate(marks):
            nxt = marks[j + 1][1] if j + 1 < len(marks) else e["size"]
            if val != covered or nxt < val:
                fail("rows_do_not_tile_section")
            row = U["rows"][k]
            extent = nxt - val
            covered = nxt
            if extent == 0:
                continue
            if U["kind"] == "object":
                who = "app"
            elif row["sym"] is None:
                who = "ambiguous"
            elif row.get("symtype") == "function":
                b = base_name(row["sym"])
                who = file_owner(row.get("file"))
                stats["owners"][who].add(b)
            else:
                b = base_name(row["sym"])
                who = "app" if (b in app and b not in lib) else "sdk" if (b in lib and b not in app) else "ambiguous"
            a0 = lo + val
            if row["kind"] in ("M", "R"):
                stats["macro_or_repeat_rows"] += 1
                put(a0, a0 + extent, ord("U"))
            elif who != "app":
                put(a0, a0 + extent, ord("U"))
            elif row["kind"] == "I":
                if extent < 2 or extent > 10 or extent % 2:
                    fail("instruction_extent_invalid")
                put(a0, a0 + 1, ord("I"))
                put(a0 + 1, a0 + extent, ord("c"))
            else:
                put(a0, a0 + extent, ord("D"))
        if covered != e["size"]:
            fail("rows_do_not_cover_section")
    last = max((i for i in range(n) if cls[i]), default=-1)
    tail = rom[last + 1:]
    if tail and len(set(tail)) != 1:
        fail("tail_not_uniform_fill")
    for i in range(last + 1, n):
        cls[i] = ord("D")
    holes = [i for i in range(n) if not cls[i]]
    if holes:
        runs, s0, p0 = [], holes[0], holes[0]
        for x in holes[1:]:
            if x != p0 + 1:
                runs.append((s0, p0 + 1))
                s0 = x
            p0 = x
        runs.append((s0, p0 + 1))
        for lo, hi in runs:
            if hi - lo > 16 or len(set(rom[lo:hi])) != 1:
                fail("unexplained_rom_range")
            for i in range(lo, hi):
                cls[i] = ord("D")
    out = bytes(cls)
    report = {"rom_sha256": hashlib.sha256(rom).hexdigest(), "rom_bytes": n, "starts": out.count(b"I"),
              "classes": {c: out.count(c.encode()) for c in "IcDU"}, "verified_bytes": stats["verified_bytes"], "relocated_bytes": stats["reloc_bytes"],
              "owners": {k: len(v) for k, v in stats["owners"].items()}, "unit_sections_placed": stats["unit_sections_placed"],
              "merged_string_sections_masked": stats["merged_string_sections"], "macro_or_repeat_rows_masked": stats["macro_or_repeat_rows"]}
    if report["starts"] == 0:
        fail("no_application_instruction")
    return out, report


def cmd_truth(a) -> int:
    work = pathlib.Path(a.work)
    try:
        cls, report = build_truth(work)
    except TruthError as error:
        print(json.dumps({"status": "TRUTH_FAILED", "error": str(error)}))
        return 1
    pathlib.Path(a.output).write_bytes(cls)
    report["classes_sha256"] = hashlib.sha256(cls).hexdigest()
    print(json.dumps(report, sort_keys=True))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--project", required=True)
    b.add_argument("--rev", required=True)
    b.add_argument("--subdir")
    b.add_argument("--work", required=True)
    b.add_argument("--mode", choices=["wine", "docker"], required=True)
    b.add_argument("--sgdk-tree")
    b.add_argument("--image", default=DEFAULT_IMAGE)
    b.add_argument("--jre")
    b.add_argument("--wineprefix")
    b.add_argument("--makefile", default="makefile.gen")
    b.add_argument("--target", default="release")
    b.add_argument("--make-args", nargs="*")
    b.add_argument("--lib", nargs="*", help="SDK libraries (paths as the toolchain sees them) whose symbols are SDK-owned")
    b.add_argument("--pre-copy", nargs="*")
    b.add_argument("--pre-cmd", action="append", help="host command run in each fresh tree before make (project codegen)")
    b.add_argument("--project-makefile", help="makefile path inside the project tree (instead of <SDK>/makefile.gen)")
    b.add_argument("--analysis-tree", help="SGDK tree whose objdump reads the objects (build toolchain without objdump)")
    b.add_argument("--save-temps-cwd", action="store_true", help="compilers older than GCC 4.5 (no -save-temps=obj)")
    b.add_argument("--allow-failed-rename", action="store_true", help="accept only a failed final `mv out/rom.bin ...` (toolchain lacks mv)")
    b.set_defaults(func=cmd_build)
    t = sub.add_parser("truth")
    t.add_argument("--work", required=True)
    t.add_argument("--output", required=True)
    t.set_defaults(func=cmd_truth)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
