#include "runtime.h"
#include "../../../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"
#include "../machine/include/segarecomp/machine/genesis/address_space_contract.h"

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
   always echoes the DATA latch's own bit 7, regardless of CTRL. This
   project has no host input frontend (see Non-goals): every button is
   modelled as permanently released, so this function needs no captured
   input state beyond the fixed released-value constants below. */
static uint8_t genesis_controller_io_data_port_read(uint8_t ctrl, uint8_t data) {
  const unsigned th = (ctrl & 0x40U) ? ((unsigned)(data >> 6) & 1U) : 1U;
  /* Released-state 6-bit (bits 5-0) button-group value for the current TH
     state: TH=1 -> "1CBRLDU" = 0x3F (C,B,R,L,D,U all released); TH=0 ->
     "0SA00DU" = 0x33 (S,A,D,U released; bits 3-2 documented forced '0'). */
  const uint8_t input_group = th ? 0x3FU : 0x33U;
  const uint8_t input_value = (uint8_t)((th << 6U) | input_group);
  uint8_t result = (uint8_t)(data & 0x80U); /* bit 7: always DATA-latch echo */
  unsigned bit;
  for (bit = 0U; bit < 7U; ++bit) {
    const unsigned mask = 1U << bit;
    result = (uint8_t)(result | ((ctrl & mask) ? (data & mask) : (input_value & mask)));
  }
  return result;
}

static int genesis_controller_io_access(GenesisDeviceState *devices, uint32_t address,
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
                                                      devices->controller_io.data[data_port_slot]);
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

/* SEG-007-T081: routes exactly the one policy-defined VDP READ selector this
    project currently implements -- a WORD read of the VDP control port's
   base address ($C00004), which GTO1 p. 19 and MCD1's own VDP port
   documentation both establish also serves as the VDP status-register read
   path. The routed value is `devices->vdp.status_register` itself (never a
    hardcoded constant): this project has no VDP interrupt state-mutation
    path, and T084 does not map DMA phase into status bits, so that field remains at its
   zero-initialized default (T042 SS8) and this read always currently
   observes 0x0000 -- an explicit SEG-007-T081 project compatibility
   policy, never a claim about real Genesis VDP status-register runtime
   behavior (VBlank/HBlank/FIFO/DMA-busy/collision/overflow bits are
   genuinely dynamic hardware state this project does not yet model). See
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
  if (dma->remaining_length == 0U) dma->phase = GENESIS_VDP_DMA_IDLE;
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
      devices->vdp.dma.fill_byte_count = fill_mode ? length : 0U;
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

static int genesis_vdp_access(GenesisDeviceState *devices, uint32_t address,
                              GenesisAccessWidth width, GenesisAccessDirection direction,
                              uint32_t *value) {
  if (direction == GENESIS_ACCESS_READ) {
    if (width == GENESIS_ACCESS_WORD && address == UINT32_C(0x00C00004)) {
      *value = devices->vdp.status_register; /* SEG-007-T081 policy value; see comment above. */
      return 1;
    }
    return 0;
  }
  /* direction == GENESIS_ACCESS_WRITE (SEG-007-T091) */
  if (address == UINT32_C(0x00C00000)) {
    /* DATA-port write. An armed VRAM fill (SEG-007-T098/T101) still owns the
       WORD source path exactly as before; anything else is the plain CPU
       DATA-port write model (SEG-007-T108 + SEG-007-T191: VRAM/CRAM/VSRAM
       targets, BYTE/WORD/LONG widths). */
    if (devices->vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
        devices->vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL) {
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

/* SEG-007-T109: fail-closed-lane recognition of exactly the one co-located PSG
   (SN76489) audio port the canonical Sonic startup route reaches -- a single
   tight interval matching ONLY the odd byte address $C00011, mirroring
   genesis_is_vdp_region's single-interval recognition shape.

   $C00011 is the port GTO1 v1.00 p. 10 "VDP AREA" labels "PSG 76489" (a
   primary Sega source); plutiedev.com "psg" ("68000: at $C00011") corroborates
   it. Charles MacDonald's Sega Genesis hardware notes additionally list odd
   mirrors ($C00013/$C00015/$C00017) and note "Doing byte-wide writes to even
   PSG addresses has no effect" -- but those mirrors are only secondarily
   attested, so, exactly like the SEG-007-T103 Z80-RAM-mirror exclusion, they
   stay fail-closed rather than be folded in without direct evidence. */
static int genesis_is_psg_region(uint32_t address) {
  /* SEG-007-T115: delegate to the shared contract predicate (byte-identical
     to the former $C00011 literal). */
  return segarecomp_genesis_psg_port_contains(address);
}

/* SEG-007-T109: the bounded, runtime-reached PSG (SN76489) BYTE WRITE.
   ================ scope ================
   This models ONLY the CPU-visible SN76489 write-command register latch. It is
   an explicitly labelled, replaceable PROJECT COMPATIBILITY POLICY (see
   docs/architecture/genesis-psg-sn76489-port-write-compatibility-policy.md),
   NOT verified Genesis/SN76489 hardware behaviour. There is NO audio synthesis,
   NO tone/noise oscillator or frequency-divider emulation, NO attenuation-ramp
   modelling, NO PSG ready/busy line, NO YM2612/FM, and NO Z80 view of the chip.

   ================ public sources ================
   - GTO1 (Sega, Genesis Technical Overview v1.00, 1991) p. 10: PSG 76489 at
     $C00011.
   - SMS Power "Development/SN76489": LATCH byte %1cctdddd (cc = channel 0..3,
     t = 1 volume / 0 tone-noise, dddd = 4-bit data); DATA byte %0-DDDDDD
     (DDDDDD = upper 6 bits of a 10-bit tone period, or the low bits of an
     attenuation / noise control); attenuation is 4-bit (0 loudest, 15 silent);
     the noise register (channel 3) is 3-bit: bit 2 = feedback mode
     (0 periodic / 1 white), bits 1-0 = shift rate.
   - plutiedev.com "psg": corroborates the port ("68000: at $C00011"), the
     volume command ($90 | channel<<5 | attenuation), the two-byte tone command
     ($80 | channel<<5 | (freq & 0x0F) then freq>>4), and the documented noise
     command set $E0-$E7.
   - Charles MacDonald, Sega Genesis hardware notes: PSG is write-only
     ("Reading the PSG addresses will cause the machine to lock up").

   ================ project compatibility policy (replaceable, not hardware) ==
   - BYTE width only. WORD/LONG writes fail closed (a WORD/LONG access to the
     odd address $C00011 is already rejected by genesis_route_access's
     odd-effective-address guard before this lane is reached; the width check
     here is defensive). MacDonald's secondary "word write, data in LSB" quirk
     is deliberately not modelled, exactly like the SEG-007-T103 Z80-area
     word-width ambiguity.
   - Write-only. Any READ of $C00011 fails closed.
   - A DATA byte (bit 7 clear) with no prior LATCH byte fails closed
     (latch_valid == 0): the latched register is genuinely undefined at that
     point and this project rejects the ambiguity rather than guess.
   - The noise register is 3 bits: a LATCH byte selecting channel 3's
     tone/noise register with data bit 3 set (outside the documented $E0-$E7
     command set) fails closed rather than guess whether hardware latches or
     ignores it. Every other command-byte value is a defined SN76489 command.

   All validation precedes any mutation: a rejected access returns 0 having
   modified neither devices->psg nor the caller's *value (T042 SS3 /
   genesis_route_access's own "on failure neither it nor the runtime is
   modified" contract). */
static int genesis_psg_access(GenesisDeviceState *devices, uint32_t address,
                              GenesisAccessWidth width, GenesisAccessDirection direction,
                              uint32_t *value) {
  GenesisPsgState *psg = &devices->psg;
  uint8_t command;
  if (address != UINT32_C(0x00C00011)) return 0;       /* wrong address */
  if (direction != GENESIS_ACCESS_WRITE) return 0;      /* PSG port is write-only */
  if (width != GENESIS_ACCESS_BYTE) return 0;           /* BYTE width only */
  command = (uint8_t)(*value & 0xFFU);
  if ((command & 0x80U) != 0U) {
    /* LATCH byte %1cctdddd. */
    uint8_t channel = (uint8_t)((command >> 5) & 0x03U);
    uint8_t is_volume = (uint8_t)((command >> 4) & 0x01U);
    uint8_t data = (uint8_t)(command & 0x0FU);
    if (!is_volume && channel == 3U && (data & 0x08U) != 0U)
      return 0; /* reserved: noise register is 3 bits, only $E0-$E7 documented */
    psg->latched_channel = channel;
    psg->latched_volume = is_volume;
    psg->latch_valid = 1U;
    if (is_volume) {
      psg->attenuation[channel] = data;
    } else if (channel < 3U) {
      psg->tone_period[channel] = (uint16_t)((psg->tone_period[channel] & UINT16_C(0x03F0)) | data);
    } else {
      psg->noise_control = (uint8_t)(data & 0x07U);
    }
    return 1;
  }
  /* DATA byte %0-DDDDDD: updates the last-latched register. */
  if (!psg->latch_valid) return 0; /* no register latched yet */
  {
    uint8_t data6 = (uint8_t)(command & 0x3FU);
    uint8_t channel = psg->latched_channel;
    if (psg->latched_volume) {
      psg->attenuation[channel] = (uint8_t)(data6 & 0x0FU);
    } else if (channel < 3U) {
      psg->tone_period[channel] =
          (uint16_t)((psg->tone_period[channel] & UINT16_C(0x000F)) | ((uint16_t)data6 << 4));
    } else {
      psg->noise_control = (uint8_t)(data6 & 0x07U);
    }
    return 1;
  }
}

/* SEG-007-T171: fail-closed-lane recognition of the YM2612 FM-synthesis
   register window ($A04000-$A04003), mirroring genesis_is_psg_region's
   single-source-of-truth delegation shape. */
static int genesis_is_ym2612_region(uint32_t address) {
  return segarecomp_genesis_ym2612_region_contains(address);
}

/* SEG-007-T171: the bounded, runtime-reached YM2612 PART-I address/status
   port ($A04000) BYTE READ, plus four same-task Scope item 6 absorption
   passes, each a documented register-select/register-data write-latch
   shape reached immediately after implementing the previous one: (2) the
   PART-I address port's own BYTE WRITE (register-select latch); (3) the
   PART-I data port's ($A04001) BYTE WRITE (register-data write); (4) the
   PART-II address port's ($A04002) BYTE WRITE (register-select latch for
   the PART-II register bank); (5) the PART-II data port's ($A04003) BYTE
   WRITE (register-data write for the PART-II register bank). This is the
   bound (this task's Scope item 6 caps same-task absorption at the
   milestone's normal six-frontier-iteration advancement bound; this is the
   fifth, and the pass that completes the register-select/register-data
   write-latch protocol symmetrically across both PART-I and PART-II).
   ================ scope ================
   This models ONLY: (a) the CPU-visible PART-I status-port BYTE read, and
   (b) accepting a BYTE write to the PART-I address port, the PART-I data
   port, the PART-II address port, or the PART-II data port as the
   documented register-select/register-data write protocol -- WITHOUT
   modelling any resulting register state or FM effect for any write. It is
   an explicitly labelled, replaceable PROJECT COMPATIBILITY POLICY (see
   docs/architecture/genesis-ym2612-status-port-byte-read-compatibility-policy.md),
   NOT verified YM2612 hardware behaviour. There is NO FM synthesis, NO
   channel/operator/LFO state, NO timer A/B modelling, NO busy-flag timing,
   and NO audio output of any kind -- this is the same "this project performs
   no audio timing/DSP modelling" boundary genesis_psg_access already
   documents for the co-located PSG, extended here to the YM2612's own
   port shapes.

   ================ public sources ================
   - GTO1 (Sega, Genesis Technical Overview v1.00, 1991) p. 10 "Z80 AREA":
     YM2612 at $A04000-$A04003.
   - plutiedev.com "ym2612": PART-I address/status port $A04000, PART-I data
     port $A04001, PART-II address/status port $A04002, PART-II data port
     $A04003; a BYTE read of a status port returns bit 7 = "Busy" (writing
     FM data) and bit 0 = "Timer A overflow", with the remaining bits
     documented as unused. A BYTE write to an address port latches an 8-bit
     register-select value (register number 0-255, PART-I selecting
     channels 1-3's registers, PART-II selecting channels 4-6's registers)
     that governs which register the *next* write to the corresponding data
     port affects; a BYTE write to a data port writes that register's data
     byte. None of these writes carries a documented side effect beyond the
     register-write protocol itself.

   ================ project compatibility policy (replaceable, not hardware) ==
   - BYTE width only for every shape; exactly the PART-I address port
     ($A04000, read or write), the PART-I data port ($A04001, write only),
     the PART-II address port ($A04002, write only), or the PART-II data
     port ($A04003, write only). Every other width/direction fails closed
     (including a READ of any data port, and the still-unconfirmed PART-II
     status-port READ).
   - READ (PART-I address port): this project models no FM register-write
     latency and no timer state, so "Busy" and "Timer A overflow" are always
     deterministically clear: the status byte is a fixed $00 ("not busy",
     "no timer overflow"), mirroring the SEG-007-T111 CTRL3 BYTE-read
     compatibility-policy precedent exactly -- an explicit, cited project
     choice, not asserted real YM2612 hardware timing truth. Side-effect-free.
   - WRITE (any of the four accepted write-shaped ports): this project
     implements no FM register/channel/operator model at all, so there is no
     register-select state to store and no register-specific behaviour to
     apply for any write -- a WRITE of any 8-bit value to any of the four
     accepted ports is unconditionally accepted (the documented protocol
     places no restriction on which value may be selected or written) and is
     side-effect-free under this policy: it mutates no device/bus state.
     This deliberately does NOT model any register-select latch or written
     register value (no `GenesisDeviceState` field is added for any of
     them), because no consumer of that state (real FM register/channel
     semantics) is implemented; adding unread state would be dead weight,
     not a compatibility policy. A later task that implements real FM
     register semantics must add that state itself.

   All validation precedes any mutation: a rejected access returns 0 having
   modified neither *value nor any runtime state (T042 SS3 /
   genesis_route_access's own "on failure neither it nor the runtime is
   modified" contract). */
static int genesis_ym2612_access(uint32_t address, GenesisAccessWidth width,
                                 GenesisAccessDirection direction, uint32_t *value) {
  if (width != GENESIS_ACCESS_BYTE) return 0; /* BYTE width only, every accepted port */
  if (address == SEGARECOMP_GENESIS_YM2612_PART1_ADDRESS_PORT && direction == GENESIS_ACCESS_READ) {
    *value = 0x00U; /* SEG-007-T171 policy: always "not busy", "no timer overflow". */
    return 1;
  }
  if (direction == GENESIS_ACCESS_WRITE &&
      (address == SEGARECOMP_GENESIS_YM2612_PART1_ADDRESS_PORT ||
       address == SEGARECOMP_GENESIS_YM2612_PART1_DATA_PORT ||
       address == SEGARECOMP_GENESIS_YM2612_PART2_ADDRESS_PORT ||
       address == SEGARECOMP_GENESIS_YM2612_PART2_DATA_PORT)) {
    /* SEG-007-T171 (frontier passes 2-5): every accepted register-select /
       register-data write-latch shape is unconditionally accepted and
       modelled as a pure no-op -- no register state is stored anywhere.
       *value is left unmodified (matching every other routed WRITE owner's
       "a write never mutates the caller's own value" contract). */
    return 1;
  }
  return 0; /* wrong port/direction/width combination */
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

/* SEG-007-T102 -- 68k-side Z80 bus-arbitration control registers.
   ================ PROJECT COMPATIBILITY POLICY ================
   This is NOT verified bus-arbitration or bus-timing behavior. No Z80 CPU
   emulation of any kind exists here or anywhere else in this runtime -- no
   Z80 core, no runtime step-through, no JIT, no instruction fetch, and no
   inspection of any Z80 or 68000 program byte: the only modeled effect is
   latching three booleans (GenesisZ80BusState) and computing a deterministic
   BUSACK read-back from them. See
   docs/architecture/genesis-z80-bus-arbitration-compatibility-policy.md.

   Publicly documented hardware facts (Sega, *Genesis Technical Overview*
   v1.00, 1991; already cited elsewhere in this project as GTO1):
     - p. 76 SS4: "$A11100 D8 (W) 0 [=] BUSREQ CANCEL, 1 [=] BUSREQ REQUEST";
       "$A11100 D8 (R) 0 [=] CPU FUNCTION STOP / ACCESSIBLE, 1 [=]
       FUNCTIONING". Step (1) "Write $0100 in $A11100 by using a WORD access.";
       step (2) "Check to see that D8 of $A11100 becomes 0."; step (4) "Write
       $0000 in $A11100 by using a WORD access." "Access to $A11100 can also
       be based on BYTE."
     - p. 76 SS4: "$A11200 D8 (W) 0: RESET REQUEST, 1: RESET CANCEL." "Access
       to $A11200 can also be based on BYTE."
     - p. 91: "RESET ON: DATA 0H (Word) -> $A11200"; "RESET OFF: DATA 100H
       (Word) -> $A11200".
     - p. 76: "At the time of POWER ON RESET, the 68000 has access to the Z80
       bus."
   Documented bit lane for BYTE access: on the 68000, a BYTE access to the
   even register address drives data lines D15-D8, so the documented D8
   control/status bit is bit 0 of that transferred byte -- hence the BYTE
   request/reset/BUSACK bit is D0 of the written/returned byte. (A BYTE access
   to the odd half of either register address is not one of the documented
   registers and fails closed.)

   Policy decisions (replaceable; not hardware claims):
     (a) Immediate grant. A BUSREQ request is granted deterministically and
         immediately: `bus_granted` tracks `bus_requested` with no delay and
         no device-step count, because no Z80 core is executing to contend for
         the bus. This is within T042 contract SS4.1 (a state transition
         caused by the documented BUSREQ access itself) and SS4.4's allowed
         envelope (deterministic, access-caused, never PC/opcode/loop-shape
         driven); it is the strongest form of that envelope (grant on the
         write itself, N=0).
     (b) Deterministic read-back. A read of $A11100 returns a word/byte whose
         BUSACK bit (D8 word / D0 byte) is 0 when `bus_granted` and 1
         otherwise; every other bit reads back 0. Real open-bus / 68000
         prefetch fill of the unused bits is explicitly NOT modeled.
     (c) RESET polarity per GTO1 p. 76 / p. 91: writing the bit as 0 asserts
         /RESET (`reset_asserted = 1`); writing it as 1 releases /RESET
         (`reset_asserted = 0`).
   Fails closed (returns 0, mutating nothing): a read of $A11200; any LONG
   access to either register; and every other address inside
   genesis_is_z80_bus_region. Every validation check precedes every mutation,
   so a rejected access is atomic (T042 SS3 / genesis_route_access's own
   "on failure neither *value nor the runtime is modified" contract). */
static int genesis_z80_bus_access(GenesisDeviceState *devices, uint32_t address,
                                  GenesisAccessWidth width, GenesisAccessDirection direction,
                                  uint32_t *value) {
  uint32_t bit;
  if (width != GENESIS_ACCESS_WORD && width != GENESIS_ACCESS_BYTE)
    return 0; /* LONG (and any invalid width) fails closed */
  if (address != UINT32_C(0x00A11100) && address != UINT32_C(0x00A11200))
    return 0; /* in-region but not one of the two documented registers */
  bit = (width == GENESIS_ACCESS_WORD) ? UINT32_C(0x0100) : UINT32_C(0x0001);
  if (address == UINT32_C(0x00A11100)) {
    if (direction == GENESIS_ACCESS_WRITE) {
      uint8_t requested = (uint8_t)((*value & bit) != 0U);
      devices->z80_bus.bus_requested = requested;
      devices->z80_bus.bus_granted = requested; /* policy (a): immediate grant */
      return 1;
    }
    /* policy (b): BUSACK bit clear iff the 68000 currently holds the bus. */
    *value = devices->z80_bus.bus_granted ? 0U : bit;
    return 1;
  }
  /* address == $A11200: WRITE-only. */
  if (direction != GENESIS_ACCESS_WRITE) return 0;
  devices->z80_bus.reset_asserted = (uint8_t)((*value & bit) == 0U); /* policy (c) */
  return 1;
}

/* SEG-007-T103: fail-closed-lane recognition of exactly the flat 68000-visible
   Z80 program-RAM window -- a single tight interval `[0x00A00000, 0x00A00000 +
   GENESIS_Z80_RAM_BYTES)`, mirroring genesis_is_z80_bus_region's
   single-interval recognition shape. GTO1 v1.00 (1991) 68K memory map (p. 7,
   overview p. 2) and Charles MacDonald's Sega Genesis hardware notes v0.8 SS1/
   SS2 both document an 8 KiB Z80 RAM at $A00000. This interval deliberately
   covers ONLY those 8 KiB: the documented Z80-RAM mirror ($A02000-$A03FFF, only
   secondarily attested from the 68000 side) and the Z80 sound-chip / bank /
   PSG addresses ($A04000+, $A06000, $A07F11) are excluded and stay fail-closed
   / future scope. See
   docs/architecture/genesis-z80-ram-window-compatibility-policy.md. */
static int genesis_is_z80_ram_window_region(uint32_t address) {
  /* SEG-007-T115: delegate to the shared contract predicate (byte-identical
     interval; GENESIS_Z80_RAM_BYTES is itself that contract constant). */
  return segarecomp_genesis_z80_ram_window_contains(address);
}

/* SEG-007-T103 -- flat 68000-visible Z80 program-RAM window ($A00000).
   ================ PROJECT COMPATIBILITY POLICY ================
   This is NOT verified Z80-bus or Z80-area access behavior. No Z80 CPU
   emulation of any kind exists here or anywhere else in this runtime -- no Z80
   core, no runtime step-through, no JIT, no instruction fetch, and no
   inspection of any Z80 or 68000 program byte: the only modeled effect is a
   flat byte-array read/write into GenesisZ80BusState.z80_ram. This realises the
   "68K copies the Z80 sound program into Z-80 S-RAM" step (GTO1 p. 91 Z-80
   start-up sequence step 3) as a plain byte-stream copy target. See
   docs/architecture/genesis-z80-ram-window-compatibility-policy.md.

   Publicly documented hardware facts:
     - GTO1 v1.00 (1991): 68K memory map (p. 7) / overview (p. 2) place an
       8 KByte Z80/sound RAM at $A00000. p. 77 gives the Z80 area range
       $A00000-$A0FFFF and states "Access from 68000 by BYTE."
     - GTO1 v1.00 p. 76 SS4: the 68000 acquires the Z80 bus before accessing the
       Z80 AREA -- "(1) Write $0100 in $A11100 by using a WORD access. (2) Check
       to see that D8 of $A11100 becomes 0. (3) Access to Z80 AREA. (4) Write
       $0000 in $A11100 by using a WORD access."
     - Charles MacDonald, Sega Genesis hardware notes v0.8: SS2 "8k static RAM";
       SS1 68000 memory map "A00000-A0FFFFh : Z80 address space"; SS2.1 Z80
       memory map "0000-1FFFh : RAM" / "2000-3FFFh : RAM (mirror)"; SS2.2 "The
       Z80 bus can only be accessed by the 68000 when the Z80 is running and the
       68000 has the bus"; SS1.2 memory-access quirks: a 68000 word-wide write
       to Z80 RAM writes only the MSB and ignores the LSB.

   Policy decisions (replaceable; not hardware claims):
     (a) Bus-grant gate. This access fails closed (returns 0, mutates nothing)
         unless `devices->z80_bus.bus_granted` is set -- the documented
         requirement that the 68000 hold the Z80 bus grant to reach the Z80 AREA
         (GTO1 p. 76 SS4; MacDonald SS2.2). The grant latch is the SEG-007-T102
         immediate-grant model.
     (b) BYTE width only. A BYTE access maps to a single z80_ram element. WORD
         and LONG (and any invalid width) fail closed: public documentation does
         not unambiguously pin the 68000's word-width semantics against the
         8-bit Z80 area -- GTO1 p. 77 documents BYTE access; MacDonald reports a
         word-write MSB-only quirk for writes only, with read-side behavior
         unspecified. This runtime rejects that ambiguity explicitly (the project charter,
         "reject ambiguity explicitly"); the runtime frontier for this window is
         byte-only.
     (c) Flat, un-mirrored. `offset = address - 0x00A00000` indexes z80_ram
         directly. The Z80-RAM mirror ($A02000-$A03FFF) is excluded (see
         genesis_is_z80_ram_window_region) and stays fail-closed: it is only
         secondarily attested from the 68000 side and folding it in without
         direct evidence would be a guess.
   Every validation check (bus grant, width, exact offset bound) precedes every
   mutation, so a rejected access is atomic: it modifies neither *value nor any
   GenesisRuntime field (T042 SS3 / genesis_route_access's own "on failure
   neither *value nor the runtime is modified" contract). */
static int genesis_z80_ram_window_access(GenesisDeviceState *devices, uint32_t address,
                                         GenesisAccessWidth width, GenesisAccessDirection direction,
                                         uint32_t *value) {
  uint32_t offset;
  if (!devices->z80_bus.bus_granted) return 0; /* policy (a): 68000 must hold the Z80 bus grant */
  if (width != GENESIS_ACCESS_BYTE) return 0;  /* policy (b): BYTE only; WORD/LONG/invalid fail closed */
  offset = address - UINT32_C(0x00A00000);
  if (offset >= GENESIS_Z80_RAM_BYTES) return 0; /* defensive; the predicate already guarantees this */
  if (direction == GENESIS_ACCESS_READ) {
    *value = devices->z80_bus.z80_ram[offset];
    return 1;
  }
  if (direction == GENESIS_ACCESS_WRITE) {
    devices->z80_bus.z80_ram[offset] = (uint8_t)(*value & 0xFFU);
    return 1;
  }
  return 0;
}

GenesisAccessResultKind genesis_route_access(GenesisRuntime *runtime, uint32_t address,
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
  if (genesis_is_device(address)) {
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_controller_io_access(&runtime->devices, address, width, direction, &routed_value)) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_psg_region(address)) {
    /* SEG-007-T109: the co-located PSG (SN76489) audio port at the odd byte
       $C00011 is routed here BEFORE the VDP lane below, because that address is
       inside genesis_is_vdp_region's interval. routed_value carries the
       caller's write value in on a WRITE (genesis_psg_access has no other way
       to learn it); the PSG port is write-only so nothing is ever read back
       into *value. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_psg_access(&runtime->devices, address, width, direction, &routed_value)) {
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_ym2612_region(address)) {
    /* SEG-007-T171: routed_value carries the caller's write value in on a
       WRITE (matching every other routed owner's contract), though
       genesis_ym2612_access's own accepted WRITE shape never actually
       consumes it -- there is no register-select state to store under this
       policy. It is read back into *value only on the accepted READ shape. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_ym2612_access(address, width, direction, &routed_value)) {
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
       read exactly as before this task. */
    if (direction == GENESIS_ACCESS_READ && width == GENESIS_ACCESS_WORD &&
         address == UINT32_C(0x00C00004) &&
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
    if (genesis_vdp_access(&runtime->devices, address, width, direction, &routed_value)) {
      /* GTO1 p. 19 documents the VBlank-pending status bit.  The status read
       * is the established routed VDP seam, so this deliberately bounded
       * policy observes a synthetic VBlank assertion only here.  There is no
       * acknowledgement selector in this substrate: once raised, pending
       * remains sticky rather than inventing an ungrounded clear behavior. */
      if (direction == GENESIS_ACCESS_READ && width == GENESIS_ACCESS_WORD &&
          address == UINT32_C(0x00C00004)) {
        ++runtime->devices.interrupt.vblank_status_read_count;
        if ((routed_value & UINT32_C(0x0008)) != 0U && !runtime->devices.interrupt.vblank_pending) {
          runtime->devices.interrupt.vblank_pending = 1U;
          ++runtime->devices.interrupt.vblank_transition_count;
        }
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
    /* SEG-007-T102: routed_value carries the caller's write value in on a
       WRITE (genesis_z80_bus_access has no other way to learn it) and is read
       back into *value only on a READ -- a write never mutates the caller's
       *value, matching genesis_route_access's documented contract. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_z80_bus_access(&runtime->devices, address, width, direction, &routed_value)) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS);
    return GENESIS_ACCESS_FAIL;
  }
  if (genesis_is_z80_ram_window_region(address)) {
    /* SEG-007-T103: routed_value carries the caller's write value in on a WRITE
       (genesis_z80_ram_window_access has no other way to learn it) and is read
       back into *value only on a READ -- a write never mutates the caller's
       *value, matching genesis_route_access's documented contract. */
    uint32_t routed_value = (direction == GENESIS_ACCESS_WRITE) ? *value : 0U;
    if (genesis_z80_ram_window_access(&runtime->devices, address, width, direction, &routed_value)) {
      if (direction == GENESIS_ACCESS_READ) *value = routed_value;
      return GENESIS_ACCESS_OK;
    }
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS,
                                    GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM);
    return GENESIS_ACCESS_FAIL;
  }
  stop = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  *stop_out = stop;
  return GENESIS_ACCESS_FAIL;
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
 * SEG-007-T047 / ADR-0020 §9: RTE restoration. Both routed reads first, commit
 * only after both succeed, atomic {sr, pc, a[7]} commit; a failed read leaves
 * all three untouched. No target-opcode fetch/decode.
 */
int genesis_exception_return(GenesisRuntime *runtime, uint32_t *restored_pc_out,
                             GenesisRuntimeStop *stop_out) {
  uint32_t sp;
  uint32_t saved_sr = 0U;
  uint32_t saved_pc = 0U;
  GenesisRuntimeStop routed = {0};
  if (runtime == 0 || restored_pc_out == 0 || stop_out == 0) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  sp = runtime->a[7];
  if ((sp & 1U) != 0U) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
                                    GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
    return 0;
  }
  /* Neither routed read mutates any runtime field; commit happens only below. */
  if (genesis_route_access(runtime, sp, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &saved_sr,
                           &routed) != GENESIS_ACCESS_OK) {
    *stop_out = routed;
    return 0;
  }
  if (genesis_route_access(runtime, sp + 2U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &saved_pc,
                           &routed) != GENESIS_ACCESS_OK) {
    *stop_out = routed;
    return 0;
  }
  const int irq6_frame = genesis_is_work_ram(sp, 6U) &&
                         genesis_take_irq6_exception_frame_origin(runtime, sp);
  runtime->sr = (uint16_t)(saved_sr & 0xFFFFU);
  runtime->pc = saved_pc;
  runtime->a[7] = sp + 6U;
  (void)irq6_frame;
  *restored_pc_out = saved_pc;
  return 1;
}

/*
 * SEG-007-T222 / ADR-0037 (extends ADR-0020 §7/§8): the shared, vector-
 * agnostic exception-frame construction primitive extracted out of the
 * formerly-inline IRQ6 admission body below. Parameterized by the target
 * handler entry (already build-time-resolved) and the new SR value (via a
 * keep-mask/forced-bits pair); `return_pc` is the
 * value pushed as the saved PC (IRQ6: the target the dispatcher was about to
 * transfer to; divide-by-zero: the instruction following the faulting
 * DIVS.W/DIVU.W). IRQ6 clears T, sets S, and raises the interrupt mask to 6;
 * synchronous divide-by-zero clears T, sets S, and preserves the pre-fault
 * interrupt mask. `fail_stop_class`/`fail_diag` select the caller-specific
 * diagnostic used for validation failures (frame extent unwritable); a
 * routed-write failure AFTER validation succeeds is always the shared
 * internal-dispatch-inconsistency stop (should never actually occur, since
 * validation already proved every destination writable).
 *
 * Byte-identical to the prior inline IRQ6 body for that caller: same checks,
 * same order, same commit. Contains no IRQ6-specific admission/scheduling
 * logic (SR-mask eligibility, VBlank-pending, grace/credit accounting) --
 * that machinery stays entirely in genesis_irq6_scheduler_and_admit.
 *
 * Returns 0 = validation failed closed (`*result` holds the GENESIS_STOP, no
 *             frame/state mutation of any kind);
 *         1 = frame constructed and committed; `*result` holds the transfer
 *             (`next_pc` set to `handler_entry`, `runtime->pc` also updated).
 * `is_irq6_frame` records only scheduler provenance; it does not affect frame
 * bytes or CPU-state entry semantics.
 */
static int genesis_construct_exception_frame_and_transfer(GenesisRuntime *runtime, uint32_t return_pc,
                                                           uint32_t handler_entry, uint16_t sr_keep_mask,
                                                           uint16_t sr_forced_bits, int is_irq6_frame,
                                                           GenesisStopClass fail_stop_class,
                                                           GenesisDiagnosticCategory fail_diag,
                                                           GenesisControlTransfer *result) {
  uint32_t a7;
  uint32_t frame_base;
  uint16_t saved_sr;
  uint32_t routed_value;
  GenesisRuntimeStop routed = {0};

  /* §7 / §8: supervisor-mode-only, validate the complete six-byte frame extent
     BEFORE the first frame write. */
  if ((runtime->sr & UINT16_C(0x2000)) == 0U) {                    /* S == 0 (user mode) */
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(fail_stop_class, fail_diag);
    return 0;
  }
  a7 = runtime->a[7];
  if (a7 < 6U || (a7 & 1U) != 0U) {                                /* underflow / misaligned */
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(fail_stop_class, fail_diag);
    return 0;
  }
  frame_base = a7 - 6U;
  if (!genesis_is_work_ram(frame_base, 6U)) {                      /* whole extent writable RAM */
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(fail_stop_class, fail_diag);
    return 0;
  }

  /* Validation complete: every remaining step is guaranteed to succeed. Push
     order (§7): SR (word) at frame_base, PC (long) at frame_base+2, big-endian
     through the routed-write boundary. */
  saved_sr = runtime->sr;
  routed_value = saved_sr;
  if (genesis_route_access(runtime, frame_base, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE,
                           &routed_value, &routed) != GENESIS_ACCESS_OK) {
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                       GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  routed_value = return_pc;
  if (genesis_route_access(runtime, frame_base + 2U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE,
                           &routed_value, &routed) != GENESIS_ACCESS_OK) {
    *result = (GenesisControlTransfer){0};
    result->kind = GENESIS_STOP;
    result->stop = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                       GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }

  /* Commit CPU state atomically after the frame is fully written. */
  runtime->a[7] = frame_base;
  runtime->sr = (uint16_t)((saved_sr & sr_keep_mask) | sr_forced_bits);
  if (is_irq6_frame) genesis_note_irq6_exception_frame(runtime, frame_base);
  result->next_pc = handler_entry;
  runtime->pc = handler_entry;
  return 1;
}

/*
 * SEG-007-T222 / ADR-0037: synchronous, unmasked, never-scheduled divide-by-
 * zero (vector 5) exception raise. Called directly from generated DIVS.W/
 * DIVU.W lowering when the divisor is zero -- NOT gated by the SR interrupt
 * mask, NOT admitted at any scheduler/dispatch boundary, and does not consume
 * or arm any IRQ6-only admission-grace/watchdog-credit state.
 */
int genesis_raise_divide_by_zero(GenesisRuntime *runtime, uint32_t fault_pc,
                                 uint32_t *handler_pc_out, GenesisRuntimeStop *stop_out) {
  GenesisControlTransfer result = {0};
  if (runtime == 0 || handler_pc_out == 0 || stop_out == 0) {
    if (stop_out != 0)
      *stop_out = genesis_access_stop(GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY,
                                      GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
    return 0;
  }
  if (!runtime->divide_by_zero_handler_present) {
    *stop_out = genesis_access_stop(GENESIS_STOP_UNSUPPORTED_CPU_FORM,
                                    GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION);
    return 0;
  }
  if (!genesis_construct_exception_frame_and_transfer(
          runtime, fault_pc, runtime->divide_by_zero_handler_entry, UINT16_C(0x7FFF), UINT16_C(0x2000), 0,
          GENESIS_STOP_UNSUPPORTED_CPU_FORM, GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION, &result)) {
    *stop_out = result.stop;
    return 0;
  }
  *handler_pc_out = result.next_pc;
  return 1;
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
 */
static int genesis_irq6_scheduler_and_admit(GenesisRuntime *runtime, uint32_t m68k_cycles,
                                            GenesisControlTransfer *result) {
  uint32_t mask;
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

  if (!runtime->devices.interrupt.vblank_pending) return 0;        /* §5 step 4 */

  /* §4: SR interrupt-mask eligibility (SR bits 10-8 vs level 6). */
  mask = (uint32_t)((runtime->sr >> 8) & 0x7U);
  if (mask >= 6U) return 0;                                        /* §5 step 3: masked, stays pending */

  if (!runtime->irq6_handler_present) return 0;                    /* no build-resolved handler */

  /* SEG-007-T222 / ADR-0037: frame construction/commit delegates to the
     shared helper (byte-identical checks/order/commit to the prior inline
     body); only the IRQ6-specific `vblank_pending` re-arm (§3) stays here,
     performed after a successful commit, exactly as before. */
  if (!genesis_construct_exception_frame_and_transfer(
          runtime, result->next_pc, runtime->irq6_handler_entry, UINT16_C(0x78FF), UINT16_C(0x2600), 1,
          GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
          GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT, result)) {
    return 2;
  }
  runtime->devices.interrupt.vblank_pending = 0U;                  /* §3 re-arm (admission clears) */
  return 1;
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
  runtime->recent_pc_history[runtime->recent_pc_history_next] = runtime->pc;
  runtime->recent_pc_history_next =
      (uint8_t)((runtime->recent_pc_history_next + 1U) % GENESIS_RECENT_PC_HISTORY_CAPACITY);
  if (runtime->recent_pc_history_count < GENESIS_RECENT_PC_HISTORY_CAPACITY)
    ++runtime->recent_pc_history_count;
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

GenesisControlTransfer genesis_runtime_retire_m68k_instruction(GenesisRuntime *runtime,
                                                                uint32_t m68k_cycles,
                                                                uint32_t next_pc) {
  GenesisControlTransfer result = {0};
  if (runtime == 0) return genesis_internal_dispatch_inconsistency_stop(runtime);
  result.kind = GENESIS_CONTINUE_AT_PC;
  result.next_pc = next_pc;
  runtime->pc = next_pc;
  if (runtime->execution_history != 0) { /* SEG-020-T003 pure side-channel append */
    GenesisExecutionHistory *history = runtime->execution_history;
    GenesisExecutionHistoryEvent *event =
        &history->events[history->total_recorded % GENESIS_EXECUTION_HISTORY_CAPACITY];
    event->sequence = history->total_recorded;
    event->next_pc = next_pc;
    event->m68k_cycles = m68k_cycles;
    ++history->total_recorded;
  }
  if (genesis_irq6_scheduler_and_admit(runtime, m68k_cycles, &result) == 2) return result;
  return result;
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
  uint32_t i; genesis_sha_u8(s,d->z80_bus.bus_requested); genesis_sha_u8(s,d->z80_bus.bus_granted); genesis_sha_u8(s,d->z80_bus.reset_asserted); genesis_sha256_update(s,d->z80_bus.z80_ram,GENESIS_Z80_RAM_BYTES);
  for(i=0U;i<GENESIS_VDP_REGISTER_COUNT;++i) { genesis_sha_u16(s,d->vdp.registers[i]); }
  genesis_sha_u8(s,d->vdp.control_port_awaiting_second_word); genesis_sha_u16(s,d->vdp.control_port_first_word); genesis_sha_u32(s,d->vdp.addressed_pointer); genesis_sha_u16(s,d->vdp.auto_increment_value); genesis_sha_u16(s,d->vdp.status_register); genesis_sha_u8(s,d->vdp.data_port_transfer_code); genesis_sha_u8(s,d->vdp.data_port_transfer_code_valid); genesis_sha256_update(s,d->vdp.vram,GENESIS_VDP_VRAM_BYTES); genesis_sha256_update(s,d->vdp.cram,GENESIS_VDP_CRAM_BYTES); genesis_sha256_update(s,d->vdp.vsram,GENESIS_VDP_VSRAM_BYTES); genesis_sha_u8(s,(uint8_t)d->vdp.dma.phase); genesis_sha_u8(s,(uint8_t)d->vdp.dma.kind); genesis_sha_u32(s,d->vdp.dma.source_address); genesis_sha_u32(s,d->vdp.dma.remaining_length); genesis_sha_u32(s,d->vdp.dma.fill_byte_count); genesis_sha_u32(s,d->vdp.dma.transfer_access_count); genesis_sha_u8(s,d->vdp.dma.write_target_code);
  genesis_sha_u8(s,d->psg.latched_channel); genesis_sha_u8(s,d->psg.latched_volume); genesis_sha_u8(s,d->psg.latch_valid); for(i=0U;i<3U;++i) { genesis_sha_u16(s,d->psg.tone_period[i]); }
  genesis_sha256_update(s,d->psg.attenuation,4U); genesis_sha_u8(s,d->psg.noise_control); genesis_sha256_update(s,d->controller_io.data,3U); genesis_sha256_update(s,d->controller_io.ctrl,3U); genesis_sha_u8(s,d->interrupt.vblank_pending); genesis_sha_u32(s,d->interrupt.vblank_status_read_count); genesis_sha_u32(s,d->interrupt.vblank_transition_count); genesis_sha_u8(s,d->interrupt.checkpoint_entered); genesis_sha_u32(s,d->interrupt.vblank_transition_count_at_checkpoint_entry);
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
           category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612;
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
    return 0;
  case GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY:
    return category == GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY;
  case GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED:
    return category == GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED;
  case GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY:
    return category == GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED;
  case GENESIS_STOP_C4_LOWERING_GAP:
    return category == GENESIS_DIAG_C4_LOWERING_GAP;
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
  if (fprintf(output, "],\"usp\":\"0x%08x\",\"sr\":\"0x%04x\",\"pc\":\"0x%08x\",\"work_ram_base64\":\"", runtime->usp, (unsigned)runtime->sr, runtime->pc) < 0 || genesis_base64(output, runtime->work_ram, UINT32_C(65536)) != 0 || fputs("\"},", output) == EOF) return 1;
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
int genesis_write_ephemeral_pc_history(FILE *output, const GenesisRuntime *runtime,
                                       const GenesisControlTransfer *result) {
  uint8_t history_index;
  uint8_t oldest;
  if (output == 0 || runtime == 0 || result == 0) return 1;
  if (fputc('[', output) == EOF) return 1;
  if (result->kind == GENESIS_RUNNER_RESOURCE_LIMIT) {
    oldest = (uint8_t)(runtime->recent_pc_history_count < GENESIS_RECENT_PC_HISTORY_CAPACITY
                       ? 0U : runtime->recent_pc_history_next);
    for (history_index = 0U; history_index < runtime->recent_pc_history_count; ++history_index) {
      const uint8_t slot = (uint8_t)((oldest + history_index) % GENESIS_RECENT_PC_HISTORY_CAPACITY);
      if (fprintf(output, "%s\"0x%08x\"", history_index == 0U ? "" : ",",
                 runtime->recent_pc_history[slot]) < 0) return 1;
    }
  }
  return fputs("]\n", output) == EOF;
}

int genesis_write_ephemeral_execution_history(FILE *output, const GenesisRuntime *runtime) {
  uint64_t first = 0U;
  uint64_t index;
  const GenesisExecutionHistory *history;
  if (output == 0 || runtime == 0) return 1;
  history = runtime->execution_history;
  if (fputc('[', output) == EOF) return 1;
  if (history != 0) {
    if (history->total_recorded > GENESIS_EXECUTION_HISTORY_CAPACITY)
      first = history->total_recorded - GENESIS_EXECUTION_HISTORY_CAPACITY;
    for (index = first; index < history->total_recorded; ++index) {
      const GenesisExecutionHistoryEvent *event = &history->events[index % GENESIS_EXECUTION_HISTORY_CAPACITY];
      if (fprintf(output, "%s{\"seq\":%llu,\"next_pc\":\"0x%08x\",\"cycles\":%u}", index == first ? "" : ",",
                  (unsigned long long)event->sequence, event->next_pc, event->m68k_cycles) < 0) return 1;
    }
  }
  return fputs("]\n", output) == EOF;
}
