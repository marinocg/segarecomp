#pragma once

// Master System cartridge ingestion (SEG-009-T002; machine contract sections 1, 4.1, 4.2; ADR 0061/0064).
//
// Generation time only. A ROM is accepted iff it belongs to the one baseline profile (Master System II, NTSC, export
// region), its size is admissible and a mapper family is DECLARED. The header may identify platform/region/profile only;
// it never establishes the mapper, and no byte pattern, size, checksum or database infers one. All rejections are typed
// `SmsError` values (sms_error.h) raised before anything is emitted.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include "sms_error.h"
#include "sms_mapper_contract.h"

namespace segarecomp::machine::master_system {

// Declaration sources in precedence order (first present wins, contract section 4.1). Two declarations that name
// different families are a conflict (SMS_ERROR_MAPPER_UNDECLARED, the identity is ambiguous).
enum class DeclarationSource : std::uint8_t { build_option, manifest, fixture_builder, local_identity };
[[nodiscard]] const char* declaration_source_name(DeclarationSource source) noexcept;

struct MapperDeclaration {
  std::string family;  // as written by the declarer: "sega", "rom_only", or a name that is then rejected
  DeclarationSource source = DeclarationSource::build_option;
};

// A cartridge/profile manifest `{"mapper": "...", "sha256": "..."}` (extra keys are allowed, flat JSON only; the
// fixture builder also writes "declaration_source"). `input_sha256` must equal the manifest's "sha256" when present and
// the manifest must name a mapper. On failure `error` is SMS_ERROR_MAPPER_UNDECLARED and `detail` says why.
struct ManifestResult {
  SmsError error = SMS_OK;
  std::string detail;
  MapperDeclaration declaration;
};
[[nodiscard]] ManifestResult parse_mapper_manifest(std::string_view text, std::string_view input_sha256);

struct IngestOptions {
  // The caller explicitly selected the baseline platform/profile. Needed when the ROM has no recognizable header or an
  // ambiguous/invalid one. It never overrides a recognized header of another console or region (Game Gear, Japan,
  // a 16-bit console): those stay SMS_ERROR_PROFILE_UNSUPPORTED.
  bool explicit_profile = false;
  std::vector<MapperDeclaration> declarations;
};

enum class ChecksumStatus : std::uint8_t { match, mismatch, not_evaluated };
[[nodiscard]] const char* checksum_status_name(ChecksumStatus status) noexcept;

// Recorded identity of an accepted cartridge (provenance). The checksum and header size code are informational only.
struct CartridgeIdentity {
  std::string sha256;
  std::size_t size = 0;
  std::string header_class;         // "tmr_sega" (header found) or "none" (explicit profile selection)
  std::size_t header_offset = 0;    // valid when header_class == "tmr_sega"
  std::string region;               // "sms_export" or "unspecified"
  std::string profile;              // "sms2_ntsc_export"
  SmsMapperFamily mapper = SMS_MAPPER_UNDECLARED;
  DeclarationSource declaration_source = DeclarationSource::build_option;
  std::uint32_t bank_count = 0;     // 16 KiB banks
  unsigned header_size_code = 0;
  ChecksumStatus checksum = ChecksumStatus::not_evaluated;
};

struct IngestResult {
  SmsError error = SMS_OK;
  std::string detail;  // human-readable reason (never commercial-derived bytes)
  CartridgeIdentity identity;
  std::vector<std::uint8_t> rom;  // copy of the validated input
  [[nodiscard]] bool ok() const noexcept { return error == SMS_OK; }
};

[[nodiscard]] const char* mapper_family_name(SmsMapperFamily family) noexcept;
[[nodiscard]] IngestResult ingest_cartridge(std::span<const std::uint8_t> rom, const IngestOptions& options);

}  // namespace segarecomp::machine::master_system
