#ifndef SEGARECOMP_RUNTIME_GENESIS_CHECKPOINT_EVIDENCE_H
#define SEGARECOMP_RUNTIME_GENESIS_CHECKPOINT_EVIDENCE_H

#include <stdint.h>

/*
 * Neutral checkpoint-evidence schema shared with the future independent
 * oracle.  This header deliberately declares data only: no device, CPU, or
 * rendering behavior belongs here.  See the contract, section 12.
 */
#define GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION 1U
#define GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION 1U
#define GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES 256U
#define GENESIS_FRAME_WIDTH 320U  /* legacy/H40 non-interlaced width: the geometry of a zero-initialized artifact */
#define GENESIS_FRAME_HEIGHT 224U /* legacy non-interlaced V28 height */
/* SEG-055: the largest artifact geometry (H40 x interlace-mode-2) and the compact per-scanline raster state. */
#define GENESIS_FRAME_MAX_WIDTH 320U
#define GENESIS_FRAME_MAX_HEIGHT 448U
#define GENESIS_FRAME_MAX_PALETTES 256U
#define GENESIS_RASTER_MAX_LINES 224U /* active V28 field lines per frame (the NTSC schedule's VBlank onset line) */
#define GENESIS_STABLE_FRAME_VBLANK_COUNT 2U
#define GENESIS_MAX_NAME_LENGTH 64U
#define GENESIS_MAX_RAW_BYTES 12U
#define GENESIS_VDP_REGISTER_COUNT 24U
#define GENESIS_VDP_VRAM_BYTES 65536U
#define GENESIS_VDP_CRAM_BYTES 128U
#define GENESIS_VDP_VSRAM_BYTES 80U
#define GENESIS_Z80_RAM_BYTES 8192U

/* Inert persistent-state and transaction record shapes shared by the schema.
 * They describe storage only; all mutation remains in runtime.c. */
typedef enum GenesisVdpDmaPhase { GENESIS_VDP_DMA_IDLE = 0, GENESIS_VDP_DMA_BUSY = 1 } GenesisVdpDmaPhase;
typedef enum GenesisVdpDmaKind { GENESIS_VDP_DMA_MEMORY_TO_VRAM = 0, GENESIS_VDP_DMA_VRAM_FILL = 1 } GenesisVdpDmaKind;
typedef struct GenesisVdpDmaState { GenesisVdpDmaPhase phase; GenesisVdpDmaKind kind; uint32_t source_address; uint32_t remaining_length; uint32_t fill_byte_count; uint32_t transfer_access_count; uint8_t write_target_code; } GenesisVdpDmaState;
typedef struct GenesisVdpState { uint16_t registers[GENESIS_VDP_REGISTER_COUNT]; uint8_t control_port_awaiting_second_word; uint16_t control_port_first_word; uint32_t addressed_pointer; uint16_t auto_increment_value; uint16_t status_register; uint8_t data_port_transfer_code; uint8_t data_port_transfer_code_valid; uint8_t vram[GENESIS_VDP_VRAM_BYTES]; uint8_t cram[GENESIS_VDP_CRAM_BYTES]; uint8_t vsram[GENESIS_VDP_VSRAM_BYTES]; GenesisVdpDmaState dma; } GenesisVdpState;
typedef struct GenesisZ80BusState { uint8_t bus_requested; uint8_t bus_granted; uint8_t reset_released; uint16_t bank; uint8_t z80_ram[GENESIS_Z80_RAM_BYTES]; } GenesisZ80BusState;
typedef struct GenesisPsgState { uint32_t write_count; uint32_t write_digest; } GenesisPsgState;
typedef struct GenesisControllerIoState { uint8_t data[3]; uint8_t ctrl[3]; } GenesisControllerIoState;
typedef struct GenesisInterruptState { uint8_t vblank_pending; uint32_t vblank_status_read_count; uint32_t vblank_transition_count; uint8_t checkpoint_entered; uint32_t vblank_transition_count_at_checkpoint_entry; } GenesisInterruptState;
typedef struct GenesisDeviceState { GenesisZ80BusState z80_bus; GenesisVdpState vdp; GenesisPsgState psg; GenesisControllerIoState controller_io; GenesisInterruptState interrupt; } GenesisDeviceState;
typedef enum GenesisBusKind { GENESIS_BUS_INSTRUCTION_READ = 1, GENESIS_BUS_DATA_READ = 2, GENESIS_BUS_DATA_WRITE = 3, GENESIS_BUS_STACK_READ = 4, GENESIS_BUS_STACK_WRITE = 5 } GenesisBusKind;
typedef enum GenesisBusRegion { GENESIS_REGION_RAW_CARTRIDGE_ROM = 1, GENESIS_REGION_SYNTHETIC_WORK_RAM = 2 } GenesisBusRegion;
typedef struct GenesisBusAccess { uint64_t ordinal; GenesisBusKind kind; uint32_t address; uint8_t raw_bytes[GENESIS_MAX_RAW_BYTES]; uint8_t raw_byte_count; GenesisBusRegion region; } GenesisBusAccess;

typedef struct GenesisDeterministicOptions {
  uint32_t schema_version;
  uint32_t instruction_budget;
  uint32_t stable_frame_vblank_count;
} GenesisDeterministicOptions;

typedef struct GenesisCheckpointIdentity {
  char checkpoint_id[GENESIS_MAX_NAME_LENGTH];
  uint8_t checkpoint_id_length;
  char rom_sha256[65];
  GenesisDeterministicOptions options;
  uint8_t options_digest[32];
} GenesisCheckpointIdentity;

typedef enum GenesisCheckpointPcClass {
  GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN = 0,
} GenesisCheckpointPcClass;

typedef struct GenesisCpuEvidence {
  uint32_t d[8];
  uint32_t a[8];
  uint16_t sr;
  GenesisCheckpointPcClass pc_class;
} GenesisCpuEvidence;

typedef struct GenesisRamEvidence { uint8_t work_ram_digest[32]; } GenesisRamEvidence;
typedef struct GenesisDeviceEvidence { GenesisDeviceState devices; } GenesisDeviceEvidence;

typedef struct GenesisTransactionEvidence {
  uint8_t has_full_transactions;
  uint16_t transaction_count;
  GenesisBusAccess transactions[GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES];
  uint8_t transaction_digest[32];
} GenesisTransactionEvidence;

/*
 * SEG-055: the frame artifact now carries its own geometry and, when CRAM changed during active display, one
 * palette per distinct scanline state.  `pixels` holds `width * height` CRAM indices (row stride == width).
 * A zero-initialized artifact (width == height == 0, palette_count <= 1, every row_palette entry 0) IS the legacy
 * 320x224 single-palette artifact, so every earlier producer, digest and consumer keeps its meaning:
 *   - `palette_snapshot` is palette slot 0 (the palette of the first row);
 *   - `row_palette[row]` selects the slot of an output row (slot k >= 1 lives in `extra_palettes[k - 1]`);
 *   - `frame_digest` is SHA-256(pixels[0, w*h) || palette_snapshot) for a legacy-shaped artifact (exactly the
 *     pre-SEG-055 digest) and additionally covers width, height, palette_count, row_palette and the extra palettes
 *     for every other shape (see genesis_vdp_frame_digest in vdp_render.c).
 */
typedef struct GenesisFrameArtifact {
  uint8_t pixels[GENESIS_FRAME_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT];
  uint8_t palette_snapshot[GENESIS_VDP_CRAM_BYTES];
  uint8_t frame_digest[32];
  uint16_t width;
  uint16_t height;
  uint16_t palette_count;
  uint8_t row_palette[GENESIS_FRAME_MAX_HEIGHT];
  uint8_t extra_palettes[GENESIS_FRAME_MAX_PALETTES - 1U][GENESIS_VDP_CRAM_BYTES];
} GenesisFrameArtifact;

/* Active geometry of an artifact: a zero width/height means the legacy 320x224 shape (this header stays data-only, so
 * these are expressions, not functions). The per-row palette lookup lives in vdp_render.c
 * (genesis_vdp_frame_row_palette). */
#define GENESIS_FRAME_ARTIFACT_WIDTH(frame) ((frame)->width != 0U ? (unsigned)(frame)->width : GENESIS_FRAME_WIDTH)
#define GENESIS_FRAME_ARTIFACT_HEIGHT(frame) ((frame)->height != 0U ? (unsigned)(frame)->height : GENESIS_FRAME_HEIGHT)

/* SEG-055: one scanline's mutable VDP state (everything the renderer reads that a game can change between lines
 * without VRAM traffic: the register file, CRAM and VSRAM) -- 256 bytes, never a VRAM snapshot. */
typedef struct GenesisRasterLine {
  uint16_t registers[GENESIS_VDP_REGISTER_COUNT];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
} GenesisRasterLine;

/* The per-field-line state of one completed active display period (line L = the state when scanline L began). */
typedef struct GenesisRasterFrameState {
  GenesisRasterLine lines[GENESIS_RASTER_MAX_LINES];
} GenesisRasterFrameState;

typedef struct GenesisCheckpointEvidenceBundle {
  uint32_t schema_version;
  GenesisCheckpointIdentity identity;
  GenesisCpuEvidence cpu;
  GenesisRamEvidence ram;
  GenesisDeviceEvidence device;
  GenesisTransactionEvidence transaction;
  GenesisFrameArtifact frame;
  uint8_t bundle_digest[32];
} GenesisCheckpointEvidenceBundle;

typedef struct GenesisCheckpointEvidenceSummary {
  uint8_t cpu_present;
  uint8_t ram_present;
  uint8_t device_present;
  uint8_t transaction_present;
  uint8_t frame_present;
  uint8_t cpu_matches;
  uint8_t ram_matches;
  uint8_t device_matches;
  uint8_t transaction_matches;
  uint8_t frame_matches;
  uint8_t verdict_passed;
} GenesisCheckpointEvidenceSummary;

#endif
