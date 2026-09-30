"""SEG-009-T004: independent executable specification of the SMS VDP port/state protocol.

Written from the machine contract (docs/architecture/master-system-machine-contract.md section 9) and the public
MacDonald/SMS Power! documents it cites, never from the C runtime (platforms/master-system/runtime/sms_vdp.c) and never
from a reference emulator's source. It is the third implementation in the port/state differential: the generated-native
runtime and the two pinned references (tests/sms_vdp_oracle_test.py) are compared against the results it predicts.

`mutation` names a deliberately wrong rule. A differential that cannot tell a mutant from the truth is not a
differential, so the tests require every mutant to disagree with the generated-native results:
  latch_not_reset_by_status   a status read does not clear the first/second-byte latch
  no_read_prefetch            the code-0 command does not pre-fetch the read buffer
  write_does_not_load_buffer  a data write does not load the read buffer
  no_address_wrap             the address counter saturates at $3FFF instead of wrapping
  cram_no_alias               CRAM addresses $20-$3F do not alias $00-$1F
  first_byte_not_immediate    the first control byte does not update the address low byte at once
  line_counter_off_by_one     the line interrupt comes one line late (reload value R10 + 1)
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import sms_fixture_rom as builder  # noqa: E402

MUTATIONS = ("latch_not_reset_by_status", "no_read_prefetch", "write_does_not_load_buffer", "no_address_wrap",
             "cram_no_alias", "first_byte_not_immediate", "line_counter_off_by_one")
STATUS_LOW_BITS = 0x1F  # contract section 9.3: bits 4-0 are the project convention %11111


class Vdp:
    def __init__(self, mutation=None):
        assert mutation is None or mutation in MUTATIONS
        self.mutation = mutation
        self.vram = bytearray(0x4000)
        self.cram = bytearray(0x20)
        self.reg = [0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0, 0, 0, 0xFF]
        self.address = 0
        self.code = 0
        self.first = None  # first control byte while the latch is set
        self.buffer = 0
        self.reg_writes = []  # (register, value) of every completed code-2 command, registers 11-15 included
        self.vram_writes = 0  # data-port writes that target VRAM (code 0, 1, 2)
        self.cram_writes = 0  # data-port writes that target CRAM (code 3)

    def _inc(self):
        if self.mutation == "no_address_wrap" and self.address == 0x3FFF:
            return  # the mutant saturates instead of wrapping
        self.address = (self.address + 1) & 0x3FFF

    def control(self, value):
        if self.first is None:
            self.first = value
            if self.mutation != "first_byte_not_immediate":
                self.address = (self.address & 0x3F00) | value
            return
        low = self.first
        self.first = None
        self.code = value >> 6
        self.address = ((value & 0x3F) << 8) | low
        if self.code == 0:
            if self.mutation != "no_read_prefetch":
                self.buffer = self.vram[self.address]
                self._inc()
        elif self.code == 2:
            register = value & 0x0F
            self.reg_writes.append((register, low))
            if register <= 10:
                self.reg[register] = low

    def data_write(self, value):
        self.first = None
        if self.code == 3:
            self.cram_writes += 1
            index = self.address & (0x3F if self.mutation == "cram_no_alias" else 0x1F)
            if index < 0x20:
                self.cram[index] = value & 0x3F
        else:
            self.vram_writes += 1
            self.vram[self.address] = value
        if self.mutation != "write_does_not_load_buffer":
            self.buffer = value
        self._inc()

    def data_read(self):
        self.first = None
        value = self.buffer
        self.buffer = self.vram[self.address]
        self._inc()
        return value

    def status(self):
        if self.mutation != "latch_not_reset_by_status":
            self.first = None
        return STATUS_LOW_BITS  # flags (bits 7-5) are timing/sprite dependent and are masked out of this comparison


def run_sequence(seed, mutation=None):
    """Runs the `vdp_seq_*` fixture program: the generated operations, then the final marker write and the VRAM read-back
    checksum of the ROM. Returns (reads, kinds, checksum, vdp) with reads[i] the byte the ROM stored at $C200+i and
    kinds[i] 'status' or 'data'."""
    vdp = Vdp(mutation)
    reads, kinds = [], []
    for op, arg in builder.vdp_sequence(seed):
        if op == builder.SEQ_CTRL:
            vdp.control(arg)
        elif op == builder.SEQ_DATA_W:
            vdp.data_write(arg)
        elif op == builder.SEQ_STATUS:
            reads.append(vdp.status())
            kinds.append("status")
        elif op == builder.SEQ_DATA_R:
            reads.append(vdp.data_read())
            kinds.append("data")
    vdp.data_write(0x5A)  # the ROM marks the final address
    vdp.control(0)
    vdp.control(0)
    hl = bc = 0
    for _ in range(0x4000):
        bc = (bc + vdp.data_read()) & 0xFFFF
        hl = (hl + bc) & 0xFFFF
    return reads, kinds, (hl, bc), vdp


def line_interrupt_lines(r10, active=192, mutation=None):
    """Lines (0..active, one frame) on which the line interrupt flag is set for a constant R10 (contract section 9.4):
    the counter is reloaded from R10 outside the active display, decremented on lines 0..active, and an underflow
    reloads it and raises the flag."""
    reload_value = r10 + 1 if mutation == "line_counter_off_by_one" else r10
    counter, lines = reload_value, []
    for line in range(active + 1):
        if counter == 0:
            counter = reload_value
            lines.append(line)
        else:
            counter -= 1
    return lines
