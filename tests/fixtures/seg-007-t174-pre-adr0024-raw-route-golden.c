#define _POSIX_C_SOURCE 200809L
#include "runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char GENESIS_BRIDGE_ROM_SHA256[65] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const GenesisReportMetadata GENESIS_BRIDGE_REPORT_METADATA = { GENESIS_CPU_DIMENSIONS_NONE };
static int genesis_write_requested_full_report(const char *report_path, const char *report_fd, const GenesisRuntime *runtime, const GenesisControlTransfer *result) { FILE *full; char *end; long descriptor; if (report_path == NULL && report_fd == NULL) return 0; if (report_path != NULL) full = fopen(report_path, "w"); else { errno = 0; descriptor = strtol(report_fd, &end, 10); if (errno != 0 || *report_fd == '\0' || *end != '\0' || descriptor < 0 || descriptor > INT_MAX) return 1; full = fdopen((int)descriptor, "w"); } if (full == NULL) return 1; if (genesis_write_full_report(full, runtime, result, GENESIS_BRIDGE_ROM_SHA256, &GENESIS_BRIDGE_REPORT_METADATA) != 0 || fclose(full) != 0) return 1; return 0; }
static int genesis_write_requested_ephemeral_pc_history(const char *ephemeral_report_fd, const GenesisRuntime *runtime, const GenesisControlTransfer *result) { FILE *ephemeral; char *end; long descriptor; if (ephemeral_report_fd == NULL) return 0; errno = 0; descriptor = strtol(ephemeral_report_fd, &end, 10); if (errno != 0 || *ephemeral_report_fd == '\0' || *end != '\0' || descriptor < 0 || descriptor > INT_MAX) return 1; ephemeral = fdopen((int)descriptor, "w"); if (ephemeral == NULL) return 1; if (genesis_write_ephemeral_pc_history(ephemeral, runtime, result) != 0 || fclose(ephemeral) != 0) return 1; return 0; }
static int genesis_parse_bridge_argv(int argc, char **argv, const char **report_path_out, const char **report_fd_out, uint32_t *instruction_budget_out, const char **ephemeral_report_fd_out) { int index; int instruction_budget_seen = 0; *report_path_out = NULL; *report_fd_out = NULL; *ephemeral_report_fd_out = NULL; if (argc == 1) return 0; if (argc != 3 && argc != 5 && argc != 7) return 1; for (index = 1; index < argc; index += 2) { const char *flag = argv[index]; const char *value = argv[index + 1]; if (strcmp(flag, "--full-report-path") == 0) { if (*report_path_out != NULL || *report_fd_out != NULL) return 1; *report_path_out = value; } else if (strcmp(flag, "--full-report-fd") == 0) { if (*report_path_out != NULL || *report_fd_out != NULL) return 1; *report_fd_out = value; } else if (strcmp(flag, "--ephemeral-report-fd") == 0) { if (*ephemeral_report_fd_out != NULL) return 1; *ephemeral_report_fd_out = value; } else if (strcmp(flag, "--instruction-budget") == 0) { char *end; unsigned long parsed; size_t k; if (instruction_budget_seen) return 1; if (value[0] == '\0') return 1; for (k = 0; value[k] != '\0'; ++k) { if (value[k] < '0' || value[k] > '9') return 1; } errno = 0; parsed = strtoul(value, &end, 10); if (errno != 0 || *end != '\0' || parsed > UINT32_MAX || parsed == 0) return 1; *instruction_budget_out = (uint32_t)parsed; instruction_budget_seen = 1; } else return 1; } return 0; }
static GenesisControlTransfer genesis_frontier_stop_00000C04(GenesisRuntime *runtime) {
  GenesisControlTransfer transfer = {0};
  (void)runtime;
  transfer.kind = GENESIS_STOP;
  transfer.stop.stop_class = GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET;
  transfer.stop.diagnostic_category = GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE;
  transfer.stop.provenance.has_instruction_provenance = 1U;
  transfer.stop.provenance.instruction.cpu_variant = GENESIS_CPU_MC68000;
  transfer.stop.provenance.instruction.source_address = UINT32_C(0x00000C04);
  transfer.stop.provenance.instruction.image_offset = UINT64_C(4);
  transfer.stop.provenance.instruction.primary_bytes[0] = UINT8_C(0x4E);
  transfer.stop.provenance.instruction.primary_bytes[1] = UINT8_C(0xBB);
  transfer.stop.provenance.instruction.length = UINT32_C(4);
  transfer.stop.provenance.bus_access_count = UINT8_C(0);
  return transfer;
}

void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source) {
  if (source->source_address == UINT32_C(0x00000C00)) {
    stop->provenance.mapping_claim_count = UINT8_C(1);
    stop->provenance.mapping_claims[0].name_length = UINT8_C(3);
    stop->provenance.mapping_claims[0].name[0] = UINT8_C(114);
    stop->provenance.mapping_claims[0].name[1] = UINT8_C(111);
    stop->provenance.mapping_claims[0].name[2] = UINT8_C(109);
    stop->provenance.mapping_claims[0].target_begin = UINT32_C(0x00000C00);
    stop->provenance.mapping_claims[0].target_end = UINT32_C(0x00000C0C);
    stop->provenance.mapping_claims[0].image_begin = UINT64_C(0);
    stop->provenance.mapping_claims[0].image_end = UINT64_C(12);
    stop->provenance.bus_access_count = UINT8_C(1);
    stop->provenance.bus_accesses[0].ordinal = UINT64_C(0);
    stop->provenance.bus_accesses[0].kind = GENESIS_BUS_INSTRUCTION_READ;
    stop->provenance.bus_accesses[0].address = source->source_address;
    stop->provenance.bus_accesses[0].raw_byte_count = UINT8_C(4);
    stop->provenance.bus_accesses[0].region = GENESIS_REGION_RAW_CARTRIDGE_ROM;
    stop->provenance.bus_accesses[0].raw_bytes[0] = UINT8_C(0x02);
    stop->provenance.bus_accesses[0].raw_bytes[1] = UINT8_C(0x40);
    stop->provenance.bus_accesses[0].raw_bytes[2] = UINT8_C(0x01);
    stop->provenance.bus_accesses[0].raw_bytes[3] = UINT8_C(0xFF);
    return;
  }
}
GenesisControlTransfer genesis_static_stop(GenesisStopClass class_, GenesisDiagnosticCategory category, const GenesisInstructionProvenance *source, uint8_t has_access, uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction) {
  GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop.stop_class = class_; transfer.stop.diagnostic_category = category; transfer.stop.provenance.has_instruction_provenance = 1U; transfer.stop.provenance.instruction = *source;
  if (has_access != 0U) { transfer.stop.provenance.has_access = 1U; transfer.stop.provenance.access_address = address; transfer.stop.provenance.access_width = width; transfer.stop.provenance.access_direction = direction; }
  genesis_attach_route_provenance(&transfer.stop, source); return transfer;
}

static GenesisControlTransfer genesis_block_00000C00(GenesisRuntime *runtime) {
  {
#define pc runtime->pc
  {
const uint32_t m68k_ea_value_0 = UINT32_C(0x000001FF);
{ const uint32_t logical_source = m68k_ea_value_0; const uint32_t logical_destination = runtime->d[0]; const uint32_t logical_result = logical_destination & logical_source; runtime->d[0] = (runtime->d[0] & UINT32_C(0xFFFF0000)) | ((logical_result) & UINT32_C(0xFFFF)); runtime->sr = (uint16_t)((runtime->sr & UINT16_C(0xFFF0)) | (((logical_result & UINT32_C(0x0000FFFF)) & UINT32_C(0x00008000)) != 0U ? UINT16_C(8) : UINT16_C(0)) | ((logical_result & UINT32_C(0x0000FFFF)) == 0U ? UINT16_C(4) : UINT16_C(0))); }
pc += UINT32_C(4);
}
#undef pc
  }
  GenesisControlTransfer transfer = {0};
  if (runtime->pc == UINT32_C(0x00000C04)) return genesis_frontier_stop_00000C04(runtime);
  transfer.kind = GENESIS_CONTINUE_AT_PC;
  transfer.next_pc = runtime->pc;
  return transfer;
}

static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {
  if (runtime->pc == UINT32_C(0x00000C00)) return genesis_block_00000C00(runtime);
  if (runtime->pc == UINT32_C(0x00000C04)) return genesis_frontier_stop_00000C04(runtime);
  return genesis_internal_dispatch_inconsistency_stop(runtime);
}

GenesisControlTransfer genesis_bridge_dispatch(GenesisRuntime *runtime) {
  return genesis_dispatch(runtime);
}
static const uint8_t genesis_owned_region_data_0[] = { UINT8_C(0x02), UINT8_C(0x40), UINT8_C(0x01), UINT8_C(0xFF), UINT8_C(0x4E), UINT8_C(0xBB), UINT8_C(0x00), UINT8_C(0x04), UINT8_C(0x4E), UINT8_C(0x71), UINT8_C(0x60), UINT8_C(0xF4) };
static const GenesisOwnedCartridgeRegion genesis_owned_cartridge_regions[] = {
  { UINT32_C(0x00000C00), UINT32_C(0x00000C0C), genesis_owned_region_data_0, UINT32_C(0x0000000C) }
};
int main(int argc, char **argv) { GenesisRuntime runtime = {0}; GenesisControlTransfer result; const char *report_path; const char *report_fd; uint32_t instruction_budget = UINT32_C(128); const char *ephemeral_report_fd; if (genesis_parse_bridge_argv(argc, argv, &report_path, &report_fd, &instruction_budget, &ephemeral_report_fd) != 0) return 1; runtime.a[7] = UINT32_C(0x00FF0100); runtime.pc = UINT32_C(0x00000C00);   runtime.owned_regions = genesis_owned_cartridge_regions;
  runtime.owned_region_count = UINT32_C(1);
result = genesis_runtime_run(&runtime, genesis_bridge_dispatch, instruction_budget); if (genesis_write_requested_full_report(report_path, report_fd, &runtime, &result) != 0) return 1; if (genesis_write_requested_ephemeral_pc_history(ephemeral_report_fd, &runtime, &result) != 0) return 1; return genesis_write_sanitized_report(&result, GENESIS_BRIDGE_ROM_SHA256, &GENESIS_BRIDGE_REPORT_METADATA); }
