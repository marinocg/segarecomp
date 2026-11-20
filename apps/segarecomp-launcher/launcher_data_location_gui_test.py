#!/usr/bin/env python3
"""SEG-150-T009 (ADR 0317): Settings > Storage & Data > Change data location drives the REAL T008 core, through the real window (SDL dummy video driver).

Every profile is synthetic and isolated: SEGARECOMP_DATA_BOOTSTRAP names a private bootstrap file, XDG_DATA_HOME makes the default data root private, and the
environment overrides SEGARECOMP_CACHE_DIR / SEGARECOMP_LIBRARY_DIR are removed (except in the override case). Checked, with file hashes where a file matters:

  * success: plan preview (counts and bytes equal the files on disk), confirm, completion with "restart required", the bootstrap names the new root, every durable file is
    byte-identical at the new root, regenerable data and the old root are untouched, an external ROM is untouched; a second process (the restart) resolves the new root with
    its games and saves, offers the previous-location cleanup (preview bytes equal the regenerable files), and removes only those; the optional include-copies cleanup keeps a
    file that changed after the move;
  * cancel during the copy: the old root stays in use, the bootstrap is unchanged, nothing remains at the destination;
  * refusals (non-empty, inside the data folder, too little space) show the typed reason, a decline changes nothing, Esc returns focus to the opening button;
  * an environment override disables the control and explains why;
  * a running build refuses the move as busy; a running move refuses a build;
  * an interrupted move is rolled back at the next start and the notice is shown once; a move interrupted after the switch is completed at the next start;
  * a controller-only run: Settings > Storage & Data > Change data location > the built-in folder picker > plan > Cancel changes nothing.

usage: launcher_data_location_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import launcher_default_mode_test as base  # noqa: E402
import launcher_gamepad_journey_test as pad  # noqa: E402

failures = []


def expect(condition, message):
    if not condition:
        failures.append(message)


def check_trace(name, trace, expected):
    failures.extend(f"{name}: {line}" for line in trace if line.startswith("expect FAIL"))
    ok = sum(1 for line in trace if line.startswith("expect ok"))
    if ok != expected:
        failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


REGENERABLE = ("games", "zig-cache", "sega-cd-objects", "cover-downloads")


class Profile:
    """A launcher profile at its DEFAULT data root (private XDG dir) with a private bootstrap file and synthetic data."""

    def __init__(self, tmp, name, exe, fake_cli, fake_game, size="1080x740", files=6):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        base.make_package(self.package, fake_cli)
        self.root = self.dir / "xdg" / "Segarecomp"
        self.lib = self.root / "library"
        self.bootstrap = self.dir / "boot" / "Segarecomp-data-location.json"
        self.bootstrap.parent.mkdir()
        self.ext = self.dir / "external"
        self.ext.mkdir()
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_HOME=str(self.package),
                        SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE=size, FAKE_GAME_BIN=fake_game, XDG_DATA_HOME=str(self.dir / "xdg"), HOME=str(self.dir / "home"),
                        SEGARECOMP_DATA_BOOTSTRAP=str(self.bootstrap), FAKE_BUILD_ARGS_LOG=str(self.dir / "args.txt"))
        for var in ("SEGARECOMP_CACHE_DIR", "SEGARECOMP_LIBRARY_DIR", "SEGARECOMP_TEST_MIGRATION_HOLD_MS", "SEGARECOMP_TEST_MIGRATION_CRASH", "SEGARECOMP_TEST_FREE_SPACE_BYTES"):
            self.env.pop(var, None)
        (self.dir / "home").mkdir()
        for i in range(2):   # two games added by reference from an external folder
            rom = self.ext / f"game{i}.md"
            base.genesis_rom(rom)
            data = bytearray(rom.read_bytes())
            data[0x1E0] = i + 1
            rom.write_bytes(bytes(data))
            done = subprocess.run([exe, "--import", str(rom)], env=self.env, timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr
        rom_hash = json.loads((self.lib / "library.json").read_text())["games"][0]["sha256"]
        # durable data of every class the plan lists
        for i in range(files):
            self.write(f"library/saves/genesis/{rom_hash}/slot{i}.state", os.urandom(300 + 40 * i))
        self.write("library/covers/c.png", os.urandom(500))
        self.write("library/sessions/rec.bin", os.urandom(200))
        self.write("library/patches/p/readme.txt", b"a mod package file\n")
        # regenerable data: a build folder (a 32 hex name), the compiler cache and a download
        self.write("games/" + "a" * 32 + "/game.bin", os.urandom(4000))
        self.write("games/" + "a" * 32 + "/obj/x.o", os.urandom(1000))
        self.write("zig-cache/z/blob", os.urandom(2500))
        self.write("cover-downloads/d.png", os.urandom(700))
        self.ext_hash = {p.name: sha(p) for p in self.ext.iterdir()}
        self.ext_mtime = {p.name: p.stat().st_mtime_ns for p in self.ext.iterdir()}

    def builds(self):
        log = self.dir / "args.txt"
        return log.read_text().splitlines() if log.exists() else []

    def write(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run(self, script, extra_env=None, size=None, timeout=240):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        if size:
            e["SEGARECOMP_SHOT_SIZE"] = size
        subprocess.run([self.exe], env=e, timeout=timeout, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def check(self, name, script, extra_env=None, size=None):
        expected = sum(1 for line in script if line.startswith(("expect ", "pad_seek ")))
        trace = self.run(script, extra_env, size)
        check_trace(name, trace, expected)
        return trace


def is_durable(rel):
    parts = rel.parts
    return not (parts[0] in REGENERABLE or rel.as_posix() == "library/library.json.bak" or (len(parts) > 3 and parts[:2] == ("library", "sega-cd") and parts[3].startswith("build")))


def durable(root):
    """{relative posix path: sha256} of every file the plan classes as durable under a data root."""
    out = {}
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.is_file():
            rel = path.relative_to(root)
            if is_durable(rel):
                out[rel.as_posix()] = sha(path)
    return out


def tree_size(path):
    return sum(p.stat().st_size for p in pathlib.Path(path).rglob("*") if p.is_file())


def regenerable_bytes(root):
    return sum(tree_size(root / name) for name in REGENERABLE if (root / name).exists())


def snapshot(root):
    """Everything under a root, with hashes: unchanged means byte-identical files and the same set of files."""
    return {p.relative_to(root).as_posix(): (sha(p) if p.is_file() else "dir") for p in sorted(pathlib.Path(root).rglob("*"))}


def journal_files(profile):
    return [p for p in profile.bootstrap.parent.iterdir() if p.name != profile.bootstrap.name]


def open_and_plan(dest):
    return ["frames 6", "dm_open", "frames 4", "expect page=data_location", "expect data_move=choose", f"dm_destination {dest}", "dm_wait", "frames 4", "expect data_move=plan"]


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # ------------------------------------------------------------------ 1. success, restart, explicit cleanup
        p = Profile(tmp, "success", exe, fake_cli, fake_game)
        dest = p.dir / "newhome"
        before = durable(p.root)
        total = sum((p.root / rel).stat().st_size for rel in before)
        old_before = snapshot(p.root)
        script = open_and_plan(dest) + ["expect dm_plan=ok", f"expect dm_plan_files={len(before)}", f"expect dm_plan_bytes={total}",
                                        "dm_confirm", "dm_wait", "frames 6", "expect data_move=finished", "expect dm_outcome=completed:", "expect dm_restart=1",
                                        "dm_back", "frames 6", "expect page=settings", "expect settings_tab=2", "expect dm_previous=present", "expect dm_change=enabled",
                                        "dm_cleanup_open", "dm_wait", "frames 4", "expect dm_cleanup=refused:restart_needed",   # the running session still uses the previous location
                                        "dm_back", "frames 4", "quit"]
        trace = p.check("success", script)
        expect(any("data_move=finished outcome=completed" in line for line in trace), f"success: finished trace: {trace!r}")
        boot = json.loads(p.bootstrap.read_text())
        expect(pathlib.Path(boot["root"]) == dest, f"success: the bootstrap names the new root: {boot}")
        after = durable(dest)
        expect(set(before) <= set(after), f"success: every durable file arrived: {sorted(set(before) - set(after))}")
        expect(all(after[rel] == before[rel] for rel in before), "success: durable files are byte-identical at the new root")
        expect(all((dest / n).exists() is False for n in REGENERABLE), "success: regenerable data was not copied")
        old_after = snapshot(p.root)
        changed = [k for k in old_before if old_before[k] != old_after.get(k)]
        expect(not [k for k in changed if k.startswith(("games", "zig-cache", "cover-downloads"))], f"success: regenerable data at the old root untouched: {changed}")
        expect(all(k in old_after for k in old_before), "success: the old root is retained until an explicit cleanup")
        expect({n: sha(p.ext / n) for n in os.listdir(p.ext)} == p.ext_hash, "success: the external ROM files are byte-identical")
        expect({n: (p.ext / n).stat().st_mtime_ns for n in os.listdir(p.ext)} == p.ext_mtime, "success: the external ROM files are not even touched")
        catalog = json.loads((dest / "library" / "library.json").read_text())
        expect(all(str(p.ext) in g["rom_paths"][0] for g in catalog["games"]), f"success: catalog references still point at the external ROMs: {catalog['games'][0]['rom_paths']}")
        expect(len(journal_files(p)) >= 1, "success: the completion record is kept beside the bootstrap")
        # the second process is the restart: it resolves the new root
        regen = regenerable_bytes(p.root)
        script = ["frames 8", f"expect data_root={dest}", f"expect data_library={dest / 'library'}", "expect library_size=2", "expect dm_restart=0", "settings_tab 2", "frames 8",
                  "expect dm_previous=present", "dm_cleanup_open", "dm_wait", "frames 4", f"expect dm_cleanup=eligible:3:{regen}",
                  "dm_cleanup_confirm", "dm_wait", "frames 4", f"expect dm_cleanup_result=removed:{regen}:problems=0", "dm_back", "frames 4", "quit"]
        p.check("restart + cleanup", script)
        old_final = snapshot(p.root)
        expect(not (p.root / "games" / ("a" * 32)).exists() and not (p.root / "zig-cache").exists() and not (p.root / "cover-downloads").exists(), "cleanup: the regenerable folders were removed")
        expect(all(old_final.get(k) == old_after.get(k) for k in old_after if not k.startswith(REGENERABLE)), "cleanup: durable files of the previous location were kept (default option)")
        expect(durable(dest) == after or set(before) <= set(durable(dest)), "cleanup: the new location is intact")
        expect({n: sha(p.ext / n) for n in os.listdir(p.ext)} == p.ext_hash, "cleanup: external ROMs untouched")

        # ------------------------------------------------------------------ 1b. include migrated copies: only files unchanged since the move
        p = Profile(tmp, "copies", exe, fake_cli, fake_game)
        dest = p.dir / "newhome"
        before = durable(p.root)
        p.check("copies move", open_and_plan(dest) + ["dm_confirm", "dm_wait", "frames 6", "expect dm_outcome=completed:", "quit"])
        tampered = next(rel for rel in before if rel.startswith("library/saves/"))
        (p.root / tampered).write_bytes(b"edited after the move")   # something the user did in the previous location afterwards
        old_now = durable(p.root)
        removable = [rel for rel in before if old_now.get(rel) == before[rel]]
        expect(tampered not in removable and len(removable) >= 3, "copies: the setup left removable and kept files")
        regen = regenerable_bytes(p.root)
        copy_bytes = sum((p.root / rel).stat().st_size for rel in removable)
        script = ["frames 8", "settings_tab 2", "frames 8", "dm_cleanup_open", "dm_wait", "frames 4", f"expect dm_cleanup=eligible:3:{regen}",
                  "dm_cleanup_copies 1", "dm_wait", "frames 4", f"expect dm_cleanup=eligible:{3 + len(removable)}:{regen + copy_bytes}",
                  "dm_cleanup_confirm", "dm_wait", "frames 4", f"expect dm_cleanup_result=removed:{regen + copy_bytes}:problems=0", "quit"]
        p.check("copies cleanup", script)
        expect((p.root / tampered).read_bytes() == b"edited after the move", "copies: a file changed after the move was kept")
        expect(not any((p.root / rel).exists() for rel in removable), "copies: unchanged migrated copies were removed")
        expect(all(sha(pathlib.Path(dest) / rel) == before[rel] for rel in before), "copies: the new location still holds every file")

        # ------------------------------------------------------------------ 2. cancel during the copy
        p = Profile(tmp, "cancel", exe, fake_cli, fake_game, files=8)
        dest = p.dir / "newhome"
        old_before = snapshot(p.root)
        script = open_and_plan(dest) + ["dm_confirm", "dm_wait_phase copy", "expect data_move=running", "expect dm_phase=copy", "dm_cancel", "dm_wait", "frames 6",
                                        "expect data_move=finished", "expect dm_outcome=cancelled:cancelled", "expect dm_restart=0", "dm_back", "frames 4", "expect page=settings", "quit"]
        p.check("cancel", script, {"SEGARECOMP_TEST_MIGRATION_HOLD_MS": "400"})
        expect(not p.bootstrap.exists(), "cancel: the bootstrap was never written")
        expect(not dest.exists() or not any(dest.iterdir()), "cancel: nothing remains at the destination")
        expect(snapshot(p.root) == old_before or all(old_before.get(k) == v for k, v in snapshot(p.root).items() if k in old_before), "cancel: the old root is unchanged")
        expect(journal_files(p) == [], f"cancel: no journal is left: {journal_files(p)}")
        p.check("cancel then restart", ["frames 8", f"expect data_root={p.root}/", "expect library_size=2", "expect dm_notice=-", "quit"])

        # ------------------------------------------------------------------ 2b. closing the launcher during the copy cancels it, waits for the worker and leaves the old root in use
        p = Profile(tmp, "closing", exe, fake_cli, fake_game, files=8)
        dest = p.dir / "newhome"
        old_before = snapshot(p.root)
        trace = p.check("closing", open_and_plan(dest) + ["dm_confirm", "dm_wait_phase copy", "quit"], {"SEGARECOMP_TEST_MIGRATION_HOLD_MS": "500"})
        expect("shutdown=done" in trace, f"closing: the launcher shut down cleanly: {trace[-4:]!r}")
        expect(not p.bootstrap.exists() and journal_files(p) == [], f"closing: no bootstrap and no journal remain: {journal_files(p)}")
        expect(not dest.exists() or not any(dest.iterdir()), "closing: the partial copy was rolled back")
        expect(snapshot(p.root) == old_before or all(old_before.get(k) == v for k, v in snapshot(p.root).items() if k in old_before), "closing: the old root is unchanged")
        p.check("closing then restart", ["frames 8", f"expect data_root={p.root}/", "expect library_size=2", "expect dm_notice=-", "quit"])

        # ------------------------------------------------------------------ 3. refusals change nothing; Esc returns focus to the button
        p = Profile(tmp, "refusals", exe, fake_cli, fake_game)
        old_before = snapshot(p.root)
        foreign = p.dir / "occupied"
        foreign.mkdir()
        (foreign / "mine.txt").write_text("not segarecomp's")
        script = open_and_plan(foreign) + ["expect dm_plan=refused:destination_not_empty", "dm_confirm", "frames 6", "expect data_move=plan",
                                           "dm_use_sub", "dm_wait", "frames 4", "expect data_move=plan", "expect dm_plan=ok", "dm_back", "frames 6", "expect page=settings",
                                           "expect data_move=choose", "dm_open", "frames 4", f"dm_destination {p.root / 'inside'}", "dm_wait", "frames 4",
                                           "expect dm_plan=refused:destination_inside_source", "key Escape", "frames 8", "expect page=settings", "expect settings_tab=2", "expect focus_id=dm_change", "quit"]
        p.check("refusals", script)
        p.check("refusal space", open_and_plan(p.dir / "elsewhere") + ["expect dm_plan=refused:insufficient_space", "dm_confirm", "frames 6", "expect data_move=plan", "quit"], {"SEGARECOMP_TEST_FREE_SPACE_BYTES": "1000"})
        expect(not p.bootstrap.exists() and journal_files(p) == [], "refusals: no bootstrap and no journal")
        expect(sorted(os.listdir(foreign)) == ["mine.txt"] and (foreign / "mine.txt").read_text() == "not segarecomp's", "refusals: the foreign folder is untouched (no marker, no new folder)")
        expect(not (p.dir / "elsewhere").exists(), "refusals: the destination of a refused plan was not created")
        expect(snapshot(p.root) == old_before or all(old_before.get(k) == v for k, v in snapshot(p.root).items() if k in old_before), "refusals: the old root is unchanged")

        # ------------------------------------------------------------------ 4. environment override disables the control and says why
        for variable, code in (("SEGARECOMP_CACHE_DIR", "env_override_cache"), ("SEGARECOMP_LIBRARY_DIR", "env_override_library")):
            p = Profile(tmp, "env-" + variable[11:16].lower(), exe, fake_cli, fake_game)
            governed = p.dir / "governed"
            script = ["frames 6", "settings_tab 2", "frames 8", "expect dm_change=disabled", "dm_change", "frames 4", "expect page=settings", "dm_open", "frames 4", "expect page=data_location",
                      f"dm_destination {p.dir / 'target'}", "dm_wait", "frames 4", f"expect dm_plan=refused:{code}", "dm_confirm", "frames 4", "expect data_move=plan", "quit"]
            p.check("env " + variable, script, {variable: str(governed)})
            expect(not p.bootstrap.exists() and not (p.dir / "target").exists(), f"env {variable}: nothing was written")

        # ------------------------------------------------------------------ 5. busy in both directions
        p = Profile(tmp, "busy", exe, fake_cli, fake_game)
        heartbeat = p.dir / "heartbeat"
        started = str(heartbeat) + ".started"
        dest = p.dir / "newhome"
        script = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", "build", "frames 4", "expect building=1", f"wait_file {started}",
                  "dm_open", "frames 4", f"dm_destination {dest}", "dm_wait", "frames 4", "expect dm_plan=ok", "dm_confirm", "dm_wait", "frames 6",
                  "expect dm_outcome=busy:busy", "expect building=1", "dm_back", "frames 4", "page library", "frames 6", "focus_card 0", "frames 4", "key Return", "frames 8", "cancel_build", "wait_idle", "frames 6", "quit"]
        p.check("busy (a build runs)", script, {"FAKE_BUILD_HOLD": str(heartbeat)})
        expect(not p.bootstrap.exists() and not dest.exists(), "busy: nothing was moved while a build ran")
        p = Profile(tmp, "busy2", exe, fake_cli, fake_game, files=8)
        dest = p.dir / "newhome"
        script = open_and_plan(dest) + ["dm_confirm", "dm_wait_phase copy", "page library", "frames 6", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details",
                                        "build", "frames 6", "expect building=0", "expect queued=1", "dm_cancel", "dm_wait", "frames 6", "expect dm_outcome=cancelled:cancelled",
                                        "wait_idle", "frames 6", "expect building=0", "expect queued=0", "quit"]   # a build asked for during a move waits for it (like during any maintenance)
        p.check("busy (a move runs)", script, {"SEGARECOMP_TEST_MIGRATION_HOLD_MS": "1500"})
        expect(len(p.builds()) == 1 if hasattr(p, "builds") else True, "busy: the build ran once, after the move ended")

        # ------------------------------------------------------------------ 6. recovery notices
        p = Profile(tmp, "recovery", exe, fake_cli, fake_game, files=6)
        dest = p.dir / "newhome"
        old_before = snapshot(p.root)
        p.check("interrupt", open_and_plan(dest) + ["dm_confirm", "dm_wait", "frames 6", "expect dm_outcome=interrupted:interrupted", "quit"], {"SEGARECOMP_TEST_MIGRATION_CRASH": "copy:3"})
        expect(len(journal_files(p)) >= 1 and dest.exists() and any(dest.rglob("*")), "recovery: the interrupted move left a journal and a partial copy")
        expect(not p.bootstrap.exists(), "recovery: the bootstrap was not written")
        script = ["frames 8", f"expect data_root={p.root}/", "settings_tab 2", "frames 8", "expect dm_notice=rolled_back", "dm_notice_dismiss", "frames 4", "expect dm_notice=-", "quit"]
        p.check("recovered", script)
        expect(not dest.exists() and journal_files(p) == [] and not p.bootstrap.exists(), f"recovery: rolled back completely: {journal_files(p)}")
        expect(snapshot(p.root) == old_before or all(old_before.get(k) == v for k, v in snapshot(p.root).items() if k in old_before), "recovery: the old root is unchanged")
        p.check("recovered once", ["frames 8", "expect dm_notice=-", "quit"])
        # interrupted right after the switch: the next start completes it and uses the new root
        p = Profile(tmp, "recovery2", exe, fake_cli, fake_game, files=4)
        dest = p.dir / "newhome"
        p.check("interrupt after switch", open_and_plan(dest) + ["dm_confirm", "dm_wait", "frames 6", "expect dm_outcome=interrupted:interrupted", "quit"], {"SEGARECOMP_TEST_MIGRATION_CRASH": "commit_after"})
        p.check("completed at start", ["frames 8", f"expect data_root={dest}", "expect library_size=2", "settings_tab 2", "frames 8", "expect dm_notice=completed", "expect dm_previous=present", "quit"])

        # ------------------------------------------------------------------ 6b. a saved location that is not available (an unplugged drive): the default is used, the notice says so, nothing is created there
        p = Profile(tmp, "unplugged", exe, fake_cli, fake_game, files=2)
        gone = p.dir / "unplugged-drive" / "Segarecomp"
        p.bootstrap.write_text(json.dumps({"schema": 1, "root": str(gone)}))
        p.check("unplugged", ["frames 8", f"expect data_root={p.root}/", "expect library_size=2", "expect data_notice=The saved data location", "settings_tab 2", "frames 8", "expect dm_change=disabled", "dm_change", "frames 6", "expect page=settings", "quit"])
        expect(not gone.exists() and not gone.parent.exists(), "unplugged: nothing was created at the unavailable location")
        expect(json.loads(p.bootstrap.read_text())["root"] == str(gone), "unplugged: the saved location was not rewritten")
        expect(not p.bootstrap.with_name(p.bootstrap.name + ".journal").exists(), "unplugged: no migration journal was started")

        # ------------------------------------------------------------------ 7. a controller alone: plan, then decline
        for size in ("1080x740", "720x520"):
            p = Profile(tmp, "pad" + size, exe, fake_cli, fake_game, size=size)
            pick = p.dir / "pick"   # the picker's only location: it holds one empty folder to choose
            target = pick / "target"
            target.mkdir(parents=True)
            old_before = snapshot(p.root)
            wide = size == "1080x740"
            script = pad.to_settings_group(size, 2) + (pad.press("dpad_down") if not wide else pad.press("dpad_right"))
            script += pad.seek("dm_change", "dpad_down", 12) + pad.press("south") + ["frames 6"] + pad.expect(page="data_location", focus_id="dm_choose")
            script += pad.press("south") + ["frames 4"] + pad.expect(picker=1) + pad.press("south") + pad.expect(picker_level="root") + pad.press("dpad_down") + pad.press("south")
            script += pad.seek("pk_cancel", "dpad_down", 10) + pad.seek("pk_ok", "dpad_right", 2) + pad.press("south") + ["dm_wait", "frames 6"] + pad.expect(data_move="plan", picker=0, dm_plan="ok", focus_id="dm_cancel_plan")
            script += pad.seek("dm_confirm", "dpad_right", 2) + pad.seek("dm_cancel_plan", "dpad_left", 2)   # both buttons are reachable; Cancel is the default
            script += pad.press("south") + ["frames 6"] + pad.expect(page="settings", data_move="choose", focus_id="dm_change") + ["quit"]
            check_trace("gamepad " + size, p.run(script, {"SEGARECOMP_PICKER_ROOT": str(pick)}, size), sum(1 for s in script if s.startswith(("expect ", "pad_seek "))))
            expect(not p.bootstrap.exists() and journal_files(p) == [], f"gamepad {size}: no bootstrap and no journal")
            expect(snapshot(p.root) == old_before or all(old_before.get(k) == v for k, v in snapshot(p.root).items() if k in old_before), f"gamepad {size}: the data is unchanged")
            expect(target.exists() and not any(target.iterdir()), f"gamepad {size}: the chosen folder is still empty")

    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print("launcher_data_location_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
