#ifndef SEGARECOMP_DEVICE_SEGA_GENESIS_CONTROLLER_IO_CONTRACT_H
#define SEGARECOMP_DEVICE_SEGA_GENESIS_CONTROLLER_IO_CONTRACT_H

#include <stdint.h>

/*
 * C/C++ shared, policy-labelled controller-I/O contract.
 *
 * This deliberately contains only the already-selected, side-effect-free
 * compatibility selectors (four: CTRL1/CTRL2 LONG read, CTRL3 WORD read,
 * Version-register BYTE read, CTRL3 BYTE read).  Host translation and the
 * strict-C11 runtime use these identifiers rather than maintaining separate
 * literals.  It is not a general controller model and authorizes no additional
 * selector.
 */
#define SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN UINT32_C(0x00A10000)
#define SEGARECOMP_GENESIS_CONTROLLER_IO_END UINT32_C(0x00A10020)
#define SEGARECOMP_GENESIS_CONTROLLER_CTRL1_CTRL2_LONG_BEGIN UINT32_C(0x00A10008)
#define SEGARECOMP_GENESIS_CONTROLLER_CTRL3_WORD_BEGIN UINT32_C(0x00A1000C)
/* SEG-007-T111: the odd/low meaningful-data byte lane of the CTRL 3 register
 * whose WORD-read selector is SEG-007-T038. `$A1000D` = the CTRL3 WORD selector's
 * even transfer base + 1. */
#define SEGARECOMP_GENESIS_CONTROLLER_CTRL3_BYTE_ADDRESS \
  (SEGARECOMP_GENESIS_CONTROLLER_CTRL3_WORD_BEGIN + UINT32_C(1))
#define SEGARECOMP_GENESIS_CONTROLLER_VERSION_BYTE_ADDRESS UINT32_C(0x00A10001)
#define SEGARECOMP_GENESIS_CONTROLLER_VERSION_POLICY_VALUE UINT32_C(0xA0)

/*
 * SEG-055: console region profiles and the guest-visible hardware version register ($A10001, BYTE read).
 *
 * Version register layout (Sega Genesis Technical Overview; independently corroborated by Genesis Plus GX `core/io_ctrl.c`
 * `io_reset`, where the register is the region code plus the expansion bit, and by the public Sonic 3 disassembly
 * `sonicretro/skdisasm` `s3.asm`, whose lockout code takes bits 7-6 as the territory id 0 = Japan NTSC, 2 = overseas NTSC,
 * 3 = overseas PAL): bit 7 = 0 domestic (Japan) / 1 overseas, bit 6 = 0 NTSC (60 Hz) / 1 PAL (50 Hz), bit 5 = 1 when no
 * expansion unit (Mega-CD) is attached, bits 3-0 = hardware revision (0 here). The three profiles therefore read:
 *   Japan NTSC  (domestic, 60 Hz)  0x20
 *   USA NTSC    (overseas, 60 Hz)  0xA0   <- the default: exactly SEGARECOMP_GENESIS_CONTROLLER_VERSION_POLICY_VALUE
 *   Europe PAL  (overseas, 50 Hz)  0xE0
 * The numeric profile ids are chosen so that a zero-initialized runtime is the historical USA NTSC console.
 * The register value is a property of the CONFIGURED CONSOLE, never of the cartridge: no ROM byte is read or altered and
 * no cartridge compatibility logic is bypassed -- the game's own region check simply sees the configured hardware.
 */
typedef enum SegarecompGenesisRegionProfile {
  SEGARECOMP_GENESIS_REGION_USA_NTSC = 0,
  SEGARECOMP_GENESIS_REGION_JAPAN_NTSC = 1,
  SEGARECOMP_GENESIS_REGION_EUROPE_PAL = 2,
} SegarecompGenesisRegionProfile;
#define SEGARECOMP_GENESIS_REGION_PROFILE_COUNT 3U
#define SEGARECOMP_GENESIS_VERSION_OVERSEAS_BIT UINT32_C(0x80)
#define SEGARECOMP_GENESIS_VERSION_PAL_BIT UINT32_C(0x40)
#define SEGARECOMP_GENESIS_VERSION_NO_EXPANSION_BIT UINT32_C(0x20)

static inline uint32_t segarecomp_genesis_region_version_value(uint32_t profile) {
  switch (profile) {
    case SEGARECOMP_GENESIS_REGION_JAPAN_NTSC: return SEGARECOMP_GENESIS_VERSION_NO_EXPANSION_BIT;
    case SEGARECOMP_GENESIS_REGION_EUROPE_PAL:
      return SEGARECOMP_GENESIS_VERSION_OVERSEAS_BIT | SEGARECOMP_GENESIS_VERSION_PAL_BIT |
             SEGARECOMP_GENESIS_VERSION_NO_EXPANSION_BIT;
    case SEGARECOMP_GENESIS_REGION_USA_NTSC:
    default: return SEGARECOMP_GENESIS_VERSION_OVERSEAS_BIT | SEGARECOMP_GENESIS_VERSION_NO_EXPANSION_BIT;
  }
}
/* Three DIFFERENT facts about a console profile; do not conflate them:
 *   selectable    -- the profile may be configured and the guest sees its version-register value (segarecomp_genesis_region_is_selectable);
 *   runtime timing -- what THIS machine model executes: every profile runs the 60 Hz NTSC schedule (segarecomp_genesis_region_runtime_hz);
 *   hardware timing -- what the real console of that region does: Europe PAL is 50 Hz (segarecomp_genesis_region_hardware_hz).
 * PAL is therefore an APPROXIMATION: the PAL version register on a 60 Hz machine. It is never evidence of 50 Hz correctness (frame
 * pacing, audio timing and any PAL-dependent game logic are those of 60 Hz). Unknown profile values are not selectable. */
static inline int segarecomp_genesis_region_is_selectable(uint32_t profile) {
  return profile == SEGARECOMP_GENESIS_REGION_USA_NTSC || profile == SEGARECOMP_GENESIS_REGION_JAPAN_NTSC ||
         profile == SEGARECOMP_GENESIS_REGION_EUROPE_PAL;
}
static inline uint32_t segarecomp_genesis_region_runtime_hz(uint32_t profile) {
  (void)profile;
  return 60U;
}
static inline uint32_t segarecomp_genesis_region_hardware_hz(uint32_t profile) {
  return profile == SEGARECOMP_GENESIS_REGION_EUROPE_PAL ? 50U : 60U;
}

/* The complete, deliberately small set of supported selectors (four).  This
 * table is the C/C++ policy source of truth: consumers must not reproduce
 * selector literals or policy values in their own dispatches. */
typedef enum SegarecompGenesisControllerIoPolicyIdentity {
  SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T020 = 1,
  SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T038 = 2,
  SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T079 = 3,
  SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T111 = 4,
} SegarecompGenesisControllerIoPolicyIdentity;

typedef struct SegarecompGenesisControllerIoSelector {
  uint32_t address;
  uint8_t width;
  uint8_t direction;
  uint32_t policy_value;
  SegarecompGenesisControllerIoPolicyIdentity identity;
} SegarecompGenesisControllerIoSelector;

enum {
  SEGARECOMP_GENESIS_CONTROLLER_ACCESS_READ = 0,
  SEGARECOMP_GENESIS_CONTROLLER_ACCESS_WRITE = 1,
};

static const SegarecompGenesisControllerIoSelector
    segarecomp_genesis_controller_io_selectors[] = {
        {SEGARECOMP_GENESIS_CONTROLLER_CTRL1_CTRL2_LONG_BEGIN, 4U,
         SEGARECOMP_GENESIS_CONTROLLER_ACCESS_READ, UINT32_C(0),
         SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T020},
        {SEGARECOMP_GENESIS_CONTROLLER_CTRL3_WORD_BEGIN, 2U,
         SEGARECOMP_GENESIS_CONTROLLER_ACCESS_READ, UINT32_C(0),
         SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T038},
        {SEGARECOMP_GENESIS_CONTROLLER_VERSION_BYTE_ADDRESS, 1U,
         SEGARECOMP_GENESIS_CONTROLLER_ACCESS_READ,
         SEGARECOMP_GENESIS_CONTROLLER_VERSION_POLICY_VALUE,
         SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T079},
        {SEGARECOMP_GENESIS_CONTROLLER_CTRL3_BYTE_ADDRESS, 1U,
         SEGARECOMP_GENESIS_CONTROLLER_ACCESS_READ, UINT32_C(0),
         SEGARECOMP_GENESIS_CONTROLLER_POLICY_SEG_007_T111},
};
#define SEGARECOMP_GENESIS_CONTROLLER_IO_SELECTOR_COUNT \
  (sizeof(segarecomp_genesis_controller_io_selectors) / \
   sizeof(segarecomp_genesis_controller_io_selectors[0]))

/*
 * SEG-007-T121: the bounded controller-I/O GPIO-register WRITE-direction
 * selector family. This is the write mirror of the read selectors above and is
 * a deliberately labelled, replaceable PROJECT COMPATIBILITY POLICY (see the
 * SEG-007-T121 section of
 * docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md),
 * NOT a verified Genesis-hardware claim.
 *
 * Recognised targets: the six general-purpose I/O-port data/direction latches
 * GTO1 p. 72-75 documents -- DATA1 $A10003, DATA2 $A10005, DATA3 $A10007,
 * CTRL1 $A10009, CTRL2 $A1000B, CTRL3 $A1000D. Each is documented R/W. BYTE
 * width only, addressing the register byte itself (the runtime-reached form is
 * a CTRL3 $A1000D BYTE write). WORD/LONG width, the serial S-CTRL / TxDATA /
 * RxDATA registers, and the Version register remain unconditionally
 * fail-closed.
 *
 * CTRL3 $A1000D is also the SEG-007-T111 BYTE *read* selector / SEG-007-T038
 * WORD read selector address: a BYTE write here is a latched store, while a
 * BYTE/WORD read keeps returning its unchanged read-selector policy constant
 * (the latch is never read back -- the VDP registers[] write-only precedent).
 * The controller-I/O policy parity contract
 * (tests/controller_io_policy_parity_test.cpp) is updated to allow exactly this
 * one read-selector address to additionally accept a BYTE write.
 *
 * This predicate is recognition-only (shared by the translation-time routing
 * gate and the runtime device gate); the deterministic latched-register store
 * semantics live in the runtime owner (genesis_controller_io_access,
 * platforms/genesis/runtime/runtime.c).
 *
 * Returns the 0..5 latch index (0..2 = DATA1..DATA3, 3..5 = CTRL1..CTRL3) for a
 * recognised BYTE write target, or -1 otherwise.
 *
 * SEG-007-T166 reuses this same recognizer, unchanged, for the DATA1/DATA2
 * (index 0/1) BYTE *read* selector family: both the translation-time
 * routing gate (m68k_route_genesis_device_access) and the runtime device
 * gate (genesis_controller_io_access) filter this function's result to
 * index 0 or 1 with direction == read, rather than duplicating the address
 * list. See the SEG-007-T166 section of
 * docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md.
 */
static inline int
segarecomp_genesis_controller_io_gpio_register_index(uint32_t address, uint32_t width) {
  if (width != 1U) {
    return -1;
  }
  switch (address) {
    case UINT32_C(0x00A10003): return 0;
    case UINT32_C(0x00A10005): return 1;
    case UINT32_C(0x00A10007): return 2;
    case UINT32_C(0x00A10009): return 3;
    case UINT32_C(0x00A1000B): return 4;
    case UINT32_C(0x00A1000D): return 5;
    default: return -1;
  }
}

#endif
