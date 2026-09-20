#ifndef SEGARECOMP_RUNTIME_GENESIS_RUNTIME_H
#define SEGARECOMP_RUNTIME_GENESIS_RUNTIME_H

/* SEG-018-T006: Windows host support. This header is the first project include of
 * every generated/runtime translation unit, so it owns the CRT feature selection
 * before any CRT header is seen: the ISO C stdio functions and the POSIX names
 * the generated bridge uses (fdopen) are used deliberately, and the runtime keeps
 * whole-machine state in automatic storage, so reserve the 8 MiB main-thread stack
 * POSIX hosts provide (the Windows default is 1 MiB). */
#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS 1
#endif
#pragma comment(linker, "/STACK:8388608")
#endif

#include <stdint.h>
#include <stdio.h>

#include "../machine/include/segarecomp/machine/genesis/address_space_contract.h"
#include "checkpoint_evidence.h"

/* checkpoint_evidence.h intentionally keeps its standalone schema literals.
 * These production-side checks keep those literals aligned with the shared
 * address-space aliases used when runtime.h supplies the same declarations. */
#if defined(__cplusplus)
static_assert(GENESIS_MAX_RAW_BYTES == SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES,
              "checkpoint evidence raw-byte capacity must match the address-space contract");
static_assert(GENESIS_Z80_RAM_BYTES == SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES,
              "checkpoint evidence Z80 RAM capacity must match the address-space contract");
#else
_Static_assert(GENESIS_MAX_RAW_BYTES == SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES,
               "checkpoint evidence raw-byte capacity must match the address-space contract");
_Static_assert(GENESIS_Z80_RAM_BYTES == SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES,
               "checkpoint evidence Z80 RAM capacity must match the address-space contract");
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small local SHA-256 implementation shared across this runtime (see
 * runtime.c's own header comment above its definition). Exposed here so
 * other Genesis runtime translation units (e.g. vdp_render.c's checkpoint C5
 * frame-digest computation) can reuse this exact implementation instead of
 * duplicating a second SHA-256. `genesis_sha256_block` and the small
 * `genesis_sha_u8/u16/u32/u64` helpers remain private to runtime.c; only the
 * init/update/final entry points a generic caller needs are exposed.
 */
typedef struct GenesisSha256 {
  uint32_t h[8];
  uint64_t length;
  uint8_t block[64];
  uint32_t used;
} GenesisSha256;

void genesis_sha256_init(GenesisSha256 *state);
void genesis_sha256_update(GenesisSha256 *state, const uint8_t *data, uint32_t count);
void genesis_sha256_final(GenesisSha256 *state, uint8_t output[32]);

#ifdef __cplusplus
} /* extern "C" */
#endif

/*
 * SEG-007-T077: one generic, generated, build-time-embedded, read-only
 * cartridge-data region a runtime-computed (non-constant-foldable)
 * effective address may be served from through genesis_route_access,
 * instead of failing closed. See
 * docs/decisions/0006-generic-cartridge-data-region-ownership.md.
 * `data` always points at a compiled `static const uint8_t[]` array the
 * generated program embeds at build time; it is never a pointer into the
 * original ROM file, and the generated program never opens or reads that
 * file at runtime.
 */
typedef struct GenesisOwnedCartridgeRegion {
  uint32_t begin;      /* inclusive */
  uint32_t end;        /* exclusive */
  const uint8_t *data; /* end - begin bytes, big-endian source order */
  uint32_t length;     /* == end - begin; redundant but checked, matching GenesisMappingClaim's own style */
} GenesisOwnedCartridgeRegion;

/*
 * SEG-007-T091: the documented VDP write-only register count. GTO1 p. 22
 * SS4 "VDP REGISTER": "VDP has write only register #0 through #23 and read
 * only status register total 25 register." -- i.e. 24 write-only registers
 * (#0-#23) plus the one separate read-only status register
 * (`GenesisVdpState.status_register` below, already implemented by
 * SEG-007-T081; never part of this array). This is a publicly documented
 * hardware fact, not a project policy choice.
 */
#ifndef SEGARECOMP_RUNTIME_GENESIS_CHECKPOINT_EVIDENCE_H
#define GENESIS_VDP_REGISTER_COUNT 24U
#define GENESIS_VDP_VRAM_BYTES 65536U

/*
 * SEG-007-T108: the documented CRAM (Color RAM) byte size. This realises the
 * exact named placeholder the persistent device-state contract (SEG-007-T042)
 * SS1.3 reserved as `GENESIS_VDP_CRAM_BYTES`, in the same way SEG-007-T103
 * realised `GENESIS_Z80_RAM_BYTES`. Public size fact: Sega, Genesis Technical
 * Overview v1.00 (1991), p. 2 ("64 x 9-bits of CRAM (Color RAM)") and p. 12
 * ("64 9-bit wide color registers"); each color register is accessed as one
 * 16-bit CONTROL/DATA-port word (GTO1 pp. 28-33 CRAM access examples), so the
 * VDP-owned byte storage is 64 * 2 = 128 bytes. Independently corroborated by
 * plutiedev.com "Tiles and palettes" (four palettes of sixteen colors = 64
 * BGR word entries). See
 * docs/references/genesis-vdp-data-port-cpu-write-contract.md.
 */
#define GENESIS_VDP_CRAM_BYTES 128U

/*
 * SEG-007-T108: the documented VSRAM (Vertical Scroll RAM) byte size, realising
 * the SEG-007-T042 SS1.3 named placeholder `GENESIS_VDP_VSRAM_BYTES`. Public
 * size fact: Sega, Genesis Technical Overview v1.00 (1991), p. 12 ("40 10bit
 * words inside the VDP chip"); each word is accessed as one 16-bit
 * CONTROL/DATA-port word, so the VDP-owned byte storage is 40 * 2 = 80 bytes.
 * Independently corroborated by copetti.org "Mega Drive / Genesis Architecture"
 * ("80 B VSRAM"). See
 * docs/references/genesis-vdp-data-port-cpu-write-contract.md.
 */
#define GENESIS_VDP_VSRAM_BYTES 80U

/*
 * SEG-007-T103: the documented 68000-visible Z80 program-RAM size. This
 * realises the exact named placeholder the persistent device-state contract
 * (SEG-007-T042) SS1.2 reserved as `GENESIS_Z80_RAM_BYTES`. Public size fact:
 * Sega, Genesis Technical Overview v1.00 (1991), 68K memory map p. 7 / overview
 * p. 2 ("8 KByte" sound RAM at $A00000); independently reported by Charles
 * MacDonald, Sega Genesis hardware notes v0.8, SS2 ("8k static RAM"). See
 * docs/architecture/genesis-z80-ram-window-compatibility-policy.md.
 */
/* SEG-007-T115: the byte count is now the single shared constant from the
 * translation-time/runtime address-space contract, byte-identical to the
 * former literal 8192; the translation-time device-routing gate and this
 * runtime therefore recognise the same window. */
#define GENESIS_Z80_RAM_BYTES SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES

typedef enum GenesisVdpDmaPhase {
  GENESIS_VDP_DMA_IDLE = 0,
  GENESIS_VDP_DMA_BUSY = 1,
} GenesisVdpDmaPhase;

/* The active DMA source family is explicit: a VRAM fill has no MC68000
   source address and its programmed count is measured in destination bytes.
   SEG-007-T169: despite its name (unchanged to avoid an unrelated rename
   across every existing call site), GENESIS_VDP_DMA_MEMORY_TO_VRAM now
   covers every documented memory-to-target write DMA this runtime arms
   (VRAM, CRAM, or VSRAM) -- the actual destination is `write_target_code`
   below, not implied by this enumerator name. Only GENESIS_VDP_DMA_VRAM_FILL
   remains VRAM-only, per its own bounded fill-engine scope. */
typedef enum GenesisVdpDmaKind {
  GENESIS_VDP_DMA_MEMORY_TO_VRAM = 0,
  GENESIS_VDP_DMA_VRAM_FILL = 1,
} GenesisVdpDmaKind;

typedef struct GenesisVdpDmaState {
  GenesisVdpDmaPhase phase;
  GenesisVdpDmaKind kind;
  /* MC68000 byte address reconstructed from VDP registers #21--#23 for
     the selected memory-to-VRAM DMA mode. */
  uint32_t source_address;
  /* Remaining 16-bit words. A programmed zero length is represented as
     65536, so this deliberately has more than 16 bits. */
  uint32_t remaining_length;
  /* Remaining destination bytes for the selected VRAM-fill mode. This is
     deliberately distinct from remaining_length, whose unit is source words
     in the memory-to-VRAM mode. */
  uint32_t fill_byte_count;
  /* Incremented once for each successful, access-caused word transfer. */
  uint32_t transfer_access_count;
  /* SEG-007-T169: the masked (CD5 removed) CD5-CD0 write-target code armed
     for a GENESIS_VDP_DMA_MEMORY_TO_VRAM transfer (0x01 VRAM, 0x03 CRAM, or
     0x05 VSRAM -- see genesis_vdp_write_target_buffer in runtime.c); the DMA
     progression engine consumes this to select its destination buffer.
     Meaningful only while phase == GENESIS_VDP_DMA_BUSY and
     kind == GENESIS_VDP_DMA_MEMORY_TO_VRAM. Zero-initialized (SS8); the
     VRAM-fill kind never reads this field (its own destination is always
     vram, hardcoded in genesis_vdp_data_port_fill_write). */
  uint8_t write_target_code;
} GenesisVdpDmaState;

/*
 * SEG-007-T081/SEG-007-T091: the persistent VDP register-file/control-port/
 * status model reserved by docs/architecture/genesis-persistent-device-
 * state-and-checkpoint-evidence-contract.md (SEG-007-T042) SS1.3.
 * SEG-007-T081 populated only `status_register`, the one field its own
 * bounded VDP control-port WORD-read selector needed. SEG-007-T091 adds
 * `registers[]`, `control_port_awaiting_second_word`,
 * `control_port_first_word`, `addressed_pointer`, and `auto_increment_value`
 * for its own bounded control-port command-word WRITE selectors (see the VDP
 * addendum to
 * docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md).
    * SEG-007-T084 adds the bounded memory-to-VRAM engine; SEG-007-T098 adds
    * its distinct armed VRAM-fill DATA-port WORD source. CRAM/VSRAM and every
    * other CPU DATA-port access remain out of scope.
 */
typedef struct GenesisVdpState {
  uint16_t registers[GENESIS_VDP_REGISTER_COUNT]; /* Zero-initialized (SS8).
                                Written only by the SEG-007-T091 register-set
                                command-word WRITE selector below (RS4-RS0
                                register number, D7-D0 data byte, GTO1 p. 20
                                "WRITE1: REGISTER SET"). Never read by any
                                currently-implemented selector. */
  uint8_t control_port_awaiting_second_word; /* 1 iff a first control-port
                                word has been received and this state is
                                awaiting the second word of the documented
                                two-word address-set command (GTO1 p. 20
                                "WRITE2: ADDRESS SET"); 0 otherwise.
                                Zero-initialized (SS8). */
  uint16_t control_port_first_word; /* The verbatim first word of a pending
                                two-word address-set command, meaningful only
                                while control_port_awaiting_second_word == 1.
                                The first word's low half (A13-A0, CD1-CD0) is
                                additionally applied to addressed_pointer /
                                data_port_transfer_code immediately when it is
                                written (Mode-5 VDP two-halves command model);
                                a DATA-port access cancels a still-pending
                                sequence (shared write-pending flip-flop) and
                                clears this back to 0. Zero-initialized (SS8). */
  uint32_t addressed_pointer; /* The current 16-bit VRAM/CRAM/VSRAM address
                                (0x0000-0xFFFF) the completed non-DMA
                                two-word address-set command most recently
                                set (GTO1 p. 20/p. 27-33), stored widened to
                                uint32_t per T042 SS1.3's own field type. The
                                first control word already merges its A13-A0
                                bits here before the second word arrives
                                (Mode-5 two-halves command model); the bounded
                                CPU DATA-port WRITE path consumes and
                                auto-increments it. Zero-initialized (SS8). */
  uint16_t auto_increment_value; /* Mirrors the current value of REGISTER
                                #15 (INC7-INC0), the documented VRAM/CRAM/
                                VSRAM address auto-increment value (GTO1
                                p. 28 "VRAM address is increased by the value
                                of REGISTER #15"; p. 37 "REG. #15" diagram).
                                Set only as a named post-access side effect
                                of a register-set command-word WRITE to
                                register #15 -- see genesis_vdp_access below.
                                 Zero-initialized (SS8); consumed only by the
                                 bounded armed VRAM-fill DATA-port WORD write. */
  uint16_t status_register; /* Zero-initialized (SS8); never mutated by any
                                 currently-implemented write/DMA/interrupt
                                 path -- see genesis_vdp_access below. */
  uint8_t data_port_transfer_code; /* SEG-007-T108: the CD5-CD0 transfer code
                                (GTO1 p. 20/p. 27 access-mode table) recorded
                                by the most recently accepted non-DMA two-word
                                address-set command. Meaningful only while
                                data_port_transfer_code_valid == 1. Consumed
                                only by the bounded CPU DATA-port ($C00000)
                                WRITE path in genesis_vdp_access below (the one
                                reached shape: CRAM WRITE, code 0x03).
                                Zero-initialized (SS8). */
  uint8_t data_port_transfer_code_valid; /* SEG-007-T108: 1 iff a non-DMA
                                two-word address-set command has selected a
                                data_port_transfer_code since reset; 0
                                otherwise. Zero-initialized (SS8) -- so a CPU
                                DATA-port write with no code selected fails
                                closed. */
  uint8_t vram[GENESIS_VDP_VRAM_BYTES]; /* VDP-owned byte storage for the
                                 selected DMA target only. CPU DATA-port
                                 access remains SEG-007-T083's scope. */
  uint8_t cram[GENESIS_VDP_CRAM_BYTES]; /* SEG-007-T108: VDP-owned CRAM byte
                                 storage, big-endian halfword order. Written
                                 only by the bounded CPU DATA-port CRAM WRITE
                                 path in genesis_vdp_access below; the current
                                 VDP address wraps modulo GENESIS_VDP_CRAM_BYTES
                                 within it (a labeled project compatibility
                                 policy -- see the reference contract). CRAM
                                 DATA-port reads and CRAM DMA remain out of
                                 scope. Zero-initialized (SS8). */
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES]; /* SEG-007-T108: VDP-owned VSRAM byte
                                 storage, big-endian halfword order. Written
                                 only by the bounded CPU DATA-port VSRAM WRITE
                                 path in genesis_vdp_access below; the current
                                 VDP address wraps modulo GENESIS_VDP_VSRAM_BYTES
                                 within it (a labeled project compatibility
                                 policy). VSRAM DATA-port reads and VSRAM DMA
                                 remain out of scope. Zero-initialized (SS8). */
  GenesisVdpDmaState dma;
} GenesisVdpState;

/*
 * SEG-007-T102/SEG-007-T103: the persistent 68k-side Z80 bus-arbitration latch
 * reserved by T042 contract SS1.1/SS1.2 as `GenesisDeviceState.z80_bus`.
 * SEG-007-T102 added the three arbitration-latch booleans the canonical Sonic
 * startup route exercises through the $A11100/$A11200 bus-arbitration control
 * registers -- see
 * docs/architecture/genesis-z80-bus-arbitration-compatibility-policy.md.
 * SEG-007-T103 adds the `z80_ram[GENESIS_Z80_RAM_BYTES]` field T042 SS1.2
 * reserved and T102 deliberately deferred: it is the flat 68000-visible Z80
 * program-RAM window ($A00000) backing storage, zero-initialised per T042 SS8.
 * See docs/architecture/genesis-z80-ram-window-compatibility-policy.md.
 *
 * DELIBERATE DEVIATION from T042 SS1.2's field list: the `busreq_status_read_count`
 * device-step counter is still NOT added -- it is unused because the
 * bus-arbitration compatibility policy grants the bus immediately (there is no
 * "after N reads" progression to count). Per this project's "add abstractions
 * only when a current target exercises them" rule (the project charter, Scope
 * Discipline), a later task adds it when it first needs it, following T042
 * SS1.1's extension discipline.
 */
typedef struct GenesisZ80BusState {
  uint8_t bus_requested;  /* 1 iff the 68000 has asserted BUSREQ ($A11100
                             D8 for a WORD access / D0 for a BYTE access = 1,
                             "BUSREQ REQUEST", GTO1 v1.00 p. 76) and not yet
                             cancelled it; 0 otherwise. Zero-initialized (T042
                             SS8): at power-on the 68000 already has the Z80
                             bus (GTO1 p. 76), so "not requested" is the
                             correct zero default. */
  uint8_t bus_granted;    /* 1 iff the Z80 bus is currently granted to the
                             68000 side. Under the compatibility policy this
                             tracks `bus_requested` exactly (deterministic
                             immediate grant -- no Z80 core is executing to
                             contend). */
  uint8_t reset_asserted; /* 1 iff the 68000 currently holds the Z80 /RESET
                             line asserted ($A11200 D8/D0 = 0, "RESET REQUEST",
                             GTO1 p. 76); 0 when released (= 1, "RESET
                             CANCEL"). Zero-initialized (T042 SS8). GTO1 p. 76
                             notes the Z80 is reset during the console's own
                             power-on-reset sequence; that initial-value detail
                             is deliberately NOT modeled here because no Z80
                             core exists and the startup route drives this
                             register explicitly (policy doc, "intentional
                             deviations"). */
  uint8_t z80_ram[GENESIS_Z80_RAM_BYTES]; /* SEG-007-T103: the flat
                             68000-visible Z80 program-RAM window backing
                             storage (window base $A00000, GENESIS_Z80_RAM_BYTES
                             bytes). Zero-initialized per T042 SS8. Read/written
                             only by genesis_z80_ram_window_access as a flat
                             byte array -- no Z80 core, decode, or instruction
                             fetch. See
                             docs/architecture/genesis-z80-ram-window-compatibility-policy.md. */
} GenesisZ80BusState;

/*
 * SEG-007-T109: minimal persistent PSG (SN76489) command-latch state. This is
 * a new genuinely-stateful subsystem (sound generation) added to
 * `GenesisDeviceState` per T042 SS1.1's extension discipline -- it does NOT
 * force PSG state into the VDP struct even though the PSG port is co-located in
 * the recognised VDP address window ($C00000..$C0001F).
 *
 * This models ONLY the CPU-visible register-latch bookkeeping of the SN76489
 * write protocol. It is an explicitly labelled, replaceable PROJECT
 * COMPATIBILITY POLICY (see
 * docs/architecture/genesis-psg-sn76489-port-write-compatibility-policy.md),
 * NOT verified hardware behaviour. There is NO audio synthesis, NO
 * tone/noise oscillator, NO attenuation-ramp or frequency-divider emulation,
 * NO PSG ready/busy line, and NO Z80 view of the chip.
 *
 * Command-byte format (SN76489), triangulated across SMS Power
 * "Development/SN76489", plutiedev.com "psg", and Charles MacDonald's Sega
 * Genesis hardware notes:
 *   - LATCH byte  %1cctdddd : cc = channel 0..3, t = 1 volume / 0 tone-noise,
 *                             dddd = 4-bit data (low 4 bits of a tone period,
 *                             a 4-bit attenuation, or a 3-bit noise control).
 *   - DATA byte   %0-DDDDDD : DDDDDD updates the last-latched register -- upper
 *                             6 bits of a 10-bit tone period, or the low bits
 *                             of an attenuation / noise control.
 *   - Noise register (channel 3, t = 0): bit 2 = feedback mode
 *                             (0 periodic / 1 white), bits 1-0 = shift rate.
 * Zero-initialised per T042 SS8.
 */
typedef struct GenesisPsgState {
  uint8_t latched_channel;  /* 0..3: the channel selected by the most recent LATCH byte. */
  uint8_t latched_volume;   /* 1 iff that LATCH byte selected the volume register; 0 = tone/noise. */
  uint8_t latch_valid;      /* 1 once any LATCH byte has been seen since reset; a DATA byte with
                               this still 0 fails closed (no register is latched yet). */
  uint16_t tone_period[3];  /* 10-bit period for tone channels 0..2 (0x000..0x3FF). */
  uint8_t attenuation[4];   /* 4-bit attenuation for channels 0..3 (0 = loudest, 15 = silent). */
  uint8_t noise_control;    /* 3-bit noise register for channel 3: bit 2 feedback, bits 1-0 rate. */
} GenesisPsgState;

/* Persistent interrupt/checkpoint timing state.  It is mutated only through
 * the routed VDP status-read policy or the dispatcher checkpoint hook. */
typedef struct GenesisInterruptState {
  uint8_t vblank_pending;
  uint32_t vblank_status_read_count;
  uint32_t vblank_transition_count;
  uint8_t checkpoint_entered;
  uint32_t vblank_transition_count_at_checkpoint_entry;
} GenesisInterruptState;

/*
 * SEG-007-T081/SEG-007-T102/SEG-007-T109: T042 SS1.1's `GenesisDeviceState`
 * composition. `z80_bus` (SEG-007-T102), `vdp` (SEG-007-T081+) and `psg`
 * (SEG-007-T109), controller I/O (SEG-007-T121), and interrupt/checkpoint
 * state (SEG-007-T131) are populated.  The latter remains storage-only here;
 * its routed VDP observation and dispatcher hook are owned by runtime.c.
 */
/*
 * SEG-007-T121: minimal persistent controller-I/O GPIO-register latch state.
 * This models ONLY the CPU-visible data/direction register latches of the
 * three general-purpose I/O ports (DATA1..DATA3 $A10003/5/7, CTRL1..CTRL3
 * $A10009/B/D -- GTO1 p. 72-75). It is an explicitly labelled, replaceable
 * PROJECT COMPATIBILITY POLICY (see the SEG-007-T121 section of
 * docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md),
 * NOT verified Genesis hardware behaviour. There is NO controller input
 * sourcing, NO TH/TR/TL handshake, NO serial-shift register, NO TH interrupt,
 * and NO peripheral device model. A write is a deterministic latched store;
 * no currently-implemented read selector consumes these fields (the read
 * selectors keep returning their own SEG-007-T020/T038/T079/T111 policy
 * constants), exactly like the VDP `registers[]` write-only precedent.
 * Zero-initialised per T042 SS8: at power-on both the data and direction
 * latches read as 0 (all pins input, no value driven).
 */
typedef struct GenesisControllerIoState {
  uint8_t data[3]; /* DATA1..DATA3 output-data latch (full 8 bits stored; the
                      CTRL direction bits govern which pins are physically
                      driven, which this project does not model). */
  uint8_t ctrl[3]; /* CTRL1..CTRL3 latch: bit 7 = TH-INT enable (interrupts
                      are not modelled), bits 6-0 = per-pin direction
                      (1 = output). CTRL3 ($A1000D) is also the T111/T038 read
                      selector address; a read there still returns the unchanged
                      read-selector constant, never this latch. */
} GenesisControllerIoState;

typedef struct GenesisDeviceState {
  GenesisZ80BusState z80_bus;
  GenesisVdpState vdp;
  GenesisPsgState psg;
  GenesisControllerIoState controller_io;
  GenesisInterruptState interrupt;
} GenesisDeviceState;
#endif

/*
 * SEG-007-T252 / ADR-0040: the former SEG-007-T107 `GenesisLoopProgressNote`,
 * SEG-007-T211/ADR-0035 `GenesisLoopCompletionNote`, SEG-007-T155/ADR-0017
 * `GenesisDataProgressNote` types (and the `GenesisRuntime` fields that held
 * them), and the `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` ceiling, have all been
 * removed along with the generated-runtime progress watchdog itself. See
 * docs/decisions/0040-runner-owned-dispatch-allowance-replaces-generated-
 * runtime-progress-watchdog.md.
 */

/* NTSC master-clock timing.  This is integer guest time, never host time. */
#define GENESIS_NTSC_MASTER_TICKS_PER_SECOND UINT64_C(53693175)
#define GENESIS_M68K_CYCLE_MASTER_TICKS UINT32_C(7)
#define GENESIS_NTSC_MASTER_TICKS_PER_LINE UINT32_C(3420)
#define GENESIS_NTSC_LINES_PER_FRAME UINT32_C(262)
#define GENESIS_NTSC_VBLANK_ONSET_LINE UINT32_C(224)
#define GENESIS_NTSC_MASTER_TICKS_PER_FRAME \
  ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_LINE * (uint64_t)GENESIS_NTSC_LINES_PER_FRAME)
#define GENESIS_NTSC_VBLANK_ONSET_TICK \
  ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_LINE * (uint64_t)GENESIS_NTSC_VBLANK_ONSET_LINE)

/*
 * SEG-007-T047 / ADR-0020 §2: production-runtime-only VBlank scheduler state.
 * Declared as a `GenesisRuntime`-level sibling of `devices` (see below). It
 * is NEVER a `GenesisDeviceState`/`GenesisInterruptState` member and is never
 * evidence-bearing (ADR-0020 §10): keeping it out of `GenesisDeviceState`
 * keeps the checkpoint-evidence bundle schema unchanged.
 */
typedef struct GenesisInterruptScheduler {
  uint64_t master_ticks; /* checked, monotonically increasing guest time */
} GenesisInterruptScheduler;

/* One bit per aligned work-RAM word records whether the six-byte exception
 * frame beginning there was constructed for IRQ6.  The basic MC68000 frame
 * itself carries only SR and PC, so this runtime-private provenance is needed
 * to keep the synthetic IRQ6 post-RTE grace out of synchronous exceptions.
 * Bit addressing retains origins independently for nested frames. */
#define GENESIS_EXCEPTION_FRAME_ORIGIN_BYTES (UINT32_C(65536) / UINT32_C(16))

/* SEG-007-T050: the one seam this translation unit uses to reach T049's
 * VDP composition owner (platforms/genesis/runtime/vdp_render.c,
 * genesis_vdp_produce_frame) WITHOUT this file (runtime.c) or this header
 * gaining a hard compile/link-time dependency on vdp_render.{h,c}. Mirrors
 * the existing GenesisDispatchFunction pattern above: a plain function
 * pointer typedef, no vdp_render.h include, no vdp_render.c symbol
 * referenced by name from within runtime.c's own object code. This keeps
 * every one of this project's many other direct `cc ... runtime.c ...`
 * test/tool invocations (CPU-semantics/generated-C harnesses that never
 * touch rendering) linkable exactly as before -- passing NULL for this
 * parameter (see genesis_extract_checkpoint_evidence below) skips frame
 * production entirely, with zero link-time cost. A caller that does want a
 * real frame passes `genesis_vdp_produce_frame` itself (its signature is
 * already identical) after `#include "vdp_render.h"` at its own call site. */
typedef int (*GenesisFrameProducerFunction)(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                            const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                            const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                                            const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                            GenesisFrameArtifact *frame_out);

/* SEG-007-T255: optional, non-semantic, host-owned live-frame observer.
 * `producer` has the GenesisFrameProducerFunction shape (normally
 * genesis_vdp_produce_frame). `latest` is one caller-owned artifact that always
 * holds the newest completed frame; `sequence` counts publications (starts at
 * the caller's value, normally 0) and only advances on a successful publish.
 * Rule for multiple boundaries crossed by one retirement: the retirement
 * renders the (single) current VDP state once and publishes one frame (latest
 * frame wins; the sequence advances by exactly one). Absent (NULL) observer ==
 * exactly the unobserved headless behavior. Never serialized into
 * checkpoint/guest state; never read by guest semantics. */
typedef struct GenesisLiveFrameObserver {
  GenesisFrameProducerFunction producer;
  GenesisFrameArtifact *latest;
  uint64_t sequence;
} GenesisLiveFrameObserver;

/* Fixed pure-C11 persistent state ABI for generated Genesis startup blocks. */
typedef struct GenesisRuntime {
  uint32_t d[8];
  uint32_t a[8];
  /* SEG-007-T085: persistent User Stack Pointer. The bounded startup policy
     supports only MOVE An,USP, with no user-mode/privilege/exception model. */
  uint32_t usp;
  uint16_t sr;
  uint32_t pc;
  uint8_t work_ram[65536];
  /* SEG-007-T077: zero-valued (NULL/0) by every existing
     `GenesisRuntime runtime = {0};` construction, so a generated program
     that never proves an owned cartridge-data region is provably
     unaffected -- genesis_route_access's owned-region scan below is then
     always a zero-iteration loop, identical to today's unconditional
     fail-closed ROM-read behavior. */
  const GenesisOwnedCartridgeRegion *owned_regions;
  uint32_t owned_region_count;
  /* SEG-007-T081: zero-initialized identically to every other GenesisRuntime
     field (T042 SS8); see GenesisDeviceState above. */
  GenesisDeviceState devices;
  /* SEG-007-T252 / ADR-0040: the former SEG-007-T107 `loop_progress`,
     SEG-007-T211/ADR-0035 `loop_completion`, and SEG-007-T155/ADR-0017
     `data_progress` watchdog-note fields have been removed along with the
     generated-runtime progress watchdog itself. */
  /* SEG-007-T047 / ADR-0020 §2: zero-initialized by every existing
     `GenesisRuntime runtime = {0};` construction. Sibling of `devices`, never a
     member of it. */
  GenesisInterruptScheduler scheduler;
  uint8_t exception_frame_irq6_origin[GENESIS_EXCEPTION_FRAME_ORIGIN_BYTES];
  /* SEG-007-T047 / ADR-0020 §6: the build-time-resolved MC68000 level-6
     interrupt autovector handler entry address (the long word at vector-table
     offset 0x78 in the mapped cartridge image). Resolved entirely at static
     discovery / emission time and written once by generated `main` before
     genesis_runtime_run; NEVER fetched or decoded at runtime.
     `irq6_handler_present == 0` (the zero default) means no generated program
     established an IRQ6 handler, and interrupt admission never fires. */
  uint32_t irq6_handler_entry;
  uint8_t irq6_handler_present;
  /* SEG-007-T222 / ADR-0037: the build-time-resolved MC68000 vector-5
     (Zero Divide) handler entry address (the long word at vector-table
     offset 0x14), mirroring `irq6_handler_entry`'s own resolution/ownership
     rule exactly (resolved at static discovery / emission time, written once
     by generated `main`, NEVER fetched or decoded at runtime).
     `divide_by_zero_handler_present == 0` means no generated program
     established a divide-by-zero handler, and a DIVS.W/DIVU.W divisor==0
     fails closed via GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION. */
  uint32_t divide_by_zero_handler_entry;
  uint8_t divide_by_zero_handler_present;
  /* SEG-007-T252 / ADR-0040 correction: a fixed-size, no-dynamic-allocation
     circular buffer of the latest GENESIS_RECENT_PC_HISTORY_CAPACITY (64)
     architectural PC values, for LOCAL DIAGNOSTIC USE ONLY. Convention
     (exactly one, documented here so it is never mixed with the alternative):
     `genesis_runtime_step` records `runtime->pc` -- the PC ABOUT TO DISPATCH
     -- at the very top of its own call, before `dispatch()` runs. This is
     diagnostic-only state, NOT guest semantics: it must never affect dispatch
     selection, guest state, VBlank/IRQ/device timing, generation, or become
     runtime-confirmed target authority, and recording it must never change
     behavior based on the recorded values (it is a pure side-channel append).
     It is EXCLUDED from every stable serialization -- genesis_sha_options,
     genesis_write_sanitized_report, genesis_write_full_report (which never
     emits it, under any result kind), and checkpoint-evidence bundling all
     ignore it entirely; it is never part of the wire ABI. The dedicated,
     separate `genesis_write_ephemeral_pc_history` writer emits it (as a bare
     JSON array, not part of any report object) ONLY when
     `result->kind == GENESIS_RUNNER_RESOURCE_LIMIT`, for the generated
     bridge's `--ephemeral-report-fd` argv path and
     `tools/genesis_startup_bridge.py`'s own private `ephemeral_frontier()`
     diagnostic assembly to read and then discard -- never asserted present by
     any schema validator, never printed to the sanitized stdout report, and
     never reachable via `--full-report-path`/`--full-report-fd`. */
  uint32_t recent_pc_history[64];
  uint8_t recent_pc_history_count;   /* number of valid entries, saturates at 64 */
  uint8_t recent_pc_history_next;    /* circular write index, wraps at 64 */
  /* SEG-007-T255: host-owned optional observer (NULL = absent); non-semantic. */
  GenesisLiveFrameObserver *live_frame_observer;
} GenesisRuntime;

/* SEG-007-T252 / ADR-0040 correction: capacity of `GenesisRuntime.
   recent_pc_history` -- diagnostic-only, see the field's own doc comment. */
#define GENESIS_RECENT_PC_HISTORY_CAPACITY 64

typedef enum GenesisAccessWidth {
  GENESIS_ACCESS_BYTE = 1,
  GENESIS_ACCESS_WORD = 2,
  GENESIS_ACCESS_LONG = 4,
} GenesisAccessWidth;

typedef enum GenesisAccessDirection {
  GENESIS_ACCESS_READ = 0,
  GENESIS_ACCESS_WRITE = 1,
} GenesisAccessDirection;

typedef enum GenesisStopClass {
  GENESIS_STOP_UNSUPPORTED_CPU_FORM = 1,
  GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS = 2,
  GENESIS_STOP_UNSUPPORTED_MEMORY_REGION = 3,
  GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET = 4,
  GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET = 5,
  GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT = 6,
  GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY = 7,
  GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED = 8,
  /* ADR 0013 Decision §6: a static-discovery-stage prefix boundary, distinct
   * from GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED (ADR 0007's unrelated
   * runtime watchdog class). */
  GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9,
  GENESIS_STOP_C4_LOWERING_GAP = 10,
} GenesisStopClass;

typedef enum GenesisCpuVariant { GENESIS_CPU_MC68000 = 1 } GenesisCpuVariant;

typedef struct GenesisInstructionProvenance {
  GenesisCpuVariant cpu_variant;
  uint32_t source_address;
  uint64_t image_offset;
  uint8_t primary_bytes[2];
  uint32_t length;
} GenesisInstructionProvenance;

#ifndef SEGARECOMP_RUNTIME_GENESIS_CHECKPOINT_EVIDENCE_H
typedef enum GenesisBusKind {
  GENESIS_BUS_INSTRUCTION_READ = 1,
  GENESIS_BUS_DATA_READ = 2,
  GENESIS_BUS_DATA_WRITE = 3,
  GENESIS_BUS_STACK_READ = 4,
  GENESIS_BUS_STACK_WRITE = 5,
} GenesisBusKind;

typedef enum GenesisBusRegion {
  GENESIS_REGION_RAW_CARTRIDGE_ROM = 1,
  GENESIS_REGION_SYNTHETIC_WORK_RAM = 2,
} GenesisBusRegion;

/* SEG-007-T113 / ADR 0008: the generated-runtime provenance ABI's
 * raw-instruction-bytes capacity is the single shared constant from the
 * translation-time/runtime address-space contract. Static emitter constants,
 * this emitted header layout, and the runtime validator therefore agree
 * byte-for-byte; anything longer still fails closed before any state mutation. */
#define GENESIS_MAX_RAW_BYTES SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES
typedef struct GenesisBusAccess {
  uint64_t ordinal;
  GenesisBusKind kind;
  uint32_t address;
  uint8_t raw_bytes[GENESIS_MAX_RAW_BYTES];
  uint8_t raw_byte_count;
  GenesisBusRegion region;
} GenesisBusAccess;
#endif

#ifndef GENESIS_MAX_NAME_LENGTH
#define GENESIS_MAX_NAME_LENGTH 64U
#endif
typedef struct GenesisMappingClaim {
  char name[GENESIS_MAX_NAME_LENGTH];
  uint8_t name_length;
  uint32_t target_begin;
  uint32_t target_end;
  uint64_t image_begin;
  uint64_t image_end;
} GenesisMappingClaim;

#define GENESIS_MAX_MAPPING_CLAIMS 4U
#define GENESIS_MAX_BUS_ACCESSES 4U

typedef enum GenesisDiagnosticCategory {
  GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS = 1,
  GENESIS_DIAG_UNMAPPED_INSTRUCTION_ADDRESS = 2,
  GENESIS_DIAG_TRUNCATED_INSTRUCTION = 3,
  GENESIS_DIAG_ILLEGAL_INSTRUCTION = 4,
  GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION = 5,
  GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM = 6,
  GENESIS_DIAG_ODD_DIRECT_TARGET = 7,
  GENESIS_DIAG_CONFLICTING_ADDRESS_MAPPING = 8,
  GENESIS_DIAG_INVALID_ADDRESS_MAPPING = 9,
  GENESIS_DIAG_UNMAPPED_DIRECT_TARGET = 10,
  GENESIS_DIAG_MID_INSTRUCTION_DIRECT_TARGET = 11,
  GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE = 12,
  GENESIS_DIAG_INVALID_FRONTEND_IMAGE_SOURCE_ID = 13,
  GENESIS_DIAG_FRONTEND_IMAGE_BYTE_LENGTH_MISMATCH = 14,
  GENESIS_DIAG_INVALID_MAPPING_CLAIM = 15,
  GENESIS_DIAG_VECTOR_FIXTURE_ID_MISMATCH = 16,
  GENESIS_DIAG_VECTOR_IMAGE_SHA256_MISMATCH = 17,
  GENESIS_DIAG_VECTOR_CPU_VARIANT_MISMATCH = 18,
  GENESIS_DIAG_VECTOR_EXECUTION_ENTRY_SPACE_MISMATCH = 19,
  GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_INSTRUCTION = 20,
  GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_BLOCK = 21,
  GENESIS_DIAG_EXECUTION_ENTRY_NOT_DISCOVERED_BLOCK_START = 22,
  GENESIS_DIAG_VECTOR_BLOCK_INSTRUCTION_COUNT_MISMATCH = 23,
  GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT = 24,
  GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS = 25,
  GENESIS_DIAG_ROM_WRITE_PROHIBITED = 26,
  GENESIS_DIAG_UNMAPPED_DATA_ACCESS = 27,
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO = 28,
  GENESIS_DIAG_INVALID_STACK_ALIGNMENT = 29,
  GENESIS_DIAG_INVALID_STACK_RANGE = 30,
  GENESIS_DIAG_RETURN_CONTEXT_MISSING = 31,
  GENESIS_DIAG_RETURN_TARGET_MISMATCH = 32,
  GENESIS_DIAG_STARTUP_GRAPH_MISMATCH = 33,
  GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED = 34,
  GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY = 35,
  GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED = 36,
  GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET = 37,
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP = 38,
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS = 39,
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM = 40,
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG = 41,
  GENESIS_DIAG_C4_LOWERING_GAP = 42,
  /* SEG-007-T047 / ADR-0020 §5 step 2 / §8: an eligible IRQ6 admission whose
     six-byte MC68000 exception frame cannot be constructed fail-closed
     (supervisor bit clear at admission, A7 underflow/wrap, or the frame extent
     is not entirely a writable, aligned work-RAM range). Paired with
     GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT. No CPU/device state
     is mutated and no partial frame is written before this stop. */
  GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT = 43,
  /* SEG-007-T171: the YM2612 FM-synthesis register window ($A04000-$A04003)
     for every access this project does not (yet) support -- only the
     runtime-confirmed PART-I status-port BYTE read is accepted (see
     genesis_ym2612_access); every other port/direction/width in the window
     fails closed with this category. */
  GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_YM2612 = 44,
  /* SEG-007-T174 / ADR-0024: Tier 2's own fail-closed diagnostic. Paired with
     the existing GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET stop class (no new
     stop class): a computed indirect JMP/JSR target whose control EA was the
     recognized pc_index8 form but whose value-flow proof failed at
     generation time (so no Tier-1 M68kIndirectTargetEaSet exists) computed a
     runtime EA that is NOT a member of the generation-time
     EmittedCodeAddressSet. Distinct from
     GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE (Tier 1's own non-member
     diagnostic for the SAME stop class) so a sanitized report can always
     tell which tier's membership guard actually fired. */
  GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED = 45,
  /* SEG-007-T222 / ADR-0037: a DIVS.W/DIVU.W divide-by-zero whose synchronous
     vector-5 exception delivery cannot be constructed fail-closed -- either
     no build-resolved divide-by-zero handler is installed
     (`divide_by_zero_handler_present == 0`) or the six-byte exception frame
     cannot be constructed (supervisor bit clear, A7 underflow/wrap, or the
     frame extent is not entirely writable/aligned work RAM), mirroring
     GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT's own frame-
     construction failure shape for the unrelated IRQ6 path. Paired with
     GENESIS_STOP_UNSUPPORTED_CPU_FORM (a synchronous CPU-exception delivery
     failure, not a device/scheduling one). No CPU/device state is mutated
     and no partial frame is written before this stop. */
  GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION = 46,
  GENESIS_DIAG_UNACCOUNTED_INSTRUCTION_TIMING = 47,
  GENESIS_DIAG_VIRTUAL_TIME_OVERFLOW = 48,
} GenesisDiagnosticCategory;

typedef struct GenesisProvenance {
  uint8_t has_instruction_provenance;
  GenesisInstructionProvenance instruction;
  uint8_t has_access;
  uint32_t access_address;
  GenesisAccessWidth access_width;
  GenesisAccessDirection access_direction;
  uint8_t mapping_claim_count;
  GenesisMappingClaim mapping_claims[GENESIS_MAX_MAPPING_CLAIMS];
  uint8_t bus_access_count;
  GenesisBusAccess bus_accesses[GENESIS_MAX_BUS_ACCESSES];
} GenesisProvenance;

/*
 * Translation-time CPU frontier metadata. It is separate from a runtime stop
 * so stops contain only the failure and source provenance. Generated C may
 * select only these normalized literals; it never receives target bytes.
 */
typedef enum GenesisCpuDimensions {
  GENESIS_CPU_DIMENSIONS_NONE = 0,
  GENESIS_CPU_DIMENSIONS_NOP = 1,
  GENESIS_CPU_DIMENSIONS_STOP_IMMEDIATE_WORD = 2,
  GENESIS_CPU_DIMENSIONS_RESET = 3,
  GENESIS_CPU_DIMENSIONS_MOVE_AN_TO_USP = 4,
} GenesisCpuDimensions;
/*
 * SEG-007-T142 correction: one literal per distinct (ir_kind, M68kC4GapClass)
 * shape the shared build-time classifier (classify_m68k_c4_gap_shapes in
 * libs/codegen/c11/src/frontend.cpp) can currently produce -- derived from the
 * minimal necessary subset of ADR-0015 Decision 7's seven dimensions
 * (ir_kind + gap only; no dimension that never varies across the
 * represented shapes is carried). This is a finite, exhaustive, explicitly
 * enumerated whitelist: there is deliberately no generic catch-all literal.
 * A future unlowerable operation with no explicitly represented shape here
 * fails emission closed (the classifier/emitter reject translation) rather
 * than reporting ambiguous "other" evidence.
 */
typedef enum GenesisC4LoweringDimensions {
  GENESIS_C4_LOWERING_DIMENSIONS_NONE = 0,
  /* missing_dispatcher: ir_kind has no C4 lowering body at all. */
  GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_NE_SHORT_MISSING_DISPATCHER = 1,
  GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_ALWAYS_SHORT_MISSING_DISPATCHER = 2,
  GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_MISSING_DISPATCHER = 3,
  GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_IMMEDIATE_MISSING_DISPATCHER = 4,
  GENESIS_C4_LOWERING_DIMENSIONS_ADD_IMMEDIATE_MISSING_DISPATCHER = 5,
  GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_DISPATCHER = 6,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_DISPATCHER = 7,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_DISPATCHER = 8,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_DISPATCHER = 9,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_DISPATCHER = 10,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_DISPATCHER = 11,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_DISPATCHER = 12,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_DISPATCHER = 13,
  GENESIS_C4_LOWERING_DIMENSIONS_WRITE_SWAP_MISSING_DISPATCHER = 14,
  GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_WORD_MISSING_DISPATCHER = 15,
  GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_LONG_MISSING_DISPATCHER = 16,
  GENESIS_C4_LOWERING_DIMENSIONS_PUSH_EFFECTIVE_ADDRESS_MISSING_DISPATCHER = 17,
  GENESIS_C4_LOWERING_DIMENSIONS_LINK_FRAME_MISSING_DISPATCHER = 18,
  GENESIS_C4_LOWERING_DIMENSIONS_UNLINK_FRAME_MISSING_DISPATCHER = 19,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_DISPATCHER = 20,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_DISPATCHER = 21,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_DISPATCHER = 22,
  GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_REGISTER_MISSING_DISPATCHER = 23,
  GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_MEMORY_MISSING_DISPATCHER = 24,
  /*
   * There is deliberately no missing_routing literal: a statically
   * foldable-but-unroutable MOVEM source (M68kC4GapClass::missing_routing)
   * is advisory-only in the preflight inventory. The emitter still lowers
   * that source through the ordinary runtime genesis_route_access call, so
   * its eventual ROM-read failure is the existing runtime's own
   * GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY defensive stop, never an
   * emitted GENESIS_STOP_C4_LOWERING_GAP -- this shape never reaches
   * c4_lowering_dimension_literal.
   */
  /* requires_architecture_decision: a deferred auto-update address commit. */
  GENESIS_C4_LOWERING_DIMENSIONS_ADD_AUTO_UPDATE = 26,
  GENESIS_C4_LOWERING_DIMENSIONS_CLR_AUTO_UPDATE = 27,
  GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_AUTO_UPDATE = 28,
  GENESIS_C4_LOWERING_DIMENSIONS_ADDA_AUTO_UPDATE = 29,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBA_AUTO_UPDATE = 30,
  GENESIS_C4_LOWERING_DIMENSIONS_CMPA_AUTO_UPDATE = 31,
  GENESIS_C4_LOWERING_DIMENSIONS_CMP_AUTO_UPDATE = 45,
  GENESIS_C4_LOWERING_DIMENSIONS_CMPI_AUTO_UPDATE = 46,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_AUTO_UPDATE = 32,
  GENESIS_C4_LOWERING_DIMENSIONS_TST_AUTO_UPDATE = 33,
  GENESIS_C4_LOWERING_DIMENSIONS_ANDI_AUTO_UPDATE = 34,
  /* SEG-007-T167: the logical family (AND/OR/EOR + ORI/EORI) has no
   * deferred-address-commit contract; an auto-updating operand is declined. */
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_AUTO_UPDATE = 50,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_AUTO_UPDATE = 51,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_AUTO_UPDATE = 52,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_AUTO_UPDATE = 53,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_AUTO_UPDATE = 54,
  /* SEG-007-T168: NOT (logical complement) shares the sibling logical
   * family's declined-auto-update shape -- no deferred-address-commit
   * contract for this family, matching AND/OR/EOR/ANDI/ORI/EORI. */
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_AUTO_UPDATE = 60,
  /* missing_fact: represented, statically foldable, but no retained fact. */
  GENESIS_C4_LOWERING_DIMENSIONS_MOVE_MISSING_FACT = 35,
  GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_MISSING_FACT = 36,
  GENESIS_C4_LOWERING_DIMENSIONS_ADD_MISSING_FACT = 37,
  GENESIS_C4_LOWERING_DIMENSIONS_ADDA_MISSING_FACT = 38,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBA_MISSING_FACT = 39,
  GENESIS_C4_LOWERING_DIMENSIONS_CMPA_MISSING_FACT = 40,
  GENESIS_C4_LOWERING_DIMENSIONS_CMP_MISSING_FACT = 47,
  GENESIS_C4_LOWERING_DIMENSIONS_CMPI_MISSING_FACT = 48,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_MISSING_FACT = 41,
  GENESIS_C4_LOWERING_DIMENSIONS_TST_MISSING_FACT = 42,
  GENESIS_C4_LOWERING_DIMENSIONS_CLR_MISSING_FACT = 43,
  GENESIS_C4_LOWERING_DIMENSIONS_ANDI_MISSING_FACT = 44,
  /* SEG-007-T153: ADDQ read-modify-write to a statically foldable absolute
   * destination with no retained resolver fact (mirrors ADD_MISSING_FACT). */
  GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_FACT = 49,
  /* SEG-007-T167: represented logical-family instruction with a statically
   * foldable memory operand and no retained resolver fact. */
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_FACT = 55,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_FACT = 56,
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_FACT = 57,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_FACT = 58,
  GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_FACT = 59,
  /* SEG-007-T168: represented NOT with a statically foldable memory
   * destination and no retained resolver fact (mirrors CLR_MISSING_FACT,
   * except NOT is read-modify-write so it needs both destination_read and
   * destination_write, exactly like ADD's own destination fact shape). */
  GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_MISSING_FACT = 61,
  /* SEG-007-T170: the C4 subtract-family missing-dispatcher batch. SUB/SUBI
   * share ADD/ADDI's two-operand shape, but this family's shared lowering
   * body still uses the pre-add-family-fix naive auto-update technique
   * (like its already-represented SUBA/SUBQ siblings), so an auto-updating
   * operand is declined instead of deferred-committed. */
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_AUTO_UPDATE = 62,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_AUTO_UPDATE = 63,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_FACT = 64,
  GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_FACT = 65,
  /* SEG-007-T209: BCHG/BCLR/BSET share BTST's declined-auto-update shape --
   * this family also carries no deferred-address-commit contract. */
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_AUTO_UPDATE = 66,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_AUTO_UPDATE = 67,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_AUTO_UPDATE = 68,
  /* SEG-007-T209: represented BCHG/BCLR/BSET with a statically foldable
   * memory destination and no retained resolver fact (write-side-only,
   * mirrors SUBQ/SUBI/CLR_MISSING_FACT, not BIT_TEST_MISSING_FACT's
   * read-side one). */
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_FACT = 69,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_FACT = 70,
  GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_FACT = 71,
  /* SEG-007-T220: MULS.W <ea>,Dn shares CMP's exact read-only-source,
   * fixed-Dn-destination shape (the compare family carries no
   * deferred-address-commit contract either). */
  GENESIS_C4_LOWERING_DIMENSIONS_MULS_AUTO_UPDATE = 72,
  GENESIS_C4_LOWERING_DIMENSIONS_MULS_MISSING_FACT = 73,
} GenesisC4LoweringDimensions;
typedef struct GenesisRuntimeStop {
  GenesisStopClass stop_class;
  GenesisDiagnosticCategory diagnostic_category;
  /* ADR 0015: this is selected by the generated stop site, not program metadata. */
  GenesisC4LoweringDimensions c4_lowering_dimensions;
  GenesisProvenance provenance;
} GenesisRuntimeStop;
typedef struct GenesisReportMetadata {
  GenesisCpuDimensions cpu_dimensions;
} GenesisReportMetadata;

typedef enum GenesisControlTransferKind {
  GENESIS_CONTINUE_AT_PC = 0,
  GENESIS_STOP = 1,
  GENESIS_COMPLETE = 2,
  /* SEG-007-T252 / ADR-0040: produced ONLY by genesis_runtime_run, when its
   * caller-supplied finite dispatch allowance is exhausted while the guest is
   * still GENESIS_CONTINUE_AT_PC. Disjoint from GENESIS_STOP: this is a
   * runner (host invocation policy) resource limit, never a guest semantic
   * stop, and never GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED. genesis_runtime_step
   * never produces this kind. */
  GENESIS_RUNNER_RESOURCE_LIMIT = 3,
} GenesisControlTransferKind;

typedef struct GenesisControlTransfer {
  GenesisControlTransferKind kind;
  uint32_t next_pc;
  /* Structurally meaningless/zeroed when kind == GENESIS_RUNNER_RESOURCE_LIMIT
   * (stop_class 0 is not a valid GenesisStopClass) -- a caller must switch on
   * `kind` first and never infer a guest stop from `stop` alone. */
  GenesisRuntimeStop stop;
  /* SEG-007-T252 / ADR-0040: meaningful only when kind == GENESIS_RUNNER_RESOURCE_LIMIT:
   * the deterministic count of guest dispatch steps genesis_runtime_run
   * actually took before its allowance was exhausted. Derived only from the
   * runner's own finite loop counter, never from any guest state, so
   * identical allowances on an identical fixture always report an identical
   * count. Zero for every other kind. */
  uint32_t runner_dispatch_count;
} GenesisControlTransfer;

typedef enum GenesisAccessResultKind {
  GENESIS_ACCESS_OK = 0,
  GENESIS_ACCESS_FAIL = 1,
} GenesisAccessResultKind;

/*
 * The only generated-runtime memory boundary.  `value` is read only after an
 * OK read and is consumed only on an OK write.  On failure neither it nor the
 * runtime is modified.  Addresses are MC68000 program addresses, never host
 * pointers.
 *
 * SEG-007-T091's sole, explicit, narrowly-scoped exception: a LONG write to
 * the VDP CONTROL port ($C00004) that fails after its first (high-word)
 * sub-write already succeeded leaves that sub-write's own already-committed
 * `GenesisVdpState` mutation in place -- see the block comment above
 * genesis_vdp_access (tools/genesis_startup_bridge_runtime.c) and the VDP
 * addendum to
 * docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md
 * for the full citation and justification. No other selector in this file
 * departs from this general contract.
 */
#ifdef __cplusplus
extern "C" {
#endif

GenesisAccessResultKind genesis_route_access(GenesisRuntime *runtime, uint32_t address,
                                              GenesisAccessWidth width,
                                              GenesisAccessDirection direction,
                                              uint32_t *value,
                                              GenesisRuntimeStop *stop_out);

/* Defensive finite-dispatch failure.  It never mutates the runtime. */
GenesisControlTransfer genesis_internal_dispatch_inconsistency_stop(GenesisRuntime *runtime);

typedef GenesisControlTransfer (*GenesisDispatchFunction)(GenesisRuntime *runtime);

/* Retires exactly one already-completed generated MC68000 instruction.  It is
 * the sole Genesis timing/admission seam; dispatch itself has no cadence. */
GenesisControlTransfer genesis_runtime_retire_m68k_instruction(GenesisRuntime *runtime,
                                                                uint32_t m68k_cycles,
                                                                uint32_t next_pc);

/*
 * SEG-007-T252 / ADR-0040: the former SEG-007-T107 `genesis_note_loop_backedge`,
 * SEG-007-T211/ADR-0035 `genesis_note_loop_completion`, and SEG-007-T155/
 * ADR-0017 `genesis_note_data_progress` watchdog-progress-note APIs have been
 * removed along with the generated-runtime progress watchdog they fed.
 * Termination/progress policy is now the runner's alone (see
 * genesis_runtime_run below).
 */

/*
 * SEG-007-T124 / ADR-0009 (docs/decisions/0009-computed-indirect-control-
 * flow-target-resolution.md): the one shared runtime membership guard for a
 * proven, finite computed/indirect control-flow target set. Generated code
 * evaluates a computed control EA from architectural registers, then calls
 * this to check the result against exactly the sorted candidate array C4
 * already validated for that source instruction -- it performs no lookup,
 * decode, or fetch of any target instruction or image byte, and it selects
 * no dispatch target itself; a nonzero result only tells the caller it may
 * proceed to set `runtime->pc` (and, for a call, push the continuation)
 * before the existing generated dispatcher takes over on the next drive
 * iteration. `targets` is never NULL when `count` is nonzero.
 */
int m68k_indirect_target_member(const uint32_t *targets, uint32_t count, uint32_t value);
/* SEG-007-T174 / ADR-0024: Tier 2's own shared-array membership test against
   the generation-time EmittedCodeAddressSet. The caller-supplied array is
   generated in strictly ascending sorted order (see
   libs/codegen/c11/src/frontend.cpp's `emitted_code_address_set` construction), so
   this performs a binary search rather than m68k_indirect_target_member's
   linear scan -- the shared set may be large (every emitted block entry in
   the whole generation), unlike a per-site Tier-1 candidate array, which
   stays small. Never reads or decodes the source image; it only ever
   compares one already-computed integer against a compiled-in address
   table. */
int m68k_emitted_code_address_member(const uint32_t *addresses, uint32_t count, uint32_t value);

/*
 * SEG-007-T252 / ADR-0040: the guest-owned runtime step/boundary contract.
 * Runs only a generated finite dispatcher for exactly one dispatch step; it
 * never fetches target bytes and carries no notion of "no progress". See the
 * definition in runtime.c for the exact ordering of the VDP DMA drain,
 * dispatch() call, checkpoint-class observation, and VBlank/IRQ6 scheduler
 * tick + admission it performs.
 */
GenesisControlTransfer genesis_runtime_step(GenesisRuntime *runtime, GenesisDispatchFunction dispatch);

/*
 * SEG-007-T252 / ADR-0040: the runner-owned finite dispatch allowance --
 * this project's ONLY generated-execution repetition mechanism now that the
 * generated-runtime progress watchdog (formerly genesis_runtime_drive, ADR-
 * 0007/0016/0017/0018/0019/0035) has been retired. Calls genesis_runtime_step
 * up to `dispatch_allowance` times (an overflow-safe upper bound: never
 * wraps, even when `dispatch_allowance == UINT32_MAX`), returning immediately
 * on any genuine guest GENESIS_STOP/GENESIS_COMPLETE. If the allowance is
 * exhausted while the guest is still GENESIS_CONTINUE_AT_PC, this returns the
 * new, disjoint GENESIS_RUNNER_RESOURCE_LIMIT outcome -- a host resource
 * limit, never a guest semantic stop, and never
 * GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED. `dispatch_allowance == 0` is
 * rejected the same way the former `dispatch_budget == 0` case was. This
 * translation unit contains no wall-clock read or sleep of any kind:
 * headless/automated execution is full-speed by construction.
 */
GenesisControlTransfer genesis_runtime_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                           uint32_t dispatch_allowance);

/*
 * SEG-007-T047 / ADR-0020 §9: RTE restoration, consuming exactly the six-byte
 * basic MC68000 exception frame constructed by interrupt admission or a
 * supported synchronous exception. Reads the saved SR from address `SP` and
 * the saved PC from `SP+2` through genesis_route_access (both routed reads), validates BOTH reads
 * succeed, then commits `sr`, `pc`, and `a[7] += 6` together atomically. If
 * either routed read fails, returns 0 with `*stop_out` set and leaves `sr`,
 * `pc`, and `a[7]` completely unmodified. On success returns 1 and writes the
 * restored PC to `*restored_pc_out`. Only a returned IRQ6 frame arms the
 * project-only synthetic IRQ6 admission grace; synchronous exception returns
 * do not touch scheduler state. It performs no target-opcode fetch/decode and
 * selects no dispatch target itself.
 */
int genesis_exception_return(GenesisRuntime *runtime, uint32_t *restored_pc_out,
                             GenesisRuntimeStop *stop_out);

/*
 * SEG-007-T222 / ADR-0037: the synchronous, unmasked, never-scheduled
 * divide-by-zero (vector 5) CPU exception raise, called directly from
 * generated DIVS.W/DIVU.W lowering when the divisor is zero. Reuses ADR-0020
 * §7/§8's exception-frame construction (the same shared helper IRQ6 admission
 * uses) with `fault_pc` (the instruction immediately following the faulting
 * DIVS.W/DIVU.W) as the pushed return PC. NOT gated by the SR interrupt mask,
 * NOT scheduled, and does not consume/arm any IRQ6 admission-grace or
 * watchdog progress-credit state. Fails closed (returns 0, `*stop_out` set)
 * if no build-resolved handler is installed
 * (`divide_by_zero_handler_present == 0`) or the frame cannot be constructed;
 * on success returns 1 and writes the resolved handler entry to
 * `*handler_pc_out`. Performs no target-opcode fetch/decode.
 */
int genesis_raise_divide_by_zero(GenesisRuntime *runtime, uint32_t fault_pc,
                                 uint32_t *handler_pc_out, GenesisRuntimeStop *stop_out);

/* Returns 1 only after the contract's checkpoint observation condition holds.
 * It is a pure snapshot: caller-owned identity/transaction data is copied,
 * and the runtime is never modified. `frame_producer` may be NULL (frame
 * stays at its never-rendered `{0}` value, exactly as before SEG-007-T050);
 * when non-NULL it is called exactly once, after the stable-frame gate,
 * against the just-copied persistent VDP state -- see
 * GenesisFrameProducerFunction above and this function's own definition. A
 * renderer failure (`frame_producer` returning nonzero) does not fail this
 * whole extraction; every other evidence category remains valid. */
int genesis_extract_checkpoint_evidence(const GenesisRuntime *runtime,
                                        const GenesisCheckpointIdentity *identity,
                                        const GenesisTransactionEvidence *transaction,
                                        GenesisFrameProducerFunction frame_producer,
                                        GenesisCheckpointEvidenceBundle *bundle_out);

/* Emits only §18's committable fields; it never writes bundle digests or raw
 * CPU/device/RAM/transaction/frame/options values. */
int genesis_write_checkpoint_evidence_summary(const GenesisCheckpointEvidenceBundle *bundle,
                                              const GenesisCheckpointEvidenceSummary *summary);

/* SEG-007-T050 add-on: exposes the single "is this GenesisFrameArtifact a
 * genuinely produced frame, not the never-rendered `{0}` sentinel" predicate
 * (defined in runtime.c) so other translation units (e.g.
 * platforms/genesis/runtime/frame_export.c) can reuse it exactly rather than
 * duplicating a second, potentially divergent all-zero-digest check. Returns
 * nonzero if `frame->frame_digest` is not all-zero bytes. `frame` must be
 * non-NULL; this function performs no NULL check of its own (callers already
 * NULL-check the enclosing artifact/bundle before reaching this predicate). */
int genesis_frame_artifact_is_populated(const GenesisFrameArtifact *frame);

/* Canonical sanitized wire report used by generated bridge mains. */
int genesis_write_sanitized_report(const GenesisControlTransfer *result,
                                    const char *rom_sha256,
                                    const GenesisReportMetadata *metadata);
int genesis_write_full_report(FILE *output, const GenesisRuntime *runtime,
                               const GenesisControlTransfer *result, const char *rom_sha256,
                               const GenesisReportMetadata *metadata);

/* SEG-007-T252 / ADR-0040 correction: NOT part of the stable/required
 * full-report or sanitized-report schema, and NOT part of the wire ABI --
 * local-diagnostic-use only. The only permitted caller is the generated
 * bridge's dedicated `--ephemeral-report-fd` argv path (see
 * libs/codegen/c11/src/genesis.cpp); it must never be wired to any
 * `--full-report-path`/`--full-report-fd` consumer. See the field's own doc
 * comment on `GenesisRuntime.recent_pc_history` above for the diagnostic
 * convention this writer serializes.
 */
int genesis_write_ephemeral_pc_history(FILE *output, const GenesisRuntime *runtime,
                                       const GenesisControlTransfer *result);

#ifdef __cplusplus
}
#endif

#endif
