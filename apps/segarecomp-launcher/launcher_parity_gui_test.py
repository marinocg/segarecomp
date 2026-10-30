#!/usr/bin/env python3
"""SEG-109-T005 (ADR 0253): Genesis, Master System and Sega CD through the SAME launcher journeys, on the real window (SDL dummy video driver, software renderer).

Project-authored fixtures only (synthetic ROM headers, a synthetic BIOS/disc/program statement, the product and game doubles); no commercial data.

For every platform, in one shared library:
  * Library -> Details -> Build -> Play -> Return, with the keyboard (and a separate pass with the mouse, and one with a virtual gamepad only);
  * Build cancellation and retry; a failed build leaves the previous playable program byte-identical;
  * the build-mode selection and a cover change persist across a relaunch;
  * Storage inspection and cleanup; a Sega CD game's save and recordings survive every cleanup, a rebuild and the removal of the game from the library;
  * the Details page and the context menus have the same structure: same sections in the same reading order, the same Play / Build / More buttons at the same
    columns, the same menu items (the Sega CD extras - BIOS/disc/program/recording - are the documented hardware-specific ones);
  * at 1080x740 and at the minimum supported width (720x520). Screenshots are written to $SEGARECOMP_PARITY_SHOTS when it is set.

usage: launcher_parity_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game> <fake_scd_program> [size ...] [stage ...]
stages: journey (keyboard journey, failure/cancel/retry, layout and menu comparison), persist (settings, cover, storage, protected user data), input (mouse and
controller only); default all
"""
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
PLATFORMS = ("genesis", "master-system", "sega-cd")
FILTER = {"genesis": 1, "master-system": 2, "sega-cd": 3}
TITLE = {"genesis": "Alpha", "master-system": "Beta", "sega-cd": "Gamma"}   # one word each: the controller seek takes whitespace-separated arguments
MODE = {"genesis": ("compatibility", "Compatibility"), "master-system": ("compatibility", "Compatibility"), "sega-cd": ("file_first", "File-first")}
SCD_EXTRA_ITEMS = {"Open recordings folder", "Replace program...", "Import program...", "Verify again", "Locate BIOS...", "Locate disc..."}   # hardware/product-specific
IDENTITY_ITEMS = {"Copy ROM SHA-256", "Copy game identity"}


# ---------------------------------------------------------------- fixtures
def make_package(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/genesis/runtime/vdp_render.c", "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c",
                "runtime/platforms/sega-cd/runtime/scd_main.c", "runtime/platforms/sega-cd/viewer/scd_viewer.c",
                "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c", "sdl3/include/SDL3/SDL.h", "sdl3/lib/libSDL3.so.0"):
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


def png(path):
    """A small valid PNG (project-authored, 32x32 flat colour; the launcher accepts 16..2048 pixels)."""
    raw = b"".join(b"\x00" + bytes((30, 120, 200)) * 32 for _ in range(32))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def scd_fixture(root, fake_program):
    """A synthetic BIOS-like image, a two-track disc and the statement of a program double built for exactly these two."""
    bios_bytes = b"SYNTHETIC BIOS-LIKE IMAGE FOR THE PARITY TEST " + b"x" * 4000
    track1, track2 = b"SYNTHETIC TRACK ONE " + b"1" * 3000, b"SYNTHETIC TRACK TWO " + b"2" * 5000
    root.mkdir(parents=True)
    (root / "disc").mkdir()
    (root / "bios.bin").write_bytes(bios_bytes)
    cue = root / "disc" / (TITLE["sega-cd"] + ".cue")
    cue.write_text('FILE "Game (Track 1).bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\nFILE "Game (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
    (root / "disc" / "Game (Track 1).bin").write_bytes(track1)
    (root / "disc" / "Game (Track 2).bin").write_bytes(track2)
    statement = ('{"describe":1,"runtime_abi":1,"profile":"bios_image","viewer":true,"program_identity":"%s","disc_identity":"%s","bios":{"size":%d,"sha256":"%s"},'
                 '"disc_files":[{"name":"Game (Track 1).bin","size":%d,"sha256":"%s"},{"name":"Game (Track 2).bin","size":%d,"sha256":"%s"}]}\n'
                 % ("7" * 64, "8" * 64, len(bios_bytes), sha(bios_bytes), len(track1), sha(track1), len(track2), sha(track2)))
    (root / "statement.json").write_text(statement)
    return root / "bios.bin", cue, root / "statement.json", fake_program


def parse_layout(trace):
    """The last `layout ...` line as {name: (x, y, w, h)}."""
    lines = [l for l in trace if l.startswith("layout ")]
    out = {}
    if lines:
        for part in lines[-1][len("layout "):].split(";"):
            if ":" in part:
                name, rect = part.split(":")
                out[name] = tuple(int(v) for v in rect.split(","))
    return out


def parse_menu(trace):
    """The last non-empty `menu_items ...` line as a normalized set of enabled/disabled-stripped labels."""
    best = []
    for l in trace:
        if l.startswith("menu_items ") and len(l) > len("menu_items "):
            best = l[len("menu_items "):].split("|")
    return best


def normalize_menu(items, platform):
    names = {i.replace(" [disabled]", "") for i in items}
    names = {"Copy identity" if n in IDENTITY_ITEMS else n for n in names}
    if platform == "sega-cd":
        names -= SCD_EXTRA_ITEMS
    names.discard("Locate ROM...")
    return names


class Case:
    def __init__(self, exe, fake_cli, fake_game, fake_scd, tmp, shots):
        self.exe = exe
        self.dir = tmp
        self.shots = shots
        self.package = tmp / "package"
        make_package(self.package, fake_cli)
        self.lib, self.cache = tmp / "lib", tmp / "cache"
        self.game_log, self.args_log = tmp / "game.log", tmp / "args.txt"
        self.bios, self.cue, self.statement, self.fake_scd = scd_fixture(tmp / "scd", pathlib.Path(fake_scd))
        self.program = tmp / "scd" / "prog" / ("game-viewer" + EXE_SUFFIX)
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache), SEGARECOMP_LIBRARY_DIR=str(self.lib),
                        SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_NO_STOP_SCREEN="1",
                        SEGARECOMP_VIEWER_SETTINGS=str(tmp / "viewer.cfg"), FAKE_GAME_BIN=fake_game, FAKE_GAME_LOG=str(self.game_log),
                        FAKE_BUILD_ARGS_LOG=str(self.args_log), FAKE_SCD_PROGRAM=str(self.fake_scd), FAKE_SCD_DESCRIBE=str(self.statement))
        for name, make in (("Alpha.md", genesis_rom), ("Beta.sms", sms_rom)):
            make(tmp / name)
            done = subprocess.run([exe, "--import", str(tmp / name)], env=self.env, timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr
        data = json.loads((self.lib / "library.json").read_text())
        for game in data["games"]:
            if game["platform"] == "master-system":
                game["mapper"] = "sega"   # the header never names it: the user (here the test) declares it
        (self.lib / "library.json").write_text(json.dumps(data))
        trace = self.run(["frames 8", "page import", "frames 6", "scd_form %s|%s|" % (self.bios, self.cue), "frames 4", "scd_add", "wait_scd", "frames 10", "quit"])
        assert any("scd=added" in l for l in trace), trace

    def run(self, script, extra_env=None, size="1080x740"):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), SEGARECOMP_SHOT_SIZE=size, **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=900, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []

    def games(self):
        return {g["platform"]: g for g in json.loads((self.lib / "library.json").read_text())["games"]}

    def scd_dir(self):
        found = sorted((self.lib / "sega-cd").glob("*"))
        return found[0] if found else None

    def program_of(self, platform):
        """The executable a build of the platform produced (None before a build)."""
        if platform == "sega-cd":
            d = self.scd_dir()
            return d / "build" / ("program-viewer" + EXE_SUFFIX) if d is not None else None
        found = sorted(p for p in (self.cache / "games").glob("*/*") if p.name == "game" + EXE_SUFFIX) if (self.cache / "games").exists() else []
        wanted = [p for p in found if (self.cache / "games" / p.parent.name).is_dir()]
        for p in wanted:   # the entry of this platform: its policy file or status names a request; the platform is recorded in the catalog by the build key
            key = p.parent.name
            owner = [g for g in self.games().values() if g.get("current_build_key") == key or any(b["key"] == key for b in g.get("builds", []))]
            if owner and owner[0]["platform"] == platform:
                return p
        return None

    def leftovers(self):
        found = [p for p in self.cache.rglob("*") if p.name.endswith((".partial", ".failed"))] if self.cache.exists() else []
        if (self.lib / "sega-cd").exists():
            found += [p for p in (self.lib / "sega-cd").rglob("*") if p.name.endswith((".partial", ".failed"))]
        return found


class Report:
    def __init__(self):
        self.failures, self.checked = [], 0

    def trace(self, name, trace, expected=None):
        bad = [l for l in trace if l.startswith("expect FAIL")]
        self.failures.extend(f"{name}: {l}" for l in bad)
        if bad:   # the lines of the run that explain it
            self.failures.extend(f"{name}:   | {l[:160]}" for l in trace if l.startswith(("scd=", "build=", "run=", "details=", "cleanup", "script=", "storage")))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        self.checked += ok
        if expected is not None and ok != expected:
            self.failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def ok(self, condition, message):
        self.checked += 1 if condition else 0
        if not condition:
            self.failures.append(message)


# ---------------------------------------------------------------- script vocabulary
# One window runs the SAME segment for the three platforms in turn (open the game from the library, act, return to the library); the trace is cut back into
# one part per platform at the `expect detail=<title>` marker each segment begins with.
START = ["frames 10"]


def open_game(p):
    """Library -> Details with the keyboard (focus a card, Return)."""
    return [f"platform_filter {FILTER[p]}", "frames 4", "expect visible=1", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details", f"expect detail={TITLE[p]}"]


BACK = ["key Escape", "frames 8", "expect page=library"]


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def expect(**kw):
    return [f"expect {k}={v}" for k, v in kw.items()]


def shot(c, p, name, size):
    return [f"shot {c.shots / (p + '-' + name + '-' + size + '.png')}", "frames 3"] if c.shots is not None else []


def split_by_platform(trace):
    """{platform: [trace lines from its `expect ok detail=<title>` marker to the next one]}."""
    parts, current = {}, None
    for line in trace:
        for p in PLATFORMS:
            if line.startswith(f"expect ok detail={TITLE[p]}"):
                current = p
                parts[p] = []
        if current is not None:
            parts[current].append(line)
    return parts


def window(c, size, segments, extra_env=None):
    """One window running segment(p) for every platform in turn; returns (trace, per-platform parts)."""
    script = list(START)
    for p in PLATFORMS:
        script += segments(p)
    script.append("quit")
    trace = c.run(script, extra_env, size)
    return trace, split_by_platform(trace)


# ---------------------------------------------------------------- stage: journey
def stage_journey(r, c, size):
    print(f"-- journey {size}")
    hold = c.dir / "heartbeat"
    # 1. Library -> Details -> Build -> Play -> Return (keyboard), then the Details layout and the menus of a ready game
    def ok_journey(p):
        s = open_game(p) + ["expect can_play=0", "expect building=0"] + shot(c, p, "details-new", size) + ["layout"]
        s += ["build", "wait_idle", "frames 6", "expect building=0", "expect build_outcome=ok", "expect can_play=1"]
        s += ["play", "frames 4", "wait_play_end", "frames 10", "expect playing=0", "layout"] + shot(c, p, "details-ready", size)
        s += ["scroll_page end", "frames 4"] + shot(c, p, "details-bottom", size) + ["scroll_page 0", "frames 4"]
        s += ["open_more", "frames 4", "menu_items", "expect menu_has=View build log", "expect menu_has=Manage storage...", "key Escape", "frames 8", "expect page=library"]
        s += ["focus_card 0", "frames 4", "key Menu", "frames 6", "menu_items", "key Escape", "frames 6"]
        return s
    trace, parts = window(c, size, ok_journey, {"FAKE_SCD_WRITE_SAVE": "1"})
    r.trace(f"{size} journey", trace)
    layouts, menus, library_menus = {}, {}, {}
    programs = {}
    for p in PLATFORMS:
        lines = parts.get(p, [])
        r.ok(bool(lines), f"{p} {size}: the journey ran")
        layouts[p] = parse_layout(lines)
        item_lines = [l for l in lines if l.startswith("menu_items ") and len(l) > len("menu_items ")]
        menus[p] = item_lines[0][len("menu_items "):].split("|") if item_lines else []
        library_menus[p] = item_lines[1][len("menu_items "):].split("|") if len(item_lines) > 1 else []
        r.ok(bool(menus[p]) and bool(library_menus[p]), f"{p} {size}: the More menu and the library context menu listed items")
        prog = c.program_of(p)
        r.ok(prog is not None and prog.is_file(), f"{p} {size}: the build produced an executable")
        programs[p] = (prog, sha(prog.read_bytes()) if prog is not None and prog.is_file() else "")
    # 2. a failed build leaves the previous playable program byte-identical
    def failed_build(p):
        return open_game(p) + ["build", "wait_idle", "frames 6", "expect build_outcome=failed", "expect failure=1", "expect can_play=1", "expect building=0"] + BACK
    trace, parts = window(c, size, failed_build, {"FAKE_BUILD_FAIL": "optimized,sega-cd"})
    r.trace(f"{size} failed build", trace)
    for p in PLATFORMS:
        r.ok(programs[p][0] is not None and sha(programs[p][0].read_bytes()) == programs[p][1], f"{p} {size}: the failed build left the previous program in place")
    # 3. a cancelled build: the Cancel button takes the Build slot, nothing partial is left, the previous program stays
    started = pathlib.Path(f"{hold}.started")
    started.unlink(missing_ok=True)
    def cancelled_build(p):
        s = open_game(p) + ["build", "frames 40", "expect building=1", "expect build_count=1", "layout"] + shot(c, p, "building", size)
        return s + ["cancel_build", "cancel_build", "wait_idle", "frames 6", "expect building=0", "expect stopping=0", "expect build_count=0", "expect build_outcome=cancelled", "expect can_play=1"] + BACK
    trace, parts = window(c, size, cancelled_build, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"})
    r.trace(f"{size} cancel", trace)
    building = {}
    for p in PLATFORMS:
        building[p] = parse_layout(parts.get(p, []))
        r.ok("build" in building[p] and "play" not in building[p], f"{p} {size}: Cancel takes the Build slot while a build runs: {building[p]}")
        r.ok(sha(programs[p][0].read_bytes()) == programs[p][1], f"{p} {size}: the cancelled rebuild left the previous program in place")
    r.ok(not [x for x in c.leftovers() if x.name.endswith(".partial")], f"{size}: a cancelled build leaves no partial folder: {c.leftovers()}")
    size_before = hold.stat().st_size if hold.exists() else 0
    time.sleep(0.5)
    r.ok((hold.stat().st_size if hold.exists() else 0) == size_before, f"{size}: no process of a cancelled build is still running")
    # 3b. ONE build queue across platforms: a game of the other kind waits behind the running build, can be taken out of the queue, and starts when the first ends
    for first, second in (("genesis", "sega-cd"), ("sega-cd", "master-system"), ("master-system", "sega-cd")):
        started.unlink(missing_ok=True)
        script = list(START) + open_game(first) + ["build", "frames 40", "expect building=1"] + BACK
        script += open_game(second) + ["build", "frames 8", "expect queued=1", "expect build_count=2", "layout", "cancel_build", "frames 8", "expect queued=0", "expect build_count=1"] + BACK
        script += open_game(first) + ["cancel_build", "wait_idle", "frames 6", "expect building=0", "expect build_count=0", "expect build_outcome=cancelled"] + BACK + ["quit"]
        trace = c.run(script, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}, size)
        r.trace(f"{size} queue {first} then {second}", trace)
        qlayout = parse_layout(trace[[i for i, l in enumerate(trace) if l.startswith("layout ")][0]:][:1]) if any(l.startswith("layout ") for l in trace) else {}
        r.ok("build" in qlayout, f"{size}: a queued {second} game shows Unqueue in the Build slot: {qlayout}")
    r.ok(not [x for x in c.leftovers() if x.name.endswith(".partial")], f"{size}: nothing partial after the queue scenarios")
    # 4. retry after the cancel and the failure
    trace, _ = window(c, size, lambda p: open_game(p) + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "expect can_play=1"] + BACK)
    r.trace(f"{size} retry", trace)
    # ---- structural comparison: same sections, same order, same columns, same menus ----
    def order(layout):
        return [n for n, _ in sorted(layout.items(), key=lambda kv: (kv[1][1] // 8, kv[1][0])) if n != "recording"]
    ref = PLATFORMS[0]
    for p in PLATFORMS[1:]:
        r.ok(set(layouts[p]) - {"recording"} == set(layouts[ref]), f"{p} {size}: the same sections as {ref}: {sorted(layouts[p])} vs {sorted(layouts[ref])}")
        r.ok(order(layouts[p]) == order(layouts[ref]), f"{p} {size}: the sections are in the same reading order: {order(layouts[p])} vs {order(layouts[ref])}")
        for name in ("play", "build", "more", "config", "provenance"):
            if name in layouts[p] and name in layouts[ref]:
                a, b = layouts[p][name], layouts[ref][name]
                r.ok(a[0] == b[0] and abs(a[2] - b[2]) <= 2, f"{p} {size}: {name} is at the same column and width as {ref}: {a} vs {b}")
        r.ok(building[p].get("build", (0, 0, 0, 0))[0] == building[ref].get("build", (1, 0, 0, 0))[0] and building[p]["build"][2] == building[ref]["build"][2],
             f"{p} {size}: Cancel is at the same column and width as {ref}: {building[p].get('build')} vs {building[ref].get('build')}")
        r.ok(abs(layouts[p]["play"][1] - layouts[ref]["play"][1]) <= 24, f"{p} {size}: the action row is at the same height as {ref}: {layouts[p]['play']} vs {layouts[ref]['play']}")
        r.ok(normalize_menu(menus[p], p) == normalize_menu(menus[ref], ref), f"{p} {size}: the More menu offers the same items as {ref}: {sorted(normalize_menu(menus[p], p) ^ normalize_menu(menus[ref], ref))}")
        r.ok(normalize_menu(library_menus[p], p) == normalize_menu(library_menus[ref], ref),
             f"{p} {size}: the library context menu offers the same items as {ref}: {sorted(normalize_menu(library_menus[p], p) ^ normalize_menu(library_menus[ref], ref))}")
    stripped = lambda items: {i.replace(" [disabled]", "") for i in items}
    r.ok({"Manage storage...", "Open build folder"} <= stripped(menus["sega-cd"]), f"{size}: the Sega CD menu has Manage storage and Open build folder")
    r.ok(any(i.startswith("Download cover") and "[disabled]" not in i for i in menus["sega-cd"]), f"{size}: the Sega CD cover download is offered: {menus['sega-cd']}")
    r.ok(all("Rebuild" in stripped(library_menus[p]) for p in PLATFORMS), f"{size}: the library menu of a built game offers Rebuild on every platform: {library_menus}")


# ---------------------------------------------------------------- stage: persist
def stage_persist(r, c, size):
    print(f"-- persist {size}")
    png_file = c.dir / "cover.png"
    png(png_file)
    # a built, played game of each platform (the Sega CD play stores the program double's save mark)
    trace, _ = window(c, size, lambda p: open_game(p) + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "play", "frames 4", "wait_play_end", "frames 10", "expect playing=0"] + BACK,
                      {"FAKE_SCD_WRITE_SAVE": "1"})
    r.trace("prepare", trace)
    # settings and covers persist across a relaunch
    def settings(p):
        mode, label = MODE[p]
        return open_game(p) + [f"mode {mode}", "frames 6", f"expect mode={label}", f"cover {png_file}", "frames 10", "expect cover=1", "open_more", "frames 4", "menu_items", "key Escape", "frames 8", "expect page=library"]
    trace, parts = window(c, size, settings)
    r.trace("settings and cover", trace)
    for p in PLATFORMS:
        items = [l[len("menu_items "):].split("|") for l in parts.get(p, []) if l.startswith("menu_items ") and len(l) > len("menu_items ")]
        r.ok(bool(items) and "Remove cover" in {i.replace(" [disabled]", "") for i in items[0]}, f"{p}: Remove cover appears once a cover is set: {items}")
    r.ok(len(list((c.lib / "covers").glob("*"))) >= 3, "every game's cover is in the library's covers folder")
    trace, _ = window(c, size, lambda p: open_game(p) + [f"expect mode={MODE[p][1]}", "expect cover=1"] + BACK)   # a new process: the stored selection and cover
    r.trace("relaunch", trace)
    persisted = c.games()
    r.ok(all(persisted[p].get("cover") for p in PLATFORMS), f"the covers are recorded in the library: {[persisted[p].get('cover') for p in PLATFORMS]}")
    # Storage: inspection (the Details rail and the measured report), then cleanup; Sega CD user data is protected
    scd_dir = c.scd_dir()
    (scd_dir / "recordings").mkdir(exist_ok=True)
    (scd_dir / "recordings" / "session-1.txt").write_text("synthetic input recording")
    save_before = (scd_dir / "backup-ram.bin").read_bytes()
    r.ok(len(save_before) == 8192 and save_before[100] == 0x5A, "Sega CD: the play stored the game's save (the program double wrote its mark)")
    def inspect(p):
        return open_game(p) + ["build", "wait_idle", "frames 6", "expect can_play=1", "scan", "wait_scan", "report", "layout"] + BACK
    trace, parts = window(c, size, inspect)
    r.trace("storage inspection", trace)
    for p in PLATFORMS:
        report = [l for l in parts.get(p, []) if l.startswith("storage ")]
        r.ok(bool(report) and "total=0 " not in report[-1] and "storage" in parse_layout(parts.get(p, [])), f"{p}: the Details rail and the measured storage exist: {report}")
        r.ok(bool(report) and (("user=0 " in report[-1]) == (p != "sega-cd")), f"{p}: only a Sega CD game reports protected user data: {report}")
    programs = {p: c.program_of(p) for p in PLATFORMS}
    trace, _ = window(c, size, lambda p: open_game(p) + ["scan", "wait_scan", "cleanup archive", "wait_idle", "frames 8", "expect maintenance=0", "expect can_play=0"] + BACK)
    r.trace("archive", trace)
    for p in PLATFORMS:
        r.ok(programs[p] is not None and not programs[p].exists(), f"{p}: the archive removed the generated build ({programs[p]})")
    r.ok((scd_dir / "backup-ram.bin").read_bytes() == save_before and (scd_dir / "recordings" / "session-1.txt").exists(), "Sega CD: the archive left the save and the recordings untouched")
    # rebuild: every game plays again, and the Sega CD run loads the stored save
    trace, parts = window(c, size, lambda p: open_game(p) + ["build", "wait_idle", "frames 6", "expect can_play=1", "play", "frames 4", "wait_play_end", "frames 10", "expect playing=0"] + BACK,
                          {"FAKE_SCD_EXPECT_SAVE": "1"})
    r.trace("rebuild and play", trace)
    r.ok(any(l.startswith("scd=run_exited code=0") for l in parts.get("sega-cd", [])), f"Sega CD: the run after the rebuild loaded the stored save: {[l for l in parts.get('sega-cd', []) if 'scd=' in l]}")
    r.ok((scd_dir / "backup-ram.bin").read_bytes() == save_before, "Sega CD: the save is intact after the rebuild and the run")
    # removal from the library keeps a Sega CD game's user data; adding the same BIOS and disc again finds it
    script = START + open_game("sega-cd") + ["scan", "wait_scan", "cleanup remove", "wait_idle", "frames 10", "expect maintenance=0"]
    script += ["page import", "frames 6", "scd_form %s|%s|" % (c.bios, c.cue), "frames 4", "scd_add", "wait_scd", "frames 10", "build", "wait_idle", "frames 6", "expect can_play=1",
               "play", "frames 4", "wait_play_end", "frames 10", "expect playing=0", "quit"]
    trace = c.run(script, {"FAKE_SCD_EXPECT_SAVE": "1"}, size)
    r.trace("removal and re-adding", trace)
    r.ok(any(l.startswith("cleanup=remove") for l in trace), "the removal plan ran")
    r.ok(any(l.startswith("scd=run_exited code=0") for l in trace) and (scd_dir / "backup-ram.bin").read_bytes() == save_before,
         f"Sega CD: the game removed and added again finds its earlier save: {[l for l in trace if 'scd=' in l]}")
    r.ok((scd_dir / "recordings" / "session-1.txt").exists(), "Sega CD: the recordings survived the removal")


# ---------------------------------------------------------------- stage: input
def stage_input(r, c, size):
    print(f"-- input {size}")
    # mouse only: open from the card, Build, Play and More with the pointer
    def mouse(p):
        s = [f"platform_filter {FILTER[p]}", "frames 4", "mouse_card 0", "frames 3", "click", "frames 10", "expect page=details", f"expect detail={TITLE[p]}", "layout"]
        s += ["mouse_action build", "frames 3", "click", "frames 6", "wait_idle", "frames 8", "expect build_outcome=ok", "layout", "mouse_action play", "frames 3", "click", "frames 6", "wait_play_end", "frames 10", "expect playing=0"]
        return s + ["layout", "mouse_action more", "frames 3", "click", "frames 6", "menu_items", "key Escape", "frames 8", "expect page=library"]
    trace, parts = window(c, size, mouse)
    r.trace("mouse", trace)
    for p in PLATFORMS:
        r.ok(any(l.startswith("menu_items ") and len(l) > len("menu_items ") for l in parts.get(p, [])), f"{p}: the More button opens its menu with the pointer")
    # controller only (a virtual gamepad; no mouse event, no keyboard): a ready game is Back -> favorite -> Play, Build is to the right of Play
    hold = c.dir / "pad-heartbeat"
    pad_start = ["frames 10", "pad_attach", "frames 20"]
    def pad_open(p):
        return [f"platform_filter {FILTER[p]}", "frames 6"] + seek(f"card:{TITLE[p]}", "dpad_right", 6) + press("south") + press("dpad_right") + expect(page="details", detail=TITLE[p]) + seek("play", "dpad_down", 4)
    script = list(pad_start)
    for p in PLATFORMS:
        script += pad_open(p) + seek("build", "dpad_right", 3) + press("south") + ["frames 40"] + expect(building=1)
        script += seek("cancel_build", "dpad_down", 12) + press("south") + ["wait_idle", "frames 20"] + expect(building=0, build_outcome="cancelled") + press("east") + expect(page="library")
    script.append("quit")
    trace = c.run(script, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}, size)
    r.trace("gamepad cancel", trace)
    script = list(pad_start)
    for p in PLATFORMS:
        script += pad_open(p) + seek("build", "dpad_right", 3) + press("south") + ["wait_idle", "frames 20"] + expect(build_outcome="ok", can_play=1)
        script += seek("play", "dpad_left", 3) + press("south") + ["frames 10", "wait_play_end", "frames 10"] + expect(playing=0)
        script += seek("more", "dpad_right", 4) + press("south") + ["frames 6", "menu_items"] + press("east") + press("east") + expect(page="library")
    script.append("quit")
    trace = c.run(script, {}, size)
    r.trace("gamepad build, play and menu", trace)
    parts = split_by_platform(trace)
    for p in PLATFORMS:
        r.ok(any(l.startswith("menu_items ") and len(l) > len("menu_items ") for l in parts.get(p, [])), f"{p}: the controller opens the More menu")
    r.ok(not [x for x in c.leftovers() if x.name.endswith(".partial")], f"nothing partial is left behind: {c.leftovers()}")


STAGES = {"journey": stage_journey, "persist": stage_persist, "input": stage_input}


def main():
    exe, fake_cli, fake_game, fake_scd = sys.argv[1:5]
    sizes = [a for a in sys.argv[5:] if "x" in a and a[0].isdigit()] or ["1080x740", "720x520"]
    stages = [a for a in sys.argv[5:] if a in STAGES] or list(STAGES)
    r = Report()
    shots_dir = os.environ.get("SEGARECOMP_PARITY_SHOTS")
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        shots = pathlib.Path(shots_dir) if shots_dir else None
        if shots is not None:
            shots.mkdir(parents=True, exist_ok=True)
        for size in sizes:
            sub = tmp / size
            sub.mkdir()
            c = Case(exe, fake_cli, fake_game, fake_scd, sub, shots)
            for stage in stages:
                STAGES[stage](r, c, size)
    if r.failures:
        print("\n".join(r.failures))
        return 1
    print(f"launcher_parity_gui_test: ok ({r.checked} checks at {', '.join(sizes)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
