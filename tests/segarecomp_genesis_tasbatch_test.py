#!/usr/bin/env python3
"""SEG-081-T001: offline tests of the three-game TAS batch tooling (standard library, synthetic data only; no ROM, no emulator, no model)."""
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_genesis_tasbatch as tb  # noqa: E402
import segarecomp_genesis_tasbatch_ml as ml  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_games_table():
    check(tb.ORDER == ["vm2", "bnr", "ewj2"], "frozen execution order")
    for g, v in tb.GAMES.items():
        check(v["rom"]["md5"] == v["movie"]["header_md5"], f"{g}: movie header hash equals the ROM MD5")
        check(len(v["rom"]["sha256"]) == 64 and len(v["movie"]["sha256"]) == 64, f"{g}: digests")
        check(v["lineage"].startswith("T81_"), f"{g}: lineage id is outside the SEG-072 D## namespace")
    check(tb.GAMES["vm2"]["movie"]["frames"] == 28632 and tb.GAMES["ewj2"]["movie"]["frames"] == 71699, "published frame counts")
    check(tb.GAMES["ewj2"]["movie"]["emuVersion"] == "Version 2.6.3", "EWJ2 movie names the older emulator")


def test_generic_lua():
    lua = tb.GENERIC_LUA
    for banned in ("zone=", "0xFFFE", "0xFFB0", "0xFFF6", "rings", "lives"):
        check(banned not in lua, f"no game variable {banned} in the checkpoint Lua")
    for need in ("ram=%s", "vram=%s", "cram=%s", "vsram=%s", "client.gettool(\"cdl\")", "event.onmemoryexecuteany"):
        check(need in lua, f"generic Lua keeps {need}")




def test_policy_port_matches_lua():
    exe = shutil.which("lua5.4") or shutil.which("lua")
    if not exe:
        print("lua not available: policy equivalence test skipped")
        return
    src = tb.GENERIC_LUA
    a, b = src.index("local names ="), src.index("local ckf")
    body = src[a:b]
    for seed, frames in ((1, 5000), (2, 5000), (1, 7321)):
        prog = (f'local S = {{frames = {frames}, seed = {seed}, start_window = 40}}\n' + body +
                f'for f = 0, {frames - 1} do local p = (f == 0) and {{}} or policy(f) local t = {{}} '
                f'for _, n in ipairs(names) do if p[n] == 1 then t[#t+1] = n:sub(4) end end table.sort(t) print(table.concat(t, ",")) end\n')
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / "p.lua").write_text(prog)
            r = subprocess.run([exe, str(pathlib.Path(d) / "p.lua")], capture_output=True, text=True)
        check(r.returncode == 0, f"lua policy runs (seed {seed}): {r.stderr[:200]}")
        lua_rows = r.stdout.split("\n")[:frames]
        py_rows = [",".join(sorted(x)) for x in tb.policy_rows(frames, seed)]
        check(lua_rows == py_rows, f"python policy equals the SEG-080 Lua policy (seed {seed}, {frames} frames)")
    check(len({tuple(sorted(x)) for x in tb.policy_rows(3000, 1)}) > 4, "the policy is not degenerate")
    check(tb.policy_rows(5000, 1) != tb.policy_rows(5000, 2), "seeds differ")


def make_bk2(players=2, six=False, frames=10):
    names = ["Up", "Down", "Left", "Right", "A", "B", "C", "Start"] + (["X", "Y", "Z", "Mode"] if six else [])
    key = "LogKey:#Power|Reset|" + "".join("#P%d %s|" % (p + 1, names[0]) if False else "" for p in range(0))
    key = "LogKey:#Power|Reset|" + "".join(("#" if i == 0 else "") + f"P{p + 1} {n}|" for p in range(players) for i, n in enumerate(names))
    row = "|..|" + "".join("." * len(names) + "|" for _ in range(players))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("Header.txt", "MovieVersion BizHawk v2.0\nPlatform GEN\nSHA1 AA\nCore Genplus-gx\n")
        z.writestr("SyncSettings.json", json.dumps({"o": {"UseSixButton": six}}))
        z.writestr("Subtitles.txt", "secret subtitle")
        z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join([row] * frames) + "\n[/Input]\n")
    return buf.getvalue(), row


def test_control_movie():
    for players, six in ((1, False), (2, False), (2, True)):
        src, row = make_bk2(players, six, 10)
        out = tb.control_movie(src, 3000, 1)
        zi, zo = zipfile.ZipFile(io.BytesIO(src)), zipfile.ZipFile(io.BytesIO(out))
        check(zi.namelist() == zo.namelist(), "same archive members")
        for n in ("Header.txt", "SyncSettings.json"):
            check(zi.read(n) == zo.read(n), f"{n} is untouched (core, controllers, sync settings)")
        check(zo.read("Subtitles.txt") == b"", "TAS subtitles are blanked")
        lines = zo.read("Input Log.txt").decode().splitlines()
        rows = [x for x in lines if x.startswith("|")]
        check(len(rows) == 3000, "one row per frame")
        check(all(len(r) == len(row) for r in rows), f"row shape equals the movie's ({players}p, six={six})")
        check(any(r != row for r in rows[2400:]), "inputs appear")
        check(rows[0] == row, "row 0 is empty")
        if players == 2:
            check(all(r.split("|")[3] == "." * (12 if six else 8) for r in rows), "player 2 stays idle")


def sig_fixture():
    # words: NOP NOP BRA.s(+4) NOP NOP NOP JSR(A0) RTS ; then data
    words = [0x4E71, 0x4E71, 0x6004, 0x4E71, 0x4E71, 0x4E71, 0x4E90, 0x4E75, 0xDEAD, 0xBEEF, 0x4E71, 0x4E75]
    img = b"".join(w.to_bytes(2, "big") for w in words)
    fam = {0x4E71: 1, 0x6004: 2, 0x4E90: 3, 0x4E75: 4}
    tok = bytearray(len(words) * 8)
    for i, w in enumerate(words):
        if w in fam:
            tok[i * 8:(i + 1) * 8] = bytes([2, 1, fam[w], 0, 0, 0, 0, 0])
    cls = bytearray(b"U" * len(img))
    for i, w in enumerate(words):
        if w in fam:
            cls[i * 2:i * 2 + 2] = b"Ic"
    cls[16:20] = b"DDDD"
    return img, bytes(cls), bytes(tok)


def test_signatures():
    img, cls, tok = sig_fixture()
    s = tb.struct_sigs(img, cls, tok)
    check(len(s["forms"]) == 4, "four distinct forms")
    check(len(s["g3"]) >= 3 and len(s["g4"]) >= 2, "n-grams over consecutive starts")
    check(any("ind" in c for c in s["cfg"]), "indirect JSR recorded as an indirect cfg pattern")
    check(len(s["boundary"]) == 1, "one code/data boundary pattern")
    ref = {k: set() for k in tb.ALL_KINDS}
    ref["form_counts"] = {}
    nv = tb.novelty(s, {"r": ref})
    check(nv["r"]["N_struct"] == len(s["forms"]) + len(s["cfg"]), "everything is novel against an empty registry")
    same = {k: set(s[k]) for k in tb.ALL_KINDS}
    same["form_counts"] = s["form_counts"]
    nv2 = tb.novelty(s, {"r": same}, s)
    check(nv2["r"]["N_struct"] == 0 and nv2["r"]["rare_forms_in_target"] == 3 and nv2["r"]["N_struct_not_in_control"] == 0, "self-reference has zero novelty; counts <= RARE_MAX are rare")
    part = {k: set(list(s[k])[:1]) for k in tb.ALL_KINDS}
    part["form_counts"] = {}
    check(0 < tb.novelty(s, {"r": part})["r"]["N_struct"] < nv["r"]["N_struct"], "partial reference gives partial novelty")
    merged = tb.merge_sigs([s, s])
    check(set(merged["forms"]) == set(s["forms"]) and merged["form_counts"][next(iter(s["form_counts"]))] == 2 * s["form_counts"][next(iter(s["form_counts"]))], "merge adds counts")


def test_classification():
    base = {"input_present": True, "cpu_hours": 2.0, "replay_ok": True, "tracing_ok": True, "consistency_ok": True, "extra_cells": 120, "control_cells": 1000, "n_struct": 5}
    check(tb.classify(base)["class"] == "QUALIFIED_USEFUL", "absolute rule")
    check(tb.classify(dict(base, extra_cells=99, control_cells=1000))["class"] == "QUALIFIED_LOW_YIELD", "99 of 1000 is low yield")
    check(tb.classify(dict(base, extra_cells=30, control_cells=100))["class"] == "QUALIFIED_USEFUL", "relative rule: >= 25 and >= 20 %")
    check(tb.classify(dict(base, extra_cells=24, control_cells=10))["class"] == "QUALIFIED_LOW_YIELD", "relative rule needs 25 cells")
    check(tb.classify(dict(base, extra_cells=29, control_cells=200))["class"] == "QUALIFIED_LOW_YIELD", "29 of 200 is below 20 %")
    check(tb.classify(dict(base, n_struct=0))["class"] == "QUALIFIED_LOW_YIELD", "no novel structural signature -> low yield")
    check(tb.classify(dict(base, extra_cells=500))["ambitious_secondary"] is True, "ambitious secondary target recorded, not required")
    check(tb.classify(dict(base, input_present=False, replay_ok=False))["class"] == "INPUT_REQUIRED", "input first")
    check(tb.classify(dict(base, cpu_hours=8.1, replay_ok=False))["class"] == "BUDGET_EXHAUSTED", "budget before replay")
    check(tb.classify(dict(base, replay_ok=False, tracing_ok=False))["class"] == "REPLAY_INVALID", "replay before tracing")
    check(tb.classify(dict(base, tracing_ok=False, consistency_ok=False))["class"] == "TRACING_UNAVAILABLE", "tracing before consistency")
    check(tb.classify(dict(base, consistency_ok=False))["class"] == "LABEL_POLICY_CONFLICT", "consistency")
    check(tb.useful(100, 10 ** 6) and not tb.useful(99, 10 ** 6), "absolute boundary")


def test_batch():
    U, L = "QUALIFIED_USEFUL", "QUALIFIED_LOW_YIELD"
    d = lambda c, b=0: tb.batch_decision(dict(zip(tb.ORDER, c)), b)["decision"]
    check(d([U, U, L]) == "CROSS_ENGINE_ACQUISITION_SUPPORTED", "two useful games")
    check(d([U, U, "REPLAY_INVALID"]) == "CROSS_ENGINE_ACQUISITION_SUPPORTED", "two useful, one invalid replay")
    check(d([U, U, "LABEL_POLICY_CONFLICT"]) == "CROSS_ENGINE_ACQUISITION_MIXED", "a label conflict is an unresolved major inconsistency")
    check(d([U, U, U], 1) == "CROSS_ENGINE_ACQUISITION_MIXED", "an open review blocker prevents SUPPORTED")
    check(d([U, L, "REPLAY_INVALID"]) == "CROSS_ENGINE_ACQUISITION_MIXED", "one useful game")
    check(d([L, "INPUT_REQUIRED", "REPLAY_INVALID"]) == "CROSS_ENGINE_ACQUISITION_MIXED", "only one game qualifies")
    check(d([L, L, "REPLAY_INVALID"]) == "CROSS_ENGINE_ACQUISITION_NOT_SUPPORTED", "two qualified games, none useful")
    check(d(["INPUT_REQUIRED", "REPLAY_INVALID", "BUDGET_EXHAUSTED"]) == "EVIDENCE_INSUFFICIENT", "nothing qualified")


def trace(**kw):
    t = {"frames": 9000, "movie_finished_at": 9000, "movie_mode_end": "FINISHED", "cdl_sha256": "c", "pcs_sha256": "p", "reads_sha256": "r", "edges_sha256": "e",
         "new_pcs_per_growth_window": {"0": 10, "1": 5, "2": 4, "3": 3, "4": 1}, "map_mismatch": 0, "odd_pcs": 0, "reset_pc_executed": True, "vbl_pc_executed": True,
         "read_self_fetch_fraction": 0.001, "cdl": {"cdl_exec_mismatch_bytes": 0, "cdl_exec_bytes": 1000}}
    t.update(kw)
    return t


def cks(n, vary=True, shift=0):
    out = []
    for i in range(1, 6):
        v = (i if vary else 1) + shift
        out.append({"tag": "ck", "frame": 9000 * i // 6, "state": f"ram=R{v} vram=V{v} cram=C{v} vsram=S{v} uniq=1 reads=1 edges=1 shot="})
    out.append({"tag": "final", "frame": 9000, "state": "ram=R9 vram=V9 cram=C9 vsram=S9 uniq=1 reads=1 edges=1 shot="})
    return out


def test_gates():
    ident = {"ok": True}
    g = tb.replay_gate(ident, 9000, trace(), trace(), trace(), cks(9000), cks(9000), cks(9000))
    check(g["replay_ok"], f"healthy replay passes {g}")
    check(not tb.replay_gate({"ok": False}, 9000, trace(), trace(), trace(), cks(9000), cks(9000), cks(9000))["replay_ok"], "identity failure")
    check(not tb.replay_gate(ident, 9000, trace(frames=8999), trace(), trace(), cks(9000), cks(9000), cks(9000))["replay_ok"], "playback must emulate N frames")
    check(not tb.replay_gate(ident, 9000, trace(movie_finished_at=-1, movie_mode_end="PLAYING"), trace(), trace(), cks(9000), cks(9000), cks(9000))["replay_ok"], "movie must finish")
    check(not tb.replay_gate(ident, 9000, trace(), trace(), trace(), cks(9000), cks(9000, vary=False), cks(9000, vary=False))["replay_ok"], "stuck state (no progression)")
    check(not tb.replay_gate(ident, 9000, trace(), trace(), trace(), cks(9000), cks(9000), cks(9000, shift=1))["replay_ok"], "checkpoint digests must repeat")
    check(not tb.replay_gate(ident, 9000, trace(), trace(), trace(pcs_sha256="x"), cks(9000), cks(9000), cks(9000))["replay_ok"], "trace repeat must be identical")
    check(not tb.replay_gate(ident, 9000, trace(), trace(), trace(), cks(9000, shift=3), cks(9000), cks(9000))["replay_ok"], "tracing may not perturb emulation")
    check(not tb.replay_gate(ident, 9000, trace(), trace(new_pcs_per_growth_window={"0": 5}), trace(), cks(9000), cks(9000), cks(9000))["replay_ok"], "new code in >= 3 fifths")
    na = cks(9000)
    na[1]["state"] = na[1]["state"].replace("cram=C2", "cram=na")
    check(not tb.replay_gate(ident, 9000, trace(), trace(), trace(), na, na, na)["replay_ok"], "an unreadable (na) digest domain invalidates the replay gate")
    lab = {"meta": {"decoders": {"executed_rom_pcs": 100}, "tiers": {"E": 100}, "typed": {}}}
    check(tb.tracing_gate(trace(), lab, [trace(), trace()], 9000, True)["tracing_ok"], "healthy tracing")
    check(not tb.tracing_gate(trace(map_mismatch=1), lab, [trace()], 9000, True)["tracing_ok"], "ROM mapping mismatch")
    check(not tb.tracing_gate(trace(), lab, [trace(frames=10)], 9000, True)["tracing_ok"], "control frame count")
    check(not tb.tracing_gate(trace(), lab, [trace()], 9000, False)["tracing_ok"], "CDL fixture")
    bad = {"meta": {"decoders": {"executed_rom_pcs": 100}, "tiers": {"E": 90}, "typed": {"E_LEN": 10}}}
    check(not tb.tracing_gate(trace(), bad, [trace()], 9000, True)["tracing_ok"], "accepted-E fraction")


def test_failure_classes():
    with tempfile.TemporaryDirectory() as d:
        pr = pathlib.Path(d)
        check(tb.failure_class(pr, "vm2") == "INPUT_REQUIRED", "no identity file -> input required")
        (pr / "identity.json").write_text(json.dumps({"ok": False}))
        check(tb.failure_class(pr, "vm2") == "INPUT_REQUIRED", "identity mismatch rejects the game")
        (pr / "identity.json").write_text(json.dumps({"ok": True}))
        check(tb.failure_class(pr, "vm2") == "REPLAY_INVALID", "no playback trace -> replay invalid, never input required")
        n = tb.GAMES["vm2"]["movie"]["frames"]
        (pr / "runs" / "playback").mkdir(parents=True)
        (pr / "runs" / "playback" / "trace.json").write_text(json.dumps({"frames": n - 5, "movie_finished_at": -1, "movie_mode_end": "PLAYING"}))
        check(tb.failure_class(pr, "vm2") == "REPLAY_INVALID", "short playback")
        (pr / "runs" / "playback" / "trace.json").write_text(json.dumps({"frames": n, "movie_finished_at": n, "movie_mode_end": "FINISHED"}))
        check(tb.failure_class(pr, "vm2") == "TRACING_UNAVAILABLE", "playback fine, traced runs missing")
        rows = [{"attempt": "a", "label": "run:tas_a", "intent": True, "t": 0}]
        (pr / "ledger.jsonl").write_text("\n".join(json.dumps(r) for r in rows))
        check(abs(tb.ledger_hours_game(pr, complete=False) - tb.DANGLING_SECONDS / 3600) < 1e-9, "a dangling attempt costs its timeout, not the wall clock")
        rows += [{"attempt": f"b{i}", "label": "x", "cpu_seconds": 9000, "wall_seconds": 1, "ok": True} for i in range(4)]
        (pr / "ledger.jsonl").write_text("\n".join(json.dumps(r) for r in rows))
        check(tb.failure_class(pr, "vm2") == "BUDGET_EXHAUSTED", "ceiling exceeded")


def test_budget_guard():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "vm2").mkdir()
        rows = [{"attempt": f"a{i}", "label": "run:x", "intent": True, "t": 0} for i in range(2)]
        (root / "vm2" / "ledger.jsonl").write_text("\n".join(json.dumps(r) for r in rows))
        tb.budget_guard(root, "vm2")                       # per-run guard ignores in-flight siblings
        try:
            tb.budget_guard(root, "vm2", dangling=True)    # 2 x 6 h >= 8 h
            check(False, "launch guard must refuse stale dangling attempts")
        except tb.ge.EvidenceError:
            pass


def test_consistency():
    from segarecomp_genesis_label_policy import E, H
    tiers = bytes([E] * 100 + [H] * 20)
    cls = b"I" + b"c" * 99 + b"D" * 20
    meta = {"audit": {"length_mismatch": 0, "not_ordinary_decode": 0, "odd_starts": 0}}
    c = tb.consistency(meta, tiers, cls, [(1000, 1100)], {5, 6})
    check(c["ok"] and c["e_bytes_also_read_as_data"] == 2, "weak overlap is counted, never gating")
    c = tb.consistency(meta, tiers, cls, [(0, 10)], set())
    check(not c["ok"] and c["e_bytes_in_dma_source"] == 10, "E bytes inside a DMA source exceed 0.5 %")
    check(not tb.consistency({"audit": {}}, tiers, cls, [], set())["ok"], "missing audit keys do not pass")
    check(not tb.consistency(meta, b"", b"", [], set())["ok"], "no E bytes does not pass")
    check(not tb.consistency({"audit": {"length_mismatch": 1, "not_ordinary_decode": 0, "odd_starts": 0}}, tiers, cls, [], set())["ok"], "decode audit")


def test_ml_verdict():
    arms = ("B0", "C", "T", "TW", "T_matched")
    mk = lambda ap: {a: {"ap": ap.get(a, ap["B0"]), "auc": 0.9} for a in arms}
    folds = lambda d: {"L1": {a: {"ap": 0.98 + d.get(a, 0)} for a in arms}, "L2": {a: {"ap": 0.97 + d.get(a, 0)} for a in arms}}
    rate = lambda fn, fp: {a: {"fn_rate_positive_cells": fn.get(a, 0.10), "fp_rate_negative_cells": fp.get(a, 0.001), "fp_rate_hard_negative_cells": 0.001} for a in arms}
    v = ml.ml_verdict(mk({"B0": 0.988, "T": 0.990, "C": 0.989}), folds({}), rate({}, {}))
    check(v["verdict"] == "ML_INCONCLUSIVE" and "saturated" in v["reason"], "tiny deltas on a saturated baseline are inconclusive")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.96, "C": 0.952, "T_matched": 0.958}), folds({"T": 0.01}), rate({}, {}))
    check(v["verdict"] == "ML_GAIN_SUPPORTED", f"gain {v}")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.96, "C": 0.952, "T_matched": 0.951}), folds({"T": 0.01}), rate({}, {}))
    check(v["verdict"] == "ML_INCONCLUSIVE" and "volume" in v["reason"], "gain that vanishes at matched volume is volume-confounded")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.96, "C": 0.9595, "T_matched": 0.96}), folds({"T": 0.01}), rate({}, {}))
    check(v["verdict"] == "ML_INCONCLUSIVE", "gain over B0 but not over the control")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.94, "C": 0.95}), folds({}), rate({}, {}))
    check(v["verdict"] == "ML_HARM_SUPPORTED", "pooled harm")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.949, "C": 0.95}), {"L1": {a: {"ap": 0.9 if a == "T" else 0.98} for a in arms}}, rate({}, {}))
    check(v["verdict"] == "ML_INCONCLUSIVE", "tiny pooled delta, below sensitivity")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.9505, "C": 0.9}), {"L1": {a: {"ap": 0.9 if a == "T" else 0.98} for a in arms}}, rate({}, {}))
    check(v["verdict"] == "ML_INCONCLUSIVE", "a lone noisy lineage dip with flat or positive pooled AP is not harm")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.9495, "C": 0.9}), {"L1": {a: {"ap": 0.9 if a == "T" else 0.98} for a in arms}}, rate({}, {}))
    check(v["verdict"] == "ML_HARM_SUPPORTED", "a lineage regression beyond 0.02 corroborated by a negative pooled delta")
    v = ml.ml_verdict(mk({"B0": 0.95, "T": 0.96, "C": 0.95, "T_matched": 0.96}), folds({"T": 0.01}), rate({"T": 0.2}, {}))
    check(v["verdict"] == "ML_HARM_SUPPORTED", "false-negative regression")
    check(abs(ml.containment({1, 2, 3, 4}, {1, 2}) - 0.5) < 1e-9 and ml.containment(set(), {1}) == 0.0, "containment")


def test_isolation_and_plan():
    reg = [("s2", "x" * 64, "D01")]
    check(tb.isolation_check("vm2", reg)["ok"], "no collision")
    check(not tb.isolation_check("vm2", [("a", tb.GAMES["vm2"]["rom"]["sha256"], "D99")])["ok"], "image collision is detected")
    check(not tb.isolation_check("vm2", [("a", "z", "T81_VM2")])["ok"], "lineage collision is detected")
    s256, s1 = tb.sealed_identities()
    check(len(s256) >= 1 and tb.GAMES["vm2"]["rom"]["sha256"] not in s256 and all(tb.GAMES[g]["rom"]["sha1"] not in s1 for g in tb.GAMES), "none of the games is a sealed entry")
    if tb.PLAN.exists():
        check(bool(tb.verify_plan()), "frozen plan verifies")
        check(tb.PLAN_DIGEST.read_text().strip() == tb.ge.sha256(tb.PLAN.read_bytes()), "digest matches the plan bytes")
    p = tb.PROTOCOL
    check(p["usefulness"]["ambitious_secondary"] == 500 and tb.THRESH["extra_strong_code_cells_abs"] == 100, "thresholds as requested")
    check("never counted towards the strong-code rule" in p["usefulness"]["weak_labels"], "weak labels never satisfy the strong-code rule")
    check(p["limits"]["cpu_hours_per_game"] == 8 and p["limits"]["cpu_hours_total"] == 24, "resource ceilings")


def test_committed_results_are_aggregate_only():
    banned = ('"/root', "/home/", ".md\"", ".bin\"")
    for f in sorted((ROOT / "tools").glob("segarecomp_genesis_tasbatch*.json")):
        txt = f.read_text()
        for b in banned:
            check(b not in txt, f"{f.name}: no path or file name ({b})")


def main():
    test_games_table()
    test_generic_lua()
    test_policy_port_matches_lua()
    test_control_movie()
    test_signatures()
    test_classification()
    test_batch()
    test_gates()
    test_failure_classes()
    test_budget_guard()
    test_consistency()
    test_ml_verdict()
    test_isolation_and_plan()
    test_committed_results_are_aggregate_only()
    print("FAILURES:", len(FAILURES), *FAILURES)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
