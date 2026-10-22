| SEG-077-T001 synthetic Genesis cartridge (authored; no commercial content).
| Ground truth comes from this file's GNU as listing. It exercises: executed code, an untaken direct branch target (static reach),
| a jump table with an unused entry, inline data after a JSR (must never become code), a VDP DMA from ROM (data evidence), a CPU-read
| pointer table, a checksum loop over code, an IRQ-enabled VBlank handler, execution that jumps into the middle of an executed
| instruction (overlapping spans), a never-referenced dead routine, 0xFF padding, and - only while pad button C is held - a routine
| copied to work RAM, executed there and then cleared (any RAM execution must disable data-read evidence).
        .text
        .org    0
vectors:
        .long   0x00FFFE00              | initial SSP
        .long   reset                   | reset PC
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   vblank                  | vector 30: level-6 (VBlank) autovector
        .long   exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .long   exc_handler, exc_handler
        .org    0x100
header:
        .ascii  "SEGA GENESIS    "
        .ascii  "(C)SEGARECOMP   "
        .ascii  "SYNTHETIC EVIDENCE CARTRIDGE                    "
        .ascii  "SYNTHETIC EVIDENCE CARTRIDGE                    "
        .ascii  "GM 00000000-00"
        .word   0                       | checksum
        .ascii  "J               "
        .long   0, 0x1FFFF, 0xFF0000, 0xFFFFFF
        .ascii  "            "
        .ascii  "            "
        .ascii  "                                        "
        .ascii  "JUE             "
        .org    0x200
reset:
        move.w  #0x2700,%sr
        lea     0xC00004,%a4
        lea     0xC00000,%a5
        move.w  #0x8004,(%a4)           | reg0
        move.w  #0x8174,(%a4)           | reg1: display, vblank, DMA enable
        move.w  #0x8F02,(%a4)           | autoincrement 2
        bsr     dma_tiles
        | checksum loop: reads executed AND never-executed code as data (its reads must never become data labels)
        lea     reset,%a0
        move.w  #(ptr_table-reset)/2-1,%d7
        moveq   #0,%d2
2:      add.w   (%a0)+,%d2
        dbra    %d7,2b
        | 3-button pad on port 1: TH as output, TH = 1 (then bit 5 of the data port is button C, active low)
        move.b  #0x40,0xA10009
        move.b  #0x40,0xA10003
        | jump into the middle of an executed instruction: ovl's long immediate is also executed as NOP; RTS
        bsr     ovl
        lea     ovl+2,%a0
        jsr     (%a0)
        | the same with an outer instruction both decoders decode (entered by a direct BSR: never re-admitted as static reach)
        bsr     ovl2
        lea     ovl2+2,%a0
        jsr     (%a0)
        | inline-argument call: two data words follow the JSR
        jsr     inline_args
        .word   0x4E75, 0x4EF9
        | read a pointer table entry
        lea     ptr_table,%a0
        move.l  4(%a0),%a1
        | jump-table dispatch for indices 0..2 (entry 3 is never used)
        moveq   #0,%d6
dispatch:
        move.w  %d6,%d0
        add.w   %d0,%d0
        move.w  jtab(%pc,%d0.w),%d0
        jmp     jtab(%pc,%d0.w)
jtab:
        .word   case0-jtab, case1-jtab, case2-jtab, case3-jtab
case0:
        addq.w  #1,%d6
        bra   dispatch
case1:
        addq.w  #1,%d6
        tst.w   %d6
        beq   never_taken             | d6 != 0 here: target only statically reached
        bra   dispatch
case2:
        bra   main_loop
case3:
        move.w  #0x1234,%d5             | unused jump-table entry: never executed
        bra   case3
never_taken:
        move.w  #0x5678,%d4
        bra   main_loop
main_loop:
        move.w  #0x2000,%sr             | interrupts enabled (VBlank)
        addq.l  #1,%d3
        btst    #5,0xA10003
        bne     main_loop               | C not held
        bsr     call_ram
        bra     main_loop
call_ram:
        lea     ram_routine,%a0
        lea     0xFF0000,%a1
        move.w  #(ram_routine_end-ram_routine)/2-1,%d7
1:      move.w  (%a0)+,(%a1)+
        dbra    %d7,1b
        jsr     0xFF0000
        clr.l   0xFF0000                | the RAM routine is cleared after use
        clr.w   0xFF0004
        rts
ovl:
        move.l  #0x4E714E75,%d0         | its immediate is NOP; RTS when entered at ovl+2
        rts
ovl2:
        move.w  #0x4E71,%d0             | its immediate is NOP when entered at ovl2+2
        rts
vblank:
        movem.l %d0-%d1/%a4,-(%sp)      | every VBlank re-programs and repeats the tile DMA (exercised after state loads)
        lea     0xC00004,%a4
        bsr     dma_tiles
        movem.l (%sp)+,%d0-%d1/%a4
        addq.l  #1,0xFF0100
        rte
dma_tiles:
        | DMA 68K -> VRAM 0x0000: length (words), source (address >> 1)
        move.w  #0x9320,(%a4)           | reg 0x13 = 32 words (64 bytes)
        move.w  #0x9400,(%a4)
        move.l  #tiles,%d0
        lsr.l   #1,%d0
        move.w  %d0,%d1
        and.w   #0xFF,%d1
        or.w    #0x9500,%d1
        move.w  %d1,(%a4)
        lsr.l   #8,%d0
        move.w  %d0,%d1
        and.w   #0xFF,%d1
        or.w    #0x9600,%d1
        move.w  %d1,(%a4)
        lsr.l   #8,%d0
        and.w   #0x7F,%d0
        or.w    #0x9700,%d0
        move.w  %d0,(%a4)
        move.l  #0x40000080,(%a4)       | VRAM write + CD5: start DMA
        rts
inline_args:
        move.l  (%sp)+,%a0
        move.w  (%a0)+,%d2
        move.w  (%a0)+,%d2
        jmp     (%a0)
exc_handler:
        rte
dead_routine:
        move.w  #0x2222,%d0
        rts
        .balign 2
ram_routine:
        move.w  #0x1111,%d0
        rts
ram_routine_end:
        .balign 4
ptr_table:
        .long   case0, case1, tiles, header
        .balign 2
tiles:
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .long   0x11223344, 0x11223344
        .balign 2
        .org    0x20000, 0xFF
