#include "runtime.h"
#include "../../../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"
#include "../machine/include/segarecomp/machine/genesis/address_space_contract.h"
#include "../../../libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#if defined(_WIN32)
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>
#endif

static int genesis_valid_rom_sha256(const char *value);
static int genesis_checkpoint_identity_is_valid(const GenesisCheckpointIdentity *identity);

static GenesisRuntimeStop genesis_access_stop(GenesisStopClass stop_class,
                                               GenesisDiagnosticCategory category) {
  GenesisRuntimeStop stop = {0};
  stop.stop_class = stop_class;
  stop.diagnostic_category = category;
  return stop;
}

static int genesis_width_is_valid(GenesisAccessWidth width) {
  return width == GENESIS_ACCESS_BYTE || width == GENESIS_ACCESS_WORD || width == GENESIS_ACCESS_LONG;
}

static int genesis_is_work_ram(uint32_t address, uint32_t width) {
  return segarecomp_genesis_work_ram_contains(address, width);
}

/* The basic MC68000 frame has no vector/origin field. Keep the project-only
 * IRQ6 scheduler provenance out-of-band, keyed by its aligned work-RAM frame
 * address, so nested synchronous frames cannot consume an outer IRQ6 origin. */
static uint8_t genesis_exception_frame_origin_mask(uint32_t frame_base) {
  const uint32_t word_slot = (frame_base - SEGARECOMP_GENESIS_WORK_RAM_BEGIN) >> 1U;
  return (uint8_t)(UINT8_C(1) << (word_slot & 7U));
}

static uint32_t genesis_exception_frame_origin_byte(uint32_t frame_base) {
  return ((frame_base - SEGARECOMP_GENESIS_WORK_RAM_BEGIN) >> 1U) >> 3U;
}

static void genesis_note_irq6_exception_frame(GenesisRuntime *runtime, uint32_t frame_base) {
  const uint32_t byte = genesis_exception_frame_origin_byte(frame_base);
  runtime->exception_frame_irq6_origin[byte] =
      (uint8_t)(runtime->exception_frame_irq6_origin[byte] | genesis_exception_frame_origin_mask(frame_base));
}

static int genesis_take_irq6_exception_frame_origin(GenesisRuntime *runtime, uint32_t frame_base) {
  const uint32_t byte = genesis_exception_frame_origin_byte(frame_base);
  const uint8_t mask = genesis_exception_frame_origin_mask(frame_base);
  const int is_irq6 = (runtime->exception_frame_irq6_origin[byte] & mask) != 0U;
  runtime->exception_frame_irq6_origin[byte] =
      (uint8_t)(runtime->exception_frame_irq6_origin[byte] & (uint8_t)(UINT8_C(0xFF) ^ mask));
  return is_irq6;
}

/* This bounded classification is deliberately only a fail-closed device lane. */
static int genesis_is_device(uint32_t address) {
  return address >= SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN && address < SEGARECOMP_GENESIS_CONTROLLER_IO_END;
}

/* SEG-007-T039: routes exactly the two policy-defined controller-I/O
   selectors this project currently implements -- the SEG-007-T020/T021
   CTRL1/CTRL2 LONG-read selector (src/m68k_pipeline.cpp's
   m68k_controller_io_access) and the SEG-007-T038 CTRL3 WORD-read selector
   -- to the exact same zero-valued policy result the static resolver
   returns for the identical access shape
   (docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md).
   Every other controller-I/O address, width, or direction within the
   recognized device region remains unconditionally fail-closed by the sole
   caller below, matching SEG-007-T030's already-validated runtime
   baseline. This is recognition-only: neither selector reads or mutates
   any runtime/controller/bus state, and it makes no claim that any
   compiled/executed generated program can currently reach this routing
   extension (SEG-007-T040 closes that separate coverage gap). */
/* SEG-007-T121: the write mirror -- a deterministic latched store to one of the
   six GPIO data/direction registers (DATA1..DATA3, CTRL1..CTRL3), BYTE width
   only (the runtime-reached form is a CTRL3 write; WORD/LONG stay fail-closed
   via the register recognizer). CTRL3 $A1000D is simultaneously the T111/T038
   read-selector address; a read there still returns the unchanged read-selector
   policy constant (this latch is never read back). This mutates only
   devices->controller_io
   and is side-effect-free with respect to every read selector. It is a
   replaceable PROJECT COMPATIBILITY POLICY, not verified hardware behaviour --
   see the SEG-007-T121 section of the compatibility-policy document. All
   validation precedes the single store, so a rejected access mutates nothing
   (T042 SS3 / genesis_route_access's "on failure neither it nor the runtime is
   modified" contract). `*value` carries the caller's write value in. */
/* SEG-007-T166: the three-button-pad DATA1/DATA2 BYTE-read combinational
   function. Per pin, the returned bit is either the CTRL latch's own
   host-driven DATA-latch bit (CTRL bit set -- output/host-driven pin) or a
   deterministic "all buttons released" input value selected by the TH
   (select) line's own current state (CTRL bit clear -- controller-driven
   input pin). This is the documented three-button joypad protocol: DATA
   bit 6 is TH; TH=1 returns "?1CBRLDU" (C, B, Right, Left, Down, Up) on
   bits 5-0; TH=0 returns "?0SA00DU" (Start, A, then bits 3-2 forced to
   '0', Down, Up) on bits 5-0; a '0' bit means pressed, a '1' bit means
   released (GTO1 pp. 72, 75's documented CTRL direction/DATA-latch
   mechanism, corroborated by Charles MacDonald's Sega Genesis hardware
   notes ("MCD1"), gen-hw.txt SS3.2 "Gamepad specifics", for the TH-bit
   position and the two per-TH-state button-group bit layouts -- see the
   SEG-007-T166 section of
   docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md).
   DATA bit 7 has no corresponding CTRL direction bit (MCD1: "Bit 7 isn't
   connected to any pin... it will latch a value written to it") -- it
   always echoes the DATA latch's own bit 7, regardless of CTRL. Player 1
   (port 1) buttons come from the host-supplied `pad` mask (GENESIS_PAD_*,
   SEG-011-T004); a zero mask, and port 2 always, is every button released. */
static uint8_t genesis_controller_io_data_port_read(uint8_t ctrl, uint8_t data, uint8_t pad) {
  const unsigned th = (ctrl & 0x40U) ? ((unsigned)(data >> 6) & 1U) : 1U;
  /* Released-state 6-bit (bits 5-0) button-group value for the current TH
     state: TH=1 -> "1CBRLDU" = 0x3F (C,B,R,L,D,U all released); TH=0 ->
     "0SA00DU" = 0x33 (S,A,D,U released; bits 3-2 documented forced '0'). */
  const uint8_t input_group = th
      ? (uint8_t)(0x3FU & ~(((pad & GENESIS_PAD_UP) ? 0x01U : 0U) |
                            ((pad & GENESIS_PAD_DOWN) ? 0x02U : 0U) |
                            ((pad & GENESIS_PAD_LEFT) ? 0x04U : 0U) |
                            ((pad & GENESIS_PAD_RIGHT) ? 0x08U : 0U) |
                            ((pad & GENESIS_PAD_B) ? 0x10U : 0U) |
                            ((pad & GENESIS_PAD_C) ? 0x20U : 0U)))
      : (uint8_t)(0x33U & ~(((pad & GENESIS_PAD_UP) ? 0x01U : 0U) |
                            ((pad & GENESIS_PAD_DOWN) ? 0x02U : 0U) |
                            ((pad & GENESIS_PAD_A) ? 0x10U : 0U) |
                            ((pad & GENESIS_PAD_START) ? 0x20U : 0U)));
  const uint8_t input_value = (uint8_t)((th << 6U) | input_group);
  uint8_t result = (uint8_t)(data & 0x80U); /* bit 7: always DATA-latch echo */
  unsigned bit;
  for (bit = 0U; bit < 7U; ++bit) {
    const unsigned mask = 1U << bit;
    result = (uint8_t)(result | ((ctrl & mask) ? (data & mask) : (input_value & mask)));
  }
  return result;
}

static int genesis_controller_io_access(GenesisDeviceState *devices, uint8_t pad1, uint32_t address,
                                          GenesisAccessWidth width,
                                          GenesisAccessDirection direction, uint32_t *value) {
  size_t index;
  if (direction == GENESIS_ACCESS_WRITE) {
    int slot = segarecomp_genesis_controller_io_gpio_register_index(address, (uint32_t)width);
    if (slot < 0) return 0;
    if (slot < 3) devices->controller_io.data[slot] = (uint8_t)(*value & 0xFFU);
    else devices->controller_io.ctrl[slot - 3] = (uint8_t)(*value & 0xFFU);
    return 1;
  }
  {
    const int data_port_slot =
        segarecomp_genesis_controller_io_gpio_register_index(address, (uint32_t)width);
    if (data_port_slot == 0 || data_port_slot == 1) {
      *value = genesis_controller_io_data_port_read(devices->controller_io.ctrl[data_port_slot],
                                                      devices->controller_io.data[data_port_slot],
                                                      data_port_slot == 0 ? pad1 : 0U);
      return 1;
    }
    if (data_port_slot == 2) { /* DATA3 (expansion port, no device): same pin model as DATA1/DATA2 with every line released */
      *value = genesis_controller_io_data_port_read(devices->controller_io.ctrl[2], devices->controller_io.data[2], 0U);
      return 1;
    }
    if (data_port_slot >= 3 && data_port_slot <= 4) { /* CTRL1, CTRL2: documented R/W direction latches read back */
      *value = devices->controller_io.ctrl[data_port_slot - 3];
      return 1;
    }
  }
  for (index = 0U; index < SEGARECOMP_GENESIS_CONTROLLER_IO_SELECTOR_COUNT; ++index) {
    const SegarecompGenesisControllerIoSelector *selector =
        &segarecomp_genesis_controller_io_selectors[index];
    if (address != selector->address || (uint32_t)width != selector->width ||
        (uint32_t)direction != selector->direction) continue;
    *value = selector->policy_value;
    return 1;
  }
  return 0;
}

/* This bounded classification is deliberately only a fail-closed device
   lane, mirroring genesis_is_device's own single-interval-recognition
   shape exactly. The interval matches GTO1 p. 10's "VDP AREA" base
   register block (DATA $C00000, CONTROL $C00004, HV COUNTER $C00008, PSG
   76489 $C00011), sized to the same 0x20-byte convention
   genesis_is_device already uses for the controller-I/O window -- see the
   VDP addendum to
   docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md.
   The co-located PSG (SN76489) audio port at $C00011 has its own dedicated
   fail-closed-style lane (genesis_is_psg_region / genesis_psg_access below),
   routed ahead of this VDP lane in genesis_route_access. */
static int genesis_is_vdp_region(uint32_t address) {
  /* One source of truth for the VDP-window interval, shared with the
     translation-time device-routing gate. Byte-granular recognition
     (width 1) preserves the exact prior interval semantics. */
  return segarecomp_genesis_vdp_region_contains(address, 1U);
}

/* SEG-007-T081: the original policy-defined VDP READ selector was a WORD
    read of the VDP control port's base address ($C00004), which GTO1 p. 19 and MCD1's VDP port
   documentation both establish also serves as the VDP status-register read
   path. The routed value is `devices->vdp.status_register` itself (never a
    hardcoded constant): this project has no VDP interrupt state-mutation
    path, and T084 does not map DMA phase into status bits, so that field remains at its
    zero-initialized default (T042 SS8) and reads currently
   observes 0x0000 -- an explicit SEG-007-T081 project compatibility
   policy, never a claim about real Genesis VDP status-register runtime
    behavior (VBlank/HBlank/FIFO/DMA-busy/collision/overflow bits are
    genuinely dynamic hardware state this project does not yet model).
    BYTE reads at $C00004/$C00005 now return the modeled high/low byte of
    the same status word; each successful status read cancels the pending
    two-word control command and counts as one synthetic VBlank observation.
    Unused high bits are a project policy, not reconstructed open bus. See
    docs/references/genesis-vdp-status-byte-read-contract.md and
   the VDP addendum to
   docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md
   for the full citation/policy discussion.

   SEG-007-T091 adds the WORD-WRITE command-word protocol at the same
   address ($C00004 only -- the second documented mirror lane $C00006 is not
   implemented for either direction), per GTO1 p. 20 "WRITE1: REGISTER SET"/
   "WRITE2: ADDRESS SET" (independently re-verified against the primary PDF
   diagrams, not merely the lower-fidelity OCR text derivative also cited
   elsewhere in this project):

   (a) Register-set command (one WORD): bits 15-13 fixed "100", RS4-RS0
       (bits 12-8) the register number, D7-D0 (bits 7-0) the data byte.
       Implemented for RS in the documented valid range #0-#23
       (GENESIS_VDP_REGISTER_COUNT); an RS value outside that range remains
       fail-closed. Writing register #15 also updates `auto_increment_value`
       as a named post-access side effect (GTO1 p. 28/p. 37: register #15's
       INC7-INC0 field *is* the documented VRAM/CRAM/VSRAM address
       auto-increment value) -- a project inference connecting T042 SS1.3's
       reserved `auto_increment_value` field to the one register GTO1
       documents as its source, since GTO1's own WRITE2 two-word diagrams
       carry no increment-value bits of their own.

   (b) Two-word, non-DMA VRAM/CRAM/VSRAM address-set command: first word
       (any WORD write to $C00004 not matching (a)'s bit-15-13 "100"
       pattern while not already awaiting a second word) latches
       `control_port_first_word` and sets
       `control_port_awaiting_second_word`; the following WORD write to
       $C00004 is always treated as the second word (per the VDP's own
       documented two-word command state machine -- GTO1 draws no separate
       "abort/restart" case). The second word's CD5-CD0 code (CD1/CD0 from
       the first word's bits 15-14; CD5-CD2 from the second word's bits
       7-4) is accepted only for the six explicit non-DMA codes GTO1 p. 20/
       p. 27 tabulates (VRAM/CRAM/VSRAM READ/WRITE: 0x00, 0x01, 0x03, 0x04,
       0x05, 0x08); the composed 16-bit address (A15-A14 from the second
       word's bits 1-0; A13-A0 from the first word's bits 13-0) is stored in
       `addressed_pointer`. Any other second-word bit pattern -- including
       every reserved bit GTO1 documents as fixed-zero -- remains
       fail-closed, leaving the pending first-word state untouched (matching
       genesis_route_access's own "on failure neither it nor the runtime is
       modified" contract).

   Deliberately, explicitly narrowed out (fail-closed) this pass, per this
   task's own bounded Scope:

    - SEG-007-T084 now accepts only the cited memory-to-VRAM DMA subset;
      every other CD5-set command remains fail-closed. See
      docs/references/genesis-vdp-dma-contract.md.
    - The second documented CONTROL-port mirror lane, $C00006, for either
      direction (T081's own prior narrowing, unchanged by this task).
   - Any VRAM/CRAM/VSRAM data-port ($C00000) byte-level read/write access
     itself (SEG-007-T083's own separate scope); this task only populates
     the `addressed_pointer`/`auto_increment_value` state a future data-port
     access would consume.

   LONG-word writes to $C00004 (SEG-007-T091, second frontier pass): GTO1
   p. 20 documents "Long word access is equivalent to two word accesses,
   with D31-D16 written first." This is decomposed below by splitting the
   32-bit value into `high` (bits 31-16) and `low` (bits 15-0) and calling
   `genesis_vdp_control_port_write_word` -- the exact same WORD-write body
   above, factored into its own static helper -- first with `high`, then
   (only if that first call succeeds) with `low`. Each of the two calls is
   itself exactly one WORD write through the identical command-word state
   machine (a) and (b) document; a LONG write can therefore complete two
   chained register-set commands, or a two-word address-set command's first
   and second word, in a single access.

   Partial-completion policy (high word succeeds, low word fails): this
   returns overall failure (`genesis_vdp_access` returns 0, so
   `genesis_route_access` reports GENESIS_ACCESS_FAIL for the whole LONG
   access) WITHOUT rolling back whatever state the high word's own
   already-successful WORD write already durably committed. This is a
   deliberate, explicit, narrow exception to `genesis_route_access`'s own
   general "on failure neither it nor the runtime is modified" contract
   (see its header doc comment), justified specifically and only because
   GTO1 itself documents a LONG write at $C00004 as literally two
   independent, sequential bus transactions -- not one atomic operation.
   Real 68000/VDP hardware performing this same two-transaction bus sequence
   would identically leave the first transaction's effect committed if the
   second transaction were rejected by the VDP for any reason; modeling
   this pair as a single roll-back-able unit would not just complicate the
   implementation, it would depart from the two-sequential-transaction
   hardware model GTO1 itself documents. Every *other* WRITE selector in
   this file (including each standalone WORD write, and the (b) two-word
   address-set command's own "no mutation on rejection" rule for a single
   rejected second word) still fully honors the general contract; only this
   specific LONG-write two-sub-write composition is exempted, and only
   because each sub-write is independently a complete, self-contained,
   already-fully-validated-before-mutating WORD write in its own right (see
   `genesis_vdp_control_port_write_word` below: every validation check for a
   given word precedes every mutation for that same word, so a single
   sub-write is still atomic -- only the two-sub-write LONG sequence as a
   whole is not).

   See the VDP addendum to
   docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md
   for the full citation/policy discussion of every selector above, including
   this LONG-write subsection. */

/* SEG-007-T091 (second frontier pass): the exact WORD-write command-word
   protocol body, factored out of genesis_vdp_access's own original WORD-write
   case into its own static helper so the LONG-write decomposition documented
   above can invoke it twice without duplicating any command-word logic. This
   is a pure refactor of that original body -- register-set command
   detection, two-word address-set sequencing, and every existing
   narrowing/rejection rule are unchanged; only its shape (a standalone
   function taking the already-extracted 16-bit `word`, returning success/
   failure) is new. Every validation check for `word` precedes every mutation
   of `devices->vdp` in every path below, so a single call to this helper is
   itself always atomic: on failure it returns 0 having mutated nothing. */
static int genesis_vdp_dma_source_is_routable(uint32_t address) {
  return address < UINT32_C(0x00400000) || genesis_is_work_ram(address, 2U);
}

/* SEG-007-T169: shared CD5-CD0 write-target-code -> destination-buffer
   mapping. GTO1 p. 20/p. 27's access-mode table documents VRAM WRITE = 0x01,
   CRAM WRITE = 0x03, VSRAM WRITE = 0x05 (see docs/references/genesis-vdp-
   data-port-cpu-write-contract.md, already cited for the non-DMA CPU
   DATA-port write path below); this helper is reused unchanged by both that
   path and the memory-to-target DMA transfer engine below, whose own armed
   `write_target_code` is the identical CD5-CD0 code with the CD5 (DMA) bit
   masked off. Every other code -- including every READ code -- is not a
   write target and returns NULL. */
static uint8_t *genesis_vdp_write_target_buffer(GenesisVdpState *vdp, uint8_t write_code,
                                                uint32_t *out_size) {
  if (write_code == 0x01U) { *out_size = GENESIS_VDP_VRAM_BYTES; return vdp->vram; }
  if (write_code == 0x03U) { *out_size = GENESIS_VDP_CRAM_BYTES; return vdp->cram; }
  if (write_code == 0x05U) { *out_size = GENESIS_VDP_VSRAM_BYTES; return vdp->vsram; }
  return NULL;
}

/* SEG-007-T084: the project advances one selected DMA word only after a
   successful routed CONTROL-port status read while busy. This is an explicit
   deterministic access-caused progression policy (not a timing model). */
static GenesisAccessResultKind genesis_vdp_progress_dma(GenesisRuntime *runtime,
                                                          GenesisRuntimeStop *stop_out) {
  GenesisVdpDmaState *dma = &runtime->devices.vdp.dma;
  GenesisVdpState *vdp = &runtime->devices.vdp;
  uint32_t value = 0U;
  uint32_t destination;
  uint32_t target_size = 0U;
  uint8_t *target;
  if (dma->phase != GENESIS_VDP_DMA_BUSY) return GENESIS_ACCESS_OK;
  /* A fill is driven exclusively by its DATA-port source WORD, not by the
     bounded status-read progression policy for memory-to-VRAM DMA. */
  if (dma->kind == GENESIS_VDP_DMA_VRAM_FILL) return GENESIS_ACCESS_OK;
  /* SEG-007-T169: the armed command's write-target code was already
     restricted to a documented VRAM/CRAM/VSRAM write shape in
     genesis_vdp_control_port_write_word below before this DMA was ever
     armed, so this lookup cannot fail for a DMA armed through that path;
     the NULL check is a defensive fail-closed guard, not a reachable case. */
  target = genesis_vdp_write_target_buffer(vdp, dma->write_target_code, &target_size);
  if (target == NULL) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    return GENESIS_ACCESS_FAIL;
  }
  if (!genesis_vdp_dma_source_is_routable(dma->source_address)) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_route_access(runtime, dma->source_address, GENESIS_ACCESS_WORD,
                           GENESIS_ACCESS_READ, &value, stop_out) != GENESIS_ACCESS_OK)
    return GENESIS_ACCESS_FAIL;
  destination = vdp->addressed_pointer % target_size;
  /* SEG-007-T169: a CRAM/VSRAM DMA destination requires an even current
     address, mirroring the plain CPU DATA-port write's identical
     documented-uncertainty policy (genesis_vdp_data_port_target_write_
     halfword above declines an odd CRAM/VSRAM address the same way before
     any mutation). VRAM's own DMA behavior is completely unchanged by this
     task -- it never had, and still does not have, this check. */
  if (target != vdp->vram && (destination & 1U) != 0U) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    return GENESIS_ACCESS_FAIL;
  }
  target[destination] = (uint8_t)(value >> 8);
  target[(destination + 1U) % target_size] = (uint8_t)value;
  vdp->addressed_pointer = (destination + vdp->auto_increment_value) % target_size;
  dma->source_address = (dma->source_address + 2U) & UINT32_C(0x00FFFFFE);
  --dma->remaining_length;
  ++dma->transfer_access_count;
  if (dma->remaining_length == 0U) {
    dma->phase = GENESIS_VDP_DMA_IDLE;
    /* Compat repair: a finished memory-to-VDP DMA leaves the VDP's command code and address register where the transfer ended
       (the DMA bit is ignored by the data port), so the cartridge may continue with plain DATA-port writes into the same
       target without a fresh address command. The armed write target (VRAM/CRAM/VSRAM, already restricted to the documented
       write codes) becomes the selected plain-write transfer code and the post-transfer address is the pointer above. */
    vdp->data_port_transfer_code = dma->write_target_code;
    vdp->data_port_transfer_code_valid = 1U;
  }
  return GENESIS_ACCESS_OK;
}

/* SEG-007-T175: real Genesis hardware stops the 68000 from fetching or
   executing any further instruction while a 68000-memory-to-VDP
   (VRAM/CRAM/VSRAM) DMA transfer is in progress -- the VDP takes over the
   68000 bus immediately following the DMA-triggering CONTROL-port write and
   holds it until the transfer completes (see the VDP DMA citations already
   referenced by SEG-007-T084/T091/T098/T101/T108 and
   docs/references/genesis-vdp-data-port-cpu-write-contract.md; even the
   second half of an in-flight CPU LONG access can be held off until the
   transfer finishes). segarecomp's own SEG-007-T084 DMA progression policy is
   explicitly NOT a timing model, so before this task it let generated CPU
   dispatch resume between individual DMA words -- something real hardware's
   bus-stall behaviour never permits.

   PRIMARY suspension mechanism (reviewer correction, this same task): a
   generated dispatch/basic-block boundary is NOT the right granularity for
   this invariant -- one generated block can contain several 68000
   instructions in straight-line sequence (the pre-existing SEG-007-T084
   fixture itself arms and, under the old policy, progressed a DMA within a
   single generated block), so a CONTROL-port write that arms the DMA and a
   later straight-line instruction in the SAME block must not both execute
   before this mechanism intervenes. `genesis_vdp_drain_memory_to_vdp_dma_body`
   is therefore invoked synchronously by `genesis_route_access` (below) as a
   post-access, device-side effect of the exact routed VDP CONTROL-port write
   that transitions the DMA into `GENESIS_VDP_DMA_BUSY` /
   `GENESIS_VDP_DMA_MEMORY_TO_VRAM` -- BEFORE that routed access itself
   returns control to generated C. This drains the existing memory-to-VDP
   transfer engine (genesis_vdp_progress_dma's own transfer body above,
   reused unchanged) fully to GENESIS_VDP_DMA_IDLE, so even a following
   straight-line instruction in the identical generated block cannot execute
   until the DMA the program itself just armed has completed. This is
   ordering/visibility only -- no cycle-accurate timing is modeled.

   Partial-completion policy: if the DMA-triggering CONTROL-port write has
   already committed (the arm itself always succeeds atomically, exactly as
   before this task) and the synchronous drain then fails because the DMA
   source becomes unroutable, that already-committed arm is left in place
   (`dma.phase` stays `GENESIS_VDP_DMA_BUSY` at whatever partial transfer
   point the failure occurred) and the overall routed access reports failure
   -- the identical documented non-atomic partial-completion policy
   `genesis_vdp_access`'s own LONG-write decomposition (SEG-007-T091) already
   uses, applied one level up.

   GENESIS_VDP_DMA_VRAM_FILL is deliberately excluded: genesis_vdp_progress_dma
   itself already treats a FILL as a no-op (its payload/completion is driven
   exclusively by the CPU's own DATA-port write below), so draining it here
   would never terminate. The existing FILL write-routing path, its WORD-only
   restriction, and its CPU-driven DATA-port trigger are completely unchanged
   by this task.

   Termination is guaranteed by the transfer engine's own existing, unchanged
   contract: genesis_vdp_progress_dma strictly decrements `remaining_length`
   on every successful step and clears BUSY once it reaches zero, so this loop
   always finishes in a bounded, finite number of steps for the exact word
   count the generated program itself armed. On an unroutable DMA source the
   loop stops immediately with the same fail-closed stop the status-read-
   triggered path already produced. None of these drain steps call the
   generated dispatch function, so they cannot masquerade as CPU instruction
   execution and never participate in any runner/guest accounting (there is
   no watchdog progress-credit accounting left; SEG-007-T252 / ADR-0040).

   DEFENSIVE secondary mechanism: `genesis_runtime_step` (below) also still
   drains any already-`BUSY` memory-to-VDP DMA immediately before each
   dispatch step. With the synchronous drain above as the primary mechanism,
   every reachable arm site already leaves the DMA `IDLE` before returning to
   generated C, so this pre-dispatch check should never actually observe
   `BUSY` in practice -- it remains only as a defensive invariant against an
   already-`BUSY` runtime state (for example a future arm site added without
   also wiring the synchronous drain), never as the sole suspension
   mechanism. */
static GenesisAccessResultKind genesis_vdp_drain_memory_to_vdp_dma_body(GenesisRuntime *runtime,
                                                                         GenesisRuntimeStop *stop_out) {
  GenesisVdpDmaState *dma = &runtime->devices.vdp.dma;
  if (dma->phase != GENESIS_VDP_DMA_BUSY || dma->kind != GENESIS_VDP_DMA_MEMORY_TO_VRAM)
    return GENESIS_ACCESS_OK;
  while (dma->phase == GENESIS_VDP_DMA_BUSY) {
    if (genesis_vdp_progress_dma(runtime, stop_out) != GENESIS_ACCESS_OK) return GENESIS_ACCESS_FAIL;
  }
  return GENESIS_ACCESS_OK;
}

/* Step wrapper: adapts the shared drain body above to the
   GenesisControlTransfer shape genesis_runtime_step's own stop path uses.
   See the defensive-mechanism note above the shared body. */
static int genesis_vdp_drain_memory_to_vdp_dma(GenesisRuntime *runtime,
                                                GenesisControlTransfer *transfer_out) {
  GenesisRuntimeStop stop = {0};
  if (genesis_vdp_drain_memory_to_vdp_dma_body(runtime, &stop) != GENESIS_ACCESS_OK) {
    transfer_out->kind = GENESIS_STOP;
    transfer_out->next_pc = runtime->pc;
    transfer_out->stop = stop;
    return 0;
  }
  return 1;
}

static int genesis_vdp_control_port_write_word(GenesisDeviceState *devices, uint16_t word) {
  if (devices->vdp.control_port_awaiting_second_word) {
    /* Second word of a pending two-word address-set command. */
    uint16_t first = devices->vdp.control_port_first_word;
    uint8_t cd1 = (uint8_t)((first >> 15) & 1U);
    uint8_t cd0 = (uint8_t)((first >> 14) & 1U);
    uint8_t high_byte = (uint8_t)((word >> 8) & 0xFFU);       /* documented fixed-zero */
    uint8_t cd5 = (uint8_t)((word >> 7) & 1U);
    uint8_t cd4 = (uint8_t)((word >> 6) & 1U);
    uint8_t cd3 = (uint8_t)((word >> 5) & 1U);
    uint8_t cd2 = (uint8_t)((word >> 4) & 1U);
    uint8_t reserved_bits = (uint8_t)((word >> 2) & 3U);      /* documented fixed-zero */
    uint8_t code = (uint8_t)((uint8_t)(cd5 << 5) | (uint8_t)(cd4 << 4) | (uint8_t)(cd3 << 3) |
                              (uint8_t)(cd2 << 2) | (uint8_t)(cd1 << 1) | cd0);
    uint32_t addr_high2 = (uint32_t)(word & 3U);              /* A15,A14 */
    uint32_t addr_low14 = (uint32_t)(first & UINT32_C(0x3FFF)); /* A13-A0 */
    if (high_byte != 0U || reserved_bits != 0U) return 0; /* undocumented reserved bit set */
    if (cd5 != 0U) {
      /* SEG-007-T084: for memory-to-VDP DMA, register #23 bit 7 is clear
         and bits 6--0 are source A23--A17. Bit 7 set selects the separate
         fill/copy family. This runtime accepts only the bounded fill mode;
         copy remains fail-closed. */
      uint32_t length = (uint32_t)(devices->vdp.registers[19] & 0xFFU) |
                        ((uint32_t)(devices->vdp.registers[20] & 0xFFU) << 8);
      uint32_t source = ((uint32_t)(devices->vdp.registers[23] & 0x7FU) << 17) |
                        ((uint32_t)(devices->vdp.registers[22] & 0xFFU) << 9) |
                        ((uint32_t)(devices->vdp.registers[21] & 0xFFU) << 1);
      uint8_t dma_mode = (uint8_t)(devices->vdp.registers[23] & 0xC0U);
      int fill_mode = dma_mode == 0x80U;
      /* In the memory-to-VRAM family bit 6 remains source A23; only the
         10/11 encodings select fill/copy respectively. */
      int memory_mode = (devices->vdp.registers[23] & 0x80U) == 0U;
      /* SEG-007-T169: the DMA-family write-target code (CD5 already known
         set here; mask it off to compare against the identical non-DMA
         VRAM/CRAM/VSRAM WRITE codes GTO1 p. 20/p. 27's access-mode table
         documents -- 0x01/0x03/0x05, the same codes the non-DMA two-word
         address-set command below already accepts). Only a documented write
         target is armable; every other CD5-set code (every DMA READ code,
         and every undocumented combination) remains fail-closed, unchanged
         from before this task. The bounded fill-mode family (SEG-007-T098/
         T101) stays VRAM-only: genesis_vdp_data_port_fill_write only ever
         targets vdp->vram, so a fill armed against a CRAM/VSRAM code would
         silently mismatch its own destination -- fail closed instead. */
      uint8_t write_target_code = (uint8_t)(code & 0x1FU);
      if ((devices->vdp.registers[1] & 0x10U) == 0U) return 0;
      if (write_target_code != 0x01U && write_target_code != 0x03U && write_target_code != 0x05U)
        return 0;
      if (fill_mode && write_target_code != 0x01U) return 0;
      if (!memory_mode && !fill_mode) return 0;
      devices->vdp.addressed_pointer = (addr_high2 << 14) | addr_low14;
      devices->vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      devices->vdp.dma.kind = fill_mode ? GENESIS_VDP_DMA_VRAM_FILL
                                         : GENESIS_VDP_DMA_MEMORY_TO_VRAM;
      devices->vdp.dma.source_address = memory_mode ? source : 0U;
      devices->vdp.dma.remaining_length = memory_mode ? (length == 0U ? UINT32_C(65536) : length) : 0U;
      /* Compat repair: as for memory-to-VRAM DMA just above, a programmed DMA length of zero means 0x10000 (the 16-bit
         length counter wraps), which cartridges use to clear all of VRAM with one fill. */
      devices->vdp.dma.fill_byte_count = fill_mode ? (length == 0U ? UINT32_C(65536) : length) : 0U;
      devices->vdp.dma.transfer_access_count = 0U;
      devices->vdp.dma.write_target_code = write_target_code;
      /* SEG-007-T108: a DMA command supersedes any previously selected non-DMA
         CPU DATA-port transfer code, so a later plain CPU DATA-port write
         stays fail-closed unless a fresh non-DMA address-set command reselects
         a target. */
      devices->vdp.data_port_transfer_code = 0U;
      devices->vdp.data_port_transfer_code_valid = 0U;
      devices->vdp.control_port_awaiting_second_word = 0U;
      devices->vdp.control_port_first_word = 0U;
      return 1;
    }
    if (code != 0x00U && code != 0x01U && code != 0x03U && code != 0x04U && code != 0x05U &&
        code != 0x08U)
      return 0; /* undocumented CD5-CD0 combination */
    devices->vdp.addressed_pointer = (addr_high2 << 14) | addr_low14;
    /* SEG-007-T108: persist the selected CD5-CD0 transfer code so the bounded
       CPU DATA-port WRITE path in genesis_vdp_access can consume it. This is
       exactly what the reached CRAM-clear write needs -- no generic
       control-port/data-port state machine (SEG-007-T083 scope). */
    devices->vdp.data_port_transfer_code = code;
    devices->vdp.data_port_transfer_code_valid = 1U;
    devices->vdp.control_port_awaiting_second_word = 0U;
    devices->vdp.control_port_first_word = 0U;
    return 1;
  }
  if ((word & UINT32_C(0xE000)) == UINT32_C(0x8000)) {
    /* One-word register-set command: RS4-RS0 = bits 12-8, data = bits 7-0. */
    uint8_t reg = (uint8_t)((word >> 8) & 0x1FU);
    uint8_t data = (uint8_t)(word & 0xFFU);
    if (reg >= GENESIS_VDP_REGISTER_COUNT) return 0; /* documented valid range is #0-#23 */
    devices->vdp.registers[reg] = data;
    if (reg == 15U) devices->vdp.auto_increment_value = data; /* GTO1 p. 28/p. 37; see comment above. */
    return 1;
  }
  /* First word of a two-word address-set command. On real Mode-5 VDP hardware
     the command address and code are single internal registers written in two
     halves: this FIRST word already updates A13-A0 and CD1-CD0 immediately;
     only A15-A14 and CD5-CD2 wait for the second word (Genesis Plus GX
     `vdp_ctrl_w` / BlastEm `vdp.c` command-word handling; Charles MacDonald,
     "Sega Genesis VDP documentation"; Eke-Eke genvdp; plutiedev "VDP command
     reference" / "The address register"). The write-pending flip-flop
     (control_port_awaiting_second_word) is shared with the DATA port and any
     DATA-port access clears it -- see genesis_vdp_data_port_cpu_write. The
     low A13-A0 bits are merged over the retained A15-A14 bits; the low CD1-CD0
     bits are merged over the retained CD5-CD2 bits only when a transfer code
     is already selected (our model has no meaningful code register otherwise,
     and the DATA-port path stays fail-closed on an unselected code anyway). */
  devices->vdp.control_port_first_word = word;
  devices->vdp.control_port_awaiting_second_word = 1U;
  devices->vdp.addressed_pointer = (devices->vdp.addressed_pointer & UINT32_C(0xC000)) |
                                   (uint32_t)(word & UINT32_C(0x3FFF));
  if (devices->vdp.data_port_transfer_code_valid)
    devices->vdp.data_port_transfer_code =
        (uint8_t)((devices->vdp.data_port_transfer_code & 0x3CU) |
                  (uint8_t)((word >> 14) & 0x03U));
  return 1;
}

/* SEG-007-T175 (second correction): one complete VDP CONTROL-port WORD bus
   transaction plus its immediate post-access memory-to-VDP suspension
   effect. This is the single owner of "a CONTROL-port WORD write, followed
   by a synchronous drain if that exact write just armed a memory-to-VDP
   DMA" -- reused by both the plain WORD CONTROL-port write (via
   genesis_route_access's existing generic VDP dispatch, unchanged below)
   and the LONG CONTROL-port write's own two-sub-transaction decomposition
   (genesis_route_access, further below), so the invariant is expressed once,
   not duplicated per caller.

   A LONG CONTROL-port write decomposes into two sequential WORD bus
   transactions, high half first (SEG-007-T091, GTO1 p. 20). EITHER half can
   independently be the exact second command word that arms a memory-to-VDP
   DMA transfer: if `control_port_awaiting_second_word` was already true
   before the LONG access began, the HIGH half itself completes that pending
   command and can arm the DMA, and generated C would otherwise then execute
   the LOW half's own bus transaction immediately afterward, all still within
   the same routed LONG access -- exactly the same granularity gap this
   task's own synchronous-drain-on-arm mechanism was introduced to close for
   ordinary WORD accesses. Draining after EVERY sub-transaction that arms a
   memory-to-VDP DMA (not merely once after the whole LONG access completes)
   closes this remaining hole: the LOW half's own bus transaction cannot
   begin while a DMA the HIGH half just armed is still BUSY, matching the
   documented 68000 bus-stall at the level of each individual 16-bit bus
   transaction.

   `genesis_vdp_progress_dma` remains the sole transfer-step implementation;
   this helper only sequences the existing, unmodified
   `genesis_vdp_drain_memory_to_vdp_dma_body` drain after the existing,
   unmodified `genesis_vdp_control_port_write_word` word-write body -- no
   new transfer semantics, no timing/cycle scheduling, no opcode/PC
   recognition.

   Return/output contract: returns 1 iff the word was accepted AND (if it
   armed a memory-to-VDP DMA) that DMA has already been drained to
   GENESIS_VDP_DMA_IDLE. Returns 0 otherwise, and distinguishes the two
   failure shapes via `*rejected`: `*rejected = 1` means the word itself was
   an invalid/undocumented CONTROL-port protocol shape (the caller should
   report the existing generic GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS /
   GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP stop, exactly as before this
   task); `*rejected = 0` means the word was valid and its arm (if any)
   already committed, but the subsequent drain itself failed (an unroutable
   DMA source) and populated `*stop_out` with that specific stop -- the
   already-committed arm is left in place, matching the identical
   non-atomic partial-completion policy genesis_route_access's own VDP
   branch already documents for the plain WORD case. */
static int genesis_vdp_control_port_write_word_and_suspend(GenesisRuntime *runtime, uint16_t word,
                                                            int *rejected,
                                                            GenesisRuntimeStop *stop_out) {
  *rejected = 0;
  if (!genesis_vdp_control_port_write_word(&runtime->devices, word)) {
    *rejected = 1;
    return 0;
  }
  if (genesis_vdp_drain_memory_to_vdp_dma_body(runtime, stop_out) != GENESIS_ACCESS_OK) return 0;
  return 1;
}

/* SEG-007-T101 VRAM-fill boundary-wrap compatibility policy.

   Once a fill is armed at an even VRAM pointer with a non-zero count, its
   selected destination progression is PERMITTED to cross the 64 KiB VRAM
   boundary: each advance (the one pre-fill increment and every per-byte
   increment) wraps modulo 64 KiB, and the committed post-fill pointer is the
   wrapped value (see genesis_vdp_data_port_fill_write).

   This is an explicitly labeled, replaceable PROJECT COMPATIBILITY POLICY, not
   verified Genesis hardware behavior. It is based only on the public 16-bit
   VDP-address model (Sega, Genesis Technical Overview v1.00, 1991, pp. 2, 20,
   38-41) and convergent independent mature emulator implementations, neither of
   which is an independent hardware observation of crossing disposition (byte
   ordering, final pointer/DMA state, partial-commit). Stronger reproducible
   hardware evidence may replace this policy only through a separately evidenced
   change; it must never be silently read as a hardware claim.

   Every OTHER neighboring form still fails closed before any mutation: a zero
   count, an out-of-range or odd armed VRAM pointer, and (in the caller) an
   unsupported access width/direction/port, a non-fill DMA mode, or a
   non-armed-fill DATA-port shape. The DATA-port source WORD itself is captured
   at an even P/P+1 pair that never wraps. */
static int genesis_vdp_fill_destinations_are_valid(uint32_t pointer, uint32_t count) {
  /* The bounded VRAM-fill evidence covers only an even VRAM address for the
     DATA-port source WORD. Do not guess the odd-address byte ordering. */
  if (count == 0U || pointer >= GENESIS_VDP_VRAM_BYTES || (pointer & 1U) != 0U)
    return 0;
  return 1;
}

/* Captures the CPU WORD before any mutation. The captured high byte is the
   fill value; the source write is low at P and high at P+1 (P is even and in
   range, so this pair never wraps). The pointer increments once before the
   fill begins, then each filled byte advances it by that same increment; every
   such advance wraps modulo 64 KiB under the boundary-wrap compatibility
   policy above. All armed state is preflighted above, so this helper has no
   failure path after its first store. */
static int genesis_vdp_data_port_fill_write(GenesisDeviceState *devices, uint32_t value) {
  GenesisVdpState *vdp = &devices->vdp;
  GenesisVdpDmaState *dma = &vdp->dma;
  uint32_t pointer = vdp->addressed_pointer;
  uint32_t count = dma->fill_byte_count;
  uint32_t increment = vdp->auto_increment_value;
  uint32_t destination;
  uint32_t index;
  uint8_t high = (uint8_t)(value >> 8);
  uint8_t low = (uint8_t)value;
  if (dma->phase != GENESIS_VDP_DMA_BUSY || dma->kind != GENESIS_VDP_DMA_VRAM_FILL ||
      !genesis_vdp_fill_destinations_are_valid(pointer, count))
    return 0;
  vdp->vram[pointer] = low;
  vdp->vram[pointer + 1U] = high;
  destination = (pointer + increment) & UINT32_C(0xFFFF);
  for (index = 0U; index < count; ++index) {
    vdp->vram[destination] = high;
    destination = (destination + increment) & UINT32_C(0xFFFF);
  }
  vdp->addressed_pointer = destination;
  dma->fill_byte_count = 0U;
  dma->phase = GENESIS_VDP_DMA_IDLE;
  return 1;
}

/* SEG-007-T108 + SEG-007-T191: the runtime-reached plain (non-armed-fill,
   non-DMA) CPU DATA-port ($C00000) WRITE model.
   ================ scope ================
   Plain CPU data-port writes whose transfer target was selected by a
   completed non-DMA two-word address-set command as a documented WRITE
   target -- VRAM WRITE (CD5-CD0 code 0x01, SEG-007-T191), CRAM WRITE (0x03)
   or VSRAM WRITE (0x05) -- in BYTE, WORD or LONG access width. It is NOT a
   generic data-port state machine: DATA-port reads and CRAM/VSRAM/VRAM read
   paths, plus generic control-port ownership, remain out of scope and stay
   fail-closed.

   ================ public sources ================
   - GTO1 (Sega, Genesis Technical Overview v1.00, 1991) p. 20 / p. 27: the
     CD5-CD0 access-mode table -- CRAM WRITE = 000011 (0x03), VSRAM WRITE =
     000101 (0x05). Independently corroborated by plutiedev.com "VDP command
     reference".
   - GTO1 p. 20: "Long word access is equivalent to two word accesses, with
     D31-D16 written first." A LONG data-port write is therefore two sequential
     16-bit writes, high half first, mirroring the existing $C00004 LONG-write
     decomposition and its documented non-atomic partial-completion policy.
   - GTO1 p. 28: "VRAM address is increased by the value of REGISTER # 15"
     after each data-port access; plutiedev.com "VDP command reference":
     "after every word written the address is automatically incremented by the
     autoincrement amount".
   - GTO1 p. 2 / p. 12: CRAM is 64 nine-bit color registers, each accessed as a
     16-bit word -> 128 bytes (GENESIS_VDP_CRAM_BYTES); VSRAM is 40 ten-bit
     words -> 80 bytes (GENESIS_VDP_VSRAM_BYTES). Both corroborated by
     copetti.org "Mega Drive / Genesis Architecture" ("128 B CRAM", "80 B
     VSRAM").
   See docs/references/genesis-vdp-data-port-cpu-write-contract.md.

   ================ project compatibility policy (replaceable, not hardware) ==
   - The current VDP address wraps modulo the selected target's documented byte
     size; GTO1 documents the 16-bit address register but not the CRAM/VSRAM
     wrap disposition.
   - Each 16-bit sub-write is fully validated before it mutates anything, so a
     single sub-write is atomic. The two-sub-write LONG sequence keeps the same
     documented non-atomic partial-completion policy the $C00004 LONG write
     already uses (GTO1 p. 20): if the second sub-write fails, the first
     sub-write's committed target byte and pointer advance are left in place
     and the overall access reports failure.
   - VRAM at an odd current address applies GTO1's documented high/low data
     byte exchange (A0 ignored for address decoding). CRAM/VSRAM at an odd
     current address fail closed rather than guess the odd-address byte
     ordering GTO1 documents only for VRAM.
   - A BYTE data-port write is modeled as a WORD write with the data byte
     mirrored into both halves ((b<<8)|b), matching convergent mature
     emulator behavior; this is a replaceable project compatibility policy,
     not a verified hardware claim. */
/* SEG-007-T191: one 16-bit data-port sub-write into the latched VRAM / CRAM /
   VSRAM target, with the documented post-access auto-increment.

   GTO1 (Sega, Genesis Technical Overview v1.00, 1991) pp. 20 / 27-33:
   - even current address: the halfword is stored big-endian (D15-D8 first).
   - "VRAM address is increased by the value of REGISTER # 15, independent
     data size. VRAM address A0 is used in the calculation of the address
     increment, but is ignored during address decoding."
   - VRAM only, odd current address: "high and low bytes are exchanged if
     A0 = 1" -- the even byte pair (A0 ignored for decoding) is written with
     the two data bytes swapped.

   CRAM / VSRAM at an odd current address stay fail-closed exactly as before
   (SEG-007-T108): GTO1 documents the odd-address byte exchange only for
   VRAM, and the reached clear loops use an even base with auto-increment 2.
   Address wrap remains modulo the selected target's documented byte size
   (replaceable project compatibility policy, see the contract file). */
static int genesis_vdp_data_port_target_write_halfword(GenesisVdpState *vdp, uint8_t *target,
                                                       uint32_t target_size, uint8_t write_code,
                                                       uint16_t halfword) {
  uint32_t dest = vdp->addressed_pointer % target_size;
  uint8_t high = (uint8_t)(halfword >> 8);
  uint8_t low = (uint8_t)halfword;
  if ((dest & 1U) != 0U) {
    if (write_code != 0x01U) return 0; /* CRAM/VSRAM odd address: fail closed (GTO1 undocumented) */
    dest &= ~UINT32_C(1);              /* VRAM: A0 ignored for address decoding (GTO1) */
    target[dest] = low;                /* VRAM: high/low data bytes exchanged when A0 = 1 (GTO1) */
    target[dest + 1U] = high;
  } else {
    target[dest] = high;                            /* big-endian high byte */
    target[(dest + 1U) % target_size] = low;
  }
  vdp->addressed_pointer = (vdp->addressed_pointer + vdp->auto_increment_value) % target_size;
  return 1;
}

/* SEG-007-T191: generalized plain (non-armed-fill, non-DMA) CPU DATA-port
   ($C00000) WRITE model. Extends the bounded SEG-007-T108 path to the full
   data-port write surface the executed Sonic frontier reached:

   - routes to VRAM (CD5-CD0 code 0x01), CRAM (0x03) or VSRAM (0x05) by the
     latched transfer code, reusing genesis_vdp_write_target_buffer;
   - covers BYTE, WORD and LONG access widths (LONG = two sequential 16-bit
     writes, D31-D16 first, GTO1 p. 20);
   - a BYTE write is modeled as a WORD write whose data byte is mirrored into
     both halves ((b<<8)|b) -- a replaceable project compatibility policy
     matching convergent mature-emulator behavior, GTO1's own byte-write
     phrasing ("data is D7~D0, and may be written to $C00000 or $C00001") is
     not precise enough to be a hardware claim.

   Fail-closed (mutating nothing) for: an armed DMA/fill engine, an
   incomplete two-word command latch, no selected transfer code, a selected
   READ code, and (CRAM/VSRAM only) an odd current address. See
   docs/references/genesis-vdp-data-port-cpu-write-contract.md. */
static int genesis_vdp_data_port_cpu_write(GenesisDeviceState *devices,
                                           GenesisAccessWidth width, uint32_t value) {
  GenesisVdpState *vdp = &devices->vdp;
  uint8_t *target;
  uint32_t target_size;
  uint8_t write_code;
  int accepted;
  /* No DMA may be armed: an armed memory-to-VRAM or fill engine owns the
     data port and this plain CPU path must not race it. */
  if (vdp->dma.phase != GENESIS_VDP_DMA_IDLE) return 0;
  /* A 68000 DATA-port access while a two-word CONTROL command is only half
     written BREAKS/cancels that pending sequence on real Mode-5 VDP hardware:
     the write-pending flip-flop is shared between the CONTROL and DATA ports
     and any DATA-port access clears it (Genesis Plus GX `vdp_data_w` /
     `vdp_ctrl_w` `pending`; BlastEm; Charles MacDonald, "Sega Genesis VDP
     documentation"; plutiedev "VDP command reference"). The first control
     word's low half (A13-A0, CD1-CD0) was already applied when it was written
     (see genesis_vdp_control_port_write_word); the abandoned second half is
     dropped and the next CONTROL-port word is taken as a fresh first half.
     This flip-flop clear is NOT conditional on the data write then succeeding
     -- hardware clears it on any data-port access -- so it happens even when
     the write is rejected just below for lack of a selected transfer code.
     The DATA write itself then uses the current address/code state: retained
     A15-A14 / CD5-CD2 from the last completed command plus the A13-A0 /
     CD1-CD0 the cancelled command's first word already wrote. */
  if (vdp->control_port_awaiting_second_word) {
    vdp->control_port_awaiting_second_word = 0U;
    vdp->control_port_first_word = 0U;
  }
  if (!vdp->data_port_transfer_code_valid) return 0;
  write_code = vdp->data_port_transfer_code;
  /* Resolves 0x01 (VRAM) / 0x03 (CRAM) / 0x05 (VSRAM); every READ code
     (0x00/0x04/0x08) is not a write target and fails closed here. */
  target = genesis_vdp_write_target_buffer(vdp, write_code, &target_size);
  if (target == NULL) return 0;
  if (width == GENESIS_ACCESS_LONG) {
    /* GTO1 p. 20: two sequential 16-bit writes, D31-D16 first. Same
       documented non-atomic partial-completion policy as the $C00004 LONG
       write: if the second sub-write fails, the first stays committed. */
    if (!genesis_vdp_data_port_target_write_halfword(vdp, target, target_size, write_code,
                                                     (uint16_t)((value >> 16) & 0xFFFFU)))
      return 0;
    accepted = genesis_vdp_data_port_target_write_halfword(vdp, target, target_size, write_code,
                                                           (uint16_t)(value & 0xFFFFU));
  } else if (width == GENESIS_ACCESS_WORD) {
    accepted = genesis_vdp_data_port_target_write_halfword(vdp, target, target_size, write_code,
                                                           (uint16_t)(value & 0xFFFFU));
  } else if (width == GENESIS_ACCESS_BYTE) {
    uint8_t data_byte = (uint8_t)value;
    accepted = genesis_vdp_data_port_target_write_halfword(
        vdp, target, target_size, write_code,
        (uint16_t)(((uint16_t)data_byte << 8) | (uint16_t)data_byte));
  } else {
    return 0;
  }
  return accepted;
}

/* SEG-021-T041: the runtime-reached plain (non-armed-DMA) CPU DATA-port
   ($C00000) READ model, the read-direction sibling of
   genesis_vdp_data_port_cpu_write above.
   ================ scope ================
   Plain CPU data-port reads whose transfer target was selected by a
   completed non-DMA two-word address-set command as a documented READ
   target -- VRAM READ (CD5-CD0 code 0x00), VSRAM READ (0x04) or CRAM READ
   (0x08). WORD and LONG widths only (see the BYTE note below). It is NOT a
   generic data-port state machine: every WRITE code, an unselected code, and
   an armed DMA/fill engine remain fail-closed here exactly as the write path
   already documents for its own out-of-scope shapes.

   ================ public sources ================
   - GTO1 (Sega, Genesis Technical Overview v1.00, 1991) p. 20 / p. 27: the
     CD5-CD0 access-mode table -- VRAM READ = 000000 (0x00), VSRAM READ =
     000100 (0x04), CRAM READ = 001000 (0x08). Independently corroborated by
     plutiedev.com "VDP command reference".
   - GTO1 p. 20: "Long word access is equivalent to two word accesses, with
     D31-D16 written first." This project reads the identical documented
     sequencing for a LONG data-port access in either direction: two
     sequential 16-bit transactions, high half first -- trivial reuse of the
     same per-halfword helper the write path already established.
   - GTO1 p. 28: the current VDP address is advanced by register #15's
     auto-increment value after each data-port access, in either direction;
     plutiedev.com "VDP command reference" corroborates.
   - GTO1 p. 28: "VRAM address A0 is used in the calculation of the address
     increment, but is ignored during address decoding" and "high and low
     bytes are exchanged if A0 = 1". This project reads that exchange as
     direction-symmetric: it renames which stored byte of the even base pair
     supplies the bus's high/low half, not merely a write-only quirk. The
     write path (genesis_vdp_data_port_target_write_halfword) already
     implements one direction of this exchange (bus halfword H at odd
     address A stores low(H) at base=A&~1 and high(H) at base+1); reading
     the SAME odd address back with the same exchange inverted --
     high=stored[base+1], low=stored[base] -- reproduces H exactly, which is
     the only self-consistent round-trip reading of GTO1's own exchange
     rule. CRAM/VSRAM at an odd current address stay fail-closed exactly
     like the write path: GTO1 documents the exchange only for VRAM.

   ================ project compatibility policy (replaceable, not hardware) ==
   - BYTE data-port reads are NOT modeled. The write path's own BYTE support
     is itself a replaceable, non-hardware-cited compatibility policy
     (mirroring the write byte into both halves) that GTO1 does not precisely
     describe for BYTE writes either; for reads GTO1 gives no comparable
     byte-lane selection rule at all (which physical half of the fetched
     word a BYTE read of $C00000 vs $C00001 would return is undocumented
     here), and the runtime-selected frontier this task resolves is a WORD
     read. Guessing a BYTE read-lane split would be exactly the kind of
     unevidenced hardware claim this project avoids; BYTE reads of the
     DATA port remain fail-closed.
   - The current VDP address wraps modulo the selected target's documented
     byte size, identical to the write path's own policy.
   - Each 16-bit sub-read is fully validated before it advances the pointer
     or (for LONG) proceeds to the second sub-read, so a single sub-read is
     atomic; the two-sub-read LONG sequence keeps the identical documented
     non-atomic partial-completion policy the write path already uses (GTO1
     p. 20): if the second sub-read fails, the first sub-read's already-
     advanced pointer is left in place and the overall access reports
     failure.
   See docs/references/genesis-vdp-data-port-cpu-write-contract.md (extended
   by this task to cover the read direction; the read/write protocol shares
   one control-port command latch and one addressed pointer, so the two
   directions are documented together rather than duplicated). */
static const uint8_t *genesis_vdp_read_target_buffer(const GenesisVdpState *vdp, uint8_t read_code,
                                                      uint32_t *out_size) {
  if (read_code == 0x00U) { *out_size = GENESIS_VDP_VRAM_BYTES; return vdp->vram; }
  if (read_code == 0x04U) { *out_size = GENESIS_VDP_VSRAM_BYTES; return vdp->vsram; }
  if (read_code == 0x08U) { *out_size = GENESIS_VDP_CRAM_BYTES; return vdp->cram; }
  return NULL;
}

/* SEG-021-T041: one 16-bit data-port sub-read from the latched VRAM / CRAM /
   VSRAM target, with the documented post-access auto-increment -- the read
   direction sibling of genesis_vdp_data_port_target_write_halfword. See the
   citations in the doc comment above genesis_vdp_data_port_cpu_read. */
static int genesis_vdp_data_port_target_read_halfword(GenesisVdpState *vdp, const uint8_t *target,
                                                       uint32_t target_size, uint8_t read_code,
                                                       uint16_t *halfword_out) {
  uint32_t dest = vdp->addressed_pointer % target_size;
  if ((dest & 1U) != 0U) {
    uint32_t base;
    if (read_code != 0x00U) return 0; /* CRAM/VSRAM odd address: fail closed (GTO1 undocumented) */
    base = dest & ~UINT32_C(1);
    *halfword_out = (uint16_t)(((uint16_t)target[base + 1U] << 8) | (uint16_t)target[base]);
  } else {
    *halfword_out = (uint16_t)(((uint16_t)target[dest] << 8) |
                               (uint16_t)target[(dest + 1U) % target_size]);
  }
  vdp->addressed_pointer = (vdp->addressed_pointer + vdp->auto_increment_value) % target_size;
  return 1;
}

/* SEG-021-T041: generalized plain (non-armed-DMA) CPU DATA-port ($C00000)
   READ model. See the doc comment above for scope/citations/policy. Fail-
   closed (mutating nothing) for: an armed DMA/fill engine, no selected
   transfer code, a selected WRITE code, an unsupported width, and (CRAM/
   VSRAM only) an odd current address.

   Ordering mirrors genesis_vdp_data_port_cpu_write exactly: the armed-DMA
   guard runs first (a DATA-port access must never race an armed transfer
   engine, and if it did, nothing here may mutate anything -- see the
   analysis below), THEN the shared two-word CONTROL-port write-pending
   flip-flop is unconditionally cleared (real Mode-5 VDP hardware clears it
   on any DATA-port access, read or write, independent of whether the read
   itself then succeeds), THEN the read target/width is validated.

   Could a CPU DATA-port READ ever actually observe `dma.phase !=
   GENESIS_VDP_DMA_IDLE`? No: SEG-007-T175's synchronous drain
   (genesis_vdp_drain_memory_to_vdp_dma_body, invoked from
   genesis_route_access immediately after the exact CONTROL-port write that
   arms a memory-to-VRAM DMA) and the VRAM-fill engine's own single-CPU-
   write completion (genesis_vdp_data_port_fill_write, which clears BUSY
   before returning) both leave `dma.phase` IDLE before control ever returns
   to generated C, so no reachable generated dispatch step can execute a
   DATA-port READ while a DMA is BUSY under this project's synchronous-DMA
   policy. This guard is kept anyway, unchanged in shape from the write
   path's own identical guard, purely as a defensive invariant against a
   future arm site added without also wiring the synchronous drain -- never
   as the sole suspension mechanism. */
static int genesis_vdp_data_port_cpu_read(GenesisDeviceState *devices, GenesisAccessWidth width,
                                          uint32_t *value_out) {
  GenesisVdpState *vdp = &devices->vdp;
  const uint8_t *target;
  uint32_t target_size;
  uint8_t read_code;
  uint16_t high_half;
  uint16_t low_half;
  if (vdp->dma.phase != GENESIS_VDP_DMA_IDLE) return 0;
  if (vdp->control_port_awaiting_second_word) {
    vdp->control_port_awaiting_second_word = 0U;
    vdp->control_port_first_word = 0U;
  }
  if (!vdp->data_port_transfer_code_valid) return 0;
  read_code = vdp->data_port_transfer_code;
  /* Resolves 0x00 (VRAM) / 0x04 (VSRAM) / 0x08 (CRAM); every WRITE code
     (0x01/0x03/0x05) is not a read target and fails closed here. */
  target = genesis_vdp_read_target_buffer(vdp, read_code, &target_size);
  if (target == NULL) return 0;
  if (width == GENESIS_ACCESS_WORD) {
    if (!genesis_vdp_data_port_target_read_halfword(vdp, target, target_size, read_code, &low_half))
      return 0;
    *value_out = low_half;
    return 1;
  }
  if (width == GENESIS_ACCESS_LONG) {
    /* GTO1 p. 20: two sequential 16-bit transactions, D31-D16 first. Same
       documented non-atomic partial-completion policy as the write path: if
       the second sub-read fails, the first sub-read's pointer advance is
       left in place. */
    if (!genesis_vdp_data_port_target_read_halfword(vdp, target, target_size, read_code, &high_half))
      return 0;
    if (!genesis_vdp_data_port_target_read_halfword(vdp, target, target_size, read_code, &low_half))
      return 0;
    *value_out = ((uint32_t)high_half << 16) | (uint32_t)low_half;
    return 1;
  }
  return 0; /* BYTE data-port reads are out of scope; see the doc comment. */
}

/* The one recognized VDP status-read access shape: the WORD lane and both
   BYTE lanes of the CONTROL port's status alias. Every caller that needs to
   decide "is this a status-read transaction" (DMA-progress suspension,
   VBlank-pending observation, and the dispatch below) shares this single
   predicate so the three call sites cannot silently diverge on which shapes
   count as a status read. */
static int genesis_is_vdp_status_read_shape(uint32_t address, GenesisAccessWidth width,
                                            GenesisAccessDirection direction) {
  if (direction != GENESIS_ACCESS_READ) return 0;
  if (width == GENESIS_ACCESS_WORD) return address == UINT32_C(0x00C00004);
  if (width == GENESIS_ACCESS_BYTE)
    return address == UINT32_C(0x00C00004) || address == UINT32_C(0x00C00005);
  return 0;
}

/* SEG-021-T040: status bit 3 (VBlank flag, GTO1 p.19) is the one status bit
   this owner already treats as interrupt-relevant (see the read-time
   observation below). It must be a truthful live projection of the same
   NTSC scheduler/video-timing state that already drives IRQ6 admission
   (`genesis_irq6_scheduler_and_admit`'s own `crosses_onset` test), not the
   permanently-zero static `status_register` field: `master_ticks` is
   monotonic guest time, and one frame's VBlank window is exactly
   [onset, frame) modulo the frame length -- the same window the scheduler
   uses to decide whether to raise `vblank_pending`. This is a pure read-time
   projection: it never mutates the scheduler, never advances time, and
   introduces no second clock. */
static int genesis_vdp_live_vblank_status_bit(uint64_t master_ticks) {
  return (master_ticks % GENESIS_NTSC_MASTER_TICKS_PER_FRAME) >= GENESIS_NTSC_VBLANK_ONSET_TICK;
}

/* One status transaction, shared by the WORD and both BYTE lanes. The VDP
   clears the control-command write-pending flip-flop on a status read; the
   returned snapshot precedes that side effect. Upper unused bus bits are not
   reconstructed: this owner's existing status_register policy supplies them,
   except for bit 3, which SEG-021-T040 overrides with the live projection
   above so every lane samples one identical, truthful status word. */
static uint16_t genesis_vdp_status_read(GenesisDeviceState *devices, uint64_t master_ticks) {
  uint16_t status = (uint16_t)(devices->vdp.status_register & (uint16_t)~UINT16_C(0x0008));
  if (genesis_vdp_live_vblank_status_bit(master_ticks)) status |= UINT16_C(0x0008);
  devices->vdp.control_port_awaiting_second_word = 0U;
  devices->vdp.control_port_first_word = 0U;
  return status;
}

static int genesis_vdp_access(GenesisDeviceState *devices, uint32_t address,
                              GenesisAccessWidth width, GenesisAccessDirection direction,
                              uint32_t *value, uint16_t *status_sample_out,
                              uint64_t master_ticks) {
  if (direction == GENESIS_ACCESS_READ) {
    if (genesis_is_vdp_status_read_shape(address, width, direction)) {
      const uint16_t status = genesis_vdp_status_read(devices, master_ticks);
      *status_sample_out = status;
      *value = width == GENESIS_ACCESS_WORD ? status :
               (address & 1U) ? (status & UINT16_C(0xFF)) : (status >> 8);
      return 1;
    }
    /* SEG-021-T041: the plain CPU DATA-port ($C00000) READ, WORD/LONG only
       -- see genesis_vdp_data_port_cpu_read's own doc comment for scope,
       citations and the BYTE-exclusion rationale. */
    if (address == UINT32_C(0x00C00000) &&
        (width == GENESIS_ACCESS_WORD || width == GENESIS_ACCESS_LONG)) {
      uint32_t read_value;
      if (!genesis_vdp_data_port_cpu_read(devices, width, &read_value)) return 0;
      *value = read_value;
      return 1;
    }
    return 0;
  }
  /* direction == GENESIS_ACCESS_WRITE (SEG-007-T091) */
  if (address == UINT32_C(0x00C00000) ||
      (address == UINT32_C(0x00C00001) && width == GENESIS_ACCESS_BYTE &&
       devices->vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
       devices->vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL)) {
    /* DATA-port write. An armed VRAM fill owns one WORD-equivalent source
       transaction, including an 8-bit write on either CPU DATA-port lane;
       otherwise use the plain CPU DATA-port write model (SEG-007-T108 +
       SEG-007-T191: VRAM/CRAM/VSRAM targets, BYTE/WORD/LONG widths). */
    if (devices->vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
        devices->vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL) {
      if (width == GENESIS_ACCESS_BYTE) {
        const uint32_t data_byte = *value & UINT32_C(0xFF);
        return genesis_vdp_data_port_fill_write(devices, (data_byte << 8) | data_byte);
      }
      if (width != GENESIS_ACCESS_WORD) return 0;
      return genesis_vdp_data_port_fill_write(devices, *value);
    }
    return genesis_vdp_data_port_cpu_write(devices, width, *value);
  }
  if (width == GENESIS_ACCESS_WORD && address == UINT32_C(0x00C00004)) {
    return genesis_vdp_control_port_write_word(devices, (uint16_t)(*value & 0xFFFFU));
  }
  /* SEG-007-T091 (second frontier pass) originally decomposed a LONG
     CONTROL-port write into two sequential WORD writes right here. SEG-007-
     T175 (second correction) moved that decomposition up into
     genesis_route_access's own VDP branch instead, because either half can
     independently arm a memory-to-VDP DMA and the resulting synchronous
     drain needs the full GenesisRuntime (for genesis_vdp_progress_dma's own
     routed source read) that this GenesisDeviceState-only function does not
     have. genesis_route_access now intercepts this exact (LONG, WRITE,
     $C00004) shape before ever reaching this function -- see the block
     comment there for the corrected two-sub-transaction sequencing and its
     preserved high-first order / partial-completion policy. This function
     is therefore never actually called with that shape; the fall-through
     below is unreachable in practice but kept fail-closed for defense in
     depth, matching every other unrecognized shape's own policy. */
  return 0;
}

/* SEG-032-T006 (ADR 0072, contract section 9) -- the 68000's PSG (SN76489) port: BYTE writes at the odd addresses $C00011,
   $C00013, $C00015, $C00017 (GTO1 v1.00 p. 10 "VDP AREA: PSG 76489"; MacDonald, Genesis Plus GX and ares agree on the four odd
   mirrors; byte writes to the even addresses have no effect and stay unmapped here). The chip is write-only: a read, and any
   WORD/LONG access, fails closed with GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG.

   Replaces the SEG-007-T109 command-latch compatibility model. The device is the shared `Sn76489` of libs/device/sega/psg,
   reached through `runtime->audio_hooks` from BOTH CPUs (the Z80 through its $7F11 window, z80_machine.c); the byte is
   delivered at the guest time of the 68000 access after the Z80 was run to that time, so the two writers interleave in master-time
   order. The only evidence-bearing PSG state is the log of the 68000's port traffic: a DATA byte before any LATCH byte is the
   device's business (it is accepted and changes nothing, as on the Master System), not a fail-closed case here. Without an
   attached sound device the byte is accepted and discarded. */
static int genesis_is_psg_region(uint32_t address) {
  return segarecomp_genesis_psg_port_contains(address);
}

int genesis_psg_port_write(GenesisRuntime *runtime, uint8_t value, uint64_t master_ticks) {
  const GenesisAudioHooks *hooks = runtime->audio_hooks;
  if (hooks != 0 && hooks->psg_write != 0) return hooks->psg_write(hooks->context, runtime, value, master_ticks);
  return 1; /* no sound device attached: accepted and discarded */
}

static int genesis_psg_access_68k(GenesisRuntime *runtime, GenesisAccessWidth width, GenesisAccessDirection direction,
                                  uint32_t *value) {
  GenesisPsgState *log = &runtime->devices.psg;
  uint8_t byte;
  if (direction != GENESIS_ACCESS_WRITE) return 0; /* the PSG port is write-only */
  if (width != GENESIS_ACCESS_BYTE) return 0;
  byte = (uint8_t)(*value & 0xFFU);
  if (!genesis_psg_port_write(runtime, byte, runtime->scheduler.master_ticks)) return 0;
  log->write_digest = (log->write_count == 0U ? UINT32_C(2166136261) : log->write_digest);
  log->write_digest = (log->write_digest ^ byte) * UINT32_C(16777619);
  ++log->write_count;
  return 1;
}

/* SEG-007-T171: fail-closed-lane recognition of the YM2612 FM-synthesis
   register window ($A04000-$A04003), mirroring genesis_is_psg_region's
   single-source-of-truth delegation shape. */
static int genesis_is_ym2612_region(uint32_t address) {
  return segarecomp_genesis_ym2612_region_contains(address);
}

/* SEG-032-T007 (ADR 0072/0074, contract section 9) -- the 68000's YM2612 ports, $A04000-$A04003: BYTE read or write at each
   (a read returns the status byte whatever the port, Genesis Plus GX and ares). Replaces the SEG-007-T171 status-port-only
   compatibility model: the device is the vendored-ymfm YM2612 attached through `audio_hooks` and shared with the Z80 (Z80 view
   `$4000-$5FFF`, `address & 3`). The access reaches the device at the guest time of the 68000 access after the Z80 was run to that
   time. WORD/LONG fail closed with GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612. The grant is not required here: the canonical
   startup route is proved to reach the chip, and the references disagree about whether the decode needs the Z80 bus (open fact). */
static int genesis_ym2612_access_68k(GenesisRuntime *runtime, uint32_t address, GenesisAccessWidth width,
                                     GenesisAccessDirection direction, uint32_t *value) {
  const uint32_t port = address & 3U;
  if (width != GENESIS_ACCESS_BYTE) return 0;
  if (direction == GENESIS_ACCESS_READ) {
    uint8_t status = 0U;
    if (!genesis_ym2612_port_read(runtime, port, runtime->scheduler.master_ticks, &status)) return 0;
    *value = status;
    return 1;
  }
  return genesis_ym2612_port_write(runtime, port, (uint8_t)(*value & 0xFFU), runtime->scheduler.master_ticks);
}

/* SEG-007-T102: fail-closed-lane recognition of exactly the two 68k-side Z80
   bus-arbitration control registers -- BUSREQ ($A11100) and RESET ($A11200) --
   as a single tight interval, mirroring genesis_is_vdp_region's
   single-interval recognition shape. GTO1 v1.00 (1991) p. 76 SS4 "Z80
   CONTROL" documents both register addresses; the interval is sized to cover
   both and nothing else. Every in-region address that is not one of the two
   documented registers, and every unsupported shape at those two registers,
   fails closed with GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS. */
static int genesis_is_z80_bus_region(uint32_t address) {
  /* SEG-007-T115: delegate to the shared contract predicate so the
     translation-time routing gate and this runtime recognise a
     byte-identical interval. */
  return segarecomp_genesis_z80_arbitration_region_contains(address);
}

/* SEG-032-T005 (ADR 0072, contract section 4) -- 68k-side Z80 bus-arbitration control registers, BUSREQ ($A11100) and RESET
   ($A11200). Replaces the SEG-007-T102 compatibility policy (immediate grant, reset-only latch): the Z80 is a real secondary CPU.

   Frozen against the Genesis Technical Overview v1.00 p. 76/91 (register addresses, polarity, the documented acquire sequence),
   Genesis Plus GX (`gen_zbusreq_w`, `gen_zreset_w`, mem68k.c BUSACK read) and ares (`APU::setBUSREQ/setRES`, `busgrantedCPU`):
     - Power-on: /RESET is asserted (`reset_released == 0`), BUSREQ clear: the Z80 is held in reset.
     - BUSACK ($A11100 D8 word / D0 byte, 0 = granted) is granted iff BUSREQ is asserted AND /RESET is released; BUSREQ with /RESET
       low is recorded but never acknowledged.
     - Before any edge is applied the attached Z80 machine is run to the access instant (the `run_to` hook), so a BUSREQ is acknowledged
       once the Z80 reached the request time, and the 68K never observes a stale Z80.
     - Edges (not level writes) reach the machine: BUSREQ assert/release, /RESET assert/release, together with the runnable
       transition and image-epoch classification of the epoch tracker below. The machine resets the Z80 and the YM2612 on /RESET
       release, resumes it on BUSREQ release, and activates an image at an epoch.
   Documented bit lane for BYTE access: a BYTE access to the even register address drives D15-D8, so the control/status bit is D0.
   Fails closed (returns 0, mutating nothing): a read of $A11200, any LONG access to either register, every other address inside
   genesis_is_z80_bus_region. Every validation check precedes every mutation. */
static int genesis_z80_bus_shape_ok(uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction) {
  if (width != GENESIS_ACCESS_WORD && width != GENESIS_ACCESS_BYTE) return 0; /* LONG (and any invalid width) fails closed */
  if (address != UINT32_C(0x00A11100) && address != UINT32_C(0x00A11200)) return 0; /* in-region but not a register */
  if (address == UINT32_C(0x00A11200) && direction != GENESIS_ACCESS_WRITE) return 0; /* RESET is write-only */
  return 1;
}

/* Runs the attached Z80 machine up to the current guest time. 0 = ok (or no machine), 1 = typed stop in *stop. */
static int genesis_z80_sync(GenesisRuntime *runtime, GenesisRuntimeStop *stop) {
  const GenesisZ80Hooks *hooks = runtime->z80_hooks;
  runtime->z80_synced_ticks = runtime->scheduler.master_ticks;
  if (hooks == 0 || hooks->run_to == 0) return 0;
  return hooks->run_to(hooks->context, runtime, runtime->scheduler.master_ticks, stop);
}

/* Applies one BUSREQ/RESET write: latches, epoch tracker, machine edge hook. The caller has already synchronized the Z80. */
static int genesis_z80_bus_write(GenesisRuntime *runtime, uint32_t address, int set, GenesisRuntimeStop *stop) {
  GenesisZ80BusState *bus = &runtime->devices.z80_bus;
  GenesisZ80EpochObserver *tracker = &runtime->z80_epoch;
  const uint8_t old_requested = bus->bus_requested;
  const uint8_t old_released = bus->reset_released;
  const int was_runnable = old_released && !old_requested;
  GenesisZ80Event event;
  int runnable, transition, epoch;
  if (address == UINT32_C(0x00A11100)) {
    if ((uint8_t)set == old_requested) return 0; /* no edge */
    bus->bus_requested = (uint8_t)set;
    event = set ? GENESIS_Z80_EVENT_BUSREQ_ASSERT : GENESIS_Z80_EVENT_BUSREQ_RELEASE;
  } else {
    if ((uint8_t)set == old_released) return 0; /* no edge */
    bus->reset_released = (uint8_t)set;
    if (set) tracker->executed = 0U; /* /RESET release: the Z80 is reset and pristine again (contract section 4.6) */
    event = set ? GENESIS_Z80_EVENT_RESET_RELEASE : GENESIS_Z80_EVENT_RESET_ASSERT;
  }
  bus->bus_granted = (uint8_t)(bus->bus_requested && bus->reset_released);
  runnable = bus->reset_released && !bus->bus_requested;
  transition = runnable && !was_runnable;
  epoch = transition && !tracker->executed;
  if (epoch) {
    ++tracker->epoch_count;
    if (tracker->on_epoch != 0) {
      GenesisZ80EpochEvent probe;
      probe.ordinal = tracker->epoch_count;
      probe.master_ticks = runtime->scheduler.master_ticks;
      probe.ram = bus->z80_ram;
      probe.written_bitmap = tracker->written;
      tracker->on_epoch(tracker->context, &probe);
    }
  }
  if (runtime->z80_hooks != 0 && runtime->z80_hooks->bus_event != 0 &&
      runtime->z80_hooks->bus_event(runtime->z80_hooks->context, runtime, event, runtime->scheduler.master_ticks, transition, epoch,
                                    tracker->written, stop))
    return 1;
  if (epoch) tracker->executed = 1U;
  if (transition) memset(tracker->written, 0, sizeof(tracker->written)); /* the hold window ends at every runnable transition */
  return 0;
}

/* SEG-032-T004 (ADR 0072, contract section 3) -- the 68000 view of the Z80 area. Replaces the SEG-007-T103 flat byte window.
   Reachable only while the 68000 holds the Z80 bus (GTO1 v1.00 p. 76 SS4; MacDonald SS2.2); otherwise the typed stop
   GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS. Frozen against Genesis Plus GX `z80_read_byte`/`z80_write_byte` and ares:
     - $A00000-$A03FFF: the 8 KiB sound RAM and its mirror (`address & $1FFF`). A BYTE access reads/writes one byte; a WORD write
       stores its HIGH byte at the (even) address, a WORD read returns the byte in both halves (MacDonald SS1.2 word-write quirk).
     - $A06000-$A060FF: the write-only bank register; a BYTE write shifts `data & 1`, a WORD write the D8 of the word, into bit 8
       of the 9-bit register (the previous value shifts right). A read is an unmapped access.
   LONG (and any invalid width) fails closed with the pre-existing GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM. Every
   validation check precedes every mutation, so a rejected access is atomic. Returns 0 on success, else the diagnostic. */
static int genesis_is_z80_ram_window_region(uint32_t address) {
  return segarecomp_genesis_z80_ram_window_contains(address) || segarecomp_genesis_z80_bank_register_contains(address);
}

static GenesisDiagnosticCategory genesis_z80_area_access(GenesisDeviceState *devices, uint32_t address,
                                                         GenesisAccessWidth width, GenesisAccessDirection direction,
                                                         uint32_t *value) {
  if (!devices->z80_bus.bus_granted) return GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS;
  if (width != GENESIS_ACCESS_BYTE && width != GENESIS_ACCESS_WORD) return GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM;
  if (segarecomp_genesis_z80_bank_register_contains(address)) {
    uint32_t bit;
    if (direction != GENESIS_ACCESS_WRITE) return GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS;
    bit = (width == GENESIS_ACCESS_WORD) ? ((*value >> 8) & 1U) : (*value & 1U);
    devices->z80_bus.bank = (uint16_t)(((bit << 8) | (devices->z80_bus.bank >> 1)) & 0x1FFU);
    return (GenesisDiagnosticCategory)0;
  }
  {
    const uint32_t offset = (address - UINT32_C(0x00A00000)) & (GENESIS_Z80_RAM_BYTES - 1U);
    if (direction == GENESIS_ACCESS_READ) {
      const uint32_t byte = devices->z80_bus.z80_ram[offset];
      *value = (width == GENESIS_ACCESS_WORD) ? ((byte << 8) | byte) : byte;
      return (GenesisDiagnosticCategory)0;
    }
    if (direction == GENESIS_ACCESS_WRITE) {
      devices->z80_bus.z80_ram[offset] = (uint8_t)((width == GENESIS_ACCESS_WORD) ? ((*value >> 8) & 0xFFU) : (*value & 0xFFU));
      return (GenesisDiagnosticCategory)0;
    }
  }
  return GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM;
}

/* SEG-032-T004/T007 (ADR 0072): the YM2612 port seam both CPUs reach (declared in runtime.h): forwards to the attached device. */
int genesis_ym2612_port_read(GenesisRuntime *runtime, uint32_t port, uint64_t master_ticks, uint8_t *value) {
  const GenesisAudioHooks *hooks = runtime->audio_hooks;
  if (hooks != 0 && hooks->ym_read != 0) return hooks->ym_read(hooks->context, runtime, port & 3U, master_ticks, value);
  *value = 0U; /* no sound device attached: absent hardware reads as zero */
  return 1;
}

int genesis_ym2612_port_write(GenesisRuntime *runtime, uint32_t port, uint8_t value, uint64_t master_ticks) {
  const GenesisAudioHooks *hooks = runtime->audio_hooks;
  if (hooks != 0 && hooks->ym_write != 0) return hooks->ym_write(hooks->context, runtime, port & 3U, value, master_ticks);
  return 1; /* no sound device attached: accepted and discarded */
}

void genesis_ym2612_port_reset(GenesisRuntime *runtime, uint64_t master_ticks) {
  const GenesisAudioHooks *hooks = runtime->audio_hooks;
  if (hooks != 0 && hooks->ym_reset != 0) hooks->ym_reset(hooks->context, runtime, master_ticks);
}

/* SEG-032-T002/T005 (ADR 0073): the 68K wrote one Z80 RAM byte (mirror resolved): it belongs to the hold window. */
static void genesis_z80_epoch_note_ram_write(GenesisRuntime *runtime, uint32_t address) {
  const uint32_t offset = (address - UINT32_C(0x00A00000)) & (GENESIS_Z80_RAM_BYTES - 1U);
  runtime->z80_epoch.written[offset >> 3] |= (uint8_t)(1U << (offset & 7U));
}

static GenesisAccessResultKind genesis_route_access_unrecorded(GenesisRuntime *runtime, uint32_t address,
                                              GenesisAccessWidth width,
                                              GenesisAccessDirection direction, uint32_t *value,
                                              GenesisRuntimeStop *stop_out) {
  GenesisRuntimeStop stop;
  uint32_t byte_count;
  uint32_t offset;
  uint32_t result = 0U;
  uint32_t index;
  uint32_t region_index;
  if (runtime == 0 || value == 0 || stop_out == 0 || !genesis_width_is_valid(width) ||
      (direction != GENESIS_ACCESS_READ && direction != GENESIS_ACCESS_WRITE)) {
    if (stop_out != 0) *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                                        GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    return GENESIS_ACCESS_FAIL;
  }
  byte_count = (uint32_t)width;
  if ((address & UINT32_C(0xFF000000)) != 0U) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT);
    return GENESIS_ACCESS_FAIL;
  }
  if ((byte_count > 1U && (address & 1U) != 0U)) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_work_ram(address, byte_count)) {
    offset = address - SEGARECOMP_GENESIS_WORK_RAM_BEGIN;
    if (direction == GENESIS_ACCESS_READ) {
      for (index = 0U; index < byte_count; ++index) result = (result << 8U) | runtime->work_ram[offset + index];
      *value = result;
    } else {
      /* All checks precede the first store: a failed access is atomic. */
      for (index = 0U; index < byte_count; ++index)
        runtime->work_ram[offset + (byte_count - 1U - index)] = (uint8_t)(*value >> (index * 8U));
    }
    return GENESIS_ACCESS_OK;
  }
  if (address < UINT32_C(0x00400000)) {
    /* ADR 0098: the header-declared cartridge SRAM extent overlays the ROM; the SRAM owner answers every access that touches
       it, in both directions, before the immutable-ROM paths below. Everything outside the extent is unchanged. */
    if (runtime->cartridge_sram != 0 && address >= SEGARECOMP_GENESIS_CARTRIDGE_SRAM_WINDOW_BEGIN) {
      const GenesisCartridgeSramConfig *sram = runtime->cartridge_sram;
      const int sram_class = segarecomp_genesis_cartridge_sram_classify(sram->start, sram->end, sram->lane_odd, address,
                                                                        byte_count);
      /* While the SRAM is hidden ($A130F1 bit 0 clear over a ROM that extends into the extent) the extent is plain cartridge
         ROM, handled by the immutable-ROM paths below with every width; a write there is the ordinary ROM-write stop. */
      const int visible = sram->always_mapped != 0U ||
                          (runtime->cartridge_sram_control & SEGARECOMP_GENESIS_CARTRIDGE_SRAM_CONTROL_MAP) != 0U;
      if (sram_class != SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE && (visible || !sram->layout_supported)) {
        if (!sram->layout_supported) {
          *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                          GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_LAYOUT);
          return GENESIS_ACCESS_FAIL;
        }
        if (sram_class == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_BYTE && runtime->cartridge_sram_storage != 0 &&
            ((address - sram->start) >> 1U) < sram->storage_bytes) {
          const uint32_t sram_index = (address - sram->start) >> 1U;
          if (direction == GENESIS_ACCESS_READ)
            *value = runtime->cartridge_sram_storage[sram_index];
          else if ((runtime->cartridge_sram_control & SEGARECOMP_GENESIS_CARTRIDGE_SRAM_CONTROL_PROTECT) == 0U)
            runtime->cartridge_sram_storage[sram_index] = (uint8_t)(*value & UINT32_C(0xFF));
          return GENESIS_ACCESS_OK; /* a write-protected store is ignored, like the hardware */
        }
        *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                        GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_ACCESS);
        return GENESIS_ACCESS_FAIL;
      }
    }
    /* SEG-007-T077: a runtime-computed (non-constant-foldable) read whose
       address falls inside a statically proven, generated, build-time-
       embedded, read-only cartridge-data region resolves against that
       region's own compiled backing data with a runtime bounds check,
       instead of failing closed -- see
       docs/decisions/0006-generic-cartridge-data-region-ownership.md. This
       never reads the original ROM file (only a compiled array the
       generated program itself embeds), never fetches/decodes an
       instruction, and applies to reads only: every write and every
       address outside every proven region remains exactly as fail-closed
       as before this widening. */
    if (direction == GENESIS_ACCESS_READ) {
      for (region_index = 0U; region_index < runtime->owned_region_count; ++region_index) {
        const GenesisOwnedCartridgeRegion *region = &runtime->owned_regions[region_index];
        if (region->end <= region->begin || region->length != region->end - region->begin) continue;
        if (address >= region->begin && address < region->end && byte_count <= region->end - address) {
          result = 0U;
          for (index = 0U; index < byte_count; ++index)
            result = (result << 8U) | region->data[(address - region->begin) + index];
          *value = result;
          return GENESIS_ACCESS_OK;
        }
      }
    }
    *stop_out = genesis_access_stop(direction == GENESIS_ACCESS_WRITE ? GENESIS_STOP_UNSUPPORTED_MEMORY_REGION
                                                                       : GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                    direction == GENESIS_ACCESS_WRITE ? GENESIS_DIAG_ROM_WRITE_PROHIBITED
                                                                     : GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return GENESIS_ACCESS_FAIL;
  }
  if (address == SEGARECOMP_GENESIS_MAPPER_SRAM_CONTROL_REGISTER && runtime->cartridge_sram != 0 &&
      runtime->cartridge_sram->layout_supported) {
    /* ADR 0098: a cartridge with supported SRAM owns the mapper's SRAM control register (BYTE write, defined bits only). */
    if (direction == GENESIS_ACCESS_WRITE &&
        segarecomp_genesis_cartridge_sram_control_write_admitted((uint32_t)width, *value) != 0) {
      runtime->cartridge_sram_control = (uint8_t)(*value & SEGARECOMP_GENESIS_CARTRIDGE_SRAM_CONTROL_DEFINED);
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_ACCESS);
    return GENESIS_ACCESS_FAIL;
  }
  if (direction == GENESIS_ACCESS_WRITE && segarecomp_genesis_idle_control_register(address) != 0 &&
      segarecomp_genesis_idle_control_write_admitted(address, (uint32_t)width, *value) != 0) {
    return GENESIS_ACCESS_OK; /* idle-state control register written with its idle value: no state to change */
  }
  if (genesis_is_device(address)) {
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_controller_io_access(&runtime->devices, runtime->pad1, address, width, direction, &routed_value)) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_psg_region(address)) {
    if (genesis_z80_sync(runtime, stop_out)) return GENESIS_ACCESS_FAIL; /* SEG-032-T005: shared device, Z80 time first */
    /* SEG-007-T109: the co-located PSG (SN76489) audio port at the odd byte
       $C00011 is routed here BEFORE the VDP lane below, because that address is
       inside genesis_is_vdp_region's interval. routed_value carries the
       caller's write value in on a WRITE (genesis_psg_access has no other way
       to learn it); the PSG port is write-only so nothing is ever read back
       into *value. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_psg_access_68k(runtime, width, direction, &routed_value)) {
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_ym2612_region(address)) {
    if (genesis_z80_sync(runtime, stop_out)) return GENESIS_ACCESS_FAIL; /* SEG-032-T005: shared device, Z80 time first */
    /* SEG-007-T171: routed_value carries the caller's write value in on a
       WRITE (matching every other routed owner's contract), though
       genesis_ym2612_access's own accepted WRITE shape never actually
       consumes it -- there is no register-select state to store under this
       policy. It is read back into *value only on the accepted READ shape. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_ym2612_access_68k(runtime, address, width, direction, &routed_value)) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_vdp_region(address)) {
    /* SEG-007-T091: routed_value must carry the caller's own write value in
       on a WRITE access (genesis_vdp_access has no other way to learn what
       is being written); it is read back into *value only on a READ
       access -- a write never mutates the caller's own *value, matching
       genesis_route_access's own documented contract. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    uint16_t status_sample = 0U;
    /* SEG-007-T084's original status-read-triggered progression event. Do not
       progress on writes here: they remain exclusively command/register
       state accesses and must not acquire a hidden scheduler side effect on
       this specific read-triggered path. SEG-007-T175 (below, after a
       successful access) is now the PRIMARY suspension/progression
       mechanism for GENESIS_VDP_DMA_MEMORY_TO_VRAM: it drains synchronously
       the moment the arming CONTROL-port write itself commits, so by the
       time any later status read reaches this line the DMA is already
       GENESIS_VDP_DMA_IDLE and this call is a documented no-op
       (genesis_vdp_progress_dma's own phase guard). It is retained,
       unmodified, as a defensive invariant, not removed, so an unforeseen
       path that somehow leaves the DMA BUSY still progresses on a status
       read exactly as before this task. SEG-021-T037 widens the guard's own
       shape test to the shared genesis_is_vdp_status_read_shape predicate:
       every valid status-read shape (WORD and both BYTE lanes) is one status
       transaction, so this defensive invariant must not silently exempt the
       BYTE lanes from it. */
    if (genesis_is_vdp_status_read_shape(address, width, direction) &&
         genesis_vdp_progress_dma(runtime, stop_out) != GENESIS_ACCESS_OK)
      return GENESIS_ACCESS_FAIL;
    /* SEG-007-T175 (second correction): a LONG CONTROL-port write is handled
       entirely here, intercepted before the generic genesis_vdp_access
       dispatch below, because either of its two sub-transactions can
       independently arm a memory-to-VDP DMA and each such arm must drain
       synchronously (via genesis_vdp_control_port_write_word_and_suspend,
       which needs the full GenesisRuntime this routed caller already has)
       before the NEXT sub-transaction is allowed to run -- not merely once
       after the whole LONG access completes. This preserves the existing
       high-first order (SEG-007-T091, GTO1 p. 20) and the existing
       non-atomic partial-completion policy: if the high half's own word is
       rejected, nothing is mutated and the generic VDP-region stop below
       applies exactly as before; if the high half's word is accepted but
       its own resulting drain fails, the high half's already-committed arm
       is left in place and the overall access fails with that drain's own
       specific stop; only once the high half (and its own drain, if any)
       fully succeeds does the low half's sub-transaction run at all, and the
       identical rule applies to it before this whole LONG access returns.
       genesis_vdp_progress_dma remains the sole transfer-step
       implementation; no new transfer semantics are introduced here. */
    if (direction == GENESIS_ACCESS_WRITE && width == GENESIS_ACCESS_LONG &&
        address == UINT32_C(0x00C00004)) {
      const uint16_t high = (uint16_t)((*value >> 16) & 0xFFFFU);
      const uint16_t low = (uint16_t)(*value & 0xFFFFU);
      int rejected = 0;
      if (!genesis_vdp_control_port_write_word_and_suspend(runtime, high, &rejected, stop_out)) {
        if (rejected)
          *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                          GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
        return GENESIS_ACCESS_FAIL;
      }
      if (!genesis_vdp_control_port_write_word_and_suspend(runtime, low, &rejected, stop_out)) {
        if (rejected)
          *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                          GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
        return GENESIS_ACCESS_FAIL;
      }
      return GENESIS_ACCESS_OK;
    }
    if (genesis_vdp_access(&runtime->devices, address, width, direction,
                           &routed_value, &status_sample, runtime->scheduler.master_ticks)) {
      /* SEG-021-T040: GTO1 p. 19's VBlank status bit is now a truthful live
       * projection of the scheduler's own video-timing state (see
       * genesis_vdp_status_read / genesis_vdp_live_vblank_status_bit above),
       * not the permanently-zero static status_register field it used to
       * read verbatim. The scheduler's own crossing-onset edge
       * (genesis_irq6_scheduler_and_admit's `crosses_onset` test) remains
       * the sole place that raises `vblank_pending` and therefore the sole
       * IRQ6 edge source: a status read only *observes* bit 3 here and must
       * never also arm `vblank_pending` merely because it sampled an
       * already-live bit. Before this task, this block also armed
       * `vblank_pending` whenever the sampled bit was set; that read was
       * always of the permanently-zero static field, so it was
       * unreachable in practice. Once the bit is truthfully live, keeping
       * that arm would let a status read taken anywhere inside an
       * already-admitted VBlank window immediately re-arm `vblank_pending`
       * once more (the scheduler already cleared it on admission),
       * producing a spurious duplicate IRQ6 request the instant the
       * handler polls status -- an interrupt storm, not real hardware
       * behavior. Only the read-observation counter remains here. */
       if (genesis_is_vdp_status_read_shape(address, width, direction)) {
         ++runtime->devices.interrupt.vblank_status_read_count;
      }
      /* SEG-007-T175 PRIMARY suspension mechanism: if this exact access is
         the CONTROL-port write that just armed a memory-to-VDP DMA transfer
         (dma.phase is now BUSY / MEMORY_TO_VRAM as a direct side effect of
         genesis_vdp_access above), drain it synchronously to IDLE right
         here -- before this routed access returns to its caller (generated
         C) at all. This is a device-side effect of the write itself, not a
         dispatch-boundary check, so even a later straight-line instruction
         in the SAME generated block cannot execute until the transfer
         completes, matching the documented 68000 bus-stall exactly. If the
         drain then fails (an unroutable DMA source), the already-committed
         arm is left in place and the overall access reports failure -- the
         same non-atomic partial-completion policy the LONG-write
         decomposition above already documents, applied one level up. */
      if (genesis_vdp_drain_memory_to_vdp_dma_body(runtime, stop_out) != GENESIS_ACCESS_OK)
        return GENESIS_ACCESS_FAIL;
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_z80_bus_region(address)) {
    /* SEG-032-T005: the Z80 runs to the access time first; a read answers the live BUSACK; a write applies one edge. */
    if (!genesis_z80_bus_shape_ok(address, width, direction)) {
      *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS);
      return GENESIS_ACCESS_FAIL;
    }
    if (genesis_z80_sync(runtime, stop_out)) return GENESIS_ACCESS_FAIL;
    {
      const uint32_t bit = (width == GENESIS_ACCESS_WORD) ? UINT32_C(0x0100) : UINT32_C(0x0001);
      if (direction == GENESIS_ACCESS_READ) {
        *value = runtime->devices.z80_bus.bus_granted ? 0U : bit; /* BUSACK bit clear iff granted; other bits read 0 */
        return GENESIS_ACCESS_OK;
      }
      if (genesis_z80_bus_write(runtime, address, (*value & bit) != 0U, stop_out)) return GENESIS_ACCESS_FAIL;
    }
    return GENESIS_ACCESS_OK;
  }
  if (genesis_is_z80_ram_window_region(address)) {
    /* SEG-032-T004: routed_value carries the caller's write value in on a WRITE and is read back into *value only on a READ --
       a write never mutates the caller's *value, matching genesis_route_access's documented contract. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    GenesisDiagnosticCategory diagnostic;
    if (genesis_z80_sync(runtime, stop_out)) return GENESIS_ACCESS_FAIL; /* the 68K sees the Z80 as of now */
    /* Compat repair: a LONG access to the sound RAM window is, on the 68000's 16-bit bus, two consecutive WORD accesses (high
       half first), each following the window's own WORD rule (a write stores its high byte, a read returns the byte in both
       halves). Both halves must lie inside the RAM window and the 68K must hold the Z80 bus; the bus-grant gate is evaluated
       once for both halves, so a rejected access mutates nothing. The write-only bank register keeps its BYTE/WORD-only rule. */
    if (width == GENESIS_ACCESS_LONG && segarecomp_genesis_z80_ram_window_contains(address) &&
        segarecomp_genesis_z80_ram_window_contains(address + 3U) && runtime->devices.z80_bus.bus_granted) {
      uint32_t high = (direction == GENESIS_ACCESS_WRITE) ? ((*value >> 16) & 0xFFFFU) : 0U;
      uint32_t low = (direction == GENESIS_ACCESS_WRITE) ? (*value & 0xFFFFU) : 0U;
      diagnostic = genesis_z80_area_access(&runtime->devices, address, GENESIS_ACCESS_WORD, direction, &high);
      if (diagnostic == (GenesisDiagnosticCategory)0)
        diagnostic = genesis_z80_area_access(&runtime->devices, address + 2U, GENESIS_ACCESS_WORD, direction, &low);
      if (diagnostic == (GenesisDiagnosticCategory)0) {
        if (direction == GENESIS_ACCESS_READ) *value = (high << 16) | low;
        else {
          genesis_z80_epoch_note_ram_write(runtime, address);
          genesis_z80_epoch_note_ram_write(runtime, address + 2U);
        }
        return GENESIS_ACCESS_OK;
      }
      *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, diagnostic);
      return GENESIS_ACCESS_FAIL;
    }
    diagnostic = genesis_z80_area_access(&runtime->devices, address, width, direction, &routed_value);
    if (diagnostic == (GenesisDiagnosticCategory)0) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      else if (segarecomp_genesis_z80_ram_window_contains(address)) genesis_z80_epoch_note_ram_write(runtime, address);
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, diagnostic);
    return GENESIS_ACCESS_FAIL;
  }
  stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  *stop_out = stop;
  return GENESIS_ACCESS_FAIL;
}

/* SEG-020-T003: single bounded typed history append. Pure side-channel. */
static void genesis_history_append(GenesisRuntime *runtime, GenesisHistoryEventKind kind, uint32_t pc,
                                   uint32_t next_pc, uint8_t detail, uint8_t width, uint8_t direction,
                                   uint8_t bus) {
  GenesisExecutionHistory *history = &runtime->execution_history;
  GenesisExecutionHistoryEvent *event = &history->events[history->total_recorded % GENESIS_EXECUTION_HISTORY_CAPACITY];
  event->boundary = history->retired_count;
  event->pc = pc;
  event->next_pc = next_pc;
  event->kind = (uint8_t)kind;
  event->detail = detail;
  event->width = width;
  event->direction = direction;
  event->bus = bus;
  ++history->total_recorded;
}


/* SEG-020-T004: opt-in M68k checkpoint. Pure side-channel; see runtime.h. */
static void genesis_m68k_effect_note(GenesisRuntime *runtime, uint8_t kind, uint8_t width, uint32_t address,
                                     uint32_t value) {
  GenesisM68kCheckpoint *cp = &runtime->m68k_checkpoint;
  if (cp->effect_count >= GENESIS_M68K_CHECKPOINT_EFFECT_CAPACITY) {
    cp->unsupported_for_comparison = 1U;
    return;
  }
  cp->effects[cp->effect_count].kind = kind;
  cp->effects[cp->effect_count].width = width;
  cp->effects[cp->effect_count].address = address;
  cp->effects[cp->effect_count].value = value;
  ++cp->effect_count;
}

static uint64_t genesis_fnv_bytes(uint64_t h, uint64_t v, unsigned n) {
  unsigned i;
  for (i = 0; i < n; ++i) {
    h ^= (uint8_t)(v >> (8U * i));
    h *= UINT64_C(0x100000001B3);
  }
  return h;
}

static uint64_t genesis_m68k_digest_fields(const GenesisM68kCheckpoint *cp) {
  uint64_t h = UINT64_C(0xCBF29CE484222325);
  unsigned i;
  for (i = 0; i < 8U; ++i) h = genesis_fnv_bytes(h, cp->d[i], 4U);
  for (i = 0; i < 8U; ++i) h = genesis_fnv_bytes(h, cp->a[i], 4U);
  h = genesis_fnv_bytes(h, cp->usp, 4U);
  h = genesis_fnv_bytes(h, cp->sr, 2U);
  h = genesis_fnv_bytes(h, cp->pc, 4U);
  h = genesis_fnv_bytes(h, cp->last_effect_count, 1U);
  for (i = 0; i < cp->last_effect_count; ++i) {
    h = genesis_fnv_bytes(h, cp->last_effects[i].kind, 1U);
    h = genesis_fnv_bytes(h, cp->last_effects[i].width, 1U);
    h = genesis_fnv_bytes(h, cp->last_effects[i].address, 4U);
    h = genesis_fnv_bytes(h, cp->last_effects[i].value, 4U);
  }
  h = genesis_fnv_bytes(h, cp->last_unsupported, 1U);
  return h;
}

static void genesis_device_checkpoint_finalize(GenesisRuntime *runtime);

static void genesis_m68k_checkpoint_finalize(GenesisRuntime *runtime) {
  GenesisM68kCheckpoint *cp = &runtime->m68k_checkpoint;
  unsigned i;
  if (runtime->device_checkpoint.enabled) genesis_device_checkpoint_finalize(runtime);
  for (i = 0; i < 8U; ++i) {
    cp->d[i] = runtime->d[i];
    cp->a[i] = runtime->a[i];
  }
  /* SEG-021-T018: the architectural USP (the inactive slot in supervisor mode, A7 in user mode). */
  cp->usp = (runtime->sr & UINT16_C(0x2000)) != 0U ? runtime->usp : runtime->a[7];
  cp->sr = runtime->sr;
  cp->pc = runtime->pc;
  cp->last_effect_count = cp->effect_count;
  cp->last_unsupported = cp->unsupported_for_comparison;
  for (i = 0; i < cp->effect_count; ++i) cp->last_effects[i] = cp->effects[i];
  for (; i < GENESIS_M68K_CHECKPOINT_EFFECT_CAPACITY; ++i) cp->last_effects[i] = (GenesisM68kEffect){0};
  cp->digest = genesis_m68k_digest_fields(cp);
  cp->boundary = cp->valid ? cp->boundary + 1U : 1U;
  cp->valid = 1U;
  cp->effect_count = 0U;
  cp->unsupported_for_comparison = 0U;
}

uint64_t genesis_m68k_checkpoint_digest(const GenesisRuntime *runtime) {
  return runtime != 0 && runtime->m68k_checkpoint.valid ? runtime->m68k_checkpoint.digest : 0U;
}

GenesisM68kCheckpointComparison genesis_m68k_checkpoint_compare(const GenesisM68kCheckpoint *a,
                                                                const GenesisM68kCheckpoint *b) {
  unsigned i;
  if (a == 0 || b == 0 || !a->valid || !b->valid || a->last_unsupported || b->last_unsupported)
    return GENESIS_M68K_CHECKPOINT_UNSUPPORTED;
  if (a->digest != b->digest) return GENESIS_M68K_CHECKPOINT_DIFFERENT;
  /* Digest collision guard: fields must agree too. */
  for (i = 0; i < 8U; ++i)
    if (a->d[i] != b->d[i] || a->a[i] != b->a[i]) return GENESIS_M68K_CHECKPOINT_DIFFERENT;
  if (a->usp != b->usp || a->sr != b->sr || a->pc != b->pc || a->last_effect_count != b->last_effect_count)
    return GENESIS_M68K_CHECKPOINT_DIFFERENT;
  for (i = 0; i < a->last_effect_count; ++i)
    if (a->last_effects[i].kind != b->last_effects[i].kind || a->last_effects[i].width != b->last_effects[i].width ||
        a->last_effects[i].address != b->last_effects[i].address ||
        a->last_effects[i].value != b->last_effects[i].value)
      return GENESIS_M68K_CHECKPOINT_DIFFERENT;
  return GENESIS_M68K_CHECKPOINT_EQUAL;
}

int genesis_m68k_checkpoint_write_detail(FILE *output, const GenesisRuntime *runtime) {
  const GenesisM68kCheckpoint *cp;
  unsigned i;
  if (output == 0 || runtime == 0) return 1;
  cp = &runtime->m68k_checkpoint;
  if (!cp->valid) return fputs("{\"m68k_checkpoint\":null}\n", output) < 0;
  fprintf(output, "{\"m68k_checkpoint\":{\"boundary\":%llu,\"digest\":\"%016llx\",\"unsupported\":%u,\"pc\":%u,\"sr\":%u,\"usp\":%u,\"d\":[",
          (unsigned long long)cp->boundary, (unsigned long long)cp->digest, (unsigned)cp->last_unsupported,
          (unsigned)cp->pc, (unsigned)cp->sr, (unsigned)cp->usp);
  for (i = 0; i < 8U; ++i) fprintf(output, "%s%u", i ? "," : "", (unsigned)cp->d[i]);
  fputs("],\"a\":[", output);
  for (i = 0; i < 8U; ++i) fprintf(output, "%s%u", i ? "," : "", (unsigned)cp->a[i]);
  fputs("],\"effects\":[", output);
  for (i = 0; i < cp->last_effect_count; ++i)
    fprintf(output, "%s{\"k\":%u,\"w\":%u,\"a\":%u,\"v\":%u}", i ? "," : "", (unsigned)cp->last_effects[i].kind,
            (unsigned)cp->last_effects[i].width, (unsigned)cp->last_effects[i].address,
            (unsigned)cp->last_effects[i].value);
  return fputs("]}}\n", output) < 0;
}

/* Device-visible classification only (work RAM and cartridge ROM are not
   device accesses); same predicate order as the router. 0 = not a device. */
static uint8_t genesis_history_device_region(uint32_t address) {
  if (address < UINT32_C(0x00400000) || genesis_is_work_ram(address, 1U)) return 0U;
  if (genesis_is_device(address)) return GENESIS_HISTORY_REGION_CONTROLLER_IO;
  if (genesis_is_psg_region(address)) return GENESIS_HISTORY_REGION_PSG;
  if (genesis_is_ym2612_region(address)) return GENESIS_HISTORY_REGION_YM2612;
  if (genesis_is_vdp_region(address)) return GENESIS_HISTORY_REGION_VDP;
  if (genesis_is_z80_bus_region(address)) return GENESIS_HISTORY_REGION_Z80_BUS;
  if (genesis_is_z80_ram_window_region(address)) return GENESIS_HISTORY_REGION_Z80_RAM_WINDOW;
  return 0U;
}

/* SEG-020-T006: opt-in Genesis device checkpoint. Pure side-channel; see runtime.h. */
static const char *const genesis_device_component_names[GENESIS_DEVICE_COMPONENT_COUNT] = {
    "vdp_registers", "vdp_dma", "vdp_vram", "vdp_cram", "vdp_vsram",
    "interrupt",     "psg",     "z80_bus",  "z80_ram", "controller_io"};

const char *genesis_device_component_name(unsigned index) {
  return index < GENESIS_DEVICE_COMPONENT_COUNT ? genesis_device_component_names[index] : "unknown";
}

static void genesis_device_event_note(GenesisRuntime *runtime, uint8_t kind, uint8_t region, uint8_t width,
                                      uint32_t address, uint32_t value) {
  GenesisDeviceCheckpoint *cp = &runtime->device_checkpoint;
  if (kind == GENESIS_DEVICE_EVENT_WRITE) cp->mem_dirty = 1U;
  if (cp->event_count >= GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY) {
    cp->unsupported_for_comparison = 1U;
    return;
  }
  cp->events[cp->event_count].kind = kind;
  cp->events[cp->event_count].region = region;
  cp->events[cp->event_count].width = width;
  cp->events[cp->event_count].address = address;
  cp->events[cp->event_count].value = value;
  ++cp->event_count;
}

static uint64_t genesis_fnv_buffer(uint64_t h, const uint8_t *bytes, size_t count) {
  size_t i;
  for (i = 0; i < count; ++i) {
    h ^= bytes[i];
    h *= UINT64_C(0x100000001B3);
  }
  return h;
}

static void genesis_device_checkpoint_finalize(GenesisRuntime *runtime) {
  GenesisDeviceCheckpoint *cp = &runtime->device_checkpoint;
  const GenesisDeviceState *d = &runtime->devices;
  const uint64_t basis = UINT64_C(0xCBF29CE484222325);
  const uint8_t dma_busy = d->vdp.dma.phase == GENESIS_VDP_DMA_BUSY ? 1U : 0U;
  uint64_t h;
  unsigned i;
  /* Interrupt events derived from the interrupt state delta of this boundary. */
  {
    const uint32_t raised = d->interrupt.vblank_transition_count - cp->prev_vblank_transition_count;
    if (raised != 0U) genesis_device_event_note(runtime, GENESIS_DEVICE_EVENT_VBLANK_RAISE, 0U, 0U, 0U, raised);
    if ((cp->prev_vblank_pending != 0U || raised != 0U) && d->interrupt.vblank_pending == 0U)
      genesis_device_event_note(runtime, GENESIS_DEVICE_EVENT_IRQ_ADMIT, 0U, 0U, 0U, 6U);
  }
  cp->prev_vblank_transition_count = d->interrupt.vblank_transition_count;
  cp->prev_vblank_pending = d->interrupt.vblank_pending;

  h = basis;
  for (i = 0; i < GENESIS_VDP_REGISTER_COUNT; ++i) h = genesis_fnv_bytes(h, d->vdp.registers[i], 2U);
  h = genesis_fnv_bytes(h, d->vdp.control_port_awaiting_second_word, 1U);
  h = genesis_fnv_bytes(h, d->vdp.control_port_first_word, 2U);
  h = genesis_fnv_bytes(h, d->vdp.addressed_pointer, 4U);
  h = genesis_fnv_bytes(h, d->vdp.auto_increment_value, 2U);
  h = genesis_fnv_bytes(h, d->vdp.status_register, 2U);
  h = genesis_fnv_bytes(h, d->vdp.data_port_transfer_code, 1U);
  h = genesis_fnv_bytes(h, d->vdp.data_port_transfer_code_valid, 1U);
  cp->component[0] = h;
  h = basis;
  h = genesis_fnv_bytes(h, (uint64_t)d->vdp.dma.phase, 1U);
  h = genesis_fnv_bytes(h, (uint64_t)d->vdp.dma.kind, 1U);
  h = genesis_fnv_bytes(h, d->vdp.dma.source_address, 4U);
  h = genesis_fnv_bytes(h, d->vdp.dma.remaining_length, 4U);
  h = genesis_fnv_bytes(h, d->vdp.dma.fill_byte_count, 4U);
  h = genesis_fnv_bytes(h, d->vdp.dma.transfer_access_count, 4U);
  h = genesis_fnv_bytes(h, d->vdp.dma.write_target_code, 1U);
  cp->component[1] = h;
  if (!cp->valid || cp->mem_dirty || dma_busy || cp->prev_dma_busy) {
    cp->component[2] = genesis_fnv_buffer(basis, d->vdp.vram, GENESIS_VDP_VRAM_BYTES);
    cp->component[3] = genesis_fnv_buffer(basis, d->vdp.cram, GENESIS_VDP_CRAM_BYTES);
    cp->component[4] = genesis_fnv_buffer(basis, d->vdp.vsram, GENESIS_VDP_VSRAM_BYTES);
    cp->component[8] = genesis_fnv_buffer(basis, d->z80_bus.z80_ram, GENESIS_Z80_RAM_BYTES);
  }
  cp->mem_dirty = 0U;
  cp->prev_dma_busy = dma_busy;
  h = basis;
  h = genesis_fnv_bytes(h, d->interrupt.vblank_pending, 1U);
  h = genesis_fnv_bytes(h, d->interrupt.vblank_status_read_count, 4U);
  h = genesis_fnv_bytes(h, d->interrupt.vblank_transition_count, 4U);
  h = genesis_fnv_bytes(h, d->interrupt.checkpoint_entered, 1U);
  h = genesis_fnv_bytes(h, d->interrupt.vblank_transition_count_at_checkpoint_entry, 4U);
  cp->component[5] = h;
  h = basis;
  h = genesis_fnv_bytes(h, d->psg.write_count, 4U);
  h = genesis_fnv_bytes(h, d->psg.write_digest, 4U);
  cp->component[6] = h;
  h = basis;
  h = genesis_fnv_bytes(h, d->z80_bus.bus_requested, 1U);
  h = genesis_fnv_bytes(h, d->z80_bus.bus_granted, 1U);
  h = genesis_fnv_bytes(h, d->z80_bus.reset_released, 1U);
  h = genesis_fnv_bytes(h, (uint32_t)d->z80_bus.bank, 2U);
  cp->component[7] = h;
  h = basis;
  for (i = 0; i < 3U; ++i) {
    h = genesis_fnv_bytes(h, d->controller_io.data[i], 1U);
    h = genesis_fnv_bytes(h, d->controller_io.ctrl[i], 1U);
  }
  cp->component[9] = h;

  cp->last_event_count = cp->event_count;
  cp->last_unsupported = cp->unsupported_for_comparison;
  for (i = 0; i < cp->event_count; ++i) cp->last_events[i] = cp->events[i];
  for (; i < GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY; ++i) cp->last_events[i] = (GenesisDeviceEvent){0};
  h = basis;
  for (i = 0; i < GENESIS_DEVICE_COMPONENT_COUNT; ++i) h = genesis_fnv_bytes(h, cp->component[i], 8U);
  h = genesis_fnv_bytes(h, cp->last_event_count, 1U);
  for (i = 0; i < cp->last_event_count; ++i) {
    h = genesis_fnv_bytes(h, cp->last_events[i].kind, 1U);
    h = genesis_fnv_bytes(h, cp->last_events[i].region, 1U);
    h = genesis_fnv_bytes(h, cp->last_events[i].width, 1U);
    h = genesis_fnv_bytes(h, cp->last_events[i].address, 4U);
    h = genesis_fnv_bytes(h, cp->last_events[i].value, 4U);
  }
  h = genesis_fnv_bytes(h, cp->last_unsupported, 1U);
  cp->digest = h;
  cp->boundary = cp->valid ? cp->boundary + 1U : 1U;
  cp->valid = 1U;
  cp->event_count = 0U;
  cp->unsupported_for_comparison = 0U;
}

GenesisDeviceCheckpointComparison genesis_device_checkpoint_compare(const GenesisDeviceCheckpoint *a,
                                                                    const GenesisDeviceCheckpoint *b,
                                                                    const char **component_out) {
  unsigned i;
  if (component_out != 0) *component_out = 0;
  if (a == 0 || b == 0 || !a->valid || !b->valid || a->last_unsupported || b->last_unsupported)
    return GENESIS_DEVICE_CHECKPOINT_UNSUPPORTED;
  if (a->last_event_count != b->last_event_count) {
    if (component_out != 0) *component_out = "events";
    return GENESIS_DEVICE_CHECKPOINT_EVENT_DIFFERENT;
  }
  for (i = 0; i < a->last_event_count; ++i)
    if (a->last_events[i].kind != b->last_events[i].kind || a->last_events[i].region != b->last_events[i].region ||
        a->last_events[i].width != b->last_events[i].width ||
        a->last_events[i].address != b->last_events[i].address ||
        a->last_events[i].value != b->last_events[i].value) {
      if (component_out != 0) *component_out = "events";
      return GENESIS_DEVICE_CHECKPOINT_EVENT_DIFFERENT;
    }
  for (i = 0; i < GENESIS_DEVICE_COMPONENT_COUNT; ++i)
    if (a->component[i] != b->component[i]) {
      if (component_out != 0) *component_out = genesis_device_component_names[i];
      return GENESIS_DEVICE_CHECKPOINT_STATE_DIFFERENT;
    }
  return GENESIS_DEVICE_CHECKPOINT_EQUAL;
}

int genesis_device_checkpoint_write_detail(FILE *output, const GenesisRuntime *runtime) {
  const GenesisDeviceCheckpoint *cp;
  unsigned i;
  if (output == 0 || runtime == 0) return 1;
  cp = &runtime->device_checkpoint;
  if (!cp->valid) return fputs("{\"device_checkpoint\":null}\n", output) < 0;
  fprintf(output, "{\"device_checkpoint\":{\"boundary\":%llu,\"digest\":\"%016llx\",\"unsupported\":%u,\"components\":{",
          (unsigned long long)cp->boundary, (unsigned long long)cp->digest, (unsigned)cp->last_unsupported);
  for (i = 0; i < GENESIS_DEVICE_COMPONENT_COUNT; ++i)
    fprintf(output, "%s\"%s\":\"%016llx\"", i ? "," : "", genesis_device_component_names[i],
            (unsigned long long)cp->component[i]);
  fputs("},\"events\":[", output);
  for (i = 0; i < cp->last_event_count; ++i)
    fprintf(output, "%s{\"k\":%u,\"r\":%u,\"w\":%u,\"a\":%u,\"v\":%u}", i ? "," : "", (unsigned)cp->last_events[i].kind,
            (unsigned)cp->last_events[i].region, (unsigned)cp->last_events[i].width,
            (unsigned)cp->last_events[i].address, (unsigned)cp->last_events[i].value);
  return fputs("]}}\n", output) < 0;
}

/* SEG-021-T036: `discarded` marks a DATA read whose value the instruction architecturally discards (the read-before-
   write of memory CLR, memory Scc and memory MOVE from SR). It changes exactly one lane: a WORD or even-BYTE read of
   the write-only Z80 RESET register ($A11200) returns a deterministic 0 and mutates nothing (reset/bus state
   unchanged). Hardware: that read returns open-bus data -- the MSB of the next instruction fetch with the LSB zero
   (Charles MacDonald, "Sega Genesis hardware notes" v0.8, section 1 note 4) -- with no side effect; GTO1 v1.00 p. 76
   documents the register as write-only. The value is never observed, so 0 is a faithful stand-in. Every other access
   (including LONG, the odd byte, and every value-consuming read of $A11200) takes the ordinary route below and fails
   closed exactly as before. The VDP data port under a write-class code and the PSG port stay fail-closed even for a
   discarded read: a 68000 read of either locks the machine up on hardware (see the VDP data-port write contract and
   the PSG compatibility policy). The access is recorded like any other DATA read (no new bus kind). */
static GenesisAccessResultKind genesis_route_access_classified(GenesisRuntime *runtime, GenesisBusKind bus_kind,
                                                               uint32_t address, GenesisAccessWidth width,
                                                               GenesisAccessDirection direction, uint32_t *value,
                                                               GenesisRuntimeStop *stop_out, int discarded) {
  GenesisAccessResultKind status;
  const int is_read_kind = bus_kind == GENESIS_BUS_INSTRUCTION_READ || bus_kind == GENESIS_BUS_DATA_READ ||
                           bus_kind == GENESIS_BUS_STACK_READ;
  const int is_write_kind = bus_kind == GENESIS_BUS_DATA_WRITE || bus_kind == GENESIS_BUS_STACK_WRITE;
  if ((!is_read_kind && !is_write_kind) || (is_read_kind && direction != GENESIS_ACCESS_READ) ||
      (is_write_kind && direction != GENESIS_ACCESS_WRITE)) {
    if (stop_out != 0) *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                                        GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
    return GENESIS_ACCESS_FAIL;
  }
  if (discarded && runtime != 0 && value != 0 && bus_kind == GENESIS_BUS_DATA_READ &&
      segarecomp_genesis_discarded_read_admitted(address, (uint32_t)width) != 0) {
    *value = 0U;
    status = GENESIS_ACCESS_OK;
  } else {
    status = genesis_route_access_unrecorded(runtime, address, width, direction, value, stop_out);
  }
  if (status == GENESIS_ACCESS_OK && runtime != 0 && runtime->m68k_checkpoint.enabled && is_write_kind &&
      value != 0)
    genesis_m68k_effect_note(runtime, GENESIS_M68K_EFFECT_WRITE, (uint8_t)width, address,
                             width == GENESIS_ACCESS_LONG ? *value : *value & (width == GENESIS_ACCESS_WORD ? 0xFFFFU : 0xFFU));
  if (status == GENESIS_ACCESS_OK && runtime != 0 && runtime->device_checkpoint.enabled && is_write_kind &&
      value != 0) {
    const uint8_t device_region = genesis_history_device_region(address);
    if (device_region != 0U)
      genesis_device_event_note(runtime, GENESIS_DEVICE_EVENT_WRITE, device_region, (uint8_t)width, address,
                                width == GENESIS_ACCESS_LONG ? *value : *value & (width == GENESIS_ACCESS_WORD ? 0xFFFFU : 0xFFU));
  }
  if (status == GENESIS_ACCESS_OK && runtime != 0 && runtime->execution_history.detail_enabled) {
    const uint8_t region = genesis_history_device_region(address);
    if (region != 0U)
      genesis_history_append(runtime, GENESIS_HISTORY_ACCESS, 0U, 0U, region, (uint8_t)width, (uint8_t)direction,
                             (uint8_t)bus_kind);
  }
  return status;
}

GenesisAccessResultKind genesis_route_access_bus(GenesisRuntime *runtime, GenesisBusKind bus_kind, uint32_t address,
                                                 GenesisAccessWidth width, GenesisAccessDirection direction,
                                                 uint32_t *value, GenesisRuntimeStop *stop_out) {
  return genesis_route_access_classified(runtime, bus_kind, address, width, direction, value, stop_out, 0);
}

GenesisAccessResultKind genesis_route_access(GenesisRuntime *runtime, uint32_t address,
                                              GenesisAccessWidth width,
                                              GenesisAccessDirection direction, uint32_t *value,
                                              GenesisRuntimeStop *stop_out) {
  return genesis_route_access_bus(runtime,
                                  direction == GENESIS_ACCESS_WRITE ? GENESIS_BUS_DATA_WRITE : GENESIS_BUS_DATA_READ,
                                  address, width, direction, value, stop_out);
}

GenesisAccessResultKind genesis_route_access_discarded_read(GenesisRuntime *runtime, uint32_t address,
                                                            GenesisAccessWidth width,
                                                            GenesisAccessDirection direction, uint32_t *value,
                                                            GenesisRuntimeStop *stop_out) {
  return genesis_route_access_classified(runtime, GENESIS_BUS_DATA_READ, address, width, direction, value, stop_out,
                                         1);
}


GenesisControlTransfer genesis_internal_dispatch_inconsistency_stop(GenesisRuntime *runtime) {
  GenesisControlTransfer transfer = {0};
  (void)runtime;
  transfer.kind = GENESIS_STOP;
  transfer.stop.stop_class = GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY;
  transfer.stop.diagnostic_category = GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY;
  return transfer;
}

/*
 * SEG-007-T252 / ADR-0040: the former SEG-007-T107 (`genesis_note_loop_backedge`),
 * SEG-007-T211/ADR-0035 (`genesis_note_loop_completion`), and SEG-007-T155/
 * ADR-0017 (`genesis_note_data_progress`) watchdog-progress-note APIs, and the
 * `GenesisLoopProgressNote`/`GenesisLoopCompletionNote`/`GenesisDataProgressNote`
 * storage they wrote into, have been removed along with the generated-runtime
 * progress watchdog they fed. Termination/progress policy is now the runner's
 * (`genesis_runtime_run`'s finite dispatch allowance), never a CPU/codegen-
 * proven guest fact. See docs/decisions/0040-runner-owned-dispatch-allowance-
 * replaces-generated-runtime-progress-watchdog.md.
 */

/*
 * SEG-007-T124 / ADR-0009: a linear scan over the small, generation-time-fixed
 * candidate array is the exact same generic membership test every other
 * bounded, generated array in this runtime already relies on -- it selects
 * no dispatch target and never reads the source image or a target
 * instruction.
 */
int m68k_indirect_target_member(const uint32_t *targets, uint32_t count, uint32_t value) {
  uint32_t index;
  if (targets == 0) return 0;
  for (index = 0; index < count; ++index) {
    if (targets[index] == value) return 1;
  }
  return 0;
}

/*
 * SEG-007-T174 / ADR-0024: an ordinary sorted-array binary search over the
 * generation-time-fixed EmittedCodeAddressSet. Like
 * m68k_indirect_target_member above, it selects no dispatch target and never
 * reads the source image or a target instruction -- it only ever compares
 * one already-computed integer against a compiled-in address table. The
 * caller (generated C, see libs/codegen/c11/src/m68k.cpp) always supplies the
 * array in strictly ascending sorted order.
 */
int m68k_emitted_code_address_member(const uint32_t *addresses, uint32_t count, uint32_t value) {
  uint32_t low = 0;
  uint32_t high = count;
  if (addresses == 0) return 0;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2U;
    if (addresses[mid] == value) return 1;
    if (addresses[mid] < value) low = mid + 1U;
    else high = mid;
  }
  return 0;
}

static GenesisCheckpointPcClass genesis_checkpoint_pc_class_for_target(uint32_t target) {
#if defined(SEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET)
  /* Test-only seam: production defines no checkpoint class.  A test build may
   * provide one synthetic resolved target solely to exercise the sticky
   * mechanism through this same dispatcher path. */
  if (target == (uint32_t)SEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET)
    return (GenesisCheckpointPcClass)1;
#endif
  (void)target;
  /* T131 owns the mechanism but no non-UNKNOWN target category. */
  return GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN;
}

/*
 * SEG-021-T018 / ADR 0043 §7: the Genesis machine binding of the M68K-owned
 * exception core (libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h).
 * The core owns entry ordering, the six-byte frame, the SR update, the SSP/USP
 * swap and RTE; this binding supplies only the machine hooks: routed stack
 * access (with the existing stack bus kinds), work-RAM frame-extent
 * validation, build-time-resolved handler entries, and the project-only IRQ6
 * frame-origin bookkeeping plus diagnostic side channels through the entry /
 * return notifications. One binding context per call; no file-scope state.
 *
 * Frame-write guarantee (ADR 0043 §5 / §7 hook contract): the core's
 * frame_write hook is non-fallible, so validate_stack_extent must accept only
 * extents in which every aligned word/long frame write is certain to succeed.
 * It accepts an even `base` whose masked 24-bit physical extent lies wholly
 * in work RAM (the extent check also rejects crossing the physical boundary). For
 * such an extent every frame write the core issues (word at base, long at
 * base + 2) reaches genesis_route_access_bus with a stack-write bus kind and
 * WRITE direction (kind/direction check passes), a valid width, non-null
 * runtime/value/stop pointers, a masked 24-bit bus address,
 * an even address, and a range inside work RAM, so
 * genesis_route_access_unrecorded takes its work-RAM store branch, which has
 * no failure return. The routed path is kept so the stack_write bus kind,
 * execution history and checkpoint write effects remain observable.
 *
 * If the routed write nevertheless fails (an impossible invariant violation),
 * the binding records it in `frame_write_failed`; the core still completes and
 * commits (no rollback of frame bytes) and
 * genesis_construct_exception_frame_and_transfer converts the record into the
 * terminal GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY stop. It is never a
 * recoverable core outcome.
 */
typedef struct GenesisM68kExceptionContext {
  GenesisRuntime *runtime;
  GenesisRuntimeStop routed;
  int irq6_frame;
  int frame_write_failed;
} GenesisM68kExceptionContext;

static int genesis_m68k_validate_stack_extent(void *context, uint32_t base, uint32_t length,
                                               SegarecompM68kStackDirection direction) {
  (void)context;
  (void)direction;
  /* Even base (the core also checks it) and the whole extent in work RAM: the
     precondition of the non-fallible frame_write guarantee above. */
  return (base & 1U) == 0U && base <= UINT32_MAX - length &&
         genesis_is_work_ram(base & UINT32_C(0x00FFFFFF), length);
}

static int genesis_m68k_stack_read(void *context, uint32_t address, uint32_t size, uint32_t *value) {
  GenesisM68kExceptionContext *bound = (GenesisM68kExceptionContext *)context;
  GenesisRuntimeStop routed = {0};
  if (genesis_route_access_bus(bound->runtime, GENESIS_BUS_STACK_READ, address & UINT32_C(0x00FFFFFF),
                               size == 2U ? GENESIS_ACCESS_WORD : GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, value,
                               &routed) != GENESIS_ACCESS_OK) {
    bound->routed = routed;
    return 0;
  }
  return 1;
}

/* Non-fallible by the ADR 0043 §5 hook contract (see the binding comment
   above); an impossible routed failure is recorded, never returned. */
static void genesis_m68k_frame_write(void *context, uint32_t address, uint32_t size, uint32_t value) {
  GenesisM68kExceptionContext *bound = (GenesisM68kExceptionContext *)context;
  GenesisRuntimeStop routed = {0};
  uint32_t routed_value = value;
  if (genesis_route_access_bus(bound->runtime, GENESIS_BUS_STACK_WRITE, address & UINT32_C(0x00FFFFFF),
                               size == 2U ? GENESIS_ACCESS_WORD : GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE,
                               &routed_value, &routed) != GENESIS_ACCESS_OK)
    bound->frame_write_failed = 1;
}

/* SEG-021-T019 / ADR 0043 §3: the software-exception vector set (4, 6, 7, 10,
   11, 32-47) the table `software_exception_handler_entry` serves. */
static int genesis_is_software_exception_vector(uint32_t vector) {
  return vector == 4U || vector == 6U || vector == 7U || vector == 10U || vector == 11U ||
         (vector >= 32U && vector <= 47U);
}

/* ADR 0020 §6 / ADR 0037 B / ADR 0043 §7: the Genesis vector table is the
   immutable cartridge image at address 0, resolved at build time; only the
   generated `main`'s compiled-in entries are ever selected here. */
static SegarecompM68kVectorResolution genesis_m68k_resolve_vector(void *context, uint32_t vector,
                                                                  uint32_t *handler_entry) {
  const GenesisRuntime *runtime = ((GenesisM68kExceptionContext *)context)->runtime;
  switch (vector) {
  case GENESIS_M68K_VECTOR_ZERO_DIVIDE:
    if (!runtime->divide_by_zero_handler_present) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
    *handler_entry = runtime->divide_by_zero_handler_entry;
    return SEGARECOMP_M68K_VECTOR_HANDLER;
  case GENESIS_M68K_VECTOR_PRIVILEGE_VIOLATION:
    if (!runtime->privilege_violation_handler_present) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
    *handler_entry = runtime->privilege_violation_handler_entry;
    return SEGARECOMP_M68K_VECTOR_HANDLER;
  case GENESIS_M68K_VECTOR_LEVEL6_AUTOVECTOR:
    if (!runtime->irq6_handler_present) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
    *handler_entry = runtime->irq6_handler_entry;
    return SEGARECOMP_M68K_VECTOR_HANDLER;
  default:
    /* SEG-021-T019: the software-exception vectors, from the build-time table. */
    if (genesis_is_software_exception_vector(vector) && runtime->software_exception_handler_present[vector]) {
      *handler_entry = runtime->software_exception_handler_entry[vector];
      return SEGARECOMP_M68K_VECTOR_HANDLER;
    }
    return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
  }
}

static void genesis_m68k_on_exception_entry(void *context, uint32_t vector, uint32_t frame_base,
                                            uint32_t handler_entry) {
  GenesisRuntime *runtime = ((GenesisM68kExceptionContext *)context)->runtime;
  if (vector == GENESIS_M68K_VECTOR_LEVEL6_AUTOVECTOR)
    genesis_note_irq6_exception_frame(runtime, frame_base & UINT32_C(0x00FFFFFF));
  if (runtime->m68k_checkpoint.enabled)
    genesis_m68k_effect_note(runtime, GENESIS_M68K_EFFECT_TRAP, 0U, handler_entry, vector);
  if (runtime->execution_history.detail_enabled)
    genesis_history_append(runtime, GENESIS_HISTORY_TRANSFER, 0U, handler_entry,
                           GENESIS_HISTORY_TRANSFER_EXCEPTION_ENTRY, 0U, 0U, 0U);
}

static void genesis_m68k_on_exception_return(void *context, uint32_t frame_base) {
  GenesisM68kExceptionContext *bound = (GenesisM68kExceptionContext *)context;
  const uint32_t physical_base = frame_base & UINT32_C(0x00FFFFFF);
  bound->irq6_frame = genesis_is_work_ram(physical_base, 6U) &&
                      genesis_take_irq6_exception_frame_origin(bound->runtime, physical_base);
}

static SegarecompM68kMachineHooks genesis_m68k_exception_hooks(GenesisM68kExceptionContext *context) {
  SegarecompM68kMachineHooks hooks = {0};
  hooks.context = context;
  hooks.validate_stack_extent = genesis_m68k_validate_stack_extent;
  hooks.stack_read = genesis_m68k_stack_read;
  hooks.frame_write = genesis_m68k_frame_write;
  hooks.resolve_vector = genesis_m68k_resolve_vector;
  hooks.on_exception_entry = genesis_m68k_on_exception_entry;
  hooks.on_exception_return = genesis_m68k_on_exception_return;
  return hooks;
}

static SegarecompM68kCpuBinding genesis_m68k_cpu_binding(GenesisRuntime *runtime) {
  SegarecompM68kCpuBinding cpu;
  cpu.sr = &runtime->sr;
  cpu.active_sp = &runtime->a[7];
  cpu.inactive_sp = &runtime->usp;
  cpu.pc = &runtime->pc;
  return cpu;
}

/*
 * SEG-007-T047 / ADR-0020 §9, generalized by ADR 0043 §5: RTE restoration
 * through the M68K-owned core. Both routed reads first; the atomic
 * {sr, pc, a[7], inactive SP} commit happens only after both succeed; a failed
 * read leaves every field untouched. A restored T = 1 fails closed (trace is
 * deferred, ADR 0043 §6). No target-opcode fetch/decode.
 */
int genesis_exception_return(GenesisRuntime *runtime, uint32_t *restored_pc_out,
                             GenesisRuntimeStop *stop_out) {
  GenesisM68kExceptionContext context = {0};
  SegarecompM68kMachineHooks hooks;
  SegarecompM68kCpuBinding cpu;
  SegarecompM68kExceptionStatus status;
  if (runtime == 0 || restored_pc_out == 0 || stop_out == 0) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  context.runtime = runtime;
  hooks = genesis_m68k_exception_hooks(&context);
  cpu = genesis_m68k_cpu_binding(runtime);
  status = segarecomp_m68k_exception_return(&hooks, &cpu, restored_pc_out);
  switch (status) {
  case SEGARECOMP_M68K_EXCEPTION_OK: return 1;
  case SEGARECOMP_M68K_EXCEPTION_STACK_INVALID:
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    (runtime->a[7] & 1U) != 0U ? GENESIS_DIAG_INVALID_STACK_ALIGNMENT
                                                                : GENESIS_DIAG_INVALID_STACK_RANGE);
    return 0;
  case SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED:
    *stop_out = context.routed;
    return 0;
  case SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED:
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION, GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION);
    return 0;
  case SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE:
  case SEGARECOMP_M68K_EXCEPTION_BAD_BINDING:
    break;
  }
  *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                  GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
  return 0;
}

/*
 * SEG-007-T222 / ADR-0037, generalized by ADR 0043 §5: construct the six-byte
 * frame and transfer to the build-time-resolved handler of `vector` through
 * the M68K-owned core. `sr_keep_mask`/`sr_forced_bits` give the new SR (IRQ6:
 * clear T, set S, mask 6; synchronous exceptions: clear T, set S, keep the
 * mask). `fail_stop_class`/`fail_diag` select the caller-specific diagnostic
 * for a frame that cannot be constructed or a handler that is not installed.
 * A routed frame-write failure AFTER validation is impossible by the binding's
 * frame-write guarantee; if it happens anyway it is recorded by the binding and
 * reported here as the terminal internal-dispatch-inconsistency stop (the
 * committed frame and CPU state are NOT rolled back; the run ends).
 *
 * Returns 0 = failed closed (`*result` holds the GENESIS_STOP; for every
 *             ordinary refusal no frame or state mutation of any kind);
 *         1 = frame constructed and committed; `*result` holds the transfer
 *             (`next_pc` and `runtime->pc` are the handler entry).
 */
static int genesis_finish_exception_entry(SegarecompM68kExceptionStatus status, int frame_write_failed,
                                          uint32_t handler_entry, GenesisStopClass fail_stop_class,
                                          GenesisDiagnosticCategory fail_diag, GenesisControlTransfer *result) {
  if (status != SEGARECOMP_M68K_EXCEPTION_OK || frame_write_failed) {
    const int inconsistent = frame_write_failed || (status != SEGARECOMP_M68K_EXCEPTION_STACK_INVALID &&
                                                    status != SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE);
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = inconsistent ? genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY)
                                : genesis_access_stop(fail_stop_class, fail_diag);
    return 0;
  }
  result->next_pc = handler_entry;
  return 1;
}

static int genesis_construct_exception_frame_and_transfer(GenesisRuntime *runtime, uint32_t vector,
                                                           uint32_t return_pc, uint16_t sr_keep_mask,
                                                           uint16_t sr_forced_bits,
                                                           GenesisStopClass fail_stop_class,
                                                           GenesisDiagnosticCategory fail_diag,
                                                           GenesisControlTransfer *result) {
  GenesisM68kExceptionContext context = {0};
  SegarecompM68kMachineHooks hooks;
  SegarecompM68kCpuBinding cpu;
  uint32_t handler_entry = 0U;
  SegarecompM68kExceptionStatus status;
  context.runtime = runtime;
  hooks = genesis_m68k_exception_hooks(&context);
  cpu = genesis_m68k_cpu_binding(runtime);
  status = segarecomp_m68k_exception_enter(&hooks, &cpu, vector, return_pc, sr_keep_mask, sr_forced_bits,
                                           &handler_entry);
  return genesis_finish_exception_entry(status, context.frame_write_failed, handler_entry, fail_stop_class, fail_diag,
                                        result);
}

/*
 * SEG-021-T020 / ADR 0043 §3, §7: take the interrupt the M68K-owned acceptance
 * rule recognized at this boundary. Genesis acknowledges every level it wires
 * with the autovector (only VBlank, level 6, is wired: vector 30), so the
 * vector, the six-byte frame on the SSP, the entry SR (S = 1, T = 0, I =
 * level) and the stacked PC (`result->next_pc`, the next instruction -- after
 * a STOP, the instruction after STOP) are exactly the IRQ6 delivery of ADR 0020.
 * Returns 1 (entered; `result` holds the handler) or 0 (failed closed with the
 * interrupt diagnostic; nothing changed).
 */
static int genesis_accept_interrupt(GenesisRuntime *runtime, uint32_t level, GenesisControlTransfer *result) {
  GenesisM68kExceptionContext context = {0};
  SegarecompM68kMachineHooks hooks;
  SegarecompM68kCpuBinding cpu;
  uint32_t handler_entry = 0U;
  SegarecompM68kExceptionStatus status;
  context.runtime = runtime;
  hooks = genesis_m68k_exception_hooks(&context);
  cpu = genesis_m68k_cpu_binding(runtime);
  status = segarecomp_m68k_interrupt_enter(
      &hooks, &cpu, &runtime->m68k_interrupt, level,
      segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, level, 0U), result->next_pc,
      &handler_entry);
  return genesis_finish_exception_entry(status, context.frame_write_failed, handler_entry,
                                        GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                        GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT, result);
}

static int genesis_irq6_scheduler_and_admit(GenesisRuntime *runtime, uint32_t m68k_cycles,
                                            int admit_interrupt, GenesisControlTransfer *result);

/*
 * The synchronous (never scheduled, never masked) exception raise shared by
 * vectors 4-8, 10, 11 and 32-47: builds the frame through the core and completes
 * the faulting instruction's diagnostic boundary (the generated lowering returns
 * the handler transfer directly and never reaches the retirement path).
 * SEG-021-T022 / ADR 0043 §8: after the entry commits, the CPU-owned
 * exception-processing time `entry_cycles` advances the deterministic scheduler
 * (ADR 0041) exactly as a retirement would, but admits no interrupt at this
 * boundary (a request latched here is admitted at the next retirement boundary,
 * as before). A zero count is refused before anything changes.
 */
static int genesis_raise_synchronous_exception(GenesisRuntime *runtime, uint32_t vector, uint32_t stacked_pc,
                                               uint32_t entry_cycles, GenesisStopClass fail_stop_class,
                                               GenesisDiagnosticCategory fail_diag, uint32_t *handler_pc_out,
                                               GenesisRuntimeStop *stop_out) {
  GenesisControlTransfer result = {0};
  if (runtime == 0 || handler_pc_out == 0 || stop_out == 0) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  if (entry_cycles == 0U) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                    GENESIS_DIAG_UNACCOUNTED_INSTRUCTION_TIMING);
    return 0;
  }
  if (!genesis_construct_exception_frame_and_transfer(runtime, vector, stacked_pc, UINT16_C(0x7FFF), UINT16_C(0x2000),
                                                      fail_stop_class, fail_diag, &result)) {
    *stop_out = result.stop;
    return 0;
  }
  *handler_pc_out = result.next_pc;
  /* SEG-021-T022: charge the entry (no admission). Only a virtual-time overflow can fail here, after the committed
     entry; it is a terminal stop like every other scheduler overflow. */
  {
    GenesisControlTransfer charge = {0};
    if (genesis_irq6_scheduler_and_admit(runtime, entry_cycles, 0, &charge) == 2) {
      *stop_out = charge.stop;
      return 0;
    }
  }
  /* SEG-020-T004: runtime->pc is already the handler entry, the frame writes and trap
     effect are pending; the faulting instruction's diagnostic boundary completes here
     (exactly once). No retirement or IRQ admission. */
  if (runtime->m68k_checkpoint.enabled) genesis_m68k_checkpoint_finalize(runtime);
  return 1;
}

/*
 * SEG-007-T222 / ADR-0037: synchronous, unmasked, never-scheduled divide-by-
 * zero (vector 5) exception raise. Called directly from generated DIVS.W/
 * DIVU.W lowering when the divisor is zero -- NOT gated by the SR interrupt
 * mask, NOT admitted at any scheduler/dispatch boundary, and does not consume
 * or arm any IRQ6-only admission-grace/watchdog-credit state.
 */
int genesis_raise_divide_by_zero(GenesisRuntime *runtime, uint32_t fault_pc, uint32_t entry_cycles,
                                 uint32_t *handler_pc_out, GenesisRuntimeStop *stop_out) {
  return genesis_raise_synchronous_exception(runtime, GENESIS_M68K_VECTOR_ZERO_DIVIDE, fault_pc, entry_cycles,
                                             GENESIS_STOP_UNSUPPORTED_CPU_FORM,
                                             GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION, handler_pc_out,
                                             stop_out);
}

/*
 * SEG-021-T018 / ADR 0043 §3: privilege violation (vector 8), raised by the
 * generated lowering of a privileged instruction executed with SR.S = 0.
 * `fault_pc` is the address of the privileged instruction itself; nothing of
 * that instruction has executed. The saved SR has S = 0; the frame goes on the
 * SSP (the inactive slot) and the USP moves to the inactive slot.
 */
int genesis_raise_privilege_violation(GenesisRuntime *runtime, uint32_t fault_pc, uint32_t entry_cycles,
                                      uint32_t *handler_pc_out, GenesisRuntimeStop *stop_out) {
  return genesis_raise_synchronous_exception(runtime, GENESIS_M68K_VECTOR_PRIVILEGE_VIOLATION, fault_pc, entry_cycles,
                                             GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION,
                                             GENESIS_DIAG_UNSUPPORTED_PRIVILEGE_VIOLATION_EXCEPTION, handler_pc_out,
                                             stop_out);
}

/*
 * SEG-021-T019 / ADR 0043 §3: the software exceptions (TRAP #n, TRAPV, CHK,
 * ILLEGAL, line 1010/1111 and every other illegal word) share the synchronous
 * raise; only the build-time vector and stacked PC differ.
 */
int genesis_raise_software_exception(GenesisRuntime *runtime, uint32_t vector, uint32_t stacked_pc,
                                     uint32_t entry_cycles, uint32_t *handler_pc_out, GenesisRuntimeStop *stop_out) {
  if (!genesis_is_software_exception_vector(vector)) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  return genesis_raise_synchronous_exception(runtime, vector, stacked_pc, entry_cycles,
                                             GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION,
                                             GENESIS_DIAG_UNSUPPORTED_SOFTWARE_EXCEPTION, handler_pc_out, stop_out);
}

/*
 * SEG-021-T019 / ADR 0043 §5: RTR through the M68K-owned frame-return core
 * (validated routed reads, atomic CCR/PC/SP commit, no privilege check and no
 * exception-return notification: RTR is not an exception return).
 */
int genesis_return_restore_condition_codes(GenesisRuntime *runtime, uint32_t *restored_pc_out,
                                           GenesisRuntimeStop *stop_out) {
  GenesisM68kExceptionContext context = {0};
  SegarecompM68kMachineHooks hooks;
  SegarecompM68kCpuBinding cpu;
  SegarecompM68kExceptionStatus status;
  if (runtime == 0 || restored_pc_out == 0 || stop_out == 0) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  context.runtime = runtime;
  hooks = genesis_m68k_exception_hooks(&context);
  cpu = genesis_m68k_cpu_binding(runtime);
  status = segarecomp_m68k_return_restore_ccr(&hooks, &cpu, restored_pc_out);
  switch (status) {
  case SEGARECOMP_M68K_EXCEPTION_OK: return 1;
  case SEGARECOMP_M68K_EXCEPTION_STACK_INVALID:
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    (runtime->a[7] & 1U) != 0U ? GENESIS_DIAG_INVALID_STACK_ALIGNMENT
                                                                : GENESIS_DIAG_INVALID_STACK_RANGE);
    return 0;
  case SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED:
    *stop_out = context.routed;
    return 0;
  case SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED:
  case SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE:
  case SEGARECOMP_M68K_EXCEPTION_BAD_BINDING:
    break;
  }
  *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                  GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
  return 0;
}

/*
 * SEG-007-T047 / ADR-0020 §5 steps 1-4, §7, §8. Advances the device scheduler,
 * then, when an eligible VBlank IRQ6 is pending, constructs the six-byte basic
 * MC68000 exception frame (validate-then-commit, no partial write) and delivers
 * it by overriding `runtime->pc` / `result->next_pc` -- reusing the existing
 * control-transfer mechanism, no second dispatcher.
 *
 * Returns 0 = nothing admitted (caller proceeds normally);
 *         1 = interrupt delivered (`genesis_runtime_step` proceeds normally;
 *             `result` already reflects the handler entry as the next PC --
 *             there is no progress-credit/accounting concept left to update);
 *         2 = admission failed closed (`*result` holds the GENESIS_STOP).
 * `admit_interrupt == 0` still advances the scheduler and latches a newly
 * pending VBlank edge, but leaves every pending IRQ unadmitted.
 */
static int genesis_irq6_scheduler_and_admit(GenesisRuntime *runtime, uint32_t m68k_cycles,
                                            int admit_interrupt, GenesisControlTransfer *result) {
  uint32_t level;
  const uint64_t before = runtime->scheduler.master_ticks;
  const uint64_t delta = (uint64_t)m68k_cycles * GENESIS_M68K_CYCLE_MASTER_TICKS;
  const uint64_t frame = GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  const uint64_t onset = GENESIS_NTSC_VBLANK_ONSET_TICK;
  int crosses_onset;
  if (m68k_cycles == 0U || UINT64_MAX - before < delta) {
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                       m68k_cycles == 0U ? GENESIS_DIAG_UNACCOUNTED_INSTRUCTION_TIMING
                                                                 : GENESIS_DIAG_VIRTUAL_TIME_OVERFLOW);
    return 2;
  }
  crosses_onset = before < onset ? before + delta >= onset
                                : (before - onset) / frame != (before + delta - onset) / frame;
  if (crosses_onset && (runtime->devices.vdp.registers[1] & UINT16_C(0x0020)) != 0U &&
      !runtime->devices.interrupt.vblank_pending &&
      runtime->devices.interrupt.vblank_transition_count == UINT32_MAX) {
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                       GENESIS_DIAG_VIRTUAL_TIME_OVERFLOW);
    return 2;
  }
  runtime->scheduler.master_ticks = before + delta;
  /* SEG-032-T005: the Z80 follows the leader. It is synchronized once the 68K clock has advanced a quantum past the last
     synchronization (default 512 master ticks, about 34 Z80 cycles); every interaction with Z80-domain state synchronizes
     first, so results never depend on this cadence (contract section 10, tested at three cadences). */
  if (runtime->z80_hooks != 0 && runtime->z80_hooks->run_to != 0) {
    const uint64_t quantum = runtime->z80_sync_quantum != 0U ? runtime->z80_sync_quantum : UINT64_C(512);
    if (runtime->scheduler.master_ticks - runtime->z80_synced_ticks >= quantum &&
        genesis_z80_sync(runtime, &result->stop)) {
      result->kind = GENESIS_STOP;
      return 2;
    }
  }
  /* An onset is crossed iff floor((t-onset)/frame) changes.  The addition is
     checked above, so no wrapped phase can manufacture an event. */
  if (crosses_onset) {
    /* SEG-007-T255: a genuine virtual frame boundary (never IRQ/IE0/SR/pending
       dependent). Render the current VDP state into a local artifact and
       publish it atomically; failure publishes nothing and mutates nothing. */
    GenesisLiveFrameObserver *observer = runtime->live_frame_observer;
    if (observer != 0 && observer->producer != 0 && observer->latest != 0 &&
        observer->sequence != UINT64_MAX) {
      GenesisFrameArtifact produced;
      memset(&produced, 0, sizeof(produced));
      if (observer->producer(runtime->devices.vdp.vram, runtime->devices.vdp.vsram,
                             runtime->devices.vdp.cram, runtime->devices.vdp.registers, &produced) == 0) {
        *observer->latest = produced;
        ++observer->sequence;
      }
    }
    /* §3: VDP register #1 bit 5 (IE0) gating -- the real, already-persisted
       register state, mirroring the existing DMA-enable (bit 4) precedent. */
    if ((runtime->devices.vdp.registers[1] & UINT16_C(0x0020)) != 0U &&
        !runtime->devices.interrupt.vblank_pending) {
      runtime->devices.interrupt.vblank_pending = 1U;              /* 0 -> 1 rising edge */
      ++runtime->devices.interrupt.vblank_transition_count;        /* T042 §13.2 rule */
    }
  }

  /* SEG-021-T020 / ADR 0043 §7: this boundary samples the machine's request
     level into the M68K-owned CPU record. Genesis wires one source: the
     pending VBlank requests level 6 (autovectored). */
  segarecomp_m68k_interrupt_sample(&runtime->m68k_interrupt,
                                   runtime->devices.interrupt.vblank_pending ? 6U : 0U);
  if (!admit_interrupt) return 0;                                  /* §5 step 4 */

  /* §4 / ADR 0043: the CPU-owned acceptance rule (level > SR mask for 1-6;
     level 7 transition-sensitive). A masked request stays pending (§5 step 3). */
  level = segarecomp_m68k_interrupt_recognized_level(&runtime->m68k_interrupt, runtime->sr);
  if (level == 0U) return 0;

  if (!runtime->irq6_handler_present) {                            /* no build-resolved handler */
    if (runtime->irq6_vector_in_work_ram) {
      /* The vector slot points at work-RAM (a RAM jump stub), which this architecture cannot execute: the recognized
         interrupt cannot be delivered, so stop fail-closed rather than silently dropping it. Nothing is mutated. */
      *result = (GenesisControlTransfer){0};
      result->kind = GENESIS_STOP;
      result->stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                         GENESIS_DIAG_IRQ6_VECTOR_IN_WORK_RAM);
      return 2;
    }
    return 0;
  }

  /* SEG-007-T222 / ADR-0037, SEG-021-T020: the frame and commit go through the
     M68K-owned interrupt entry (identical checks/order/commit: vector 30, SR <-
     (SR & 0x78FF) | 0x2600, the next instruction stacked); only the
     IRQ6-specific `vblank_pending` re-arm (§3) stays here, performed after a
     successful commit, exactly as before. */
  if (!genesis_accept_interrupt(runtime, level, result)) return 2;
  runtime->devices.interrupt.vblank_pending = 0U;                  /* §3 re-arm (admission clears) */
  /* SEG-021-T022 / ADR 0043 §8: the interrupt exception-processing cost -- 44 cycles for the autovectored entry, CPU-owned --
     advances the same scheduler after the entry commits, admitting nothing at that boundary; a VBlank onset
     crossed while it is charged latches normally and is admitted at the next retirement boundary. */
  {
    const uint32_t entry_cycles = segarecomp_m68k_exception_entry_cycles(
        segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, level, 0U));
    GenesisControlTransfer charge = {0};
    if (genesis_irq6_scheduler_and_admit(runtime, entry_cycles, 0, &charge) == 2) {
      result->kind = GENESIS_STOP;
      result->stop = charge.stop;
      return 2;
    }
  }
  return 1;
}

/*
 * SEG-021-T020 / ADR 0043 §7, ADR 0041: the Genesis `wait_while_stopped`
 * scheduler hook. Runs at the retirement boundary of a STOP whose own boundary
 * admitted nothing. The CPU executes no instruction while stopped, so nothing
 * but virtual time can change the machine's interrupt sources: the only wired
 * source is the VBlank level-6 request, which can wake the CPU iff its handler
 * is installed, it is already pending or VDP register 1 IE0 is set, and the
 * mask STOP loaded is below 6 (segarecomp_m68k_stop_wake_possible). If it can,
 * virtual time advances -- in whole CPU cycles, through the same scheduler --
 * to the first whole-cycle boundary at or after the next VBlank onset (at most
 * one CPU cycle late), where the request latches and is accepted
 * with the instruction after STOP stacked. Otherwise the run ends with the
 * explicit stopped_without_wake_source diagnostic instead of spinning.
 */
static void genesis_m68k_wait_while_stopped(GenesisRuntime *runtime, GenesisControlTransfer *result) {
  const uint64_t now = runtime->scheduler.master_ticks;
  const uint64_t frame = GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  const uint64_t onset = GENESIS_NTSC_VBLANK_ONSET_TICK;
  const int vblank_source = (runtime->irq6_handler_present || runtime->irq6_vector_in_work_ram) &&
                            (runtime->devices.interrupt.vblank_pending ||
                             (runtime->devices.vdp.registers[1] & UINT16_C(0x0020)) != 0U);
  uint64_t target;
  uint64_t ticks;
  if (!segarecomp_m68k_stop_wake_possible(&runtime->m68k_interrupt, vblank_source ? 6U : 0U, runtime->sr)) {
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
                                       GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE);
    return;
  }
  /* The next onset strictly after `now` (an onset at exactly `now` was already
     crossed by the advance that reached it). */
  target = now < onset ? onset : onset + ((now - onset) / frame + 1U) * frame;
  ticks = target - now;
  if (genesis_irq6_scheduler_and_admit(
          runtime, (uint32_t)((ticks + GENESIS_M68K_CYCLE_MASTER_TICKS - 1U) / GENESIS_M68K_CYCLE_MASTER_TICKS), 1,
          result) == 0) {
    /* Unreachable by the wake condition above; never spin. */
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                       GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
  }
}

void genesis_m68k_enter_stopped_state(GenesisRuntime *runtime) {
  if (runtime != 0) runtime->m68k_interrupt.stopped = 1U;
}

/* SEG-026-T001: execution-PC coverage (see GenesisExecutionCoverage in runtime.h). Host-side bookkeeping
   only: these functions read nothing but their own record and the PC values passed in. */
int genesis_execution_coverage_contains(const GenesisExecutionCoverage *coverage, uint32_t pc) {
  const uint32_t bus = pc & UINT32_C(0x00FFFFFF);
  if (coverage == 0 || coverage->bitmap == 0 || (bus & 1U) != 0U) return 0;
  return (coverage->bitmap[bus >> 4U] >> ((bus >> 1U) & 7U)) & 1U;
}

static void genesis_execution_coverage_dispatch(GenesisExecutionCoverage *coverage, uint32_t pc) {
  if (coverage->has_current && coverage->current_pc == pc) return; /* the retirement successor, as tracked */
  coverage->current_pc = pc;
  coverage->current_cause =
      (uint8_t)(coverage->has_last_retired ? GENESIS_COVERAGE_CAUSE_DISPATCH : GENESIS_COVERAGE_CAUSE_INITIAL);
  coverage->has_current = 1U;
}

static void genesis_execution_coverage_retire(GenesisExecutionCoverage *coverage) {
  uint32_t pc;
  uint32_t bus;
  uint8_t *byte;
  uint8_t bit;
  if (!coverage->has_current) {
    ++coverage->unknown_retirement_count;
    return;
  }
  pc = coverage->current_pc;
  bus = pc & UINT32_C(0x00FFFFFF);
  if (bus != pc) ++coverage->wide_pc_count;
  if ((bus & 1U) != 0U) {
    ++coverage->odd_pc_count;
  } else if (coverage->bitmap != 0) {
    byte = &coverage->bitmap[bus >> 4U];
    bit = (uint8_t)(1U << ((bus >> 1U) & 7U));
    if ((*byte & bit) == 0U) {
      *byte = (uint8_t)(*byte | bit);
      ++coverage->distinct_count;
      if (coverage->witnesses != 0 && coverage->witness_count < coverage->witness_capacity) {
        GenesisExecutionCoverageWitness *witness = &coverage->witnesses[coverage->witness_count++];
        witness->retirement_ordinal = coverage->retirement_count;
        witness->previous_pc = coverage->current_cause == GENESIS_COVERAGE_CAUSE_INTERRUPT_RESUMPTION
                                   ? coverage->current_predecessor
                                   : (coverage->has_last_retired ? coverage->last_retired_pc : 0U);
        witness->pc = pc;
        witness->cause = coverage->has_last_retired ? coverage->current_cause : (uint8_t)GENESIS_COVERAGE_CAUSE_INITIAL;
      } else if (coverage->witnesses != 0) {
        ++coverage->witness_overflow;
      }
    }
  }
  ++coverage->retirement_count;
  coverage->last_retired_pc = pc;
  coverage->has_last_retired = 1U;
  coverage->has_current = 0U;
}

static void genesis_execution_coverage_successor(GenesisExecutionCoverage *coverage, int continues,
                                                 uint32_t successor, uint32_t unredirected_successor) {
  const int redirected = successor != unredirected_successor;
  if (!continues) return;
  coverage->current_pc = successor;
  coverage->current_cause =
      (uint8_t)(redirected ? GENESIS_COVERAGE_CAUSE_INTERRUPT_ENTRY : GENESIS_COVERAGE_CAUSE_RETIRE_SUCCESSOR);
  coverage->has_current = 1U;
  if (redirected) {
    ++coverage->interrupt_redirects;
    if (coverage->interrupt_depth == GENESIS_EXECUTION_COVERAGE_INTERRUPT_DEPTH) {
      /* Bounded: drop the oldest pending resumption (a handler that never returned). */
      unsigned index;
      for (index = 1U; index < GENESIS_EXECUTION_COVERAGE_INTERRUPT_DEPTH; ++index) {
        coverage->interrupted_successor[index - 1U] = coverage->interrupted_successor[index];
        coverage->interrupted_predecessor[index - 1U] = coverage->interrupted_predecessor[index];
      }
      --coverage->interrupt_depth;
      ++coverage->interrupt_depth_overflow;
    }
    coverage->interrupted_successor[coverage->interrupt_depth] = unredirected_successor;
    coverage->interrupted_predecessor[coverage->interrupt_depth] = coverage->last_retired_pc;
    ++coverage->interrupt_depth;
  } else if (coverage->interrupt_depth != 0U &&
             coverage->interrupted_successor[coverage->interrupt_depth - 1U] == successor) {
    --coverage->interrupt_depth;
    coverage->current_cause = (uint8_t)GENESIS_COVERAGE_CAUSE_INTERRUPT_RESUMPTION;
    coverage->current_predecessor = coverage->interrupted_predecessor[coverage->interrupt_depth];
    ++coverage->interrupt_resumptions;
  }
}

/*
 * SEG-007-T252 / ADR-0040: the guest-owned runtime step/boundary contract.
 * Performs exactly one dispatch step -- the SEG-007-T175 defensive VDP DMA
 * drain, one dispatch() call, T131's checkpoint-class observation, the
 * SEG-007-T047 / ADR-0020 §5 device-scheduler tick and SR-masked IRQ6
 * admission -- and returns immediately with whatever GenesisControlTransfer
 * results. It carries NO notion of "no progress": it never reads or writes
 * any progress-credit/watchdog state (there is none left to read), and it
 * never loops. `result.kind` is always GENESIS_STOP or GENESIS_COMPLETE on
 * those genuine guest outcomes, else GENESIS_CONTINUE_AT_PC with
 * `runtime->pc` already advanced (and possibly overridden by an admitted
 * IRQ6 exception's handler entry) after exactly one guest step.
 */
GenesisControlTransfer genesis_runtime_step(GenesisRuntime *runtime, GenesisDispatchFunction dispatch) {
  GenesisControlTransfer result = {0};
  if (runtime == 0 || dispatch == 0)
    return genesis_internal_dispatch_inconsistency_stop(runtime);
  /* SEG-007-T252 / ADR-0040 correction: record the PC-about-to-dispatch value
     into the bounded diagnostic-only circular history, at this single
     documented boundary, before dispatch() runs. Pure side-channel append --
     no guest state, dispatch selection, or timing is read from or affected by
     this recording. */
  genesis_history_append(runtime, GENESIS_HISTORY_DISPATCH, runtime->pc, 0U, 0U, 0U, 0U, 0U);
  if (runtime->execution_coverage != 0) genesis_execution_coverage_dispatch(runtime->execution_coverage, runtime->pc);
  /* SEG-007-T175 DEFENSIVE mechanism only (see the PRIMARY synchronous
     drain documented above genesis_vdp_drain_memory_to_vdp_dma_body /
     inside genesis_route_access's VDP branch, which now drains a
     memory-to-VDP DMA to GENESIS_VDP_DMA_IDLE the instant it is armed,
     before the arming access itself ever returns to generated C). Every
     reachable arm site already drains synchronously, so this pre-dispatch
     check should never actually observe BUSY in practice; it remains only
     as a guard against an already-BUSY runtime state, never as the sole
     suspension mechanism. It must still precede the dispatch() call below,
     not follow it. */
  if (!genesis_vdp_drain_memory_to_vdp_dma(runtime, &result)) return result;
  result = dispatch(runtime);
  if (result.kind != GENESIS_CONTINUE_AT_PC) return result;
  /* T131 deliberately defines only UNKNOWN, so this pure resolved-target
   * classifier is presently a no-op.  Keeping it here makes a later,
   * cited class an additive classification rather than a second dispatcher. */
  if (!runtime->devices.interrupt.checkpoint_entered &&
      genesis_checkpoint_pc_class_for_target(result.next_pc) != GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN) {
    runtime->devices.interrupt.checkpoint_entered = 1U;
    runtime->devices.interrupt.vblank_transition_count_at_checkpoint_entry =
        runtime->devices.interrupt.vblank_transition_count;
  }
  runtime->pc = result.next_pc;
  return result;
}

static GenesisControlTransfer genesis_runtime_retire_m68k_instruction_impl(
    GenesisRuntime *runtime, uint32_t m68k_cycles, uint32_t next_pc,
    const GenesisControlTransfer *pending_stop) {
  GenesisControlTransfer result = {0};
  const int stop_pending = pending_stop != 0;
  if (runtime == 0 || (stop_pending && pending_stop->kind != GENESIS_STOP))
    return genesis_internal_dispatch_inconsistency_stop(runtime);
  if (runtime->execution_coverage != 0) genesis_execution_coverage_retire(runtime->execution_coverage);
  result.kind = GENESIS_CONTINUE_AT_PC;
  result.next_pc = next_pc;
  runtime->pc = next_pc;
  (void)genesis_irq6_scheduler_and_admit(runtime, m68k_cycles, !stop_pending, &result);
  /* SEG-021-T020: a STOP whose own boundary accepted nothing waits here. */
  if (!stop_pending && result.kind == GENESIS_CONTINUE_AT_PC && runtime->m68k_interrupt.stopped)
    genesis_m68k_wait_while_stopped(runtime, &result);
  if (runtime->m68k_checkpoint.enabled) genesis_m68k_checkpoint_finalize(runtime);
  if (runtime->execution_coverage != 0)
    genesis_execution_coverage_successor(runtime->execution_coverage,
                                         !stop_pending && result.kind == GENESIS_CONTINUE_AT_PC, result.next_pc,
                                         next_pc);
  if (stop_pending && result.kind == GENESIS_CONTINUE_AT_PC) return *pending_stop;
  return result;
}

GenesisControlTransfer genesis_runtime_retire_m68k_instruction(GenesisRuntime *runtime,
                                                                uint32_t m68k_cycles,
                                                                uint32_t next_pc) {
  return genesis_runtime_retire_m68k_instruction_impl(runtime, m68k_cycles, next_pc, 0);
}

GenesisControlTransfer genesis_runtime_retire_m68k_instruction_before_stop(
    GenesisRuntime *runtime, uint32_t m68k_cycles, uint32_t next_pc,
    const GenesisControlTransfer *pending_stop) {
  if (runtime == 0 || pending_stop == 0 || pending_stop->kind != GENESIS_STOP)
    return genesis_internal_dispatch_inconsistency_stop(runtime);
  return genesis_runtime_retire_m68k_instruction_impl(runtime, m68k_cycles, next_pc, pending_stop);
}

GenesisControlTransfer genesis_runtime_retire_m68k_instruction_at(GenesisRuntime *runtime, uint32_t retired_pc,
                                                                  uint32_t fallthrough_pc,
                                                                  GenesisHistoryTransferKind transfer_kind,
                                                                  uint32_t m68k_cycles, uint32_t next_pc) {
  if (runtime != 0 && runtime->execution_history.detail_enabled) {
    if (transfer_kind != GENESIS_HISTORY_TRANSFER_NONE && next_pc != fallthrough_pc)
      genesis_history_append(runtime, GENESIS_HISTORY_TRANSFER, 0U, next_pc, (uint8_t)transfer_kind, 0U, 0U, 0U);
    genesis_history_append(runtime, GENESIS_HISTORY_RETIRED, retired_pc, next_pc, 0U, 0U, 0U, 0U);
    ++runtime->execution_history.retired_count;
  }
  return genesis_runtime_retire_m68k_instruction(runtime, m68k_cycles, next_pc);
}

GenesisControlTransfer genesis_runtime_retire_m68k_instruction_at_before_stop(
    GenesisRuntime *runtime, uint32_t retired_pc, uint32_t fallthrough_pc,
    GenesisHistoryTransferKind transfer_kind, uint32_t m68k_cycles, uint32_t next_pc,
    const GenesisControlTransfer *pending_stop) {
  if (runtime == 0 || pending_stop == 0 || pending_stop->kind != GENESIS_STOP)
    return genesis_internal_dispatch_inconsistency_stop(runtime);
  if (runtime != 0 && runtime->execution_history.detail_enabled) {
    if (transfer_kind != GENESIS_HISTORY_TRANSFER_NONE && next_pc != fallthrough_pc)
      genesis_history_append(runtime, GENESIS_HISTORY_TRANSFER, 0U, next_pc, (uint8_t)transfer_kind, 0U, 0U, 0U);
    genesis_history_append(runtime, GENESIS_HISTORY_RETIRED, retired_pc, next_pc, 0U, 0U, 0U, 0U);
    ++runtime->execution_history.retired_count;
  }
  return genesis_runtime_retire_m68k_instruction_impl(runtime, m68k_cycles, next_pc, pending_stop);
}

/*
 * SEG-007-T252 / ADR-0040: the runner-owned finite dispatch allowance. This
 * is the sole caller-visible repetition mechanism left in this runtime: it
 * calls genesis_runtime_step up to `dispatch_allowance` times, stopping
 * immediately on any genuine guest GENESIS_STOP/GENESIS_COMPLETE. It performs
 * NO guest-semantic reasoning of any kind -- no loop/data-transform proof
 * consumption, no progress credit, no per-instance slot -- because none of
 * that exists any more; it is purely a deterministic, overflow-safe upper
 * bound on how many guest steps this one invocation may take. If the
 * allowance is exhausted while the guest is still GENESIS_CONTINUE_AT_PC,
 * this returns the new, disjoint GENESIS_RUNNER_RESOURCE_LIMIT outcome --
 * never the guest GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED stop. This
 * translation unit contains no wall-clock read or sleep call of any kind
 * (headless/automated execution is full-speed by construction); see
 * tests/genesis_runtime_run_no_wallclock_test.cpp.
 */
void genesis_runtime_set_pad1(GenesisRuntime *runtime, uint8_t mask) {
  if (runtime != NULL) runtime->pad1 = mask;
}

void genesis_cartridge_sram_install(GenesisRuntime *runtime, const GenesisCartridgeSramConfig *config, uint8_t *storage) {
  uint32_t index;
  if (runtime == 0 || config == 0) return;
  runtime->cartridge_sram = config;
  runtime->cartridge_sram_storage = storage;
  runtime->cartridge_sram_control = 0U;
  if (storage != 0)
    for (index = 0U; index < config->storage_bytes; ++index) storage[index] = SEGARECOMP_GENESIS_CARTRIDGE_SRAM_FILL;
}

GenesisControlTransfer genesis_runtime_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                           uint32_t dispatch_allowance) {
  GenesisControlTransfer result = {0};
  uint32_t step;
#if defined(_WIN32)
  /* SEG-018-T006: report bytes are canonical ("\n" only) and hashed/compared
   * byte-exact; the Windows CRT's default text mode would emit "\r\n". */
  _set_fmode(_O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
  if (runtime == 0 || dispatch == 0 || dispatch_allowance == 0U)
    return genesis_internal_dispatch_inconsistency_stop(runtime);
  for (step = 0U; step < dispatch_allowance; ++step) {
    result = genesis_runtime_step(runtime, dispatch);
    if (result.kind != GENESIS_CONTINUE_AT_PC) return result;
  }
  {
    GenesisControlTransfer exhausted = {0};
    exhausted.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    exhausted.next_pc = runtime->pc;
    /* `.stop` is left fully zeroed (GenesisRuntimeStop{0}) so a caller that
       inspects `.stop.stop_class` without first switching on `.kind` cannot
       mistake this for any real guest stop -- zero is not a valid
       GenesisStopClass value. `runner_dispatch_count` is the deterministic
       count of guest steps actually taken before exhaustion (always equal to
       `dispatch_allowance` here, since every genuine guest stop/completion
       already returned above); it is derived only from the runner's own
       finite loop counter, never from any guest state. */
    exhausted.runner_dispatch_count = dispatch_allowance;
    return exhausted;
  }
}

/* Small local SHA-256 implementation for evidence digests.  It consumes only
 * caller/runtime bytes; it has no CPU, device, or rendering semantics.
 * GenesisSha256 and genesis_sha256_init/update/final are declared in
 * runtime.h so other translation units (e.g. vdp_render.c's checkpoint C5
 * frame-digest computation) can reuse this exact implementation instead of
 * duplicating a second SHA-256. */
static uint32_t genesis_ror32(uint32_t value, uint32_t count) {
  return (value >> count) | (value << (32U - count));
}
static void genesis_sha256_block(GenesisSha256 *state, const uint8_t *block) {
  static const uint32_t k[64] = { 0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U };
  uint32_t w[64], a, b, c, d, e, f, g, h, i;
  for (i = 0U; i < 16U; ++i) w[i] = ((uint32_t)block[i * 4U] << 24U) | ((uint32_t)block[i * 4U + 1U] << 16U) | ((uint32_t)block[i * 4U + 2U] << 8U) | block[i * 4U + 3U];
  for (; i < 64U; ++i) w[i] = w[i-16U] + (genesis_ror32(w[i-15U],7U)^genesis_ror32(w[i-15U],18U)^(w[i-15U]>>3U)) + w[i-7U] + (genesis_ror32(w[i-2U],17U)^genesis_ror32(w[i-2U],19U)^(w[i-2U]>>10U));
  a=state->h[0]; b=state->h[1]; c=state->h[2]; d=state->h[3]; e=state->h[4]; f=state->h[5]; g=state->h[6]; h=state->h[7];
  for (i=0U;i<64U;++i) { uint32_t t1=h+(genesis_ror32(e,6U)^genesis_ror32(e,11U)^genesis_ror32(e,25U))+((e&f)^((~e)&g))+k[i]+w[i]; uint32_t t2=(genesis_ror32(a,2U)^genesis_ror32(a,13U)^genesis_ror32(a,22U))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
  state->h[0]+=a;state->h[1]+=b;state->h[2]+=c;state->h[3]+=d;state->h[4]+=e;state->h[5]+=f;state->h[6]+=g;state->h[7]+=h;
}
void genesis_sha256_update(GenesisSha256 *state, const uint8_t *data, uint32_t count) {
  uint32_t take;
  while (count != 0U) { take = 64U - state->used; if (take > count) take = count; memcpy(state->block + state->used, data, take); state->used += take; data += take; count -= take; state->length += take; if (state->used == 64U) { genesis_sha256_block(state, state->block); state->used = 0U; } }
}
void genesis_sha256_init(GenesisSha256 *state) {
  *state = (GenesisSha256){{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                             0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U},0U,{0},0U};
}
void genesis_sha256_final(GenesisSha256 *state, uint8_t output[32]) {
  uint64_t bits; uint32_t i;
  state->block[state->used++] = 0x80U; if (state->used > 56U) { while (state->used < 64U) state->block[state->used++] = 0U; genesis_sha256_block(state,state->block); state->used=0U; } while (state->used < 56U) state->block[state->used++] = 0U; bits=state->length*8U; for(i=0U;i<8U;++i) state->block[63U-i]=(uint8_t)(bits>>(i*8U)); genesis_sha256_block(state,state->block); for(i=0U;i<8U;++i) { output[i*4U]=(uint8_t)(state->h[i]>>24U);output[i*4U+1U]=(uint8_t)(state->h[i]>>16U);output[i*4U+2U]=(uint8_t)(state->h[i]>>8U);output[i*4U+3U]=(uint8_t)state->h[i]; }
}

static void genesis_sha_u8(GenesisSha256 *state, uint8_t value) { genesis_sha256_update(state, &value, 1U); }
static void genesis_sha_u16(GenesisSha256 *state, uint16_t value) { uint8_t b[2] = {(uint8_t)(value >> 8U), (uint8_t)value}; genesis_sha256_update(state,b,2U); }
static void genesis_sha_u32(GenesisSha256 *state, uint32_t value) { uint8_t b[4] = {(uint8_t)(value >> 24U),(uint8_t)(value >> 16U),(uint8_t)(value >> 8U),(uint8_t)value}; genesis_sha256_update(state,b,4U); }
static void genesis_sha_u64(GenesisSha256 *state, uint64_t value) { genesis_sha_u32(state,(uint32_t)(value >> 32U)); genesis_sha_u32(state,(uint32_t)value); }
static int genesis_valid_bus_access(const GenesisBusAccess *access) {
  return access->raw_byte_count <= GENESIS_MAX_RAW_BYTES &&
         (access->address & UINT32_C(0xFF000000)) == 0U &&
         access->kind >= GENESIS_BUS_INSTRUCTION_READ && access->kind <= GENESIS_BUS_STACK_WRITE &&
         access->region >= GENESIS_REGION_RAW_CARTRIDGE_ROM &&
         access->region <= GENESIS_REGION_SYNTHETIC_WORK_RAM;
}
static void genesis_sha_bus_access(GenesisSha256 *state, const GenesisBusAccess *access) {
  genesis_sha_u64(state,access->ordinal); genesis_sha_u8(state,(uint8_t)access->kind); genesis_sha_u32(state,access->address); genesis_sha_u8(state,access->raw_byte_count); genesis_sha256_update(state,access->raw_bytes,access->raw_byte_count); genesis_sha_u8(state,(uint8_t)access->region);
}
static void genesis_sha_options(GenesisSha256 *state, const GenesisDeterministicOptions *options) { genesis_sha_u32(state,options->schema_version); genesis_sha_u32(state,options->instruction_budget); genesis_sha_u32(state,options->stable_frame_vblank_count); }
static void genesis_sha_device(GenesisSha256 *s, const GenesisDeviceState *d) {
  uint32_t i; genesis_sha_u8(s,d->z80_bus.bus_requested); genesis_sha_u8(s,d->z80_bus.bus_granted); genesis_sha_u8(s,d->z80_bus.reset_released); genesis_sha_u8(s,(uint8_t)(d->z80_bus.bank >> 8)); genesis_sha_u8(s,(uint8_t)d->z80_bus.bank); genesis_sha256_update(s,d->z80_bus.z80_ram,GENESIS_Z80_RAM_BYTES);
  for(i=0U;i<GENESIS_VDP_REGISTER_COUNT;++i) { genesis_sha_u16(s,d->vdp.registers[i]); }
  genesis_sha_u8(s,d->vdp.control_port_awaiting_second_word); genesis_sha_u16(s,d->vdp.control_port_first_word); genesis_sha_u32(s,d->vdp.addressed_pointer); genesis_sha_u16(s,d->vdp.auto_increment_value); genesis_sha_u16(s,d->vdp.status_register); genesis_sha_u8(s,d->vdp.data_port_transfer_code); genesis_sha_u8(s,d->vdp.data_port_transfer_code_valid); genesis_sha256_update(s,d->vdp.vram,GENESIS_VDP_VRAM_BYTES); genesis_sha256_update(s,d->vdp.cram,GENESIS_VDP_CRAM_BYTES); genesis_sha256_update(s,d->vdp.vsram,GENESIS_VDP_VSRAM_BYTES); genesis_sha_u8(s,(uint8_t)d->vdp.dma.phase); genesis_sha_u8(s,(uint8_t)d->vdp.dma.kind); genesis_sha_u32(s,d->vdp.dma.source_address); genesis_sha_u32(s,d->vdp.dma.remaining_length); genesis_sha_u32(s,d->vdp.dma.fill_byte_count); genesis_sha_u32(s,d->vdp.dma.transfer_access_count); genesis_sha_u8(s,d->vdp.dma.write_target_code);
  genesis_sha_u32(s,d->psg.write_count); genesis_sha_u32(s,d->psg.write_digest); genesis_sha256_update(s,d->controller_io.data,3U); genesis_sha256_update(s,d->controller_io.ctrl,3U); genesis_sha_u8(s,d->interrupt.vblank_pending); genesis_sha_u32(s,d->interrupt.vblank_status_read_count); genesis_sha_u32(s,d->interrupt.vblank_transition_count); genesis_sha_u8(s,d->interrupt.checkpoint_entered); genesis_sha_u32(s,d->interrupt.vblank_transition_count_at_checkpoint_entry);
}

int genesis_extract_checkpoint_evidence(const GenesisRuntime *runtime,
                                        const GenesisCheckpointIdentity *identity,
                                        const GenesisTransactionEvidence *transaction,
                                        GenesisFrameProducerFunction frame_producer,
                                        GenesisCheckpointEvidenceBundle *bundle_out) {
  GenesisCheckpointEvidenceBundle bundle = {0};
  GenesisSha256 digest;
  uint32_t index;
  if (runtime == 0 || identity == 0 || transaction == 0 || bundle_out == 0 ||
      !genesis_checkpoint_identity_is_valid(identity) ||
      transaction->transaction_count > GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES ||
      transaction->has_full_transactions > 1U ||
      runtime->devices.vdp.dma.phase < GENESIS_VDP_DMA_IDLE ||
      runtime->devices.vdp.dma.phase > GENESIS_VDP_DMA_BUSY ||
      runtime->devices.vdp.dma.kind < GENESIS_VDP_DMA_MEMORY_TO_VRAM ||
      runtime->devices.vdp.dma.kind > GENESIS_VDP_DMA_VRAM_FILL ||
      !runtime->devices.interrupt.checkpoint_entered ||
       runtime->devices.interrupt.vblank_transition_count - runtime->devices.interrupt.vblank_transition_count_at_checkpoint_entry < GENESIS_STABLE_FRAME_VBLANK_COUNT)
    return 0;
  for (index = 0U; index < transaction->transaction_count; ++index)
    if (!genesis_valid_bus_access(&transaction->transactions[index])) return 0;
  bundle.schema_version = GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION;
  bundle.identity = *identity;
  genesis_sha256_init(&digest);
  genesis_sha_options(&digest, &bundle.identity.options);
  genesis_sha256_final(&digest, bundle.identity.options_digest);
  memcpy(bundle.cpu.d, runtime->d, sizeof(bundle.cpu.d)); memcpy(bundle.cpu.a, runtime->a, sizeof(bundle.cpu.a));
  bundle.cpu.sr = runtime->sr; bundle.cpu.pc_class = GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN;
  genesis_sha256_init(&digest); genesis_sha256_update(&digest, runtime->work_ram, sizeof(runtime->work_ram)); genesis_sha256_final(&digest, bundle.ram.work_ram_digest);
  bundle.device.devices = runtime->devices;
  bundle.transaction = *transaction;
  /* SEG-007-T050: call the caller-supplied frame producer (see
   * GenesisFrameProducerFunction above -- normally T049's existing
   * genesis_vdp_produce_frame, platforms/genesis/runtime/vdp_render.c) against the
   * *just-copied* persistent VDP state (bundle.device.devices.vdp), exactly
   * once, here, after the stable-frame gate above has already required two
   * post-checkpoint-entry VBlank transitions (T042 §13). This function
   * performs no rendering logic of its own -- it only calls the supplied
   * composition owner and copies its output verbatim into T042 §12's own
   * `GenesisFrameArtifact` bundle member (no new frame schema).
   *
   * `frame_producer == NULL` (skip entirely) and a renderer failure (the
   * supplied function returning nonzero, e.g. because the current reached
   * VDP register state is outside vdp_render.c's documented bound surface)
   * both behave identically: this whole extraction still succeeds, and
   * cpu/ram/device/transaction evidence remain valid and useful on their
   * own, matching this bundle's existing per-category present/match design.
   * `bundle.frame` is simply left at its `{0}` zero-initialization from the
   * `bundle` declaration above -- byte-for-byte the same "inert" value
   * SEG-007-T131 already left it at -- and
   * `genesis_write_checkpoint_evidence_summary` below independently detects
   * that exact zero state (a genuine SHA-256 digest is never literally all
   * zero bytes) to compute whether a real frame artifact exists, rather
   * than trusting any new out-of-band success flag. */
  if (frame_producer != 0) {
    (void)frame_producer(bundle.device.devices.vdp.vram, bundle.device.devices.vdp.vsram,
                          bundle.device.devices.vdp.cram, bundle.device.devices.vdp.registers,
                          &bundle.frame);
  }
  genesis_sha256_init(&digest);
  for (index = 0U; index < bundle.transaction.transaction_count; ++index) genesis_sha_bus_access(&digest, &bundle.transaction.transactions[index]);
  genesis_sha256_final(&digest, bundle.transaction.transaction_digest);
  genesis_sha256_init(&digest); genesis_sha_u32(&digest,bundle.schema_version); genesis_sha256_update(&digest,(const uint8_t *)bundle.identity.checkpoint_id,bundle.identity.checkpoint_id_length); genesis_sha_u8(&digest,bundle.identity.checkpoint_id_length); genesis_sha256_update(&digest,(const uint8_t *)bundle.identity.rom_sha256,64U); genesis_sha_options(&digest,&bundle.identity.options); genesis_sha256_update(&digest,bundle.identity.options_digest,32U); for(index=0U;index<8U;++index) genesis_sha_u32(&digest,bundle.cpu.d[index]); for(index=0U;index<8U;++index) genesis_sha_u32(&digest,bundle.cpu.a[index]); genesis_sha_u16(&digest,bundle.cpu.sr); genesis_sha_u8(&digest,(uint8_t)bundle.cpu.pc_class); genesis_sha256_update(&digest,bundle.ram.work_ram_digest,32U); genesis_sha_device(&digest,&bundle.device.devices); genesis_sha_u8(&digest,bundle.transaction.has_full_transactions); genesis_sha_u16(&digest,bundle.transaction.transaction_count); if(bundle.transaction.has_full_transactions) for(index=0U;index<bundle.transaction.transaction_count;++index) genesis_sha_bus_access(&digest,&bundle.transaction.transactions[index]); genesis_sha256_update(&digest,bundle.transaction.transaction_digest,32U); genesis_sha256_update(&digest,bundle.frame.pixels,sizeof(bundle.frame.pixels)); genesis_sha256_update(&digest,bundle.frame.palette_snapshot,sizeof(bundle.frame.palette_snapshot)); genesis_sha256_update(&digest,bundle.frame.frame_digest,32U); genesis_sha256_final(&digest,bundle.bundle_digest);
  *bundle_out = bundle;
  return 1;
}

static int genesis_checkpoint_id_is_safe(const GenesisCheckpointIdentity *identity) {
  uint32_t index;
  if (identity->checkpoint_id_length == 0U ||
      identity->checkpoint_id_length > GENESIS_MAX_NAME_LENGTH) return 0;
  for (index = 0U; index < identity->checkpoint_id_length; ++index) {
    char value = identity->checkpoint_id[index];
    if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.')) return 0;
  }
  return 1;
}

static int genesis_checkpoint_identity_is_valid(const GenesisCheckpointIdentity *identity) {
  if (identity == 0 || !genesis_checkpoint_id_is_safe(identity) ||
      !genesis_valid_rom_sha256(identity->rom_sha256) ||
      identity->options.schema_version != GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION ||
      identity->options.stable_frame_vblank_count != GENESIS_STABLE_FRAME_VBLANK_COUNT)
    return 0;
  return 1;
}

/* SEG-007-T050: detects whether `frame` is a genuinely produced
 * GenesisFrameArtifact (see genesis_vdp_produce_frame /
 * genesis_extract_checkpoint_evidence above) rather than the never-rendered
 * `{0}` value SEG-007-T131 originally left this schema member at. A real
 * SHA-256 digest (frame_digest, computed over pixels then palette_snapshot
 * per T042 §12.5/vdp_render.h's checkpoint-C5 comment) is never literally 32
 * zero bytes for any input this project's own genesis_sha256_final ever
 * produces, so an all-zero frame_digest is a safe, deterministic, bounded
 * sentinel for "never populated" -- this is a presence check on the
 * existing schema's own field, not a new frame schema, comparison rule, or
 * parallel evidence representation.
 *
 * SEG-007-T050 add-on: declared (non-static) in runtime.h so the frame-export
 * module (platforms/genesis/runtime/frame_export.c) can reuse this exact predicate
 * instead of duplicating a second, potentially divergent all-zero-digest
 * check. */
int genesis_frame_artifact_is_populated(const GenesisFrameArtifact *frame) {
  static const uint8_t zero_digest[sizeof(frame->frame_digest)] = {0};
  return memcmp(frame->frame_digest, zero_digest, sizeof(zero_digest)) != 0;
}

int genesis_write_checkpoint_evidence_summary(const GenesisCheckpointEvidenceBundle *bundle,
                                              const GenesisCheckpointEvidenceSummary *summary) {
  int all_required_ready;
  int frame_ready;
  if (bundle == 0 || summary == 0 || bundle->schema_version != GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION ||
      bundle->cpu.pc_class != GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN ||
      !genesis_checkpoint_identity_is_valid(&bundle->identity)) return 1;
  /* SEG-007-T050: this function still enforces the "no PASS while frame is
   * absent/not-ready" invariant itself, rather than trusting the caller's
   * verdict_passed/frame_present/frame_matches outright -- but it now
   * verifies that claim against the bundle's own real frame-population
   * state (see genesis_frame_artifact_is_populated above) instead of always
   * forcing it false. A caller that claims frame_present/frame_matches
   * while the bundle's own frame was never actually rendered still gets a
   * forced "fail" (frame_ready remains 0). frame_matches' own comparison
   * *semantics* (tolerance, exact-subset rule) remain SEG-007-T053's owned
   * concern, unmodified and unintroduced here; this function only verifies
   * presence, exactly as it already does for every other required
   * category's "present" claim implicitly by trusting the caller for those
   * (this task adds no new per-category matching rule). */
  frame_ready = genesis_frame_artifact_is_populated(&bundle->frame);
  all_required_ready = summary->verdict_passed &&
      summary->cpu_present && summary->cpu_matches &&
      summary->ram_present && summary->ram_matches &&
      summary->device_present && summary->device_matches &&
      summary->transaction_present && summary->transaction_matches &&
      summary->frame_present && summary->frame_matches && frame_ready;
  return printf("{\"schema_version\":%u,\"report_kind\":\"checkpoint_evidence_summary\",\"checkpoint_id\":\"%.*s\",\"rom_sha256\":\"%s\",\"pc_class\":%u,\"present\":{\"cpu\":%s,\"ram\":%s,\"device\":%s,\"transaction\":%s,\"frame\":%s},\"frame_state\":\"%s\",\"matches\":{\"cpu\":%s,\"ram\":%s,\"device\":%s,\"transaction\":%s},\"verdict\":\"%s\"}\n", bundle->schema_version, (int)bundle->identity.checkpoint_id_length, bundle->identity.checkpoint_id, bundle->identity.rom_sha256, (unsigned)bundle->cpu.pc_class, summary->cpu_present?"true":"false",summary->ram_present?"true":"false",summary->device_present?"true":"false",summary->transaction_present?"true":"false",frame_ready?"true":"false",frame_ready?"ready":"not_ready",summary->cpu_matches?"true":"false",summary->ram_matches?"true":"false",summary->device_matches?"true":"false",summary->transaction_matches?"true":"false",all_required_ready?"pass":"fail") < 0;
}

static const char *genesis_stop_class_name(GenesisStopClass value) {
  switch (value) {
  case GENESIS_STOP_UNSUPPORTED_CPU_FORM: return "unsupported_cpu_form";
  case GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS: return "unsupported_device_access";
  case GENESIS_STOP_UNSUPPORTED_MEMORY_REGION: return "unsupported_memory_region";
  case GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET: return "unresolved_indirect_target";
  case GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET: return "known_but_unemitted_target";
  case GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT: return "unsupported_interrupt_or_scheduling_event";
  case GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY: return "internal_dispatch_inconsistency";
  case GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED: return "instruction_budget_exhausted";
  case GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY: return "discovery_prefix_boundary";
  case GENESIS_STOP_C4_LOWERING_GAP: return "c4_lowering_gap";
  case GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION: return "unsupported_cpu_exception";
  case GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION: return "unsupported_z80_execution";
  }
  return 0;
}

static const char *genesis_diagnostic_name(GenesisDiagnosticCategory value) {
  switch (value) {
  case GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS: return "odd_instruction_address";
  case GENESIS_DIAG_UNMAPPED_INSTRUCTION_ADDRESS: return "unmapped_instruction_address";
  case GENESIS_DIAG_TRUNCATED_INSTRUCTION: return "truncated_instruction";
  case GENESIS_DIAG_ILLEGAL_INSTRUCTION: return "illegal_instruction";
  case GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION: return "valid_but_unsupported_instruction";
  case GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM: return "unsupported_instruction_form";
  case GENESIS_DIAG_ODD_DIRECT_TARGET: return "odd_direct_target";
  case GENESIS_DIAG_CONFLICTING_ADDRESS_MAPPING: return "conflicting_address_mapping";
  case GENESIS_DIAG_INVALID_ADDRESS_MAPPING: return "invalid_address_mapping";
  case GENESIS_DIAG_UNMAPPED_DIRECT_TARGET: return "unmapped_direct_target";
  case GENESIS_DIAG_MID_INSTRUCTION_DIRECT_TARGET: return "mid_instruction_direct_target";
  case GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE: return "reached_unresolved_direct_edge";
  case GENESIS_DIAG_INVALID_FRONTEND_IMAGE_SOURCE_ID: return "invalid_frontend_image_source_id";
  case GENESIS_DIAG_FRONTEND_IMAGE_BYTE_LENGTH_MISMATCH: return "frontend_image_byte_length_mismatch";
  case GENESIS_DIAG_INVALID_MAPPING_CLAIM: return "invalid_mapping_claim";
  case GENESIS_DIAG_VECTOR_FIXTURE_ID_MISMATCH: return "vector_fixture_id_mismatch";
  case GENESIS_DIAG_VECTOR_IMAGE_SHA256_MISMATCH: return "vector_image_sha256_mismatch";
  case GENESIS_DIAG_VECTOR_CPU_VARIANT_MISMATCH: return "vector_cpu_variant_mismatch";
  case GENESIS_DIAG_VECTOR_EXECUTION_ENTRY_SPACE_MISMATCH: return "vector_execution_entry_space_mismatch";
  case GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_INSTRUCTION: return "execution_entry_inside_discovered_instruction";
  case GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_BLOCK: return "execution_entry_inside_discovered_block";
  case GENESIS_DIAG_EXECUTION_ENTRY_NOT_DISCOVERED_BLOCK_START: return "execution_entry_not_discovered_block_start";
  case GENESIS_DIAG_VECTOR_BLOCK_INSTRUCTION_COUNT_MISMATCH: return "vector_block_instruction_count_mismatch";
  case GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT: return "effective_address_not_24bit";
  case GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS: return "odd_effective_address";
  case GENESIS_DIAG_ROM_WRITE_PROHIBITED: return "rom_write_prohibited";
  case GENESIS_DIAG_UNMAPPED_DATA_ACCESS: return "unmapped_data_access";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO: return "unsupported_device_region_controller_io";
  case GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY: return "internal_dispatch_inconsistency";
  case GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED: return "instruction_budget_exhausted";
  case GENESIS_DIAG_INVALID_STACK_ALIGNMENT: return "invalid_stack_alignment";
  case GENESIS_DIAG_INVALID_STACK_RANGE: return "invalid_stack_range";
  case GENESIS_DIAG_RETURN_CONTEXT_MISSING: return "return_context_missing";
  case GENESIS_DIAG_RETURN_TARGET_MISMATCH: return "return_target_mismatch";
  case GENESIS_DIAG_STARTUP_GRAPH_MISMATCH: return "startup_graph_mismatch";
  case GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED: return "discovery_budget_exhausted";
  case GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET: return "known_but_unemitted_target";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP: return "unsupported_device_region_vdp";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS: return "unsupported_device_region_z80_bus";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM: return "unsupported_device_region_z80_ram";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG: return "unsupported_device_region_psg";
  case GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612: return "unsupported_device_region_ym2612";
  case GENESIS_DIAG_C4_LOWERING_GAP: return "c4_lowering_gap";
  case GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED: return "tier2_computed_target_not_emitted";
  case GENESIS_DIAG_UNSUPPORTED_PRIVILEGE_VIOLATION_EXCEPTION: return "unsupported_privilege_violation_exception";
  case GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION: return "unsupported_trace_exception";
  case GENESIS_DIAG_UNSUPPORTED_SOFTWARE_EXCEPTION: return "unsupported_software_exception";
  case GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE: return "stopped_without_wake_source";
  case GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS: return "z80_view_unmapped_access";
  case GENESIS_DIAG_Z80_BANK_TARGET_UNSUPPORTED: return "z80_bank_target_unsupported";
  case GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS: return "genesis_68k_z80_area_without_bus";
  case GENESIS_DIAG_Z80_UNKNOWN_IMAGE: return "z80_unknown_image";
  case GENESIS_DIAG_Z80_CODE_MISMATCH: return "z80_code_mismatch";
  case GENESIS_DIAG_Z80_NO_OWNER: return "z80_no_owner";
  case GENESIS_DIAG_Z80_MUTABLE_CODE: return "z80_mutable_code";
  case GENESIS_DIAG_Z80_UNRESOLVED_FETCH_MAPPING: return "z80_unresolved_fetch_mapping";
  case GENESIS_DIAG_Z80_UNSUPPORTED_ACKNOWLEDGE: return "z80_unsupported_acknowledge";
  case GENESIS_DIAG_IRQ6_VECTOR_IN_WORK_RAM: return "irq6_vector_in_work_ram";
  case GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_LAYOUT: return "unsupported_cartridge_sram_layout";
  case GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_ACCESS: return "unsupported_cartridge_sram_access";
  default: return 0;
  }
}

static int genesis_valid_stop_pair(GenesisStopClass stop_class,
                                   GenesisDiagnosticCategory category) {
  switch (stop_class) {
  case GENESIS_STOP_UNSUPPORTED_CPU_FORM:
    return category == GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION ||
           category == GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM;
  case GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS:
    return category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO ||
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP ||
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS ||
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM ||
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG ||
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612 ||
           category == GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS || category == GENESIS_DIAG_Z80_BANK_TARGET_UNSUPPORTED ||
           category == GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS ||
           category == GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_LAYOUT ||
           category == GENESIS_DIAG_UNSUPPORTED_CARTRIDGE_SRAM_ACCESS;
  case GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION:
    return category == GENESIS_DIAG_Z80_UNKNOWN_IMAGE || category == GENESIS_DIAG_Z80_CODE_MISMATCH ||
           category == GENESIS_DIAG_Z80_NO_OWNER || category == GENESIS_DIAG_Z80_MUTABLE_CODE ||
           category == GENESIS_DIAG_Z80_UNRESOLVED_FETCH_MAPPING || category == GENESIS_DIAG_Z80_UNSUPPORTED_ACKNOWLEDGE;
  case GENESIS_STOP_UNSUPPORTED_MEMORY_REGION:
    return category == GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT ||
           category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS ||
           category == GENESIS_DIAG_ROM_WRITE_PROHIBITED ||
           category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS ||
           category == GENESIS_DIAG_INVALID_STACK_ALIGNMENT ||
           category == GENESIS_DIAG_INVALID_STACK_RANGE ||
           category == GENESIS_DIAG_RETURN_TARGET_MISMATCH;
  case GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET:
    return category == GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE ||
           category == GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED;
  case GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET:
    return category == GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET;
  case GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT:
    /* SEG-021-T020: a STOP with no possible wake source. */
    return category == GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE || category == GENESIS_DIAG_IRQ6_VECTOR_IN_WORK_RAM;
  case GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY:
    return category == GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY;
  case GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED:
    return category == GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED;
  case GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY:
    return category == GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED;
  case GENESIS_STOP_C4_LOWERING_GAP:
    return category == GENESIS_DIAG_C4_LOWERING_GAP;
  case GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION:
    return category == GENESIS_DIAG_UNSUPPORTED_PRIVILEGE_VIOLATION_EXCEPTION ||
           category == GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION ||
           category == GENESIS_DIAG_UNSUPPORTED_SOFTWARE_EXCEPTION;
  }
  return 0;
}

static int genesis_c4_lowering_dimensions_fields(GenesisC4LoweringDimensions dimensions,
                                                  const char **family) {
  if (family == 0) return 0;
  switch (dimensions) {
  case GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_NE_SHORT_MISSING_DISPATCHER:
    *family = "branch_ne_short_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_ALWAYS_SHORT_MISSING_DISPATCHER:
    *family = "branch_always_short_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_MISSING_DISPATCHER:
    *family = "compare_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_IMMEDIATE_MISSING_DISPATCHER:
    *family = "compare_immediate_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADD_IMMEDIATE_MISSING_DISPATCHER:
    *family = "add_immediate_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_DISPATCHER:
    *family = "add_quick_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_DISPATCHER:
    *family = "subtract_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_DISPATCHER:
    *family = "subtract_immediate_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_DISPATCHER:
    *family = "logical_and_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_DISPATCHER:
    *family = "logical_or_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_DISPATCHER:
    *family = "logical_or_immediate_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_DISPATCHER:
    *family = "exclusive_or_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_DISPATCHER:
    *family = "exclusive_or_immediate_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_WRITE_SWAP_MISSING_DISPATCHER:
    *family = "write_swap_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_WORD_MISSING_DISPATCHER:
    *family = "sign_extend_word_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_LONG_MISSING_DISPATCHER:
    *family = "sign_extend_long_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_PUSH_EFFECTIVE_ADDRESS_MISSING_DISPATCHER:
    *family = "push_effective_address_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LINK_FRAME_MISSING_DISPATCHER:
    *family = "link_frame_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_UNLINK_FRAME_MISSING_DISPATCHER:
    *family = "unlink_frame_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_DISPATCHER:
    *family = "bit_change_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_DISPATCHER:
    *family = "bit_clear_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_DISPATCHER:
    *family = "bit_set_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_REGISTER_MISSING_DISPATCHER:
    *family = "shift_rotate_register_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_MEMORY_MISSING_DISPATCHER:
    *family = "shift_rotate_memory_missing_dispatcher"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADD_AUTO_UPDATE: *family = "add_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CLR_AUTO_UPDATE: *family = "clr_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_AUTO_UPDATE: *family = "movea_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADDA_AUTO_UPDATE: *family = "adda_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBA_AUTO_UPDATE: *family = "suba_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMPA_AUTO_UPDATE: *family = "cmpa_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMP_AUTO_UPDATE: *family = "cmp_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMPI_AUTO_UPDATE: *family = "cmpi_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_AUTO_UPDATE: *family = "bit_test_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_TST_AUTO_UPDATE: *family = "tst_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ANDI_AUTO_UPDATE: *family = "andi_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_AUTO_UPDATE: *family = "logical_and_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_AUTO_UPDATE: *family = "logical_or_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_AUTO_UPDATE: *family = "logical_or_immediate_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_AUTO_UPDATE: *family = "exclusive_or_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_AUTO_UPDATE: *family = "exclusive_or_immediate_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_AUTO_UPDATE: *family = "logical_not_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_MOVE_MISSING_FACT: *family = "move_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_MISSING_FACT: *family = "movea_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADD_MISSING_FACT: *family = "add_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADDA_MISSING_FACT: *family = "adda_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBA_MISSING_FACT: *family = "suba_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMPA_MISSING_FACT: *family = "cmpa_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMP_MISSING_FACT: *family = "cmp_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CMPI_MISSING_FACT: *family = "cmpi_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_MISSING_FACT: *family = "bit_test_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_TST_MISSING_FACT: *family = "tst_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_CLR_MISSING_FACT: *family = "clr_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ANDI_MISSING_FACT: *family = "andi_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_FACT: *family = "add_quick_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_FACT: *family = "logical_and_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_FACT: *family = "logical_or_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_FACT: *family = "logical_or_immediate_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_FACT: *family = "exclusive_or_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_FACT: *family = "exclusive_or_immediate_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_MISSING_FACT: *family = "logical_not_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_AUTO_UPDATE: *family = "subtract_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_AUTO_UPDATE: *family = "subtract_immediate_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_FACT: *family = "subtract_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_FACT: *family = "subtract_immediate_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_AUTO_UPDATE: *family = "bit_change_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_AUTO_UPDATE: *family = "bit_clear_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_AUTO_UPDATE: *family = "bit_set_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_FACT: *family = "bit_change_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_FACT: *family = "bit_clear_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_FACT: *family = "bit_set_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_MULS_AUTO_UPDATE: *family = "muls_auto_update"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_MULS_MISSING_FACT: *family = "muls_missing_fact"; return 1;
  case GENESIS_C4_LOWERING_DIMENSIONS_NONE: return 0;
  }
  return 0;
}

static const char *genesis_access_width_name(GenesisAccessWidth value) {
  switch (value) {
  case GENESIS_ACCESS_BYTE: return "byte";
  case GENESIS_ACCESS_WORD: return "word";
  case GENESIS_ACCESS_LONG: return "long";
  }
  return 0;
}

static const char *genesis_access_direction_name(GenesisAccessDirection value) {
  switch (value) {
  case GENESIS_ACCESS_READ: return "read";
  case GENESIS_ACCESS_WRITE: return "write";
  }
  return 0;
}

static const char *genesis_bus_kind_name(GenesisBusKind value) {
  switch (value) {
  case GENESIS_BUS_INSTRUCTION_READ: return "instruction_read";
  case GENESIS_BUS_DATA_READ: return "data_read";
  case GENESIS_BUS_DATA_WRITE: return "data_write";
  case GENESIS_BUS_STACK_READ: return "stack_read";
  case GENESIS_BUS_STACK_WRITE: return "stack_write";
  }
  return 0;
}

static const char *genesis_bus_region_name(GenesisBusRegion value) {
  switch (value) {
  case GENESIS_REGION_RAW_CARTRIDGE_ROM: return "raw_cartridge_rom";
  case GENESIS_REGION_SYNTHETIC_WORK_RAM: return "synthetic_work_ram";
  }
  return 0;
}

static int genesis_valid_rom_sha256(const char *value) {
  uint32_t index;
  if (value == 0) return 0;
  for (index = 0U; index < 64U; ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return 0;
  }
  return value[64] == '\0';
}

static int genesis_cpu_dimensions_fields(GenesisCpuDimensions dimensions, const char **family,
                                         const char **size, const char **addressing_mode_class) {
  if (family == 0 || size == 0 || addressing_mode_class == 0) return 0;
  switch (dimensions) {
  case GENESIS_CPU_DIMENSIONS_NOP:
    *family = "nop"; *size = "none"; *addressing_mode_class = "implied"; return 1;
  case GENESIS_CPU_DIMENSIONS_STOP_IMMEDIATE_WORD:
    *family = "stop"; *size = "word"; *addressing_mode_class = "immediate"; return 1;
  case GENESIS_CPU_DIMENSIONS_RESET:
    *family = "reset"; *size = "none"; *addressing_mode_class = "implied"; return 1;
  case GENESIS_CPU_DIMENSIONS_MOVE_AN_TO_USP:
    *family = "move_to_usp"; *size = "long"; *addressing_mode_class = "address_register_direct"; return 1;
  case GENESIS_CPU_DIMENSIONS_NONE: return 0;
  }
  return 0;
}

/* SEG-007-T073: `metadata->cpu_dimensions` is a single whole-program-static
   value compiled from *any* unsupported_cpu_form frontier the emitter found
   anywhere in the source program, independent of whether that frontier is
   ever actually reached at runtime. The report is representable per the
   actually-reached `result`/`stop_class`, never per that unrelated compiled
   static value: this gate must consult `metadata->cpu_dimensions` only when
   the actually-reached stop is `GENESIS_STOP_UNSUPPORTED_CPU_FORM`. For a
   completion or for every other stop class, the wire report always emits
   `cpu_dimensions: null` (see genesis_write_sanitized_report/
   genesis_write_full_report below) regardless of what static value happens
   to be compiled into the program's `GenesisReportMetadata`, so this gate
   must not reject an otherwise valid, already-`genesis_valid_stop_pair`-
   conforming stop just because that unrelated static field is non-`NONE`. */
static int genesis_valid_report_metadata(const GenesisControlTransfer *result,
                                         const GenesisReportMetadata *metadata) {
  const char *family;
  const char *size;
  const char *addressing_mode_class;
  if (result == 0 || metadata == 0) return 0;
  if (result->kind == GENESIS_COMPLETE)
    return result->stop.c4_lowering_dimensions == GENESIS_C4_LOWERING_DIMENSIONS_NONE;
  /* SEG-007-T252 / ADR-0040: GENESIS_RUNNER_RESOURCE_LIMIT's `.stop` is
     required to be fully zeroed (structurally meaningless), mirroring the
     GENESIS_COMPLETE check above -- a nonzero `c4_lowering_dimensions` or a
     nonzero `metadata->cpu_dimensions` would mean some caller populated stop
     fields for a kind that must never carry guest stop semantics. */
  if (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT)
    return result->stop.c4_lowering_dimensions == GENESIS_C4_LOWERING_DIMENSIONS_NONE &&
           metadata->cpu_dimensions == GENESIS_CPU_DIMENSIONS_NONE;
  if (result->kind != GENESIS_STOP) return 0;
  if (result->stop.stop_class == GENESIS_STOP_C4_LOWERING_GAP)
    return genesis_c4_lowering_dimensions_fields(result->stop.c4_lowering_dimensions, &family) &&
           metadata->cpu_dimensions == GENESIS_CPU_DIMENSIONS_NONE;
  if (result->stop.c4_lowering_dimensions != GENESIS_C4_LOWERING_DIMENSIONS_NONE) return 0;
  if (result->stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM)
    return genesis_cpu_dimensions_fields(metadata->cpu_dimensions, &family, &size,
                                         &addressing_mode_class);
  return 1;
}

static int genesis_valid_provenance(const GenesisProvenance *provenance) {
  uint32_t index;
  if (provenance == 0 || provenance->has_instruction_provenance > 1U || provenance->has_access > 1U ||
      provenance->mapping_claim_count > GENESIS_MAX_MAPPING_CLAIMS ||
      provenance->bus_access_count > GENESIS_MAX_BUS_ACCESSES) return 0;
  if (provenance->has_instruction_provenance != 0U) {
    const GenesisInstructionProvenance *instruction = &provenance->instruction;
    if (instruction->cpu_variant != GENESIS_CPU_MC68000 ||
        (instruction->source_address & UINT32_C(0xFF000000)) != 0U ||
        instruction->length < 2U || instruction->length > GENESIS_MAX_RAW_BYTES) return 0;
  } else if (provenance->instruction.cpu_variant != 0 || provenance->instruction.source_address != 0U ||
             provenance->instruction.image_offset != 0U || provenance->instruction.primary_bytes[0] != 0U ||
             provenance->instruction.primary_bytes[1] != 0U || provenance->instruction.length != 0U) return 0;
  if (provenance->has_access != 0U) {
    if ((provenance->access_address & UINT32_C(0xFF000000)) != 0U ||
        genesis_access_width_name(provenance->access_width) == 0 ||
        genesis_access_direction_name(provenance->access_direction) == 0) return 0;
  } else if (provenance->access_address != 0U || provenance->access_width != 0 ||
             provenance->access_direction != 0) return 0;
  for (index = 0U; index < provenance->mapping_claim_count; ++index) {
    const GenesisMappingClaim *claim = &provenance->mapping_claims[index];
    if (claim->name_length > GENESIS_MAX_NAME_LENGTH ||
        (claim->target_begin & UINT32_C(0xFF000000)) != 0U ||
        (claim->target_end & UINT32_C(0xFF000000)) != 0U ||
        claim->target_begin >= claim->target_end || claim->image_begin >= claim->image_end ||
        claim->image_end - claim->image_begin != (uint64_t)claim->target_end - claim->target_begin) return 0;
  }
  for (index = 0U; index < provenance->bus_access_count; ++index) {
    const GenesisBusAccess *access = &provenance->bus_accesses[index];
    if (access->ordinal != index || access->raw_byte_count > GENESIS_MAX_RAW_BYTES ||
        (access->address & UINT32_C(0xFF000000)) != 0U || genesis_bus_kind_name(access->kind) == 0 ||
        genesis_bus_region_name(access->region) == 0) return 0;
  }
  return 1;
}

int genesis_write_sanitized_report(const GenesisControlTransfer *result, const char *rom_sha256,
                                   const GenesisReportMetadata *metadata) {
  const char *family;
  const char *size;
  const char *addressing_mode_class;
  if (result == 0 || !genesis_valid_rom_sha256(rom_sha256) ||
      !genesis_valid_report_metadata(result, metadata)) return 1;
  if (result->kind == GENESIS_COMPLETE)
    return printf("{\"schema_version\":1,\"report_kind\":\"sanitized\",\"rom_sha256\":\"%s\",\"result\":\"completed\",\"stop_class\":null,\"diagnostic_category\":null,\"cpu_dimensions\":null,\"c4_lowering_dimensions\":null}\n", rom_sha256) < 0;
  /* SEG-007-T252 / ADR-0040: a runner resource-limit exhaustion is a sibling
     top-level `"result"` value, never nested under `stop_class` /
     `diagnostic_category` (both stay null here, exactly like `completed`).
     `runner_dispatch_count` is the one new field, always present for this
     result and never emitted for any other. This can never be conflated with
     the guest `instruction_budget_exhausted` stop, which uses
     `"result":"stop"`. */
  if (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT)
    return printf("{\"schema_version\":1,\"report_kind\":\"sanitized\",\"rom_sha256\":\"%s\",\"result\":\"runner_resource_limit\",\"stop_class\":null,\"diagnostic_category\":null,\"cpu_dimensions\":null,\"c4_lowering_dimensions\":null,\"runner_dispatch_count\":%lu}\n", rom_sha256, (unsigned long)result->runner_dispatch_count) < 0;
  if (result->kind != GENESIS_STOP || genesis_stop_class_name(result->stop.stop_class) == 0 ||
       genesis_diagnostic_name(result->stop.diagnostic_category) == 0 ||
       !genesis_valid_stop_pair(result->stop.stop_class, result->stop.diagnostic_category)) return 1;
  if (result->stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM)
    return !genesis_cpu_dimensions_fields(metadata->cpu_dimensions, &family, &size,
                                          &addressing_mode_class) ||
                    printf("{\"schema_version\":1,\"report_kind\":\"sanitized\",\"rom_sha256\":\"%s\",\"result\":\"stop\",\"stop_class\":\"%s\",\"diagnostic_category\":\"%s\",\"cpu_dimensions\":{\"family\":\"%s\",\"size\":\"%s\",\"addressing_mode_class\":\"%s\"},\"c4_lowering_dimensions\":null}\n", rom_sha256, genesis_stop_class_name(result->stop.stop_class), genesis_diagnostic_name(result->stop.diagnostic_category), family, size, addressing_mode_class) < 0;
  if (result->stop.stop_class == GENESIS_STOP_C4_LOWERING_GAP)
    return !genesis_c4_lowering_dimensions_fields(result->stop.c4_lowering_dimensions, &family) ||
                   printf("{\"schema_version\":1,\"report_kind\":\"sanitized\",\"rom_sha256\":\"%s\",\"result\":\"stop\",\"stop_class\":\"c4_lowering_gap\",\"diagnostic_category\":\"c4_lowering_gap\",\"cpu_dimensions\":null,\"c4_lowering_dimensions\":{\"family\":\"%s\"}}\n", rom_sha256, family) < 0;
  return printf("{\"schema_version\":1,\"report_kind\":\"sanitized\",\"rom_sha256\":\"%s\",\"result\":\"stop\",\"stop_class\":\"%s\",\"diagnostic_category\":\"%s\",\"cpu_dimensions\":null,\"c4_lowering_dimensions\":null}\n", rom_sha256, genesis_stop_class_name(result->stop.stop_class), genesis_diagnostic_name(result->stop.diagnostic_category)) < 0;
}

static int genesis_hex_byte(FILE *output, uint8_t value) { return fprintf(output, "%02x", (unsigned)value) < 0; }
static int genesis_json_string(FILE *output, const char *text, uint8_t length) {
  uint8_t index;
  if (fputc('"', output) == EOF) return 1;
  for (index = 0U; index < length; ++index) {
    const unsigned char value = (unsigned char)text[index];
    if (value == '"' || value == '\\') {
      if (fputc('\\', output) == EOF || fputc(value, output) == EOF) return 1;
    } else if (value < 0x20U) {
      if (fprintf(output, "\\u%04x", (unsigned)value) < 0) return 1;
    } else if (fputc(value, output) == EOF) return 1;
  }
  return fputc('"', output) == EOF;
}
static int genesis_base64(FILE *output, const uint8_t *bytes, uint32_t count) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t index;
  for (index = 0U; index + 2U < count; index += 3U) {
    if (fputc(table[bytes[index] >> 2U], output) == EOF ||
        fputc(table[((bytes[index] & 3U) << 4U) | (bytes[index + 1U] >> 4U)], output) == EOF ||
        fputc(table[((bytes[index + 1U] & 15U) << 2U) | (bytes[index + 2U] >> 6U)], output) == EOF ||
        fputc(table[bytes[index + 2U] & 63U], output) == EOF) return 1;
  }
  if (index < count) {
    if (fputc(table[bytes[index] >> 2U], output) == EOF || fputc(table[(bytes[index] & 3U) << 4U], output) == EOF) return 1;
    if (index + 1U == count) return fputs("==", output) == EOF;
    if (fputc(table[(bytes[index + 1U] & 15U) << 2U], output) == EOF) return 1;
    if (fputc('=', output) == EOF) return 1;
  }
  return 0;
}
int genesis_write_full_report(FILE *output, const GenesisRuntime *runtime,
                              const GenesisControlTransfer *result, const char *rom_sha256,
                              const GenesisReportMetadata *metadata) {
  uint32_t index;
  const GenesisProvenance *provenance;
  const char *access_width;
  const char *access_direction;
  const char *c4_family;
  if (output == 0 || runtime == 0 || !genesis_valid_rom_sha256(rom_sha256) ||
      !genesis_valid_report_metadata(result, metadata)) return 1;
  if (result->kind != GENESIS_STOP && result->kind != GENESIS_COMPLETE && result->kind != GENESIS_RUNNER_RESOURCE_LIMIT)
    return 1;
  if (result->kind == GENESIS_STOP && (genesis_stop_class_name(result->stop.stop_class) == 0 ||
         genesis_diagnostic_name(result->stop.diagnostic_category) == 0 ||
         !genesis_valid_stop_pair(result->stop.stop_class, result->stop.diagnostic_category) ||
         !genesis_valid_provenance(&result->stop.provenance))) return 1;
  if (fprintf(output, "{\"schema_version\":1,\"report_kind\":\"full\",\"rom_sha256\":\"%s\",\"result\":\"%s\",\"runtime\":{\"d\":[", rom_sha256,
              result->kind == GENESIS_COMPLETE ? "completed" :
              (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT ? "runner_resource_limit" : "stop")) < 0) return 1;
  for (index = 0U; index < 8U; ++index) if (fprintf(output, "%s\"0x%08x\"", index == 0U ? "" : ",", runtime->d[index]) < 0) return 1;
  if (fputs("],\"a\":[", output) == EOF) return 1;
  for (index = 0U; index < 8U; ++index) if (fprintf(output, "%s\"0x%08x\"", index == 0U ? "" : ",", runtime->a[index]) < 0) return 1;
  if (fprintf(output, "],\"usp\":\"0x%08x\",\"sr\":\"0x%04x\",\"pc\":\"0x%08x\",\"work_ram_base64\":\"", (runtime->sr & UINT16_C(0x2000)) != 0U ? runtime->usp : runtime->a[7], (unsigned)runtime->sr, runtime->pc) < 0 || genesis_base64(output, runtime->work_ram, UINT32_C(65536)) != 0 || fputs("\"},", output) == EOF) return 1;
  if (result->kind == GENESIS_COMPLETE) return fputs("\"stop_class\":null,\"diagnostic_category\":null,\"c4_lowering_dimensions\":null,\"provenance\":null}\n", output) == EOF;
  /* SEG-007-T252 / ADR-0040: sibling top-level result, `stop_class`/
     `diagnostic_category`/`c4_lowering_dimensions`/`provenance` all null
     (never nested guest-stop fields), plus the one new deterministic
     `runner_dispatch_count` field. */
  if (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT)
    return fprintf(output,
        "\"stop_class\":null,\"diagnostic_category\":null,\"c4_lowering_dimensions\":null,\"provenance\":null,\"runner_dispatch_count\":%lu}\n",
        (unsigned long)result->runner_dispatch_count) < 0;
  provenance = &result->stop.provenance;
  if (result->stop.stop_class == GENESIS_STOP_C4_LOWERING_GAP) {
    if (!genesis_c4_lowering_dimensions_fields(result->stop.c4_lowering_dimensions, &c4_family) ||
        fprintf(output, "\"stop_class\":\"%s\",\"diagnostic_category\":\"%s\",\"c4_lowering_dimensions\":{\"family\":\"%s\"},\"provenance\":{\"has_instruction_provenance\":%s,\"instruction\":", genesis_stop_class_name(result->stop.stop_class), genesis_diagnostic_name(result->stop.diagnostic_category), c4_family, provenance->has_instruction_provenance ? "true" : "false") < 0) return 1;
  } else if (fprintf(output, "\"stop_class\":\"%s\",\"diagnostic_category\":\"%s\",\"c4_lowering_dimensions\":null,\"provenance\":{\"has_instruction_provenance\":%s,\"instruction\":", genesis_stop_class_name(result->stop.stop_class), genesis_diagnostic_name(result->stop.diagnostic_category), provenance->has_instruction_provenance ? "true" : "false") < 0) return 1;
  if (provenance->has_instruction_provenance) {
    const GenesisInstructionProvenance *p = &provenance->instruction;
    if (fprintf(output, "{\"cpu_variant\":\"mc68000\",\"source_address\":\"0x%08x\",\"image_offset\":%llu,\"primary_bytes\":\"", p->source_address, (unsigned long long)p->image_offset) < 0 || genesis_hex_byte(output, p->primary_bytes[0]) || genesis_hex_byte(output, p->primary_bytes[1]) || fprintf(output, "\",\"length\":%u}", p->length) < 0) return 1;
  } else if (fputs("null", output) == EOF) return 1;
  if (provenance->has_access != 0U && (genesis_access_width_name(provenance->access_width) == 0 || genesis_access_direction_name(provenance->access_direction) == 0)) return 1;
  access_width = provenance->has_access ? genesis_access_width_name(provenance->access_width) : 0;
  access_direction = provenance->has_access ? genesis_access_direction_name(provenance->access_direction) : 0;
  if (access_width) {
    if (fprintf(output, ",\"has_access\":true,\"access_address\":\"0x%08x\",\"access_width\":\"%s\",\"access_direction\":\"%s\",\"mapping_claim_count\":%u,\"mapping_claims\":[", provenance->access_address, access_width, access_direction, (unsigned)provenance->mapping_claim_count) < 0) return 1;
  } else if (fprintf(output, ",\"has_access\":false,\"access_address\":\"0x%08x\",\"access_width\":null,\"access_direction\":null,\"mapping_claim_count\":%u,\"mapping_claims\":[", provenance->access_address, (unsigned)provenance->mapping_claim_count) < 0) return 1;
  for (index = 0U; index < provenance->mapping_claim_count; ++index) {
    const GenesisMappingClaim *claim = &provenance->mapping_claims[index];
    if (claim->name_length > GENESIS_MAX_NAME_LENGTH) return 1;
    if (index != 0U && fputc(',', output) == EOF) return 1;
    if (fputs("{\"name\":", output) == EOF || genesis_json_string(output, claim->name, claim->name_length) ||
        fprintf(output, ",\"target_begin\":\"0x%08x\",\"target_end\":\"0x%08x\",\"image_begin\":%llu,\"image_end\":%llu}", claim->target_begin, claim->target_end, (unsigned long long)claim->image_begin, (unsigned long long)claim->image_end) < 0) return 1;
  }
  if (fprintf(output, "],\"bus_access_count\":%u,\"bus_accesses\":[", (unsigned)provenance->bus_access_count) < 0) return 1;
  for (index = 0U; index < provenance->bus_access_count; ++index) {
    const GenesisBusAccess *access = &provenance->bus_accesses[index];
    uint32_t byte_index;
    if (access->raw_byte_count > GENESIS_MAX_RAW_BYTES) return 1;
    if (index != 0U && fputc(',', output) == EOF) return 1;
    if (genesis_bus_kind_name(access->kind) == 0 || genesis_bus_region_name(access->region) == 0) return 1;
    if (fprintf(output, "{\"ordinal\":%llu,\"kind\":\"%s\",\"address\":\"0x%08x\",\"raw_bytes\":\"", (unsigned long long)access->ordinal, genesis_bus_kind_name(access->kind), access->address) < 0) return 1;
    for (byte_index = 0U; byte_index < access->raw_byte_count; ++byte_index)
      if (genesis_hex_byte(output, access->raw_bytes[byte_index])) return 1;
    if (fprintf(output, "\",\"raw_byte_count\":%u,\"region\":\"%s\"}", (unsigned)access->raw_byte_count, genesis_bus_region_name(access->region)) < 0) return 1;
  }
  return fputs("]}}\n", output) == EOF;
}

/* SEG-007-T252 / ADR-0040 correction: NOT part of the stable/required
   full-report or sanitized-report schema, and NOT part of the wire ABI --
   this is a deliberately separate, local-diagnostic-use-only writer. It must
   never be wired to any `--full-report-path`/`--full-report-fd` consumer;
   the only permitted caller is the dedicated `--ephemeral-report-fd` argv
   path in the generated bridge (see libs/codegen/c11/src/genesis.cpp). Writes a
   bare JSON array (not an object) of the runtime's recorded recent-PC
   history, oldest -> newest, hex-formatted exactly like every other PC field
   in genesis_write_full_report. Emits `[]` when `result` is not a
   GENESIS_RUNNER_RESOURCE_LIMIT stop or the history is empty. */
uint32_t genesis_recent_pc_history_project(const GenesisRuntime *runtime,
                                           uint32_t out[GENESIS_RECENT_PC_HISTORY_CAPACITY]) {
  const GenesisExecutionHistory *history;
  uint64_t total;
  uint64_t index;
  uint64_t first;
  uint32_t count = 0U;
  uint32_t keep;
  if (runtime == 0 || out == 0) return 0U;
  history = &runtime->execution_history;
  total = history->total_recorded;
  first = total > GENESIS_EXECUTION_HISTORY_CAPACITY ? total - GENESIS_EXECUTION_HISTORY_CAPACITY : 0U;
  for (index = first; index < total; ++index)
    if (history->events[index % GENESIS_EXECUTION_HISTORY_CAPACITY].kind == GENESIS_HISTORY_DISPATCH) ++count;
  keep = count < GENESIS_RECENT_PC_HISTORY_CAPACITY ? count : GENESIS_RECENT_PC_HISTORY_CAPACITY;
  count -= keep; /* dispatch events to skip */
  keep = 0U;
  for (index = first; index < total; ++index) {
    const GenesisExecutionHistoryEvent *event = &history->events[index % GENESIS_EXECUTION_HISTORY_CAPACITY];
    if (event->kind != GENESIS_HISTORY_DISPATCH) continue;
    if (count != 0U) { --count; continue; }
    out[keep++] = event->pc;
  }
  return keep;
}

/* Line 1: the recent-PC array (unchanged transport, RESOURCE_LIMIT only).
   Line 2 (only when detail history is enabled, any result kind): the typed
   history, oldest -> newest, no values/payloads. */
int genesis_write_ephemeral_pc_history(FILE *output, const GenesisRuntime *runtime,
                                       const GenesisControlTransfer *result) {
  uint32_t pcs[GENESIS_RECENT_PC_HISTORY_CAPACITY];
  uint32_t count;
  uint32_t index;
  if (output == 0 || runtime == 0 || result == 0) return 1;
  if (fputc('[', output) == EOF) return 1;
  if (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT) {
    count = genesis_recent_pc_history_project(runtime, pcs);
    for (index = 0U; index < count; ++index)
      if (fprintf(output, "%s\"0x%08x\"", index == 0U ? "" : ",", pcs[index]) < 0) return 1;
  }
  if (fputs("]\n", output) == EOF) return 1;
  if (runtime->execution_history.detail_enabled) {
    const GenesisExecutionHistory *history = &runtime->execution_history;
    const uint64_t total = history->total_recorded;
    const uint64_t first = total > GENESIS_EXECUTION_HISTORY_CAPACITY ? total - GENESIS_EXECUTION_HISTORY_CAPACITY : 0U;
    uint64_t event_index;
    if (fputc('[', output) == EOF) return 1;
    for (event_index = first; event_index < total; ++event_index) {
      const GenesisExecutionHistoryEvent *event = &history->events[event_index % GENESIS_EXECUTION_HISTORY_CAPACITY];
      const char *sep = event_index == first ? "" : ",";
      int rc = 0;
      if (event->kind == GENESIS_HISTORY_DISPATCH)
        rc = fprintf(output, "%s{\"b\":%llu,\"k\":\"dispatch\",\"pc\":\"0x%08x\"}", sep,
                     (unsigned long long)event->boundary, event->pc);
      else if (event->kind == GENESIS_HISTORY_RETIRED)
        rc = fprintf(output, "%s{\"b\":%llu,\"k\":\"retired\",\"pc\":\"0x%08x\",\"next\":\"0x%08x\"}", sep,
                     (unsigned long long)event->boundary, event->pc, event->next_pc);
      else if (event->kind == GENESIS_HISTORY_TRANSFER)
        rc = fprintf(output, "%s{\"b\":%llu,\"k\":\"transfer\",\"t\":%u,\"next\":\"0x%08x\"}", sep,
                     (unsigned long long)event->boundary, (unsigned)event->detail, event->next_pc);
      else
        rc = fprintf(output, "%s{\"b\":%llu,\"k\":\"access\",\"bk\":%u,\"r\":%u,\"w\":%u,\"d\":%u}", sep,
                     (unsigned long long)event->boundary, (unsigned)event->bus, (unsigned)event->detail,
                     (unsigned)event->width, (unsigned)event->direction);
      if (rc < 0) return 1;
    }
    if (fputs("]\n", output) == EOF) return 1;
  }
  return 0;
}
