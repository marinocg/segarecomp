#!/usr/bin/env python3
"""SEG-081-T001: three-game TAS acquisition and cross-engine corpus validation (OFFLINE research tooling).

A game-independent configuration layer over the merged SEG-080 tooling (`segarecomp_genesis_bizhawk`, `segarecomp_genesis_evidence` and
`segarecomp_genesis_label_policy`, all imported UNCHANGED). Three inputs (a ROM, a `.bk2` movie, the pinned BizHawk 2.9.1) go through the
same pipeline; nothing here knows a game's RAM map:

  identity   pin ROM / movie / emulator identities against the frozen game table (movie-header MD5 == local ROM MD5)
  control    write the equal-duration control as a `.bk2` that reuses the TAS movie's header, core and sync settings and carries the
             SEG-080 seeded input policy (ported verbatim from the SEG-080 Lua and tested against it)
  run        one scenario (playback | tas_a | tas_b | ctl_a | ctl2_a) -> a trace directory (BizHawk M68K bus callbacks + native CDL)
  label      the unchanged SEG-077 `label()` per condition, then the `seg080.label_policy.v1` strong / strong+weak variants
  analyze    replay / tracing / consistency gates, marginal strong-code yield, structural novelty and the per-game classification
  registry-sigs / batch / fixture / freeze / verify-plan

Checkpoint state is generic: work-RAM, VRAM, CRAM and VSRAM digests (no game variable). Private outputs live only under an ignored
directory; durable output is aggregate-only. There is no source truth for these titles: correctness means internal consistency, ROM mapping,
decoder agreement and provenance, never a measured error rate.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import io
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_bizhawk as bz  # noqa: E402
import segarecomp_genesis_evidence as ge  # noqa: E402
import segarecomp_genesis_label_policy as lp  # noqa: E402

SCHEMA = "segarecomp.genesis_tasbatch.v1"
PLAN = HERE / "segarecomp_genesis_tasbatch.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_tasbatch.plan.digest"
GAMES_FILE = HERE / "segarecomp_genesis_tasbatch.games.json"
GAMES = json.loads(GAMES_FILE.read_text())
ORDER = sorted(GAMES, key=lambda g: GAMES[g]["order"])
CELL = ge.CELL_BYTES
EMU = {"EmuHawk.exe": "6ce622d4ed4e8460ce362cf35ef67dc70096fec2c9a174cbef6a3e5b04f18bcc",
       "gpgx.wbx.zst": "067ced324e9bfdbda74196f36e296dc257009d90fce84b691b3daf07ab005502",
       "package_sha256": "5afb39afef8874bc6ef62bdac1e40a2aa3c47d6b74f9acc62a057cc44d355634"}
SCENARIOS = ["playback", "tas_a", "tas_b", "ctl_a", "ctl2_a"]
EXPECTED_LEDGER = ["run:" + s for s in SCENARIOS] + ["label:tas", "label:ctl", "label:ctl2"]
BUCKETS = (16, 64, 256, 1024)
RARE_MAX = 3

THRESH = {"extra_strong_code_cells_abs": 100, "extra_strong_code_cells_rel": 0.20, "extra_strong_code_cells_rel_min_abs": 25,
          "ambitious_extra_cells": 500, "novel_structural_signatures_min": 1, "strong_data_conflict_fraction_max": 0.005,
          "cpu_hours_per_game": 8, "cpu_hours_total": 24}

PROTOCOL = {
    "schema": SCHEMA + ".plan",
    "task": "SEG-081-T001",
    "games": GAMES,
    "order": ORDER,
    "emulator": {"pins": EMU, "note": "BizHawk 2.9.1 linux-x64 under Mono 6.8, Genplus-gx waterbox core, Xvfb, unprivileged user (SEG-080 setup). The "
                 "Earthworm Jim 2 movie header names 2.6.3: replay on 2.9.1 is attempted first; one bounded compatibility investigation is allowed; "
                 "2.6.3 is used only if existing tooling supports it without new instrumentation. No movie input or emulator setting is altered."},
    "identity": {"procedure": "ROM size + SHA-256 equal the frozen table; the movie-header hash field (an MD5 in BizHawk) equals the local ROM MD5; movie "
                 "SHA-256, input-log SHA-256, frame count, core, emuVersion and the absence of a savestate equal the frozen table; the ROM header serial/region "
                 "is recorded privately; any ambiguity (several candidate files, a revision mismatch) rejects the game. ROM SHA-1 values quoted in the "
                 "request are not used as identity.",
                 "rom_source": "operator-supplied ROMs under games/exp-3-games only; nothing is downloaded or substituted"},
    "isolation": {"checked": "SEG-072 corpus manifest (development + sealed), SEG-075/077 candidate lists and their lineage ids; none of the three titles is a "
                  "sealed or reserved-blind member (machine-checked by image SHA-256/SHA-1 against the manifest's sealed entries; the SEG-077 candidate list is "
                  "title-level and was checked by hand: Vectorman 2 and Earthworm Jim 2 are sequels of listed development candidates Vectorman and Earthworm Jim "
                  "and share an engine family with them; they are NOT identical images and are disclosed as engine-adjacent). Image SHA-256 and lineage "
                  "ids are re-checked against the registry by `analyze` and `diagnose`.",
                  "rule": "a title whose image SHA-256 or lineage id appears in the sealed/blind set is excluded and reported"},
    "replay": {"window": "the complete movie from power-on (frames = movie input rows)",
               "checkpoints": "five checkpoints at floor(k*N/6) for k=1..5 plus the final completion state at frame N; each records "
                              "work-RAM, VRAM, CRAM and VSRAM digests and the cumulative executed-PC / read / edge counts",
               "playback": "an untraced playback of the movie gives the reference end state (movie FINISHED at frame N) and the unperturbed checkpoint digests",
               "gate_a": {"identity_ok": "all identity checks pass",
                          "played_to_end": "playback and traced runs both emulate exactly N frames and the playback run reports the movie FINISHED",
                          "progression": ">= 5 of the 6 checkpoint states have pairwise distinct (RAM, VRAM) digests AND at least 0.5 %% of the run's distinct ROM PCs are first "
                                         "executed in each of >= 3 of the 5 equal movie-time fifths",
                          "no_perturbation": "playback and traced checkpoint digests (RAM, VRAM, CRAM, VSRAM) are identical at every checkpoint; a digest that is "
                                              "\"na\" (unreadable domain) or a missing domain invalidates the gate",
                          "determinism": "two traced TAS runs have identical PC-set, read-set, edge-set, CDL digests and checkpoint digests",
                          "failure": "REPLAY_INVALID"},
               "gate_tracing": {"map_mismatch": 0, "odd_pcs": 0, "accepted_E_fraction_min": 0.99, "reset_and_vblank_pc_executed": True,
                                "read_self_fetch_fraction_max": 0.10, "cdl_exec_mismatch_fraction_max": 0.001, "cdl_fixture_ok": True,
                                "controls": "frames == N, map mismatch 0, odd PCs 0, reset and vblank PCs executed, self-fetch and CDL bounds",
                                "failure": "TRACING_UNAVAILABLE"}},
    "control": {"policy": "the SEG-080 seeded-input policy (SEG-077 `random`), ported verbatim: input row f (0-based) = policy(f), row 0 empty; seed 1 primary, "
                          "seed 2 sensitivity; start window 40 s",
                "delivery": "amendment of the SEG-080 delivery (frozen before any run): the control is a synthetic `.bk2` that reuses the TAS movie's header, "
                            "core, controller and sync settings and replaces only the input log, so ROM, core configuration, frame count, tracing and labelling "
                            "are identical by construction (SEG-080 ran the control with BizHawk default sync settings). The port is tested against the Lua policy.",
                "frames": "N (the TAS movie frame count)", "label_inputs": "tas = tas_a only; ctl = ctl_a (seed 1); ctl2 = ctl2_a (seed 2); tas_b serves determinism"},
    "labels": {"policy": lp.POLICY_ID, "tiers": "E executed I/c strong code; R static reach weak code; H hardware header strong data; M DMA-source strong data; "
               "W ordinary ROM read weak nonexclusive data; U unknown/masked", "variants": ["strong = E+H+M", "weak = strong+R+W with cell weights"],
               "weak_weight": lp.WEAK_WEIGHT, "cell_rule": "unchanged SEG-072: positive when any I, negative when all D, else masked",
               "no_truth": "no source truth exists; source truth and ML predictions never repair labels",
               "overlap": "a byte that is both executed and observed as a read / DMA source stays code (E); the overlap is counted and disclosed, never "
                          "silently treated as exclusive"},
    "consistency": {"audit": "ordinary-decode audit of every labelled start has zero length mismatches, non-ordinary decodes and odd starts",
                    "strong_data_conflict": "bytes of E code that lie inside an observed DMA-source extent / total E bytes <= %s" % THRESH["strong_data_conflict_fraction_max"],
                    "weak_conflicts": "E bytes that were also ordinary data reads are counted and disclosed; they never gate",
                    "decoders": "E starts require agreement of two independent decoders (m68k-disasm walk and the CPU token report), already enforced by `label()`; "
                                "the accepted-E fraction and rejection classes are reported",
                    "failure": "LABEL_POLICY_CONFLICT"},
    "metrics": {"coverage": ["executed ROM PCs", "TAS-only PCs", "E instruction starts and continuation bytes", "strong code-positive cells (TAS, control, extra)",
                             "extra indirect targets and edges", "cumulative executed-PC growth at 25/50/75/100 % of the emulated time",
                             "CPU seconds per extra strong code cell"],
                "data": ["H / M / W bytes", "strong and weak negative cells", "mixed cells", "hard-negative cells", "overlap counts", "masked proportion",
                         "strong-versus-weak differences"],
                "novelty": {"signatures": ["forms = (family, ea_src, ea_dst, width, control)", "3-grams and 4-grams of consecutive-start forms",
                                           "cfg = (flow kind, target distance class or indirect, source form, target form)",
                                           "boundary = (form before data, log2 data-run bucket, form after data), taken from the weak variant because strong "
                                           "labels never abut code with data (descriptive only)", "rare forms (registry count <= %d)" % RARE_MAX],
                            "references": ["SEG-072 qualified development registry (target excluded by construction)", "SEG-080 Sonic 2 TAS strong labels",
                                           "earlier games of this batch (TAS strong labels)"],
                            "primary": "N_struct_not_in_control = |(forms U cfg) of the TAS strong labels  minus  the registry  minus  the seed-1 control strong labels| "
                                       "(cfg tuples need a LABELLED target form, so partial coverage cannot create signatures; n-grams and boundary patterns are descriptive; "
                                       "N_struct against the registry alone is reported beside it)"}},
    "usefulness": {"extra_strong_code_cells": "TAS strong code-positive cells minus the seed-1 control's (set difference, no truth filter)",
                   "rule": "extra >= %d  OR  (extra >= %d AND extra >= %.2f * control cells)" % (THRESH["extra_strong_code_cells_abs"],
                           THRESH["extra_strong_code_cells_rel_min_abs"], THRESH["extra_strong_code_cells_rel"]),
                   "ambitious_secondary": THRESH["ambitious_extra_cells"], "novelty": "N_struct_not_in_control >= %d" % THRESH["novel_structural_signatures_min"],
                   "weak_labels": "never counted towards the strong-code rule; data-only and mixed-cell yields are reported separately",
                   "sensitivity": "the seed-2 control is reported (extra cells, usefulness) but never changes the primary class"},
    "classes": ["QUALIFIED_USEFUL", "QUALIFIED_LOW_YIELD", "REPLAY_INVALID", "TRACING_UNAVAILABLE", "LABEL_POLICY_CONFLICT", "INPUT_REQUIRED", "BUDGET_EXHAUSTED"],
    "precedence": "INPUT_REQUIRED (input missing OR identity mismatch: the game is rejected, not replayed), BUDGET_EXHAUSTED, REPLAY_INVALID, TRACING_UNAVAILABLE, LABEL_POLICY_CONFLICT, then QUALIFIED_*",
    "limits": {"cpu_hours_per_game": THRESH["cpu_hours_per_game"], "cpu_hours_total": THRESH["cpu_hours_total"],
               "compat_attempts_per_game": 1, "traced_tas_replays": 2, "controls": "seed 1 primary + seed 2 sensitivity", "ledger": "append-only <private>/<game>/ledger.jsonl"},
    "batch_decision": {"CROSS_ENGINE_ACQUISITION_SUPPORTED": ">= 2 games QUALIFIED_USEFUL (each with structural novelty) and no unresolved major evidence "
                                                              "inconsistency (no LABEL_POLICY_CONFLICT game, no open review blocker)",
                       "CROSS_ENGINE_ACQUISITION_MIXED": "exactly one QUALIFIED_USEFUL game, or >= 2 useful but an unresolved major inconsistency, or exactly one "
                                                         "qualified game; the limiting factor is named",
                       "CROSS_ENGINE_ACQUISITION_NOT_SUPPORTED": ">= 2 qualified games and none QUALIFIED_USEFUL",
                       "EVIDENCE_INSUFFICIENT": "no game is qualified (inputs missing, replay invalid, untraceable, over budget)",
                       "separate": "the model-A verdict never changes the batch decision",
                       "open_review_blockers": "the number of unresolved BLOCKER findings in the final independent review record; it is printed with the decision"},
    "model_a": {"runs_when": ">= 2 games are qualified", "model": "SEG-072 model A / M1 HistGradientBoosting (frozen HGB_CONFIG), masked cells excluded",
                "folds": "leave-one-lineage-out over every qualified SEG-072 lineage (D01 included); train = other lineages (truth) + the arm's new-game labels",
                "arms": {"B0": "base only", "C": "base + seed-1 control strong labels of the qualified games", "T": "base + TAS strong labels",
                         "TW": "base + TAS strong+weak labels (cell weights)", "T_matched": "sensitivity: TAS strong labels randomly down-sampled (seed 81) to the "
                                                                                      "control's positive and negative cell counts"},
                "isolation": "new-game lineage ids and image SHA-256 must be absent from the registry; 12-byte-shingle containment of labelled code runs "
                             "between each new game and each held-out lineage must be <= 0.30 in both directions, else that game is withheld from that fold",
                "operating_point": "threshold = the 10th percentile of B0 scores over the fold's positive cells (B0 recall 0.90); every arm scored at it",
                "metrics": "per-lineage and pooled AP (primary) and AUC; positive-cell false-negative rate; negative and hard-negative false-positive rates; "
                           "worst-lineage delta; error concentration; sample-volume sensitivity (T_matched)",
                "verdict": {"sensitivity_precondition": "max(|AP(T)-AP(B0)|, |AP(T)-AP(C)|) >= 0.005, else ML_INCONCLUSIVE (cannot distinguish inputs)",
                            "ML_GAIN_SUPPORTED": "AP(T)-AP(B0) >= 0.005 and AP(T)-AP(C) >= 0.003 and worst-lineage AP(T)-AP(B0) >= -0.02 and macro FN(T) <= FN(B0)+0.01 "
                                                 "and macro FP(T) <= FP(B0)+0.002 and AP(T_matched)-AP(B0) >= half of AP(T)-AP(B0) (otherwise volume-confounded -> ML_INCONCLUSIVE)",
                            "ML_HARM_SUPPORTED": "AP(T)-AP(B0) <= -0.005 or (worst-lineage AP(T)-AP(B0) < -0.02 and pooled AP(T) < AP(B0)) or macro FN(T) > FN(B0)+0.02 or macro FP(T) > FP(B0)+0.005",
                            "ML_INCONCLUSIVE": "otherwise", "reported_not_decisive": "TW, C and T_matched deltas"},
                "budget": "one fit per arm per fold; no search, no models B/C, no SEG-077 retraining"},
    "disclosures": ["the control moves player 1 only; the two-player TAS movies (Vectorman 2, Earthworm Jim 2) may use player 2",
                    "control policy equivalence is to Lua 5.4 integer semantics (tested); SEG-080 fed the same Lua text to BizHawk's interpreter",
                    "a title-screen-only control can fail the vblank/reset-PC fidelity check and then yields TRACING_UNAVAILABLE for the game",
                    "the native CDL data flag is blanket (for example a boot checksum sweep reads nearly every ROM byte): E bytes that were also read as data "
                    "are counted and disclosed, never gating", "model-A error rates are macro-means over folds (`macro_error_rates`)"],
    "privacy": "ROMs, movies, traces, labels, signatures, screenshots, per-address lists and weights stay in the ignored private directory; committed output is "
               "aggregate counts, identities (hashes), digests, hours, and the decision",
    "review": "independent plan review before the first trace; reproduction and delta review before acceptance (synthetic CDL fixture, per-game fingerprints, "
              "marginal counts, identities, tier provenance, novelty, overlap and lineage isolation, ceilings, privacy, CI)",
    "amendments": "a correction after the first result keeps this plan, records an amendment with what had been observed, and keeps comparisons interpretable",
}
PLAN_EXTRA = ("bound_code",)
BOUND = ["tools/segarecomp_genesis_tasbatch.py", "tools/segarecomp_genesis_tasbatch_ml.py", "tools/segarecomp_genesis_tasbatch.games.json",
         "tools/segarecomp_genesis_bizhawk.py", "tools/segarecomp_genesis_evidence.py", "tools/segarecomp_genesis_label_policy.py",
         "tools/segarecomp_ml_v3_train.py", "tools/segarecomp_ml_region_gen2_train.py", "tools/segarecomp_ml_v3.corpus_manifest.json",
         "tools/segarecomp_ml_v3.plan.json", "tests/segarecomp_genesis_tasbatch_test.py"]
DANGLING_SECONDS = 21600          # an attempt with an intent line and no completion line is costed at the per-run timeout, never at the wall clock


# ------------------------------------------------------------------------------------------------------------------------- Lua
_OLD_SNAP = re.compile(r"local function snap\(tag\).*?ckf:flush\(\)\nend\n", re.S)
GENERIC_SNAP = r'''local function snap(tag)
  local function hr(a, n, d) local ok, v = pcall(function() return memory.hash_region(a, n, d) end) return ok and v or "na" end
  local ram = hr(0xFF0000, 0x10000, "M68K BUS")
  local vram = hr(0, 0x10000, "VRAM")
  local cram = hr(0, 0x80, "CRAM")
  local vsram = hr(0, 0x50, "VSRAM")
  local shot = ""
  if isck[frame_now] then
    local path = S.out .. "/shot_" .. frame_now .. ".png"
    local ok = pcall(function() client.screenshot(path) end)
    shot = ok and path or ""
  end
  ckf:write(string.format("%s\t%d\tram=%s vram=%s cram=%s vsram=%s uniq=%d reads=%d edges=%d shot=%s\n",
    tag, emu.framecount(), ram, vram, cram, vsram, nuniq, nread, nedge, shot))
  ckf:flush()
end
'''
GENERIC_LUA = _OLD_SNAP.sub(lambda m: GENERIC_SNAP, bz.LUA)
assert GENERIC_LUA != bz.LUA and "zone=" not in GENERIC_LUA and "0xFFFE10" not in GENERIC_LUA
bz.LUA = GENERIC_LUA           # the SEG-080 module renders this template (a runtime substitution; the bound file is untouched)


# ------------------------------------------------------------------------------------------------------------------- scenarios
def checkpoints(frames: int) -> list:
    return [frames * k // 6 for k in range(1, 6)]


def scenario(game: str, sid: str) -> dict:
    g = GAMES[game]
    n, size = g["movie"]["frames"], g["rom"]["size"]
    base = {"id": sid, "frames": n, "size": size, "checkpoints": checkpoints(n)}
    if sid == "playback":
        return dict(base, mode="playback")
    if sid in ("tas_a", "tas_b", "ctl_a", "ctl2_a"):
        return dict(base, mode="movie")                   # the control is also a movie (see `control_movie`)
    raise ge.EvidenceError(f"unknown scenario {sid}")


def policy_rows(frames: int, seed: int, start_window: int = 40) -> list:
    """The SEG-080 seeded policy (Lua, verbatim). Returns, per input row f, the set of pressed button names."""
    state = [seed * 2654435761 % 2147483648 + 1]

    def rnd(k):
        state[0] = (state[0] * 1103515245 + 12345) % 2147483648
        return state[0] % k
    act = [None]

    def policy(frame):
        if frame >= (frames // 60) * 60:
            return set()
        p = set()
        if frame < start_window * 60:
            if frame % 120 < 6:
                p.add("Start")
            if 60 <= frame % 120 < 66:
                p.add("A")
            return p
        f = frame % 900
        if f < 4 or 24 <= f < 28:
            return {"Start"}
        if frame % 8 == 0 or act[0] is None:
            d = rnd(100)
            a = set()
            if d < 50:
                a.add("Right")
            elif d < 65:
                a.add("Left")
            elif d < 75:
                a.add("Up")
            elif d < 85:
                a.add("Down")
            if rnd(100) < 30:
                a.add("A")
            if rnd(100) < 30:
                a.add("B")
            if rnd(100) < 30:
                a.add("C")
            act[0] = a
        return act[0]
    return [set() if f == 0 else set(policy(f)) for f in range(frames)]


MNEMONIC = {"Up": "U", "Down": "D", "Left": "L", "Right": "R", "A": "A", "B": "B", "C": "C", "Start": "S", "X": "X", "Y": "Y", "Z": "Z", "Mode": "M"}


def control_movie(tas_bk2: bytes, frames: int, seed: int) -> bytes:
    """A .bk2 identical to the TAS movie except for its input log (and a blanked subtitle track)."""
    zin = zipfile.ZipFile(io.BytesIO(tas_bk2))
    log = zin.read("Input Log.txt").decode().splitlines()
    key = next(ln for ln in log if ln.startswith("LogKey:"))
    groups = [g for g in key[len("LogKey:"):].split("#") if g]
    widths = [len([x for x in g.split("|") if x]) for g in groups]            # Power|Reset, then one group per player
    names = [[x for x in g.split("|") if x] for g in groups]
    rows = policy_rows(frames, seed)
    out = ["[Input]", key]
    for pressed in rows:
        parts = ["|" + "." * widths[0] + "|"]
        for gi in range(1, len(groups)):
            cells = []
            for nm in names[gi]:
                b = nm.split(" ", 1)[1]
                cells.append(MNEMONIC[b] if gi == 1 and b in pressed else ".")
            parts.append("".join(cells) + "|")
        out.append("".join(parts))
    out.append("[/Input]")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.filename == "Input Log.txt":
                data = ("\n".join(out) + "\n").encode()
            elif info.filename == "Subtitles.txt":
                data = b""
            zout.writestr(info.filename, data)
    return buf.getvalue()


# ----------------------------------------------------------------------------------------------------------------------- identity
def identity_check(game: str, rom: bytes, movie_path: pathlib.Path, bizhawk: pathlib.Path | None = None, package: pathlib.Path | None = None) -> dict:
    g = GAMES[game]
    mi = bz.inspect_movie(movie_path)
    rec = {"game": game, "rom": {"size": len(rom), "sha1": hashlib.sha1(rom).hexdigest(), "sha256": hashlib.sha256(rom).hexdigest(),
                                 "md5": hashlib.md5(rom).hexdigest()}, "movie": mi}
    m = g["movie"]
    rec["checks"] = {
        "rom_size_sha256": (rec["rom"]["size"], rec["rom"]["sha256"]) == (g["rom"]["size"], g["rom"]["sha256"]),
        "rom_sha1_md5": (rec["rom"]["sha1"], rec["rom"]["md5"]) == (g["rom"]["sha1"], g["rom"]["md5"]),
        "movie_header_md5_equals_rom_md5": (mi["header"]["SHA1"] or "").lower() == rec["rom"]["md5"] == m["header_md5"],
        "movie_sha256": mi["sha256"] == m["sha256"], "input_log_sha256": mi["input_log_sha256"] == m["input_log_sha256"],
        "frames": mi["frames"] == m["frames"], "core": mi["header"]["Core"] == m["Core"],
        "emu_version_header": mi["header"]["emuVersion"] == m["emuVersion"], "no_savestate": not mi["starts_from_savestate"],
        "players": mi["players_in_log"] == m["players_in_log"], "six_button": mi["sync"]["UseSixButton"] == m["UseSixButton"],
        "plain_image": ge.identity(rom)["format"] == "plain"}
    if bizhawk is not None:
        emu = {"EmuHawk.exe": bz.sha256_file(bizhawk / "EmuHawk.exe"), "gpgx.wbx.zst": bz.sha256_file(next(bizhawk.rglob("gpgx.wbx.zst")))}
        if package is not None:
            emu["package_sha256"] = bz.sha256_file(package)
        rec["emulator"] = emu
        rec["checks"]["emulator_pins"] = all(EMU[k] == v for k, v in emu.items())
    rec["ok"] = all(rec["checks"].values())
    return rec


def cmd_identity(a) -> int:
    verify_plan()
    rom = pathlib.Path(a.rom).read_bytes()
    rec = identity_check(a.game, rom, pathlib.Path(a.movie), pathlib.Path(a.bizhawk), pathlib.Path(a.package) if a.package else None)
    pathlib.Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(a.out).write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({"game": a.game, "ok": rec["ok"], "checks": rec["checks"]}, sort_keys=True))
    return 0 if rec["ok"] else 2


# ------------------------------------------------------------------------------------------------------------------------- run
def cmd_run(a) -> int:
    verify_plan()
    private = pathlib.Path(a.private) / a.game
    ge.require_private(private.parent)
    budget_guard(pathlib.Path(a.private), a.game)
    sc = scenario(a.game, a.scenario_id)
    jobs = pathlib.Path(a.jobs)
    movie = pathlib.Path(a.movie)
    if a.scenario_id.startswith("ctl"):
        seed = 1 if a.scenario_id == "ctl_a" else 2
        ctl = jobs / f"{a.game}_{a.scenario_id}.bk2"
        ctl.parent.mkdir(parents=True, exist_ok=True)
        ctl.write_bytes(control_movie(movie.read_bytes(), sc["frames"], seed))
        movie = ctl
    ns = argparse.Namespace(rom=a.rom, movie=str(movie), bizhawk=a.bizhawk, scenario=json.dumps(sc), out=str(private / "runs" / a.scenario_id),
                            jobs=a.jobs, ledger=str(private / "ledger.jsonl"), user=a.user, display=a.display, timeout=a.timeout)
    private.mkdir(parents=True, exist_ok=True)
    return bz.measured(ns.ledger, "run:" + a.scenario_id, bz.run_scenario, ns)


# ------------------------------------------------------------------------------------------------------------------------ label
def apply_policy(label_dir: pathlib.Path, out: pathlib.Path, name: str, lineage: str) -> dict:
    """`seg080.label_policy.v1` strong / weak variants via the unchanged SEG-080 functions (no plan check: this plan binds them)."""
    ge.require_private(out.parent if not out.exists() else out)
    tiers, cls = lp.tiers_of(label_dir)
    meta = json.loads((label_dir / "meta.json").read_text())
    summary = {}
    for which in ("strong", "weak"):
        d = out / which
        d.mkdir(parents=True, exist_ok=False)
        v = lp.build_variant(tiers, cls, which)
        labels = lp.cell_labels(v)
        (d / "classes.bin").write_bytes(v)
        (d / "tokens.bin").write_bytes((label_dir / "tokens.bin").read_bytes())
        (d / "seeds.json").write_text((label_dir / "seeds.json").read_text())
        (d / "weights.json").write_text(json.dumps(lp.cell_weights(tiers, v, labels)))
        m = dict(meta, id=f"{name}-{which}", lineage=lineage, policy=lp.POLICY_ID, variant=which)
        m["bytes"] = {c: v.count(c.encode()) for c in "IcDU"}
        (d / "meta.json").write_text(json.dumps(m, indent=1, sort_keys=True))
        summary[which] = {"cells_pos": labels.count(1), "cells_neg": labels.count(0), "cells_masked": labels.count(-1), "bytes": m["bytes"]}
    return summary


def cmd_label(a) -> int:
    plan = verify_plan()
    if ge.sha256(pathlib.Path(a.cli).read_bytes()) != a.cli_sha256:
        raise ge.EvidenceError("decoder CLI digest differs from --cli-sha256")
    private = pathlib.Path(a.private) / a.game
    budget_guard(pathlib.Path(a.private), a.game)
    (private / "cli.json").write_text(json.dumps({"cli_sha256": a.cli_sha256}))
    g = GAMES[a.game]
    ge.PLAN = pathlib.Path("/nonexistent/seg081-no-t001-plan")
    runs = {"tas": "tas_a", "ctl": "ctl_a", "ctl2": "ctl2_a"}
    for cond, sid in runs.items():
        out = private / "labels" / cond

        def work(cond=cond, sid=sid, out=out):
            meta = ge.label(pathlib.Path(a.rom), [private / "runs" / sid], None, a.cli, out, g["lineage"], f"{a.game}-{cond}")
            apply_policy(out, private / "policy" / cond, f"{a.game}-{cond}", g["lineage"])
            return meta
        meta = bz.measured(str(private / "ledger.jsonl"), "label:" + cond, work)
        print(json.dumps(ge.sanitized(meta), sort_keys=True))
    return 0


# ----------------------------------------------------------------------------------------------------------------------- signatures
def form_at(tokens: bytes, pc: int):
    w = pc // 2
    t = tokens[w * ge.TOKEN_FIELDS:(w + 1) * ge.TOKEN_FIELDS]
    return (t[2], t[4], t[5], t[6], t[3]) if len(t) == ge.TOKEN_FIELDS else None


def log_bucket(n: int) -> int:
    return 0 if n <= 0 else int(math.log2(n))


def dist_class(d: int) -> str:
    sgn = "f" if d >= 0 else "b"
    for i, b in enumerate(BUCKETS):
        if abs(d) <= b:
            return f"{sgn}{i}"
    return f"{sgn}{len(BUCKETS)}"


def instruction_starts(cls: bytes) -> list:
    """(pc, length) of every labelled instruction (an I followed by its c continuation bytes)."""
    out, i, n = [], 0, len(cls)
    while i < n:
        if cls[i] == 73:
            j = i + 1
            while j < n and cls[j] == 99:
                j += 1
            out.append((i, j - i))
            i = j
        else:
            i += 1
    return out


def is_indirect(img: bytes, pc: int) -> bool:
    w = ge.u16(img, pc)
    return w is not None and w & 0xFF80 == 0x4E80 and (w & 0x3F) not in (0x38, 0x39, 0x3A)


def struct_sigs(img: bytes, cls: bytes, tokens: bytes) -> dict:
    """Structural signature sets (strings) plus the form frequency counter. Pure function of image, classes and the CPU token report."""
    starts = instruction_starts(cls)
    forms_at = {pc: form_at(tokens, pc) for pc, _n in starts}
    forms, g3, g4, cfg, bnd = set(), set(), set(), set(), set()
    count = collections.Counter()
    run, last_end = [], -1
    for pc, n in starts:
        f = forms_at[pc]
        forms.add(f)
        count[f] += 1
        run = (run + [f]) if pc == last_end else [f]
        if len(run) >= 3:
            g3.add(tuple(run[-3:]))
        if len(run) >= 4:
            g4.add(tuple(run[-4:]))
        last_end = pc + n
        tg, kind = ge.flow(img, pc, n)
        if is_indirect(img, pc):
            cfg.add(("ind", kind, f, None))
        elif tg:
            t = tg[0]
            tf = forms_at.get(t)
            if tf is not None:                      # an unlabelled target (partial coverage) is not a structural signature
                cfg.add((kind, dist_class(t - pc), f, tf))
        elif kind != "normal":
            cfg.add((kind, "none", f, None))
    size = len(cls)
    i = 0
    after = {pc: pc for pc, _n in starts}
    ends = {pc + n: pc for pc, n in starts}
    while i < size:
        if cls[i] == 68:
            j = i
            while j < size and cls[j] == 68:
                j += 1
            before = forms_at.get(ends.get(i)) if i in ends else None
            nxt = forms_at.get(j) if j in after else None
            if before is not None or nxt is not None:
                bnd.add((before, log_bucket(j - i), nxt))
            i = j
        else:
            i += 1
    enc = lambda s: sorted(repr(x) for x in s)
    return {"forms": enc(forms), "g3": enc(g3), "g4": enc(g4), "cfg": enc(cfg), "boundary": enc(bnd),
            "form_counts": {repr(k): v for k, v in count.items()}}


STRUCT_KINDS = ("forms", "cfg")
ALL_KINDS = ("forms", "g3", "g4", "cfg", "boundary")


def merge_sigs(items) -> dict:
    out = {k: set() for k in ALL_KINDS}
    cnt = collections.Counter()
    for s in items:
        for k in ALL_KINDS:
            out[k].update(s[k])
        cnt.update(s["form_counts"])
    out["form_counts"] = dict(cnt)
    return out


def novelty(target: dict, refs: dict, control: dict | None = None) -> dict:
    """Signatures of `target` absent from each reference set (and, for the TAS-attributable count, also from the control)."""
    res = {}
    for name, ref in refs.items():
        per = {}
        for k in ALL_KINDS:
            t = set(target[k])
            per[k] = {"target": len(t), "absent_from_reference": len(t - set(ref[k])), "reference": len(ref[k])}
            if control is not None:
                per[k]["absent_from_reference_and_control"] = len(t - set(ref[k]) - set(control[k]))
        struct = set()
        struct_ctl = set()
        for k in STRUCT_KINDS:
            struct |= {(k, x) for x in set(target[k]) - set(ref[k])}
            if control is not None:
                struct_ctl |= {(k, x) for x in set(target[k]) - set(ref[k]) - set(control[k])}
        rc = ref["form_counts"]
        rare = [f for f in target["forms"] if 0 < rc.get(f, 0) <= RARE_MAX]
        absent_forms = [f for f in target["forms"] if rc.get(f, 0) == 0]
        rel = {k: round(per[k]["absent_from_reference"] / per[k]["reference"], 4) if per[k]["reference"] else None for k in ALL_KINDS}
        res[name] = {"per_kind": per, "N_struct": len(struct), "N_struct_not_in_control": len(struct_ctl) if control is not None else None,
                     "rare_forms_in_target": len(rare), "forms_absent_from_reference": len(absent_forms), "relative_increase": rel}
    return res


# ------------------------------------------------------------------------------------------------------------------ gates / classes
def parse_state(s: str) -> dict:
    return dict(kv.split("=", 1) for kv in s.split() if "=" in kv)


def digests(ck: list) -> list:
    return [(x["frame"], {k: v for k, v in parse_state(x["state"].split(" shot=")[0]).items() if k in ("ram", "vram", "cram", "vsram")}) for x in ck
            if x["tag"] in ("ck", "final")]


FIFTH_MIN_FRACTION = 0.005


def fifths_with_new_code(trace: dict, frames: int) -> int:
    """Fifths of movie time in which at least 0.5 % of the run's executed ROM PCs were first executed."""
    per = [0] * 5
    for k, v in trace["new_pcs_per_growth_window"].items():
        per[min(4, (int(k) * bz.GROWTH_EVERY) * 5 // max(1, frames))] += v
    total = sum(per)
    return sum(1 for x in per if total and x >= FIFTH_MIN_FRACTION * total)


def replay_gate(rec_identity: dict, n: int, playback: dict, tas_a: dict, tas_b: dict, ck_play: list, ck_a: list, ck_b: list) -> dict:
    d_a, d_b, d_p = digests(ck_a), digests(ck_b), digests(ck_play)
    ck_only = [x for x in d_a]
    states = {json.dumps(x[1], sort_keys=True) for x in ck_only if "ram" in x[1]}
    distinct = len({(x[1].get("ram"), x[1].get("vram")) for x in ck_only})
    played = playback["frames"] == n and (playback["movie_finished_at"] > 0 or playback["movie_mode_end"] == "FINISHED") and tas_a["frames"] == n
    fifths = fifths_with_new_code(tas_a, n)
    n_ck = len(ck_only)
    progression = n_ck >= 6 and distinct >= 5 and fifths >= 3
    no_perturb = bool(d_p) and d_p == d_a
    det = {"cdl": tas_a["cdl_sha256"] == tas_b["cdl_sha256"], "pcs": tas_a["pcs_sha256"] == tas_b["pcs_sha256"], "reads": tas_a["reads_sha256"] == tas_b["reads_sha256"],
           "edges": tas_a["edges_sha256"] == tas_b["edges_sha256"], "checkpoints": d_a == d_b and tas_b["frames"] == n}
    na = any(v == "na" for _f, d in d_a + d_b + d_p for v in d.values()) or any(set(d) != {"ram", "vram", "cram", "vsram"} for _f, d in d_a)
    ok = bool(rec_identity["ok"] and played and progression and no_perturb and all(det.values()) and not na)
    return {"identity_ok": rec_identity["ok"], "played_to_end": played, "checkpoints": n_ck, "distinct_ram_vram_states": distinct, "fifths_with_new_code": fifths,
            "progression": progression, "no_perturbation": no_perturb, "digest_domains_missing": na, "determinism": det, "states": len(states), "replay_ok": ok}


def tracing_gate(tas_a: dict, labelled: dict, ctls: list, n: int, fixture_ok: bool) -> dict:
    meta = labelled["meta"]
    executed = meta["decoders"]["executed_rom_pcs"]
    accepted = meta["tiers"]["E"] / executed if executed else 0.0
    cdl_frac = tas_a["cdl"]["cdl_exec_mismatch_bytes"] / max(1, tas_a["cdl"]["cdl_exec_bytes"])
    rej = {k: v for k, v in meta["typed"].items() if k.startswith("E_")}
    fid = {"map_mismatch": tas_a["map_mismatch"], "odd_pcs": tas_a["odd_pcs"], "accepted_E_fraction": round(accepted, 5), "cdl_exec_mismatch_fraction": round(cdl_frac, 6),
           "reset_pc_executed": tas_a["reset_pc_executed"], "vbl_pc_executed": tas_a["vbl_pc_executed"], "read_self_fetch_fraction": tas_a["read_self_fetch_fraction"],
           "e_rejections": sum(rej.values()), "e_rejection_classes": rej, "cdl_fixture_ok": bool(fixture_ok)}
    ok = (tas_a["map_mismatch"] == 0 and tas_a["odd_pcs"] == 0 and accepted >= 0.99 and tas_a["reset_pc_executed"] and tas_a["vbl_pc_executed"]
          and tas_a["read_self_fetch_fraction"] <= 0.10 and cdl_frac <= 0.001 and bool(fixture_ok))
    cok = []
    for c in ctls:
        cok.append(bool(c["frames"] == n and c["map_mismatch"] == 0 and c["odd_pcs"] == 0 and c["reset_pc_executed"] and c["vbl_pc_executed"]
                        and c["read_self_fetch_fraction"] <= 0.10 and c["cdl"]["cdl_exec_mismatch_bytes"] / max(1, c["cdl"]["cdl_exec_bytes"]) <= 0.001))
    fid["controls_ok"] = cok
    return {"fidelity": fid, "tracing_ok": bool(ok and all(cok))}


def consistency(meta: dict, tiers: bytes, cls: bytes, dma_ext: list, read_bytes: set) -> dict:
    a = meta["audit"]
    audit_ok = all(k in a and a[k] == 0 for k in ("length_mismatch", "not_ordinary_decode", "odd_starts"))
    e_bytes = [i for i, t in enumerate(tiers) if t == lp.E]
    in_dma = bytearray(len(cls))
    for b, e in dma_ext:
        for k in range(b, min(e, len(cls))):
            in_dma[k] = 1
    e_dma = sum(1 for i in e_bytes if in_dma[i])
    e_read = sum(1 for i in e_bytes if i in read_bytes)
    frac = e_dma / len(e_bytes) if e_bytes else 0.0
    return {"audit": a, "audit_ok": audit_ok, "e_bytes": len(e_bytes), "e_bytes_in_dma_source": e_dma, "e_bytes_also_read_as_data": e_read,
            "strong_data_conflict_fraction": round(frac, 6), "ok": bool(audit_ok and e_bytes and frac <= THRESH["strong_data_conflict_fraction_max"])}


def useful(extra: int, control_cells: int) -> bool:
    return extra >= THRESH["extra_strong_code_cells_abs"] or (extra >= THRESH["extra_strong_code_cells_rel_min_abs"]
                                                              and extra >= THRESH["extra_strong_code_cells_rel"] * control_cells)


def classify(rec: dict) -> dict:
    """Frozen per-game classification (pure). `rec` carries input_present, cpu_hours, replay_ok, tracing_ok, consistency_ok, extra_cells,
    control_cells, n_struct."""
    if not rec.get("input_present", False) or not rec.get("identity_ok", True):
        return {"class": "INPUT_REQUIRED"}
    if rec.get("cpu_hours", 0) > THRESH["cpu_hours_per_game"]:
        return {"class": "BUDGET_EXHAUSTED"}
    if not rec.get("replay_ok"):
        return {"class": "REPLAY_INVALID"}
    if not rec.get("tracing_ok"):
        return {"class": "TRACING_UNAVAILABLE"}
    if not rec.get("consistency_ok"):
        return {"class": "LABEL_POLICY_CONFLICT"}
    u = useful(rec["extra_cells"], rec["control_cells"])
    nov = rec["n_struct"] >= THRESH["novel_structural_signatures_min"]
    return {"class": "QUALIFIED_USEFUL" if u and nov else "QUALIFIED_LOW_YIELD", "code_yield_rule": u, "novelty_rule": nov,
            "ambitious_secondary": rec["extra_cells"] >= THRESH["ambitious_extra_cells"]}


def batch_decision(classes: dict, open_review_blockers: int = 0) -> dict:
    qual = [g for g, c in classes.items() if c.startswith("QUALIFIED")]
    use = [g for g, c in classes.items() if c == "QUALIFIED_USEFUL"]
    conflict = [g for g, c in classes.items() if c == "LABEL_POLICY_CONFLICT"]
    major = len(conflict) + open_review_blockers
    if not qual:
        d = "EVIDENCE_INSUFFICIENT"
    elif len(use) >= 2 and major == 0:
        d = "CROSS_ENGINE_ACQUISITION_SUPPORTED"
    elif len(use) >= 2 or len(qual) == 1 or len(use) == 1:
        d = "CROSS_ENGINE_ACQUISITION_MIXED"
    else:
        d = "CROSS_ENGINE_ACQUISITION_NOT_SUPPORTED"
    limiting = None
    if d == "CROSS_ENGINE_ACQUISITION_MIXED":
        limiting = ("unresolved major inconsistency" if len(use) >= 2 else "only one game qualified" if len(qual) == 1 else "only one game was useful")
    return {"decision": d, "qualified": qual, "useful": use, "unresolved_major": major, "limiting_factor": limiting}


# ---------------------------------------------------------------------------------------------------------------------- analysis
def cell_kinds(cls: bytes) -> dict:
    n = (len(cls) + CELL - 1) // CELL
    pos, neg, mixed, masked = set(), set(), set(), set()
    for c in range(n):
        ch = cls[c * CELL:(c + 1) * CELL]
        if b"I" in ch:
            pos.add(c)
            if b"D" in ch:
                mixed.add(c)
        elif ch.count(b"D") == len(ch):
            neg.add(c)
        else:
            masked.add(c)
    return {"pos": pos, "neg": neg, "mixed": mixed, "masked": masked}


def load_run(d: pathlib.Path) -> dict:
    t = json.loads((d / "trace.json").read_text())
    return {"trace": t, "ck": json.loads((d / "checkpoints.json").read_text())}


def read_set(d: pathlib.Path) -> set:
    out = set()
    for rs in json.loads((d / "reads.json").read_text()).values():
        out |= ge.unranges(rs)
    return out


def indirect_edges(edges: list, img: bytes) -> set:
    return {(p, t) for p, t in edges if p < len(img) and is_indirect(img, p)}


def cond_metrics(private: pathlib.Path, cond: str, sid: str, img: bytes) -> dict:
    lab = private / "labels" / cond
    tiers, cls = lp.tiers_of(lab)
    tokens = (lab / "tokens.bin").read_bytes()
    meta = json.loads((lab / "meta.json").read_text())
    out = {"tiers": tiers, "cls": cls, "tokens": tokens, "meta": meta}
    for which in ("strong", "weak"):
        v = (private / "policy" / cond / which / "classes.bin").read_bytes()
        k = cell_kinds(v)
        out[which] = {"cls": v, "kinds": k, "hard": bz.hard_cells(v, img, tokens, k["neg"])}
    r = private / "runs" / sid
    out["pcs"] = {int(p) for p in json.loads((r / "pcs.json").read_text())["rom"]}
    edges = [tuple(e) for e in json.loads((r / "edges.json").read_text())]
    out["ind_edges"] = indirect_edges(edges, img)
    out["ind_targets"] = {t for _p, t in out["ind_edges"]}
    out["dma"] = [tuple(x) for x in json.loads((r / "dma.json").read_text())["rom_extents"]]
    out["reads"] = read_set(r)
    return out


def growth(trace: dict) -> dict:
    win = sorted((int(k), v) for k, v in trace["new_pcs_per_growth_window"].items())
    total = sum(v for _k, v in win)
    last = max((k for k, _v in win), default=0)
    out, cum = {}, 0
    for q in (25, 50, 75, 100):
        lim = last * q // 100
        out[str(q)] = sum(v for k, v in win if k <= lim)
    out["total_new_pcs"] = total
    return out


def tier_counts(m: dict) -> dict:
    t, c = m["tiers"], m["cls"]
    names = {lp.E: "E", lp.R: "R", lp.H: "H", lp.M: "M", lp.W: "W"}
    out = {n: 0 for n in names.values()}
    for i, x in enumerate(t):
        if x in names:
            out[names[x]] += 1
    out["E_I"] = sum(1 for i, x in enumerate(t) if x == lp.E and c[i] == 73)
    out["E_c"] = sum(1 for i, x in enumerate(t) if x == lp.E and c[i] == 99)
    out["U"] = c.count(b"U")
    return out


def variant_summary(m: dict, which: str) -> dict:
    k = m[which]["kinds"]
    v = m[which]["cls"]
    return {"cells_pos": len(k["pos"]), "cells_neg": len(k["neg"]), "cells_mixed": len(k["mixed"]), "cells_masked": len(k["masked"]),
            "hard_negative_cells": len(m[which]["hard"]), "labelled_bytes": sum(1 for c in v if c != 85), "masked_fraction_bytes": round(v.count(b"U") / len(v), 4)}


def analyze_game(game: str, private: pathlib.Path, rom: bytes, registry_sigs: dict | None, sonic_sigs: dict | None, earlier: dict, fixture_ok: bool) -> dict:
    g = GAMES[game]
    n = g["movie"]["frames"]
    priv = private / game
    ident = json.loads((priv / "identity.json").read_text())
    runs = {s: load_run(priv / "runs" / s) for s in SCENARIOS}
    tm = {c: cond_metrics(priv, c, s, rom) for c, s in (("tas", "tas_a"), ("ctl", "ctl_a"), ("ctl2", "ctl2_a"))}
    labelled = {"meta": tm["tas"]["meta"]}
    rg = replay_gate(ident, n, runs["playback"]["trace"], runs["tas_a"]["trace"], runs["tas_b"]["trace"], runs["playback"]["ck"], runs["tas_a"]["ck"], runs["tas_b"]["ck"])
    tg = tracing_gate(runs["tas_a"]["trace"], labelled, [runs["ctl_a"]["trace"], runs["ctl2_a"]["trace"]], n, fixture_ok)
    cons = consistency(tm["tas"]["meta"], tm["tas"]["tiers"], tm["tas"]["cls"], tm["tas"]["dma"], tm["tas"]["reads"])
    ledger = ledger_hours_game(priv)
    extents = json.loads((priv / "labels" / "tas" / "spans.json").read_text())["data_extents"]
    ex = {w: sorted(tm["tas"][w]["kinds"]["pos"] - tm["ctl"][w]["kinds"]["pos"]) for w in ("strong", "weak")}
    ex2 = sorted(tm["tas"]["strong"]["kinds"]["pos"] - tm["ctl2"]["strong"]["kinds"]["pos"])
    ctl_cells = len(tm["ctl"]["strong"]["kinds"]["pos"])
    ctl2_cells = len(tm["ctl2"]["strong"]["kinds"]["pos"])
    sig = {c: struct_sigs(rom, tm[c]["strong"]["cls"], tm[c]["tokens"]) for c in ("tas", "ctl")}
    for c in sig:
        sig[c]["boundary"] = struct_sigs(rom, tm[c]["weak"]["cls"], tm[c]["tokens"])["boundary"]      # code/data boundaries need weak data (descriptive)
    refs = {}
    if registry_sigs is not None:
        refs["registry"] = registry_sigs
    if sonic_sigs is not None:
        refs["sonic2_seg080"] = sonic_sigs
    for k, v in earlier.items():
        refs["batch:" + k] = v
    nov = novelty(sig["tas"], refs, sig["ctl"]) if refs else {}
    n_struct = nov.get("registry", {}).get("N_struct_not_in_control") or 0
    tas_pcs, ctl_pcs = tm["tas"]["pcs"], tm["ctl"]["pcs"]
    rec = {"input_present": True, "identity_ok": bool(ident["ok"]), "cpu_hours": ledger, "replay_ok": rg["replay_ok"], "tracing_ok": tg["tracing_ok"], "consistency_ok": cons["ok"],
           "extra_cells": len(ex["strong"]), "control_cells": ctl_cells, "n_struct": n_struct}
    cls = classify(rec)
    tas_tr = runs["tas_a"]["trace"]
    cpu = tas_tr["cpu_seconds"]
    cost = round(cpu / len(ex["strong"]), 3) if ex["strong"] else None
    res = {"game": game, "title": g["title"], "lineage": g["lineage"], "frames": n, "image": {"size": g["rom"]["size"], "sha256": g["rom"]["sha256"]},
           "movie_sha256": g["movie"]["sha256"], "classification": cls, "gate_replay": rg, "gate_tracing": tg, "consistency": cons, "cpu_hours": round(ledger, 3),
           "coverage": {"executed_rom_pcs": {"tas": len(tas_pcs), "control": len(ctl_pcs), "control_seed2": len(tm["ctl2"]["pcs"]), "tas_only": len(tas_pcs - ctl_pcs),
                                             "control_only": len(ctl_pcs - tas_pcs)},
                        "tiers": {c: tier_counts(tm[c]) for c in tm},
                        "strong_code_cells": {"tas": len(tm["tas"]["strong"]["kinds"]["pos"]), "control": ctl_cells, "control_seed2": ctl2_cells,
                                              "extra_vs_control": len(ex["strong"]), "extra_vs_control_seed2": len(ex2),
                                              "relative_increase": round(len(ex["strong"]) / ctl_cells, 4) if ctl_cells else None,
                                              "extra_weak_variant_cells": len(ex["weak"]), "useful_vs_seed2": useful(len(ex2), ctl2_cells)},
                        "indirect": {"targets_tas": len(tm["tas"]["ind_targets"]), "targets_control": len(tm["ctl"]["ind_targets"]),
                                     "extra_targets": len(tm["tas"]["ind_targets"] - tm["ctl"]["ind_targets"]),
                                     "edges_tas": len(tm["tas"]["ind_edges"]), "extra_edges": len(tm["tas"]["ind_edges"] - tm["ctl"]["ind_edges"])},
                        "growth_new_pcs_cumulative": {"tas": growth(tas_tr), "control": growth(runs["ctl_a"]["trace"])},
                        "cpu_seconds_per_extra_strong_cell": cost, "frames_per_extra_strong_cell": round(n / len(ex["strong"]), 2) if ex["strong"] else None},
           "data": {"strong": {c: variant_summary(tm[c], "strong") for c in tm}, "weak": {c: variant_summary(tm[c], "weak") for c in tm},
                    "extra_data": {"strong_negative_cells_vs_control": len(tm["tas"]["strong"]["kinds"]["neg"] - tm["ctl"]["strong"]["kinds"]["neg"]),
                                   "weak_negative_cells_vs_control": len(tm["tas"]["weak"]["kinds"]["neg"] - tm["ctl"]["weak"]["kinds"]["neg"]),
                                   "extra_mixed_cells_strong": len(tm["tas"]["strong"]["kinds"]["mixed"] - tm["ctl"]["strong"]["kinds"]["mixed"]),
                                   "extra_hard_negative_cells_strong": len(tm["tas"]["strong"]["hard"] - tm["ctl"]["strong"]["hard"])},
                    "overlap": {"cdl_exec_and_data_bytes": tas_tr["cdl"]["cdl_exec_and_data_bytes"], "e_bytes_in_dma_source": cons["e_bytes_in_dma_source"],
                                "e_bytes_also_read_as_data": cons["e_bytes_also_read_as_data"],
                                "rom_bytes_read_as_data_fraction": round(len(tm["tas"]["reads"]) / len(rom), 4), "r_data_conflicts": tm["tas"]["meta"]["typed"].get("R_DATA_CONFLICT", 0),
                                "ambiguous_masked_extents": sum(1 for e in extents if e["masked"])},
                    "data_extents": {c: dict(collections.Counter(e["kind"] + ("_masked" if e["masked"] else "") for e in json.loads(
                        (priv / "labels" / c / "spans.json").read_text())["data_extents"])) for c in ("tas", "ctl", "ctl2")},
                    "strong_vs_weak": {"labelled_bytes_lost_without_weak": round(1 - variant_summary(tm["tas"], "strong")["labelled_bytes"] /
                                                                                    max(1, variant_summary(tm["tas"], "weak")["labelled_bytes"]), 4)}},
           "novelty": nov, "extra_cells_kept_private": False}
    # private artifacts used by later games / the model-A diagnostic
    (priv / "sigs_tas_strong.json").write_text(json.dumps({k: (v if isinstance(v, (dict,)) else v) for k, v in sig["tas"].items()}))
    (priv / "extra_cells.json").write_text(json.dumps({"strong": ex["strong"], "seed2": ex2}))
    return res


def ledger_hours_game(priv: pathlib.Path, complete: bool = True, dangling: bool = True) -> float:
    lf = priv / "ledger.jsonl"
    if not lf.exists():
        return 0.0
    rows = [json.loads(ln) for ln in lf.read_text().splitlines() if ln.strip()]
    done = {r["attempt"]: r for r in rows if "cpu_seconds" in r}
    ok = {r["label"] for r in done.values() if r["ok"]}
    missing = [e for e in EXPECTED_LEDGER if e not in ok]
    if complete and missing:
        raise ge.EvidenceError(f"ledger lacks a successful attempt for {missing}")
    total = sum(r["cpu_seconds"] for r in done.values())
    if dangling:
        total += DANGLING_SECONDS * sum(1 for r in rows if r.get("intent") and r["attempt"] not in done)
    return total / 3600


def budget_guard(private_root: pathlib.Path, game: str, dangling: bool = False) -> None:
    """Refuse to start work when attempts already reach a ceiling (8 CPU-h for the game, 24 for the batch). Per-run calls count completed attempts
    only (sibling runs of one launch are legitimately in flight); the `guard` subcommand, run before each launch, also costs stale dangling attempts."""
    mine = ledger_hours_game(private_root / game, complete=False, dangling=dangling)
    total = sum(ledger_hours_game(private_root / g, complete=False, dangling=dangling) for g in ORDER)
    if mine >= THRESH["cpu_hours_per_game"] or total >= THRESH["cpu_hours_total"]:
        (private_root / game).mkdir(parents=True, exist_ok=True)
        (private_root / game / "status").write_text("BUDGET_EXHAUSTED\n")
        raise ge.EvidenceError(f"CPU ceiling reached (game {mine:.2f} h, batch {total:.2f} h)")


def failure_class(priv: pathlib.Path, game: str):
    """Class of a game whose acquisition did not complete (None when every run and label exists). Frozen precedence."""
    n = GAMES[game]["movie"]["frames"]
    idf = priv / "identity.json"
    if not idf.exists() or not json.loads(idf.read_text()).get("ok"):
        return "INPUT_REQUIRED"
    if ledger_hours_game(priv, complete=False) > THRESH["cpu_hours_per_game"]:
        return "BUDGET_EXHAUSTED"
    pb = priv / "runs" / "playback" / "trace.json"
    if not pb.exists():
        return "REPLAY_INVALID"
    t = json.loads(pb.read_text())
    if t["frames"] != n or not (t["movie_finished_at"] > 0 or t["movie_mode_end"] == "FINISHED"):
        return "REPLAY_INVALID"
    if any(not (priv / "runs" / s / "trace.json").exists() for s in ("tas_a", "tas_b", "ctl_a", "ctl2_a")):
        return "TRACING_UNAVAILABLE"
    if any(not (priv / "labels" / c / "meta.json").exists() or not (priv / "policy" / c / "weak" / "meta.json").exists() for c in ("tas", "ctl", "ctl2")):
        return "TRACING_UNAVAILABLE"
    return None


def sigs_from_json(d: dict) -> dict:
    out = {k: set(d[k]) for k in ALL_KINDS}
    out["form_counts"] = d["form_counts"]
    return out


def cmd_registry_sigs(a) -> int:
    verify_plan()
    import segarecomp_ml_v3_train as tr
    tr.DATA = pathlib.Path(a.registry)
    qualified = tr.qualified_lineages()
    sets, ids = [], []
    for d in tr.registry(set(qualified)):
        im = tr.Image(d)
        ids.append((im.id, ge.sha256(im.rom), im.lineage))
        sets.append(struct_sigs(im.rom, im.classes, im.tokens.tobytes()))
    reg = merge_sigs(sets)
    out = {k: sorted(reg[k]) for k in ALL_KINDS}
    out["form_counts"] = reg["form_counts"]
    out["images"] = sorted(ids)
    out["registry_digest"] = ge.sha256(json.dumps(sorted(ids)).encode())
    pathlib.Path(a.out).write_text(json.dumps(out))
    print(json.dumps({"images": len(ids), "registry_digest": out["registry_digest"], **{k: len(reg[k]) for k in ALL_KINDS}}, sort_keys=True))
    return 0


def sonic_sigs(label_dir: pathlib.Path) -> dict:
    tiers, cls = lp.tiers_of(label_dir)
    meta = json.loads((label_dir / "meta.json").read_text())
    img = pathlib.Path(meta["image"]).read_bytes()
    v = lp.build_variant(tiers, cls, "strong")
    return struct_sigs(img, v, (label_dir / "tokens.bin").read_bytes())


def sealed_identities() -> tuple:
    man = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())
    ents = man["sealed"]["entries"]
    return {e["image_sha256"] for e in ents if "image_sha256" in e}, {e["image_sha1"] for e in ents if "image_sha1" in e}


def isolation_check(game: str, registry_images: list) -> dict:
    g = GAMES[game]
    shas = {x[1] for x in registry_images}
    lins = {x[2] for x in registry_images}
    s256, s1 = sealed_identities()
    sealed = g["rom"]["sha256"] in s256 or g["rom"]["sha1"] in s1
    return {"image_sha256_in_registry": g["rom"]["sha256"] in shas, "lineage_in_registry": g["lineage"] in lins, "image_in_sealed_blind_manifest": sealed,
            "ok": g["rom"]["sha256"] not in shas and g["lineage"] not in lins and not sealed}


def cmd_analyze(a) -> int:
    plan = verify_plan()
    priv = pathlib.Path(a.private)
    ge.require_private(priv)
    rom = pathlib.Path(a.rom).read_bytes()
    ident = identity_check(a.game, rom, pathlib.Path(a.movie))          # ROM/movie recomputed from the bytes, never read from a file
    idf = priv / a.game / "identity.json"
    stored = json.loads(idf.read_text()) if idf.exists() else {}
    ident["checks"]["emulator_pins"] = bool(stored.get("checks", {}).get("emulator_pins", False))      # pinned by `identity`; absent -> not ok
    if "emulator" in stored:
        ident["emulator"] = stored["emulator"]
    ident["ok"] = all(ident["checks"].values())
    idf.parent.mkdir(parents=True, exist_ok=True)
    idf.write_text(json.dumps(ident, indent=1, sort_keys=True))
    fc = failure_class(priv / a.game, a.game)
    if fc is not None:
        res = {"game": a.game, "title": GAMES[a.game]["title"], "classification": {"class": fc}, "cpu_hours": round(ledger_hours_game(priv / a.game, complete=False), 3),
               "identity_checks": ident["checks"], "plan_sha256": ge.sha256(PLAN.read_bytes())}
        pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
        print(json.dumps({"game": a.game, "classification": res["classification"], "partial": True}))
        return 0
    reg = json.loads(pathlib.Path(a.registry_sigs).read_text())
    registry_sigs = sigs_from_json(reg)
    iso = isolation_check(a.game, [tuple(x) for x in reg["images"]])
    if not iso["ok"]:
        raise ge.EvidenceError(f"corpus isolation collision: {iso}")
    sonic = sigs_from_json(json.loads(pathlib.Path(a.sonic_sigs).read_text())) if a.sonic_sigs else None
    earlier = {}
    for e in a.earlier or []:
        earlier[e] = sigs_from_json(json.loads((priv / e / "sigs_tas_strong.json").read_text()))
    fx = json.loads(pathlib.Path(a.fixture).read_text())
    fixture_ok = bool(fx["ok"] and fx.get("plan_sha256") == ge.sha256(PLAN.read_bytes()) and fx.get("bound_code") == plan["bound_code"])
    res = analyze_game(a.game, priv, rom, registry_sigs, sonic, earlier, fixture_ok)
    res["isolation"] = iso
    res["registry_digest"] = reg["registry_digest"]
    res["plan_sha256"] = ge.sha256(PLAN.read_bytes())
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({"game": a.game, "classification": res["classification"], "extra": res["coverage"]["strong_code_cells"]["extra_vs_control"],
                      "N_struct": res["novelty"].get("registry", {}).get("N_struct"), "replay_ok": res["gate_replay"]["replay_ok"],
                      "tracing_ok": res["gate_tracing"]["tracing_ok"], "cpu_hours": res["cpu_hours"]}, sort_keys=True))
    return 0


def cmd_sonic_sigs(a) -> int:
    verify_plan()
    s = sonic_sigs(pathlib.Path(a.label_dir))
    out = {k: sorted(s[k]) for k in ALL_KINDS}
    out["form_counts"] = s["form_counts"]
    pathlib.Path(a.out).write_text(json.dumps(out))
    print(json.dumps({k: len(s[k]) for k in ALL_KINDS}))
    return 0


def cmd_batch(a) -> int:
    verify_plan()
    results = {g: json.loads(pathlib.Path(p).read_text()) for g, p in (x.split("=", 1) for x in a.result)}
    classes = {g: r["classification"]["class"] for g, r in results.items()}
    for g in ORDER:
        classes.setdefault(g, "INPUT_REQUIRED")          # only a game with no result file at all (no analyze/classify output exists)
    total = sum(r.get("cpu_hours", 0) for r in results.values())
    dec = batch_decision(classes, a.open_review_blockers)
    if total > THRESH["cpu_hours_total"]:
        dec["budget_note"] = "total CPU ceiling exceeded"
    dec.update({"open_review_blockers": a.open_review_blockers, "classes": classes, "cpu_hours_total": round(total, 3)})
    print(json.dumps(dec, indent=1, sort_keys=True))
    if a.out:
        pathlib.Path(a.out).write_text(json.dumps(dec, indent=1, sort_keys=True))
    return 0


# ----------------------------------------------------------------------------------------------------------------------- fixture
def fixture_movie(rom: bytes, frames: int) -> bytes:
    """A hand-made one-player power-on .bk2 for the synthetic cartridge (proves BizHawk loads a synthesized control movie)."""
    md5 = hashlib.md5(rom).hexdigest().upper()
    header = f"MovieVersion BizHawk v2.0\nPlatform GEN\nGameName fixture\nSHA1 {md5}\nCore Genplus-gx\nemuVersion Version 2.9.1\nOriginalEmuVersion Version 2.9.1\nrerecordCount 0\n"
    sync = json.dumps({"o": {"$type": "BizHawk.Emulation.Cores.Consoles.Sega.gpgx.GPGX+GPGXSyncSettings, BizHawk.Emulation.Cores", "UseSixButton": False,
                             "ControlTypeLeft": 1, "ControlTypeRight": 0, "Region": 0, "Filter": 1, "LowPassRange": 26214, "LowFreq": 880, "HighFreq": 5000,
                             "LowGain": 1.0, "MidGain": 1.0, "HighGain": 1.0, "BackdropColor": 4294902015}})
    key = "LogKey:#Power|Reset|#P1 Up|P1 Down|P1 Left|P1 Right|P1 A|P1 B|P1 C|P1 Start|"
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Header.txt", header)
        z.writestr("SyncSettings.json", sync)
        z.writestr("Comments.txt", "")
        z.writestr("Subtitles.txt", "")
        z.writestr("BizVersion.txt", "Version 2.9.1")
        z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join(["|..|........|"] * frames) + "\n[/Input]\n")
    return buf.getvalue()


def cmd_fixture(a) -> int:
    if PLAN.exists():
        verify_plan()          # the tooling fixture may run before the freeze (no commercial input is involved)
    out = pathlib.Path(a.out)
    rom, truth = bz.build_fixture()
    rom = rom + bytes(int(a.pad) - len(rom)) if int(a.pad) > len(rom) else rom
    jobs = pathlib.Path(a.jobs)
    jobs.mkdir(parents=True, exist_ok=True)
    fx = jobs / "fixture.gen"
    fx.write_bytes(rom)
    mv = jobs / "fixture.bk2"
    mv.write_bytes(fixture_movie(rom, 120))
    sc = {"id": "fixture", "mode": "movie", "frames": 120, "size": len(rom), "checkpoints": checkpoints(120)}
    ns = argparse.Namespace(rom=str(fx), movie=str(mv), bizhawk=a.bizhawk, scenario=json.dumps(sc), out=str(out), jobs=a.jobs, ledger=a.ledger, user=a.user,
                            display=a.display, timeout="300")
    bz.measured(a.ledger, "fixture", bz.run_scenario, ns)
    res = bz.fixture_check(out, truth)
    ck = json.loads((out / "checkpoints.json").read_text())
    res["generic_checkpoint_fields"] = all(f in ck[0]["state"] for f in ("ram=", "vram=", "cram=", "vsram="))
    res["no_game_fields"] = "zone=" not in ck[0]["state"]
    st = parse_state(ck[0]["state"])
    res["domains_readable"] = all(st.get(k, "na") != "na" for k in ("ram", "vram", "cram", "vsram"))
    res["plan_sha256"] = ge.sha256(PLAN.read_bytes()) if PLAN.exists() else None
    res["bound_code"] = bound_digests()
    res["ok"] = bool(res["ok"] and res["generic_checkpoint_fields"] and res["no_game_fields"] and res["domains_readable"])
    (out / "fixture.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps(res, sort_keys=True))
    return 0 if res["ok"] else 2


# ------------------------------------------------------------------------------------------------------------------------ plan
def bound_digests() -> dict:
    root = HERE.parent
    return {f: ge.sha256((root / f).read_bytes()) for f in BOUND if (root / f).exists()}


def cmd_freeze(a) -> int:
    if PLAN.exists():
        raise ge.EvidenceError("plan already frozen")
    ml = HERE / "segarecomp_genesis_tasbatch_ml.py"
    if not ml.exists():
        raise ge.EvidenceError("the model-A module must exist before the freeze")
    doc = dict(PROTOCOL)
    doc["bound_code"] = bound_digests()
    data = ge.canonical(doc)
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(ge.sha256(data) + "\n")
    print(json.dumps({"plan_sha256": ge.sha256(data), "bound": sorted(doc["bound_code"])}))
    return 0


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if ge.sha256(data) != PLAN_DIGEST.read_text().strip():
        raise ge.EvidenceError("plan digest mismatch")
    doc = json.loads(data)
    if {k: v for k, v in doc.items() if k not in PLAN_EXTRA} != json.loads(json.dumps(PROTOCOL)):
        raise ge.EvidenceError("committed plan differs from the protocol in code")
    if doc["bound_code"] != bound_digests():
        raise ge.EvidenceError("bound code changed after the freeze")
    return doc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("identity")
    p.add_argument("--game", required=True, choices=ORDER), p.add_argument("--rom", required=True), p.add_argument("--movie", required=True)
    p.add_argument("--bizhawk", required=True), p.add_argument("--package"), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_identity)
    p = sub.add_parser("run")
    p.add_argument("--game", required=True, choices=ORDER), p.add_argument("--scenario-id", required=True, choices=SCENARIOS)
    p.add_argument("--rom", required=True), p.add_argument("--movie", required=True), p.add_argument("--bizhawk", required=True)
    p.add_argument("--jobs", required=True), p.add_argument("--private", required=True)
    p.add_argument("--user", default="bhuser"), p.add_argument("--display", default="99"), p.add_argument("--timeout", default="21600")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("label")
    p.add_argument("--game", required=True, choices=ORDER), p.add_argument("--rom", required=True), p.add_argument("--cli", required=True)
    p.add_argument("--cli-sha256", required=True), p.add_argument("--private", required=True)
    p.set_defaults(fn=cmd_label)
    p = sub.add_parser("registry-sigs")
    p.add_argument("--registry", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_registry_sigs)
    p = sub.add_parser("sonic-sigs")
    p.add_argument("--label-dir", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_sonic_sigs)
    p = sub.add_parser("analyze")
    p.add_argument("--game", required=True, choices=ORDER), p.add_argument("--rom", required=True), p.add_argument("--private", required=True)
    p.add_argument("--movie", required=True), p.add_argument("--registry-sigs", required=True), p.add_argument("--sonic-sigs"), p.add_argument("--earlier", nargs="*")
    p.add_argument("--fixture", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_analyze)
    p = sub.add_parser("batch")
    p.add_argument("--result", nargs="+", required=True, help="game=path"), p.add_argument("--open-review-blockers", type=int, default=0), p.add_argument("--out")
    p.set_defaults(fn=cmd_batch)
    p = sub.add_parser("fixture")
    p.add_argument("--bizhawk", required=True), p.add_argument("--jobs", required=True), p.add_argument("--out", required=True), p.add_argument("--ledger", required=True)
    p.add_argument("--user", default="bhuser"), p.add_argument("--display", default="99"), p.add_argument("--pad", default="0")
    p.set_defaults(fn=cmd_fixture)
    p = sub.add_parser("guard")
    p.add_argument("--game", required=True, choices=ORDER), p.add_argument("--private", required=True)
    p.set_defaults(fn=lambda a: verify_plan() and budget_guard(pathlib.Path(a.private), a.game, dangling=True) or print("guard ok") or 0)
    p = sub.add_parser("freeze")
    p.set_defaults(fn=cmd_freeze)
    p = sub.add_parser("verify-plan")
    p.set_defaults(fn=lambda a: print(json.dumps({"plan_sha256": ge.sha256(PLAN.read_bytes()), "ok": bool(verify_plan())})) or 0)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
