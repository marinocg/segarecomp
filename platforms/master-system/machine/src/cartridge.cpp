#include "segarecomp/machine/master_system/cartridge.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::master_system {
namespace {

constexpr const char* profile_name = "sms2_ntsc_export";

[[nodiscard]] std::string normalize(std::string_view name) {
  std::string out;
  for (const char c : name) out.push_back(c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// Known non-baseline families (machine contract 4.1): a declared one is a typed unsupported-mapper error. Any other name
// is unknown and therefore undeclared.
[[nodiscard]] bool known_unsupported(const std::string& name) {
  static const char* const names[] = {"codemasters", "korean", "korean_msx", "korean_zemina", "msx", "nemesis",
                                      "janggun", "4pak", "4_pak", "eeprom", "eeprom_93c46", "multicart", "sg1000",
                                      "sega_korean", "korea"};
  return std::any_of(std::begin(names), std::end(names), [&](const char* n) { return name == n; });
}

// Flat JSON object of string/number/boolean/null values. Anything nested or malformed is rejected.
[[nodiscard]] bool parse_flat_json(std::string_view text, std::map<std::string, std::string>& out) {
  std::size_t i = 0;
  const auto skip = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
  const auto string = [&](std::string& value) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    value.clear();
    while (i < text.size() && text[i] != '"') {
      if (text[i] == '\\') {
        if (++i >= text.size() || (text[i] != '"' && text[i] != '\\' && text[i] != '/')) return false;
      }
      value.push_back(text[i++]);
    }
    if (i >= text.size()) return false;
    ++i;
    return true;
  };
  skip();
  if (i >= text.size() || text[i++] != '{') return false;
  skip();
  if (i < text.size() && text[i] == '}') return ++i, skip(), i == text.size();
  for (;;) {
    std::string key, value;
    skip();
    if (!string(key)) return false;
    skip();
    if (i >= text.size() || text[i++] != ':') return false;
    skip();
    if (i < text.size() && text[i] == '"') {
      if (!string(value)) return false;
    } else {
      while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '-' || text[i] == '.'))
        value.push_back(text[i++]);
      if (value.empty()) return false;
    }
    if (!out.emplace(key, value).second) return false;  // duplicate key is ambiguous
    skip();
    if (i >= text.size()) return false;
    if (text[i] == ',') { ++i; continue; }
    if (text[i] == '}') return ++i, skip(), i == text.size();
    return false;
  }
}

[[nodiscard]] std::size_t checksum_limit(unsigned code) {
  switch (code) {
    case 0xA: return 0x2000;
    case 0xB: return 0x4000;
    case 0xC: return 0x8000;
    case 0xE: return 0x10000;
    case 0xF: return 0x20000;
    case 0x0: return 0x40000;
    case 0x1: return 0x80000;
    default: return 0;
  }
}

// SMS Power! "ROM header": 16-bit sum of the size-code range excluding the 16 header bytes. Informational only.
[[nodiscard]] ChecksumStatus evaluate_checksum(std::span<const std::uint8_t> rom, std::size_t header, unsigned code) {
  const std::size_t limit = checksum_limit(code);
  if (limit == 0 || limit > rom.size() || header + 16 > limit) return ChecksumStatus::not_evaluated;
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i < limit; ++i)
    if (i < header || i >= header + 16) sum += rom[i];
  const std::uint32_t stored = static_cast<std::uint32_t>(rom[header + 10]) | (static_cast<std::uint32_t>(rom[header + 11]) << 8);
  return (sum & 0xFFFFu) == stored ? ChecksumStatus::match : ChecksumStatus::mismatch;
}

[[nodiscard]] IngestResult fail(SmsError error, std::string detail) {
  IngestResult result;
  result.error = error;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

const char* declaration_source_name(DeclarationSource source) noexcept {
  switch (source) {
    case DeclarationSource::build_option: return "build_option";
    case DeclarationSource::manifest: return "manifest";
    case DeclarationSource::fixture_builder: return "fixture_builder";
    case DeclarationSource::local_identity: return "local_identity";
  }
  return "invalid";
}

const char* checksum_status_name(ChecksumStatus status) noexcept {
  switch (status) {
    case ChecksumStatus::match: return "match";
    case ChecksumStatus::mismatch: return "mismatch";
    case ChecksumStatus::not_evaluated: return "not_evaluated";
  }
  return "invalid";
}

const char* mapper_family_name(SmsMapperFamily family) noexcept {
  switch (family) {
    case SMS_MAPPER_SEGA: return "sega";
    case SMS_MAPPER_ROM_ONLY: return "rom_only";
    case SMS_MAPPER_UNDECLARED: break;
  }
  return "undeclared";
}

ManifestResult parse_mapper_manifest(std::string_view text, std::string_view input_sha256) {
  ManifestResult result;
  const auto reject = [&](std::string detail) {
    result.error = SMS_ERROR_MAPPER_UNDECLARED;
    result.detail = std::move(detail);
    return result;
  };
  std::map<std::string, std::string> keys;
  if (!parse_flat_json(text, keys)) return reject("mapper manifest is not a flat JSON object");
  const auto mapper = keys.find("mapper");
  if (mapper == keys.end() || mapper->second.empty()) return reject("mapper manifest names no mapper");
  const auto digest = keys.find("sha256");
  if (digest == keys.end()) return reject("mapper manifest carries no sha256 of the input");
  if (digest->second != input_sha256) return reject("mapper manifest sha256 does not match the input");
  DeclarationSource source = DeclarationSource::manifest;
  if (const auto declared = keys.find("declaration_source"); declared != keys.end()) {
    if (declared->second == "fixture_builder") source = DeclarationSource::fixture_builder;
    else if (declared->second == "local_identity") source = DeclarationSource::local_identity;
    else if (declared->second != "manifest") return reject("mapper manifest has an unknown declaration_source");
  }
  result.declaration = {mapper->second, source};
  return result;
}

IngestResult ingest_cartridge(std::span<const std::uint8_t> rom, const IngestOptions& options) {
  // 1. Platform / profile. The header identifies platform, region and profile only.
  const RomInfo info = inspect_rom(rom);
  IngestResult result;
  CartridgeIdentity& id = result.identity;
  id.profile = profile_name;
  if (info.outcome == ClassificationOutcome::recognized) {
    if (info.platform != Platform::master_system)
      return fail(SMS_ERROR_PROFILE_UNSUPPORTED, std::string("header recognizes another platform: ") + platform_name(info.platform));
    if (info.region_system != "sms_export")
      return fail(SMS_ERROR_PROFILE_UNSUPPORTED, "unsupported region " + info.region_system + " (baseline: sms_export)");
    id.header_class = "tmr_sega";
    id.region = info.region_system;
    id.header_size_code = info.rom_size_code;
    for (const auto& candidate : info.candidates)
      if (candidate.status == "valid" && candidate.family == "master_system") id.header_offset = candidate.offset;
    id.checksum = evaluate_checksum(rom, id.header_offset, id.header_size_code);
  } else {
    if (!options.explicit_profile)
      return fail(SMS_ERROR_PROFILE_UNSUPPORTED,
                  std::string("no unambiguous Master System header (") + diagnostic_name(info.diagnostic) +
                      "): select the platform/profile explicitly");
    id.header_class = "none";
    id.region = "unspecified";
  }

  // 2. Size.
  const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(rom.size(), 0xFFFFFFFFu));
  if (rom.size() != size || !sms_rom_size_supported(size))
    return fail(SMS_ERROR_ROM_SIZE_UNSUPPORTED, "ROM size " + std::to_string(rom.size()) + " is not 32/64/128/256/512 KiB");

  // 3. Mapper identity: declared, never inferred. Precedence by source; differing names are ambiguous.
  if (options.declarations.empty())
    return fail(SMS_ERROR_MAPPER_UNDECLARED, "no mapper declaration (build option, manifest, fixture or local identity)");
  auto declarations = options.declarations;
  std::stable_sort(declarations.begin(), declarations.end(),
                   [](const MapperDeclaration& a, const MapperDeclaration& b) { return a.source < b.source; });
  const std::string name = normalize(declarations.front().family);
  for (const auto& declaration : declarations)
    if (normalize(declaration.family) != name) return fail(SMS_ERROR_MAPPER_UNDECLARED, "conflicting mapper declarations");
  if (name == "sega") id.mapper = SMS_MAPPER_SEGA;
  else if (name == "rom_only") id.mapper = SMS_MAPPER_ROM_ONLY;
  else if (known_unsupported(name)) return fail(SMS_ERROR_MAPPER_UNSUPPORTED, "mapper family '" + name + "' is outside the baseline");
  else return fail(SMS_ERROR_MAPPER_UNDECLARED, "unknown mapper family name");
  id.declaration_source = declarations.front().source;
  if (id.mapper == SMS_MAPPER_ROM_ONLY && size != SMS_ROM_ONLY_SIZE)
    return fail(SMS_ERROR_ROM_SIZE_UNSUPPORTED, "rom_only requires a 32 KiB ROM");

  id.size = rom.size();
  id.bank_count = sms_bank_count(size);
  id.sha256 = sha256_hex(rom);
  result.rom.assign(rom.begin(), rom.end());
  return result;
}

}  // namespace segarecomp::machine::master_system
