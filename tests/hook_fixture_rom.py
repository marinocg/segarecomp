"""SEG-138-T012 fixture: a fabricated Genesis image (no vendor code) with ONE hookable subroutine. It is the gameplay fixture's rule (score += 1 per turn while B is held, position += 1 while Right is held)
plus, every turn, `jsr bump`: bump adds 1 to a work-RAM word (0xFF0020) and then spends ~1000 loop iterations in d3, so the call is in flight for a good part of every frame."""
import hashlib
import struct

BUMP_ADDRESS = 0x400
BUMP_OFFSET = 0x20
LOOP = 999
TURNS = 90
OTHER_ADDRESS = 0x440   # an `rts` no code calls
RTE_ADDRESS = 0x460     # an `rte` (called only with rte_call=True)
HANDLER = 0x500   # every exception/interrupt vector points at this `bra.s *`


def make_rom(loop=LOOP, vector_target=HANDLER, rte_call=False, computed=False):
    out = bytearray()
    labels, fixups = {}, []

    def w(*words):
        for word in words:
            out.extend(struct.pack(">H", word))

    def l(value):
        out.extend(struct.pack(">I", value))

    def label(name):
        w(0x6000, 0x0002)
        labels[name] = len(out)

    def branch_s(opcode, name):
        fixups.append((len(out), name))
        w(opcode << 8)

    w(0x41F9); l(0x00C00004)
    w(0x43F9); l(0x00C00000)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001):
        w(0x30BC, reg)
    w(0x20BC); l(0xC0000000)
    for colour in (0x0420, 0x00E0, 0x000E):
        w(0x32BC, colour)
    w(0x20BC); l(0x40200000)
    for _ in range(8):
        w(0x22BC); l(0x11111111)
    w(0x20BC); l(0x40400000)
    for row in range(8):
        w(0x22BC); l(0x12121212 if row % 2 == 0 else 0x21212121)
    w(0x20BC); l(0x40000003)
    for tile in (1, 2):
        w(0x303C, 63)
        label(f"fill{tile}")
        w(0x32BC, tile)
        w(0x51C8, 0xFFFA)
    w(0x45F9); l(0x00FF0000)
    w(0x13FC, 0x0040); l(0x00A10009)
    w(0x13FC, 0x0040); l(0x00A10003)
    w(0x426A, 0x10)
    w(0x426A, 0x12)
    w(0x426A, BUMP_OFFSET)
    w(0x3E3C, TURNS - 1)
    label("turn")
    label("wait_blank")
    w(0x3010)
    w(0x0800, 3)
    branch_s(0x67, "wait_blank")
    w(0x1039); l(0x00A10003)
    w(0x4600)
    w(0x0800, 4)
    branch_s(0x67, "no_b")
    w(0x526A, 0x10)
    label("no_b")
    w(0x0800, 3)
    branch_s(0x67, "no_right")
    w(0x526A, 0x12)
    label("no_right")
    w(0x4EB9); l(BUMP_ADDRESS)                       # jsr bump
    if rte_call:
        w(0x4EB9); l(RTE_ADDRESS)                    # jsr to a function that returns with RTE (a return form the build cannot classify)
    label("wait_active")
    w(0x3010)
    w(0x0800, 3)
    branch_s(0x66, "wait_active")
    w(0x51CF)
    at = len(out)
    w(0x0000)
    out[at:at + 2] = struct.pack(">h", labels["turn"] - at)
    if computed:
        w(0x4E93)                                    # jsr (a3): a computed call, so an uncalled site is `computed target`, not `not a function entry`
    w(0x4E70)
    for at, name in fixups:
        displacement = labels[name] - (at + 2)
        assert -128 <= displacement <= 126 and displacement != 0, (name, displacement)
        out[at + 1] = displacement & 0xFF
    code = out
    rom = bytearray(struct.pack(">H", 0x60FE) * 0x400)
    rom[0:8] = struct.pack(">II", 0x00FFFE00, 0x00000200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", vector_target)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP HOOK FIXTURE".ljust(48) + b"SEGARECOMP HOOK FIXTURE".ljust(48) + b"GM 00000000-00" + b"\0\0" +
              b"J".ljust(16) + struct.pack(">IIII", 0, 0x7FF, 0xFF0000, 0xFFFFFF) + b" " * 12 + b" " * 12 + b" " * 40 + b"JUE".ljust(16))
    assert len(header) == 0x100
    rom[0x100:0x200] = header
    assert 0x200 + len(code) < BUMP_ADDRESS, len(code)
    rom[0x200:0x200 + len(code)] = code
    # bump: addq.w #1,0x20(a2); move.w #loop,d3; l: dbra d3,l; rts
    body = bytearray()
    body += struct.pack(">HH", 0x526A, BUMP_OFFSET)
    body += struct.pack(">HH", 0x363C, loop)
    body += struct.pack(">HH", 0x51CB, 0xFFFE)      # dbra d3,*  (displacement -2: back to itself)
    body += struct.pack(">H", 0x4E75)
    rom[BUMP_ADDRESS:BUMP_ADDRESS + len(body)] = body
    rom[OTHER_ADDRESS:OTHER_ADDRESS + 2] = struct.pack(">H", 0x4E75)      # a function nobody calls: rts
    rom[RTE_ADDRESS:RTE_ADDRESS + 2] = struct.pack(">H", 0x4E73)          # rte
    return bytes(rom)


if __name__ == "__main__":
    import sys
    image = make_rom()
    open(sys.argv[1], "wb").write(image)
    print(hashlib.sha256(image).hexdigest())
