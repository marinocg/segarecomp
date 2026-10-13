#pragma once

// SMS generation route (SEG-009-T002): ingest -> ImageSet -> `codegen::z80::emit_image_set`. This is the single entry
// point `segarecomp build` routes SMS through later (T010); there is no second emitter.
//
// Files written to `directory` (deterministic, byte-identical across runs; nothing is left behind on failure):
//   <stem>*.c / <stem>.h ...   the sharded image C from emit_image_set, listed in <stem>.units
//   <stem>_rom.c               `const uint8_t sms_rom_data[]`, `const uint32_t sms_rom_size`,
//                              `const uint32_t sms_rom_mapper_family` (SmsMapperFamily value): the embedded cartridge
//                              (ADR 0064 section 2); appended to <stem>.units
//   <stem>_cartridge.json      sorted-key identity/provenance record: sha256, size, header class, region, profile,
//                              mapper family + declaration source, checksum status, bank count

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/codegen/c11/z80.hpp"
#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/master_system/ml_region_z80_producer.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

namespace segarecomp::machine::master_system {

struct EmitRequest {
  std::filesystem::path directory;
  std::string stem = "sms_image";
  codegen::z80::EmitOptions codegen;  // `directory` and `stem` above override the nested values
  // SEG-057-T002 (ADR 0119): the experimental Optimized policy. When set, the native D512HP producer proposes the admitted starts and the
  // proposal is validated by the unchanged admission contract; ANY failure (model identity, producer input, inference, emitter rejection)
  // emits the ordinary broad AOT instead and says why in EmitOutcome::admission. Ignored when `codegen.admission` is already supplied.
  bool propose_admission = false;
  // Test seam (dependency injection, not a product option): replaces the native producer. Defaults to `ml_propose`.
  std::function<MlProposal(const IngestResult&, const codegen::z80::ImageSet&)> proposer;
};

// What the Optimized request resolved to (machine-readable; no address, byte or per-cell datum).
struct AdmissionReport {
  bool requested = false;
  std::string effective = "broad";  // "broad" | "ml_region_z80"
  bool fallback = false;            // requested but the emitted program is the broad AOT
  std::string reason = "none";      // none | model_identity | producer_input | inference | admission_rejected
  std::size_t cells = 0, selected_cells = 0, image_offsets = 0, admitted_offsets = 0;
  double inference_seconds = 0;
  std::string plan_sha256;          // identity of the admitted set (empty unless effective == ml_region_z80)
};

struct EmitOutcome {
  SmsError sms_error = SMS_OK;  // typed ingestion rejection (nothing emitted)
  std::string error;            // non-empty on any failure (typed rejection detail or emitter/file error)
  CartridgeIdentity identity;
  codegen::z80::EmitStats stats;
  std::vector<codegen::z80::OwnerRecord> owners;  // filled when request.codegen.record_owners is set
  AdmissionReport admission;
  ImageProvenanceCounts provenance;               // sanitized executable-image provenance of the emitted images (SEG-028)
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] std::string cartridge_provenance_json(const CartridgeIdentity& identity);
[[nodiscard]] EmitOutcome emit_cartridge(std::span<const std::uint8_t> rom, const IngestOptions& options,
                                         const EmitRequest& request);

}  // namespace segarecomp::machine::master_system
