#!/usr/bin/env python3
"""SEG-009-T013 durable dependency gate for the Master System product (hermetic, no build, no ROM).

usage: sms_dependency_gate_test.py <product-root>

Two scans over the whole product tree (comments stripped, so prose such as a citation of a public SMS page is fine):

  1. reference scan: no file under libs/** or platforms/genesis/** includes an SMS product header or names an SMS product
     identifier/target/path (SMS runtime, machine, viewer, headless vocabulary). The ROM-header classifier vocabulary of
     libs/media (`Platform::master_system`, the `sms_gg_*` diagnostics) is classification data, not a dependency, and is
     deliberately not matched; includes and link references from media are.
  2. link closure: the transitive `target_link_libraries` closure of every SMS target is an explicit allowlist (the PSG
     library links nothing; the runtime reaches only the PSG; the headless/viewer targets never reach the recompiler, M68k,
     Genesis or codegen), and no target defined under libs/** or platforms/genesis/** reaches an SMS product target.

Negative controls run the same checker on in-memory mutations of the tree (an overlay of changed/added files): media
linking the SMS runtime, the Genesis runtime including an SMS header, core linking the SMS runtime, M68k linking the SMS
machine, the SMS viewer linking the recompiler, the PSG linking a library, an SMS identifier in a generic library. The
unmutated tree and a prose-only mention must pass.

It also holds the witness for the capability rows `timing.pal` and `vdp.revision_5124`: no header field, ingestion option,
build flag or driver flag can request PAL timing or the SMS 1 VDP (the profile is fixed to NTSC SMS 2).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
GENERIC_ROOTS = ("libs", "platforms/genesis")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inl"}
CMAKE_NAMES = {"CMakeLists.txt"}

SMS_INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]*)[>"]')
SMS_INCLUDE_BAD = re.compile(r"(^|/)sms_[A-Za-z0-9_]*\.h$|master[_-]system")
SMS_IDENT = re.compile(
    r"\bSms[A-Z]\w*|\bSMS_(?:ERROR|MAPPER|IRQ|STOP|PORT|PAD|FB|CYCLES|LINES|VDP|INPUT)\w*|"
    r"\bsms_(?:memory|machine|vdp|psg|pad|input|render|ports|error|mapper|sha256|audio|viewer|headless|image)\w*")
SMS_CMAKE = re.compile(r"master[_-]system|\bsms_[a-z]", re.I)

SMS_PRODUCT_TARGETS = {
    "segarecomp_runtime_master_system", "segarecomp_machine_master_system", "segarecomp_headless_master_system",
    "segarecomp_viewer_master_system", "segarecomp_viewer_master_system_main_check", "segarecomp_viewer_master_system_sdl3",
}
PSG = "segarecomp_device_psg"
# Exact allowed transitive closure (excluding the target itself) of every SMS target.
ALLOWED_CLOSURE = {
    PSG: set(),
    "segarecomp_runtime_master_system": {PSG},
    "segarecomp_machine_master_system": {"segarecomp_media", "segarecomp_base", "segarecomp_codegen_c11", "segarecomp_codegen_c11_z80",
                                         "segarecomp_cpu_z80", "segarecomp_runtime_master_system", PSG,
                                         # SEG-028 (ADR 0077): the CPU-neutral executable-image artifact and its Z80 projection.
                                         "segarecomp_codegen_c11_z80_image", "segarecomp_recompiler"},
    "segarecomp_headless_master_system": {"segarecomp_runtime_master_system", PSG},
    "segarecomp_viewer_master_system": {"segarecomp_runtime_master_system", PSG},
    "segarecomp_viewer_master_system_main_check": {"segarecomp_viewer_master_system", "segarecomp_runtime_master_system", PSG},
    "segarecomp_viewer_master_system_sdl3": {"segarecomp_viewer_master_system", "segarecomp_runtime_master_system", PSG, "SDL3::SDL3"},
}


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def strip_cmake_comments(text):
    return re.sub(r"#[^\n]*", "", text)


class Tree:
    """The product tree as files on disk plus an optional in-memory overlay (rel path -> text; used by the controls)."""

    def __init__(self, root, overlay=None):
        self.root, self.overlay = root, dict(overlay or {})

    def files(self, prefixes):
        seen = set()
        for prefix in prefixes:
            base = self.root / prefix
            for path in sorted(base.rglob("*")) if base.is_dir() else []:
                if path.is_file() and (path.suffix in SOURCE_SUFFIXES or path.name in CMAKE_NAMES):
                    rel = path.relative_to(self.root).as_posix()
                    seen.add(rel)
                    yield rel, self.overlay.get(rel, path.read_text(encoding="utf-8", errors="ignore"))
        for rel, text in sorted(self.overlay.items()):
            if rel not in seen and any(rel.startswith(p + "/") for p in prefixes):
                yield rel, text


def parse_targets(tree):
    """Returns (aliases, links): alias -> real target, real target -> set of direct link names (aliases resolved later)."""
    aliases, links, known = {}, {}, set()
    for rel, text in tree.files(("libs", "platforms")):
        if not rel.endswith("CMakeLists.txt"):
            continue
        code = strip_cmake_comments(text)
        for m in re.finditer(r"add_library\(\s*(\S+)\s+ALIAS\s+(\S+)\s*\)", code):
            aliases[m.group(1)] = m.group(2)
        for m in re.finditer(r"add_library\(\s*(\S+)(?!\s+ALIAS)", code):
            if m.group(1) not in aliases:
                known.add(m.group(1))
        for m in re.finditer(r"target_link_libraries\(\s*(\S+)\s+([^)]*)\)", code):
            items = [t for t in m.group(2).split() if t not in ("PUBLIC", "PRIVATE", "INTERFACE") and not t.startswith("$")]
            links.setdefault(m.group(1), set()).update(items)
    return aliases, links, known


def closure(target, aliases, links):
    def real(name):
        return aliases.get(name, name)

    out, stack = set(), [real(target)]
    while stack:
        for dep in links.get(stack.pop(), ()):
            dep = real(dep)
            if dep not in out:
                out.add(dep)
                stack.append(dep)
    return out


def run_gate(tree):
    errors = []
    # ---- 1. reference scan ------------------------------------------------------------------------------------------
    scanned = 0
    for rel, text in tree.files(GENERIC_ROOTS):
        scanned += 1
        if rel.endswith("CMakeLists.txt"):
            if SMS_CMAKE.search(strip_cmake_comments(text)):
                errors.append("%s: a generic/Genesis CMake file references an SMS product" % rel)
            continue
        code = strip_c_comments(text)
        for inc in SMS_INCLUDE.findall(code):
            if SMS_INCLUDE_BAD.search(inc):
                errors.append("%s: includes the SMS product header %s" % (rel, inc))
        m = SMS_IDENT.search(SMS_INCLUDE.sub(" ", code))
        if m:
            errors.append("%s: names the SMS product identifier %s" % (rel, m.group(0)))
    if scanned < 40:
        errors.append("too few generic/Genesis files scanned (%d)" % scanned)
    # ---- 2. link closure ----------------------------------------------------------------------------------------------
    aliases, links, known = parse_targets(tree)
    for target, allowed in ALLOWED_CLOSURE.items():
        if target not in known:
            errors.append("target %s not found" % target)
            continue
        extra = closure(target, aliases, links) - allowed
        if extra:
            errors.append("%s links outside its allowlist: %s" % (target, sorted(extra)))
    generic = {t for t in known if t not in SMS_PRODUCT_TARGETS and t != PSG}
    for target in sorted(generic):
        reached = closure(target, aliases, links) & SMS_PRODUCT_TARGETS
        if reached:
            errors.append("%s reaches SMS product targets: %s" % (target, sorted(reached)))
    if len(generic) < 10:
        errors.append("too few generic targets found (%d)" % len(generic))
    return errors


def controls():
    failed = []
    disk = Tree(ROOT)

    def text(rel):
        return (ROOT / rel).read_text(encoding="utf-8")

    def expect(name, overlay, needle):
        errors = run_gate(Tree(ROOT, overlay))
        if not any(needle in e for e in errors):
            failed.append("control %s: fault not detected (%s)" % (name, errors[:3]))

    expect("media links the SMS runtime", {"libs/media/CMakeLists.txt": text("libs/media/CMakeLists.txt") +
           "\ntarget_link_libraries(segarecomp_media PUBLIC segarecomp::runtime_master_system)\n"}, "segarecomp_media")
    expect("Genesis runtime includes an SMS header", {"platforms/genesis/runtime/runtime.h": text("platforms/genesis/runtime/runtime.h") +
           '\n#include "sms_machine.h"\n'}, "includes the SMS product header")
    expect("core links the SMS runtime", {"libs/core/CMakeLists.txt": text("libs/core/CMakeLists.txt") +
           "\ntarget_link_libraries(segarecomp_base INTERFACE segarecomp::runtime_master_system)\n"}, "segarecomp_base")
    expect("m68k links the SMS machine", {"libs/cpu/m68k/CMakeLists.txt": text("libs/cpu/m68k/CMakeLists.txt") +
           "\ntarget_link_libraries(segarecomp_cpu_m68k PUBLIC segarecomp::machine_master_system)\n"}, "segarecomp_cpu_m68k")
    expect("SMS viewer links the recompiler", {"platforms/master-system/viewer/CMakeLists.txt":
           text("platforms/master-system/viewer/CMakeLists.txt") +
           "\ntarget_link_libraries(segarecomp_viewer_master_system PUBLIC segarecomp::recompiler)\n"}, "segarecomp_viewer_master_system links outside")
    expect("PSG links a library", {"libs/device/sega/psg/CMakeLists.txt": text("libs/device/sega/psg/CMakeLists.txt") +
           "\ntarget_link_libraries(segarecomp_device_psg PUBLIC segarecomp::base)\n"}, "segarecomp_device_psg links outside")
    expect("SMS runtime links the M68k CPU", {"platforms/master-system/runtime/CMakeLists.txt":
           text("platforms/master-system/runtime/CMakeLists.txt") + "\ntarget_link_libraries(segarecomp_runtime_master_system PUBLIC segarecomp::cpu_m68k)\n"},
           "segarecomp_runtime_master_system links outside")
    expect("a generic library names an SMS identifier", {"libs/media/src/rom.cpp": text("libs/media/src/rom.cpp") +
           "\nstatic int leak = (int)sizeof(SmsMachine);\n"}, "SMS product identifier")
    expect("a generic CMake file names an SMS target", {"libs/recompiler/CMakeLists.txt": text("libs/recompiler/CMakeLists.txt") +
           "\nadd_dependencies(segarecomp_recompiler segarecomp_runtime_master_system)\n"}, "references an SMS product")
    # positive controls: prose and the classifier vocabulary are not dependencies
    prose = run_gate(Tree(ROOT, {"libs/media/src/rom.cpp": text("libs/media/src/rom.cpp") +
                                 "\n/* the SMS runtime (sms_machine.h) consumes this */\n// Master System note\n"}))
    if prose:
        failed.append("control: comment prose was flagged: %s" % prose[:3])
    return failed, disk


# ---- witness: no request channel for PAL / the SMS 1 VDP ---------------------------------------------------------------
WORDS = re.compile(r"pal|ntsc|region|profile|tv|standard|revision|5124|sms1|vdp[-_]?version", re.I)


def witness():
    errors = []
    header = (ROOT / "platforms/master-system/machine/include/segarecomp/machine/master_system/cartridge.hpp").read_text(encoding="utf-8")
    body = re.search(r"struct IngestOptions \{(.*?)\n\};", header, re.S)
    fields = set(re.findall(r"^\s*(?:std::vector<[^>]+>|bool|int|unsigned|std::string)\s+(\w+)", strip_c_comments(body.group(1)), re.M)) if body else None
    if fields != {"explicit_profile", "declarations"}:
        errors.append("IngestOptions fields changed (%s): a new option may be a PAL/SMS1 request channel; update the witness" % fields)
    flags = {}
    for rel in ("apps/segarecomp/build_command.cpp", "platforms/master-system/headless/sms_headless.c",
                "platforms/master-system/viewer/sms_viewer.c"):
        flags[rel] = set(re.findall(r'"(--[a-z0-9-]+)"', (ROOT / rel).read_text(encoding="utf-8")))
        if not flags[rel]:
            errors.append("%s: no option names found" % rel)
        for flag in sorted(flags[rel]):
            if WORDS.search(flag[2:]) and flag != "--platform":
                errors.append("%s: option %s could select a TV standard/VDP revision/profile" % (rel, flag))
    build = (ROOT / "apps/segarecomp/build_command.cpp").read_text(encoding="utf-8")
    if "explicit_profile = options.platform ==" not in build:
        errors.append("the build route no longer sets explicit_profile from the platform flag only")
    cart = (ROOT / "platforms/master-system/machine/src/cartridge.cpp").read_text(encoding="utf-8")
    if len(re.findall(r'profile_name\b', cart)) < 2 or re.search(r'identity\.profile\s*=\s*(?!profile_name)', cart):
        errors.append("the accepted profile is no longer the single constant profile_name")
    return errors


def main():
    errors = run_gate(Tree(ROOT))
    control_errors, _ = controls()
    witness_errors = witness()
    for e in errors + control_errors + witness_errors:
        print("FAIL:", e)
    if errors or control_errors or witness_errors:
        return 1
    print("sms dependency gate: ok (reference scan, link closure, 9 negative controls, profile witness)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
