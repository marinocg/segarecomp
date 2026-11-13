// SEG-137-T004/T007 (ADR 0295/0296): what a package declared -> what the VM grants, and what the live generated-native path supports. Needs libs/media; the game program does not link this file.
#include "segarecomp/luau_api.hpp"

#include <algorithm>

namespace segarecomp::luau {

Limits limits_for(const mod::Manifest &manifest) {
  Limits l = policy_limits();
  if (manifest.luau_memory_bytes != 0) l.memory_bytes = std::min(l.memory_bytes, manifest.luau_memory_bytes);
  if (manifest.luau_budget_units != 0) l.callback_units = std::min(l.callback_units, manifest.luau_budget_units);
  return l;
}

HostConfig host_config_for(const mod::Manifest &manifest) {
  HostConfig h;
  const mod::Adapter *adapter = mod::find_adapter(manifest.platform);
  const auto grant = [&](const std::string &name) {
    const mod::CapabilityInfo *c = mod::find_capability(name);
    if (adapter != nullptr && c != nullptr && mod::capability_support(*adapter, *c) != mod::Support::unsupported && std::find(h.capabilities.begin(), h.capabilities.end(), name) == h.capabilities.end())
      h.capabilities.push_back(name);
  };
  for (const std::string &n : manifest.capabilities) grant(n);
  for (const std::string &n : manifest.capabilities_optional) grant(n);
  return h;
}


std::vector<std::string> live_capabilities() { return {"host.log", "overlay.rgba"}; }

std::optional<mod::Rejection> live_support(const mod::Package &package) {
  const mod::Manifest &m = package.manifest;
  if (m.platform != "genesis") return mod::Rejection{"E_LIVE_PLATFORM", "Luau scripts run in the generated-native path of Genesis programs only"};
  if (mod::trust_class(m) != "luau") return mod::Rejection{"E_LIVE_TRUST", "only a Luau-only package runs here; native and data-only packages are not scripts"};
  bool has_luau = false;
  for (const mod::Payload &p : m.payloads) has_luau = has_luau || p.kind == "luau-source";
  if (!has_luau) return mod::Rejection{"E_LIVE_TRUST", "the package has no Luau source"};
  for (const mod::Payload &p : m.payloads)   // nothing the live path does not use may be silently dropped: only the scripts and the data that make the effective image
    if (p.kind != "luau-source" && p.kind != "rom-edit-set" && p.kind != "guest-code" && p.kind != "bps-image")
      return mod::Rejection{"E_LIVE_PAYLOAD", "the payload " + p.path + " (" + p.kind + ") is not used by the live path; remove it or use a package without it"};
  if (m.abi_native != 0) return mod::Rejection{"E_LIVE_ABI", "a native ABI 2 provider does not run here"};
  const std::vector<std::string> live = live_capabilities();
  for (const std::string &c : m.capabilities)
    if (std::find(live.begin(), live.end(), c) == live.end())
      return mod::Rejection{"E_LIVE_CAPABILITY", "the capability " + c + " is not supported by the live path (supported: host.log, overlay.rgba)"};
  return std::nullopt;
}

// SEG-138-T009 (ADR 0300): the headless scheduled path (`segarecomp build --luau-package` without a viewer) runs the ONE mod scheduler's phases, so it additionally grants the work-RAM and input services,
// bounded by the manifest's declared memory ranges for the exact source image. Everything else is refused at build.
std::vector<std::string> scheduled_capabilities() {
  return {"host.log", "overlay.rgba", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "input.read", "input.intercept", "state.mod"};
}

std::optional<mod::Rejection> scheduled_support(const mod::Package &package) {
  const mod::Manifest &m = package.manifest;
  if (m.platform != "genesis") return mod::Rejection{"E_LIVE_PLATFORM", "Luau scripts run in the generated-native path of Genesis programs only"};
  if (mod::trust_class(m) != "luau") return mod::Rejection{"E_LIVE_TRUST", "only a Luau-only package runs here; native and data-only packages are not scripts"};
  bool has_luau = false;
  for (const mod::Payload &p : m.payloads) has_luau = has_luau || p.kind == "luau-source";
  if (!has_luau) return mod::Rejection{"E_LIVE_TRUST", "the package has no Luau source"};
  for (const mod::Payload &p : m.payloads)
    if (p.kind != "luau-source" && p.kind != "rom-edit-set" && p.kind != "guest-code" && p.kind != "guest-asm" && p.kind != "bps-image")
      return mod::Rejection{"E_LIVE_PAYLOAD", "the payload " + p.path + " (" + p.kind + ") is not used by the scheduled path; remove it or use a package without it"};
  if (m.abi_native != 0) return mod::Rejection{"E_LIVE_ABI", "a native ABI 2 provider does not run here"};
  if (m.has_hooks) return mod::Rejection{"E_LIVE_CAPABILITY", "function hooks are not linked into a built program yet"};
  const std::vector<std::string> live = scheduled_capabilities();
  for (const std::string &c : m.capabilities)
    if (std::find(live.begin(), live.end(), c) == live.end())
      return mod::Rejection{"E_LIVE_CAPABILITY", "the capability " + c + " is not supported by the scheduled path (supported: host.log, overlay.rgba, memory.read/write.m68k.work_ram, input.read, input.intercept, state.mod)"};
  return std::nullopt;
}

HostConfig scheduled_host_config(const mod::Manifest &manifest) {
  HostConfig h = host_config_for(manifest);
  const std::vector<std::string> live = scheduled_capabilities();
  h.capabilities.erase(std::remove_if(h.capabilities.begin(), h.capabilities.end(), [&](const std::string &c) { return std::find(live.begin(), live.end(), c) == live.end(); }), h.capabilities.end());
  return h;
}

HostConfig live_host_config(const mod::Manifest &manifest) {
  HostConfig h = host_config_for(manifest);
  const std::vector<std::string> live = live_capabilities();
  h.capabilities.erase(std::remove_if(h.capabilities.begin(), h.capabilities.end(), [&](const std::string &c) { return std::find(live.begin(), live.end(), c) == live.end(); }), h.capabilities.end());
  return h;
}

}  // namespace segarecomp::luau
