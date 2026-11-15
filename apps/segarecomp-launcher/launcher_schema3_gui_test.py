#!/usr/bin/env python3
"""SEG-143-T008: the launcher's local schema 3 flow for a NATIVE ABI 2 mod through the real launcher window and the product doubles (fake build driver and game). Synthetic identities only.

Exercises, through the GUI: choosing a schema 3 `.segapatch`; refusal of a package made for another game; the native-code approval sheet (no approval = nothing installed); installing; selecting the mod
(Play stays disabled until it is built); building the exact selected package (the build is asked for `--native-package` with the approved identity); playing it; switching to Vanilla (its program and record are
byte-identical to before); an unapproved record never builds; tampered installed bytes and a wrong-identity package never build; uninstalling restores Vanilla. The real `segarecomp mod pack` makes the packages.

usage: launcher_schema3_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game> <segarecomp-cli> <source root>
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


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




LAST_EXPECT = 0
OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]


def manifest_identity(package):
    with zipfile.ZipFile(package) as z:
        return sha(z.read("manifest.json"))


def main():
    exe, fake_cli, fake_game, real_cli, root = sys.argv[1:6]
    root = pathlib.Path(root)
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)
            print("FAIL:", message, flush=True)

    def check(name, trace, expected=None):
        expected = LAST_EXPECT if expected is None else expected
        failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        for l in trace:
            if l.startswith("expect FAIL"):
                print(f"FAIL {name}: {l}", flush=True)
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    fixtures = root / "tests/fixtures/mod_scenery"
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        p = Profile(tmp, "s3", exe, fake_cli, fake_game)
        src = p.source

        def package(name, image):
            project = tmp / name
            (project / "src").mkdir(parents=True)
            shutil.copy(fixtures / "provider.c", project / "src/provider.c")
            (project / "mod.toml").write_text((fixtures / "mod.toml").read_text().replace("@@SHA@@", sha(image)))
            out = tmp / (name + ".segapatch")
            r = subprocess.run([real_cli, "mod", "pack", str(project), "--out", str(out)], text=True, capture_output=True, timeout=300)
            assert r.returncode == 0 and json.loads(r.stdout)["ok"], r.stdout
            return out

        pkg = package("native", src)
        pkg_wrong = package("wrong", synthetic_rom(2))
        ident = manifest_identity(pkg)

        # 1. Vanilla first
        check("vanilla build", p.run(OPEN + ["build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", "expect variant=vanilla", "expect patches=0", "quit"]))
        vdir = p.builds()[0]
        vmeta = (vdir / "metadata.json").read_text()
        vexe = (vdir / ("game" + EXE_SUFFIX)).read_bytes()

        # 2. wrong-ROM package: refused with a readable message, nothing installed
        check("wrong package", p.run(OPEN + [f"patch_pick {pkg_wrong}", "frames 12", "expect modal=1", "expect patch_notice=This mod was", "key Escape", "frames 6", "expect patches=0", "quit"]))

        # 3. the native approval sheet: cancelling installs nothing; approving installs; installing builds and selects nothing
        trace = p.run(OPEN + [f"patch_pick {pkg}", "frames 12", "expect modal=1", "key Escape", "frames 6", "expect modal=0", "expect patches=0",
                              f"patch_pick {pkg}", "frames 12", "expect modal=1", "patch_confirm", "frames 20", "expect modal=0", "expect patches=1", "expect variant=vanilla",
                              "expect approved=-", "expect patch_built=0", "quit"])
        check("approve and install", trace)
        expect(not any("build=started" in l for l in trace), "installing must not start a build")
        stored = p.lib / "patches" / f"{ident}.segapatch"
        expect(stored.is_file() and sha(stored.read_bytes()) == sha(pkg.read_bytes()), "the stored copy is byte-for-byte the verified package")
        expect(p.rom.read_bytes() == src, "the original ROM file is never modified")

        # 4. select the mod: not playable until built; nothing falls back to Vanilla
        trace = p.run(OPEN + ["variant patched", "frames 30", "expect variant=patched", "expect approved=1", "expect status=Not built", "expect can_play=0", "play", "frames 10", "expect playing=0", "quit"])
        check("mod not built", trace)
        expect(not p.game_log.exists(), "Play must not start Vanilla while the mod is selected and unbuilt")

        # 5. build the exact selected package
        trace = p.run(OPEN + ["variant patched", "frames 10", "build", "wait_idle", "frames 40", "expect build_outcome=ok", "expect status=Ready", "expect vanilla_ready=1", "quit"])
        check("mod build", trace)
        dirs = p.builds()
        expect(len(dirs) == 2 and vdir in dirs, f"a second, separate build exists: {dirs}")
        mdir = next(x for x in dirs if x != vdir)
        mmeta = json.loads((mdir / "metadata.json").read_text())
        expect(mmeta.get("mod_kind") == "native" and mmeta.get("luau_identity") == ident and mmeta.get("rom_sha256") == sha(src), f"the record names the native package and the original game: {mmeta}")
        args = p.lines(p.args_log)[-1].split()
        expect("--native-package" in args and "--native-identity" in args and args[args.index("--native-identity") + 1] == ident and "--patch-package" not in args and "--luau-package" not in args, f"the build was asked for exactly the approved native package: {args}")
        expect(p.lines(p.rom_log) == [fnv(src), fnv(src)], "the mod build compiled the original image (the product derives the effective one)")
        expect((vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe and (vdir / "metadata.json").read_text() == vmeta, "the Vanilla program and record are untouched")

        # 6. play the mod, then Vanilla
        trace = p.run(OPEN + ["variant patched", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "expect variant=patched",
                              "variant vanilla", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "expect variant=vanilla", "quit"])
        check("play both", trace)
        started = p.lines(p.game_log)
        expect(len(started) == 2 and str(mdir) in started[0] and str(vdir) in started[1], f"the mod and then Vanilla were started from their own folders: {started}")
        envs = p.lines(p.env_log)
        expect(len(envs) == 2 and f"library={sha(src)}" in envs[0] and "library=-" in envs[1], f"the mod runs under its own per-game profile and Vanilla under the shared one: {envs}")

        # 7. an unapproved record never runs and never builds (the approval is the user's, per identity); restoring it plays again
        catalog = p.lib / "library.json"
        original_catalog = catalog.read_text()
        doc = json.loads(original_catalog)
        for e in doc.get("entries", doc.get("games", [])):
            for rec in e.get("patches", []):
                rec["native_approved"] = 0
        catalog.write_text(json.dumps(doc))
        games_before = p.lines(p.game_log)
        trace = p.run(OPEN + ["variant patched", "frames 10", "expect approved=0", "play", "frames 20", "frames 6", "expect playing=0", "quit"])
        check("unapproved play", trace)
        expect(p.lines(p.game_log) == games_before, "an unapproved native mod is never started, even when a program of it exists")
        catalog.write_text(original_catalog)

        # 8. tampered installed bytes never run: the stored copy no longer is the approved package
        good = stored.read_bytes()
        tampered = bytearray(good)
        tampered[len(tampered) // 2] ^= 0x55
        stored.write_bytes(bytes(tampered))
        trace = p.run(OPEN + ["variant patched", "frames 10", "play", "frames 20", "frames 6", "expect playing=0", "quit"])
        check("tampered play", trace)
        expect(p.lines(p.game_log) == games_before, "tampered installed bytes are never started")
        stored.write_bytes(good)
        trace = p.run(OPEN + ["variant patched", "frames 10", "play", "frames 20", "wait_play_end", "frames 6", "expect variant=patched", "quit"])
        check("restored play", trace)
        expect(len(p.lines(p.game_log)) == len(games_before) + 1, "the unaltered package plays again")

        # 9. uninstall restores Vanilla
        trace = p.run(OPEN + ["frames 6", "patch_remove 0", "frames 10", "patch_confirm", "frames 20", "expect patches=0", "expect variant=vanilla", "expect can_play=1", "quit"])
        check("uninstall", trace)
        expect(not stored.exists() and (vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe, "the package is gone and the Vanilla program is untouched")

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("launcher_schema3_gui_test: ok")
    return 0


sys.exit(main())
