#include "segarecomp/mod.hpp"

#include <algorithm>
#include <array>

namespace segarecomp::mod {

namespace {

constexpr std::array<std::string_view, 2> kGenesisRead = {"m68k.work_ram", "z80.ram"};
constexpr std::array<std::string_view, 2> kSmsRead = {"z80.work_ram", "z80.cart_ram"};
constexpr std::array<std::string_view, 4> kScdRead = {"main.work_ram", "main.word_ram", "sub.prg_ram", "z80.ram"};
constexpr std::array<std::string_view, 1> kU8Mono = {"u8-mono"};

// The adapter table (mod-platform-adapters.md sections 2 and 3). Master System has no PCM channel in this milestone (PSG only), hence no audio conversion.
constexpr std::array<Adapter, 3> kAdapters = {{
    {"genesis", "cartridge-image", 1U, "segarecomp_mod_genesis.h", "m68k.work_ram", kGenesisRead, kU8Mono, 0},
    {"master-system", "cartridge-image", 2U, "segarecomp_mod_sms.h", "z80.work_ram", kSmsRead, {}, 1},
    {"sega-cd", "disc-image", 3U, "segarecomp_mod_scd.h", "main.work_ram", kScdRead, kU8Mono, 2},
}};

constexpr Support U = Support::unsupported, E = Support::experimental, L = Support::supported_with_limitations;

// Capability table (native-sdk-abi-2.md section 4). Genesis keeps the four ABI 1 names at `supported-with-limitations` (they run today). The ABI 2 services are declarable on Genesis
// (`experimental`) while their runtime lands in M05; function hooks and host audio are gated. Master System and Sega CD register lifecycle + `host.log` only (SEG-146-T003);
// everything else is `unsupported` until SEG-147 / SEG-148 raise it, and a package that needs it is rejected, never ignored.
constexpr std::array<CapabilityInfo, 23> kCapabilities = {{
    {"host.log", 1, {E, E, E}, true},
    {"frame.work_ram", 2, {L, U, U}, true},
    {"frame.vdp", 3, {L, U, U}, true},
    {"frame.sms_vdp", 4, {U, U, U}, true},
    {"scenery.words", 5, {L, U, U}, false},
    {"overlay.rgba", 6, {L, U, U}, true},
    {"memory.read.m68k.work_ram", 7, {E, U, U}, true},
    {"memory.read.z80.ram", 7, {U, U, U}, true},
    {"memory.read.z80.work_ram", 7, {U, U, U}, true},
    {"memory.read.z80.cart_ram", 7, {U, U, U}, true},
    {"memory.read.main.work_ram", 7, {U, U, U}, true},
    {"memory.read.main.word_ram", 7, {U, U, U}, true},
    {"memory.read.sub.prg_ram", 7, {U, U, U}, true},
    {"memory.write.m68k.work_ram", 8, {E, U, U}, true},
    {"memory.write.z80.work_ram", 8, {U, U, U}, true},
    {"memory.write.main.work_ram", 8, {U, U, U}, true},
    {"input.read", 9, {E, U, U}, true},
    {"input.intercept", 10, {E, U, U}, true},
    {"hook.function", 11, {U, U, U}, true},
    {"state.mod", 12, {E, U, U}, true},
    {"audio.event.read", 13, {E, U, U}, true},
    {"audio.command.send", 14, {E, U, U}, true},
    {"audio.host.play", 15, {U, U, U}, false},
}};

}  // namespace

const char *support_name(Support level) {
  switch (level) {
    case Support::unsupported: return "unsupported";
    case Support::experimental: return "experimental";
    case Support::supported_with_limitations: return "supported-with-limitations";
    case Support::supported_and_tested: return "supported-and-tested";
  }
  return "unsupported";
}

std::span<const Adapter> adapters() { return kAdapters; }

const Adapter *find_adapter(std::string_view platform) {
  for (const Adapter &a : kAdapters) if (a.platform == platform) return &a;
  return nullptr;
}

std::span<const CapabilityInfo> capability_catalogue() { return kCapabilities; }

const CapabilityInfo *find_capability(std::string_view name) {
  for (const CapabilityInfo &c : kCapabilities) if (c.name == name) return &c;
  return nullptr;
}

Support capability_support(const Adapter &adapter, const CapabilityInfo &capability) { return capability.level[adapter.index]; }

std::optional<Rejection> check_capabilities(const Adapter &adapter, const std::vector<std::string> &required, const std::vector<std::string> &optional) {
  for (const std::string &name : optional)
    if (find_capability(name) == nullptr) return Rejection{"E_CAPABILITY_UNKNOWN", "the optional capability \"" + name + "\" is not in the catalogue"};
  for (const std::string &name : required) {
    const CapabilityInfo *c = find_capability(name);
    if (c == nullptr) return Rejection{"E_CAPABILITY_UNKNOWN", "the capability \"" + name + "\" is not in the catalogue"};
    if (capability_support(adapter, *c) == Support::unsupported)
      return Rejection{"E_CAPABILITY_UNSUPPORTED", "the capability \"" + name + "\" is not supported on " + std::string(adapter.platform)};
  }
  return std::nullopt;
}

}  // namespace segarecomp::mod
