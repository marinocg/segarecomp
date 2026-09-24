#include "segarecomp/device/sega/genesis/controller_io.hpp"

#include "runtime.h"

#include <cassert>
#include <cstddef>
#include <variant>

namespace {

using namespace segarecomp;

M68kMemoryAccessWidth host_width(std::uint8_t width) {
  return static_cast<M68kMemoryAccessWidth>(width);
}

M68kMemoryAccessDirection host_direction(std::uint8_t direction) {
  return static_cast<M68kMemoryAccessDirection>(direction);
}

M68kMemoryAccessRequest request_for(std::uint32_t address, M68kMemoryAccessWidth width,
                                    M68kMemoryAccessDirection direction) {
  return {{TargetAddressSpace::m68k_program, address}, width, direction, std::nullopt};
}

void assert_failure(const M68kMemoryAccessRequest &request, GenesisRuntime &runtime) {
  const auto host = m68k_controller_io_access(request);
  assert(std::holds_alternative<M68kControllerIoFailure>(host));
  GenesisRuntimeStop stop{};
  std::uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(&runtime, request.address.value,
                              static_cast<GenesisAccessWidth>(request.width),
                              static_cast<GenesisAccessDirection>(request.direction), &value,
                              &stop) == GENESIS_ACCESS_FAIL);
  if (request.width != M68kMemoryAccessWidth::byte && (request.address.value & 1U) != 0U) {
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
    assert(stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  } else {
    assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
    assert(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
  }
  assert(value == UINT32_C(0xDEADBEEF));
}

// SEG-007-T166: DATA1/DATA2 ($A10003/$A10005) BYTE read is a combinational
// function of the live CTRL/DATA latches, not a fixed table selector, so it
// cannot be folded through m68k_controller_io_access -- it is exclusively a
// routed-device (genesis_route_access) access, mirroring how the T121 test
// above exercises the write-direction GPIO family that is also absent from
// the fixed selector table. Mirrors T121's own CTRL3 read+write parity
// pattern: both TH states, for both DATA1 and DATA2, plus the CTRL-direction
// interaction (an input pin returns the deterministic released-state bit
// regardless of stale DATA-latch content for that pin).
void assert_data_port_read(GenesisRuntime &runtime, std::uint32_t address, std::uint8_t expected) {
  GenesisRuntimeStop stop{};
  std::uint32_t value = UINT32_C(0xDEADBEEF);
  assert(genesis_route_access(&runtime, address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value,
                              &stop) == GENESIS_ACCESS_OK);
  assert(value == static_cast<std::uint32_t>(expected));
  // m68k_controller_io_access never recognizes this address/shape: DATA1/
  // DATA2 reads are exclusively routed-device, not a fixed-table selector.
  const auto host = m68k_controller_io_access(
      request_for(address, M68kMemoryAccessWidth::byte, M68kMemoryAccessDirection::read));
  assert(std::holds_alternative<M68kControllerIoFailure>(host));
}

void test_data1_data2_th_multiplexed_combinational_read() {
  const std::uint32_t data1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 3U; // $A10003
  const std::uint32_t data2 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 5U; // $A10005
  const std::uint32_t ctrl1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 9U; // $A10009
  const std::uint32_t ctrl2 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 0xBU; // $A1000B

  for (const auto &port : {std::pair{data1, ctrl1}, std::pair{data2, ctrl2}}) {
    const auto [data_address, ctrl_address] = port;

    // All pins configured input (CTRL = 0x00, the T042 SS8 power-on default):
    // TH is itself an input pin here (CTRL bit 6 clear), so the deterministic
    // TH default (released/idle-high) selects the TH=1 released group.
    {
      GenesisRuntime runtime{};
      assert_data_port_read(runtime, data_address, 0x7FU); // bit7=0 (unwritten), TH default 1, group 0x3F
    }

    // CTRL configures TH (bit 6) as output, driven high (TH=1): the C/B/
    // Right/Left/Down/Up released-state group (0x3F) is returned on bits 5-0.
    {
      GenesisRuntime runtime{};
      GenesisRuntimeStop stop{};
      std::uint32_t wvalue = UINT32_C(0x40); // CTRL: bit 6 (TH) output, rest input
      assert(genesis_route_access(&runtime, ctrl_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      wvalue = UINT32_C(0x40); // DATA: drive TH high
      assert(genesis_route_access(&runtime, data_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      assert_data_port_read(runtime, data_address, 0x7FU); // bit7=0, TH echo 0x40, group 0x3F
    }

    // CTRL configures TH as output, driven low (TH=0): the Start/A/Down/Up
    // released-state group (0x33, bits 3-2 documented forced '0') is
    // returned on bits 5-0.
    {
      GenesisRuntime runtime{};
      GenesisRuntimeStop stop{};
      std::uint32_t wvalue = UINT32_C(0x40); // CTRL: bit 6 (TH) output, rest input
      assert(genesis_route_access(&runtime, ctrl_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      wvalue = UINT32_C(0x00); // DATA: drive TH low
      assert(genesis_route_access(&runtime, data_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      assert_data_port_read(runtime, data_address, 0x33U); // bit7=0, TH echo 0x00, group 0x33
    }

    // CTRL-direction interaction: every pin configured output (CTRL = 0x7F)
    // reads back exactly the DATA latch's own driven bits, regardless of the
    // documented button-group mapping -- an output pin is never overridden by
    // the released-state input convention.
    {
      GenesisRuntime runtime{};
      GenesisRuntimeStop stop{};
      std::uint32_t wvalue = UINT32_C(0x7F); // CTRL: bits 6-0 all output
      assert(genesis_route_access(&runtime, ctrl_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      wvalue = UINT32_C(0x55); // DATA: an arbitrary fully-driven pattern
      assert(genesis_route_access(&runtime, data_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      assert_data_port_read(runtime, data_address, 0x55U); // bit7 always echoes data-latch bit7 (0) here
    }

    // Stale DATA-latch content for an input-configured pin is ignored: a
    // DATA write that looks like "every button pressed" (0 bits) on the
    // bits-5..0 lane still reads back as the documented all-released group
    // (1 bits) for those bits, because CTRL still marks them as input --
    // only the CTRL direction bit governs whether the driven latch bit or
    // the released-state input bit is returned, never the latch content
    // alone.
    {
      GenesisRuntime runtime{};
      GenesisRuntimeStop stop{};
      std::uint32_t wvalue = UINT32_C(0x40); // CTRL: only TH (bit 6) output
      assert(genesis_route_access(&runtime, ctrl_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      wvalue = UINT32_C(0x40); // DATA: TH driven high; bits 5-0 written "0" (looks pressed)
      assert(genesis_route_access(&runtime, data_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE,
                                  &wvalue, &stop) == GENESIS_ACCESS_OK);
      // bit7=0 (unwritten); TH echo 0x40; bits 5-0 read back as the TH=1
      // released group (0x3F), ignoring the "pressed-looking" 0 bits just
      // written to those same (input-configured) pins.
      assert_data_port_read(runtime, data_address, 0x7FU);
    }
  }
}

} // namespace

// SEG-011-T004: player-1 host pad state. Expected bytes are hand-derived from
// the documented layout: TH=1 "?1CBRLDU" (0x7F released), TH=0 "?0SA00DU"
// (0x33 released), active-low.
void set_th(GenesisRuntime &rt, std::uint32_t data_address, std::uint32_t ctrl_address, unsigned th) {
  GenesisRuntimeStop stop{};
  std::uint32_t v = UINT32_C(0x40);
  assert(genesis_route_access(&rt, ctrl_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &v,
                              &stop) == GENESIS_ACCESS_OK);
  v = th ? UINT32_C(0x40) : UINT32_C(0x00);
  assert(genesis_route_access(&rt, data_address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &v,
                              &stop) == GENESIS_ACCESS_OK);
}

void assert_pad(std::uint8_t pad, unsigned th, std::uint8_t expected) {
  const std::uint32_t d1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 3U;
  const std::uint32_t c1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 9U;
  GenesisRuntime rt{};
  genesis_runtime_set_pad1(&rt, pad);
  set_th(rt, d1, c1, th);
  assert_data_port_read(rt, d1, expected);
}

void test_player1_pad() {
  // Released default is unchanged.
  assert_pad(0, 1, 0x7F); assert_pad(0, 0, 0x33);
  // Directions at TH=1.
  assert_pad(GENESIS_PAD_UP, 1, 0x7E); assert_pad(GENESIS_PAD_DOWN, 1, 0x7D);
  assert_pad(GENESIS_PAD_LEFT, 1, 0x7B); assert_pad(GENESIS_PAD_RIGHT, 1, 0x77);
  // Up/Down at TH=0; Left/Right invisible there (bits 3-2 forced 0).
  assert_pad(GENESIS_PAD_UP, 0, 0x32); assert_pad(GENESIS_PAD_DOWN, 0, 0x31);
  assert_pad(GENESIS_PAD_LEFT | GENESIS_PAD_RIGHT, 0, 0x33);
  // B/C only at TH=1; A/Start only at TH=0.
  assert_pad(GENESIS_PAD_B, 1, 0x6F); assert_pad(GENESIS_PAD_C, 1, 0x5F);
  assert_pad(GENESIS_PAD_B | GENESIS_PAD_C, 0, 0x33);
  assert_pad(GENESIS_PAD_A, 0, 0x23); assert_pad(GENESIS_PAD_START, 0, 0x13);
  assert_pad(GENESIS_PAD_A | GENESIS_PAD_START, 1, 0x7F);
  // Combinations.
  assert_pad(GENESIS_PAD_RIGHT | GENESIS_PAD_B, 1, 0x67);
  assert_pad(GENESIS_PAD_START | GENESIS_PAD_A, 0, 0x03);
  // Release restores the released bits.
  {
    const std::uint32_t d1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 3U;
    const std::uint32_t c1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 9U;
    GenesisRuntime rt{};
    set_th(rt, d1, c1, 1);
    genesis_runtime_set_pad1(&rt, GENESIS_PAD_RIGHT | GENESIS_PAD_B);
    assert_data_port_read(rt, d1, 0x67);
    genesis_runtime_set_pad1(&rt, 0);
    assert_data_port_read(rt, d1, 0x7F);
  }
  // Input pin TH (CTRL clear) with pad held: default TH=1 group.
  {
    GenesisRuntime rt{};
    genesis_runtime_set_pad1(&rt, GENESIS_PAD_C);
    assert_data_port_read(rt, SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 3U, 0x5F);
  }
  // Port 2 stays released whatever player 1 does.
  {
    const std::uint32_t d2 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 5U;
    const std::uint32_t c2 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 0xBU;
    GenesisRuntime rt{};
    genesis_runtime_set_pad1(&rt, 0xFFU);
    set_th(rt, d2, c2, 1);
    assert_data_port_read(rt, d2, 0x7F);
    set_th(rt, d2, c2, 0);
    assert_data_port_read(rt, d2, 0x33);
  }
  // Writes/latches unaffected by pad state: mixed CTRL mask echoes DATA on output pins.
  {
    const std::uint32_t d1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 3U;
    const std::uint32_t c1 = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN + 9U;
    GenesisRuntime rt{};
    GenesisRuntimeStop stop{};
    genesis_runtime_set_pad1(&rt, GENESIS_PAD_UP | GENESIS_PAD_B);
    std::uint32_t v = 0x41U; // TH + bit0 outputs
    assert(genesis_route_access(&rt, c1, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
    v = 0xC0U; // bit7 latch, TH=1, bit0 = 0 driven
    assert(genesis_route_access(&rt, d1, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK);
    // bit7 echo 0x80 | TH 0x40 | group: B pressed (bit4 clear) -> 0x2E; bit0 output = 0
    assert_data_port_read(rt, d1, 0xEEU);
  }
}

int main() {
  test_player1_pad();
  GenesisRuntime runtime{};
  for (std::size_t index = 0; index < SEGARECOMP_GENESIS_CONTROLLER_IO_SELECTOR_COUNT; ++index) {
    const auto &selector = segarecomp_genesis_controller_io_selectors[index];
    const M68kMemoryAccessRequest request = request_for(
        selector.address, host_width(selector.width), host_direction(selector.direction));
    const auto host = m68k_controller_io_access(request);
    const auto *success = std::get_if<M68kControllerIoResult>(&host);
    assert(success != nullptr);
    assert(success->value == selector.policy_value);
    assert(static_cast<unsigned>(success->policy_provenance) + 1U == selector.identity);

    GenesisRuntimeStop stop{};
    std::uint32_t value = UINT32_C(0xDEADBEEF);
    assert(genesis_route_access(&runtime, selector.address,
                                static_cast<GenesisAccessWidth>(selector.width),
                                static_cast<GenesisAccessDirection>(selector.direction), &value,
                                &stop) == GENESIS_ACCESS_OK);
    assert(value == success->value);

    // SEG-007-T121: CTRL3 ($A1000D) is simultaneously a read selector and the
    // one runtime-reached GPIO BYTE-write latch. For that single address a BYTE
    // write is an accepted latched store; the read behaviour above stays
    // byte-for-byte unchanged (it never consults the latch). Every other
    // read-selector address keeps rejecting writes.
    const int gpio_write_slot = segarecomp_genesis_controller_io_gpio_register_index(
        selector.address, static_cast<std::uint32_t>(host_width(selector.width)));
    if (gpio_write_slot >= 0) {
      GenesisRuntime rw{};
      GenesisRuntimeStop wstop{};
      std::uint32_t wvalue = UINT32_C(0x0000005A);
      assert(genesis_route_access(&rw, selector.address, GENESIS_ACCESS_BYTE,
                                  GENESIS_ACCESS_WRITE, &wvalue, &wstop) == GENESIS_ACCESS_OK);
      assert(rw.devices.controller_io.ctrl[2] == 0x5A);
      // Read-behaviour-unchanged guarantee: after the write, a read at the same
      // address still returns the read selector's policy constant, not the latch.
      std::uint32_t rvalue = UINT32_C(0xDEADBEEF);
      assert(genesis_route_access(&rw, selector.address,
                                  static_cast<GenesisAccessWidth>(selector.width),
                                  static_cast<GenesisAccessDirection>(selector.direction),
                                  &rvalue, &wstop) == GENESIS_ACCESS_OK);
      assert(rvalue == success->value);
    } else {
      assert_failure(request_for(selector.address, host_width(selector.width),
                                 M68kMemoryAccessDirection::write), runtime);
    }
    const auto wrong_width = static_cast<std::uint8_t>(selector.width == 1U ? 2U : 1U);
    assert_failure(request_for(selector.address, host_width(wrong_width),
                               M68kMemoryAccessDirection::read), runtime);
  }
  assert_failure(request_for(SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN,
                             M68kMemoryAccessWidth::byte,
                             M68kMemoryAccessDirection::read), runtime);
  test_data1_data2_th_multiplexed_combinational_read();
}
