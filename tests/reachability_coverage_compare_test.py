#!/usr/bin/env python3
"""SEG-026-T001: hermetic checks for the challenger CLI, the private O-vs-D comparison and the bridge's
--execution-coverage flag validation (project-authored synthetic image; no ROM).

SEG-026-T002: --pc-index-recovery CLI aggregates and the comparison's falsification of proven PC-indexed target
sets (an observed escape outside a proven set is counted; runtime coverage never feeds the challenger).

SEG-030-T003: the generalized per-family falsification of every resolved computed site (`computed_sites` of the M68K core
report), counting retire and interrupt-resumption witnesses."""
import json
import pathlib
import subprocess
import sys
import tempfile


def words(image: bytearray, at: int, values: list[int]) -> None:
    for value in values:
        image[at:at + 2] = value.to_bytes(2, "big")
        at += 2


def synthetic_image() -> bytes:
    image = bytearray(0x800)
    image[0:4] = (0x00FFFE00).to_bytes(4, "big")
    image[4:8] = (0x200).to_bytes(4, "big")
    image[30 * 4:30 * 4 + 4] = (0x300).to_bytes(4, "big")  # IRQ6 handler
    # 0x200: BSR.S $210; JMP (2,PC,D0.W) at 0x202 -> table-selected targets 0x230 / 0x240 (unknown statically)
    words(image, 0x200, [0x610E, 0x4EFB, 0x0002, 0x602A, 0x6036])
    words(image, 0x210, [0x4E71, 0x4E75])        # callee: NOP; RTS
    words(image, 0x230, [0x4E71, 0x61DC, 0x60FE])  # reached only through the PC-indexed jump: NOP; BSR.S $210; BRA *
    words(image, 0x240, [0x4E71, 0x60FC])
    words(image, 0x300, [0x4E71, 0x4E73])        # handler: NOP; RTE
    words(image, 0x600, [0x4E71, 0x4E71, 0x4E75])  # unreachable legal island
    return bytes(image)


def recovery_image() -> bytes:
    image = bytearray(0x800)
    image[0:4] = (0x00FFFE00).to_bytes(4, "big")
    image[4:8] = (0x200).to_bytes(4, "big")
    # 0x200: MOVE.B ($F100).W,D0; ANDI.W #4,D0; JMP (2,PC,D0.W) at 0x208 -> proven targets 0x20C / 0x210 exactly.
    words(image, 0x200, [0x1038, 0xF100, 0x0240, 0x0004, 0x4EFB, 0x0002])
    words(image, 0x20C, [0x60FE, 0x4E71, 0x60FE, 0x4E71, 0x60FE])  # 0x214: legal code outside the proven set
    return bytes(image)


def check_recovery(root: pathlib.Path, segarecomp: str, compare: pathlib.Path, tmpdir: pathlib.Path) -> None:
    rom = tmpdir / "recovery.bin"
    rom.write_bytes(recovery_image())
    base = [segarecomp, "genesis-reachability-challenger", "--rom", str(rom), "--entry", "00000200", "--mapping-base",
            "00000000", "--rom-sha256", "0" * 64]
    bad = subprocess.run(base + ["--private-output", str(tmpdir / "bad.json"), "--pc-index-width-domains"],
                         capture_output=True, text=True)
    assert bad.returncode == 2, "the width-domain variant requires --pc-index-recovery"
    private = tmpdir / "recovery.json"
    observed = {0x200, 0x204, 0x208, 0x20C, 0x214}
    pcs = tmpdir / "recovery-pcs.txt"
    pcs.write_text("".join(f"{pc:06x}\n" for pc in sorted(observed)))
    classification = tmpdir / "recovery-classification.json"
    result = subprocess.run(base + ["--private-output", str(private), "--pc-index-recovery", "--classify-pcs", str(pcs),
                                    "--classify-output", str(classification)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    aggregate = json.loads(result.stdout)
    recovery = aggregate["pc_index_recovery"]
    assert recovery["sites_encountered"] == 1 and recovery["resolved"] == 1 and recovery["recovered_targets"] == 2, recovery
    assert aggregate["sites"]["jmp_(d8,PC,Xn)"] == 0 and aggregate["discovered"] == 5, aggregate
    assert "000214" not in result.stdout and "0x" not in result.stdout
    challenger = json.loads(private.read_text())
    assert challenger["pc_index_sites"]["000208"]["targets"] == ["00020c", "000210"], challenger
    # Observed: the proven target 0x20C, and an escape to 0x214 first entered from the resolved site.
    coverage = tmpdir / "recovery-coverage"
    coverage.mkdir()
    (coverage / "coverage.bitmap").write_bytes(bitmap_of(observed))
    witnesses = [(0, 0x0, 0x200, 0), (1, 0x200, 0x204, 1), (2, 0x204, 0x208, 1), (3, 0x208, 0x20C, 1),
                 (4, 0x208, 0x214, 1)]
    (coverage / "witnesses.txt").write_text("".join(f"{o} {p:08x} {c:08x} {k}\n" for o, p, c, k in witnesses))
    out = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger", str(private),
                          "--classification", str(classification)], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    report = json.loads(out.stdout)
    check = report["pc_index_recovery_check"]
    assert check["escapes_outside_proven_targets"] == 1 and check["sites_with_escapes"] == 1, check
    assert check["proven_targets"] == 2 and check["proven_targets_observed"] == 1, check
    assert report["structural_first_gate_missing_pcs"] == {"pc_index_recovery_escape": 1}, report
    assert report["observed_pc_index_sites_by_local_domain"] == {"resolved": 1}, report
    assert "000214" not in out.stdout and "0x" not in out.stdout


def check_computed_sites(compare: pathlib.Path, tmpdir: pathlib.Path) -> None:
    """Synthetic private core report (project-authored PCs): a resolved jsr_an site, a resolved pc_index_explicit site and an
    unresolved jmp_an site. Observed: one in-set target of each resolved site, one escape from the jsr_an site entered on an
    interrupt resumption, and an arbitrary entry from the unresolved site (never an escape)."""
    private = tmpdir / "core.json"
    private.write_text(json.dumps({
        "aggregate": {"exception_model": "strict"},
        "discovered": ["000200", "000204", "000300", "000400"],
        "sites": {"jsr_(An)": [], "jmp_(An)": ["000208"], "jmp_(d8,PC,Xn)": []},
        "computed_sites": {
            "000204": {"family": "jsr_an", "outcome": "resolved", "reason": "none", "detail": "none", "targets": ["000300"]},
            "000206": {"family": "pc_index_explicit", "outcome": "resolved", "reason": "none", "detail": "none",
                       "targets": ["000400", "000410"]},
            "000208": {"family": "jmp_an", "outcome": "unknown", "reason": "unknown_input", "detail": "base_unknown",
                       "targets": []}}}))
    coverage = tmpdir / "core-coverage"
    coverage.mkdir()
    observed = {0x200, 0x204, 0x206, 0x208, 0x300, 0x400, 0x500, 0x600}
    (coverage / "coverage.bitmap").write_bytes(bitmap_of(observed))
    witnesses = [(0, 0x0, 0x200, 0), (1, 0x200, 0x204, 1), (2, 0x204, 0x300, 1), (3, 0x300, 0x206, 1),
                 (4, 0x206, 0x400, 1), (5, 0x400, 0x208, 1), (6, 0x208, 0x600, 1), (7, 0x204, 0x500, 4)]
    (coverage / "witnesses.txt").write_text("".join(f"{o} {p:08x} {c:08x} {k}\n" for o, p, c, k in witnesses))
    out = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger", str(private)],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    check = json.loads(out.stdout)["computed_site_escape_check"]
    assert check["resolved_sites"] == 2 and check["escapes_outside_proven_targets"] == 1, check
    assert check["sites_with_escapes"] == 1, check
    jsr = check["families"]["jsr_an"]
    assert jsr["first_entries_from_resolved_sites"] == 2 and jsr["escapes_outside_proven_targets"] == 1, jsr
    assert jsr["proven_targets_observed"] == 1 and jsr["resolved_sites_executed"] == 1, jsr
    pc_index = check["families"]["pc_index_explicit"]
    assert pc_index["escapes_outside_proven_targets"] == 0 and pc_index["proven_targets"] == 2, pc_index
    assert "jmp_an" not in check["families"], check
    assert "000500" not in out.stdout and "0x" not in out.stdout


def bitmap_of(pcs: set[int]) -> bytes:
    data = bytearray(1 << 20)
    for pc in pcs:
        index = pc >> 1
        data[index >> 3] |= 1 << (index & 7)
    return bytes(data)


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    segarecomp = sys.argv[2]
    compare = root / "tools" / "reachability_coverage_compare.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)
        rom = tmpdir / "image.bin"
        rom.write_bytes(synthetic_image())
        private = tmpdir / "challenger.json"
        result = subprocess.run([segarecomp, "genesis-reachability-challenger", "--rom", str(rom), "--entry", "00000200",
                                 "--mapping-base", "00000000", "--rom-sha256", "0" * 64, "--private-output", str(private)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        aggregate = json.loads(result.stdout)
        assert aggregate["discovered"] == 6, aggregate  # 200, 210, 212, continuation 202, handler 300, 302
        assert aggregate["sites"]["jmp_(d8,PC,Xn)"] == 1 and aggregate["sites"]["rte"] == 1
        challenger = json.loads(private.read_text())
        discovered = {int(x, 16) for x in challenger["discovered"]}
        assert 0x600 not in discovered and 0x230 not in discovered, discovered
        # Usage errors fail closed.
        bad = subprocess.run([segarecomp, "genesis-reachability-challenger", "--rom", str(rom), "--reset-entry"],
                             capture_output=True, text=True)
        assert bad.returncode == 2
        # Synthetic observed execution: the root path, the callee, the handler, a table target first entered from
        # the PC-indexed site, a PC first executed on an interrupt resumption whose architectural predecessor is
        # the table target, and the continuation of an undiscovered caller first entered by the callee's RTS.
        coverage = tmpdir / "coverage"
        coverage.mkdir()
        observed = {0x200, 0x210, 0x212, 0x202, 0x230, 0x232, 0x234, 0x300, 0x302}
        (coverage / "coverage.bitmap").write_bytes(bitmap_of(observed))
        witnesses = [(0, 0x0, 0x200, 0), (1, 0x200, 0x210, 1), (2, 0x210, 0x212, 1), (3, 0x212, 0x202, 1),
                     (4, 0x202, 0x230, 1), (5, 0x230, 0x300, 2), (6, 0x300, 0x302, 1), (7, 0x230, 0x232, 4),
                     (9, 0x212, 0x234, 1)]
        (coverage / "witnesses.txt").write_text("".join(f"{o} {p:08x} {c:08x} {k}\n" for o, p, c, k in witnesses))
        summary = tmpdir / "summary.txt"
        summary.write_text("COVERAGE_SUMMARY " + json.dumps({"epochs": [{"frame": 1, "retirements": 5},
                                                                       {"frame": 2, "retirements": 10}]}))
        out = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger", str(private),
                              "--coverage-summary", str(summary)], capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        report = json.loads(out.stdout)
        assert report["O"] == 9 and report["D"] == 6, report
        assert report["O_minus_D"] == 3 and report["O_and_D"] == 6, report
        # Temporal (witness-only) attribution names the callee's RTS for the caller's continuation ...
        assert report["first_miss_edges_by_category"] == {"pc_indexed": 1, "ordinary_rts": 1}, report
        assert report["missing_pcs_attributed_by_category"] == {"pc_indexed": 2, "ordinary_rts": 1}, report
        assert report["first_miss_distinct_sites_by_category"] == {"pc_indexed": 1, "ordinary_rts": 1}, report
        assert report["rte_rtr_counterexamples_to_normal_resumption"] == 0
        assert [c["distinct"] for c in report["checkpoints"]] == [5, 9], report
        # ... while structural attribution (private classification of observed PCs) follows the return to its
        # observed caller and finds the single PC-indexed gate.
        pcs = tmpdir / "pcs.txt"
        pcs.write_text("".join(f"{pc:06x}\n" for pc in sorted(observed)))
        classification = tmpdir / "classification.json"
        classify = subprocess.run([segarecomp, "genesis-reachability-challenger", "--rom", str(rom), "--entry",
                                   "00000200", "--mapping-base", "00000000", "--rom-sha256", "0" * 64, "--private-output",
                                   str(tmpdir / "again.json"), "--classify-pcs", str(pcs), "--classify-output",
                                   str(classification)], capture_output=True, text=True)
        assert classify.returncode == 0, classify.stderr
        assert json.loads((tmpdir / "again.json").read_text())["discovered"] == challenger["discovered"], \
            "classification never changes D"
        out = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger", str(private),
                              "--classification", str(classification)], capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        report = json.loads(out.stdout)
        assert report["structural_first_gate_missing_pcs"] == {"jmp_(d8,PC,Xn)": 3}, report
        assert report["structural_first_gate_distinct_sites"] == {"jmp_(d8,PC,Xn)": 1}, report
        assert report["nearest_mechanism_missing_pcs"] == {"jmp_(d8,PC,Xn)": 3}, report
        assert report["missing_first_entry_kinds"] == {"call_return": 1, "dynamic": 1, "fixed": 1}, report
        assert report["observed_dynamic_entries_by_family"] == {"jmp_(d8,PC,Xn)": 1}, report
        assert "000230" not in out.stdout and "0x" not in out.stdout
        # Aggregates only: no PC-looking hex strings in the durable output.
        assert "00000200" not in out.stdout and "000230" not in out.stdout and "0x" not in out.stdout
        # A witness/bitmap mismatch is rejected.
        (coverage / "witnesses.txt").write_text("0 00000000 00000200 0\n")
        mismatch = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger",
                                   str(private)], capture_output=True, text=True)
        assert mismatch.returncode == 3
        check_recovery(root, segarecomp, compare, tmpdir)
        check_computed_sites(compare, tmpdir)
    # Bridge flag validation happens before any generation (exit 8).
    bridge = root / "tools" / "genesis_startup_bridge.py"
    for extra in (["--coverage-epoch-frames", "10"], ["--coverage-disabled"], ["--coverage-no-render"],
                  ["--execution-coverage", "10", "--capture-frames", "1:1"],
                  ["--execution-coverage", "10", "--viewer"], ["--execution-coverage", "10", "--compare-runs"]):
        result = subprocess.run([sys.executable, str(bridge), "--rom", str(root / "README.md"), "--mode", "commercial"]
                                + extra, capture_output=True, text=True)
        assert result.returncode == 8, (extra, result.returncode, result.stderr)
    print("reachability_coverage_compare_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
