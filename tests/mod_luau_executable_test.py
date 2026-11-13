#!/usr/bin/env python3
"""SEG-137-T007 (ADR 0296): a Luau-only HUD mod in the REAL generated-native application path. Not a diagnostic session: this builds a generated-native Genesis program from a fabricated, redistributable,
runnable ROM with the UNMODIFIED product (`segarecomp build`), once Vanilla and once with a verified Luau-only schema 3 package linked in (`--luau-package`), runs both under the real SDL3 viewer
(dummy video/audio drivers, explicit --instruction-budget, wall-clock timeouts) and compares the rendered frames.

Proves: (1) the package resolves to an actual executable and the verified compiled artifact is what the VM loads (identity and bytecode digest agree across `mod build`, status.json and the running
program); (2) the overlay is visible in a genuinely rendered frame: the frames differ only inside the HUD region, the HUD pixels equal an independent derivation from the Vanilla frame, and the
crop's hash is pinned; (3) the run is deterministic; (4) a changed script is a different build with a different frame; (5) a script fault disables only the mod and the program runs on to the same guest
stop as Vanilla and ends on the exact Vanilla frame; (6) every unsupported combination (native, unsupported capability, another platform, wrong identity or image, syntax error, two packages,
a native patch beside it) is refused with a typed class and leaves no executable; a failed build changes nothing else; (7) Vanilla builds contain no Luau; (8) a package with a payload the live path would ignore (an asset) is refused, and a package whose typed edit changes the image is compiled from the EDITED image.
usage: mod_luau_executable_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def read_bmp(path):
    data = pathlib.Path(path).read_bytes()
    offset = int.from_bytes(data[10:14], "little")
    width, height = int.from_bytes(data[18:22], "little", signed=True), int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    assert data[:2] == b"BM" and bpp in (24, 32), (bpp, data[:2])
    step = bpp // 8
    stride = (width * step + 3) & ~3
    rows = []
    for y in range(abs(height)):
        src = (abs(height) - 1 - y) if height > 0 else y
        raw = data[offset + src * stride: offset + src * stride + width * step]
        rows.append([(raw[x * step + 2], raw[x * step + 1], raw[x * step]) for x in range(width)])   # BGR(A) -> RGB
    return rows


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), pathlib.Path(sys.argv[4])
    if sys.platform != "linux":   # like genesis_patch_provider_build_test: the full generated-native viewer build is exercised on the Linux runners
        print("mod_luau_executable_test: skipped (the generated-native viewer build is exercised on Linux)")
        return
    spec = importlib.util.spec_from_file_location("fixture_rom", root / "platforms/genesis/sdk/examples/luau-hud/fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    golden = json.loads((root / "tests/fixtures/mod_luau/hud_live_frame.json").read_text())
    example = root / "platforms/genesis/sdk/examples/luau-hud"
    with tempfile.TemporaryDirectory(prefix="segarecomp-luau-exec-") as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.rom"
        rom.write_bytes(fixture.make_rom())
        rom_sha = hashlib.sha256(rom.read_bytes()).hexdigest()
        home = tmp / "home"
        home.mkdir()
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_MUTE="1", SEGARECOMP_REWIND="off", HOME=str(home),
                   XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data"), LD_LIBRARY_PATH=str(sdl / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""))

        def run_cli(*args, timeout=300):
            return subprocess.run([cli, *map(str, args)], text=True, capture_output=True, timeout=timeout)

        def project(name, script=None, caps=None, platform_id="genesis", template=None):
            if template is None:
                d = tmp / name
                shutil.copytree(example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
                if script is not None:
                    (d / "scripts/main.luau").write_text(script)
                if caps is not None:
                    toml = (d / "mod.toml").read_text()
                    (d / "mod.toml").write_text(toml.split("[capabilities]")[0] + "[capabilities]\nrequired = [" + ", ".join(f'"{c}"' for c in caps) + "]\n")
                return d
            d = tmp / name
            r = run_cli("mod", "init", d, "--platform", platform_id, "--id", name, "--template", template, "--rom-sha256", rom_sha)
            check(r.returncode == 0, f"init {name}: {r.stdout}")
            return d

        def pack(d, build=False):
            out = tmp / (d.name + ".segapatch")
            r = run_cli("mod", "build" if build else "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc["ok"], f"{'build' if build else 'pack'} {d.name}: {r.stdout}")
            return out, doc

        def game_build(out, package=None, identity=None, rom_path=rom, extra=(), cc=None):
            args = ["build", "--rom", rom_path, "--output", out, "--cc", cc or compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1",
                    "--sdl3-include", sdl / "include", "--sdl3-lib", sdl / "lib", "--test-frame-seam", "1"]
            if package is not None:
                args += ["--luau-package", package, "--luau-identity", identity]
            return run_cli(*args, *extra, timeout=900)

        def status(out):
            return json.loads((out / "status.json").read_text())

        def run_game(out, frame, budget=4_000_000):
            run = subprocess.run([str(out / "game"), "--instruction-budget", str(budget)], text=True, capture_output=True, env=dict(env, SEGARECOMP_VIEWER_FINAL_FRAME=str(frame)), timeout=240, cwd=tmp)
            lines = {}
            for line in run.stderr.splitlines():
                tag, _, rest = line.partition(" ")
                if tag in ("VIEWER_SUMMARY", "VIEWER_NATIVE_SUMMARY", "LUAU_MOD_LOADED", "LUAU_MOD_FAULT", "LUAU_MOD_LOG", "RUNTIME_NOTES", "VIEWER_FINAL_FRAME"):
                    lines.setdefault(tag, []).append(json.loads(rest))
            return run, lines

        # ---- Vanilla: the generated-native program of the fixture ROM, no Luau anywhere ----
        van = tmp / "vanilla"
        r = game_build(van)
        check(r.returncode == 0 and (van / "game").is_file(), "the fixture ROM builds into a generated-native program: " + r.stdout[-400:] + r.stderr[-400:])
        check("luau" not in status(van), "a Vanilla status carries no luau member")
        check(b"luau-hud-host" not in (van / "game").read_bytes() and b"lua_newstate" not in (van / "game").read_bytes(), "a Vanilla executable contains no Luau VM and no provider")
        van_frame = tmp / "vanilla.bmp"
        run, van_lines = run_game(van, van_frame)
        check(van_lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop" and van_lines["VIEWER_SUMMARY"][0].get("execution") == "generated-native", "the Vanilla program runs to its typed guest stop: " + run.stderr[-300:])
        check("VIEWER_NATIVE_SUMMARY" not in van_lines, "no provider is attached to a Vanilla program")
        van_frames = van_lines["RUNTIME_NOTES"][0]["virtual_frames"]
        check(van_frames > 100, f"the Vanilla program presented a genuine run of frames ({van_frames})")
        v = read_bmp(van_frame)
        check(len(v) == 672 and len(v[0]) == 960, "the captured frame is the 320x224 picture at scale 3")
        check(v[1][1] != v[400][500], "the Vanilla frame is not a flat colour (VDP tiles over the backdrop)")

        # ---- the Luau-only package, built and linked ----
        hud_dir = project("luau-hud")
        pkg, built = pack(hud_dir, build=True)
        ident = built["package_identity"]
        check(built["trust_class"] == "luau" and built["capabilities"] == ["host.log", "overlay.rgba"], f"the example is Luau-only with the live capabilities: {built}")
        digest = built["luau"]["bytecode_digest"]
        hud = tmp / "hud"
        r = game_build(hud, pkg, ident)
        check(r.returncode == 0 and (hud / "game").is_file(), "the package builds into a generated-native executable: " + r.stdout[-400:] + r.stderr[-400:])

        # ---- SEG-138 (inherited F07 fix): an unknown C driver name is asked to compile C++ with `-x c++` and the C++ runtime library must come AFTER the objects at link time ----
        wrapper = tmp / "customdriver"
        wrapper.write_text(f'#!/bin/sh\nexec "{compiler}" "$@"\n')
        wrapper.chmod(0o755)
        custom = tmp / "hud-custom-driver"
        r = game_build(custom, pkg, ident, cc=str(wrapper))
        check(r.returncode == 0 and (custom / "game").is_file(), "a program with the Luau VM links through a custom C driver (libstdc++ after the objects): " + r.stdout[-400:] + r.stderr[-400:])
        # ---- SEG-138 (inherited F07 fix): a log line with a quotation mark and a backslash stays valid JSON and round-trips ----
        log_pkg, log_built = pack(project("luau-log", script='function reset() sega.log("say \\"hi\\" back\\\\slash plain") end\nfunction render(f) end\n'), build=True)
        logd = tmp / "hud-log"
        r = game_build(logd, log_pkg, log_built["package_identity"])
        check(r.returncode == 0 and (logd / "game").is_file(), "the log-escaping package builds: " + r.stdout[-300:] + r.stderr[-300:])
        if (logd / "game").is_file():
            _, log_lines = run_game(logd, tmp / "log.bmp")
            check([x.get("line") for x in log_lines.get("LUAU_MOD_LOG", [])] == ['say "hi" back\\slash plain'], f"the log line round-trips through JSON: {log_lines.get('LUAU_MOD_LOG')}")
        if (custom / "game").is_file():
            _, custom_lines = run_game(custom, tmp / "custom.bmp")
            check(custom_lines.get("LUAU_MOD_LOADED") is not None, "the custom-driver program runs the Luau mod")
        st = status(hud)["luau"]
        check(st["package_identity"] == ident and st["bytecode_digest"] == digest and st["original_sha256"] == rom_sha and st["effective_sha256"] == rom_sha and st["capabilities"] == "host.log,overlay.rgba",
              f"status.json records package, image, compiler/bytecode digest and capabilities: {st}")
        check(b"luau-hud-host" in (hud / "game").read_bytes(), "the executable contains the Luau provider")
        check(not (hud / "input").exists(), "the materialized effective image does not outlive the build")
        frame_a = tmp / "hud-a.bmp"
        run, lines = run_game(hud, frame_a)
        loaded = lines.get("LUAU_MOD_LOADED", [{}])[0]
        check(loaded.get("package_identity") == ident and loaded.get("bytecode_digest") == digest and loaded.get("capabilities") == "host.log,overlay.rgba",
              f"the running program loaded the verified artifact of this package: {loaded}")
        native = lines.get("VIEWER_NATIVE_SUMMARY", [{}])[0]
        check(native.get("capability") == "luau-hud-host" and native.get("hud_frames", 0) > 100 and native.get("hud_frames") == native.get("frames") and sum(native.get("declined", [1])) == 0,
              f"the viewer drew the overlay on every frame: {native}")
        check(lines["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and lines["RUNTIME_NOTES"][0]["virtual_frames"] == van_frames, "the guest ran exactly as Vanilla (same frames, same guest stop)")
        check("LUAU_MOD_FAULT" not in lines, "no fault")

        # ---- the overlay is in the rendered frame ----
        h = read_bmp(frame_a)
        diff = [(x, y) for y in range(672) for x in range(960) if h[y][x] != v[y][x]]
        check(len(diff) > 2000, f"the HUD frame differs from the Vanilla frame ({len(diff)} pixels)")
        if diff:
            xs, ys = [p[0] for p in diff], [p[1] for p in diff]
            check(max(xs) < 3 * 76 and max(ys) < 3 * 28, f"the frames differ only inside the HUD region (bbox {min(xs)},{min(ys)}-{max(xs)},{max(ys)})")
        # independent derivation: the translucent panel (0x000000C0 over the Vanilla picture), the opaque gold bar (0xFFD040)
        px, py = 3 * 70 + 1, 3 * 1 + 1            # panel only: no text reaches overlay x = 70
        for channel in range(3):
            want = (0 * 192 + v[py][px][channel] * 63 + 127) // 255
            check(abs(h[py][px][channel] - want) <= 2, f"the panel pixel is the documented blend of the Vanilla pixel: channel {channel} {h[py][px][channel]} vs {want}")
        check(h[3 * 25 + 1][3 * 2 + 1] == (0xFF, 0xD0, 0x40), f"the opaque bar pixel is the script's colour: {h[3 * 25 + 1][3 * 2 + 1]}")
        check(any(h[y][x] == (255, 255, 255) for y in range(3 * 3, 3 * 11) for x in range(3 * 4, 3 * 60)), "white text pixels are present in the title line")
        crop = b"".join(bytes(p) for row in h[:90] for p in row[:240])
        crop_hash = hashlib.sha256(crop).hexdigest()
        check(crop_hash == golden["hud_crop_sha256"], f"the rendered HUD crop hash is the pinned one: {crop_hash}")
        check(all(h[y][x] == v[y][x] for y in range(300, 672) for x in range(0, 960, 7)), "the rest of the picture is untouched")

        # ---- deterministic: a second identical run ----
        frame_b = tmp / "hud-b.bmp"
        run_b, lines_b = run_game(hud, frame_b)
        check(frame_a.read_bytes() == frame_b.read_bytes() and hashlib.sha256(frame_a.read_bytes()).hexdigest() == hashlib.sha256(frame_b.read_bytes()).hexdigest(), "two runs render byte-identical frames")
        check(lines_b["VIEWER_NATIVE_SUMMARY"] == lines["VIEWER_NATIVE_SUMMARY"] and lines_b["LUAU_MOD_LOADED"] == lines["LUAU_MOD_LOADED"], "and report identical counters and the same loaded artifact")
        # a rebuild from the same complete inputs is the same build identity (the launcher reuses the entry by key; see launcher_mod_variant_test)
        again = tmp / "hud-again"
        r = game_build(again, pkg, ident)
        check(r.returncode == 0 and status(again)["luau"] == status(hud)["luau"], "the same inputs give the same recorded identity")

        # ---- a changed script is a different package, a different bytecode digest and a different frame ----
        script2 = (example / "scripts/main.luau").read_text().replace('"LUAU HUD"', '"HUD V2"')
        pkg2, built2 = pack(project("luau-hud-v2", script=script2), build=True)
        check(built2["package_identity"] != ident and built2["luau"]["bytecode_digest"] != digest, "a modified script changes the package identity and the bytecode digest")
        hud2 = tmp / "hud2"
        r = game_build(hud2, pkg2, built2["package_identity"])
        check(r.returncode == 0 and status(hud2)["luau"]["bytecode_digest"] == built2["luau"]["bytecode_digest"], "the modified package builds its own executable")
        frame_c = tmp / "hud-c.bmp"
        run_game(hud2, frame_c)
        check(frame_c.read_bytes() != frame_a.read_bytes() and read_bmp(frame_c)[100][700] == v[100][700], "a modified script renders a different HUD frame")

        # ---- failure isolation: a script fault disables only the mod; the guest runs on to the Vanilla end state ----
        script3 = (example / "scripts/main.luau").read_text().replace("function render(frame: number)\n", "function render(frame: number)\n  if frame == 30 then error('boom') end\n")
        pkg3, built3 = pack(project("luau-hud-fault", script=script3), build=True)
        hud3 = tmp / "hud3"
        r = game_build(hud3, pkg3, built3["package_identity"])
        check(r.returncode == 0, "the faulting script builds (a runtime error is not a build error)")
        frame_d = tmp / "hud-d.bmp"
        run, lines3 = run_game(hud3, frame_d)
        fault = lines3.get("LUAU_MOD_FAULT", [{}])
        check(len(fault) == 1 and fault[0].get("cause") == "error" and fault[0].get("frame") == 30 and fault[0].get("callback") == "render", f"one stable fault line names the cause: {fault}")
        check(lines3["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and lines3["RUNTIME_NOTES"][0]["virtual_frames"] == van_frames, "the guest still runs to the same guest stop")
        n3 = lines3["VIEWER_NATIVE_SUMMARY"][0]
        check(n3["hud_frames"] == 30 and n3["declined"][5] > 80, f"the overlay stopped at the fault and the frames were presented without it: {n3}")
        check(read_bmp(frame_d) == v, "after the fault the final frame is exactly the Vanilla frame")

        # ---- the effective image is what is compiled: a package whose typed edit shortens the busy wait runs a visibly shorter program ----
        rom_bytes = rom.read_bytes()
        site = rom_bytes.index(bytes.fromhex("323c0017")) + 3          # the outer loop count of the fixture (24 -> 3)
        edit_dir = project("luau-hud-edit")
        (edit_dir / "mod.toml").write_text((edit_dir / "mod.toml").read_text() + '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n')
        (edit_dir / "edits").mkdir()
        (edit_dir / "edits/main.edits").write_text(f"* u8 0x{site:08x} 0x17 0x02\n")
        pkg_e, doc_e = pack(edit_dir)
        hud_e = tmp / "hud-edit"
        r = game_build(hud_e, pkg_e, doc_e["package_identity"])
        check(r.returncode == 0, "a package with a typed ROM edit builds: " + r.stdout[-300:])
        st_e = status(hud_e)["luau"]
        check(st_e["original_sha256"] == rom_sha and st_e["effective_sha256"] != rom_sha, "the build derived an effective image that differs from the original")
        check(not (hud_e / "input").exists(), "the effective image does not outlive the build")
        run_e, lines_e = run_game(hud_e, tmp / "hud-edit.bmp")
        check(lines_e["RUNTIME_NOTES"][0]["virtual_frames"] < van_frames // 2 and lines_e.get("LUAU_MOD_LOADED", [{}])[0].get("package_identity") == doc_e["package_identity"],
              f"the program that runs is the edited one ({lines_e['RUNTIME_NOTES'][0]['virtual_frames']} frames vs {van_frames})")

        # ---- fail closed: nothing unsupported is ignored; a refusal leaves no executable ----
        before = {p.name: hashlib.sha256((hud / p.name).read_bytes()).hexdigest() for p in (hud / "game", hud / "status.json")}

        def refused(name, cls, package, identity, rom_path=rom, extra=(), rc=None):
            out = tmp / ("refused-" + name)
            r = game_build(out, package, identity, rom_path, extra)
            check(r.returncode != 0 and not (out / "game").exists(), f"{name}: refused, no executable")
            if rc is not None:
                check(r.returncode == rc, f"{name}: usage error {r.returncode}")
            if (out / "status.json").exists():
                s = json.loads((out / "status.json").read_text())
                check(s["status"] == "failed" and s.get("diagnostic") == cls, f"{name}: typed class {cls}: {s}")
            check(not (out / "input").exists(), f"{name}: no derivative image left behind")

        native_dir = project("native-mod", template="c11-provider")
        native_pkg, native_doc = pack(native_dir)
        refused("native", "E_LIVE_TRUST", native_pkg, native_doc["package_identity"])
        wram_pkg, wram_doc = pack(project("wram", caps=("frame.work_ram", "host.log", "overlay.rgba")))
        refused("capability", "E_LIVE_CAPABILITY", wram_pkg, wram_doc["package_identity"])
        ms_pkg, ms_doc = pack(project("ms", template="luau", platform_id="master-system"))
        refused("platform", "E_LIVE_PLATFORM", ms_pkg, ms_doc["package_identity"])
        asset_dir = project("asset-mod")
        (asset_dir / "mod.toml").write_text((asset_dir / "mod.toml").read_text() + '\n[[source]]\nkind = "asset"\npath = "assets/d.bin"\nasset_class = "data"\nformat = "blob"\n')
        (asset_dir / "assets").mkdir()
        (asset_dir / "assets/d.bin").write_bytes(b"unused")
        asset_pkg, asset_doc = pack(asset_dir)
        refused("payload", "E_LIVE_PAYLOAD", asset_pkg, asset_doc["package_identity"])
        refused("identity", "LUAU_IDENTITY_CHANGED", pkg, "0" * 64)
        other_rom = tmp / "other.rom"
        other_rom.write_bytes(rom.read_bytes() + b"\x60\xfe")
        refused("image", "LUAU_IMAGE_MISMATCH", pkg, ident, rom_path=other_rom)
        syntax_pkg, syntax_doc = pack(project("syntax", script="function render(frame)\n  local = 1\nend\n"))
        refused("syntax", "E_LUAU_SYNTAX", syntax_pkg, syntax_doc["package_identity"])
        refused("two packages", "", pkg, ident, extra=("--luau-package", pkg2, "--luau-identity", built2["package_identity"]), rc=2)
        refused("beside a native patch", "", pkg, ident, extra=("--patch-package", pkg, "--patch-identity", ident), rc=2)
        after = {p.name: hashlib.sha256((hud / p.name).read_bytes()).hexdigest() for p in (hud / "game", hud / "status.json")}
        check(before == after, "refused builds changed nothing of an earlier published program")
        check((tmp / "vanilla" / "game").is_file() and run_game(van, tmp / "vanilla2.bmp")[1]["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and (tmp / "vanilla2.bmp").read_bytes() == van_frame.read_bytes(), "the Vanilla program is unchanged and renders the same frame")

        if "--print-golden" in sys.argv:
            print(json.dumps({"hud_crop_sha256": crop_hash}))
    if failures:
        sys.exit(1)
    print("mod_luau_executable_test: ok")


main()
