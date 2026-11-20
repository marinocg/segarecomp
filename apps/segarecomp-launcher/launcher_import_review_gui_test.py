#!/usr/bin/env python3
"""SEG-150-T003: the Add game review and the explicit preparation intent, on the real window with the product and game doubles.

Covers Add only versus Add & prepare, a batch with one invalid, one duplicate and one needs-choice (Master System mapper) item, declining the review,
cancelling a queued and a running preparation from the library's main action, Prepare & play (one launch, only from a successful build), and that
nothing is ever launched or prepared without the explicit request. Synthetic files only; the GUI script commands are the same actions the buttons run.
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def make_package(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                "sdl3/include/SDL3/SDL.h"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(fake_cli, root / "bin" / ("segarecomp" + EXE_SUFFIX))


def genesis_rom(path, index):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x1E0] = index
    path.write_bytes(bytes(image))


def sms_rom(path):
    image = bytearray(0x8000)
    for i in range(len(image)):
        image[i] = (i * 31 + 7) & 0xFF
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    path.write_bytes(bytes(image))


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.game_log = self.dir / "game.log"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="1080x740",
                        FAKE_GAME_BIN=fake_game, FAKE_GAME_LOG=str(self.game_log))

    def headless_import(self, path):
        done = subprocess.run([self.exe, "--import", str(path)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def run(self, script, extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=240, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def catalog(self):
        return json.loads((self.lib / "library.json").read_text())["games"] if (self.lib / "library.json").exists() else []

    def builds(self):
        games = self.cache / "games"
        return sorted(p for p in games.iterdir() if p.is_dir() and not p.name.endswith((".partial", ".failed"))) if games.exists() else []

    def launches(self):
        return len(self.game_log.read_text().splitlines()) if self.game_log.exists() else 0


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    def check(name, trace, expected):
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def has(trace, text):
        return any(l.startswith(text) for l in trace)

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. Add only: the file is inspected (nothing is in the library), then added; nothing is built and nothing launched.
        p = Profile(tmp, "add-only", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "one.md", 1)
        trace = p.run([f"import_files {p.dir / 'one.md'}", "wait_import", "frames 6", "expect review=valid", "expect library_size=0", "expect page=import", "expect review_selected=1",
                       "review_add", "frames 20", "expect library_size=1", "expect page=library", "expect build_count=0", "expect review=-", "expect main_action=Prepare & play", "quit"])
        check("add only", trace, 9)
        expect(len(p.catalog()) == 1 and not p.builds() and p.launches() == 0 and not has(trace, "build=started") and not has(trace, "run=started"), "add only built or launched something")

        # 2. Add & prepare: the same file is added and its preparation is queued; the finished build is NOT launched.
        p = Profile(tmp, "add-prepare", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "two.md", 2)
        trace = p.run([f"import_files {p.dir / 'two.md'}", "wait_import", "frames 6", "expect review=valid", "review_prepare", "wait_idle", "frames 60", "expect library_size=1",
                       "expect build_outcome=ok", "expect playing=0", "expect main_action=Play", "quit"])
        check("add and prepare", trace, 5)
        expect(len(p.builds()) == 1 and has(trace, "build=started"), "Add & prepare did not prepare the game")
        expect(p.launches() == 0 and not has(trace, "run=started") and not has(trace, "prepare_and_play"), "Add & prepare launched the game")

        # 3. A batch: valid + duplicate + invalid + needs-choice. Invalid and duplicate items are not selectable; the needs-choice item is added but never prepared;
        #    the existing entry (and so its saves and settings) is not touched.
        p = Profile(tmp, "batch", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "have.md", 3)
        p.headless_import(p.dir / "have.md")
        before = p.catalog()
        genesis_rom(p.dir / "fresh.md", 4)
        shutil.copy(p.dir / "have.md", p.dir / "have copy.md")      # the same content under another name: a duplicate by identity
        (p.dir / "notes.txt").write_text("not a game file")
        sms_rom(p.dir / "needs.sms")
        files = ";".join(str(p.dir / n) for n in ("fresh.md", "have copy.md", "notes.txt", "needs.sms"))
        trace = p.run([f"import_files {files}", "wait_import", "frames 6", "expect review=valid,duplicate,invalid,needs_choice", "expect review_selected=2", "review_toggle 1", "review_toggle 2",
                       "expect review_selected=2", "expect library_size=1", "review_prepare", "wait_idle", "frames 60", "expect library_size=3", "expect build_outcome=ok", "expect playing=0", "quit"])
        summary = [l for l in trace if l.startswith("import=summary")]
        if not summary or "1 needs a choice first" not in summary[0] or "1 need a choice" in summary[0]:   # round C P3: singular grammar
            failures.append(f"review summary grammar: {summary!r}")
        check("batch", trace, 7)
        after = p.catalog()
        expect(len(after) == 3, f"batch: expected 3 entries, got {len(after)}")
        expect(all(g in after for g in before), "batch: the existing entry changed")
        expect(len(p.builds()) == 1, f"batch: only the valid new game is prepared (not the needs-choice one, the duplicate or the existing game): {p.builds()}")
        expect(p.launches() == 0 and not has(trace, "run=started"), "batch: something was launched")

        # 4. Decline: Cancel on the review adds nothing and leaves the library as it was.
        p = Profile(tmp, "decline", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "have.md", 5)
        p.headless_import(p.dir / "have.md")
        before = p.catalog()
        genesis_rom(p.dir / "other.md", 6)
        trace = p.run([f"import_files {p.dir / 'other.md'};{p.dir / 'have.md'}", "wait_import", "frames 6", "expect review=valid,duplicate", "review_decline", "frames 6", "expect review=-",
                       "expect library_size=1", "expect build_count=0", "quit"])
        check("decline", trace, 4)
        expect(p.catalog() == before and not p.builds() and p.launches() == 0, "decline changed the library")

        # 5. Cancel of a queued and of a running preparation through the library's main action; nothing is launched.
        p = Profile(tmp, "cancel", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "alpha.md", 7)
        genesis_rom(p.dir / "beta.md", 8)
        hold = p.dir / "heartbeat"
        started = f"{hold}.started"
        trace = p.run([f"import_files {p.dir / 'alpha.md'};{p.dir / 'beta.md'}", "wait_import", "frames 6", "review_prepare", f"wait_file {started}", "frames 6", "expect build_count=2", "expect queue=1",
                       "main_action beta", "frames 4", "expect queue=0", "expect build_count=1", "main_action alpha", "wait_idle", "frames 20", "expect build_outcome=cancelled", "expect build_count=0",
                       "expect playing=0", "quit"], {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"})
        check("cancel queued and running", trace, 7)
        expect(has(trace, "main_action=Unqueue") and has(trace, "main_action=Cancel"), "the main action was not Unqueue for the queued and Cancel for the running game")
        left = [x for x in p.cache.rglob("*") if x.name.endswith((".partial", ".failed"))] if p.cache.exists() else []
        expect(not left and p.launches() == 0, f"a cancelled preparation left files behind or launched: {left}")

        # 6. Prepare & play is an explicit request: it prepares, then launches exactly once from the build it made. A failed preparation never launches.
        p = Profile(tmp, "prepare-play", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "play.md", 9)
        trace = p.run([f"import_files {p.dir / 'play.md'}", "wait_import", "frames 6", "review_add", "frames 20", "expect main_action=Prepare & play", "expect playing=0",
                       "main_action play", "wait_idle", "frames 30", "wait_play_end", "frames 20", "expect build_outcome=ok", "expect playing=0", "expect main_action=Play", "quit"])
        check("prepare and play", trace, 5)
        expect(sum(1 for l in trace if l.startswith("run=started")) == 1 and p.launches() == 1 and has(trace, "prepare_and_play=launching"), f"Prepare & play must launch exactly once: {p.launches()}")

        p = Profile(tmp, "prepare-fail", exe, fake_cli, fake_game)
        genesis_rom(p.dir / "bad.md", 10)
        trace = p.run([f"import_files {p.dir / 'bad.md'}", "wait_import", "frames 6", "review_add", "frames 20", "main_action bad", "wait_idle", "frames 30", "expect build_outcome=failed",
                       "expect playing=0", "expect main_action=Retry", "quit"], {"FAKE_BUILD_FAIL": "optimized,compatibility"})
        check("prepare and play failure", trace, 3)
        expect(p.launches() == 0 and not has(trace, "run=started") and not has(trace, "prepare_and_play=launching"), "a failed preparation launched the game")

        # 7. An unreadable / unsupported selection alone is rejected: no entry is created, Add only stays disabled (the commit would add nothing).
        p = Profile(tmp, "invalid-only", exe, fake_cli, fake_game)
        (p.dir / "junk.bin").write_bytes(b"\0" * 64)
        trace = p.run([f"import_files {p.dir / 'junk.bin'}", "wait_import", "frames 6", "expect review=invalid", "expect review_selected=0", "review_add", "frames 6", "expect library_size=0", "quit"])
        check("invalid only", trace, 3)
        expect(not p.catalog() and not p.builds(), "an invalid file created an entry")

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_import_review_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
