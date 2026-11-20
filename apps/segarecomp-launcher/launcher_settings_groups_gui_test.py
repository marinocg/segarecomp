#!/usr/bin/env python3
"""SEG-150-T007: Settings has four groups (General, Appearance & Controls, Storage & Data, Advanced), through the real window (SDL dummy video driver).

Checked: a profile written by the previous release (its settings file) opens every group with its policy unchanged and is never rewritten by merely looking at
Settings (no reset, no silent cleanup-policy change); every group is reachable with the keyboard and with a virtual gamepad, in the wide (group list) and the
narrow (drop-down) layout; the effective cache / library / installation locations report which of them an environment variable governs; Back from the cleanup page
returns to Storage & Data.

usage: launcher_settings_groups_gui_test.py <Segarecomp> <fake_segarecomp> <fake_game>
"""
import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import launcher_default_mode_test as base  # noqa: E402

failures = []

# a settings file as the previous release wrote it: non-default values everywhere, an answered first-run question
PREVIOUS = {"schema": 1, "view": "list", "sort": "title", "compact": True, "adopt_cache_on_start": False, "keep_old_artifacts": True, "intermediates_choice_made": True,
            "cleanup_mode": "keep_last_n", "keep_last_n": 7, "soft_limit_mb": 2048, "preferred_regions": ["Japan", "USA"]}


def expect(condition, message):
    if not condition:
        failures.append(message)


def check_trace(name, trace, expected):
    failures.extend(f"{name}: {line}" for line in trace if line.startswith("expect FAIL"))
    ok = sum(1 for line in trace if line.startswith("expect ok"))
    if ok != expected:
        failures.append(f"{name}: {ok} of {expected} expectations ran: {trace!r}")


def run(name, case, script, env=None):
    check_trace(name, case.run(script, env), sum(1 for line in script if line.startswith(("expect ", "pad_seek "))))


def press(button):
    return [f"pad {button} down", f"pad {button} up", "frames 4"]


def seek(target, direction="dpad_down", limit=40):
    return [f"pad_seek {target} {direction} {limit}"]


def make_case(tmp, name, exe, fake_cli, fake_game, size):
    case = base.Case(tmp, name, exe, fake_cli, fake_game)
    case.env["SEGARECOMP_SHOT_SIZE"] = size
    return case


def main():
    exe, fake_cli, fake_game = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)

        # 1. an old profile: every group opens, the loaded policy is the file's, and the file is byte-identical afterwards
        c = make_case(tmp, "legacy", exe, fake_cli, fake_game, "1080x740")
        c.lib.mkdir(parents=True, exist_ok=True)
        settings_file = c.lib / "launcher-settings.json"
        settings_file.write_text(json.dumps(PREVIOUS))
        before = settings_file.read_bytes()
        script = ["frames 6", "expect cleanup_policy=keep_last_n:7:2048", "expect view=list"]
        for group in range(4):
            script += [f"settings_tab {group}", "frames 6", "expect page=settings", f"expect settings_tab={group}", "expect cleanup_policy=keep_last_n:7:2048"]
        script += ["quit"]
        run("legacy", c, script)
        expect(settings_file.read_bytes() == before, "looking at every Settings group does not rewrite an old profile")
        expect(not (c.lib / "launcher-settings.json.tmp").exists(), "no temporary settings file is left behind")

        # 2. keyboard: Tab to the sidebar, Right into the group list, Down + Return per group (wide); the drop-down opened with Return in the narrow layout
        for size in ("1080x740", "720x520"):
            c = make_case(tmp, "keys" + size, exe, fake_cli, fake_game, size)
            wide = size == "1080x740"
            script = ["page settings", "frames 6", "expect page=settings", "key Tab", "frames 2", "expect focus=Settings", "key Right", "frames 4"]
            script += ["expect focus_id=tab#0" if wide else "expect focus_id=settings_section"]
            for group in (1, 2, 3):
                if wide:
                    script += ["key Down", "frames 3", f"expect focus_id=tab#{group}", "key Return", "frames 4", f"expect settings_tab={group}"]
                else:
                    script += ["key Return", "frames 3"] + ["key Down", "frames 3"] * group + ["key Return", "frames 4", f"expect settings_tab={group}"]   # the list opens on its first item
            script += ["key Escape", "frames 6", "expect page=library", "quit"]
            run("keys " + size, c, script)

        # 3. the effective locations: which of the three an environment variable governs
        c = make_case(tmp, "paths", exe, fake_cli, fake_game, "1080x740")
        everything = ["settings_tab 2", "frames 6", "expect settings_tab=2"]
        run("paths all set", c, everything + ["expect effective_paths=cache=env,library=env,home=env", "quit"])
        env = dict(c.env)
        for name in ("SEGARECOMP_LIBRARY_DIR", "SEGARECOMP_HOME"):
            env.pop(name)
        script_path, trace_path = c.dir / "script.txt", c.dir / "trace.txt"
        script_path.write_text("\n".join(everything + ["expect effective_paths=cache=env,library=default,home=default", "quit"]) + "\n")
        trace_path.unlink(missing_ok=True)
        import subprocess
        subprocess.run([exe], env=dict(env, SEGARECOMP_GUI_SCRIPT=str(script_path), SEGARECOMP_GUI_TRACE=str(trace_path)), timeout=180, check=True, capture_output=True)
        check_trace("paths cache only", trace_path.read_text().splitlines(), 2)

        # 4. Settings > Storage & Data opens the cleanup page; Escape returns to the group, not to the library
        for size in ("1080x740", "720x520"):
            c = make_case(tmp, "storage" + size, exe, fake_cli, fake_game, size)
            run("storage " + size, c, ["settings_tab 2", "frames 6", "page storage", "frames 6", "expect page=storage", "key Escape", "frames 6",
                                       "expect page=settings", "expect settings_tab=2", "expect settings_focus_return=none", "quit"])
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    print("launcher_settings_groups_gui_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
