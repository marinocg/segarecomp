#!/usr/bin/env python3
"""SEG-026-T001: hermetic checks for the challenger CLI, the private O-vs-D comparison and the bridge's
--execution-coverage flag validation (project-authored synthetic image; no ROM)."""
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
    words(image, 0x230, [0x4E71, 0x60FC])        # reached only through the PC-indexed jump
    words(image, 0x240, [0x4E71, 0x60FC])
    words(image, 0x300, [0x4E71, 0x4E73])        # handler: NOP; RTE
    words(image, 0x600, [0x4E71, 0x4E71, 0x4E75])  # unreachable legal island
    return bytes(image)


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
        # Synthetic observed execution: the root path, the callee, the handler, and a table target first entered
        # from the PC-indexed site (then its loop), plus a PC first executed on an interrupt resumption whose
        # architectural predecessor is the table target.
        coverage = tmpdir / "coverage"
        coverage.mkdir()
        observed = {0x200, 0x210, 0x212, 0x202, 0x230, 0x232, 0x300, 0x302}
        (coverage / "coverage.bitmap").write_bytes(bitmap_of(observed))
        witnesses = [(0, 0x0, 0x200, 0), (1, 0x200, 0x210, 1), (2, 0x210, 0x212, 1), (3, 0x212, 0x202, 1),
                     (4, 0x202, 0x230, 1), (5, 0x230, 0x300, 2), (6, 0x300, 0x302, 1), (7, 0x230, 0x232, 4)]
        (coverage / "witnesses.txt").write_text("".join(f"{o} {p:08x} {c:08x} {k}\n" for o, p, c, k in witnesses))
        summary = tmpdir / "summary.txt"
        summary.write_text("COVERAGE_SUMMARY " + json.dumps({"epochs": [{"frame": 1, "retirements": 5},
                                                                       {"frame": 2, "retirements": 8}]}))
        out = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger", str(private),
                              "--coverage-summary", str(summary)], capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        report = json.loads(out.stdout)
        assert report["O"] == 8 and report["D"] == 6, report
        assert report["O_minus_D"] == 2 and report["O_and_D"] == 6, report
        assert report["first_miss_edges_by_category"] == {"pc_indexed": 1}, report
        assert report["missing_pcs_attributed_by_category"] == {"pc_indexed": 2}, report
        assert report["first_miss_distinct_sites_by_category"] == {"pc_indexed": 1}, report
        assert report["rte_rtr_counterexamples_to_normal_resumption"] == 0
        assert [c["distinct"] for c in report["checkpoints"]] == [5, 8], report
        # Aggregates only: no PC-looking hex strings in the durable output.
        assert "00000200" not in out.stdout and "000230" not in out.stdout and "0x" not in out.stdout
        # A witness/bitmap mismatch is rejected.
        (coverage / "witnesses.txt").write_text("0 00000000 00000200 0\n")
        mismatch = subprocess.run([sys.executable, str(compare), "--coverage-dir", str(coverage), "--challenger",
                                   str(private)], capture_output=True, text=True)
        assert mismatch.returncode == 3
    # Bridge flag validation happens before any generation (exit 8).
    bridge = root / "tools" / "genesis_startup_bridge.py"
    for extra in (["--coverage-epoch-frames", "10"], ["--coverage-disabled"],
                  ["--execution-coverage", "10", "--capture-frames", "1:1"],
                  ["--execution-coverage", "10", "--viewer"], ["--execution-coverage", "10", "--compare-runs"]):
        result = subprocess.run([sys.executable, str(bridge), "--rom", str(root / "README.md"), "--mode", "commercial"]
                                + extra, capture_output=True, text=True)
        assert result.returncode == 8, (extra, result.returncode, result.stderr)
    print("reachability_coverage_compare_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
