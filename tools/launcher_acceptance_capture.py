#!/usr/bin/env python3
"""Native acceptance captures of the real launcher on synthetic profiles (SEG-150-T010). Requires Pillow, a built launcher and its test doubles.

Every profile is built from project-authored synthetic files (512-byte Genesis headers, a generated Master System image, a generated lock-on-shaped image, the project's Sega CD fixture disc,
BIOS doubles) and the product / game doubles (`fake_segarecomp`, `fake_game`, `fake_scd_program`); nothing is built from, or compared with, a commercial image, and no guest program is executed
except the game double. Each state is reached through the GUI script seam (`SEGARECOMP_GUI_SCRIPT`, `shot <file>`), at the three review sizes, with SDL's dummy video driver and software renderer
(a layout test, not a GPU or physical-display result).

usage: launcher_acceptance_capture.py --launcher <Segarecomp> --fake-cli <fake_segarecomp> --fake-game <fake_game> --fake-scd <fake_scd_program> --work <scratch dir> --out <dir> [--sizes 1080x740,720x520,1600x900] <group>...
groups: main (library, game pages: needs the `setup` group once), setup, settings, fresh, addprep, errors, scd, lockon, data
"""
import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
C = argparse.Namespace()   # launcher / double paths, work and output directories, sizes (set in main)


def make_package(root):
    for rel in ("toolchain/zig", "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c", "runtime/platforms/genesis/runtime/vdp_render.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/platforms/sega-cd/runtime/scd_main.c", "runtime/platforms/sega-cd/viewer/scd_viewer.c",
                "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c", "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(C.fake_cli, root / "bin" / "segarecomp")


def genesis_rom(path, title, seed):
    img = bytearray(0x200)
    img[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    img[0x100:0x110] = b"SEGA GENESIS    "
    name = title.upper().encode("ascii", "replace")[:47].ljust(48)
    img[0x120:0x150] = name
    img[0x150:0x180] = name
    img[0x1F0:0x200] = b"U               "
    img[0x1E0], img[0x1E1] = seed & 0xFF, seed >> 8
    path.write_bytes(bytes(img))


def sms_rom(path, seed):
    img = bytearray(0x8000)
    for i in range(len(img)):
        img[i] = (i * 31 + seed) & 0xFF
    img[0x7FF0:0x7FF8] = b"TMR SEGA"
    img[0x7FFF] = 0x4C
    path.write_bytes(bytes(img))


def lock_on_rom(path):
    slot = 0x200000

    def cartridge(identity, seed):
        img, x = bytearray(slot), seed
        for i in range(0, slot, 4096):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
            img[i] = (x >> 16) & 0xFF
        img[:12] = bytes((0, 0xFF, 0, 4, 0, 0, 0, 8, 0x70, 0, 0x4E, 0x70))
        img[0x100:0x110] = b"SEGA GENESIS    "
        img[0x120:0x150] = img[0x150:0x180] = identity[0].ljust(48).encode()
        img[0x180:0x18E] = identity[1].encode()
        img[0x1A0:0x1A8] = struct.pack(">II", 0, slot - 1)
        data = bytes(img[0x200:])
        img[0x18E:0x190] = struct.pack(">H", sum(struct.unpack(f">{len(data) // 2}H", data)) & 0xFFFF)
        return bytes(img)
    path.write_bytes(cartridge(("SONIC & KNUCKLES", "GM MK-1563 -00"), 1) + cartridge(("SONIC THE             HEDGEHOG 3", "GM MK-1079 -00"), 2))


class Prof:
    """One profile: a package double, library, cache and an environment; `run` drives the window with a script and returns its trace."""

    def __init__(self, name, fresh=True):
        self.dir = C.work / name
        if fresh and self.dir.exists():
            shutil.rmtree(self.dir)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.package = self.dir / "package"
        if fresh or not self.package.exists():
            make_package(self.package)
        self.roms = self.dir / "roms"
        self.roms.mkdir(exist_ok=True)
        self.lib, self.cache = self.dir / "lib", self.dir / "cache"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache), SEGARECOMP_LIBRARY_DIR=str(self.lib),
                        SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=C.fake_game, FAKE_GAME_LOG=str(self.dir / "game.log"),
                        SEGARECOMP_VIEWER_SETTINGS=str(self.dir / "viewer.cfg"))
        self.env.pop("SEGARECOMP_GUI_ONBOARDING", None)

    def add(self, kind, title, seed, fname=None):
        path = self.roms / (fname or (title + (".sms" if kind == "sms" else ".md")))
        sms_rom(path, seed) if kind == "sms" else genesis_rom(path, title, seed)
        self.import_path(path)
        return path

    def import_path(self, path):
        done = subprocess.run([C.launcher, "--import", str(path)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def run(self, script, size="1080x740", extra=None, name="s"):
        sp, tp = self.dir / (name + ".txt"), self.dir / (name + ".trace")
        sp.write_text("\n".join(script) + "\n")
        tp.unlink(missing_ok=True)
        env = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(sp), SEGARECOMP_GUI_TRACE=str(tp), SEGARECOMP_SHOT_SIZE=size, **(extra or {}))
        subprocess.run([C.launcher], env=env, timeout=300, check=True, capture_output=True)
        trace = tp.read_text().splitlines() if tp.exists() else []
        bad = [line for line in trace if "FAIL" in line]
        if bad:
            print(f"{size} {name}: {bad}")
        return trace

    def catalog(self):
        return json.loads((self.lib / "library.json").read_text())

    def save_catalog(self, data):
        (self.lib / "library.json").write_text(json.dumps(data))

    def update_product(self):   # a new release: every earlier build becomes an older version
        with open(self.package / "bin" / "segarecomp", "ab") as f:
            f.write(b"\0release-update")


def shot(size, name, frames=4):
    d = C.out / size
    d.mkdir(parents=True, exist_ok=True)
    return [f"shot {d / (name + '.bmp')}", f"frames {frames}"]


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def search(text):
    return ["frames 10", "key F", "frames 4", f"text {text}", "frames 8", "key Return", "frames 6"]


def open_card(index):
    return ["frames 10", f"focus_card {index}", "frames 4", "key Return", "frames 10", "wait_idle", "frames 4"]


def open_by_search(text):
    return search(text) + ["focus_card 0", "frames 4", "key Return", "frames 10", "wait_idle", "frames 4"]


def group_setup():
    """The main profile: ready, older-version, failed, never-prepared, missing-file, undeclared-mapper and non-ASCII games."""
    p = Prof("main")
    for title, seed in (("Fictional Quest", 11), ("Neon Harbor Demo", 23), ("Pixel Orchard", 37), ("Broken Vector", 41), ("Summit Chase", 43)):
        p.add("md", title, seed)
    p.add("md", "Unicode placeholder", 47, "uni.md")
    for i, title in enumerate(("Harbor Lights", "Night Relay", "Glass Garden", "Copper Bay", "Tidal Arena", "Lantern Run")):
        p.add("md", title, 60 + i)
    p.add("sms", "Coastal Drift", 5)
    p.add("sms", "Mystery Cart", 17)
    data = p.catalog()
    for g in data["games"]:
        if g["detected_title"].startswith("UNICODE"):
            g["display_title"] = "Ünïcode Ärcade – Épisode Deux – Ælfric's Extraordinarily Long Display Name Edition"
        elif g["platform"] == "genesis":
            g["display_title"] = g["detected_title"].title()
        elif "coastal" in g["detected_title"].lower():
            g["mapper"] = "sega"   # the header never names it: declared here, as the user would
    p.save_catalog(data)
    titles = [g.get("display_title") or g["detected_title"] for g in p.catalog()["games"]]

    def build(title):
        return ["main_action " + title, "wait_idle", "frames 40"]
    t = lambda s: next(x for x in titles if s.lower() in x.lower())
    p.run(["frames 10"] + build(t("Neon")) + ["quit"], name="b1")
    p.update_product()
    p.run(["frames 10"] + build(t("Fictional")) + build(t("Pixel")) + build(t("Harbor Lights")) + build(t("Coastal")) + build(t("Ünï")) + ["quit"], name="b2")
    p.run(["frames 10"] + build(t("Broken")) + ["quit"], extra={"FAKE_BUILD_FAIL": "optimized,compatibility"}, name="b3")
    (p.roms / "Summit Chase.md").unlink()


def group_main():
    p = Prof("main", fresh=False)
    for size in C.sizes:
        S = lambda n: shot(size, n)
        scen = {
            "03_library_grid": ["frames 14"] + S("03_library_grid"),
            "03_library_list": ["frames 10", "view list", "frames 8"] + S("03_library_list"),
            "lib_filter_sms": ["frames 10", "platform_filter 2", "frames 8"] + S("lib_filter_sms"),
            "lib_search_orchard": search("orchard") + S("lib_search_orchard"),
            "lib_search_noresult": search("zzzz") + S("lib_search_noresult"),
            "lib_scrolled_many": ["frames 10", "scroll_library 700", "frames 8"] + S("lib_scrolled_many"),
            "07_game_ready_long_name": open_card(0) + S("07_game_ready_long_name") + ["details_tab settings", "frames 8"] + S("17_game_settings") + ["details_tab advanced", "frames 8"] + S("24_game_advanced") + ["details_tab mods", "frames 8"] + S("13_game_mods_none"),
            "12_game_ready_sms": ["frames 10", "platform_filter 2", "frames 8", "focus_card 0", "frames 4", "key Return", "frames 10", "wait_idle", "frames 4"] + S("12_game_ready_sms") + ["details_tab settings", "frames 8"] + S("sms_game_settings") + ["details_tab advanced", "frames 8"] + S("sms_game_advanced"),
            "older_build": open_by_search("neon") + S("older_build_overview") + ["details_tab advanced", "frames 8"] + S("older_build_advanced"),
            "09_failed": open_by_search("broken") + S("09_failed_overview") + ["details_tab advanced", "frames 8"] + S("09_failed_advanced"),
            "08_unprepared": open_by_search("copper") + S("08_unprepared_overview") + ["details_tab settings", "frames 8"] + S("08_unprepared_settings"),
            "missing_rom": open_by_search("summit") + S("missing_rom_overview") + ["key Escape", "frames 8"] + S("missing_rom_library_search"),
            "mapper_needed": ["frames 10", "platform_filter 2", "frames 8", "focus_card 1", "frames 4", "key Return", "frames 10", "wait_idle", "frames 4"] + S("mapper_needed_overview") + ["details_tab settings", "frames 8"] + S("mapper_needed_settings"),
        }
        for name, script in scen.items():
            p.run(script + ["quit"], size, name="cap")


def group_settings():
    p = Prof("main", fresh=False)
    for size in C.sizes:
        S = lambda n: shot(size, n)
        for name, tab in (("18_settings_general", 0), ("19_controls_appearance", 1), ("20_storage_data", 2), ("24_advanced", 3)):
            p.run([f"settings_tab {tab}", "frames 14"] + S(name) + ["quit"], size, name="cap")
        for name, tab in (("18_settings_general_lower", 0), ("19_input_diagnostics_end", 1), ("20_storage_data_lower", 2), ("24_advanced_about", 3)):
            p.run([f"settings_tab {tab}", "frames 14"] + S(name) + ["quit"], size, {"SEGARECOMP_SHOT_SCROLL": "bottom"}, name="cap")
        p.run(["settings_tab 0", "frames 14"] + S("18_settings_general_preparation") + ["quit"], size, {"SEGARECOMP_SHOT_SCROLL": "middle"}, name="cap")   # round B: Preparation (the non-deleting default)
        p.run(["frames 10", "page storage", "frames 20", "wait_scan", "frames 10"] + S("23_cleanup") + ["scroll_page end", "frames 6"] + S("23_cleanup_lower") + ["quit"], size, name="cap")
        p.run(["frames 10", "pad_attach", "frames 20", "settings_tab 1", "frames 10"] + S("19_controls_pad_attached") + ["quit"], size, name="cap")
        p.run(["frames 10", "pad_raw_attach", "frames 20", "settings_tab 1", "frames 10"] + S("19_controls_raw_unmapped") + ["quit"], size, name="cap")
        for name, env in (("library_select_bulk", {"SEGARECOMP_SHOT_SELECT": "1"}), ("library_context_menu", {"SEGARECOMP_SHOT_MENU": "1"})):
            p.run(["frames 14"] + S(name) + ["quit"], size, env, name="cap")


def group_fresh():
    p = Prof("fresh")
    roms = p.roms
    genesis_rom(roms / "Harbor Lights.md", "Harbor Lights", 3)
    genesis_rom(roms / "Night Relay.md", "Night Relay", 4)
    sms_rom(roms / "Cart.sms", 9)
    (roms / "notes.txt").write_text("not a game")
    (roms / "sub").mkdir()
    genesis_rom(roms / "sub" / "deep.md", "Deep", 5)
    start = ["frames 10", "pad_attach", "frames 20"]
    on = {"SEGARECOMP_GUI_ONBOARDING": "always"}
    for size in C.sizes:
        S = lambda n: shot(size, n)
        cases = [
            ("01_welcome", ["frames 20"] + S("01_welcome"), on),
            ("01_welcome_pad", start + S("01_welcome_pad_focus"), on),
            ("02_empty", start + seek("welcome_skip", "dpad_right", 8) + press("south") + ["frames 8"] + S("02_empty"), on),
            ("04_add", ["frames 8", "page import", "frames 10"] + S("04_add"), {}),
            ("04_add_keys", ["frames 8", "page import", "frames 10", "key Tab", "key Tab", "frames 4"] + S("04_add_keyboard_focus"), {}),
            ("picker", start + seek("welcome_add", "dpad_right", 8) + press("south") + ["frames 8"] + S("controller_picker_locations") + press("south") + ["frames 8"] + S("controller_picker_root"), dict(on, SEGARECOMP_PICKER_ROOT=str(roms))),
            ("05_review", ["frames 8", f"import_files {roms / 'Harbor Lights.md'};{roms / 'Night Relay.md'};{roms / 'Cart.sms'};{roms / 'notes.txt'}", "wait_import", "frames 10"] + S("05_review_mixed") + ["review_add", "frames 20"] + S("05b_after_add_only"), {}),
        ]
        for name, script, env in cases:
            p.run(script + ["quit"], size, env, name="cap")
            if name == "05_review":
                shutil.rmtree(p.lib, ignore_errors=True)


def group_addprep():
    p = Prof("addprep")
    for title, seed in (("Harbor Lights", 3), ("Night Relay", 4), ("Glass Garden", 5)):
        genesis_rom(p.roms / (title + ".md"), title, seed)
    hold = p.dir / "hb"
    for size in C.sizes:
        shutil.rmtree(p.lib, ignore_errors=True)
        shutil.rmtree(p.cache, ignore_errors=True)
        S = lambda n: shot(size, n)
        files = ";".join(str(p.roms / (t + ".md")) for t in ("Harbor Lights", "Night Relay", "Glass Garden"))
        script = ["frames 8", f"import_files {files}", "wait_import", "frames 8", "review_prepare", f"wait_file {hold}.started", "frames 20"] + S("06_preparing_queued_library") + ["focus_card 0", "frames 4", "key Return", "frames 14"] + S("06_preparing_details") \
            + ["key Escape", "frames 8", "main_action NIGHT RELAY", "frames 6", "main_action GLASS GARDEN", "frames 6", "main_action HARBOR LIGHTS", "wait_idle", "frames 20", "quit"]
        p.run(script, size, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}, name="cap")
        for f in p.dir.glob("hb*"):
            f.unlink()


def group_errors():
    for size in C.sizes:
        S = lambda n: shot(size, n)
        p = Prof("err_nopkg")
        shutil.rmtree(p.package)   # the package folder does not exist: "installation incomplete"
        genesis_rom(p.roms / "A.md", "Alpha", 1)
        subprocess.run([C.launcher, "--import", str(p.roms / "A.md")], env=p.env, capture_output=True, timeout=60)
        p.run(["frames 14"] + S("error_installation_incomplete_library") + ["focus_card 0", "frames 4", "key Return", "frames 12"] + S("error_installation_incomplete_game") + ["quit"], size, name="cap")
        q = Prof("err_ro")   # the library folder cannot be created (its parent is a file)
        blocker = q.dir / "afile"
        blocker.write_text("x")
        genesis_rom(q.roms / "Q.md", "Quill", 7)
        env = {"SEGARECOMP_LIBRARY_DIR": str(blocker / "lib"), "SEGARECOMP_CACHE_DIR": str(blocker / "cache")}
        q.run(["frames 14"] + S("error_unwritable_profile_library") + ["page import", "frames 8"] + S("error_unwritable_profile_add") + ["settings_tab 2", "frames 10"] + S("error_unwritable_profile_storage") + ["quit"], size, env, name="cap")
        q.run(["frames 8", f"import_files {q.roms / 'Q.md'}", "wait_import", "frames 10", "review_add", "frames 12"] + S("error_unwritable_profile_after_add") + ["quit"], size, env, name="cap2")


def group_scd():
    spec = importlib.util.spec_from_file_location("par", str(ROOT / "apps" / "segarecomp-launcher" / "launcher_parity_gui_test.py"))
    par = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(par)

    def bios(region, tag):
        img = bytearray(0x400)
        img[0x100:0x110] = b"SEGA MEGA DRIVE "
        img[0x1F0:0x1F4] = region.encode().ljust(4)[:4]
        img[0x200:0x200 + len(tag)] = tag
        return bytes(img)
    for size in C.sizes:
        S = lambda n: shot(size, n)
        p = Prof("scd")
        root = p.dir / "scdx"
        b, cue, stmt, prog = par.scd_fixture(root, pathlib.Path(C.fake_scd))
        env = {"FAKE_SCD_PROGRAM": str(prog), "FAKE_SCD_DESCRIBE": str(stmt)}
        p.run(["page import", "frames 6", f"import_files {cue}", "wait_scd_inspect", "frames 10"] + S("10_segaCD_review_needs_bios") + [f"review_bios 0 {b}", "frames 8"] + S("11_bios_chosen_review") + ["quit"], size, env, name="cap1")
        p.run(["page import", "frames 6", f"import_files {cue}", "wait_scd_inspect", "frames 4", f"review_bios 0 {b}", "frames 4", "review_add", "frames 10", "platform_filter 3", "frames 6"] + S("scd_library")
              + ["focus_card 0", "frames 4", "key Return", "frames 12", "wait_idle", "frames 4"] + S("12_segaReady_not_prepared") + ["details_tab advanced", "frames 8"] + S("12_segaCD_advanced") + ["quit"], size, env, name="cap2")
        p.run(["platform_filter 3", "frames 8", "focus_card 0", "frames 4", "key Return", "frames 10", "build", "wait_idle", "frames 30", "wait_idle"] + S("12_segaReady_ready") + ["details_tab settings", "frames 8"] + S("12_segaCD_settings") + ["quit"], size, env, name="cap3")
        pathlib.Path(b).write_bytes(pathlib.Path(b).read_bytes() + b"tampered")   # the BIOS changed after it was added
        p.run(["platform_filter 3", "frames 8", "focus_card 0", "frames 4", "key Return", "frames 12", "wait_idle", "frames 6"] + S("scd_bios_changed_overview") + ["details_tab advanced", "frames 8"] + S("scd_bios_changed_advanced") + ["quit"], size, env, name="cap4")
        broken = root / "broken"
        shutil.copytree(root / "disc", broken)
        (broken / "track02.bin").unlink()
        q = Prof("scd_missing")
        q.run(["page import", "frames 6", f"import_files {next(broken.glob('*.cue'))}", "wait_scd_inspect", "frames 10"] + S("11_bios_disc_missing_track_review") + ["quit"], size, env, name="cap5")


def group_lockon():
    p = Prof("lockon")
    rom = p.roms / "combo.bin"
    lock_on_rom(rom)
    p.import_path(rom)
    for size in C.sizes:
        S = lambda n: shot(size, n)
        low = ["scroll_page end", "frames 6"]
        p.run(["frames 10", "focus_card 0", "frames 4", "key Return", "frames 12", "wait_idle", "frames 6"] + S("lockon_overview") + ["details_tab settings", "frames 8"] + low + S("lockon_settings_auto_advanced")
              + ["cartridge standard", "wait_idle", "frames 8"] + low + S("lockon_settings_manual_standard") + ["cartridge lock_on", "wait_idle", "frames 8"] + low + S("lockon_settings_manual_lock_on") + ["cartridge auto", "wait_idle", "frames 4", "quit"],
              size, {"SEGARECOMP_SHOT_ADVANCED": "1"}, name="cap")


class DataProf:
    """A profile at its DEFAULT data root (a private XDG folder) with a private bootstrap file and synthetic durable and regenerable data."""

    def __init__(self, name, files=8):
        self.dir = C.work / name
        if self.dir.exists():
            shutil.rmtree(self.dir)
        self.dir.mkdir(parents=True)
        self.package = self.dir / "package"
        make_package(self.package)
        self.root = self.dir / "xdg" / "Segarecomp"
        self.lib = self.root / "library"
        self.bootstrap = self.dir / "boot" / "Segarecomp-data-location.json"
        self.bootstrap.parent.mkdir()
        ext = self.dir / "external"
        ext.mkdir()
        (self.dir / "home").mkdir()
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", FAKE_GAME_BIN=C.fake_game,
                        XDG_DATA_HOME=str(self.dir / "xdg"), HOME=str(self.dir / "home"), SEGARECOMP_DATA_BOOTSTRAP=str(self.bootstrap))
        for v in ("SEGARECOMP_CACHE_DIR", "SEGARECOMP_LIBRARY_DIR"):
            self.env.pop(v, None)
        for i in range(2):
            rom = ext / f"game{i}.md"
            genesis_rom(rom, f"Game {i}", i + 1)
            done = subprocess.run([C.launcher, "--import", str(rom)], env=self.env, capture_output=True, text=True, timeout=60)
            assert "import=added" in done.stdout, done.stdout + done.stderr
        digest = json.loads((self.lib / "library.json").read_text())["games"][0]["sha256"]
        for i in range(files):
            self.w(f"library/saves/genesis/{digest}/slot{i}.state", os.urandom(300 + 40 * i))
        self.w("library/covers/c.png", os.urandom(500))
        self.w("library/sessions/rec.bin", os.urandom(200))
        self.w("library/patches/p/readme.txt", b"a mod package file\n")
        self.w("games/" + "a" * 32 + "/game.bin", os.urandom(4000))
        self.w("games/" + "a" * 32 + "/obj/x.o", os.urandom(1000))
        self.w("zig-cache/z/blob", os.urandom(2500))
        self.w("cover-downloads/d.png", os.urandom(700))

    def w(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run(self, script, size, extra=None):
        sp, tp = self.dir / "cap.txt", self.dir / "cap.trace"
        sp.write_text("\n".join(script) + "\n")
        tp.unlink(missing_ok=True)
        env = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(sp), SEGARECOMP_GUI_TRACE=str(tp), SEGARECOMP_SHOT_SIZE=size, **(extra or {}))
        subprocess.run([C.launcher], env=env, timeout=240, check=True, capture_output=True)
        trace = tp.read_text().splitlines() if tp.exists() else []
        bad = [line for line in trace if "FAIL" in line]
        if bad:
            print(size, bad)
        return trace


def group_data():
    plan = lambda dest: ["frames 6", "dm_open", "frames 4", f"dm_destination {dest}", "dm_wait", "frames 4"]
    for size in C.sizes:
        S = lambda n: shot(size, n)
        p = DataProf("data1")
        dest = p.dir / "newhome"
        p.run(["frames 6", "settings_tab 2", "frames 10"] + S("20_storage_data_with_location") + ["dm_open", "frames 6"] + S("21_relocate_choose") + [f"dm_destination {dest}", "dm_wait", "frames 6"] + S("21_relocate_plan")
              + ["scroll_page end", "frames 6"] + S("21_relocate_plan_lower") + ["dm_confirm", "dm_wait_phase copy", "frames 3"] + S("22_migrating_progress") + ["dm_wait", "frames 8"] + S("22_migrating_completed_restart_needed")
              + ["dm_back", "frames 8"] + S("21_relocate_after_settings_previous_present") + ["quit"], size, {"SEGARECOMP_TEST_MIGRATION_HOLD_MS": "2500"})
        p.run(["frames 8", "settings_tab 2", "frames 8", "dm_cleanup_open", "dm_wait", "frames 6"] + S("23_cleanup_previous_location_plan") + ["dm_cleanup_copies 1", "dm_wait", "frames 4"] + S("23_cleanup_previous_location_copies")
              + ["dm_cleanup_confirm", "dm_wait", "frames 6"] + S("23_cleanup_previous_location_done") + ["quit"], size)
        p = DataProf("data2", files=6)
        foreign = p.dir / "occupied"
        foreign.mkdir()
        (foreign / "mine.txt").write_text("not segarecomp's")
        p.run(plan(foreign) + S("21_relocate_refused_not_empty") + ["dm_back", "frames 4", "dm_open", "frames 4", f"dm_destination {p.root / 'inside'}", "dm_wait", "frames 6"] + S("21_relocate_refused_inside_source") + ["quit"], size)
        p.run(plan(p.dir / "elsewhere") + S("21_relocate_refused_space") + ["quit"], size, {"SEGARECOMP_TEST_FREE_SPACE_BYTES": "1000"})
        p = DataProf("data3")
        p.run(plan(p.dir / "newhome") + ["dm_confirm", "dm_wait_phase copy", "frames 2", "dm_cancel", "dm_wait", "frames 8"] + S("22_migrating_cancelled") + ["quit"], size, {"SEGARECOMP_TEST_MIGRATION_HOLD_MS": "400"})
        p = DataProf("data4", files=6)
        p.run(plan(p.dir / "newhome") + ["dm_confirm", "dm_wait", "frames 8"] + S("22_migrating_interrupted") + ["quit"], size, {"SEGARECOMP_TEST_MIGRATION_CRASH": "copy:3"})
        p.run(["frames 14", "settings_tab 2", "frames 10"] + S("22_migrating_recovery_notice_after_restart") + ["quit"], size)
        p = DataProf("data5")
        p.run(["frames 6", "settings_tab 2", "frames 8"] + S("21_relocate_env_override_disabled") + ["quit"], size, {"SEGARECOMP_LIBRARY_DIR": str(p.dir / "governed")})


GROUPS = {"setup": group_setup, "main": group_main, "settings": group_settings, "fresh": group_fresh, "addprep": group_addprep, "errors": group_errors, "scd": group_scd, "lockon": group_lockon, "data": group_data}


def convert(out):
    from PIL import Image
    for bmp in out.rglob("*.bmp"):
        Image.open(bmp).convert("RGB").save(bmp.with_suffix(".png"))
        bmp.unlink()


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("--launcher", type=pathlib.Path, required=True)
    a.add_argument("--fake-cli", type=pathlib.Path, required=True)
    a.add_argument("--fake-game", type=pathlib.Path, required=True)
    a.add_argument("--fake-scd", type=pathlib.Path, required=True)
    a.add_argument("--work", type=pathlib.Path, required=True, help="scratch directory for the profiles (outside the product tree)")
    a.add_argument("--out", type=pathlib.Path, required=True, help="capture output (outside the product tree)")
    a.add_argument("--sizes", default="1080x740,720x520,1600x900")
    a.add_argument("groups", nargs="+", choices=sorted(GROUPS))
    args = a.parse_args()
    C.launcher, C.fake_cli, C.fake_game, C.fake_scd = (str(x.resolve()) for x in (args.launcher, args.fake_cli, args.fake_game, args.fake_scd))
    C.work, C.out, C.sizes = args.work.resolve(), args.out.resolve(), args.sizes.split(",")
    for group in args.groups:
        GROUPS[group]()
    convert(C.out)
    print("captured:", ", ".join(args.groups))


if __name__ == "__main__":
    sys.exit(main())
