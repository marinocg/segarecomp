#!/usr/bin/env python3
"""SEG-150-T006: the per-game Mods tab through the real launcher window (SDL dummy video, software renderer) and the product doubles. Synthetic identities only.

Every check reads the window's own trace and the files on disk, never pixels:
  vocabulary  the status of each installed mod stays distinct and typed: installed, selected_needs_preparation, selected_prepared, installed_prepared, needs_approval
              (an unapproved record) and incompatible_package_missing (the stored package is gone); No mod (original game) is one selection away and Vanilla stays playable
              and byte-identical; selecting a second mod REPLACES the first (one selected variant, never two) with a stated notice
  decline     with a gamepad the native approval sheet opens with focus on Cancel; South declines: nothing is installed or approved, no build starts; approving installs
  refusal     a package for another game revision keeps its typed refusal with a next step, installs nothing
  blocked     while a preparation runs, switching and removing are blocked with a reason (mod_switch=blocked) and the selection does not change
  platform    a Master System game has a Mods tab that says plainly mods are not available for its platform; nothing can be installed there
The real `segarecomp mod pack` makes the packages. MODS_SHOTS=<dir> additionally writes the review captures (sizes in MODS_SIZES, default 1080x740,720x520).

usage: launcher_mods_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game> <segarecomp-cli> <source root>
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
    image[0x150:0x170] = b"MODSTEST                        "
    return bytes(image)


def sms_rom():
    image = bytearray(0x8000)
    for i in range(len(image)):
        image[i] = (i * 31 + 7) & 0xFF
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    return bytes(image)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def manifest_identity(package):
    with zipfile.ZipFile(package) as z:
        return sha(z.read("manifest.json"))


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def expect(**kw):
    return [f"expect {k}={v}" for k, v in kw.items()]


START = ["frames 10", "pad_attach", "frames 20"]
PAD_OPEN = START + press("dpad_down") + seek("card:MODSTEST", "dpad_right", 4) + press("south") + press("dpad_right")
OPEN = ["frames 10", "focus_card 0", "frames 4", "key Return", "frames 8", "expect page=details"]
MODS = ["details_tab mods", "frames 6", "expect details_tab=mods"]


class Profile:
    def __init__(self, tmp, name, exe, fake_cli, fake_game, rom_bytes, filename, size="720x520"):
        self.exe = exe
        self.dir = tmp / name
        self.dir.mkdir()
        self.package = self.dir / "package"
        make_package_root(self.package, fake_cli)
        self.lib, self.cache = self.dir / "lib", self.dir / "cache"
        self.game_log, self.args_log = self.dir / "game.log", self.dir / "args.log"
        self.env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_CACHE_DIR=str(self.cache),
                        SEGARECOMP_LIBRARY_DIR=str(self.lib), SEGARECOMP_HOME=str(self.package), SEGARECOMP_NO_COVER_FETCH="1", SEGARECOMP_SHOT_SIZE=size,
                        FAKE_GAME_BIN=fake_game, FAKE_GAME_LOG=str(self.game_log), FAKE_BUILD_ARGS_LOG=str(self.args_log))
        self.source = rom_bytes
        self.rom = self.dir / filename
        self.rom.write_bytes(rom_bytes)
        done = subprocess.run([exe, "--import", str(self.rom)], env=self.env, timeout=60, capture_output=True, text=True)
        assert "import=added" in done.stdout, done.stdout + done.stderr

    def builds(self):
        games = self.cache / "games"
        return sorted(p for p in games.iterdir() if p.is_dir() and not p.name.endswith((".partial", ".failed"))) if games.exists() else []

    def run(self, script, extra_env=None):
        script_path, trace_path = self.dir / "script.txt", self.dir / "trace.txt"
        script_path.write_text("\n".join(script) + "\n")
        trace_path.unlink(missing_ok=True)
        e = dict(self.env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path), **(extra_env or {}))
        subprocess.run([self.exe], env=e, timeout=300, check=True, capture_output=True)
        return trace_path.read_text().splitlines() if trace_path.exists() else []


class Runner:
    def __init__(self):
        self.failures = []

    def check(self, name, script, trace):
        expected = sum(1 for line in script if line.startswith(("expect ", "pad_seek ")))   # a seek also records its own `expect ok seek=`
        self.failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
        ok = sum(1 for l in trace if l.startswith("expect ok"))
        if ok != expected:
            self.failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")

    def expect(self, condition, message):
        if not condition:
            self.failures.append(message)


def main():
    exe, fake_cli, fake_game, real_cli, root = sys.argv[1:6]
    root = pathlib.Path(root)
    r = Runner()
    shots = os.environ.get("MODS_SHOTS")
    sizes = os.environ.get("MODS_SIZES", "1080x740,720x520").split(",") if shots else ["720x520"]
    fixtures = root / "tests/fixtures/mod_scenery"

    def shot(name, size):
        return [f"shot {pathlib.Path(shots) / (name + '-' + size + '.png')}", "frames 3"] if shots else []

    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        p = Profile(tmp, "g", exe, fake_cli, fake_game, synthetic_rom(1), "game0.md")
        src = p.source

        def pack(name, project_files):
            project = tmp / name
            project.mkdir(parents=True)
            project_files(project)
            out = tmp / (name + ".segapatch")
            done = subprocess.run([real_cli, "mod", "pack", str(project), "--out", str(out)], text=True, capture_output=True, timeout=300)
            assert done.returncode == 0 and json.loads(done.stdout)["ok"], done.stdout
            return out

        def native_files(image):
            def make(project):
                (project / "src").mkdir()
                shutil.copy(fixtures / "provider.c", project / "src/provider.c")
                (project / "mod.toml").write_text((fixtures / "mod.toml").read_text().replace("@@SHA@@", sha(image)))
            return make

        def data_files(project):
            (project / "edits").mkdir()
            (project / "edits/main.edits").write_text("fixture u16 0x00001000 0x%02x%02x 0x1234\n" % (src[0x1000], src[0x1001]))
            (project / "mod.toml").write_text('[mod]\nid = "synthetic-data-edit"\nauthor = "segarecomp tests"\nversion = "0.1.0"\nname = "Synthetic data edit"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "genesis"\n\n'
                                              '[[target.revision]]\nlabel = "fixture"\nsha256 = "%s"\n\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n' % sha(src))

        native, wrong, data = pack("native", native_files(src)), pack("wrong", native_files(synthetic_rom(2))), pack("data", data_files)
        native_id, data_id = manifest_identity(native), manifest_identity(data)
        stored_native, stored_data = p.lib / "patches" / f"{native_id}.segapatch", p.lib / "patches" / f"{data_id}.segapatch"

        # 0. Vanilla first (the reference every later step must leave byte-identical) and the empty Mods tab
        script = OPEN + ["build", "wait_idle", "frames 40"] + expect(build_outcome="ok", status="Ready", patches=0, variant="vanilla") + MODS + expect(mods_available=1, mod_status="-", can_play=1)
        script += ["quit"]
        r.check("vanilla", script, p.run(script))
        vdir = p.builds()[0]
        vexe, vmeta = (vdir / ("game" + EXE_SUFFIX)).read_bytes(), (vdir / "metadata.json").read_text()

        # 1. a refusal keeps its typed message and next step; nothing is installed
        script = OPEN + MODS + [f"patch_pick {wrong}", "frames 12"] + expect(modal=1, patch_notice="This mod was", refusal=1) + ["key Escape", "frames 6"] + expect(modal=0, patches=0, refusal=1) + ["quit"]
        r.check("refusal", script, p.run(script))

        # 2. declined consent from a gamepad: the approval sheet opens on Cancel; South declines; nothing is installed, approved or built
        script = PAD_OPEN + MODS + [f"patch_pick {native}", "frames 14"] + expect(modal=1, modal_code=1, focus_id="mod_cancel") + press("south") + expect(modal=0, patches=0, variant="vanilla")
        script += ["frames 6", "quit"]
        trace = p.run(script)
        r.check("decline", script, trace)
        r.expect(any("patch=declined_approval" in l for l in trace) and not any("build=started" in l for l in trace), f"declining records the decline and starts nothing: {trace!r}")
        r.expect(not stored_native.exists(), "a declined package is not stored")
        # ... and a pad approval installs exactly that identity
        script = PAD_OPEN + MODS + [f"patch_pick {native}", "frames 14"] + expect(modal=1, modal_code=1, focus_id="mod_cancel") + seek("mod_ok", "dpad_right", 3) + press("south") + ["frames 20"]
        script += expect(modal=0, patches=1, variant="vanilla", mod_status="installed", approved="-", patch_built=0, can_play=1) + ["quit"]
        r.check("approve", script, p.run(script))
        r.expect(stored_native.is_file() and sha(stored_native.read_bytes()) == sha(native.read_bytes()), "the stored copy is byte-for-byte the verified package")

        # 3. a data-only mod needs no code approval; the second mod does not make the first "also enabled"
        script = OPEN + MODS + [f"patch_pick {data}", "frames 12"] + expect(modal=1, modal_code=0) + ["patch_confirm", "frames 20"] + expect(modal=0, patches=2, mod_status="installed,installed", variant="vanilla")
        script += ["quit"]
        r.check("data install", script, p.run(script))

        # 4. selecting a mod: needs preparation, Play disabled (no silent fallback); a second selection REPLACES the first
        script = OPEN + MODS + ["variant patched", "frames 30"] + expect(variant="patched", mod_status="selected_needs_preparation,installed", can_play=0, approved=1, mod_switch="enabled")
        script += ["play", "frames 10"] + expect(playing=0)
        script += ["variant patched=1", "frames 20"] + expect(variant="patched", mod_status="installed,selected_needs_preparation", patch_notice="Switched fro")
        script += ["variant vanilla", "frames 20"] + expect(variant="vanilla", mod_status="installed,installed", can_play=1, patch_notice="Original gam")
        script += ["quit"]
        r.check("select", script, p.run(script))
        r.expect(not p.game_log.exists(), "Play must not start Vanilla while a mod is selected and unbuilt")

        # 5. prepare the selected mod: Prepared, then back to the original game: the mod stays Installed - Prepared and Vanilla is untouched
        script = OPEN + MODS + ["variant patched", "frames 10", "build", "wait_idle", "frames 40"] + expect(build_outcome="ok", mod_status="selected_prepared,installed", status="Ready", can_play=1)
        script += ["variant vanilla", "frames 20"] + expect(mod_status="installed_prepared,installed", variant="vanilla") + ["quit"]
        r.check("prepare", script, p.run(script))
        args = p.args_log.read_text().splitlines()[-1].split()
        r.expect("--native-package" in args and args[args.index("--native-identity") + 1] == native_id, f"the preparation was asked for exactly the approved native package: {args}")
        r.expect((vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe and (vdir / "metadata.json").read_text() == vmeta, "Vanilla stays byte-identical")

        # 6. blocked states keep their typed reason: an unapproved record, then a missing package file
        catalog = p.lib / "library.json"
        original_catalog = catalog.read_text()
        doc = json.loads(original_catalog)
        for e in doc.get("entries", doc.get("games", [])):
            for rec in e.get("patches", []):
                if rec.get("native_source_sha256"):
                    rec["native_approved"] = 0
        catalog.write_text(json.dumps(doc))
        script = OPEN + MODS + ["variant patched", "frames 20"] + expect(mod_status="needs_approval,installed", approved=0, can_play=1) + ["play", "frames 20", "frames 6"] + expect(playing=0)
        script += ["variant vanilla", "frames 6", "quit"]
        before = p.game_log.exists() and p.game_log.read_text() or ""
        r.check("unapproved", script, p.run(script))
        r.expect((p.game_log.read_text() if p.game_log.exists() else "") == before, "an unapproved native mod never starts")
        catalog.write_text(original_catalog)
        good = stored_native.read_bytes()
        stored_native.unlink()
        script = OPEN + MODS + expect(mod_status="incompatible_package_missing,installed") + ["quit"]
        r.check("package missing", script, p.run(script))
        stored_native.write_bytes(good)

        # 7. while a preparation runs the selection cannot change (switch and remove are blocked with a reason)
        hold = tmp / "hold"
        script = OPEN + MODS + ["variant patched=1", "frames 10", "build", "frames 30"] + expect(building=1, mod_switch="blocked")
        script += ["variant vanilla", "frames 6"] + expect(variant="patched") + ["cancel_build", "wait_idle", "frames 10", "quit"]
        r.check("blocked", script, p.run(script, {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}))

        # 8. uninstall keeps its confirmation and Vanilla; the selection returns to the original game
        script = OPEN + MODS + ["patch_remove 0", "frames 10"] + expect(modal=1, patches=2) + ["patch_cancel", "frames 6"] + expect(modal=0, patches=2)
        script += ["variant patched", "frames 10", "patch_remove 0", "frames 10", "patch_confirm", "frames 20"] + expect(patches=1, variant="vanilla", mod_status="installed", can_play=1) + ["quit"]
        r.check("uninstall", script, p.run(script))
        r.expect(not stored_native.exists() and (vdir / ("game" + EXE_SUFFIX)).read_bytes() == vexe, "the package is gone and the Vanilla program is untouched")

        # 9. a platform that does not admit mods says so and installs nothing
        q = Profile(tmp, "sms", exe, fake_cli, fake_game, sms_rom(), "game0.sms")
        script = OPEN + MODS + expect(mods_available=0, patches=0, details_tabs="overview,mods,settings,advanced") + [f"patch_pick {native}", "frames 12"] + expect(modal=0, patches=0) + ["quit"]
        r.check("platform", script, q.run(script))

        # review captures (only with MODS_SHOTS): the states a person reviews; each is taken with the Mods tab scrolled to its top edge (the identity header scrolls away)
        if shots:
            pathlib.Path(shots).mkdir(parents=True, exist_ok=True)
            catalog.write_text(original_catalog)
            stored_native.write_bytes(good)   # the uninstall step removed it: restore the original library for the captures
            scroll = ["scroll_page 330", "frames 6"]
            hold_env = {"FAKE_BUILD_HOLD": str(hold), "FAKE_HOLD_MS": "240000"}
            for size in sizes:
                p.env["SEGARECOMP_SHOT_SIZE"] = size
                s = OPEN + MODS + ["variant patched", "frames 20"] + shot("mods-selected-prepared-top", size) + scroll + shot("mods-selected-prepared", size)
                s += ["scroll_page 0", "variant patched=1", "frames 12"] + scroll + shot("mods-switched-needs-prep", size) + ["quit"]
                p.run(s)
                s = OPEN + MODS + ["variant vanilla", "frames 10"] + scroll + shot("mods-vanilla-selected", size) + ["quit"]
                p.run(s)
                stored_native.unlink()   # the installed package file is gone: the row is Incompatible
                p.run(OPEN + MODS + ["variant vanilla", "frames 10"] + scroll + shot("mods-incompatible", size) + ["quit"])
                stored_native.write_bytes(good)
                s = OPEN + MODS + [f"patch_pick {wrong}", "frames 12"] + shot("mods-refusal-sheet", size) + ["key Escape", "frames 8"] + scroll + shot("mods-refusal-notice", size) + ["scroll_page 0", "frames 4"]
                s += ["quit"]
                p.run(s)
                s = OPEN + MODS + ["variant patched=1", "frames 10", "build", "frames 25"] + scroll + shot("mods-blocked-preparing", size) + ["cancel_build", "wait_idle", "quit"]
                p.run(s, hold_env)
                # a fresh library for the install preview of a data-only package and the empty / unavailable states
                e = Profile(tmp, "empty" + size, exe, fake_cli, fake_game, src, "game0.md", size)
                s = OPEN + MODS + scroll + shot("mods-empty", size) + ["scroll_page 0", f"patch_pick {data}", "frames 12"] + shot("mods-preview-data", size) + ["key Escape", "frames 6", f"patch_pick {native}", "frames 12"] + shot("mods-approval-sheet", size) + ["key Escape", "frames 6", "quit"]
                e.run(s)
                m = Profile(tmp, "sms" + size, exe, fake_cli, fake_game, sms_rom(), "game0.sms", size)
                m.run(OPEN + MODS + scroll + shot("mods-unavailable-sms", size) + ["quit"])

    for failure in r.failures:
        print("FAIL:", failure)
    print("launcher_mods_gui_test:", "FAILED" if r.failures else "ok")
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
