#!/usr/bin/env python3
"""SEG-122-T009: automatic lock-on recognition through the real launcher (headless and window) and the product doubles.

Project-authored images only: pseudo-random bytes behind headers carrying the public identification fields. Checks the forwarded declaration and its absence, separate cache identities,
build metadata, headless reports and overrides, invalid platform/mapper combinations, persistence of manual choices, an entry imported before recognition existed, a missing ROM, native patch
independence, and the Advanced control with a gamepad at a small and a normal window size.

usage: launcher_lock_on_test.py <Segarecomp> <fake_segarecomp> <fake_game> <tools/segapatch.py> <provider.c> <headless|window|library|patch|gamepad>
"""
import importlib.util
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
SLOT = 0x200000
BASE = ("SONIC & KNUCKLES", "GM MK-1563 -00")
ATTACHED = ("SONIC THE             HEDGEHOG 3", "GM MK-1079 -00")
ORDINARY = ("FICTIONAL GAME", "GM 00000000-00")
spec = importlib.util.spec_from_file_location("segapatch", sys.argv[4])
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)
failures = []


def expect(condition, message):
    if not condition:
        failures.append(message)


def cartridge(identity, seed, size=SLOT):
    image = bytearray(size)
    x = seed
    for i in range(0, size, 4096):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        image[i] = (x >> 16) & 0xFF
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x120:0x150] = identity[0].ljust(48).encode()
    image[0x150:0x180] = identity[0].ljust(48).encode()
    image[0x180:0x18E] = identity[1].encode()
    image[0x1A0:0x1A8] = struct.pack(">II", 0, size - 1)
    data = bytes(image[0x200:])
    total = sum(struct.unpack(f">{len(data) // 2}H", data)) & 0xFFFF
    image[0x18E:0x190] = struct.pack(">H", total)
    return bytes(image)


def sms_rom():
    image = bytearray(0x8000)
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    return bytes(image)


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game, size="720x520"):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        package = self.dir / "package"
        for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                    "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                    "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
            path = package / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("x")
        (package / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy(fake_cli, package / "bin" / ("segarecomp" + EXE_SUFFIX))
        self.lib = self.dir / "lib"
        self.cache = self.dir / "cache"
        self.args_log = self.dir / "args.txt"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE=size, FAKE_GAME_BIN=fake_game,
                        FAKE_BUILD_ARGS_LOG=str(self.args_log))

    def write(self, name, data):
        path = self.dir / name
        path.write_bytes(data)
        return path

    def cli(self, *args):
        return subprocess.run([self.exe, *map(str, args)], env=self.env, timeout=120, capture_output=True, text=True)

    def builds(self):
        return self.args_log.read_text().splitlines() if self.args_log.exists() else []

    def catalog(self):
        return json.loads((self.lib / "library.json").read_text())

    def game(self):
        return self.catalog()["games"][0]

    def edit_game(self, **changes):
        data = self.catalog()
        for key, value in changes.items():
            if value is None:
                data["games"][0].pop(key, None)
            else:
                data["games"][0][key] = value
        (self.lib / "library.json").write_text(json.dumps(data))

    def run(self, name, script, expected=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        env = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path))
        subprocess.run([self.exe], env=env, timeout=240, check=True, capture_output=True)
        trace = trace_path.read_text().splitlines() if trace_path.exists() else []
        failures.extend(f"{name}: {line}" for line in trace if line.startswith("expect FAIL"))
        wanted = sum(1 for line in script if line.startswith(("expect ", "pad_seek "))) if expected is None else expected
        ok = sum(1 for line in trace if line.startswith("expect ok"))
        if ok != wanted:
            failures.append(f"{name}: {ok} of {wanted} expectations ran: {trace!r}")
        return trace


def report(done):
    return dict(line.split("=", 1) for line in done.stdout.splitlines() if "=" in line)


OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]
SETTLE = ["wait_idle", "frames 6"]  # the status refresh runs off the main thread; wait_idle holds until it answered


def headless(tmp, exe, fake_cli, fake_game):
    p = Profile(tmp, "headless", exe, fake_cli, fake_game)
    standalone = p.write("standalone.bin", cartridge(BASE, 1))
    combined = p.write("combined.bin", cartridge(BASE, 1) + cartridge(ATTACHED, 2))
    ordinary_2 = p.write("Sonic and Knuckles.bin", cartridge(ORDINARY, 3))     # the file name means nothing
    ordinary_4 = p.write("ordinary4.bin", cartridge(ORDINARY, 4, 2 * SLOT))
    modified = bytearray(cartridge(BASE, 1))
    modified[0x4000] ^= 0x55
    modified = p.write("modified.bin", bytes(modified))

    for name, path in (("standalone", standalone), ("combined", combined)):
        before = len(p.builds())
        done = p.cli("--build", path)
        r = report(done)
        expect(done.returncode == 0 and r.get("cartridge") == "lock_on" and r.get("cartridge_source") == "automatic", f"{name}: {done.stdout}")
        expect(len(p.builds()) == before + 1 and "--mapper lock_on" in p.builds()[-1], f"{name}: the declaration is forwarded: {p.builds()}")
        meta = json.loads((pathlib.Path(r["entry"]) / "metadata.json").read_text())
        expect(meta.get("mapper") == "lock_on" and meta.get("mapper_declaration") == "option", f"{name}: metadata records it: {meta}")
        again = report(p.cli("--build", path))
        expect(again.get("cache") == "hit" and again.get("entry") == r["entry"], f"{name}: the second build reuses its entry: {again}")

    locked = report(p.cli("--build", standalone))["entry"]
    # Explicit Standard: no flag at all, a separate identity, no record.
    before = len(p.builds())
    done = p.cli("--build", standalone, "--mapper", "standard")
    r = report(done)
    expect(done.returncode == 0 and r.get("cartridge") == "standard" and r.get("cartridge_source") == "explicit", f"explicit standard: {done.stdout}")
    expect(len(p.builds()) == before + 1 and "--mapper" not in p.builds()[-1], f"Standard is never forwarded: {p.builds()[-1:]}")
    expect(r.get("entry") != locked, "a standard build has its own cache identity")
    expect("mapper" not in json.loads((pathlib.Path(r["entry"]) / "metadata.json").read_text()), "standard metadata is what it always was")

    for name, path in (("ordinary 2 MiB", ordinary_2), ("ordinary 4 MiB", ordinary_4)):
        before = len(p.builds())
        done = p.cli("--build", path)
        r = report(done)
        expect(done.returncode == 0 and r.get("cartridge") == "standard" and "cartridge_note" not in r, f"{name}: {done.stdout}")
        expect(len(p.builds()) == before + 1 and "--mapper" not in p.builds()[-1], f"{name}: the historical command: {p.builds()[-1:]}")

    # Explicit Lock-on on an ordinary image reaches the product, which decides about the size.
    before = len(p.builds())
    done = p.cli("--build", ordinary_2, "--mapper", "lock_on")
    expect(report(done).get("cartridge_source") == "explicit" and "--mapper lock_on" in p.builds()[-1] and len(p.builds()) == before + 1, f"explicit lock-on: {done.stdout}")

    # An ambiguous image: ordinary behavior, the uncertainty stated, the override available.
    before = len(p.builds())
    done = p.cli("--build", modified)
    r = report(done)
    expect(r.get("cartridge") == "standard" and "modified" in r.get("cartridge_note", "") and "--mapper" not in p.builds()[-1], f"ambiguous: {done.stdout}")
    done = p.cli("--build", modified, "--mapper", "lock_on")
    r = report(done)
    expect(r.get("cartridge") == "lock_on" and "cartridge_note" not in r and "--mapper lock_on" in p.builds()[-1], f"ambiguous override: {done.stdout}")

    # Invalid combinations never reach a build.
    sms = p.write("g.sms", sms_rom())
    before = len(p.builds())
    for path, mapper, what in ((standalone, "sega", "Genesis"), (standalone, "bogus", "Genesis"), (sms, "lock_on", "Master System"), (sms, "standard", "Master System")):
        done = p.cli("--build", path, "--mapper", mapper)
        expect(done.returncode == 2 and "problem=--mapper" in done.stdout and what in done.stdout, f"{mapper} for {what}: rc={done.returncode} {done.stdout}")
    expect(len(p.builds()) == before, "an invalid declaration starts no build")
    done = p.cli("--build", sms, "--mapper", "sega")
    expect(done.returncode == 0 and report(done).get("mapper") == "sega" and "--mapper sega" in p.builds()[-1] and "cartridge" not in report(done), f"Master System declaration intact: {done.stdout}")

    # --mapper belongs to --build: it must not look as if it had been stored by an import.
    done = p.cli("--import", combined, "--mapper", "standard")
    expect(done.returncode == 2 and not (p.lib / "library.json").exists(), f"--import with --mapper: rc={done.returncode} {done.stdout}")

    # The window-side import uses the same recognition.
    done = p.cli("--import", combined)
    expect("import=added" in done.stdout and p.game().get("mapper") == "lock_on" and "mapper_explicit" not in p.game(), f"import: {done.stdout} {p.game()}")


def window_journey(tmp, exe, fake_cli, fake_game):
    p = Profile(tmp, "window", exe, fake_cli, fake_game)
    rom = p.write("game.bin", cartridge(BASE, 1) + cartridge(ATTACHED, 2))
    assert "import=added" in p.cli("--import", rom).stdout
    p.run("first build", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "expect hardware_changed=0", "quit"])
    expect(len(p.builds()) == 1 and "--mapper lock_on" in p.builds()[0], f"built correctly on the first attempt: {p.builds()}")
    games = lambda: sorted(d.name for d in (p.cache / "games").iterdir() if d.is_dir())
    locked = games()
    p.run("restart", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "expect status=Ready", "quit"])
    expect(len(p.builds()) == 1, "a restart builds nothing")

    # A manual Standard: the lock-on build is not reused, the reason is stated, the Standard build is separate.
    p.run("manual standard", OPEN + SETTLE + ["cartridge standard", "wait_idle", "frames 6", "expect cartridge=standard:manual", "expect can_play=0", "expect hardware_changed=1",
                                              "build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "quit"])
    expect(len(p.builds()) == 2 and "--mapper" not in p.builds()[1] and games() != locked, f"the Standard build has its own identity: {p.builds()} {games()} {locked}")
    # It survives a reimport, a restart and a rescan.
    done = p.cli("--import", rom)
    expect(p.game().get("mapper_explicit") is True and p.game().get("mapper") == "", f"reimport keeps a manual Standard: {done.stdout} {p.game()}")
    p.run("manual standard restart", OPEN + SETTLE + ["scan", "wait_scan", "frames 20", "expect cartridge=standard:manual", "expect status=Ready", "quit"])
    # Back to Automatic: recognition again, and (the default setting removed the older folder at the recompile) a build that is not for this hardware does not count.
    p.run("back to auto", OPEN + SETTLE + ["cartridge auto", "wait_idle", "frames 6", "expect cartridge=lock_on:auto", "expect can_play=0", "expect hardware_changed=1",
                                           "build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "quit"])
    expect(len(p.builds()) == 3 and "--mapper lock_on" in p.builds()[2] and "mapper_explicit" not in p.game() and games() == locked, f"Automatic: same identity as the first build: {p.builds()} {p.game()} {games()}")
    # A manual Lock-on is the same identity as the automatic one (a choice is not hardware), and it survives a reimport.
    p.run("manual lock-on", OPEN + SETTLE + ["cartridge lock_on", "wait_idle", "frames 6", "expect cartridge=lock_on:manual", "expect status=Ready", "quit"])
    done = p.cli("--import", rom)
    expect(p.game().get("mapper_explicit") is True and p.game().get("mapper") == "lock_on", f"reimport keeps a manual Lock-on: {p.game()}")
    expect(len(p.builds()) == 3, f"no rebuild: {p.builds()}")


def older_library(tmp, exe, fake_cli, fake_game):
    # An entry imported before this change, with a standard build: recognition finds it, the old build stops counting, and it says why.
    p = Profile(tmp, "older", exe, fake_cli, fake_game)
    rom = p.write("game.bin", cartridge(BASE, 1))
    assert "import=added" in p.cli("--import", rom).stdout
    p.edit_game(mapper="", mapper_explicit=True)
    p.run("standard build", OPEN + SETTLE + ["build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "quit"])
    expect(len(p.builds()) == 1 and "--mapper" not in p.builds()[0], f"standard build: {p.builds()}")
    p.edit_game(mapper_explicit=None)     # exactly the record an earlier release wrote
    untouched = {k: v for k, v in p.game().items() if k != "mapper"}
    p.run("older entry", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "expect can_play=0", "expect hardware_changed=1", "quit"])
    game = p.game()
    expect(game.get("mapper") == "lock_on", f"the recognition is saved: {game}")
    expect({k: v for k, v in game.items() if k != "mapper"} == untouched, f"no other setting moved: {untouched} -> {game}")
    p.run("rebuild", OPEN + SETTLE + ["build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "expect hardware_changed=0", "quit"])
    expect(len(p.builds()) == 2 and "--mapper lock_on" in p.builds()[1], f"rebuild: {p.builds()}")

    # A missing ROM keeps what is stored, whichever it is, and the build still reads as its own.
    rom.unlink()
    p.run("missing lock-on", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "expect status=Ready", "quit"])
    p.edit_game(mapper="")
    p.run("missing standard", OPEN + SETTLE + ["expect cartridge=standard:auto", "quit"])
    expect(p.game().get("mapper") == "", "a missing ROM never invents a declaration")
    # The ROM comes back at the same path: the next session recognizes it.
    p.write("game.bin", cartridge(BASE, 1))
    p.run("rom back", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "expect status=Ready", "quit"])
    # A different file at the recorded location is not the game.
    p.edit_game(mapper="")
    p.write("game.bin", cartridge(ORDINARY, 9))
    p.run("changed file", OPEN + SETTLE + ["expect cartridge=standard:auto", "quit"])
    p.write("game.bin", cartridge(BASE, 1))
    p.edit_game(mapper="lock_on")
    p.write("game.bin", cartridge(ORDINARY, 9))
    p.run("changed file keeps", OPEN + SETTLE + ["expect cartridge=lock_on:auto", "quit"])

    # An ordinary game: same command, nothing added to its record, and no status text about hardware.
    q = Profile(tmp, "ordinary", exe, fake_cli, fake_game)
    assert "import=added" in q.cli("--import", q.write("o.bin", cartridge(ORDINARY, 3))).stdout
    before = q.game()
    q.run("ordinary", OPEN + SETTLE + ["expect cartridge=standard:auto", "build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "expect status=Ready", "quit"])
    expect(len(q.builds()) == 1 and "--mapper" not in q.builds()[0], f"ordinary command: {q.builds()}")
    after = q.game()
    expect("mapper_explicit" not in after and after.get("mapper") == "" and after.get("aot_policy") == before.get("aot_policy"), f"ordinary record: {after}")


def native_patch(tmp, exe, fake_cli, fake_game):
    # The Sonic 3 & Knuckles journey with the doubles (the real one is evidence on a local image): import, install and approve a native patch, select it, build. No Advanced options opened.
    p = Profile(tmp, "patch", exe, fake_cli, fake_game, size="900x900")
    source = cartridge(BASE, 1) + cartridge(ATTACHED, 2)
    rom = p.write("game.bin", source)
    assert "import=added" in p.cli("--import", rom).stdout
    provider = pathlib.Path(sys.argv[5]).read_bytes()
    package = tmp / "native.segapatch"
    sp.create(source, None, package, id_="synthetic-native", name="Synthetic Native", version="1.0.0", width_max=100, native_source=provider,
              native_caps=["frame.work_ram", "frame.vdp", "scenery.words", "overlay.rgba"], author="Test Author")
    p.run("install", OPEN + SETTLE + [f"patch_pick {package}", "frames 12", "expect modal=1", "patch_confirm", "frames 20", "expect patches=1", "expect cartridge=lock_on:auto", "quit"])
    p.run("select and build", OPEN + SETTLE + ["variant patched", "wait_idle", "frames 6", "expect variant=patched", "expect cartridge=lock_on:auto", "build", "wait_idle", "wait_idle", "frames 6", "expect build_outcome=ok", "quit"])
    builds = p.builds()
    expect(len(builds) == 1 and "--mapper lock_on" in builds[0] and "--patch-package" in builds[0], f"the patched build carries the declaration: {builds}")
    # A manual choice is independent of the variant too, and removing the patch moves nothing.
    p.run("override with patch", OPEN + SETTLE + ["cartridge standard", "wait_idle", "frames 6", "expect cartridge=standard:manual", "variant vanilla", "wait_idle", "frames 6", "expect cartridge=standard:manual", "cartridge auto", "wait_idle", "frames 6", "expect cartridge=lock_on:auto", "quit"])
    p.run("remove patch", OPEN + SETTLE + ["patch_remove 0", "frames 12", "patch_confirm", "wait_idle", "frames 6", "expect patches=0", "expect cartridge=lock_on:auto", "quit"])
    expect(p.game().get("mapper") == "lock_on", "a patch never changes the cartridge hardware")


def gamepad_control(tmp, exe, fake_cli, fake_game):
    for size in ("720x520", "1080x740"):
        p = Profile(tmp, "pad" + size, exe, fake_cli, fake_game, size=size)
        assert "import=added" in p.cli("--import", p.write("game.bin", cartridge(BASE, 1))).stdout
        press = lambda button: [f"pad {button} down", f"pad {button} up", "frames 6"]
        # D-pad through the expanded tiles: Standard, Lock-on, back to Automatic, each a real gamepad press.
        steps = ["frames 10", "pad_attach", "frames 20", "focus_card 0", "frames 4", "key Return", "frames 8"] + SETTLE + ["advanced 1", "frames 6"] + press("dpad_right")
        steps += ["pad_seek cartridge#0 dpad_down 40"] + press("dpad_right") + ["expect focus_id=cartridge#1"] + press("south") + ["wait_idle", "frames 6", "expect cartridge=standard:manual", "quit"]
        p.run("pad standard " + size, steps)
        expect(p.game().get("mapper_explicit") is True and p.game().get("mapper") == "", f"{size}: the real control saved a manual Standard: {p.game()}")
        steps = ["frames 10", "pad_attach", "frames 20", "focus_card 0", "frames 4", "key Return", "frames 8"] + SETTLE + ["advanced 1", "frames 6"] + press("dpad_right")
        steps += ["pad_seek cartridge#0 dpad_down 40"] + press("dpad_right") + press("dpad_right") + ["expect focus_id=cartridge#2"] + press("south") + ["wait_idle", "frames 6", "expect cartridge=lock_on:manual"]
        steps += press("dpad_left") + press("dpad_left") + ["expect focus_id=cartridge#0"] + press("south") + ["wait_idle", "frames 6", "expect cartridge=lock_on:auto", "quit"]
        p.run("pad select " + size, steps)
        expect("mapper_explicit" not in p.game(), f"{size}: Automatic is stored as no explicit choice: {p.game()}")


PARTS = {"headless": headless, "window": window_journey, "library": older_library, "patch": native_patch, "gamepad": gamepad_control}


def main():
    exe, fake_cli, fake_game, part = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[6]
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        PARTS[part](tmp, exe, fake_cli, fake_game)
    if failures:
        print("\n".join(failures))
        return 1
    print(f"launcher_lock_on_test {part}: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
