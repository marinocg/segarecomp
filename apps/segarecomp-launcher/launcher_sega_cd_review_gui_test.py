#!/usr/bin/env python3
"""SEG-150-T005: a Sega CD disc in the ONE Add game review, on the real window (dummy video driver, scripted; the GUI commands are the actions the buttons run).

Synthetic only: a project-authored fixture disc (tools/sega_cd_fixture_disc.py) and BIOS doubles (a few hundred bytes with a Genesis-style header that only
names a region). Covers: a complete disc versus missing tracks and unsupported layouts (typed reasons), no BIOS -> a request (nothing is addable), a verified BIOS
remembered as a local profile (identity + location, never trust), reuse only after revalidation, moved / changed / different BIOS files with their explicit state
and next action, several remembered BIOS (the user chooses, nothing is guessed), a BIOS that changes between choosing and adding, cancelling the disc check, and
that a duplicate never replaces anything (the game's save data and recordings stay byte for byte).

usage: launcher_sega_cd_review_gui_test.py <Segarecomp> <source-root>
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

LAUNCHER, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def bios_bytes(region, tag):
    image = bytearray(0x400)
    image[:len(tag)] = tag
    if region:
        image[0x100:0x104] = b"SEGA"
        image[0x1F0] = ord(region)
    return bytes(image)


def build_disc(directory, variant="cooked", note=""):
    subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_fixture_disc.py"), "build", "--fixture", "comm", "--variant", variant, "--out", str(directory)], check=True, capture_output=True)
    if note:   # a different CUE sheet (so a different game) over the same tracks
        cue = directory / "disc.cue"
        cue.write_text(cue.read_text() + "REM %s\n" % note)
    return directory / "disc.cue"


class Window:
    def __init__(self, tmp, name):
        self.tmp, self.name = tmp, name
        self.lib = tmp / (name + "-lib")
        self.lib.mkdir(exist_ok=True)
        self.n = 0

    def run(self, script, extra_env=None):
        self.n += 1
        script_path, trace_path = self.tmp / ("%s-%d.script" % (self.name, self.n)), self.tmp / ("%s-%d.trace" % (self.name, self.n))
        script_path.write_text("\n".join(script) + "\n")
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_LIBRARY_DIR=str(self.lib),
                   SEGARECOMP_CACHE_DIR=str(self.tmp / (self.name + "-cache")), SEGARECOMP_HOME=str(self.tmp / "no-home"), SEGARECOMP_NO_COVER_FETCH="1",
                   SEGARECOMP_SHOT_SIZE="1080x740", SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path))
        env.update(extra_env or {})
        subprocess.run([LAUNCHER], env=env, timeout=300, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def games(self):
        f = self.lib / "library.json"
        return json.loads(f.read_text())["games"] if f.exists() else []

    def profiles(self):
        f = self.lib / "sega-cd-bios.json"
        return json.loads(f.read_text())["profiles"] if f.exists() else []


def held(trace, label):
    bad = [l for l in trace if l.startswith("expect FAIL")]
    ok = sum(l.startswith("expect ok") for l in trace)
    check(not bad and ok > 0, "%s (%s)" % (label, bad or "%d expectations held" % ok))


def inspect(cue, *more):
    return ["page import", "frames 4", "import_files %s" % cue, "wait_scd_inspect", "frames 4", *more]


def main():
    if sys.platform != "linux":
        print("SKIP: a Linux host is not available")
        return 0
    with tempfile.TemporaryDirectory(prefix="scd-review-") as tmp:
        tmp = pathlib.Path(tmp)
        usa, eur, jpn = tmp / "bios-usa.bin", tmp / "bios-eur.bin", tmp / "bios-jpn.bin"
        usa.write_bytes(bios_bytes("U", b"SYNTHETIC USA DOUBLE"))
        eur.write_bytes(bios_bytes("E", b"SYNTHETIC EUROPE DOUBLE"))
        jpn.write_bytes(bios_bytes("J", b"SYNTHETIC JAPAN DOUBLE"))
        cue1 = build_disc(tmp / "disc1")
        cue2 = build_disc(tmp / "disc2", note="second")
        cue3 = build_disc(tmp / "disc3", note="third")
        cue4 = build_disc(tmp / "disc4", note="fourth")

        # ---- A: a complete disc, no remembered BIOS: the review asks for the user's own BIOS; nothing is addable until one is chosen ----
        w = Window(tmp, "a")
        t = w.run(inspect(cue1, "expect page=import", "expect review=needs_choice", "expect review_disc.0=ok", "expect review_bios.0=none", "expect review_options.0=-", "expect review_selected=0",
                          "review_add", "review_prepare", "frames 4", "expect review=needs_choice", "expect page=import",
                          "review_bios 0 %s" % (tmp / "missing.bin"), "frames 2", "expect review_bios.0=none",
                          "review_bios 0 %s" % usa, "frames 4", "expect review=valid", "expect review_bios.0=chosen:USA", "expect review_selected=1", "expect scd_profiles=0",
                          "review_add", "frames 8", "expect page=library", "expect scd_profiles=1", "quit"))
        held(t, "A: no BIOS -> a request (nothing addable, nothing added); a missing file is refused; the verified BIOS makes it addable")
        text = "\n".join(t)
        check("scd=inspected total=1 cancelled=0" in text and "scd=bios_refused reason=bios_missing" in text and "scd=bios_chosen region=USA via=choose" in text and "scd=added program=none new=1 region=USA" in text,
              "A: the trace shows the disc check, the refused file, the chosen BIOS and the add")
        games = w.games()
        check(len(games) == 1 and games[0]["platform"] == "sega-cd", "A: exactly one Sega CD game was added (and only after the BIOS was verified)")
        profiles = w.profiles()
        check(len(profiles) == 1 and profiles[0]["sha256"] == hashlib.sha256(usa.read_bytes()).hexdigest() and profiles[0]["region"] == "USA" and pathlib.Path(profiles[0]["path"]) == usa and set(profiles[0]) == {"sha256", "size", "region", "path", "verified_at"},
              "A: the BIOS is remembered as a local profile: verified identity (SHA-256), region and location only - no trust flag")
        check(not list(w.lib.glob("*.tmp")) and not list(w.lib.glob("*.new")), "A: the profile store left no temporary file")

        # ---- B: the disc verdicts: missing tracks (named), unsupported layouts (typed reasons) ----
        broken = tmp / "broken"
        shutil.copytree(tmp / "disc1", broken)
        (broken / "track02.bin").unlink()
        raw = tmp / "rawdisc"
        raw.mkdir()
        (raw / "x.cue").write_text('FILE "x.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')
        (raw / "x.bin").write_bytes(b"x" * 2352 * 20)
        w = Window(tmp, "b")
        t = w.run(inspect(broken / "disc.cue", "expect review=invalid", "expect review_disc.0=tracks_missing", "expect review_bios.0=none", "review_add", "review_prepare", "frames 4", "expect review=invalid", "review_decline", "frames 2", "expect review=-",
                          "import_files %s" % (raw / "x.cue"), "wait_scd_inspect", "frames 4", "expect review=invalid", "expect review_disc.0=raw_sync",
                          "review_decline", "frames 2", "import_files %s" % (tmp / "nothing.cue"), "wait_scd_inspect", "frames 4", "expect review=invalid", "expect review_disc.0=cue_unreadable", "quit"))
        held(t, "B: missing tracks, an unsupported layout and an unreadable CUE are refused with their typed reasons and cannot be added")
        check(w.games() == [] and w.profiles() == [], "B: nothing was added and no BIOS was remembered")

        # ---- C: reuse only after revalidation: the remembered BIOS is measured again; one verified BIOS is offered but never added silently ----
        wa = Window(tmp, "a")
        t = wa.run(inspect(cue2, "expect review_options.0=ok", "expect review_bios.0=chosen:USA", "expect review=valid", "expect review_selected=1", "review_add", "frames 8", "expect scd_profiles=1", "quit"))
        held(t, "C: a remembered, still verified BIOS is revalidated, offered as the choice (one profile, no ambiguity) and used on Add")
        check(len(wa.games()) == 2 and len(wa.profiles()) == 1, "C: the second disc was added and the profile was refreshed, not duplicated")
        # the same BIOS and disc again: already in the library, nothing replaced; the game's own data stays byte for byte
        game_dir = tmp / "a-lib" / "sega-cd" / ("%s" % wa.games()[0]["sha256"][:16])
        (game_dir / "recordings").mkdir(parents=True, exist_ok=True)
        (game_dir / "backup-ram.bin").write_bytes(b"\x5a" * 8192)
        (game_dir / "recordings" / "session-1.txt").write_text("1 down Right\n")
        before = {p.name: p.read_bytes() for p in (game_dir / "backup-ram.bin", game_dir / "recordings" / "session-1.txt")}
        catalog_before = (wa.lib / "library.json").read_bytes()
        t = wa.run(inspect(cue1, "expect review_bios.0=chosen:USA", "expect review=duplicate", "expect review_selected=0", "review_add", "review_prepare", "frames 6", "quit"))
        held(t, "C: the same BIOS and disc again is 'already in your library'")
        check((wa.lib / "library.json").read_bytes() == catalog_before and all((game_dir / n).read_bytes() == b for n, b in (("backup-ram.bin", before["backup-ram.bin"]),)) and
              (game_dir / "recordings" / "session-1.txt").read_bytes() == before["session-1.txt"], "C: a duplicate changes nothing: the library file, the backup RAM and the recording are byte for byte the same")

        # ---- D: a moved BIOS: explicit state and next action; locating needs the very same file; another file is refused ----
        moved = tmp / "moved-bios-usa.bin"
        shutil.move(str(usa), str(moved))
        t = wa.run(inspect(cue3, "expect review=needs_choice", "expect review_options.0=bios_missing", "expect review_bios.0=none", "review_add", "frames 2", "expect review=needs_choice",
                           "review_locate_profile 0 0 %s" % eur, "frames 2", "expect review_bios.0=none", "expect review=needs_choice",
                           "review_locate_profile 0 0 %s" % moved, "frames 4", "expect review_bios.0=chosen:USA", "expect review=valid", "review_add", "frames 8", "expect scd_profiles=1", "quit"))
        held(t, "D: a moved BIOS is 'missing' (nothing chosen for the user); a different file is refused; the same file located again is used and Add works")
        check("scd=bios_refused reason=bios_changed" in "\n".join(t), "D: the different file was refused as a different BIOS (its SHA-256 differs)")
        prof = wa.profiles()
        check(len(prof) == 1 and pathlib.Path(prof[0]["path"]) == moved and len(wa.games()) == 3, "D: the profile follows the BIOS to its new location (same identity); the third disc was added")

        # ---- E: changed content at the remembered place -> 'changed'; two remembered BIOS of different regions -> the user chooses ----
        moved.write_bytes(bios_bytes("U", b"SYNTHETIC USA DOUBLE, EDITED"))
        t = wa.run(inspect(cue4, "expect review_options.0=bios_changed", "expect review_bios.0=none", "expect review=needs_choice", "review_add", "frames 2", "expect review=needs_choice", "quit"))
        held(t, "E: a BIOS file whose content changed is 'changed', never used")
        moved.write_bytes(bios_bytes("U", b"SYNTHETIC USA DOUBLE"))   # restored byte for byte
        wb = Window(tmp, "a")
        t = wb.run(inspect(cue4, "expect review_options.0=ok", "expect review_bios.0=chosen:USA", "quit"))
        held(t, "E: the restored file revalidates again (identity, not history)")
        # add a second, different-region BIOS through the review of disc 5, then the next disc sees two
        cue5 = build_disc(tmp / "disc5", note="fifth")
        t = wb.run(inspect(cue5, "review_bios 0 %s" % eur, "frames 4", "expect review=valid", "expect review_bios.0=chosen:Europe", "review_add", "frames 8", "expect scd_profiles=2", "quit"))
        held(t, "E: a second BIOS (Europe) is chosen explicitly in the review and remembered next to the first")
        check(sorted(p["region"] for p in wb.profiles()) == ["Europe", "USA"], "E: two profiles are stored, keyed by identity, each with its own region")
        cue6 = build_disc(tmp / "disc6", note="sixth")
        t = wb.run(inspect(cue6, "expect review_options.0=ok,ok", "expect review_bios.0=none", "expect review=needs_choice", "expect review_selected=0", "review_add", "frames 2", "expect review=needs_choice",
                           "review_use_profile 0 1", "frames 4", "expect review_bios.0=chosen:USA", "expect review=valid", "review_add", "frames 8", "expect page=library", "quit"))
        held(t, "E: two verified BIOS of different regions -> no choice is made for the user; choosing one explicitly then adds the game")

        # ---- F: a BIOS that changes between the choice and Add is revalidated at use time and refused; the row stays with its state ----
        cue7 = build_disc(tmp / "disc7", note="seventh")
        wf = Window(tmp, "f")
        scratch = tmp / "bios-scratch.bin"
        scratch.write_bytes(bios_bytes("J", b"SYNTHETIC SCRATCH"))
        script = inspect(cue7, "review_bios 0 %s" % scratch, "frames 4", "expect review=valid", "wait_file %s" % (tmp / "go-ahead"), "review_add", "frames 4", "expect review=needs_choice", "expect review_bios.0=none", "expect scd_profiles=0", "quit")
        import threading
        trace_file = tmp / "f-1.trace"

        def edit_after_choice():   # the BIOS file is edited from outside once the window has chosen it, then the script is released
            import time
            for _ in range(600):
                if trace_file.exists() and "scd=bios_chosen" in trace_file.read_text():
                    scratch.write_bytes(bios_bytes("J", b"SYNTHETIC SCRATCH, EDITED"))
                    (tmp / "go-ahead").write_text("go")
                    return
                time.sleep(0.05)
        editor = threading.Thread(target=edit_after_choice)
        editor.start()
        t = wf.run(script)
        editor.join()
        held(t, "F: a BIOS that changed after it was chosen is not added; the row asks for the BIOS again")
        check("scd=add_refused refusal=bios_changed" in "\n".join(t) and wf.games() == [] and wf.profiles() == [], "F: the add was refused as bios_changed; nothing was added or remembered")

        # ---- I: one remembered BIOS whose header region differs from the disc: offered, never chosen silently ----
        wi = Window(tmp, "i")
        t = wi.run(inspect(cue1, "review_bios 0 %s" % eur, "frames 4", "expect review_bios.0=chosen:Europe", "review_add", "frames 8", "quit"))
        t += wi.run(inspect(cue2, "expect review_options.0=ok", "expect review_bios.0=none", "expect review=needs_choice", "review_use_profile 0 0", "frames 4", "expect review_bios.0=chosen:Europe", "expect review=valid", "quit"))
        held(t, "I: a remembered BIOS declaring another region than the disc is not chosen for the user; using it is an explicit step")

        # ---- G: cancelling the disc check: no item, nothing added, the window stays responsive ----
        wg = Window(tmp, "g")
        t = wg.run(["page import", "frames 4", "import_files %s" % cue1, "frames 3", "scd_inspect_cancel", "wait_scd_inspect", "frames 6", "expect review=-", "expect page=import", "quit"], {"SEGARECOMP_SCD_CHECK_HOLD_MS": "5000"})
        held(t, "G: Cancel stops the disc check: no review item and the page stays usable")
        check("scd=inspected total=1 cancelled=1" in "\n".join(t) and wg.games() == [], "G: the trace says the check was cancelled; nothing was added")

        # ---- H: Add & prepare in an installation without the Sega CD runtime: only Add only is available (typed, explained), then the game is added ----
        wh = Window(tmp, "h")
        t = wh.run(inspect(cue1, "review_bios 0 %s" % eur, "frames 4", "expect review=valid", "review_prepare", "frames 4", "expect review=valid", "expect page=import", "review_add", "frames 8", "expect page=library", "quit"))
        held(t, "H: Add & prepare is refused where this installation cannot prepare Sega CD games; Add only still works")
        check(len(wh.games()) == 1, "H: the game was added by Add only")

        # ---- J: attach / re-verify of a prebuilt program (Game > Advanced) is refused with a typed, plain-language reason; a running check can be cancelled and changes nothing ----
        wj = Window(tmp, "h")
        open_game = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "expect scd_state=No program yet (can be built)"]
        t = wj.run(open_game + ["scd_attach %s" % eur, "wait_scd", "frames 6", "expect modal=1", "key Escape", "frames 6", "expect modal=0", "expect scd_state=No program yet (can be built)",
                                "scd_verify", "wait_scd", "frames 6", "expect modal=1", "key Escape", "frames 4", "quit"])
        held(t, "J: a file that is not a program is refused with a message (never trusted); the game stays as it was")
        check("scd=program_refused refusal=program_unverifiable" in "\n".join(t), "J: the refusal is the typed program_unverifiable class")
        before = (wj.lib / "library.json").read_bytes()
        t = wj.run(open_game + ["scd_attach %s" % eur, "frames 3", "scd_cancel", "wait_scd", "frames 6", "expect modal=0", "expect scd_state=No program yet (can be built)", "quit"], {"SEGARECOMP_SCD_CHECK_HOLD_MS": "5000"})
        held(t, "J: cancelling a running program check ends it quietly (no refusal sheet) and the window stays responsive")
        check("scd=verify_cancelled" in "\n".join(t) and (wj.lib / "library.json").read_bytes() == before, "J: the cancelled check changed nothing in the library")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
