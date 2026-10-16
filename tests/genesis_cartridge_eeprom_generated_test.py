#!/usr/bin/env python3
"""Generated-native coverage for the header-declared serial EEPROM (ADR 0136, SEG-068-T006).

Synthetic, project-authored images only (no commercial data).  Programs go through the production
`emit-general-startup-bridge-c --reset-entry --immutable-rom-aot` route, are compiled as strict C11 and run with a finite
instruction budget.  The guest bit-bangs the X24C01 protocol on the single odd byte location the header declares (SDA on D0,
SCL on D1) and stores every observed SDA sample into work RAM; the host compares the samples with the protocol's expectations.

Covered: write then read-back; page-write wrap (4 bytes); sequential read with master ACK across the array end; power-on
contents (0xFF); a master NACK ending a read; STOP / repeated START; determinism; and every fail-closed form (WORD, opposite
lane, an access outside the location, a ROM that reaches the location, $A130F1 not owned by an EEPROM cartridge).
"""
import pathlib
import sys
import tempfile

import genesis_cartridge_sram_generated_test as sram  # shared synthetic-ROM, emit, run and encoding helpers

LOCATION = 0x200001
HEADER = (0xE8, 0x40, LOCATION, LOCATION)
SAMPLE_BASE = 0x00FF0000


class Guest:
  """Accumulates hand-assembled code: line writes to the EEPROM location and SDA samples stored to work RAM."""

  def __init__(self):
    self.code = []
    self.samples = 0

  def line(self, sda, scl):
    self.code.append(sram.move_b_imm_abs((scl << 1) | sda, LOCATION))

  def sample(self):  # MOVE.B location.L,(SAMPLE_BASE + n).L
    self.code.append(f"13F9{LOCATION:08X}{SAMPLE_BASE + self.samples:08X}")
    self.samples += 1
    return self.samples - 1

  def start(self):
    self.line(1, 1)
    self.line(0, 1)
    self.line(0, 0)

  def stop(self):
    self.line(0, 0)
    self.line(0, 1)
    self.line(1, 1)

  def send_bit(self, bit):
    self.line(bit, 0)
    self.line(bit, 1)
    self.line(bit, 0)

  def send_byte(self, value):  # returns the sample index of the acknowledge
    for position in range(7, -1, -1):
      self.send_bit((value >> position) & 1)
    self.line(1, 0)  # release SDA
    self.line(1, 1)
    ack = self.sample()
    self.line(1, 0)
    return ack

  def receive_byte(self, ack):  # returns the 8 sample indices, MSB first
    indices = []
    for _ in range(8):
      self.line(1, 0)
      self.line(1, 1)
      indices.append(self.sample())
      self.line(1, 0)
    self.send_bit(0 if ack else 1)  # master ACK = SDA low, NACK = SDA high
    return indices

  def hex(self):
    return "".join(self.code)


def command(address, read):
  return (address << 1) | (1 if read else 0)


def byte_of(samples, indices):
  value = 0
  for index in indices:
    value = (value << 1) | (samples[index] & 1)
  return value


def run(cli, compiler, root, work, name, guest, header=HEADER, size=0x400):
  rom = sram.build_rom(guest.hex(), size=size, header=header)
  _, sanitized, full = sram.exercise(cli, compiler, root, work, name, rom, budget=20000)
  samples = None
  if full is not None:
    ram = sram.ram(full)
    samples = [ram[i] for i in range(guest.samples)]
  return sanitized, samples


def main() -> None:
  cli, compiler, root = sys.argv[1:]
  with tempfile.TemporaryDirectory() as temp:
    work = pathlib.Path(temp)

    # 1. Power-on contents, write then read-back, page wrap, sequential read, NACK, STOP.
    g = Guest()
    acks = []
    reads = {}
    # a) power-on contents: random read of cell 0x7F (set the address with a write command, repeat START as a read)
    g.start(); acks.append(g.send_byte(command(0x7F, False))); g.start()
    acks.append(g.send_byte(command(0x7F, True))); reads["poweron"] = g.receive_byte(False); g.stop()
    # b) write 0xA5 at cell 0x10, then read it back
    g.start(); acks.append(g.send_byte(command(0x10, False))); acks.append(g.send_byte(0xA5)); g.stop()
    g.start(); acks.append(g.send_byte(command(0x10, False))); g.start()
    acks.append(g.send_byte(command(0x10, True))); reads["readback"] = g.receive_byte(False); g.stop()
    # c) page write of four bytes starting at cell 0x02 wraps inside its 4-byte page: cells 0x02,0x03,0x00,0x01
    g.start(); acks.append(g.send_byte(command(0x02, False)))
    for value in (0x11, 0x22, 0x33, 0x44):
      acks.append(g.send_byte(value))
    g.stop()
    # d) sequential read from cell 0x00: expect 0x33 0x44 0x11 0x22 (the wrapped page), ACKing all but the last
    g.start(); acks.append(g.send_byte(command(0x00, False))); g.start()
    acks.append(g.send_byte(command(0x00, True)))
    seq = [g.receive_byte(index != 3) for index in range(4)]
    g.stop()
    # e) sequential read across the end of the array: cells 0x7F then 0x00 (0xFF then the page-wrapped 0x33)
    g.start(); acks.append(g.send_byte(command(0x7F, False))); g.start()
    acks.append(g.send_byte(command(0x7F, True)))
    wrap = [g.receive_byte(index != 1) for index in range(2)]
    g.stop()
    # f) a master NACK ends the read: clocking on until STOP does not change the array (read cell 0x10 again afterwards)
    g.start(); acks.append(g.send_byte(command(0x10, False))); g.start()
    acks.append(g.send_byte(command(0x10, True))); nack_first = g.receive_byte(False)
    g.line(1, 0); g.line(1, 1); g.line(1, 0); g.stop()
    g.start(); acks.append(g.send_byte(command(0x10, False))); g.start()
    acks.append(g.send_byte(command(0x10, True))); after_nack = g.receive_byte(False); g.stop()

    sanitized, samples = run(cli, compiler, root, work, "protocol", g)
    assert sanitized["result"] == "runner_resource_limit", sanitized  # the program reached its terminal spin without a stop
    assert all(samples[a] == 0 for a in acks), ("every command/data byte is acknowledged", [samples[a] for a in acks])
    assert byte_of(samples, reads["poweron"]) == 0xFF, "power-on contents are 0xFF"
    assert byte_of(samples, reads["readback"]) == 0xA5, "a written byte reads back"
    assert [byte_of(samples, indices) for indices in seq] == [0x33, 0x44, 0x11, 0x22], "page write wraps inside its 4-byte page"
    assert [byte_of(samples, indices) for indices in wrap] == [0xFF, 0x33], "sequential read wraps from cell 0x7F to cell 0x00"
    assert byte_of(samples, nack_first) == 0xA5 and byte_of(samples, after_nack) == 0xA5, "a NACKed read leaves the array unchanged"

    # 2. Fail-closed forms. Each is a typed device stop; nothing is decomposed into bytes.
    bodies = {
        "word_read": sram.movea_l_imm(0, 0x200000) + sram.move_w_ind_dn(0, 1),   # covers the location
        "long_read": sram.movea_l_imm(0, 0x200000) + "2010",                       # MOVE.L (A0),D0
        "word_write": sram.movea_l_imm(0, 0x200000) + "30BC0001",                  # MOVE.W #1,(A0)
    }
    for name, body in bodies.items():
      rom = sram.build_rom(body + sram.LOOP, header=HEADER)
      _, sanitized, _ = sram.exercise(cli, compiler, root, work, "rej_" + name, rom)
      assert sram.stop(sanitized) == ("unsupported_device_access", "unsupported_cartridge_sram_access"), (name, sanitized)

    # 2b. A WORD at the (odd) location address is rejected by the earlier, unchanged odd-effective-address stage.
    rom = sram.build_rom(sram.movea_l_imm(0, LOCATION) + "3010" + sram.LOOP, header=HEADER)
    _, sanitized, _ = sram.exercise(cli, compiler, root, work, "rej_odd_word", rom)
    assert sram.stop(sanitized) == ("unsupported_memory_region", "odd_effective_address"), sanitized

    # 3. A byte next to the location is not the EEPROM: the ordinary (unchanged) unmapped / ROM-write failure.
    rom = sram.build_rom(sram.movea_l_imm(0, 0x200003) + sram.move_b_ind_dn(0, 1) + sram.LOOP, header=HEADER)
    _, sanitized, _ = sram.exercise(cli, compiler, root, work, "neighbour_read", rom)
    assert sram.stop(sanitized)[1] != "unsupported_cartridge_sram_access", sanitized
    assert sanitized["result"] == "stop", sanitized

    # 4. A ROM that extends to the location: the ROM/device overlay is not established -> typed layout stop on any access.
    big = sram.build_rom(sram.move_b_abs_dn(LOCATION, 1) + sram.LOOP, size=0x200400, header=HEADER)
    _, sanitized, _ = sram.exercise(cli, compiler, root, work, "rom_reaches_location", big)
    assert sram.stop(sanitized) == ("unsupported_device_access", "unsupported_cartridge_sram_layout"), sanitized

    # 5. $A130F1 belongs to the SRAM mapper only: an EEPROM cartridge keeps the idle policy (bit 0 set is the unmapped stop).
    for value, expected in ((0x00, "runner_resource_limit"), (0x01, "stop")):
      rom = sram.build_rom(sram.move_b_imm_abs(value, 0xA130F1) + sram.LOOP, header=HEADER)
      _, sanitized, _ = sram.exercise(cli, compiler, root, work, f"a130f1_{value}", rom)
      assert sanitized["result"] == expected, (value, sanitized)
      if expected == "stop":
        assert sram.stop(sanitized) == ("unsupported_memory_region", "unmapped_data_access"), sanitized

  print("genesis_cartridge_eeprom_generated_test: OK")


if __name__ == "__main__":
  main()
