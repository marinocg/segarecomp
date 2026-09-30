"""SEG-009-T005: independent executable specification of the SMS Mode 4 renderer (machine contract section 9.8).

Written from the contract and the public MacDonald/SMS Power! rules it cites, never from the C renderer
(platforms/master-system/runtime/sms_render.c). The formulation is deliberately different (per-pixel evaluation, the
"coarse/fine" scroll decomposition, per-sprite pixel lists) so a shared misreading is unlikely to hide in both.

`mutation` names a deliberately wrong rule; the differential test requires every mutant to disagree with the C renderer:
  priority          the priority bit is ignored (sprites always cover the background)
  no_hscroll_lock   R0 bit 6 has no effect
  no_vscroll_lock   R0 bit 7 has no effect
  sprite_limit_7    the per-line limit is 7
  sprite_limit_9    the per-line limit is 9
  no_left_blank     R0 bit 5 has no effect
  terminator_ignored  Y = $D0 does not end the sprite list
  no_fine_gap       the fine-scroll gap shows wrapped background instead of the backdrop
  vscroll_live      R9 is taken per line instead of being latched at line 0
"""
MUTATIONS = ("priority", "no_hscroll_lock", "no_vscroll_lock", "sprite_limit_7", "sprite_limit_9", "no_left_blank",
             "terminator_ignored", "no_fine_gap", "vscroll_live")


def height_of(regs):
    """Active lines (192/224) or None when the mode is not one of the two supported Mode 4 forms."""
    m4, m2, m1, m3 = regs[0] >> 2 & 1, regs[0] >> 1 & 1, regs[1] >> 4 & 1, regs[1] >> 3 & 1
    if not m4 or regs[0] & 1:
        return None
    if (m1, m2) == (0, 0) or (m3, m2, m1) == (0, 1, 0) or (m3, m2, m1) == (1, 1, 1):
        return 192
    if (m3, m2, m1) == (0, 1, 1):
        return 224
    return None


def tile_row(vram, pattern, row):
    base = (pattern * 32 + row * 4) % 0x4000
    return [vram[(base + p) % 0x4000] for p in range(4)]


def tile_color(planes, col):
    return sum(((planes[p] >> (7 - col)) & 1) << p for p in range(4))


def sprite_entries(vram, regs, height, line, limit, mutation):
    """Sprites drawn on `line` in SAT order: returns (list of (x, pattern_row_planes...), overflow)."""
    sat = (regs[5] & 0x7E) << 7
    tall = regs[1] & 2
    zoom = 2 if regs[1] & 1 else 1
    size = (16 if tall else 8) * zoom
    chosen = []
    overflow = False
    for n in range(64):
        y = vram[(sat + n) % 0x4000]
        if height == 192 and y == 0xD0 and mutation != "terminator_ignored":
            break
        top = y + 1
        offset = (line - top) % 256
        if offset < size:
            if len(chosen) == limit:
                overflow = True
                break
            chosen.append((n, offset // zoom))
    return chosen, overflow


def render(vram, cram, regs_per_line, mutation=None):
    """`regs_per_line(line)` returns the register list (>= 11 entries) as the line sees it. Returns
    (height, rows of 256 bytes, overflow, collision) or (0, [], False, False) for an unsupported mode."""
    assert mutation is None or mutation in MUTATIONS
    first = regs_per_line(0)
    height = height_of(first)
    if height is None:
        return 0, [], False, False
    vscroll0 = first[9]
    limit = {"sprite_limit_7": 7, "sprite_limit_9": 9}.get(mutation, 8)
    overflow = collision = False
    rows = []
    for line in range(height):
        regs = regs_per_line(line)
        backdrop = cram[16 + (regs[7] & 15)] & 0x3F
        if not regs[1] & 0x40:
            rows.append([backdrop] * 256)
            continue
        hs = 0 if (regs[0] & 0x40 and line < 16 and mutation != "no_hscroll_lock") else regs[8]
        coarse, fine = hs >> 3, hs & 7
        vs_line = regs[9] if mutation == "vscroll_live" else vscroll0
        if height == 224:
            nt_base = (regs[2] & 0x0C) * 0x400 + 0x700
        else:
            nt_base = (regs[2] & 0x0E) * 0x400
        bg = []  # (colour index 0-15 or None for the backdrop gap, priority, palette)
        for x in range(256):
            if x < fine and mutation != "no_fine_gap":
                bg.append((None, 0, 0))
                continue
            vs = 0 if (regs[0] & 0x80 and x >> 3 >= 24 and mutation != "no_vscroll_lock") else vs_line
            xs = x - fine
            col = ((xs >> 3) - coarse) % 32 if x >= fine else ((x - hs) % 256) >> 3
            px = xs & 7 if x >= fine else (x - hs) % 8
            y = (line + vs) % (256 if height == 224 else 224)
            base = (nt_base + (y >> 3) * 64 + col * 2) % 0x4000
            entry = vram[base] | vram[(base + 1) % 0x4000] << 8
            r = y & 7
            if entry & 0x400:
                r = 7 - r
            if entry & 0x200:
                px = 7 - px
            color = tile_color(tile_row(vram, entry & 0x1FF, r), px)
            bg.append((color, entry >> 12 & 1, entry >> 11 & 1))
        sprites = []
        if True:
            chosen, ov = sprite_entries(vram, regs, height, line, limit, mutation)
            sat = (regs[5] & 0x7E) << 7
            zoom = 2 if regs[1] & 1 else 1
            per_pixel = {}
            for n, row in chosen:
                x0 = vram[(sat + 0x80 + 2 * n) % 0x4000] - (8 if regs[0] & 8 else 0)
                tile = vram[(sat + 0x81 + 2 * n) % 0x4000]
                if regs[1] & 2:
                    tile &= 0xFE
                tile += 256 if regs[6] & 4 else 0
                planes = tile_row(vram, tile + row // 8, row % 8)
                for dx in range(8 * zoom):
                    x = x0 + dx
                    c = tile_color(planes, dx // zoom)
                    if c and 0 <= x < 256:
                        per_pixel.setdefault(x, []).append(c)
            overflow |= ov
            for x, cs in per_pixel.items():
                if len(cs) > 1:
                    collision = True
            sprites = per_pixel
        row_out = []
        for x in range(256):
            color, prio, pal = bg[x]
            value = backdrop if color is None else cram[pal * 16 + color] & 0x3F
            if x in sprites:
                covered = prio and color and mutation != "priority"
                if not covered:
                    value = cram[16 + sprites[x][0]] & 0x3F
            if x < 8 and regs[0] & 0x20 and mutation != "no_left_blank":
                value = backdrop
            row_out.append(value)
        rows.append(row_out)
    return height, rows, overflow, collision
