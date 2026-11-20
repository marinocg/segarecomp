#!/usr/bin/env python3
"""SEG-150 corrective round B (F30), notifications (F7, F8, F9), through the real window (SDL dummy video, product and game doubles).

  * a toast is drawn in the hero band, where no control lives: it never overlaps a button (the running card's Cancel, the Game page's Play / Cancel) at 720x520 and 1080x740;
  * an import that cannot be saved to disk reports only the truthful outcome ("Could not save the library ...") and never "N game(s) added."; nothing starts preparing;
  * an unwritable library and an incomplete installation raise one standing, non-blocking notice on the Library and the Game page, and it disappears once the cause is fixed.

usage: launcher_notifications_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

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


def genesis_rom(path, seed):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x120:0x130] = f"GAME NUMBER {seed:03d}".encode()[:16].ljust(16)
    image[0x1F0] = seed
    path.write_bytes(bytes(image))


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game, games=0):
        self.exe, self.fake_cli = exe, fake_cli
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package(self.package, fake_cli)
        self.lib, self.cache = self.dir / "lib", self.dir / "cache"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache), SEGARECOMP_LIBRARY_DIR=str(self.lib),
                        SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game)
        self.roms = self.dir / "roms"
        self.roms.mkdir()
        for i in range(games):
            genesis_rom(self.roms / f"g{i}.md", i + 1)
            done = subprocess.run([exe, "--import", str(self.roms / f"g{i}.md")], env=self.env, timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr

    def start(self, script, size, extra=None):
        script_path, self.trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        self.trace_path.unlink(missing_ok=True)
        env = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(self.trace_path), SEGARECOMP_SHOT_SIZE=size, **(extra or {}))
        return subprocess.Popen([self.exe], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def run(self, script, size, extra=None):
        proc = self.start(script, size, extra)
        proc.wait(timeout=240)
        return self.trace()

    def trace(self):
        return self.trace_path.read_text().splitlines() if self.trace_path.exists() else []


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def check(trace, name, expected_ok):
        failures.extend(f"{name}: {l}" for l in trace if "FAIL" in l)
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected_ok:
            failures.append(f"{name}: {ok} of {expected_ok} expectations ran: {trace!r}")

    def expect(condition, message):
        if not condition:
            failures.append(message)

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. F7: toasts never cover a button, on the Library with a running and a queued card and on the Game page while preparing.
        for size in ("720x520", "1080x740"):
            p = Profile(tmp, "toast-" + size, exe, fake_cli, fake_game, games=3)
            hold = p.dir / "hb"
            script = ["frames 20", "main_action n:0", f"wait_file {hold}.started", "frames 6", "main_action n:1", "frames 6",
                      "expect toasts=1", "expect toast_overlap=0",
                      "focus_card 2", "frames 4", "key Return", "frames 12", "expect page=details", "build", "frames 6", "expect toasts=1", "expect toast_overlap=0",
                      "key Escape", "frames 8", "expect page=library",
                      "main_action n:2", "frames 6", "main_action n:1", "frames 6", "main_action n:0", "wait_idle", "frames 10", "quit"]
            trace = p.run(script, size, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"})
            check(trace, f"toasts {size}", 6)
            expect(sum(1 for l in trace if l.startswith("toast=Queued:")) == 2, f"toasts {size}: the queued toasts were shown: {trace!r}")
            for f in p.dir.glob("hb*"):
                f.unlink()

        # 2. F9: an import that cannot persist reports only the truth, starts nothing, and the profile shows its standing notice.
        p = Profile(tmp, "unwritable", exe, fake_cli, fake_game)
        blocker = p.dir / "afile"
        blocker.write_text("x")
        env = {"SEGARECOMP_LIBRARY_DIR": str(blocker / "lib"), "SEGARECOMP_CACHE_DIR": str(blocker / "cache")}
        genesis_rom(p.roms / "q.md", 9)
        trace = p.run(["frames 14", "expect health_notice=Your library cannot be saved", "expect library_size=0", f"import_files {p.roms / 'q.md'}", "wait_import", "frames 8",
                       "review_prepare", "frames 14", "expect build_count=0", "expect health_notice=Your library cannot be saved", "quit"], "720x520", env)
        check(trace, "unwritable import", 4)
        toasts = [l for l in trace if l.startswith("toast=")]
        expect(any(t.startswith("toast=error Could not save the library") for t in toasts), f"unwritable: the truthful toast is shown: {toasts!r}")
        expect(not any("added" in t for t in toasts), f"unwritable: no success toast beside the failure: {toasts!r}")
        expect(any(l.startswith("import=not_saved") for l in trace), f"unwritable: outcome traced: {trace!r}")

        # 3. F8 / F9: standing notice on Library and Game page; gone once the cause is fixed (the script waits for the test to repair the profile).
        p = Profile(tmp, "standing-incomplete", exe, fake_cli, fake_game, games=1)
        shutil.rmtree(p.package)
        go, done = p.dir / "go", p.dir / "go2"
        proc = p.start(["frames 20", "expect health_notice=Installation incomplete", "focus_card 0", "frames 4", "key Return", "frames 12", "expect page=details",
                        "expect health_notice=Installation incomplete", "expect modal=0", f"wait_file {go}", f"wait_file {done}", "frames 10", "expect health_notice=-", "quit"], "720x520")
        time.sleep(1.0)
        make_package(p.package, fake_cli)
        go.write_text("1")
        time.sleep(4.0)    # the health re-check runs every 3 seconds
        done.write_text("1")
        proc.wait(timeout=120)
        check(p.trace(), "incomplete notice", 5)

        p = Profile(tmp, "standing-unwritable", exe, fake_cli, fake_game)
        blocker = p.dir / "afile"
        blocker.write_text("x")
        env = {"SEGARECOMP_LIBRARY_DIR": str(blocker / "lib"), "SEGARECOMP_CACHE_DIR": str(blocker / "cache")}
        go, done = p.dir / "go", p.dir / "go2"
        proc = p.start(["frames 20", "expect health_notice=Your library cannot be saved", "expect modal=0", f"wait_file {go}", f"wait_file {done}", "frames 10", "expect health_notice=-", "quit"], "720x520", env)
        time.sleep(1.0)
        blocker.unlink()
        blocker.mkdir()
        go.write_text("1")
        time.sleep(4.0)
        done.write_text("1")
        proc.wait(timeout=120)
        check(p.trace(), "unwritable notice", 3)

        # 4. a healthy profile shows no notice at all.
        p = Profile(tmp, "healthy", exe, fake_cli, fake_game, games=1)
        check(p.run(["frames 20", "expect health_notice=-", "focus_card 0", "frames 4", "key Return", "frames 12", "expect health_notice=-", "quit"], "720x520"), "healthy", 2)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_notifications_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
