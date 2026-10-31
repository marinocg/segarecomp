#include "segarecomp/machine/sega_cd/boot_contract.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {

const char *boot_proof_name(BootProof proof) noexcept { return proof == BootProof::proven ? "proven" : "conditional"; }

const char *boot_contract_refusal_name(BootContractRefusal refusal) noexcept {
  switch (refusal) {
  case BootContractRefusal::none: return "none";
  case BootContractRefusal::bios_not_verified: return "bios_not_verified";
  case BootContractRefusal::disc_boot_area: return "disc_boot_area";
  case BootContractRefusal::header_not_verified: return "header_not_verified";
  case BootContractRefusal::boot_area_short: return "boot_area_short";
  }
  return "unknown";
}

namespace {

// A verified profile. `bios_sha256` pins an exact authentic BIOS image; `bios_tag` selects a project-authored fixture BIOS. The header form is
// the one the profile's placement was observed with; a fixture accepts any header the parser accepts (its BIOS reads the disc's descriptors).
struct Profile {
  const char *name;
  const char *bios_sha256;      // exact image, or nullptr
  const char *bios_tag;         // exactly kBiosTagBytes characters at kBiosTagOffset, or nullptr (a self-asserted selector for project fixtures, never authority)
  std::uint32_t ip_length;      // bytes the BIOS places from kBootIpTrackOffset at kIpBase
  bool header_exact;            // the disc's IP/SP descriptors must equal the verified form below
  std::uint32_t ip_descriptor_address, ip_descriptor_size, sp_descriptor_address, sp_descriptor_size;
};

constexpr Profile kProfiles[] = {
    // Authentic US Model 1 BIOS: ADR 0257 sections 1 and 4 (observed on two commercial discs with this header form; the main-side Word RAM -> Work RAM copy is an immediate-operand loop of the BIOS).
    {"sega_cd_model1_v110_us", "807384678ef259494d943eddb6954dfc7069693d0dde0c424e2028488d88e81a", nullptr, 0x8000U, true, 0x800U, 0x800U, 0x1000U, 0x7000U},
    // Authentic US Model 2 BIOS: the same placement, observed on the same two discs (ADR 0257 section 4).
    {"sega_cd_model2_v200w_us", "b1c2036b79467514eaaa69c76e5ba83801621e821059caa53cb921a0b21e3af4", nullptr, 0x8000U, true, 0x800U, 0x800U, 0x1000U, 0x7000U},
    // Project-authored fixture BIOS images (tools/sega_cd_bios_boot_contract_fixture.py): the same contract with a shorter or the full IP.
    {"project_fixture_boot_contract_1", nullptr, "SEGA BOOT-CONTRACT FIXTURE 1    ", 0x8000U, false, 0U, 0U, 0U, 0U},
    {"project_fixture_boot_contract_2", nullptr, "SEGA BOOT-CONTRACT FIXTURE 2    ", 0x4000U, false, 0U, 0U, 0U, 0U},
    // The same contract verified only for one header form (the exact-form logic of the authentic profile, exercised without a commercial BIOS).
    {"project_fixture_boot_contract_3", nullptr, "SEGA BOOT-CONTRACT FIXTURE 3    ", 0x8000U, true, 0x200U, 0x600U, 0x1000U, 0x400U},
};

const Profile *match_profile(const BiosImage &bios, BootProfileScope scope) {
  for (const auto &p : kProfiles) {
    if (p.bios_sha256 != nullptr && bios.sha256 == p.bios_sha256) return &p;
    if (p.bios_tag != nullptr && scope == BootProfileScope::with_test_fixtures && std::strlen(p.bios_tag) == kBiosTagBytes && bios.bytes.size() >= kBiosTagOffset + kBiosTagBytes &&
        std::memcmp(bios.bytes.data() + kBiosTagOffset, p.bios_tag, kBiosTagBytes) == 0)
      return &p;
  }
  return nullptr;
}

bool read_track1(const DiscImage &disc, std::uint32_t offset, std::uint32_t length, std::vector<std::uint8_t> &bytes) {
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) return false;
  const auto &first = disc.tracks().front();
  const std::uint64_t end = static_cast<std::uint64_t>(offset) + length;
  if ((end + kDiscUserBytes - 1U) / kDiscUserBytes > first.sector_count) return false;
  bytes.clear();
  bytes.reserve(length);
  std::array<std::uint8_t, kDiscUserBytes> block{};
  for (std::uint64_t at = offset; at < end;) {
    if (disc.read_user_data(first.first_lba + static_cast<std::uint32_t>(at / kDiscUserBytes), std::span<std::uint8_t, kDiscUserBytes>(block)) != DiscError::none) return false;
    const auto inside = static_cast<std::uint32_t>(at % kDiscUserBytes);
    const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(kDiscUserBytes - inside, end - at));
    bytes.insert(bytes.end(), block.begin() + inside, block.begin() + inside + take);
    at += take;
  }
  return true;
}

}  // namespace

BootContract derive_boot_contract(const BiosImage &bios, const DiscImage &disc, BootProfileScope scope) {
  BootContract out;
  const Profile *profile = match_profile(bios, scope);
  if (profile == nullptr) {
    out.refusal = BootContractRefusal::bios_not_verified;
    return out;
  }
  out.profile = profile->name;
  out.authority = profile->bios_tag != nullptr ? "test_assumed_fixture" : "verified_authentic";   // reported with every outcome, refusals included
  const auto boot = read_sega_cd_boot(disc);
  if (!boot.header) {
    out.refusal = BootContractRefusal::disc_boot_area;
    return out;
  }
  const auto &h = *boot.header;
  if (profile->header_exact && (h.ip.disc_address != profile->ip_descriptor_address || h.ip.load_size != profile->ip_descriptor_size ||
                                h.sp.disc_address != profile->sp_descriptor_address || h.sp.load_size != profile->sp_descriptor_size)) {
    out.refusal = BootContractRefusal::header_not_verified;
    return out;
  }
  // The IP side is fixed by the BIOS; the SP side is the disc's own descriptor (range-checked against the system area by the parser).
  BootContractImage ip{Processor::main, kBootIpTrackOffset, kIpBase, profile->ip_length, BootProof::conditional, {}};
  BootContractImage sp{Processor::sub, h.sp.disc_address, kSpBase, h.sp.load_size, BootProof::conditional, {}};
  if (static_cast<std::uint64_t>(sp.base) + sp.length > kPrgRamBytes || !read_track1(disc, ip.track_offset, ip.length, ip.bytes) ||
      !read_track1(disc, sp.track_offset, sp.length, sp.bytes)) {
    out.refusal = BootContractRefusal::boot_area_short;
    return out;
  }
  out.images.push_back(std::move(ip));
  out.images.push_back(std::move(sp));
  return out;
}

std::string static_boot_admission(const BootContractImage &image) {
  return "static-boot:" + std::string(processor_name(image.processor)) + ":" + std::to_string(image.track_offset) + ":" + std::to_string(image.base) + ":" +
         std::to_string(image.length) + ":" + sha256_hex(image.bytes);
}

}  // namespace segarecomp::machine::sega_cd
