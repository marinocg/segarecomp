#!/usr/bin/env python3
"""SEG-117: the Widescreen Mode selector of Settings -> Video & Controls, through the real window (SDL dummy video driver, a virtual gamepad).

Checked: the selector is reachable and operable with the gamepad and the keyboard (Return on the focused option); every mode can be chosen and is
persisted in the SHARED viewer settings file (key `video.widescreen`, no launcher-private copy); a relaunch restores it; a value the viewer wrote is
visible in the launcher and the launcher's value is what the viewer reads; older files (no key) and invalid values are Original; Restore video defaults
restores Original; and a change made in the game's menu while a game ran is picked up when the game returns, so a later launcher edit never writes
stale settings back.

usage: launcher_widescreen_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import launcher_default_mode_test as base  # noqa: E402

failures = []


def expect(condition, message):
    if not condition:
        failures.append(message)


def run(name, case, script, env=None):
    """Runs a script and requires every `expect` and every `pad_seek` in it to have run and passed."""
    check_trace(name, case.run(script, env), sum(1 for line in script if line.startswith(("expect ", "pad_seek "))))


def check_trace(name, trace, expected):
    failures.extend(f"{name}: {l}" for l in trace if l.startswith("expect FAIL"))
    ok = sum(1 for l in trace if l.startswith("expect ok"))
    if ok != expected:
        failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")


def settings(case):
    path = case.lib / "viewer-settings.cfg"
    return path.read_text() if path.exists() else ""


def key_of(text, key):
    for line in text.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1]
    return None


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


# the real gamepad route: sidebar "Settings" -> its section list (tab#N) -> the page body (1080x740: the sections are a list, not a drop-down)
START = ["frames 10", "pad_attach", "frames 20"]
TO_BODY = START + seek("Settings", "dpad_down", 4) + press("south") + press("dpad_right") + seek("tab#3", "dpad_down", 6) + press("south") + press("dpad_right")
VIDEO = ["pad_attach", "frames 4", "settings_tab 3", "frames 4"]       # a state, no navigation: the page is just open


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. default Original; gamepad selects Level 2 then Level 3; keyboard Return selects Original again; every change is persisted at once.
        c = base.Case(tmp, "select", exe, fake_cli, fake_game)
        c.env["SEGARECOMP_SHOT_SIZE"] = "1080x740"
        run("select", c, TO_BODY + ["expect settings_tab=3", "expect widescreen=original"] + seek("ws#0", "dpad_down", 20) + seek("ws#1", "dpad_right", 3) + press("south")
            + ["expect widescreen=extended_backgrounds"] + seek("ws#2", "dpad_right", 3) + press("south") + ["expect widescreen=enhanced_gameplay"]
            + seek("ws#0", "dpad_left", 3) + ["key Return", "frames 4", "expect widescreen=original"] + seek("ws#2", "dpad_right", 3) + press("south")
            + ["expect widescreen=enhanced_gameplay", "quit"])
        expect(key_of(settings(c), "video.widescreen") == "enhanced_gameplay", f"the launcher persists the choice in the shared viewer settings file: {settings(c)!r}")
        expect("widescreen" not in (c.lib / "settings.json").read_text() if (c.lib / "settings.json").exists() else True, "no launcher-private widescreen preference")

        # 2. a relaunch restores it
        run("relaunch", c, VIDEO + ["expect widescreen=enhanced_gameplay", "quit"])

        # 3. a value written by the viewer is visible in the launcher; older / invalid / hostile files are Original and other fields survive
        v = base.Case(tmp, "viewer-wrote", exe, fake_cli, fake_game)
        v.lib.mkdir(parents=True, exist_ok=True)
        (v.lib / "viewer-settings.cfg").write_text("video.scale=1\nvideo.widescreen=extended_backgrounds\naudio.volume=40\n")
        run("viewer-wrote", v, VIDEO + ["expect widescreen=extended_backgrounds", "quit"])
        for name, text in (("older", "video.scale=1\naudio.volume=40\n"), ("invalid", "video.widescreen=ultra\n"), ("number", "video.widescreen=2\n"), ("empty", "video.widescreen=\n")):
            o = base.Case(tmp, name, exe, fake_cli, fake_game)
            o.lib.mkdir(parents=True, exist_ok=True)
            (o.lib / "viewer-settings.cfg").write_text(text)
            run(name, o, VIDEO + ["expect widescreen=original", "quit"])

        # 4. Restore video defaults restores Original (and other video defaults)
        r = base.Case(tmp, "defaults", exe, fake_cli, fake_game)
        r.lib.mkdir(parents=True, exist_ok=True)
        (r.lib / "viewer-settings.cfg").write_text("video.widescreen=enhanced_gameplay\nvideo.scale=1\n")
        r.env["SEGARECOMP_SHOT_SIZE"] = "1080x740"
        run("defaults", r, TO_BODY + ["expect widescreen=enhanced_gameplay"] + seek("viewer_defaults", "dpad_down", 120) + press("south") + ["expect widescreen=original", "quit"])
        expect(key_of(settings(r), "video.widescreen") == "original" and key_of(settings(r), "video.scale") == "0", f"defaults restored in the file: {settings(r)!r}")

        # 5. a change made in the game's menu (modelled by the fake game rewriting the shared file) is not lost: the launcher re-reads at game return, and
        #    its next edit keeps it
        g = base.Case(tmp, "return", exe, fake_cli, fake_game)
        base.genesis_rom(g.dir / "g.md")
        g.import_rom(g.dir / "g.md")
        (g.lib / "viewer-settings.cfg").write_text("video.widescreen=extended_backgrounds\n")
        run("return", g, base.OPEN_AND_BUILD + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "expect widescreen=extended_backgrounds", "play", "frames 4", "wait_play_end", "frames 10",
                                             "expect playing=0", "expect widescreen=enhanced_gameplay", "quit"],
            {"FAKE_VIEWER_WIDESCREEN": "enhanced_gameplay"})
        # the launcher then edits an unrelated video setting: the in-game choice survives in the file
        g.env["SEGARECOMP_SHOT_SIZE"] = "1080x740"
        run("launcher edit", g, TO_BODY + ["expect widescreen=enhanced_gameplay"] + seek("sc#0", "dpad_down", 80) + seek("sc#1", "dpad_right", 3) + press("south") + ["quit"])
        expect(key_of(settings(g), "video.widescreen") == "enhanced_gameplay" and key_of(settings(g), "video.scale") == "1",
               f"a launcher edit after a game keeps the in-game widescreen choice: {settings(g)!r}")

        # 6. a damaged settings file at game return keeps the launcher's own state (not defaults)
        h = base.Case(tmp, "damaged", exe, fake_cli, fake_game)
        base.genesis_rom(h.dir / "g.md")
        h.import_rom(h.dir / "g.md")
        (h.lib / "viewer-settings.cfg").write_text("video.widescreen=extended_backgrounds\nvideo.scale=1\naudio.volume=40\n")
        run("damaged", h, base.OPEN_AND_BUILD + ["build", "wait_idle", "frames 6", "expect build_outcome=ok", "expect widescreen=extended_backgrounds", "play", "frames 4", "wait_play_end", "frames 10",
                                                 "expect playing=0", "expect widescreen=extended_backgrounds", "quit"], {"FAKE_VIEWER_DAMAGE": "1"})

    if failures:
        print("\n".join(failures))
        return 1
    print("launcher_widescreen_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
