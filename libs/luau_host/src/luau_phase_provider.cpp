// SEG-138-T001/T002 (ADR 0297): the Luau adapter of the mod phase scheduler. See luau_phase_provider.hpp.
#include "segarecomp/luau_phase_provider.hpp"

#include "segarecomp/mod.hpp"

#include <cstring>

namespace segarecomp::luau {

struct PhaseProvider::Impl {
  std::unique_ptr<Vm> vm;
  SegaModProvider table{};
  std::uint32_t mask = 0;
  std::uint64_t caps[2] = {0, 0};
  std::vector<OverlayOp> overlay;
  FaultSink sink = nullptr;
};

namespace {

struct Mapping {
  std::uint32_t phase;
  const char *callback;
  std::uint32_t index;   // deterministic callback index (seeds math.random; 0 and 1 are the historical reset/render)
};
constexpr Mapping kPhases[] = {
    {SEGA_MOD_PHASE_RESET, "reset", 0},         {SEGA_MOD_PHASE_FRAME_BEGIN, "frame_begin", 4}, {SEGA_MOD_PHASE_INPUT, "input", 5},
    {SEGA_MOD_PHASE_POST_INPUT, "post_input", 6}, {SEGA_MOD_PHASE_FRAME_END, "frame_end", 7},     {SEGA_MOD_PHASE_RENDER, "render", 1},
};

int svc_read(void *ctx, std::uint32_t space, std::uint32_t offset, std::uint32_t width, std::uint32_t *value) {
  const auto *h = static_cast<const SegaModHost *>(ctx);
  return h->read != nullptr ? h->read(space, offset, width, value) : SEGA_MOD_E_CAPABILITY;
}
int svc_write(void *ctx, std::uint32_t space, std::uint32_t offset, std::uint32_t width, std::uint32_t value) {
  const auto *h = static_cast<const SegaModHost *>(ctx);
  return h->write != nullptr ? h->write(space, offset, width, value) : SEGA_MOD_E_CAPABILITY;
}
int svc_input_get(void *ctx, std::uint32_t port, std::uint32_t *buttons) {
  const auto *h = static_cast<const SegaModHost *>(ctx);
  return h->input_get != nullptr ? h->input_get(port, buttons) : SEGA_MOD_E_CAPABILITY;
}
int svc_input_set(void *ctx, std::uint32_t port, std::uint32_t buttons) {
  const auto *h = static_cast<const SegaModHost *>(ctx);
  return h->input_set != nullptr ? h->input_set(port, buttons) : SEGA_MOD_E_CAPABILITY;
}

void phase_entry(void *state, const SegaModHost *host) {
  auto *impl = static_cast<PhaseProvider::Impl *>(state);
  const Mapping *map = nullptr;
  for (const Mapping &m : kPhases)
    if (m.phase == host->phase) map = &m;
  if (map == nullptr || impl->vm == nullptr) return;
  HostServices services;
  services.ctx = const_cast<SegaModHost *>(host);
  services.read = svc_read;
  services.write = svc_write;
  services.input_get = svc_input_get;
  services.input_set = svc_input_set;
  FrameInput in;
  in.number = host->frame;
  in.services = &services;
  Output out;
  if (host->phase == SEGA_MOD_PHASE_RENDER) impl->overlay.clear();
  const CallResult r = impl->vm->run_callback(map->callback, in, map->index, out);
  if (!r.ok) {
    if (impl->sink != nullptr) impl->sink(fault_name(r.fault));
    return;
  }
  for (const std::string &line : out.log)
    if (host->log != nullptr) host->log(line.data(), static_cast<std::uint32_t>(line.size()));
  if (host->phase == SEGA_MOD_PHASE_RENDER) impl->overlay = std::move(out.overlay);
}

}  // namespace

PhaseProvider::PhaseProvider(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PhaseProvider::~PhaseProvider() = default;

std::unique_ptr<PhaseProvider> PhaseProvider::create(std::string_view identity, const CompiledMod &compiled, const Limits &limits, const HostConfig &host, FaultSink sink, std::string &error) {
  auto impl = std::make_unique<Impl>();
  impl->sink = sink;
  impl->vm = Vm::create(identity, limits, host);
  if (!impl->vm) {
    error = "E_VM_CREATE";
    return nullptr;
  }
  const CallResult loaded = impl->vm->load(compiled);
  if (!loaded.ok) {
    error = std::string("E_MOD_FAULT cause=") + fault_name(loaded.fault) + " callback=load: " + loaded.message;
    return nullptr;
  }
  for (const Mapping &m : kPhases)
    if (impl->vm->defines(m.callback)) impl->mask |= 1U << m.phase;
  for (const std::string &name : host.capabilities)
    if (const mod::CapabilityInfo *info = mod::find_capability(name)) impl->caps[info->id / 64U] |= std::uint64_t{1} << (info->id % 64U);
  impl->table.size = sizeof(SegaModProvider);
  impl->table.abi_major = SEGA_MOD_ABI_MAJOR;
  impl->table.required_minor = 0;
  impl->table.platform = SEGA_MOD_PLATFORM_GENESIS;
  impl->table.name = "luau";
  impl->table.implementation_version = 1;
  impl->table.phase = phase_entry;
  return std::unique_ptr<PhaseProvider>(new PhaseProvider(std::move(impl)));
}

const SegaModProvider *PhaseProvider::provider() const { return &impl_->table; }
void *PhaseProvider::state() { return impl_.get(); }
std::uint32_t PhaseProvider::phase_mask() const { return impl_->mask; }
void PhaseProvider::capabilities(std::uint64_t out[2]) const {
  out[0] = impl_->caps[0];
  out[1] = impl_->caps[1];
}
const std::vector<OverlayOp> &PhaseProvider::overlay() const { return impl_->overlay; }
bool PhaseProvider::nondeterministic() const { return impl_->vm != nullptr && impl_->vm->nondeterministic(); }
Fault PhaseProvider::fault() const { return impl_->vm != nullptr ? impl_->vm->fault() : Fault::none; }

}  // namespace segarecomp::luau
