; SEG-145-T004: the injected driver routine. It takes the step's attenuation in A and returns the one the driver writes: every other 3 steps are two units quieter, clamped to 15.
; Port I/O is not admitted in injected code: the cartridge's own driver stays the only writer of the PSG.
free * 0x2000 0x40 0xff
org * 0x2000
  and 0x0f
  push bc
  ld c,a
  ld a,(0xc000)
  and 0x10
  jr z,keep
  ld a,c
  add a,2
  cp 16
  jr c,done
  ld a,15
done:
  pop bc
  ret
keep:
  ld a,c
  pop bc
  ret
