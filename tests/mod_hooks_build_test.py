#!/usr/bin/env python3
"""SEG-138-T012 (ADR 0308): statically resolved function hooks in BUILT Genesis programs. A FABRICATED image (tests/hook_fixture_rom.py: no commercial data) with one hookable subroutine is built with the
UNMODIFIED product into a Vanilla program and into programs carrying a native ABI 2 package that declares `[[hook]]` sites; the hooks are resolved against the exact source image, classified against the
compiled program, linked, and driven by the SAME scheduler as every other phase.

Proves: (1) a no-op before-after hook leaves the headless program bit-identical to Vanilla (frame digests, dispatch count, work RAM) and its counters pair entries with exits; `before` and `after` sites
deliver only their phase; (2) a `replace` hook that commits equals the original's effects, and one that declines is bit-identical to Vanilla; (3) replay twice is identical, and a save/restore round trip
taken while a hook call frame is in flight (and a FRESH program loading that state) reproduce the identical end state; (4) a Luau package declaring hooks is refused at build with a stable class; (5) every
E_HOOK_* class is produced by the CLI build on a crafted project and leaves no executable, including a binding that names no generated site; (6) a package without hooks links no hook code.
usage: mod_hooks_build_test.py <segarecomp-cli> <C compiler> <source root> [<SDL3 prefix>]"""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hook_fixture_rom as fixture  # noqa: E402
from mod_test_support import canon  # noqa: E402
from viewer_mod_support import Harness, sha  # noqa: E402

PROVIDER = r"""#include <segarecomp_mod_genesis.h>
#ifndef HOOK_MODE
#define HOOK_MODE 0   /* 0 observe (no-op callbacks), 1 replace-commit (does what the original does), 2 replace-decline */
#endif
static uint32_t counts[2];
static int provider_init(void *state, const SegaModHost *host) { (void)state; return host->platform == SEGA_MOD_PLATFORM ? 0 : 1; }
static void provider_phase(void *state, const SegaModHost *host) { (void)state; (void)host; }
static int provider_hook(void *state, const SegaModHost *host, uint32_t id) {
  uint32_t d0 = 0, v = 0, res, ccr;
  (void)state; (void)id;
  ++counts[host->phase == SEGA_MOD_PHASE_HOOK_EXIT ? 1 : 0];
  (void)host->read(SEGA_GEN_SPACE_M68K_HOOK_REGS, 0U, 4U, &d0);
  if (HOOK_MODE == 1 && host->phase == SEGA_MOD_PHASE_HOOK_ENTRY) {
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 32U, 2U, &v) != 0) return 0;
    res = (v + 1U) & 0xFFFFU;
    if (host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 32U, 2U, res) != 0) return 0;
    ccr = (v == 0xFFFFU) ? 0x10U : 0U;                                   /* addq.w #1 sets X from the carry; the following move.w #n,d3 clears N Z V C */
    (void)host->write(SEGA_GEN_SPACE_M68K_HOOK_REGS, 3U, 4U, 0xFFFFU);   /* d3 after the dbra loop */
    (void)host->write(SEGA_GEN_SPACE_M68K_HOOK_REGS, 16U, 4U, ccr);
    return 1;
  }
  return 0;
}
static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  uint32_t i;
  (void)state;
  if (cap < 8U) return 1;
  for (i = 0U; i < 8U; ++i) out[i] = (uint8_t)(counts[i / 4U] >> (8U * (3U - i % 4U)));
  *len = 8U;
  return 0;
}
static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  uint32_t i;
  (void)state;
  if (len != 8U) return 1;
  counts[0] = counts[1] = 0U;
  for (i = 0U; i < 8U; ++i) counts[i / 4U] |= (uint32_t)in[i] << (8U * (3U - i % 4U));
  return 0;
}
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "hook-test", 1U, provider_init, provider_phase, provider_hook, provider_save, provider_load, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { if (state_out != NULL) *state_out = counts; return &provider; }
"""

PAD = "10:1:0x10,40:1:0x08"


def toml(rom_sha, hooks, symbols=(("bump", "0x00000400"),), caps=("host.log", "hook.function", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "state.mod"), kind="native"):
    t = f'''[mod]
id = "hook-test"
version = "0.1.0"
name = "Hook test"
author = "tests"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "hookfix"
sha256 = "{rom_sha}"

[[source]]
kind = "{'native-c11' if kind == 'native' else 'luau-source'}"
path = "{'src/provider.c' if kind == 'native' else 'scripts/main.luau'}"

[capabilities]
required = [{", ".join(chr(34) + c + chr(34) for c in caps)}]
'''
    if kind == "native":
        t += "\n[abi]\nnative = 2\n"
    t += "\n[state]\nschema = 1\n\n[[memory]]\nrevision = \"hookfix\"\nspace = \"m68k.work_ram\"\noffset = 32\nlength = 2\naccess = \"read-write\"\n"
    for name, address in symbols:
        t += f'\n[[symbol]]\nname = "{name}"\nrevision = "hookfix"\naddress = "{address}"\nprovenance = "verified"\n'
    return t + hooks


WATCH = '\n[[hook]]\nid = "bump-watch"\nsymbol = "bump"\nmode = "{mode}"\nreads = ["d0", "cycles"]\n'
REPLACE = '\n[[hook]]\nid = "bump-replace"\nsymbol = "bump"\nmode = "replace"\nreads = ["d0"]\nwrites = ["d3", "ccr"]\ncost = 10044\n'


def main():
    cli, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
    sdl = sys.argv[4] if len(sys.argv) > 4 else None
    if sys.platform != "linux":
        print("mod_hooks_build_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    with tempfile.TemporaryDirectory(prefix="segarecomp-hook-build-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl or "/nonexistent", tmp)
        check = h.check
        rom = tmp / "hookfix.rom"
        rom.write_bytes(fixture.make_rom())
        rom_sha = sha(rom.read_bytes())

        def project(name, text, provider=PROVIDER, mode=0):
            d = tmp / name
            (d / "src").mkdir(parents=True)
            (d / "mod.toml").write_text(text)
            (d / "src/provider.c").write_text(provider.replace("#define HOOK_MODE 0", f"#define HOOK_MODE {mode}"))
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout) if r.stdout.strip() else {}
            check(r.returncode == 0 and doc.get("ok"), f"pack {d.name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        def build(name, package=None, identity=None, image=rom, extra=()):
            return h.build(image, tmp / name, package, identity, viewer=False, native=True, extra=extra)

        def run(name, extra_env=None, frames="64"):
            run_, lines = h.headless(tmp / name, name + "-run" + str(len(list(tmp.glob("cap-*")))), PAD, frames=frames, ram="16:48", budget=40_000_000, extra=extra_env)
            summary = lines.get("CAPTURE_SUMMARY", [{}])[0]
            mod = lines.get("MOD_SUMMARY", [{}])[0]
            return run_, summary, mod

        def hooked(name, hooks, mode=0, **kw):
            d = project(name, toml(rom_sha, hooks), mode=mode)
            package, identity = pack(d)
            r = build(name, package, identity)
            check(r.returncode == 0 and (tmp / name / "game").is_file(), f"{name} builds: {r.stdout[-500:]}{r.stderr[-300:]}")
            return package, identity

        # ---- Vanilla and the no-op hooked program ----
        r = h.build(rom, tmp / "vanilla", viewer=False)
        check(r.returncode == 0, "Vanilla program builds: " + r.stdout[-300:])
        _, van, vmod = run("vanilla")
        check(van.get("outcome") == "window_complete" and not vmod, f"Vanilla runs and reports no mod: {van.get('outcome')} {vmod}")

        hooked("watch", WATCH.format(mode="before-after"))
        _, hs, hmod = run("watch")
        hk = hmod.get("hooks", {})
        for key in ("outcome", "dispatches", "digests", "final_work_ram_sha256", "ram_frames", "frames_published"):
            check(hs.get(key) == van.get(key), f"no-op hooked program equals Vanilla: {key}")
        check(hs == van, "the whole capture summary of the no-op hooked program equals Vanilla's")
        check(hk.get("entry", 0) >= 60 and hk["entry"] - hk.get("exit", 0) in (0, 1) and hk.get("abandoned") == 0 and hk.get("unproven") == 0 and hk.get("overflowed") == 0 and not hmod["faulted"],
              f"entries pair with exits (one call may be in flight at the end of the window): {hk} faulted={hmod.get('faulted')}")
        check(hmod["callbacks"] >= hk["entry"] + hk["exit"], "hook callbacks are counted by the scheduler")

        # before-only and after-only sites deliver only their phase
        hooked("before", WATCH.format(mode="before"))
        _, bs, bmod = run("before")
        check(bs == van and bmod["hooks"]["entry"] == hk["entry"] and bmod["hooks"]["exit"] == 0, f"before: only entries: {bmod.get('hooks')}")
        hooked("after", WATCH.format(mode="after"))
        _, a_s, amod = run("after")
        check(a_s == van and amod["hooks"]["entry"] == 0 and amod["hooks"]["exit"] == hk["exit"], f"after: only exits: {amod.get('hooks')}")

        # `mod diff-run` reports the hook counters of the modded run
        dr = h.run_cli("mod", "diff-run", project("dr", toml(rom_sha, WATCH.format(mode="before-after"))), "--rom", rom, "--cc", compiler, "--sdk-dir", root / "platforms", "--frames", "40", "--budget", "40000000",
                       "--timeout", "120", "--input", "10:1:0x10", "--ram", "16:48")
        drdoc = json.loads(dr.stdout) if dr.stdout.strip() else {}
        drhooks = (drdoc.get("modded") or {}).get("mod", {}).get("hooks", {})
        check(dr.returncode == 0 and drhooks.get("entry") == 40 and drhooks.get("exit") == 40, f"mod diff-run reports hook counters: {dr.stdout[:300]} {dr.stderr[-200:]}")

        # ---- replacement: commit = the original's effects; decline = Vanilla ----
        hooked("replace", REPLACE, mode=1)
        _, rs, rmod = run("replace")
        check(not rmod["faulted"] and rmod["hooks"]["entry"] == hk["entry"] and rmod["hooks"]["exit"] == 0, f"replace: every call delivered to the callback: {rmod}")
        check(rs.get("ram_frames") == van.get("ram_frames") and rs.get("final_work_ram_sha256") == van.get("final_work_ram_sha256"), "a committing replacement leaves the same work RAM, frame by frame, as the original function")
        check(rs.get("digests") == van.get("digests"), "a committing replacement renders the same frames")
        check(rmod["committed_writes"] >= 60, f"the replacement's staged RAM writes were committed: {rmod['committed_writes']}")
        hooked("decline", REPLACE, mode=2)
        _, ds, dmod = run("decline")
        check(ds == van and not dmod["faulted"] and dmod["committed_writes"] == 0, f"a declining replacement is bit-identical to Vanilla and commits nothing: {dmod}")

        # ---- replay is deterministic; save/restore with a hook frame in flight ----
        _, rep, repmod = run("watch")
        check(rep == hs and repmod["write_digest"] == hmod["write_digest"] and repmod["hooks"] == hk, "replay: the same program and input reproduce the identical state and counters")
        in_flight = []
        for frame in range(4, 14):
            _, cs, cmod = run("watch", {"SEGARECOMP_MOD_CHECKPOINT": str(frame)})
            rt = cmod.get("roundtrip", {})
            check(rt.get("status") == "ok", f"checkpoint at {frame}: the round trip restores bit-identically: {rt}")
            check(cs == hs and cmod["hooks"]["entry"] == hk["entry"] and cmod["hooks"]["exit"] == hk["exit"], f"checkpoint at {frame}: the run continues to the identical end state")
            if cmod["hooks"].get("checkpoint_depth", 0) > 0:
                in_flight.append(frame)
        check(in_flight, "at least one checkpoint was taken with a hook call frame in flight")
        if in_flight:
            frame = in_flight[0]
            save = tmp / "inflight.state"
            _, ss, smod = run("watch", {"SEGARECOMP_MOD_CHECKPOINT": str(frame), "SEGARECOMP_MOD_SAVE": str(save)})
            check(save.is_file() and smod["roundtrip"]["status"] == "ok", "the in-flight state is saved")
            # a FRESH program loads the state at its start: the pending exit callback is still delivered (the frame was restored), and the run ends where an uninterrupted one would
            _, ls, lmod = run("watch", {"SEGARECOMP_MOD_LOAD": str(save)}, frames="64")
            check(lmod.get("loaded_state_sha256") == smod["roundtrip"]["state_sha256"], "a fresh program restores the container that was saved")
            check(not lmod["faulted"] and lmod["hooks"]["abandoned"] == 0, f"after the load the in-flight frame is not abandoned: {lmod.get('hooks')}")
            corrupt = tmp / "corrupt.state"
            data = bytearray(save.read_bytes())
            data[len(data) // 2] ^= 0xFF
            corrupt.write_bytes(bytes(data))
            c_run, _, _ = run("watch", {"SEGARECOMP_MOD_LOAD": str(corrupt)})
            check("E_STATE_RESTORE" in c_run.stderr, "a corrupted state is refused with a stable class")

        # ---- a program without hooks links none ----
        plain = project("plain", toml(rom_sha, ""))
        pkg, ident = pack(plain)
        check(build("plain-built", pkg, ident).returncode == 0, "a package without hooks builds")
        plain_game = (tmp / "plain-built" / "game").read_bytes()
        check(b"genesis_hook_sites" not in plain_game and b"bump-watch" not in plain_game, "a program without hooks carries no hook table")
        _, ps, pmod = run("plain-built")
        check(ps == van and "hooks" not in pmod, "the hook-free modded program equals Vanilla and reports no hook counters")

        # ---- a Luau package declaring hooks is refused (stable class, no executable) ----
        luau_toml = toml(rom_sha, WATCH.format(mode="before"), caps=("host.log", "hook.function", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "state.mod"), kind="luau")
        ld = tmp / "luau-hooks"
        (ld / "scripts").mkdir(parents=True)
        (ld / "mod.toml").write_text(luau_toml)
        (ld / "scripts/main.luau").write_text("function reset() end\n")
        lpkg = tmp / "luau-hooks.segapatch"
        lr = h.run_cli("mod", "pack", ld, "--out", lpkg)
        ldoc = json.loads(lr.stdout) if lr.stdout.strip() else {}
        if lr.returncode == 0:
            r = h.build(rom, tmp / "luau-built", lpkg, ldoc["package_identity"], viewer=False)
            doc = json.loads((tmp / "luau-built" / "status.json").read_text())
            check(r.returncode != 0 and doc.get("diagnostic") == "E_HOOK_CALLBACK_UNSUPPORTED" and not (tmp / "luau-built" / "game").exists(), f"a Luau package with hooks is refused at build: {r.stdout[-200:]} {doc.get('diagnostic')}")
        else:
            check(ldoc.get("class", "").startswith("E_HOOK_"), f"a Luau package with hooks is refused already at pack with a stable class: {lr.stdout[:200]}")

        # ---- every E_HOOK_* class from the CLI build; no executable remains ----
        def refused(name, cls, image=rom, text=None, forge=None, extra=(), rom_bytes=None):
            if rom_bytes is not None:
                image = tmp / (name + ".rom")
                image.write_bytes(rom_bytes)
            image_sha = sha(image.read_bytes())
            d = project(name, text if text is not None else toml(image_sha, WATCH.format(mode="before-after")))
            if forge is None:
                package, identity = pack(d)
            else:
                good = tmp / (name + "-good.segapatch")
                r0 = h.run_cli("mod", "pack", project(name + "-good", toml(image_sha, WATCH.format(mode="before-after"))), "--out", good)
                check(r0.returncode == 0, f"{name}: the base package packs")
                with zipfile.ZipFile(good) as z:
                    manifest = json.loads(z.read("manifest.json"))
                    entries = [(i, z.read(i.filename)) for i in z.infolist() if i.filename != "manifest.json"]
                forge(manifest)
                package = tmp / (name + ".segapatch")
                from mod_test_support import make_zip
                package.write_bytes(make_zip([("manifest.json", canon(manifest))] + [(i.filename, data) for i, data in entries]))
                identity = "0" * 64
            r = build(name, package, identity, image=image, extra=extra)
            status_path = tmp / name / "status.json"
            doc = json.loads(status_path.read_text()) if status_path.is_file() else {}
            check(r.returncode != 0 and doc.get("diagnostic") == cls and not (tmp / name / "game").exists(), f"{name}: {cls} at build, no executable (got rc={r.returncode} {doc.get('diagnostic')} {r.stdout[-200:]})")

        def at(address, hook_mode="before-after"):
            return toml(rom_sha, WATCH.format(mode=hook_mode), symbols=(("bump", address),))

        refused("mutable", "E_HOOK_MUTABLE_CODE", text=at("0x00ff0100"))
        refused("mid", "E_HOOK_MID_INSTRUCTION", text=at("0x00000402"))
        refused("uncompiled", "E_HOOK_NOT_COMPILED", text=at("0x00100000"))
        refused("reset-entry", "E_HOOK_NOT_FUNCTION_ENTRY", text=at("0x00000200"))
        refused("uncalled", "E_HOOK_NOT_FUNCTION_ENTRY", text=at("0x00000440"))
        c_rom = fixture.make_rom(computed=True)
        refused("computed", "E_HOOK_COMPUTED_TARGET", text=toml(sha(c_rom), WATCH.format(mode="before"), symbols=(("bump", "0x00000440"),)), rom_bytes=c_rom)
        v_rom = fixture.make_rom(vector_target=fixture.BUMP_ADDRESS)
        refused("interrupt", "E_HOOK_INTERRUPT_CONTEXT", rom_bytes=v_rom)
        r_rom = fixture.make_rom(rte_call=True)
        refused("rte", "E_HOOK_UNCLASSIFIED_RETURN", text=toml(sha(r_rom), WATCH.format(mode="after"), symbols=(("bump", "0x00000460"),)), rom_bytes=r_rom)
        refused("digest", "E_HOOK_DIGEST", rom_bytes=fixture.make_rom(loop=500), text=toml(rom_sha, WATCH.format(mode="before-after")))
        refused("binding", "E_HOOK_BINDING", extra=("--test-hook-binding", "nowhere"))

        def hooks_of(m):
            return m["hooks"]

        def overlap(m):
            m["hooks"].append(dict(m["hooks"][0], id="bump-watch2"))
            m["hooks"].sort(key=lambda x: x["id"])

        def replace_contract(m):
            m["hooks"][0]["mode"] = "replace"
            m["hooks"][0]["replace"] = {"writes": ["d0"], "cost": 0}

        refused("overlap", "E_HOOK_OVERLAP", forge=overlap)
        refused("contract", "E_HOOK_REPLACE_CONTRACT", forge=replace_contract)
        refused("mode", "E_HOOK_MODE", forge=lambda m: m["hooks"][0].update(mode="around"))
        refused("id", "E_HOOK_ID", forge=lambda m: m["hooks"][0].update(id="Bad Id"))
        refused("register", "E_HOOK_REGISTER", forge=lambda m: m["hooks"][0].update(reads=["d9"]))
        refused("revision", "E_HOOK_REVISION", forge=lambda m: m["hooks"][0].update(revision="rev-z"))
        refused("cpu", "E_HOOK_CPU_UNSUPPORTED", forge=lambda m: m["hooks"][0].update(cpu="z80"))
        refused("count", "E_HOOK_COUNT", forge=lambda m: m.update(hooks=[dict(m["hooks"][0], id=f"h{i:02d}") for i in range(65)]))

        # ---- the played SDL3 program links the SAME seam (mod_capture.cpp, -DSEGARECOMP_MOD_VIEWER): hooks run there too ----
        if sdl and (pathlib.Path(sdl) / "include").is_dir():
            vv = h.build(rom, tmp / "vanilla-v", viewer=True)
            check(vv.returncode == 0, "Vanilla viewer program builds: " + vv.stdout[-300:])
            _, _, vframe, vram = h.play(tmp / "vanilla-v", "vanilla-v", budget=40_000_000)
            for name, hooks, mode in (("watch-v", WATCH.format(mode="before-after"), 0), ("replace-v", REPLACE, 1), ("decline-v", REPLACE, 2)):
                d = project(name, toml(rom_sha, hooks), mode=mode)
                package, identity = pack(d)
                rv = h.build(rom, tmp / name, package, identity, viewer=True, native=True)
                check(rv.returncode == 0, f"{name} viewer program builds: {rv.stdout[-400:]}{rv.stderr[-300:]}")
                run_, lines, frame, ram = h.play(tmp / name, name, budget=40_000_000)
                loaded = (lines.get("MOD_LOADED") or [{}])[0]
                check(loaded.get("hooks") == 1, f"{name}: the played program reports the linked hook: {loaded}")
                check(frame.is_file() and ram.is_file() and ram.read_bytes() == vram.read_bytes() and frame.read_bytes() == vframe.read_bytes(),
                      f"{name}: the played program ends in the Vanilla frame and work RAM")

        if h.failures:
            print(f"mod_hooks_build_test: {len(h.failures)} failure(s)")
            return 1
    print("mod_hooks_build_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
