// SEG-147-T004 (ADR 0325): the Luau provider of a Master System program. `segarecomp build --luau-package` links this file, the Luau VM, the verified compiled scripts (the generated artifact) and the SAME
// scheduler, host and compositor a native package uses; this file only defines the `segarecomp_mod_provider` entry a native package would export. So a script reaches the machine through exactly the
// SegaModHost services and the SegaSmsRenderApi a C11 provider calls: one set of capability, phase, range and budget rules.
extern "C" {   // the SDK core declares the provider entry for C: this definition must have C linkage, and so must the declaration every other include sees
#include <segarecomp_mod.h>
}
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_phase_provider.hpp"

#include <segarecomp_mod_sms.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

extern "C" {
extern const unsigned char segarecomp_luau_artifact[];
extern const unsigned long segarecomp_luau_artifact_size;
extern const char segarecomp_luau_bytecode_digest[];
extern const unsigned long segarecomp_luau_memory_bytes, segarecomp_luau_budget_units;
extern const char segarecomp_sms_mod_identity[];
extern const char segarecomp_sms_mod_capabilities[];
void sms_mod_fault_current(const char *cause);   // sms_mod_scheduler.h
}

namespace {

using namespace segarecomp;

std::unique_ptr<luau::PhaseProvider> g_provider;

const SegaMasterSystemFeatures *features_of(void *ctx) {
  return static_cast<const SegaMasterSystemFeatures *>(static_cast<const SegaModHost *>(ctx)->platform_features);
}

// The render requests of one script call, forwarded to the host's render table. No table (neither presentation capability was granted) is the capability status.
int present(void *ctx, luau::PresentOp op, const std::int64_t *a, const std::uint8_t *bytes, std::uint32_t length) {
  const SegaSmsRenderApi *api = features_of(ctx)->render;
  if (api == nullptr) return SEGA_MOD_E_CAPABILITY;
  const auto i32 = [&](int n) { return static_cast<std::int32_t>(a[n]); };
  const auto u32 = [&](int n) { return static_cast<std::uint32_t>(a[n]); };
  switch (op) {
    case luau::PresentOp::rect: return api->overlay_rect(i32(0), i32(1), i32(2), i32(3), u32(4));
    case luau::PresentOp::text: return api->overlay_text(i32(0), i32(1), reinterpret_cast<const char *>(bytes), length, u32(2));
    case luau::PresentOp::color: return api->palette_color(u32(0), u32(1));
    case luau::PresentOp::scroll: return api->scroll(i32(0), i32(1));
    case luau::PresentOp::pattern: return api->sprite_pattern(u32(0), bytes);
    case luau::PresentOp::colors: return api->sprite_colors(bytes);
    case luau::PresentOp::sprite: return api->sprite_add(i32(0), i32(1), u32(2), u32(3), u32(4), u32(5));
  }
  return SEGA_MOD_E_RANGE;
}

int vdp_byte(void *ctx, luau::VdpRegion region, std::uint32_t index, std::uint32_t *value) {
  const SegaSmsVdpView *v = features_of(ctx)->vdp;
  if (v == nullptr) return SEGA_MOD_E_CAPABILITY;
  const auto take = [&](const std::uint8_t *base, std::uint32_t at, std::uint32_t limit) {
    if (at >= limit) return static_cast<int>(SEGA_MOD_E_RANGE);
    *value = base[at];
    return static_cast<int>(SEGA_MOD_OK);
  };
  int status = SEGA_MOD_E_RANGE;
  switch (region) {
    case luau::VdpRegion::vram: status = take(v->vram, index, 16384U); break;
    case luau::VdpRegion::cram: status = take(v->cram, index, 32U); break;
    case luau::VdpRegion::registers: status = take(v->registers, index, 16U); break;
    case luau::VdpRegion::sprite_y: status = index < 64U ? take(v->vram, v->sprite_table + index, 16384U) : SEGA_MOD_E_RANGE; break;
    case luau::VdpRegion::sprite_x: status = index < 64U ? take(v->vram, v->sprite_table + 128U + 2U * index, 16384U) : SEGA_MOD_E_RANGE; break;
    case luau::VdpRegion::sprite_pattern: status = index < 64U ? take(v->vram, v->sprite_table + 129U + 2U * index, 16384U) : SEGA_MOD_E_RANGE; break;
  }
  return status;
}

void fill(luau::HostServices &services) {
  services.present = present;
  services.vdp_byte = vdp_byte;
}

bool create() {
  const std::span<const std::uint8_t> stored(segarecomp_luau_artifact, static_cast<std::size_t>(segarecomp_luau_artifact_size));
  const luau::CompileResult loaded = luau::load_stored(segarecomp_sms_mod_identity, stored, segarecomp_luau_bytecode_digest);
  if (!loaded.ok) {
    std::fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", loaded.rejection.code.c_str());
    return false;
  }
  luau::HostConfig host;
  host.platform = "master-system";
  const std::string names = segarecomp_sms_mod_capabilities;
  for (std::size_t from = 0; from < names.size();) {
    std::size_t to = names.find(',', from);
    if (to == std::string::npos) to = names.size();
    host.capabilities.push_back(names.substr(from, to - from));
    from = to + 1;
  }
  luau::Limits limits = luau::policy_limits();   // a package may only lower the policy (the reader refused anything above it)
  if (segarecomp_luau_memory_bytes != 0 && segarecomp_luau_memory_bytes < limits.memory_bytes) limits.memory_bytes = segarecomp_luau_memory_bytes;
  if (segarecomp_luau_budget_units != 0 && segarecomp_luau_budget_units < limits.callback_units) limits.callback_units = segarecomp_luau_budget_units;
  std::string error;
  g_provider = luau::PhaseProvider::create(segarecomp_sms_mod_identity, loaded.compiled, limits, host, sms_mod_fault_current, error);
  if (g_provider == nullptr) {
    std::fprintf(stderr, "MOD_REFUSED {\"class\":\"E_VM_CREATE\"}\n");
    return false;
  }
  g_provider->set_service_fill(fill);
  return true;
}

}  // namespace

extern "C" const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (g_provider == nullptr && !create()) return nullptr;
  if (state_out != nullptr) *state_out = g_provider->state();
  return g_provider->provider();
}
extern "C" std::uint32_t segarecomp_sms_mod_phase_mask(void) { return g_provider != nullptr ? g_provider->phase_mask() : 0U; }
extern "C" int segarecomp_sms_mod_nondeterministic(void) { return g_provider != nullptr && g_provider->nondeterministic() ? 1 : 0; }
