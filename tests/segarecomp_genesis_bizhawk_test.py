#!/usr/bin/env python3
"""SEG-080-T001: tests of the BizHawk TAS-vs-control adapter's pure logic (standard library, synthetic data only; no BizHawk/ROM/Docker).

Covers the Lua scenario rendering, the .bk2 inspection (synthetic archive), per-cell correctness/marginal sets, class accuracy and the
code-as-data count, the Gate A / Gate B / Gate C decision logic including the ordering of failure classes, the DATA_ONLY_PROMISE
secondary, instruction n-gram and cell-pattern extraction, the region bootstrap's determinism, and the frozen-plan invariants.
"""
import io
import json
import pathlib
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
try:
    import resource  # noqa: F401  (POSIX only; the adapter uses it only when it launches BizHawk, which is not exercised here)
except ImportError:
    import types
    sys.modules["resource"] = types.ModuleType("resource")
import segarecomp_genesis_bizhawk as bz  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_lua_and_movie():
    lua = bz.render_lua({"mode": "random", "frames": 600, "seed": 1, "start_window": 40}, "/o", "/r")
    check("__MODE__" not in lua and "__OUT__" not in lua and 'mode = "random"' in lua and "seed = 1" in lua, "lua placeholders substituted")
    try:
        bz.render_lua({"mode": "bogus", "frames": 1}, "/o", "/r")
        check(False, "unknown mode rejected")
    except bz.ge.EvidenceError:
        pass
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "m.bk2"
        with zipfile.ZipFile(p, "w") as z:
            z.writestr("Header.txt", "emuVersion Version 2.9.1\nPlatform GEN\nCore Genplus-gx\nSHA1 AB\n")
            z.writestr("SyncSettings.json", json.dumps({"o": {"UseSixButton": True}}))
            z.writestr("Input Log.txt", "[Input]\nLogKey:#Power|#P1 Up|#P2 Up|\n|.|.|U|\n|.|.|.|\n[/Input]\n")
        m = bz.inspect_movie(p)
        check(m["frames"] == 2 and m["players_in_log"] == 2 and not m["starts_from_savestate"], "movie inspection")
        check(m["p2_frames_with_input"] == 1, "player 2 input counted")


def test_cells_and_accuracy():
    n = 4 * bz.CELL
    truth = bytearray(b"D" * n)
    img = bytes(i % 7 for i in range(n))
    truth[0:4] = b"Icci"[:1] + b"ccc"         # instruction at 0 (I + 3 c)
    truth[bz.CELL:bz.CELL + 2] = b"Ic"
    truth = bytes(truth)
    pred = bytearray(b"U" * n)
    pred[0:4] = b"Iccc"
    pred[4:bz.CELL] = b"D" * (bz.CELL - 4)                   # mixed positive cell 0
    pred[2 * bz.CELL:3 * bz.CELL] = b"D" * bz.CELL           # nontrivial negative (img not constant)
    pred[3 * bz.CELL:3 * bz.CELL + 2] = b"ID"                # wrong: false start / data-as-code
    ctl = bytearray(b"U" * n)
    cs = bz.cell_sets(bytes(pred), truth, img)
    check(0 in cs["pos"] and 0 in cs["mixed"] and 2 in cs["neg"] and 3 in cs["bad"], "cell predicates and bad cell")
    cc = bz.cell_sets(bytes(ctl), truth, img)
    mg = bz.marginal(cs, cc)
    check(mg["pos"] == [0] and mg["neg"] == [2] and mg["mixed"] == [0], "marginal cells")
    acc = bz.class_accuracy(bytes(pred), truth)
    check(acc["I"]["wrong"] == 1 and acc["false_starts"] == 1 and acc["code_as_data"] == 0, "accuracy and false starts")
    bad = bytearray(pred)
    bad[bz.CELL:bz.CELL + 2] = b"DD"                          # D over truth code
    check(bz.class_accuracy(bytes(bad), truth)["code_as_data"] == 2, "code-as-data counted")
    gb = bz.pick_gate_b(bytes(bad), bytes(ctl), truth)
    check(not gb["pass"], "gate B fails on code-as-data")
    clean = bytearray(truth)
    for i in range(n):
        if i % 3:
            clean[i] = 85
    check(bz.pick_gate_b(bytes(clean), bytes(ctl), truth)["pass"], "gate B passes on sound partial labels")


def test_decide():
    good_a = {"replay_ok": True, "tracing_ok": True}
    gb = {"pass": True}
    hi = {"extra_pos_correct": 30, "extra_mixed_correct": 9, "extra_neg_correct": 10}
    check(bz.decide(good_a, gb, hi, 1.0)["primary"] == "ACQUISITION_PROMISING", "all gates pass")
    check(bz.decide(good_a, gb, hi, 9.0)["primary"] == "BUDGET_EXHAUSTED", "budget")
    check(bz.decide({"replay_ok": False, "tracing_ok": True}, gb, hi, 1)["primary"] == "REPLAY_INVALID", "replay")
    check(bz.decide({"replay_ok": True, "tracing_ok": False}, gb, hi, 1)["primary"] == "TRACING_UNAVAILABLE", "tracing")
    check(bz.decide(good_a, {"pass": False}, hi, 1)["primary"] == "LABELS_UNSAFE", "labels")
    low = {"extra_pos_correct": 23, "extra_mixed_correct": 9, "extra_neg_correct": 250}
    r = bz.decide(good_a, gb, low, 1)
    check(r == {"primary": "INSUFFICIENT_CODE_YIELD", "secondary": "DATA_ONLY_PROMISE"}, "thresholds exact, secondary reported")
    check(bz.decide(good_a, gb, dict(hi, extra_mixed_correct=7), 1)["primary"] == "INSUFFICIENT_CODE_YIELD", "mixed threshold")
    check(bz.decide(good_a, gb, dict(hi, extra_pos_correct=24, extra_mixed_correct=8), 1)["primary"] == "ACQUISITION_PROMISING", "boundary")


def test_novelty_and_bootstrap():
    tokens = bytearray(64 * bz.ge.TOKEN_FIELDS)
    for w in range(32):
        tokens[w * 8:(w + 1) * 8] = bytes([2, 1, w % 3, 0, 1, 1, 2, 0])
    cls = bytearray(b"D" * 64)
    for pc in range(0, 40, 2):
        cls[pc] = ord("I")
        cls[pc + 1] = ord("c")
    sigs = bz.signatures(b"", bytes(cls), bytes(tokens))
    check(len(sigs) == 20 and all(s[2] for s in sigs[1:]), "signatures contiguous")
    check(len(bz.ngrams(sigs)) >= 3, "ngrams")
    check(bz.cell_patterns(b"D" * 512) == {"NNN"}, "cell patterns")
    tas = {"pos": set(range(0, 100)), "mixed": {1, 2}, "neg": set(range(500, 800)), "bad": set()}
    ctl = {"pos": set(range(0, 50)), "mixed": set(), "neg": set(), "bad": {60}}
    a, b = bz.bootstrap_extra(tas, ctl), bz.bootstrap_extra(tas, ctl)
    check(a == b and a["extra_pos"]["p2.5"] <= a["extra_pos"]["median"] <= a["extra_pos"]["p97.5"], "bootstrap deterministic")


def test_gate_a():
    ident = {"rom_ok": True, "movie_rom_md5_matches": True, "movie": {"starts_from_savestate": False, "players_in_log": 2, "frames": 100,
             "header": {"Core": "Genplus-gx", "emuVersion": "Version 2.9.1"}}}
    zs = [0x00, 0x0D, 0x0D, 0x0F, 0x07]
    ck = [{"tag": "ck", "frame": c, "state": f"zone={zs[i]} act=0 mode=12 x=1 y=1 shot=/x"} for i, c in enumerate(bz.CHECKPOINTS)]
    run = {"pcs_sha256": "a", "reads_sha256": "b", "edges_sha256": "c", "map_mismatch": 0, "odd_pcs": 0, "reset_pc_executed": True,
           "frames": bz.FRAMES, "movie_finished_at": 5, "movie_mode_end": "FINISHED", "read_self_fetch_fraction": 0.01,
           "vbl_pc_executed": True, "cdl_sha256": "d", "cdl": {"cdl_exec_bytes": 100, "cdl_exec_mismatch_bytes": 0}}
    lab = {"meta": {"typed": {}, "decoders": {"executed_rom_pcs": 100}, "tiers": {"E": 100}}}
    ga = bz.gate_a(ident, run, run, dict(run), ck, [dict(x) for x in ck], lab, "zone=14 act=0 mode=12", (run, dict(run)), True)
    check(ga["replay_ok"] and ga["tracing_ok"], "gate A passes on a clean synthetic record")
    zz = [{"tag": "ck", "frame": c, "state": f"zone={z} act=0 mode=12 x=1 y=1 shot="} for c, z in zip(bz.CHECKPOINTS, (0x0F, 0x0D, 0x0D, 0x00, 0x07))]
    check(not bz.gate_a(ident, run, run, dict(run), zz, zz, lab, "zone=14 act=0 mode=12", (run, dict(run)), True)["progression"], "zones out of play order fail")
    check(not bz.gate_a(ident, run, run, dict(run), ck, ck, lab, "zone=3 act=0 mode=4", (run, dict(run)), True)["replay_ok"], "wrong end state fails")
    flat = [{"tag": "ck", "frame": c, "state": "zone=0 act=0 mode=12 x=1 y=1 shot="} for c in bz.CHECKPOINTS]
    check(not bz.gate_a(ident, run, run, dict(run), flat, flat, lab, "zone=14 act=0 mode=12", (run, dict(run)), True)["replay_ok"], "no zone progression is REPLAY_INVALID")
    diff = dict(run, pcs_sha256="z")
    check(not bz.gate_a(ident, run, run, diff, ck, ck, lab, "zone=14 act=0 mode=12", (run, dict(run)), True)["tracing_ok"], "non-deterministic repeat blocks tracing")
    lab2 = {"meta": {"typed": {}, "decoders": {"executed_rom_pcs": 100}, "tiers": {"E": 90}}}
    check(not bz.gate_a(ident, run, run, dict(run), ck, ck, lab2, "zone=14 act=0 mode=12", (run, dict(run)), True)["tracing_ok"], "low accepted-E fraction blocks tracing")


def test_cdl_and_fixture():
    rom, truth = bz.build_fixture()
    check(len(rom) == 0x20000 and truth["dma"] == [(0x1000, 0x1040)], "fixture truth")
    size = 0x2000
    cdl = bytearray(size)
    for pc in (0x200, 0x208):
        cdl[pc] = cdl[pc + 1] = bz.CDL_EXEC
    cdl[0x1000:0x1040] = bytes([bz.CDL_DMA]) * 0x40
    cdl[0x1100] = cdl[0x1101] = bz.CDL_DATA          # a word read at 0x1100
    cdl[0x1201] = bz.CDL_DATA                         # a byte read at an odd address
    reads = {0x208: {0x1100}, 0x200: {0x1201, 0x1300}}   # 0x1300 not CDL data (prefetch-like): dropped
    gated, dma, summ = bz.cdl_apply(bytes(cdl), [0x200, 0x208], reads, size)
    check(gated == {0x208: {0x1100, 0x1101}, 0x200: {0x1201}}, "reads gated by CDL data; word partner added")
    check(dma == [(0x1000, 0x1040)] and summ["cdl_dma_bytes"] == 64 and summ["callback_reads_dropped_by_cdl"] == 1, "DMA extent")
    check(summ["cdl_exec_mismatch_bytes"] == 0, "exec flags agree with callback PCs")
    _, _, bad = bz.cdl_apply(bytes(cdl), [0x200, 0x208, 0x210], reads, size)
    check(bad["cdl_exec_mismatch_bytes"] == 2, "a callback PC without a CDL exec flag is a mismatch")
    # a byte read at b+1 by another reader forbids the word-partner rule
    g2, _, _ = bz.cdl_apply(bytes(cdl), [0x200], {0x200: {0x1100}, 0x300: {0x1101}}, size)
    check(g2 == {0x200: {0x1100}, 0x300: {0x1101}}, "no partner when a callback saw the odd byte")


def test_plan():
    if bz.PLAN.exists():
        check(bool(bz.verify_plan()), "frozen plan verifies")
    doc = bz.PROTOCOL
    check(doc["gate_c"]["extra_code_positive_cells_min"] == 24 and doc["gate_c"]["extra_mixed_cells_min"] == 8, "thresholds as pre-registered")
    check(doc["gate_b"]["new_label_accuracy_min"] == 0.999 and doc["window"]["frames"] == 54000, "gate B and window as pre-registered")
    check(doc["window"]["checkpoints"] == [7200, 18000, 28800, 39600, 54000], "frozen checkpoints")


def main():
    for t in (test_lua_and_movie, test_cells_and_accuracy, test_decide, test_novelty_and_bootstrap, test_gate_a, test_cdl_and_fixture, test_plan):
        t()
    print("FAILURES:", len(FAILURES), *FAILURES)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
