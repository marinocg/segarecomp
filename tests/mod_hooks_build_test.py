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

        # `mod test` reports them in the scenario's observation, and a hook-free mod omits the field
        scen = ('[[scenario]]\nname = "hooked"\nframes = 24\nbudget = 20000000\ntimeout = 120\ninput = "10:1:0x10"\nram = ["16:48"]\n')
        for pname, with_hooks in (("mt-hooked", True), ("mt-plain", False)):
            pd = project(pname, toml(rom_sha, WATCH.format(mode="before-after") if with_hooks else ""))
            (pd / "tests.toml").write_text(scen)
            tr = h.run_cli("mod", "test", pd, "--rom", rom, "--cc", compiler, "--sdk-dir", root / "platforms")
            tdoc = json.loads(tr.stdout) if tr.stdout.strip() else {}
            obs = (tdoc.get("scenarios") or [{}])[0].get("observed", {})
            if with_hooks:
                hh = obs.get("hooks", {})
                check(tr.returncode == 0 and tdoc.get("ok") and hh.get("entry", 0) >= 20 and hh.get("exit", 0) >= 19 and hh.get("abandoned") == 0 and hh.get("unproven") == 0 and hh.get("overflowed") == 0,
                      f"mod test reports hook counters in the scenario observation: {tr.stdout[:400]} {tr.stderr[-200:]}")
                check("final_frame_sha256" in obs and "final_work_ram_sha256" in obs, "the existing observation fields are unchanged")
            else:
                check(tr.returncode == 0 and tdoc.get("ok") and "hooks" not in obs and "final_work_ram_sha256" in obs, f"a hook-free mod's mod test observation has no hooks field: {tr.stdout[:300]}")

        # ---- the `--function-hooks` plan is an external input: every malformed plan is rejected before anything is generated ----
        good_site = "site bump-watch 00000400 before-after 00020001 00000000 0 0 0"
        plans = {
            "empty": "",
            "no-site": "vector 00000500\n",
            "oversized": ("vector 00000500\n" + good_site + "\n").ljust(20000, "\n"),
            "too-many-sites": "".join(f"site h{i} {0x400 + 2 * i:08x} before 00000000 00000000 0 0 0\n" for i in range(65)),
            "bad-hex": "site a 0000040g before 00000000 00000000 0 0 0\n",
            "uppercase-hex": "site a 0000040A before 00000000 00000000 0 0 0\n",
            "short-hex": "site a 400 before 00000000 00000000 0 0 0\n",
            "overflow-cost": "site a 00000400 before 00000000 00000000 0 0 99999999999999999999\n",
            "cost-over-16-bit": "site a 00000400 replace 00000000 00000000 1 0 65536\n",
            "overflow-hex": "site a 100000400 before 00000000 00000000 0 0 0\n",
            "missing-field": "site a 00000400 before 00000000 00000000 0 0\n",
            "extra-field": good_site + " 7\n",
            "trailing-space": good_site + " \n",
            "bad-mode": "site a 00000400 around 00000000 00000000 0 0 0\n".replace("00000400", "00000400"),
            "duplicate-id": good_site + "\nsite bump-watch 00000440 before 00000000 00000000 0 0 0\n",
            "duplicate-address": good_site + "\nsite other 00000400 before 00000000 00000000 0 0 0\n",
            "bad-bool": "site a 00000400 before 00000000 00000000 2 0 0\n",
            "bad-id": "site Bad_Id 00000400 before 00000000 00000000 0 0 0\n",
            "oversized-line": "site " + "a" * 300 + " 00000400 before 00000000 00000000 0 0 0\n",
            "unexpected-record": good_site + "\nhook x\n",
            "vector-extra": good_site + "\nvector 00000500 1\n",
            "vector-bad": good_site + "\nvector 5\n",
            "too-many-vectors": good_site + "\n" + "".join(f"vector {0x500 + 2 * i:08x}\n" for i in range(63)),
        }
        for pname, body in plans.items():
            pf = tmp / f"plan-{pname}.txt"
            pf.write_text(body)
            shard = tmp / f"plan-out-{pname}"
            er = h.run_cli("emit-general-startup-bridge-c", "--rom", rom, "--reset-entry", "--rom-sha256", rom_sha, "--immutable-rom-aot", "--function-hooks", pf, "--generated-c-shard-dir", shard)
            check(er.returncode == 2 and "function-hook plan rejected" in er.stderr and not shard.exists(), f"plan {pname}: rejected cleanly (rc={er.returncode}) {er.stderr[-160:]}")
        er = h.run_cli("emit-general-startup-bridge-c", "--rom", rom, "--reset-entry", "--rom-sha256", rom_sha, "--immutable-rom-aot", "--function-hooks", tmp / "no-such-plan.txt", "--generated-c-shard-dir", tmp / "plan-out-missing")
        check(er.returncode == 2 and "cannot read the plan" in er.stderr and not (tmp / "plan-out-missing").exists(), f"a missing plan is rejected: {er.stderr[-160:]}")
        okplan = tmp / "plan-ok.txt"
        okplan.write_text("vector 00000500\n" + good_site + "\n")
        er = h.run_cli("emit-general-startup-bridge-c", "--rom", rom, "--reset-entry", "--rom-sha256", rom_sha, "--immutable-rom-aot", "--function-hooks", okplan, "--generated-c-shard-dir", tmp / "plan-out-ok")
        check(er.returncode == 0 and (tmp / "plan-out-ok" / "bridge_generated.units").is_file(), f"the product-generated plan format is accepted: {er.stderr[-200:]}")

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
            # a FRESH program loads the state at its start. The container holds the state at the checkpoint's logical frame (`frame`); the fresh program then runs 64 - frame further frames of the same input, so it
            # must end at the SAME logical endpoint as the uninterrupted 64-frame reference: the in-flight call frame survives only if its pending exit callback is delivered after the load.
            # (the checkpoint is taken at the first slice boundary at or after `frame`, i.e. at logical frame `frame`; a capture window of N frames after a load ends at logical frame frame + N - 1)
            _, ls, lmod = run("watch", {"SEGARECOMP_MOD_LOAD": str(save)}, frames=str(65 - frame))
            check(lmod.get("loaded_state_sha256") == smod["roundtrip"]["state_sha256"], "a fresh program restores the container that was saved")
            lh = lmod.get("hooks", {})
            check(not lmod["faulted"] and lh.get("abandoned") == 0 and lh.get("unproven") == 0 and lh.get("overflowed") == 0, f"after the load nothing is abandoned, skipped or faulted: {lh}")
            check(ls.get("final_work_ram_sha256") == hs.get("final_work_ram_sha256") and ls.get("digests", [None])[-1] == hs["digests"][-1], "the resumed program ends in the reference's work RAM and final frame")
            check(lh.get("entry") == hk["entry"] and lh.get("exit") == hk["exit"], f"the pending exit callback of the in-flight call is delivered after the load (totals equal the uninterrupted reference): {lh} vs {hk}")
            check(lmod["write_digest"] == hmod["write_digest"] and lmod["callbacks"] == hmod["callbacks"] and lmod["committed_writes"] == hmod["committed_writes"], "the resumed program's committed-write digest and callback count equal the reference's")
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
            counters = {}
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
                summary = (lines.get("MOD_SUMMARY") or [{}])[0]
                hk_v = summary.get("hooks", {})
                counters[name] = (hk_v, summary)
                check(summary and not summary.get("faulted") and hk_v.get("entry", 0) >= 60 and hk_v.get("abandoned") == 0 and hk_v.get("unproven") == 0 and hk_v.get("overflowed") == 0,
                      f"{name}: the SDL3 executable delivered hook callbacks and skipped none: {summary}")
            wv, rv_, dv = counters["watch-v"], counters["replace-v"], counters["decline-v"]
            check(wv[0].get("exit", 0) > 0 and wv[0]["entry"] - wv[0]["exit"] in (0, 1), f"watch-v: entries pair with exits: {wv[0]}")
            check(rv_[0].get("exit") == 0 and rv_[0].get("entry") == wv[0].get("entry") and rv_[1].get("committed_writes", 0) >= 60, f"replace-v: every call replaced and the replacement's writes committed: {rv_}")
            check(dv[0].get("exit") == 0 and dv[0].get("entry") == wv[0].get("entry") and dv[1].get("committed_writes") == 0, f"decline-v: every call offered, nothing committed: {dv}")
            # a deliberately observable effect: the replacement skips the original's RAM increment and writes a marker instead, so an accidentally disconnected dispatch would leave 0x20 at the Vanilla value
            d = project("marker-v", toml(rom_sha, REPLACE), provider=PROVIDER.replace("res = (v + 1U) & 0xFFFFU;", "res = 0x1234U;"), mode=1)
            package, identity = pack(d)
            check(h.build(rom, tmp / "marker-v", package, identity, viewer=True, native=True).returncode == 0, "marker viewer program builds")
            _, mlines, _, mram = h.play(tmp / "marker-v", "marker-v", budget=40_000_000)
            check(mram.is_file() and vram.is_file() and mram.read_bytes()[0x20:0x22] == b"\x12\x34" and vram.read_bytes()[0x20:0x22] != b"\x12\x34", "the played program's work RAM shows the replacement's marker (the callback really ran)")
            print("mod_hooks_build_test: SDL3 viewer section ran")

        if h.failures:
            print(f"mod_hooks_build_test: {len(h.failures)} failure(s)")
            return 1
    print("mod_hooks_build_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
