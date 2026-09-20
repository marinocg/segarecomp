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
#define GENESIS_FRAME_WIDTH 320U
#define GENESIS_FRAME_HEIGHT 224U
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
typedef struct GenesisZ80BusState { uint8_t bus_requested; uint8_t bus_granted; uint8_t reset_asserted; uint8_t z80_ram[GENESIS_Z80_RAM_BYTES]; } GenesisZ80BusState;
typedef struct GenesisPsgState { uint8_t latched_channel; uint8_t latched_volume; uint8_t latch_valid; uint16_t tone_period[3]; uint8_t attenuation[4]; uint8_t noise_control; } GenesisPsgState;
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

typedef struct GenesisFrameArtifact {
  uint8_t pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT];
  uint8_t palette_snapshot[GENESIS_VDP_CRAM_BYTES];
  uint8_t frame_digest[32];
} GenesisFrameArtifact;

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
