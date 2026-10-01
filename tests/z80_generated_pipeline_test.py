#!/usr/bin/env python3
"""SEG-008-T003: Z80 generated-native pipeline acceptance (hermetic: no ROM, no oracle).

usage: z80_generated_pipeline_test.py <z80_image_emitter> <cc> <product-root>

Emits full synthetic 64 KiB and banked images through the public emitter, compiles them as strict C11 (warnings as
errors) with the machine-neutral runtime ABI header and executes scenario vectors natively (bounded: every step
passes a finite deadline). Checks the runtime ABI, every typed outcome and its class, resumable outcomes, 16-bit
fetch wrap, the prefix lock, image-identity dispatch, window-relative owners, the prefix-adversarial images and
byte-identical output. The same scenarios are compared with the pinned oracle by z80_conformance_oracle_test.py.
"""
import hashlib
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FAILED = []
MAX_C_BYTES_PER_64K_IMAGE = 64 * 1024 * 1024   # bounded output (ADR 0058 budget is 128 MiB; measured about 37 MiB)


def check(cond, message):
    if not cond:
        FAILED.append(message)


def tree_hash(directory):
    digest = hashlib.sha256()
    for path in sorted(pathlib.Path(directory).iterdir()):
        if path.suffix in (".c", ".h", ".units"):
            digest.update(path.name.encode())
            digest.update(path.read_bytes())
    return digest.hexdigest()


def tree_bytes(directory):
    return sum(p.stat().st_size for p in pathlib.Path(directory).iterdir() if p.suffix in (".c", ".h"))


ABI_CHECK = r'''
#include <stdio.h>
#include <string.h>
#include "segarecomp/codegen/c11/runtime/z80_runtime.h"
int main(void) {
  static const Z80Outcome resumable[] = {Z80_OUTCOME_DEADLINE, Z80_OUTCOME_HALTED, Z80_OUTCOME_PREFIX_LOCK};
  static const Z80Outcome errors[] = {Z80_ERROR_NO_OWNER, Z80_ERROR_MUTABLE_CODE, Z80_ERROR_UNRESOLVED_FETCH_MAPPING,
      Z80_ERROR_UNKNOWN_IMAGE_IDENTITY, Z80_ERROR_EXCLUDED_FORM, Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE,
      Z80_ERROR_CODE_MISMATCH};
  const char *seen[16]; int n = 0;
  for (size_t i = 0; i < sizeof resumable / sizeof resumable[0]; ++i) {
    if (!z80_outcome_is_resumable(resumable[i]) || z80_outcome_is_error(resumable[i])) return 1;
    seen[n++] = z80_outcome_name(resumable[i]);
  }
  for (size_t i = 0; i < sizeof errors / sizeof errors[0]; ++i) {
    if (z80_outcome_is_resumable(errors[i]) || !z80_outcome_is_error(errors[i])) return 2;
    seen[n++] = z80_outcome_name(errors[i]);
  }
  if (z80_outcome_is_resumable(Z80_OUTCOME_NONE) || z80_outcome_is_error(Z80_OUTCOME_NONE)) return 3;
  for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) if (!strcmp(seen[i], seen[j])) return 4;
  Z80State s; z80_reset(&s);
  if (s.pc != 0 || s.a != 0xFF || s.f != 0xFF || s.sp != 0xFFFF || s.ix != 0xFFFF || s.im != 0 || s.iff1 || s.in_prefix_run) return 5;
  if (z80_r_add(0x80, 200) != (0x80 | (200 & 0x7F))) return 6;
  printf("abi ok: %d outcomes\n", n);
  return 0;
}
'''


def abi_test(work):
    src = work / "abi.c"
    src.write_text(ABI_CHECK)
    exe = work / "abi"
    built = z.run([CC, *z.STRICT, "-I", str(z.INCLUDE_DIR), str(src), "-o", str(exe)])
    check(built.returncode == 0, "ABI header does not compile as strict C11: " + built.stderr[:500])
    if built.returncode == 0:
        r = z.run([str(exe)], timeout=30)
        check(r.returncode == 0, "ABI outcome-class check failed (code %d)" % r.returncode)
    text = (z.INCLUDE_DIR / "segarecomp/codegen/c11/runtime/z80_runtime.h").read_text()
    includes = [l for l in text.splitlines() if l.startswith("#include")]
    check(all(l in ("#include <stddef.h>", "#include <stdint.h>") for l in includes), "ABI header includes more than C11 stdint/stddef")
    for token in ("m68k", "genesis", "cpu_z80", "segarecomp/cpu"):
        check(token not in text.lower(), "ABI header mentions %r" % token)
    for field in ("in_prefix_run", "int_line", "nmi_pending", "cycles", "deadline", "code_image", "interrupt_acknowledge",
                  "io_in", "io_out"):
        check(field in text, "ABI header lacks %s" % field)


def emit_only(spec, directory, list_owners=False, group=None):
    tc = z.Toolchain(CC, EMITTER, owner_group=group)
    stats, owners, error = z.emit_image(tc, spec, directory, "img", list_owners)
    return stats, owners, error


def main():
    doc = z.load_scenarios()
    tc = z.Toolchain(CC, EMITTER)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        abi_test(work)

        batches = z.scenario_batches(doc)
        report = z.run_batches(tc, batches, work / "run", with_oracle=False, list_owners=True)
        seen_outcomes = set()
        for batch in batches:
            r = report[batch.name]
            if r.get("error"):
                check(False, "%s: %s" % (batch.name, r["error"][:600]))
                continue
            vecs = [v for v, _ in batch.vectors]
            check(len(r["generated"]) == len(vecs), "%s: missing vector results" % batch.name)
            for vec in vecs:
                steps = r["generated"].get(vec.name, [])
                for step in steps:
                    seen_outcomes.add(step["out"])
                for problem in z.check_expectations(vec, steps):
                    check(False, problem)
            for problem in z.check_same_final(vecs, r["generated"]):
                check(False, problem)
            out_dir = work / "run" / batch.name
            c_bytes = tree_bytes(out_dir)
            check(c_bytes <= MAX_C_BYTES_PER_64K_IMAGE, "%s: generated C is %d bytes (unbounded)" % (batch.name, c_bytes))
            header = (out_dir / ("z80_%s.h" % batch.name)).read_text()
            check("z80_o_" not in header, "%s: the shared header declares owners (ADR 0058 RSS constraint)" % batch.name)
            check(len(header) < 2048, "%s: shared header is not small" % batch.name)
            main_tu = (out_dir / ("z80_%s_main.c" % batch.name)).read_text()
            check("z80_run" in main_tu and "z80_entry_lookup" in main_tu, "%s: main TU lacks the dispatcher/entry lookup" % batch.name)
            entry_tus = sorted(out_dir.glob("z80_%s_entry_*.c" % batch.name))
            chunk = doc["images"][batch.name].get("entry_chunk")
            if chunk:
                # Chunked exact lookup: the main TU keeps only the chunk directory; each chunk TU declares its own owners
                # and holds at most `chunk` entries. The flat form (every other image) keeps the table in the main TU.
                owners_total = len(r["owners"]) if r.get("owners") else 0
                check(len(entry_tus) == -(-owners_total // chunk) and len(entry_tus) > 1, "%s: expected %d entry-chunk TUs, found %d"
                      % (batch.name, -(-owners_total // chunk), len(entry_tus)))
                check("z80_o_" not in main_tu.replace("z80_entry_lookup", ""), "%s: chunked main TU still references owners" % batch.name)
                for tu in entry_tus:
                    check(tu.read_text().count("UINT32_C(0x") <= chunk + 1, "%s: %s holds more than one chunk" % (batch.name, tu.name))
            else:
                check("z80_o_" in main_tu and not entry_tus, "%s: flat entry table must live in the main TU" % batch.name)
            for path in out_dir.glob("*.c"):
                text = path.read_text().lower()
                check("m68k" not in text and "genesis" not in text, "%s: generated C mentions an M68k/Genesis symbol" % path.name)
            exe_text = pathlib.Path(r["exe"]).read_bytes().lower() if r.get("exe") else b""
            check(b"m68k" not in exe_text and b"genesis" not in exe_text, "%s: linked program carries an M68k/Genesis symbol" % batch.name)

        # Every typed outcome of the ABI is exercised by a synthetic case (excluded_form is reserved: ADR 0056 excludes
        # nothing, so it is class-checked at the ABI level only) and reported in its correct class.
        for outcome in ("deadline", "halted", "prefix_lock", "no_owner", "mutable_code", "unresolved_fetch_mapping",
                        "unknown_image_identity", "im0_unsupported_acknowledge_byte"):
            check(outcome in seen_outcomes, "no scenario exercises the %s outcome" % outcome)

        # Full 64 KiB image: every start has an owner, all direct successors bind (invariant window incl. wrap).
        wrap = report["wrap64"]
        owners = wrap["owners"]
        check(wrap["stats"]["full"] == 65536 and len(owners) == 65536, "wrap64: expected 65,536 full owners")
        check(len({(o["identity"], o["key"]) for o in owners}) == 65536, "wrap64: duplicate owner keys")
        check(wrap["stats"]["bound"] == 65536, "wrap64: fall-through of every invariant owner (incl. across 0xFFFF) binds")

        # Prefix-adversarial whole-mapping images: 65,536 prefix_lock owners, no full owner, bounded output.
        for name in ("dd64", "fd64", "alt64"):
            if name not in report:  # full-scale-only image (SEGARECOMP_Z80_FULL_SCALE=1)
                continue
            s = report[name]["stats"]
            check(s["prefix_lock"] == 65536 and s["full"] == 0 and s["stubs"] == 0, "%s: whole-mapping run is not all prefix_lock" % name)
        # Chains that wrap across 0xFFFF and terminate: full owners (each chain start is one owner).
        if "mixed64" in report:
            check(report["mixed64"]["stats"]["full"] == 65536 and report["mixed64"]["stats"]["prefix_lock"] == 0,
                  "mixed64: terminating wrapped chains must be full owners")

        # Two immutable images at the same window: distinct keys per identity, no static binding into the window.
        two = {(o["identity"], o["key"]): o for o in report["two_image"]["owners"]}
        check((2, 0x4000) in two and (3, 0x4000) in two, "two_image: both banked images own the window start")
        check(not two[(2, 0x4000)]["bound"] and not two[(3, 0x4000)]["bound"], "banked owners must not bind directly")
        check(two[(1, 0x3FFF)]["bound"] is False, "invariant owner must not bind into a banked window")
        check(two[(1, 0x0100)]["bound"] is True, "invariant owner binds to its invariant successor")
        check(two[(2, 0x7FFF)]["kind"] == "stub_unresolved_fetch_mapping", "a start crossing into another banked window is a typed stub")
        check(two[(7, 0xBFFF)]["kind"] == "stub_mutable_code", "a start crossing into non-code is a typed stub")

        # Window-relative owners.
        rel = report["reloc"]["owners"]
        keys = {(o["identity"], o["key"]) for o in rel}
        check(len(keys) == len(rel), "reloc: key collision")
        bank = [o for o in rel if o["identity"] == 2]
        check(bank and all(o["relative"] for o in bank), "reloc: banked image in two windows must use window-relative owners")
        check(not any(o["bound"] for o in bank), "reloc: a base-relative target was statically bound")
        check(all(not o["relative"] for o in rel if o["identity"] != 2), "reloc: invariant windows must keep absolute owners")
        check(len(bank) == 0x4000, "reloc: one owner per (image, offset), shared by both windows")
        by_key = {(o["identity"], o["key"]): o for o in rel}
        check(by_key[(2, 0x3FFF)]["variants"] == 2, "reloc: a crossing start is a stub in one window and a full owner in the other")
        check((1, 0x0000) in by_key and (2, 0x0000) in by_key, "reloc: invariant window and banked image share offset 0 without a key collision")
        check(report["reloc"]["stats"]["variants"] == 1, "reloc: exactly one owner selects its kind from the base")

        # Windows exposing different sub-ranges: an owner exposed by only some windows must fail closed for the others.
        partial_spec = "image 1 banked\nwindow 1 4000 0 100\nwindow 1 8000 80 80\nfill 1 00 200\n"
        # Pinned to one function per start (SEG-033 reference mode) so the text of a single owner can be inspected; the grouped
        # form of the same image is covered by the owner-group differential test and the shape check below.
        _, partial_owners, error = emit_only(partial_spec, work / "partial", True, group=1)
        check(error is None, "sub-range windows failed to emit: %s" % error)
        if error is None:
            keys = {o["key"] for o in partial_owners}
            check(keys == set(range(0x100)), "sub-range windows: owners must be the union of the exposed offsets 0..0xFF")
            text = "".join(p.read_text() for p in sorted((work / "partial").glob("*_owner_*.c")))
            def owner_text(symbol):
                start = text.index(symbol + "(struct Z80Runtime *rt, uint16_t window_base) {")  # the definition (external linkage in a shard)
                end = text.find("\n/* ", start)
                return text[start:end if end != -1 else len(text)]
            head = owner_text("z80_o_0001_0010")
            check("switch (window_base)" in head and "case 0x4000" in head and "case 0x8000" not in head and "Z80_ERROR_NO_OWNER" in head,
                  "an owner exposed only at base 0x4000 must check the window base and fail closed for 0x8000")
            check("switch (window_base)" not in owner_text("z80_o_0001_0090"),
                  "an owner exposed by every window serves every base without a base check")
            grouped, _, error = emit_only(partial_spec, work / "partial_grouped", False)
            check(error is None and grouped["shape_entries"] == 0x100 and grouped["shape_owners"] == 2,
                  "the sub-range image must group its 256 starts into two bounded owners (default bound 128)")

        # Determinism: byte-identical output across two emissions, and a byte-identical build of the same units.
        spec = z.image_spec_text(doc["images"]["wrap64"]["images"])
        for name in ("a", "b"):
            _, _, error = emit_only(spec, work / ("det_" + name))
            check(error is None, "wrap64 emission failed: %s" % error)
        check(tree_hash(work / "det_a") == tree_hash(work / "det_b"), "wrap64 output is not byte-identical across runs")
        for image_name in ("reloc", "dd64", "mixed64"):  # window-relative owners and the prefix-adversarial images (emission is cheap: always)
            other_spec = z.image_spec_text(doc["images"][image_name]["images"])
            for name in ("a", "b"):
                emit_only(other_spec, work / ("%s_%s" % (image_name, name)))
            check(tree_hash(work / (image_name + "_a")) == tree_hash(work / (image_name + "_b")),
                  "%s output is not byte-identical across runs" % image_name)
        small = z.image_spec_text([{"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "0100"]], "fill": "00",
                                    "size": 0x100}])
        exes = []
        tc_fresh = z.Toolchain(CC, EMITTER, cache=False)  # reproducibility needs two real compiles, never a cache hit
        for name in ("a", "b"):
            built = z.build_generated(tc_fresh, small, work / ("small_" + name), stem="small")
            check(not built["error"], "small build failed: %s" % built["error"])
            if not built["error"]:
                exes.append(hashlib.sha256(pathlib.Path(built["exe"]).read_bytes()).hexdigest())
        check(len(exes) == 2 and exes[0] == exes[1], "linked build is not reproducible byte for byte")

        # Fail-closed emission input.
        bad_specs = {
            "identity above 16 bits": "image 70000 invariant\nwindow 70000 0 0 100\nfill 70000 00 100\n",
            "duplicate identity": "image 1 invariant\nwindow 1 0 0 100\nfill 1 00 100\nimage 1 invariant\nwindow 1 0 0 100\nfill 1 00 100\n",
            "invariant overlaps banked": "image 1 invariant\nwindow 1 0 0 100\nfill 1 00 100\nimage 2 banked\nwindow 2 80 0 100\nfill 2 00 100\n",
            "window beyond the address space": "image 1 invariant\nwindow 1 FF00 0 200\nfill 1 00 200\n",
            "image shorter than its window": "image 1 invariant\nwindow 1 0 0 100\nfill 1 00 10\n",
            "no window": "image 1 banked\nfill 1 00 10\n",
            "windows of one image overlap at the same base": "image 1 banked\nwindow 1 4000 0 100\nwindow 1 4000 80 100\nfill 1 00 200\n",
        }
        for label, text in bad_specs.items():
            stats, _, error = emit_only(text, work / "bad", False)
            check(error is not None, "invalid input accepted: " + label)

    if FAILED:
        print("\n".join("FAIL: " + m for m in FAILED))
        return 1
    print("z80 generated-native pipeline: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
