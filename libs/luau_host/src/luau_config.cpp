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
  h.platform = manifest.platform;
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
  if (m.has_hooks) return mod::Rejection{"E_HOOK_CALLBACK_UNSUPPORTED", "function hook callbacks are native ABI 2 only; a Luau callback for a hook is not supported"};
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
  return {"host.log", "overlay.rgba", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "input.read", "input.intercept", "state.mod", "audio.command.send", "audio.event.read"};
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
  if (m.has_hooks) return mod::Rejection{"E_HOOK_CALLBACK_UNSUPPORTED", "function hook callbacks are native ABI 2 only; a Luau callback for a hook is not supported"};
  if (m.abi_native != 0) return mod::Rejection{"E_LIVE_ABI", "a native ABI 2 provider does not run here"};
  if (auto r = mod::check_audio_interface(m)) return r;
  const std::vector<std::string> live = scheduled_capabilities();
  for (const std::string &c : m.capabilities)
    if (std::find(live.begin(), live.end(), c) == live.end())
      return mod::Rejection{"E_LIVE_CAPABILITY", "the capability " + c + " is not supported by the scheduled path (supported: host.log, overlay.rgba, memory.read/write.m68k.work_ram, input.read, input.intercept, state.mod, audio.command.send, audio.event.read)"};
  return std::nullopt;
}

namespace {
// The scheduled grant set: what the package declared and the platform supports, restricted to what this execution path serves. An audio capability without a servable mapping is not granted (a REQUIRED one was
// already refused by check_audio_interface), so the artifact, the build status, the VM bindings, the native capability bits, the scheduler grant and MOD_LOADED all come from this one list (ADR 0323).
HostConfig restricted_config(const mod::Manifest &manifest, const std::vector<std::string> &served) {
  HostConfig h = host_config_for(manifest);
  h.capabilities.erase(std::remove_if(h.capabilities.begin(), h.capabilities.end(),
                                      [&](const std::string &c) { return std::find(served.begin(), served.end(), c) == served.end() || !mod::audio_capability_servable(manifest, c); }),
                       h.capabilities.end());
  return h;
}
}  // namespace

std::vector<std::string> native_scheduled_capabilities();

std::optional<mod::Rejection> native_scheduled_support(const mod::Package &package) {
  const mod::Manifest &m = package.manifest;
  if (m.platform != "genesis") return mod::Rejection{"E_LIVE_PLATFORM", "native providers run in the generated-native path of Genesis programs only"};
  if (mod::trust_class(m) != "native") return mod::Rejection{"E_LIVE_TRUST", "only a native ABI 2 package runs here; Luau-only and data-only packages use their own paths"};
  if (m.abi_native != 2) return mod::Rejection{"E_LIVE_ABI", "only the native ABI 2 provider is linked into a built program"};
  bool has_native = false;
  for (const mod::Payload &p : m.payloads) has_native = has_native || p.kind == "native-c11";
  if (!has_native) return mod::Rejection{"E_LIVE_TRUST", "the package has no native-c11 source"};
  for (const mod::Payload &p : m.payloads)   // nothing the program does not use may be silently dropped: the native units and the data that make the effective image
    if (p.kind != "native-c11" && p.kind != "rom-edit-set" && p.kind != "guest-code" && p.kind != "guest-asm" && p.kind != "bps-image")
      return mod::Rejection{"E_LIVE_PAYLOAD", "the payload " + p.path + " (" + p.kind + ") is not used by the scheduled path; remove it or use a package without it"};
  if (auto r = mod::check_audio_interface(m)) return r;
  const std::vector<std::string> live = native_scheduled_capabilities();
  for (const std::string &c : m.capabilities)
    if (std::find(live.begin(), live.end(), c) == live.end())
      return mod::Rejection{"E_LIVE_CAPABILITY", "the capability " + c + " is not supported by the scheduled path (supported: host.log, overlay.rgba, scenery.words, frame.work_ram, memory.read/write.m68k.work_ram, memory.read.m68k.rom, input.read, input.intercept, state.mod, hook.function, audio.command.send, audio.event.read)"};
  if (m.has_hooks && std::find(m.capabilities.begin(), m.capabilities.end(), "hook.function") == m.capabilities.end())
    return mod::Rejection{"E_LIVE_CAPABILITY", "a package with function hooks must require hook.function"};
  return std::nullopt;
}

// SEG-138-T012 (ADR 0308): a native provider additionally gets `hook.function` (statically resolved function hooks, ADR 0298); a Luau package never does.
std::vector<std::string> native_scheduled_capabilities() {
  std::vector<std::string> caps = scheduled_capabilities();
  caps.push_back("hook.function");
  caps.push_back("frame.work_ram");   // W04 gap B: the render-phase copy of end-of-frame work RAM; it is what binds a game-art virtual-sprite set to its frame epoch (mod_capture.cpp)
  caps.push_back("scenery.words");   // SEG-140-T011: authored offscreen scenery through the typed render feature
  caps.push_back("memory.read.m68k.rom");   // SEG-152-T018 (ADR 0321): declared effective-image ranges, render phase only
  caps.push_back("frame.vdp");   // SEG-152-T003 (ADR 0320): REQUIRED-only; selects the exact-frame render callback at the producer seam (the package layer refuses an optional declaration)
  return caps;
}

HostConfig native_scheduled_host_config(const mod::Manifest &manifest) { return restricted_config(manifest, native_scheduled_capabilities()); }

HostConfig scheduled_host_config(const mod::Manifest &manifest) { return restricted_config(manifest, scheduled_capabilities()); }

namespace {
const std::vector<std::string> kSmsScheduled = {"host.log",        "overlay.rgba",   "present.transform", "frame.work_ram", "frame.sms" "_vdp", "memory.read.z80.work_ram", "memory.read.z80.cart_ram",
                                                "memory.write.z80.work_ram", "input.read", "input.intercept", "state.mod"};
}

std::optional<mod::Rejection> sms_scheduled_support(const mod::Package &package, bool native) {
  const mod::Manifest &m = package.manifest;
  if (m.platform != "master-system") return mod::Rejection{"E_LIVE_PLATFORM", "this route builds Master System programs"};
  if (mod::trust_class(m) != (native ? "native" : "luau")) return mod::Rejection{"E_LIVE_TRUST", native ? "only a native ABI 2 package runs here" : "only a Luau-only package runs here"};
  if (native && m.abi_native != 2) return mod::Rejection{"E_LIVE_ABI", "only the native ABI 2 provider is linked into a built program"};
  if (!native && m.abi_native != 0) return mod::Rejection{"E_LIVE_ABI", "a native ABI 2 provider does not run here"};
  const std::string source = native ? "native-c11" : "luau-source";
  bool has_source = false;
  for (const mod::Payload &p : m.payloads) has_source = has_source || p.kind == source;
  if (!has_source) return mod::Rejection{"E_LIVE_TRUST", "the package has no " + source};
  for (const mod::Payload &p : m.payloads)   // nothing the program does not use may be silently dropped: the scripts or units and the data that make the effective image
    if (p.kind != source && p.kind != "rom-edit-set" && p.kind != "guest-code" && p.kind != "guest-asm" && p.kind != "bps-image")
      return mod::Rejection{"E_LIVE_PAYLOAD", "the payload " + p.path + " (" + p.kind + ") is not used by the scheduled path; remove it or use a package without it"};
  if (m.has_hooks) return mod::Rejection{"E_HOOK_CPU_UNSUPPORTED", "function hooks have no Z80 implementation"};
  for (const std::string &c : m.capabilities)
    if (std::find(kSmsScheduled.begin(), kSmsScheduled.end(), c) == kSmsScheduled.end())
      return mod::Rejection{"E_LIVE_CAPABILITY", "the capability " + c + " is not supported by the Master System scheduled path"};
  return std::nullopt;
}

HostConfig sms_scheduled_host_config(const mod::Manifest &manifest) { return restricted_config(manifest, kSmsScheduled); }

HostConfig live_host_config(const mod::Manifest &manifest) {
  HostConfig h = host_config_for(manifest);
  const std::vector<std::string> live = live_capabilities();
  h.capabilities.erase(std::remove_if(h.capabilities.begin(), h.capabilities.end(), [&](const std::string &c) { return std::find(live.begin(), live.end(), c) == live.end(); }), h.capabilities.end());
  return h;
}

}  // namespace segarecomp::luau
