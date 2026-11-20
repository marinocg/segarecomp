#!/usr/bin/env python3
"""SEG-146-T006: the launcher exposes the shared session recorder for Genesis (--record / --replay / --allow-variant), through the real executable and a product double.

The launcher hands the program SEGARECOMP_RECORD / SEGARECOMP_REPLAY / SEGARECOMP_ALLOW_VARIANT in its environment (the program's argument list is unchanged); the program double logs them.
Checks: record and replay reach the program with the given file; --allow-variant only with --replay; the options are exclusive; they need --run; a replay file must exist; and a Master System game
(the shared code is not wired there yet) is refused with a stable message instead of being started.

usage: launcher_session_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
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


def genesis_rom(path):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    path.write_bytes(bytes(image))


def sms_rom(path):
    image = bytearray(0x8000)
    for i in range(len(image)):
        image[i] = (i * 31 + 7) & 0xFF
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    path.write_bytes(bytes(image))


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def check(ok, message):
        if not ok:
            failures.append(message)
            print("FAIL:", message)

    with tempfile.TemporaryDirectory() as tmp_text:
        tmp = pathlib.Path(tmp_text)
        package = tmp / "package"
        make_package(package, fake_cli)
        session_log = tmp / "session.log"
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_CACHE_DIR=str(tmp / "cache"), SEGARECOMP_LIBRARY_DIR=str(tmp / "lib"),
                   SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=fake_game, FAKE_BUILD_ARGS_LOG=str(tmp / "args.txt"), FAKE_GAME_SESSION_LOG=str(session_log))
        rom = tmp / "game.bin"
        genesis_rom(rom)
        sms = tmp / "game.sms"
        sms_rom(sms)
        recording = tmp / "recording.rec"
        recording.write_text("segarecomp-session 1\n")

        def run(*args):
            return subprocess.run([exe, *args], env=env, timeout=240, capture_output=True, text=True)

        def last_session():
            lines = session_log.read_text().splitlines() if session_log.exists() else []
            return lines[-1] if lines else ""

        done = run("--build", str(rom), "--run")
        check(done.returncode == 0 and "record=- replay=- variant=-" in last_session(), "an ordinary run hands the program no session variables: " + done.stdout + done.stderr)
        done = run("--build", str(rom), "--run", "--record", str(tmp / "out.rec"))
        check(done.returncode == 0 and f"record={(tmp / 'out.rec').resolve()} replay=- variant=-" in last_session(), "--record reaches the program: " + last_session() + done.stdout)
        done = run("--build", str(rom), "--run", "--replay", str(recording))
        check(done.returncode == 0 and f"record=- replay={recording.resolve()} variant=-" in last_session(), "--replay reaches the program: " + last_session())
        done = run("--build", str(rom), "--run", "--replay", str(recording), "--allow-variant")
        check(done.returncode == 0 and "variant=1" in last_session(), "--allow-variant reaches the program with --replay")
        before = session_log.read_text()
        done = run("--build", str(rom), "--run", "--replay", str(tmp / "missing.rec"))
        check(done.returncode == 2 and "--replay file not found" in done.stdout and session_log.read_text() == before, "a missing replay file is refused before the program starts")
        done = run("--build", str(rom), "--run", "--record", "a.rec", "--replay", str(recording))
        check(done.returncode == 2 and "exclusive" in done.stderr, "--record and --replay are exclusive")
        done = run("--build", str(rom), "--run", "--allow-variant")
        check(done.returncode == 2 and "--allow-variant applies to --replay" in done.stderr, "--allow-variant needs --replay")
        done = run("--build", str(rom), "--record", "a.rec")
        check(done.returncode == 2 and "apply to --run" in done.stderr, "--record needs --run")
        before = session_log.read_text()
        done = run("--build", str(sms), "--run", "--record", str(tmp / "sms.rec"))
        check(done.returncode == 2 and "apply to Genesis games" in done.stdout and session_log.read_text() == before, "a Master System game is refused, not started: " + done.stdout)


        # The window: the details page's "Record this session" switch, Play, and a Replay of the newest recording (the game double logs what it was handed).
        gui_dir = tmp / "gui"
        gui_dir.mkdir()
        gui_env = dict(env, SEGARECOMP_CACHE_DIR=str(gui_dir / "cache"), SEGARECOMP_LIBRARY_DIR=str(gui_dir / "lib"), SEGARECOMP_SHOT_SIZE="720x520")
        gui_log = gui_dir / "session.log"
        gui_env["FAKE_GAME_SESSION_LOG"] = str(gui_log)
        gui_rom = gui_dir / "g.md"
        genesis_rom(gui_rom)
        imported = subprocess.run([exe, "--import", str(gui_rom)], env=gui_env, timeout=60, capture_output=True, text=True)
        check("import=added" in imported.stdout, "the game imports: " + imported.stdout + imported.stderr)

        def gui_run(script):
            script_path, trace_path = gui_dir / "script.txt", gui_dir / "trace.txt"
            script_path.write_text("\n".join(script) + "\n")
            trace_path.unlink(missing_ok=True)
            subprocess.run([exe], env=dict(gui_env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path)), timeout=240, check=True, capture_output=True)
            return trace_path.read_text().splitlines() if trace_path.exists() else []

        open_and_build = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "build", "wait_idle", "frames 6", "expect build_outcome=ok"]
        trace = gui_run(open_and_build + ["session_record on", "play", "wait_play_end", "frames 4", "quit"])
        check(not any(l.startswith("expect FAIL") for l in trace) and "session=record" in "\n".join(trace), "Record + Play starts a recording: " + repr(trace))
        lines = gui_log.read_text().splitlines() if gui_log.exists() else []
        sessions = gui_dir / "lib" / "sessions" / "genesis"
        check(len(lines) == 1 and lines[0].startswith("record=") and str(sessions) in lines[0] and "replay=-" in lines[0], "the program was handed a recording path inside the library: " + repr(lines))
        folders = [p for p in sessions.iterdir()] if sessions.exists() else []
        check(len(folders) == 1, "the recordings folder exists, one per game image: " + repr(folders))
        trace = gui_run(["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "session_replay", "frames 4", "quit"])
        check("session=no_recording" in "\n".join(trace), "a Replay with no recording starts nothing: " + repr(trace))
        (folders[0] / "session-1.rec").write_text("segarecomp-session 1\n")
        trace = gui_run(["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "session_variant on", "session_replay", "wait_play_end", "frames 4", "quit"])
        lines = gui_log.read_text().splitlines()
        check(len(lines) == 2 and "replay=" in lines[1] and "session-1.rec" in lines[1] and "variant=1" in lines[1] and "record=-" in lines[1], "a Replay hands the newest recording and the variant flag to the program: " + repr(lines))
        check("session=replay allow_variant=1" in "\n".join(trace), "the replay is traced")

    if failures:
        print(len(failures), "failure(s)")
        return 1
    print("launcher_session_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
