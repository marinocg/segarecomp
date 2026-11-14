"""The INDEPENDENT golden model of the virtual-sprite viewer tests (SEG-140-T008/T009/T010, F28): written in Python from the contract of the fixture (tests/tools/vsprite_fixture_rom.py) and the rules of ADR 0303 / 0307.
Nothing here calls, links or copies the compositor under test: pattern, flip, palette and priority arithmetic is redone from first principles on the Vanilla frame the same program renders."""
import itertools  # noqa: F401  (kept for the callers that import * from here)

W, H = 320, 224
FIXTURE_PALETTE = (0x0420, 0x00E0, 0x000E, 0x0E00)
AUTHORED_PALETTE = (0x0000, 0x0E0E, 0x00EE)
UNVERIFIED, TOO_MANY, PROVENANCE, SIZE, TILE_RANGE, PALETTE, LAYER, POSITION, CLIP, RESERVED, BUDGET_MODE = 3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11
BEHIND, BETWEEN, ABOVE = 0, 1, 2


# ---------------------------------------------------------------- the independent golden model
def rgb(word):
    return ((word >> 1 & 7) * 255 // 7, (word >> 5 & 7) * 255 // 7, (word >> 9 & 7) * 255 // 7)


def solid(c):
    return [[c] * 8 for _ in range(8)]


def checker(a, b):
    return [[a if (x + y) % 2 == 0 else b for x in range(8)] for y in range(8)]


def corners():
    p = [[0] * 8 for _ in range(8)]
    p[0][0], p[7][7] = 1, 2
    return p


FIXTURE_TILES = {1: solid(1), 2: checker(1, 2), 3: solid(3)}
POOL_TILES = {1: solid(1), 2: solid(2), 3: corners(), 6: solid(1), 7: solid(2), 8: checker(1, 2), 9: solid(1)}


def native_class(x, y):
    """The fixture's native priority class of picture pixel (x, y): plane A pattern 1 on cell rows 0 (low) and 1 (high); 20 low-priority hardware sprite cells on rows 96-103 (x < 160)."""
    if y < 8:
        return "low"
    if y < 16:
        return "high"
    if 96 <= y < 104 and x < 160:
        return "low"
    return "backdrop"


def hw_row_sprites(y):
    return 20 if 96 <= y < 104 else 0


def model(base, sprites, budget="limit"):
    """The expected picture: `base` (the Vanilla frame as rows of RGB) with `sprites` composed. Each sprite: dict(src='authored'|'verified', layer, x, y, w, h, tile, pal, hflip, vflip, clip=(x,y,w,h)|None).
    Returns (picture, drawn_authored, drawn_verified). First command wins; the tests avoid overlapping sprites except where stated."""
    out = [row[:] for row in base]
    claimed = set()
    per_row = {}
    drawn = {"authored": 0, "verified": 0}
    for s in sprites:
        tiles = FIXTURE_TILES if s["src"] == "verified" else POOL_TILES
        palette = FIXTURE_PALETTE if s["src"] == "verified" else AUTHORED_PALETTE
        for py in range(s["h"] * 8):
            y = s["y"] + py
            if not 0 <= y < H:
                continue
            if budget == "limit":   # the original per-line sprite limit (20 in a 40-cell picture), hardware sprites first; a command that finds no slot on a row is dropped on that row
                if hw_row_sprites(y) + per_row.get(y, 0) >= 20:
                    continue
            per_row[y] = per_row.get(y, 0) + 1
            for px in range(s["w"] * 8):
                x = s["x"] + px
                if not 0 <= x < W or (x, y) in claimed:
                    continue
                if s["clip"] is not None:
                    cx, cy, cw, ch = s["clip"]
                    if not (cx <= x < cx + cw and cy <= y < cy + ch):
                        continue
                sx, sy = (s["w"] * 8 - 1 - px) if s.get("hflip") else px, (s["h"] * 8 - 1 - py) if s.get("vflip") else py
                cell = (sx // 8) * s["h"] + sy // 8   # column-major cell order, exactly like a hardware sprite
                idx = tiles[s["tile"] + cell][sy % 8][sx % 8] if (s["tile"] + cell) in tiles else 0
                if idx == 0:
                    continue
                cls = native_class(x, y)
                if (s["layer"] == BEHIND and cls != "backdrop") or (s["layer"] == BETWEEN and cls == "high"):
                    continue
                out[y][x] = rgb(palette[idx])
                claimed.add((x, y))
                drawn[s["src"]] += 1
    return out, drawn["authored"], drawn["verified"]


def sp(src="authored", layer=ABOVE, x=0, y=0, w=1, h=1, tile=1, pal=0, hflip=False, vflip=False, clip=None):
    return dict(src=src, layer=layer, x=x, y=y, w=w, h=h, tile=tile, pal=pal, hflip=hflip, vflip=vflip, clip=clip)


TOAST = (200, 0, 320, 40)   # picture pixels where the host's once-per-change notice ("Presentation: ...") appears (x0, y0, x1, y1); the sprites of these tests are never placed there


def outside_toast(x, y):
    return not (TOAST[0] <= x < TOAST[2] and TOAST[1] <= y < TOAST[3])


def mismatches(img, want):
    return sum(1 for y in range(H) for x in range(W) if outside_toast(x, y) and img[y][x] != want[y][x])


def toast_shown(img, base):
    return any(img[y][x] != base[y][x] for y in range(TOAST[1], TOAST[3]) for x in range(TOAST[0], TOAST[2]))


def picture(bmp):
    """The 320 x 224 picture from a 960 x 672 window capture (an exact 3x picture in Original mode): the centre sample of every source pixel."""
    return [[bmp[3 * y + 1][3 * x + 1] for x in range(W)] for y in range(H)]




# ---- ADR 0307: the PRESENTATION space (side columns) ----
SIDE_PRIORITY = 13
RESERVED = 10


def side_of(ratio_num, ratio_den):
    """The planner's extension width per side for a 320 x 224 picture with square pixels: floor(224 * num / den) wide in total (SvGeom planner), half of the extra on each side (0 when the extra is 1 or less)."""
    extra = (H * ratio_num) // ratio_den - W
    return 0 if extra <= 1 else extra // 2


def surface_from_bmp(bmp, side):
    """The (W + 2*side) x H presented surface from a 960 x 672 window capture under integer scaling: the whole number of window pixels per surface pixel, the surface centred."""
    ws = W + 2 * side
    f = max(1, min(960 // ws, 672 // H))
    ox, oy = (960 - f * ws) // 2, (672 - f * H) // 2
    return [[bmp[oy + f * y + f // 2][ox + f * x + f // 2] for x in range(ws)] for y in range(H)]


def model_wide(base, side, sprites, budget="limit", space="presentation", visible=None):
    """`base`: the Vanilla surface (rows of W + 2*side RGB pixels). Returns (surface, counts, rejection): counts[origin][region] with origin 'authored'|'verified' and region 'picture'|'left'|'right';
    rejection is the F14/ADR 0307 reason code of a whole-set rejection (the surface is then the base) or None."""
    vis = side if visible is None else visible
    ext = space == "presentation" and vis > 0
    counts = {o: {"picture": 0, "left": 0, "right": 0} for o in ("authored", "verified")}
    lo, hi = (-vis, W + vis) if ext else (0, W)
    if ext:
        for s in sprites:
            if s["layer"] == ABOVE:
                continue
            x0, x1 = max(s["x"], lo), min(s["x"] + s["w"] * 8, hi)
            y0, y1 = max(s["y"], 0), min(s["y"] + s["h"] * 8, H)
            if s["clip"] is not None:
                cx, cy, cw, ch = s["clip"]
                x0, x1, y0, y1 = max(x0, cx), min(x1, cx + cw), max(y0, cy), min(y1, cy + ch)
            if x0 < x1 and y0 < y1 and (x0 < 0 or x1 > W):
                return [row[:] for row in base], counts, SIDE_PRIORITY
    out = [row[:] for row in base]
    claimed, per_row = set(), {}
    for s in sprites:
        tiles = FIXTURE_TILES if s["src"] == "verified" else POOL_TILES
        palette = FIXTURE_PALETTE if s["src"] == "verified" else AUTHORED_PALETTE
        for py in range(s["h"] * 8):
            y = s["y"] + py
            if not 0 <= y < H:
                continue
            if s["clip"] is not None and not (s["clip"][1] <= y < s["clip"][1] + s["clip"][3]):
                continue   # a sprite clipped away on a row does not occupy it
            if budget == "limit":
                if hw_row_sprites(y) + per_row.get(y, 0) >= 20:
                    continue
            per_row[y] = per_row.get(y, 0) + 1
            for px in range(s["w"] * 8):
                x = s["x"] + px
                if not lo <= x < hi or (x, y) in claimed:
                    continue
                if s["clip"] is not None and not (s["clip"][0] <= x < s["clip"][0] + s["clip"][2]):
                    continue
                sx, sy = (s["w"] * 8 - 1 - px) if s.get("hflip") else px, (s["h"] * 8 - 1 - py) if s.get("vflip") else py
                cell = (sx // 8) * s["h"] + sy // 8
                idx = tiles[s["tile"] + cell][sy % 8][sx % 8] if (s["tile"] + cell) in tiles else 0
                if idx == 0:
                    continue
                if 0 <= x < W:
                    cls = native_class(x, y)
                    if (s["layer"] == BEHIND and cls != "backdrop") or (s["layer"] == BETWEEN and cls == "high"):
                        continue
                out[y][x + side] = rgb(palette[idx])
                claimed.add((x, y))
                counts[s["src"]]["left" if x < 0 else "right" if x >= W else "picture"] += 1
    return out, counts, None


# ---- the Luau text of a submission (the same sprites the ABI 1 / ABI 2 routes submit) ----
def hexpattern(p):
    return "".join(str(p[y][x]) for y in range(8) for x in range(8))


def lua_for(sprites, budget="limit", pool=True, extra_head="", loop_count=None, scene=None, parity=False, space=None, pool_tiles=None):
    lines = ["function render(frame: number)"]
    if parity:
        lines.append("  if frame % 2 ~= 0 then return end")
    if scene:
        lines.append(f'  sega.scene("{scene}")')
    lines.append(extra_head)
    if space:
        lines.append(f'  sega.sprite.space("{space}")')
    if budget == "extended":
        lines.append('  sega.sprite.budget("extended")')
    if pool:
        for index, pat in sorted((pool_tiles or POOL_TILES).items()):
            lines.append(f'  sega.sprite.tile({index}, "{hexpattern(pat)}")')
        lines.append('  sega.sprite.palette(0, "' + "".join("%04X" % w for w in AUTHORED_PALETTE) + '")')
    names = {BEHIND: "behind_planes", BETWEEN: "between", ABOVE: "above_all"}
    for c in sprites:
        flags = (1 if c["hflip"] else 0) | (2 if c["vflip"] else 0)
        lines.append(f'  sega.sprite.add("{c["src"]}", "{names[c["layer"]]}", {c["x"]}, {c["y"]}, {c["w"]}, {c["h"]}, {c["tile"]}, {c["pal"]}, {flags})')
        if c["clip"] is not None:
            lines.append("  sega.sprite.clip(%d, %d, %d, %d)" % c["clip"])
    if loop_count:
        lines.append(f'  for i = 1, {loop_count} do sega.sprite.add("authored", "above_all", 100, 150, 1, 1, 1, 0, 0) end')
    lines.append("end")
    return "\n".join(lines) + "\n"
