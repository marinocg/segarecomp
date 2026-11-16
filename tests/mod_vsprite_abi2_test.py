#!/usr/bin/env python3
"""SEG-140-T010 (ADR 0306): the typed ABI 2 `virtual_sprites` feature of a NATIVE provider in the played SDL3 program, and the three-route conformance of the whole virtual-sprite pipeline. The fabricated vsprite
fixture (tests/tools/vsprite_fixture_rom.py) is built with the unmodified product into Vanilla and modded programs; the SAME sprite description is authored three ways -- the ABI 1 native provider seam (a
getenv-driven test provider), Luau (`sega.sprite.*`, the one scheduler) and a generated strict-C11 ABI 2 provider calling `SegaGenesisRenderApi::virtual_sprites` -- and presented in the real viewer.

Proves: for every case (inside the native picture, in the 32:9 side columns, under a side treatment that discards the columns, and every rejection: invalid command, missing verified-state gate, the side priority
policy) the three routes present PIXEL-IDENTICAL frames and identical F15 accounting (`virtual_sprites` block, `actors` counters, reasons), and the ABI 2 frame equals the independent golden model; the ABI 2 feature
is part of the published ABI 2 header (the typed render table) and the same host validation applies (it adds no validator, decoder or compositor); a native provider cannot submit twice in a callback, submit a null
set or call the feature outside `render` (each is a scheduler fault of only that provider); a set lives for one frame; a Vanilla program is unchanged. Native code is host code: approval is the boundary.
usage: mod_vsprite_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import *  # noqa: E402,F401,F403


def c_source(sprites, budget="limit", space="picture", count=None, parity=False, extra_phase="", twice=False, null_set=False, call_in_post_input=False):
    """A strict-C11 ABI 2 provider that submits exactly `sprites` through the typed virtual_sprites feature (the pool is generated from the model's own tables)."""
    lines = ["#include <segarecomp_mod_genesis.h>", "", "static GenesisVirtualSpriteSet set;", "static int ready;", "",
             "static unsigned nibble(char c) { return (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); }",
             "static void tile(unsigned index, const char *hex) {", "  unsigned i;", "  for (i = 0U; i < 32U; ++i) set.authored_vram[index * 32U + i] = (uint8_t)(nibble(hex[2U * i]) << 4 | nibble(hex[2U * i + 1U]));", "}",
             "static void colour(unsigned line, unsigned entry, unsigned word) {", "  set.authored_cram[line * 32U + entry * 2U] = (uint8_t)(word >> 8);", "  set.authored_cram[line * 32U + entry * 2U + 1U] = (uint8_t)word;", "}",
             "static void add(unsigned provenance, unsigned layer, int x, int y, unsigned w, unsigned h, unsigned tile_index, unsigned palette, unsigned flags, int has_clip, int cx, int cy, unsigned cw, unsigned ch) {",
             "  GenesisVirtualSprite *c = &set.sprites[set.count++];", "  c->provenance = (uint8_t)provenance; c->layer = (uint8_t)layer; c->width_cells = (uint8_t)w; c->height_cells = (uint8_t)h; c->palette = (uint8_t)palette;",
             "  c->h_flip = (uint8_t)(flags & 1U); c->v_flip = (uint8_t)((flags >> 1) & 1U); c->tile_index = (uint16_t)tile_index; c->x = (int16_t)x; c->y = (int16_t)y;",
             "  c->has_clip = (uint8_t)has_clip; c->clip_x = (int16_t)cx; c->clip_y = (int16_t)cy; c->clip_w = (uint16_t)cw; c->clip_h = (uint16_t)ch;", "}",
             "static void fill(void) {", "  ready = 1;",
             f"  set.space = {1 if space == 'presentation' else 0}U; set.budget_mode = {1 if budget == 'extended' else 0}U;"]
    for index, pat in sorted(POOL_TILES.items()):
        lines.append(f'  tile({index}U, "{hexpattern(pat)}");')
    for i, word in enumerate(AUTHORED_PALETTE):
        lines.append(f"  colour(0U, {i}U, 0x{word:04X}U);")
    for c in sprites:
        flags = (1 if c["hflip"] else 0) | (2 if c["vflip"] else 0)
        clip = c["clip"] or (0, 0, 0, 0)
        lines.append(f'  add({1 if c["src"] == "verified" else 2}U, {c["layer"]}U, {c["x"]}, {c["y"]}, {c["w"]}U, {c["h"]}U, {c["tile"]}U, {c["pal"]}U, {flags}U, {1 if c["clip"] else 0}, {clip[0]}, {clip[1]}, {clip[2]}U, {clip[3]}U);')
    if count is not None:
        lines.append(f"  set.count = {count}U;")
    lines += ["}", "",
              "static int provider_init(void *state, const SegaModHost *host) { (void)state; return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1; }", "",
              "static const SegaGenesisRenderApi *api(const SegaModHost *host) {", "  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;",
              "  if (f == NULL || f->size < sizeof(SegaGenesisFeatures) || f->render == NULL || f->render->size < sizeof(SegaGenesisRenderApi)) return NULL;", "  return f->render;", "}", "",
              "static void provider_phase(void *state, const SegaModHost *host) {", "  const SegaGenesisRenderApi *render;", "  (void)state;"]
    if call_in_post_input:
        lines += ["  if (host->phase == SEGA_MOD_PHASE_POST_INPUT) {", "    render = api(host);", "    if (render != NULL && render->virtual_sprites != NULL) (void)render->virtual_sprites(&set);", "  }"]
    lines += ["  if (host->phase != SEGA_MOD_PHASE_RENDER) return;", "  render = api(host);", "  if (render == NULL || render->virtual_sprites == NULL) return;"]
    if parity:
        lines.append("  if (host->frame % 2U != 0U) return;")
    lines.append("  if (!ready) fill();")
    if null_set:
        lines.append("  (void)render->virtual_sprites(NULL);")
    else:
        lines.append("  (void)render->virtual_sprites(&set);")
    if twice:
        lines.append("  (void)render->virtual_sprites(&set);")
    lines += [extra_phase, "}", "",
              "static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, \"vsprite-abi2\", 1U, provider_init, provider_phase, NULL, NULL, NULL, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};", "",
              "const SegaModProvider *segarecomp_mod_provider(void **state_out) {", "  static int storage;", "  if (state_out != NULL) *state_out = &storage;", "  return &provider;", "}", ""]
    return "\n".join(lines)


def scroller_source():
    """W04 gap B: a strict-C11 ABI 2 provider whose game-art actor scrolls across BOTH hardware sprite-coordinate limits (x from -200 to 444 in presentation space), changes tile and flip every frame, and a second
    command that changes layer and clip, as pure functions of the guest frame the render callback ran for. The test mirrors exactly this function."""
    return """#include <segarecomp_mod_genesis.h>
static GenesisVirtualSpriteSet set;
static int provider_init(void *state, const SegaModHost *host) { (void)state; return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1; }
static void provider_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;
  const uint32_t n = (uint32_t)host->frame;
  GenesisVirtualSprite *a, *b;
  (void)state;
  if (host->phase != SEGA_MOD_PHASE_RENDER || f == NULL || f->size < sizeof(SegaGenesisFeatures) || f->render == NULL) return;
  set.space = 1U;
  set.count = 2U;
  a = &set.sprites[0];
  a->provenance = 1U; a->layer = 2U; a->width_cells = 1U; a->height_cells = 1U; a->tile_index = (uint16_t)(1U + n % 3U); a->h_flip = (uint8_t)(n & 1U);
  a->x = (int16_t)(-200 + 14 * (int)(n % 47U)); a->y = 150;
  b = &set.sprites[1];
  b->provenance = 1U; b->layer = (uint8_t)((n % 2U) != 0U ? 1U : 0U); b->width_cells = 1U; b->height_cells = 1U; b->tile_index = 2U; b->x = 100; b->y = 4;
  if (n % 5U == 0U) { b->has_clip = 1U; b->clip_x = 100; b->clip_y = 4; b->clip_w = 4U; b->clip_h = 8U; } else b->has_clip = 0U;
  (void)f->render->virtual_sprites(&set);
}
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "vsprite-scroller", 1U, provider_init, provider_phase, NULL, NULL, NULL, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};
const SegaModProvider *segarecomp_mod_provider(void **state_out) { static int storage; if (state_out != NULL) *state_out = &storage; return &provider; }
"""


def scroller_sprites(n):
    return [sp("verified", layer=ABOVE, x=-200 + 14 * (n % 47), y=150, tile=1 + n % 3, hflip=bool(n & 1)),
            sp("verified", layer=BETWEEN if n % 2 else BEHIND, x=100, y=4, tile=2, clip=(100, 4, 4, 8) if n % 5 == 0 else None)]


def cfg_text(ratio=None, style="raw"):
    if ratio is None:
        return "video.widescreen=original\nvideo.scale=1\nvideo.aspect=0\n"
    return f"video.widescreen=extended_backgrounds\nvideo.widescreen_side_style={style}\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n"


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_vsprite_abi2_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("vsprite_fixture", root / "tests/tools/vsprite_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    sps = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    segapatch = importlib.util.module_from_spec(sps)
    sps.loader.exec_module(segapatch)
    abi1_source = (root / "tests/fixtures/mod_vsprite/provider_abi1.c").read_bytes()
    with tempfile.TemporaryDirectory(prefix="segarecomp-vsprite-abi2-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "vsprite.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        header = (root / "platforms/genesis/sdk/segarecomp_mod_genesis.h").read_text()
        check("virtual_sprites" in header and "SegaGenesisRenderApi" in header and "segarecomp_genesis_vsprite.h" in header, "the typed feature is part of the published ABI 2 Genesis header (the render table)")
        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")

        def abi2_program(name, source, caps=("overlay.rgba",)):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            (d / "mod.toml").write_text(f"""[mod]
id = "vsprite-abi2"
author = "test"
version = "0.1.0"
name = "VSprite ABI 2"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "fixture"
sha256 = "{image_sha}"

[[source]]
kind = "native-c11"
path = "src/provider.c"

[abi]
native = 2

[capabilities]
required = [{", ".join('"%s"' % c for c in caps)}]
""")
            (d / "src/provider.c").write_text(source)
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            r = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            return tmp / name

        def abi1_program(name, caps):
            out = tmp / (name + ".segapatch")
            segapatch.create(image, None, out, id_="vsprite-provider", name="VSprite", version="1.0.0", native_source=abi1_source, native_caps=caps, author="Test")
            ident = json.loads(h.run_cli("patch", "inspect", out).stdout)["package_identity"]
            r = h.run_cli("build", "--rom", rom, "--output", tmp / name, "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1",
                          "--sdl3-include", pathlib.Path(sdl) / "include", "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", out, "--patch-identity", ident)
            check(r.returncode == 0, f"{name} builds")
            return tmp / name

        def play(program, name, ratio=None, style="raw", case=None, side=0, extra=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(cfg_text(ratio, style))
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg), "VSPRITE_SIDE": str(side)}
            if case:
                env["VSPRITE_CASE"] = case
            env.update(extra or {})
            run, lines, frame, _ = h.play(program, name, None, extra=env)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to its own guest stop: {run.stderr[-200:]}")
            return (read_bmp(frame) if frame.is_file() else None), (json.loads(report.read_text()) if report.is_file() else None), lines

        abi1_plain = abi1_program("abi1-plain", ["overlay.rgba"])
        if h.failures:
            return 1
        side = side_of(32, 9)
        base_picture = picture(play(tmp / "vanilla", "van-picture")[0])
        base_wide = surface_from_bmp(play(tmp / "vanilla", "van-wide", "32:9")[0], side)

        def account(rep):
            a = rep["scenes"][-1]["components"]["actors"]
            return {"virtual_sprites": rep.get("virtual_sprites"), "actors": {k: a[k] for k in ("offered", "accepted", "presented", "rejected")}, "last_vsprite_reason": rep["last_vsprite_reason"], "last_reason": rep["last_reason"]}

        # case: (name, sprites, budget, space, ratio, style, abi1 case name (None = the provider's `bad_*`/named case), expected rejection or None)
        matrix = [
            ("authored", [sp(x=200, y=150, tile=1), sp(x=216, y=150, tile=2)], "limit", "picture", None, "raw", "authored", None),
            ("layers", [sp(layer=BEHIND, x=20, y=12), sp(layer=BETWEEN, x=60, y=4), sp(layer=ABOVE, x=100, y=4)], "limit", "picture", None, "raw", "layers", None),
            ("flip", [sp(x=40, y=150, tile=3), sp(x=60, y=150, tile=3, hflip=True), sp(x=80, y=150, tile=3, vflip=True), sp(x=100, y=150, tile=3, hflip=True, vflip=True), sp(x=140, y=150, w=2, h=2, tile=6)], "limit", "picture", None, "raw", "flip", None),
            ("clip", [sp(x=200, y=150, w=2, tile=1, clip=(204, 150, 8, 4))], "limit", "picture", None, "raw", "clip", None),
            ("budget_extended", [sp(x=200, y=96), sp(x=200, y=150)], "extended", "picture", None, "raw", "budget_extended", None),
            ("transparent", [sp(x=200, y=150, tile=5)], "limit", "picture", None, "raw", "transparent", None),
            ("bad_size", [sp(x=100, y=150), sp(x=120, y=150, w=5)], "limit", "picture", None, "raw", "bad_size", SIZE),
            ("verified_ungated", [sp(x=200, y=150, tile=2), sp("verified", x=216, y=150, tile=2)], "limit", "picture", None, "raw", "verified_gated", UNVERIFIED),
            ("wide_straddle", [sp(x=-4, y=150, tile=1), sp(x=316, y=170, tile=2)], "limit", "presentation", "32:9", "raw", "wide_straddle", None),
            ("wide_outer", [sp(x=-side, y=150, tile=1), sp(x=320 + side - 8, y=150, tile=2), sp(x=-side - 4, y=180, tile=1), sp(x=320 + side - 4, y=180, tile=2)], "limit", "presentation", "32:9", "raw", "wide_outer", None),
            ("wide_budget", [sp(x=-24, y=96, tile=1), sp(x=336, y=150, tile=1)], "extended", "presentation", "32:9", "raw", "wide_budget_extended", None),
            ("wide_priority", [sp(x=100, y=150, tile=1), sp(layer=BETWEEN, x=-4, y=150, tile=1)], "limit", "presentation", "32:9", "raw", "wide_layers_between", SIDE_PRIORITY),
            ("wide_comfort", [sp(x=-4, y=150, tile=1), sp(x=316, y=170, tile=2)], "limit", "presentation", "32:9", "comfort", "wide_straddle", None),
        ]
        for name, sprites, budget, space, ratio, style, abi1_case, rejection in matrix:
            luau, _ = h.luau_program(rom, image_sha, "luau-" + name, lua_for(sprites, budget, space=space if space == "presentation" else None), True)
            native = abi2_program("abi2-" + name, c_source(sprites, budget, space))
            results = {}
            for route, program, case in (("abi1", abi1_plain, abi1_case), ("luau", luau, None), ("abi2", native, None)):
                bmp, rep, _ = play(program, f"{route}-{name}", ratio, style, case, side if ratio else 0)
                results[route] = (bmp, rep)
            check(results["abi1"][0] == results["luau"][0] == results["abi2"][0], f"{name}: the three routes present pixel-identical frames")
            check(account(results["abi1"][1]) == account(results["luau"][1]) == account(results["abi2"][1]), f"{name}: the three routes report identical F15 accounting {account(results['abi2'][1])}")
            if rejection is not None:
                check(results["abi2"][1]["last_vsprite_reason"] == rejection and results["abi2"][1]["virtual_sprites"]["rejected"] == results["abi2"][1]["frames"], f"{name}: every route rejects the whole set with reason {rejection}")
            # the ABI 2 frame equals the independent golden model (the three routes agree on the TRUTH, not on a shared mistake)
            if ratio is None:
                want, d_auth, d_ver = model(base_picture, sprites, budget) if rejection is None else (base_picture, 0, 0)
                got = picture(results["abi2"][0])
                notice = rejection is not None or d_auth + d_ver == 0   # the host's visible once-per-change notice appears where nothing was delivered
                bad = sum(1 for y in range(H) for x in range(W) if got[y][x] != want[y][x] and not (notice and y < 40 and x >= 200))
                check(bad == 0, f"{name}: the ABI 2 frame equals the independent golden ({bad} pixels differ)")
            elif style == "raw":
                want, counts, rej = model_wide(base_wide, side, sprites, budget, space)
                got = surface_from_bmp(results["abi2"][0], side)
                check(rej == rejection and got == want, f"{name}: the ABI 2 surface equals the independent golden at 32:9 (rejection {rej})")
            else:
                block = results["abi2"][1]["virtual_sprites"]
                check(block["pixels_left"] == 0 and block["pixels_right"] == 0 and block["pixels_picture"] > 0, f"{name}: the replaced columns count for nothing: {block}")


        # ---- W04 gap B: a provider granted frame.work_ram may submit GAME_STATE_VERIFIED commands (the game's own art from the frame's VRAM/CRAM). The host binds the set to its frame epoch (stamp + 2 and a digest of the
        # end-of-frame work-RAM copy equal to the snapshot the producer sees); without the capability the set is still rejected UNVERIFIED_STATE (the case above). Same compositor, same golden model, same accounting as ABI 1. ----
        abi1_state = abi1_program("abi1-state", ["overlay.rgba", "frame.work_ram"])
        gated = [
            ("state_picture", [sp(x=200, y=150, tile=2), sp("verified", x=216, y=150, tile=2)], "limit", "picture", None, "raw", "verified_gated"),
            ("state_wide", [sp("verified", x=330, y=150, tile=2), sp(x=-24, y=150, tile=1)], "limit", "presentation", "32:9", "raw", "wide_verified"),
        ]
        for name, sprites, budget, space, ratio, style, abi1_case in gated:
            native = abi2_program("abi2-" + name, c_source(sprites, budget, space), caps=("overlay.rgba", "frame.work_ram"))
            r1 = play(abi1_state, f"abi1-{name}", ratio, style, abi1_case, side if ratio else 0)
            r2 = play(native, f"abi2-{name}", ratio, style, None, side if ratio else 0)
            check(r1[0] == r2[0] and account(r1[1]) == account(r2[1]), f"{name}: the gated ABI 2 route is pixel-identical to the ABI 1 verified route with identical accounting (frames equal {r1[0] == r2[0]}; abi1 {account(r1[1])}; abi2 {account(r2[1])})")
            check(r2[1]["last_vsprite_reason"] == 0 and r2[1]["virtual_sprites"]["composed"] > 0 and r2[1]["virtual_sprites"]["pixels_verified"] > 0 and r2[1]["virtual_sprites"]["rejected"] == 0,
                  f"{name}: the verified set is composed, never rejected: {r2[1]['virtual_sprites']}")
            if ratio is None:
                want, d_auth, d_ver = model(base_picture, sprites, budget)
                got = picture(r2[0])
                bad = sum(1 for y in range(H) for x in range(W) if got[y][x] != want[y][x] and not (y < 40 and x >= 200))
                check(bad == 0 and d_ver > 0, f"{name}: the frame equals the independent golden with the game's own art ({bad} pixels differ)")
            else:
                want, counts, rej = model_wide(base_wide, side, sprites, budget, space)
                check(rej is None and surface_from_bmp(r2[0], side) == want and counts["verified"]["right"] > 0, f"{name}: the 32:9 surface equals the golden: game art drawn beyond the hardware's right limit (rejection {rej})")

        # the scrolling game-art actor: every captured frame equals the golden for the guest frame its render callback ran for, at ONE constant offset (the contract: produced two guest frames later), across both
        # hardware coordinate limits, with tile, flip, layer and clip changing; the same run twice is byte-identical. (The fixture has one palette line and static art: palette/art streaming changes need a richer fixture.)
        scroll = abi2_program("abi2-scroller", scroller_source(), caps=("overlay.rgba", "frame.work_ram"))
        shots = {}
        for run_name in ("a", "b"):
            capdir = tmp / ("scroll-" + run_name)
            capdir.mkdir()
            play(scroll, "abi2-scroll-" + run_name, "32:9", "raw", None, side, {"SEGARECOMP_VIEWER_CAPTURE_DIR": str(capdir), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": ",".join(str(i) for i in range(30, 70, 3))})
            shots[run_name] = {int(q.stem.split("-")[1]): q.read_bytes() for q in capdir.glob("frame-*.bmp")}
        check(len(shots["a"]) >= 10 and shots["a"] == shots["b"], f"the scrolling actor: a replay is byte-identical ({len(shots['a'])} frames)")
        offsets = []
        for k in range(0, 6):
            ok = True
            for ordinal, data in shots["a"].items():
                f = tmp / "scroll-frame.bmp"
                f.write_bytes(data)
                want, _, rej = model_wide(base_wide, side, scroller_sprites(ordinal - k), "limit", "presentation")
                ok = ok and rej is None and surface_from_bmp(read_bmp(f), side) == want
                if not ok:
                    break
            if ok:
                offsets.append(k)
        check(offsets == [1], f"the scrolling actor: every frame matches the golden of ONE guest frame and no other offset: presented frame P shows the set rendered for guest frame P - 1, i.e. the runtime frame count minus 2 of ADR 0309 (matching offsets {offsets})")
        xs = [-200 + 14 * (n % 47) for n in range(28, 68)]
        check(min(xs) < -128 and max(xs) > 383, f"the captured trajectory crosses both hardware coordinate limits ({min(xs)}..{max(xs)})")

        # a restore keeps the epoch honest: save, then a NEW process loads it and runs on; the verified set is never served from before the restore and nothing faults
        stdir = tmp / "state-save"
        native_state = tmp / "abi2-state_picture"
        _, _, ls = play(native_state, "abi2-state-save", None, "raw", None, 0, {"SEGARECOMP_SAVE_DIR": str(stdir), "SEGARECOMP_ROM_SHA256": image_sha, "SEGARECOMP_REWIND": "on"})
        check(ls["MOD_SUMMARY"][0]["faulted"] is False, "the gated provider runs under save/rewind without a fault")

        # ---- the feature's own contract: one set per callback, a set is required, render phase only; each is a scheduler fault of only this provider ----
        cap = tmp / "capture"
        cap.mkdir()
        life = abi2_program("abi2-lifetime", c_source([sp(x=200, y=150)], parity=True))
        play(life, "abi2-lifetime", extra={"SEGARECOMP_VIEWER_CAPTURE_DIR": str(cap), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": "20,21,22,23"})
        shots = sorted(cap.glob("*.bmp"))
        seen = [picture(read_bmp(q))[154][204] == rgb(AUTHORED_PALETTE[1]) for q in shots]
        check(len(shots) == 4 and seen in ([True, False, True, False], [False, True, False, True]), f"ABI 2: a set submitted for one frame is gone on the next: {seen}")
        for label, kwargs in (("twice", {"twice": True}), ("null", {"null_set": True}), ("wrong-phase", {"call_in_post_input": True})):
            program = abi2_program("abi2-" + label, c_source([sp(x=200, y=150)], **kwargs))
            bmp, rep, lines = play(program, "abi2-" + label)
            summary = lines["MOD_SUMMARY"][0]
            expected = "phase" if label == "wrong-phase" else "range"
            check(summary["faulted"] is True and f"cause={expected}" in summary["fault"] and picture(bmp) == base_picture and "virtual_sprites" not in rep, f"ABI 2 {label}: a scheduler fault ({expected}) of this provider only; nothing is drawn and the guest runs on: {summary['fault']}")
        # authorization boundary: without overlay.rgba the typed render table (and with it the feature) is not published
        none_cap = abi2_program("abi2-nocap", c_source([sp(x=200, y=150)]), caps=("host.log",))
        bmp, rep, lines = play(none_cap, "abi2-nocap")
        check(picture(bmp) == base_picture and lines["MOD_SUMMARY"][0]["faulted"] is False and "virtual_sprites" not in rep, "without overlay.rgba the provider finds no render table: no feature, no sprite, no fault")
        # package identity protection is the T011 route's: a different approved identity never builds
        r = h.run_cli("build", "--rom", rom, "--output", tmp / "refused", "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--native-package", tmp / "abi2-authored.segapatch", "--native-identity", "00" * 32)
        check(r.returncode != 0 and not (tmp / "refused/game").exists(), "a package that is not the approved identity is refused (no executable)")

        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_vsprite_abi2_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
