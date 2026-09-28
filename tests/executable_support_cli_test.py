"""SEG-024-T001: the opt-in executable-support report on a project-authored synthetic image.

Checks: the option requires --immutable-rom-aot; the report is sanitized (aggregates/digests only, no address
or byte), deterministic across runs, and the generated C is byte-identical with and without the report.
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import executable_support_scaling  # noqa: E402  (shared synthetic image builder)


def run(segarecomp: str, rom: pathlib.Path, digest: str, extra: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([segarecomp, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry",
                           "--rom-sha256", digest, *extra], capture_output=True, text=True, timeout=600)


def main() -> None:
    segarecomp = sys.argv[1]
    image = executable_support_scaling.synthetic_image(0x10000)
    digest = hashlib.sha256(image).hexdigest()
    with tempfile.TemporaryDirectory(prefix="seg024-cli-") as scratch:
        work = pathlib.Path(scratch)
        rom = work / "synthetic.bin"
        rom.write_bytes(image)
        refused = run(segarecomp, rom, digest, ["--executable-support-report", str(work / "r.json"),
                                                "--generated-c-output", str(work / "x.c")])
        assert refused.returncode == 2 and "requires --immutable-rom-aot" in refused.stderr, refused.stderr
        outputs = []
        for index in range(2):
            report = work / f"report{index}.json"
            generated = work / f"with{index}.c"
            done = run(segarecomp, rom, digest, ["--immutable-rom-aot", "--executable-support-report", str(report),
                                                 "--generated-c-output", str(generated)])
            assert done.returncode == 0, done.stderr
            outputs.append((report.read_bytes(), generated.read_bytes()))
        baseline = work / "baseline.c"
        done = run(segarecomp, rom, digest, ["--immutable-rom-aot", "--generated-c-output", str(baseline)])
        assert done.returncode == 0, done.stderr
        assert outputs[0][0] == outputs[1][0], "report is deterministic"
        assert outputs[0][1] == outputs[1][1] == baseline.read_bytes(), "report never changes generated C"
        text = outputs[0][0].decode()
        data = json.loads(text)
        assert data["report_only"] is True and data["schema"] == "segarecomp.executable-support-experiment.v1"
        runs = {entry["label"]: entry for entry in data["runs"]}
        universe = data["universe"]["candidates"]
        assert runs["current_facts"]["sound"] and runs["current_facts"]["live"] == universe, \
            "the fixture's reachable JSR (A0) is unconstrained, so unknown -> KEEP gives L = U"
        assert not runs["current_facts_without_any"]["sound"], "ablated floors are flagged unsound"
        assert runs["fixed_flow_only"]["live"] < universe
        assert "0x" not in text and "200" not in json.dumps(data["roots"]), "no addresses in the report"
    print("executable_support_cli_test: OK")


if __name__ == "__main__":
    main()
