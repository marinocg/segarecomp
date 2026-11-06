"""SEG-122-T011 (ADR 0271/0272): project-authored Genesis ROM for the H-interrupt differential through a work-RAM `JMP (xxx).L` stub.

Hand assembled; nothing commercial. The level-4 vector points at the work-RAM stub `$FFFFF6`. The mainline builds the stub from constant
stores, enables the H-interrupt, waits (with a bounded timeout) for four interrupts delivered to handler A, retargets the stub operand to
handler B, waits for four more, retargets back to A and waits for four more, then masks all interrupts, records SR and signals completion.
Results live in the first 256 bytes of work RAM (`$FF0000`); the completion byte `$A5` is the last byte (the existing libretro host stops on it):

  $00 W handler A count      $02 W handler B count      $0C L order log (2 bits per interrupt: 1 = A, 2 = B; newest in the low bits)
  $08 L mainline spin count (timing dependent, never compared)
  $40 W SR stacked by the first interrupt   $42 W SR inside the handler   $44 W SR read by the mainline after the final mask
  $52/$54/$56 W phase-timeout flags (1 = that wait gave up)               $FF B completion marker $A5
Variants are adversarial controls: `disabled` (no H-interrupt enable), `masked` (interrupt mask 4), `bad_opcode` (the stub holds a NOP word instead of JMP),
`bad_target` (the first operand names an unmapped address).
"""
import struct

STUB, HANDLER_A, HANDLER_B, RTE_ROM, HALT = 0xFFF6, 0x1000, 0x3000, 0x0F00, 0x0400
TIMEOUT = 0x4000
RESULT = 0x00FF0000


def _abs(offset):
    return [0x00FF, offset]


class Asm:
    def __init__(self):
        self.items, self.labels = [], {}

    def w(self, *words):
        self.items += [("w", x) for x in words]

    def label(self, name):
        self.items.append(("l", name))

    def branch(self, opcode, name):
        self.items.append(("b", (opcode, name)))

    def assemble(self, base):
        at, pos = base, {}
        for kind, value in self.items:
            if kind == "l":
                pos[value] = at
            else:
                at += 2
        out, at = [], base
        for kind, value in self.items:
            if kind == "w":
                out.append(value); at += 2
            elif kind == "b":
                opcode, name = value
                disp = pos[name] - (at + 2)
                assert -128 <= disp <= 126 and disp != 0 and disp % 2 == 0, (name, disp)
                out.append((opcode << 8) | (disp & 0xFF)); at += 2
        return struct.pack(">%dH" % len(out), *out)


def wait_for(a, counter_offset, threshold, flag_offset, name):
    a.w(0x243C, 0x0000, TIMEOUT)                          # MOVE.L #timeout,D2
    a.label(name + "_loop")
    a.w(0x0C79, threshold, *_abs(counter_offset))        # CMPI.W #threshold,(counter).L
    a.branch(0x64, name + "_done")                        # BCC.S done (counter >= threshold)
    a.w(0x52B9, *_abs(0x08))                              # ADDQ.L #1,(spin).L   -- the mainline keeps running between interrupts
    a.w(0x5382)                                           # SUBQ.L #1,D2
    a.branch(0x66, name + "_loop")                        # BNE.S loop
    a.w(0x33FC, 0x0001, *_abs(flag_offset))               # MOVE.W #1,(flag).L   -- the wait gave up
    a.label(name + "_done")


def handler(identifier, counter_offset):
    a = Asm()
    a.w(0x3E17)                                           # MOVE.W (A7),D7       stacked SR
    a.w(0x33C7, *_abs(0x40))                              # MOVE.W D7,($FF0040).L
    a.w(0x40C6)                                           # MOVE.W SR,D6
    a.w(0x33C6, *_abs(0x42))                              # MOVE.W D6,($FF0042).L
    a.w(0x5279, *_abs(counter_offset))                    # ADDQ.W #1,(counter).L
    a.w(0x2E39, *_abs(0x0C))                              # MOVE.L ($FF000C).L,D7
    a.w(0xE58F)                                           # LSL.L #2,D7
    a.w(0x5287 if identifier == 1 else 0x5487)            # ADDQ.L #1|#2,D7
    a.w(0x23C7, *_abs(0x0C))                              # MOVE.L D7,($FF000C).L
    a.w(0x4E73)                                           # RTE
    return a.assemble(HANDLER_A if identifier == 1 else HANDLER_B)


def build(variant="normal"):
    assert variant in ("normal", "disabled", "masked", "bad_opcode", "bad_target")
    rom = bytearray(0x4000)
    rom[0:4] = struct.pack(">I", 0x00FFFE00)
    rom[4:8] = struct.pack(">I", 0x200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", RTE_ROM)
    rom[0x70:0x74] = struct.pack(">I", 0x00FF0000 | STUB)    # level-4 autovector -> the work-RAM stub
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[RTE_ROM:RTE_ROM + 2] = struct.pack(">H", 0x4E73)
    rom[HANDLER_A:HANDLER_A + len(handler(1, 0x00))] = handler(1, 0x00)
    rom[HANDLER_B:HANDLER_B + len(handler(2, 0x02))] = handler(2, 0x02)
    rom[HALT:HALT + 2] = struct.pack(">H", 0x60FE)           # BRA.S * (completion spin)
    a = Asm()
    a.w(0x207C, 0x00FF, 0x0000, 0x703F)                       # MOVEA.L #$FF0000,A0 ; MOVEQ #63,D0
    a.label("clear")
    a.w(0x4298)                                               # CLR.L (A0)+
    a.w(0x51C8, 0xFFFC)                                       # DBRA D0,clear
    opcode = 0x4E71 if variant == "bad_opcode" else 0x4EF9
    first = 0x00500000 if variant == "bad_target" else HANDLER_A
    a.w(0x31FC, opcode, STUB)                                 # MOVE.W #opcode,($FFF6).W
    a.w(0x21FC, first >> 16, first & 0xFFFF, STUB + 2)        # MOVE.L #target,($FFF8).W
    a.w(0x33FC, 0x8004 if variant == "disabled" else 0x8014, 0x00C0, 0x0004)   # VDP reg 0: H-interrupt enable
    a.w(0x33FC, 0x8144, 0x00C0, 0x0004)                       # reg 1: display on, mode 5 (V-interrupt stays disabled)
    a.w(0x33FC, 0x8A0F, 0x00C0, 0x0004)                       # reg 10: H counter 15 (every 16 lines)
    a.w(0x46FC, 0x2400 if variant == "masked" else 0x2000)    # MOVE #sr,SR
    wait_for(a, 0x00, 4, 0x52, "a1")
    a.w(0x21FC, HANDLER_B >> 16, HANDLER_B & 0xFFFF, STUB + 2)  # retarget to handler B
    wait_for(a, 0x02, 4, 0x54, "b")
    a.w(0x21FC, HANDLER_A >> 16, HANDLER_A & 0xFFFF, STUB + 2)  # back to handler A
    wait_for(a, 0x00, 8, 0x56, "a2")
    a.w(0x46FC, 0x2700)                                       # mask everything
    a.w(0x40C0)                                               # MOVE.W SR,D0
    a.w(0x33C0, *_abs(0x44))                                  # MOVE.W D0,($FF0044).L
    a.w(0x223C, 0x0002, 0x0000)                               # MOVE.L #$20000,D1
    a.label("settle")
    a.w(0x5381)                                               # SUBQ.L #1,D1
    a.branch(0x66, "settle")                                  # BNE.S settle: no interrupt may arrive while masked
    a.w(0x13FC, 0x00A5, *_abs(0xFF))                          # MOVE.B #$A5,($FF00FF).L
    a.w(0x207C, 0x0000, HALT, 0x4ED0)                         # MOVEA.L #HALT,A0 ; JMP (A0)
    code = a.assemble(0x200)
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)
