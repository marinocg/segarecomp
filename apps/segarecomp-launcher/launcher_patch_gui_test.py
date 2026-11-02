#!/usr/bin/env python3
"""SEG-122-T001: Vanilla / Patched variants of one library game through the real launcher window and the product doubles.

Synthetic game identities only. Exercises: choosing a .segapatch (the file chooser's result), verification and the install sheet, refusal of a package made for
another game, selecting Patched without a build (Play stays disabled; nothing falls back to Vanilla), the separate patched build from the verified patched
image, distinct build identities, launching each variant with the right identities, the unchanged Vanilla build and executable, persistence across restarts,
cancel and failed rebuilds, a second package, removal of a patch (with and without deleting its build), a missing package file and a tampered build record.

usage: launcher_patch_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game> <tools/segapatch.py>
"""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
spec = importlib.util.spec_from_file_location("segapatch", sys.argv[4])
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)


def make_package_root(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(fake_cli, root / "bin" / ("segarecomp" + EXE_SUFFIX))


def synthetic_rom(seed, size=0x4000):
    image = bytearray(size)
    x = seed
    for i in range(0x200, size):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        image[i] = (x >> 16) & 0xFF
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x150:0x170] = b"SYNTHETIC PATCH TEST GAME       "
    return bytes(image)


def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{len(data)} {h:016x}"


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package_root(self.package, fake_cli)
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.game_log, self.env_log, self.rom_log, self.args_log = (self.dir / n for n in ("game.log", "env.log", "rom.log", "args.log"))
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE="900x900",
                        FAKE_GAME_BIN=fake_game, FAKE_GAME_LOG=str(self.game_log), FAKE_GAME_ENV_LOG=str(self.env_log), FAKE_ROM_LOG=str(self.rom_log),
                        FAKE_BUILD_ARGS_LOG=str(self.args_log))
        self.source = synthetic_rom(1)
        self.rom = self.dir / "game.md"
        self.rom.write_bytes(self.source)
        done = subprocess.run([exe, "--import", str(self.rom)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def builds(self):
        games = self.cache / "games"
        return sorted(p for p in games.iterdir() if p.is_dir() and not p.name.endswith((".partial", ".failed"))) if games.exists() else []

    def lines(self, path):
        return path.read_text().splitlines() if path.exists() else []

    def run(self, script, extra_env=None):
        global LAST_EXPECT
        LAST_EXPECT = sum(1 for line in script if line.startswith("expect "))
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=240, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []


LAST_EXPECT = 0
OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]


def settings_snapshot(lib):
    """Every viewer preference file of the library (global file and per-game profiles) with its digest."""
    out = {}
    for path in [lib / "viewer-settings.cfg", *sorted((lib / "viewer-widescreen").glob("*"))]:
        if path.is_file():
            out[path.name] = sha(path.read_bytes())
    return out


def manifest_identity(package):
    with zipfile.ZipFile(package) as z:
        return sha(z.read("manifest.json"))


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    def check(name, trace, expected=None):
        expected = LAST_EXPECT if expected is None else expected
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        p = Profile(tmp, "variants", exe, fake_cli, fake_game)
        src = p.source
        patched = bytearray(src)
        patched[0x1000:0x1008] = b"PATCHED!"
        patched[0x2000:0x2004] = b"\xde\xad\xbe\xef"
        patched = bytes(patched)
        pkg_a = tmp / "a-widescreen.segapatch"
        sp.create(src, patched, pkg_a, id_="synthetic-wide", name="Synthetic Wide", version="1.0.0", width_max=100,
                  presentation_defaults={"widescreen": "enhanced_gameplay", "side_style": "raw"})
        patched_b = bytearray(src)
        patched_b[0x1000:0x1008] = b"PATCHED2"
        pkg_b = tmp / "b-widescreen.segapatch"
        sp.create(src, bytes(patched_b), pkg_b, id_="synthetic-wide", name="Synthetic Wide", version="2.0.0", width_max=60,
                  presentation_defaults={"widescreen": "extended_backgrounds", "side_style": "borders"})
        other = bytearray(synthetic_rom(2))
        pkg_other = tmp / "other-game.segapatch"
        sp.create(bytes(other), bytes(other[:0x300]) + b"X" + bytes(other[0x301:]), pkg_other, id_="other", name="Other", version="1.0.0")
        ident_a, ident_b = manifest_identity(pkg_a), manifest_identity(pkg_b)

        # the user's existing preferences (global file and this game's profile) must survive everything below byte for byte
        (p.lib).mkdir(parents=True, exist_ok=True)
        (p.lib / "viewer-settings.cfg").write_text("video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=comfort\nvideo.widescreen_width=77\n")
        (p.lib / "viewer-widescreen").mkdir(exist_ok=True)
        (p.lib / "viewer-widescreen" / f"genesis-{sha(src)}.cfg").write_text("video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=borders\nvideo.widescreen_width=55\n")
        prefs_before = settings_snapshot(p.lib)

        # 1. Vanilla first: a normal build, untouched by everything below.
        check("vanilla build", p.run(OPEN + ["build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", "expect variant=vanilla", "expect patches=0", "quit"]))
        vanilla_dirs = p.builds()
        expect(len(vanilla_dirs) == 1, f"one vanilla build: {vanilla_dirs}")
        vdir = vanilla_dirs[0]
        vmeta = json.loads((vdir / "metadata.json").read_text())
        expect("patch_identity" not in vmeta, f"a Vanilla build records no variant: {vmeta}")
        vexe_before = (vdir / ("game" + EXE_SUFFIX)).read_bytes()
        expect(p.lines(p.rom_log) == [fnv(src)], f"the vanilla build compiled the original image: {p.lines(p.rom_log)}")
        expect(not (vdir / "input").exists(), "no materialized image is left in a build")

        # 2. A package made for another game is refused with a readable message; nothing is installed.
        trace = p.run(OPEN + [f"patch_pick {pkg_other}", "frames 12", "expect modal=1", "expect patch_notice=This patch w", "key Escape", "frames 6", "expect patches=0", "quit"])
        check("other game's package", trace)

        # 3. The right package: preview sheet, then install. Nothing is built or selected by installing.
        trace = p.run(OPEN + [f"patch_pick {pkg_a}", "frames 12", "expect modal=1", "key Escape", "frames 6", "expect modal=0", "expect patches=0",
                              f"patch_pick {pkg_a}", "frames 12", "expect modal=1", "patch_confirm", "frames 20", "expect modal=0", "expect patches=1", "expect variant=vanilla",
                              "expect patch_notice=Patch instal", "expect status=Ready", "expect patch_built=0", "expect vanilla_ready=1", "quit"])
        check("install", trace)
        expect(not any("build=started" in l for l in trace), "installing must not start a build")
        expect((p.lib / "patches" / f"{ident_a}.segapatch").is_file(), "the package is stored under its identity")
        expect(p.rom.read_bytes() == src, "the original ROM file is never modified")
        expect(not any(p.lib.glob("patches/*.partial")), "no partial package left")

        # 4. Select Patched without a build: not playable, nothing falls back to Vanilla.
        trace = p.run(OPEN + ["variant patched", "frames 30", "expect variant=patched", "expect status=Not built", "expect can_play=0", "play", "frames 10", "expect playing=0", "quit"])
        check("patched not built", trace)
        expect(not p.game_log.exists(), "Play must not start Vanilla while Patched is selected and unbuilt")
        expect(any("variant=patched" in l for l in trace), f"selection traced: {trace!r}")

        # 5. Build the patched version: a separate program from the verified patched image; Vanilla is untouched.
        trace = p.run(OPEN + ["variant patched", "frames 10", "build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", "expect patch_built=1", "expect vanilla_ready=1", "quit"])
        check("patched build", trace)
        expect(any("build=started" in l and "variant=patched" in l for l in trace), f"the build was a patched one: {trace!r}")
        dirs = p.builds()
        expect(len(dirs) == 2 and vdir in dirs, f"a second, separate build exists: {dirs}")
        pdir = next(x for x in dirs if x != vdir)
        pmeta = json.loads((pdir / "metadata.json").read_text())
        expect(pmeta.get("patch_identity") == ident_a and pmeta.get("patch_target_sha256") == sha(patched), f"patched metadata names its variant: {pmeta}")
        expect(pmeta.get("rom_sha256") == sha(src), "the library identity stays the original game")
        expect(p.lines(p.rom_log) == [fnv(src), fnv(patched)], f"the patched build compiled exactly the verified patched image: {p.lines(p.rom_log)}")
        expect(not (pdir / "input").exists() and not any(pdir.rglob("patched.rom")), "the materialized patched image is removed after the build")
        expect((vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe_before and json.loads((vdir / "metadata.json").read_text()) == vmeta, "the Vanilla program and record are untouched")
        expect(p.rom.read_bytes() == src, "the original ROM file is still unmodified")
        rom_arg = p.lines(p.args_log)[-1].split("--rom")[1].split()[0]
        expect(rom_arg.endswith(os.path.join("input", "patched.rom")) and rom_arg != str(p.rom), f"the build read the patched image from its own partial folder: {rom_arg}")

        # 6. Play each variant; the program gets the right identities.
        trace = p.run(OPEN + ["variant patched", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "expect variant=patched",
                              "variant vanilla", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "expect variant=vanilla", "quit"])
        check("play both", trace)
        started = p.lines(p.game_log)
        expect(len(started) == 2 and str(pdir) in started[0] and str(vdir) in started[1], f"Patched then Vanilla were started from their own folders: {started}")
        env_lines = p.lines(p.env_log)
        expect(len(env_lines) == 2, f"two launches logged: {env_lines}")
        if len(env_lines) == 2:
            expect(f"rom={sha(patched)} library={sha(src)}" in env_lines[0] and "(Patched: Synthetic Wide 1.0.0)" in env_lines[0], f"patched launch identities: {env_lines[0]}")
            expect(f"patch={ident_a} pws=enhanced_gameplay pside=raw" in env_lines[0], f"the verified patch's presentation defaults reach the viewer: {env_lines[0]}")
            expect(f"rom={sha(src)} library=-" in env_lines[1] and "(Patched" not in env_lines[1], f"vanilla launch identities: {env_lines[1]}")
            expect("patch=- pws=- pside=-" in env_lines[1], f"Vanilla gets no patch environment at all: {env_lines[1]}")
        expect(settings_snapshot(p.lib) == prefs_before, "installing, selecting, building, launching a patch never rewrites the global or the original game's preference files")

        # 6b. The Play button's dropdown lists both versions (and offers a build for an unbuilt one).
        trace = p.run(OPEN + ["variant vanilla", "frames 10", "open_play_menu", "frames 12", "menu_items", "quit"])
        items = next((l for l in trace if l.startswith("menu_items")), "")
        expect("Play Vanilla" in items and "Play Patched" in items, f"the Play dropdown names both versions: {items}")

        # 7. Persistence: the selection survives a restart.
        check("select patched", p.run(OPEN + ["variant patched", "frames 10", "quit"]))
        trace = p.run(OPEN + ["frames 20", "expect variant=patched", "expect patches=1", "expect status=Ready", "expect can_play=1", "quit"])
        check("restart persistence", trace)

        # 8. A cancelled rebuild of Patched keeps the published patched build and Vanilla.
        heartbeat = p.dir / "heartbeat"
        trace = p.run(OPEN + ["frames 20", "build", f"wait_file {heartbeat}.started", "cancel_build", "wait_idle", "frames 20", "expect build_outcome=cancelled", "expect status=Ready", "expect can_play=1", "quit"],
                      {"FAKE_BUILD_HOLD": str(heartbeat)})
        check("cancelled patched rebuild", trace)
        expect((pdir / ("game" + EXE_SUFFIX)).is_file() and (vdir / ("game" + EXE_SUFFIX)).is_file(), "both published programs survive a cancelled rebuild")
        expect(not any(p.cache.glob("games/*.partial")), "the cancelled attempt's folder is gone")
        # a failed rebuild too
        trace = p.run(OPEN + ["frames 20", "build", "wait_idle", "frames 20", "expect build_outcome=failed", "expect can_play=1", "quit"], {"FAKE_BUILD_FAIL": "optimized"})
        check("failed patched rebuild", trace)
        expect((vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe_before, "a failed patched rebuild never touches Vanilla")

        # 9. A second package of the same game: its own identity, its own (unbuilt) variant; the first one's build is not reused for it.
        trace = p.run(OPEN + [f"patch_pick {pkg_b}", "frames 12", "expect modal=1", "patch_confirm", "frames 20", "expect patches=2", "variant patched=1", "frames 30", "expect variant=patched", "expect status=Not built", "expect can_play=0", "quit"])
        check("second package", trace)
        expect(len(p.builds()) == 2, "the second package did not reuse another package's build")
        trace = p.run(OPEN + ["variant patched=1", "frames 10", "build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", "quit"])
        check("second package build", trace)
        expect(len(p.builds()) == 3 and p.lines(p.rom_log)[-1] == fnv(bytes(patched_b)), f"third build compiled package B's image: {p.lines(p.rom_log)[-1:]}")

        # 10. Removing a patch: Vanilla, the ROM and the other patch stay; the removed patch's build is deleted only on request.
        trace = p.run(OPEN + ["patch_remove 1", "frames 6", "expect modal=1", "patch_confirm", "frames 30", "expect patches=1", "expect variant=vanilla", "expect status=Ready", "expect vanilla_ready=1", "quit"])
        check("remove without deleting the build", trace)
        expect(len(p.builds()) == 3, "removing a patch does not delete its build unless asked")
        expect(not (p.lib / "patches" / f"{ident_b}.segapatch").exists() and (p.lib / "patches" / f"{ident_a}.segapatch").is_file(), "only the removed package file is deleted")
        trace = p.run(OPEN + ["patch_remove 0", "frames 6", "expect modal=1", "patch_confirm delete", "frames 40", "expect patches=0", "expect variant=vanilla", "expect status=Ready", "quit"])
        check("remove deleting the build", trace)
        remaining = p.builds()
        expect(pdir not in remaining and vdir in remaining, f"the patched build of the removed patch is deleted and Vanilla stays: {remaining}")
        expect(p.rom.read_bytes() == src and (vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe_before, "the ROM and the Vanilla program are untouched by removals")
        trace = p.run(OPEN + ["play", "frames 20", "wait_play_end", "frames 6", "quit"])
        expect(len(p.lines(p.game_log)) == 3 and str(vdir) in p.lines(p.game_log)[-1], "Vanilla still plays after every patch is removed")

        expect(settings_snapshot(p.lib) == prefs_before, "removing patches (with and without their builds) never rewrites the global or the original game's preference files")

        # 11. A missing package file: building Patched is refused with a reason; nothing else is started.
        trace = p.run(OPEN + [f"patch_pick {pkg_a}", "frames 12", "patch_confirm", "frames 20", "expect patches=1", "variant patched", "frames 10", "quit"])
        check("reinstall", trace)
        (p.lib / "patches" / f"{ident_a}.segapatch").unlink()
        starts = len(p.lines(p.args_log))
        trace = p.run(OPEN + ["variant patched", "frames 10", "build", "frames 20", "expect modal=1", "wait_idle", "quit"])
        check("missing package", trace)
        expect(len(p.lines(p.args_log)) == starts, "no build was started without the installed package")

    # 12. A build record cannot masquerade as another variant (tampered metadata).
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        q = Profile(tmp, "tamper", exe, fake_cli, fake_game)
        patched2 = bytearray(q.source)
        patched2[0x1000:0x1004] = b"WIDE"
        pkg = tmp / "t.segapatch"
        sp.create(q.source, bytes(patched2), pkg, id_="t", name="T", version="1")
        check("tamper setup", q.run(OPEN + ["build", "wait_idle", "frames 10", f"patch_pick {pkg}", "frames 12", "patch_confirm", "frames 20", "variant patched", "frames 10", "build", "wait_idle", "frames 30", "expect status=Ready", "quit"]))
        q.run(OPEN + ["variant patched", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "quit"])
        expect(any("title=" in l and "(Patched: T 1)" in l and "patch=- pws=- pside=-" in l for l in q.lines(q.env_log)), f"a patch that states no presentation defaults sets no override: {q.lines(q.env_log)}")
        a_dir, b_dir = q.builds()
        pat = next(x for x in (a_dir, b_dir) if "patch_identity" in json.loads((x / "metadata.json").read_text()))
        van = a_dir if pat == b_dir else b_dir
        m = json.loads((pat / "metadata.json").read_text())
        m.pop("patch_identity")
        (pat / "metadata.json").write_text(json.dumps(m))
        trace = q.run(OPEN + ["frames 30", "expect variant=patched", "expect can_play=0", "quit"])
        check("patched record stripped of its variant", trace)
        m["patch_identity"] = "0" * 64
        (pat / "metadata.json").write_text(json.dumps(m))
        check("patched record naming another package", q.run(OPEN + ["frames 30", "expect can_play=0", "quit"]))
        mv = json.loads((van / "metadata.json").read_text())
        mv["patch_identity"] = "1" * 64
        (van / "metadata.json").write_text(json.dumps(mv))
        trace = q.run(OPEN + ["variant vanilla", "frames 30", "expect variant=vanilla", "expect can_play=0", "quit"])
        check("vanilla record claiming a variant", trace)

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_patch_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
