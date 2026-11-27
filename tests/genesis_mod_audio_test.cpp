// SEG-145-T008: the audio services of the Genesis mod scheduler (SegaModAudioApi) in isolation: attach validation of the declared interface, name resolution, the phase / capability matrix with its stable
// causes, staged-and-transactional mailbox writes, the render observation copy and the absence of an undeclared capability. A native ABI 2 provider on a project-authored synthetic program; no ROM, no clock.
// The same rules reach Luau through libs/luau_host (mod_audio_api_test.py runs both through built programs).
#include "mod_scheduler.h"
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) {
    std::printf("FAIL: %s\n", message.c_str());
    ++failures;
  }
}

constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint64_t cap(unsigned id) { return uint64_t{1} << id; }
constexpr uint64_t kSend = cap(SEGA_MOD_CAP_AUDIO_COMMAND_SEND), kRead = cap(SEGA_MOD_CAP_AUDIO_EVENT_READ);
constexpr uint32_t kSfx = 0x20U, kMusic = 0x22U, kLast = 0x24U;

GenesisControlTransfer dispatch(GenesisRuntime *rt) { return genesis_runtime_retire_m68k_instruction(rt, 4U, rt->pc); }

std::vector<GenesisModAudioEntry> interface_rows() {   // rows 0..3 are the command mappings, 4.. the event mapping
  return {{"sfx", "blip", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 3U},       {"sfx", "drum", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 4U},
          {"music", "theme", GENESIS_MOD_AUDIO_COMMAND, kMusic, 2U, 0x0102U}, {"music", "stop", GENESIS_MOD_AUDIO_COMMAND, kMusic, 2U, 0x7F7FU},
          {"accepted", "theme", GENESIS_MOD_AUDIO_EVENT, kLast, 1U, 1U},   {"accepted", "blip", GENESIS_MOD_AUDIO_EVENT, kLast, 1U, 3U},
          {"accepted", "alias-of-blip", GENESIS_MOD_AUDIO_EVENT, kLast, 1U, 3U}};
}

std::vector<GenesisModAudioEntry> command_rows() {
  const std::vector<GenesisModAudioEntry> all = interface_rows();
  return {all.begin(), all.begin() + 4};
}
std::vector<GenesisModAudioEntry> event_rows() {
  const std::vector<GenesisModAudioEntry> all = interface_rows();
  return {all.begin() + 4, all.end()};
}

int g_hook_result = 0;
int hook_provider(void *, const SegaModHost *host, uint32_t) {
  const auto *features = static_cast<const SegaGenesisFeatures *>(host->platform_features);
  if (features->audio != nullptr) (void)features->audio->play_sfx(host->phase == SEGA_MOD_PHASE_HOOK_ENTRY ? "blip" : "drum");
  return host->phase == SEGA_MOD_PHASE_HOOK_ENTRY ? g_hook_result : 0;
}

struct Rig {
  const GenesisModHookBinding *hooks = nullptr;
  const SegaGenesisVdpHelpers *helpers = nullptr;
  std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();
  std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
  SegaModProvider provider{};
  std::function<void(const SegaModHost *, const SegaModAudioApi *)> body;
  std::string rejection;
  static void phase(void *state, const SegaModHost *host) {
    auto *r = static_cast<Rig *>(state);
    const auto *features = static_cast<const SegaGenesisFeatures *>(host->platform_features);
    if (r->body) r->body(host, features->audio);
  }
  bool attach(uint64_t required, uint64_t optional, const std::vector<GenesisModAudioEntry> &rows, uint32_t phases = bit(SEGA_MOD_PHASE_FRAME_BEGIN)) {
    std::memset(static_cast<void *>(rt.get()), 0, sizeof(GenesisRuntime));
    rt->pc = 0x200U;
    rt->sr = 0x2700U;
    rt->devices.vdp.registers[1] = 0x44;
    rt->devices.vdp.registers[12] = 0x81;
    provider = SegaModProvider{};
    provider.size = sizeof provider;
    provider.abi_major = SEGA_MOD_ABI_MAJOR;
    provider.platform = SEGA_MOD_PLATFORM_GENESIS;
    provider.name = "audio";
    provider.phase = Rig::phase;
    GenesisModConfig c{};
    c.mod_id = "audio";
    c.provider = &provider;
    c.provider_state = this;
    c.phase_mask = phases;
    c.required[0] = required;
    c.optional[0] = optional;
    c.audio = rows.empty() ? nullptr : rows.data();
    c.audio_count = static_cast<uint32_t>(rows.size());
    if (hooks != nullptr) {
      provider.hook = hook_provider;
      c.hooks = hooks;
      c.hook_count = 1U;
      c.required[0] |= cap(SEGA_MOD_CAP_HOOK_FUNCTION);
    }
    c.vdp_helpers = helpers;
    const char *why = "";
    const bool ok = genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0;
    rejection = why;
    return ok;
  }
  void frames(uint64_t n) {
    for (uint32_t slices = 0; rt->virtual_frame_count < n && slices < 4000U; ++slices)
      if (genesis_runtime_run(rt.get(), dispatch, 997U).kind != GENESIS_RUNNER_RESOURCE_LIMIT) break;
    check(rt->virtual_frame_count >= n, "the synthetic program reached the requested frame");
  }
  ~Rig() { genesis_mod_scheduler_detach(sched.get()); }
};

void attach_validation() {
  Rig ok;
  const bool attached = ok.attach(kSend | kRead, 0U, interface_rows());
  check(attached, "a declared interface attaches: " + ok.rejection);
  const auto refused = [](const std::vector<GenesisModAudioEntry> &rows, uint64_t required, const char *want, const char *what) {
    Rig r;
    check(!r.attach(required, 0U, rows) && r.rejection == want, std::string(what) + ": expected " + want + ", got " + r.rejection);
  };
  refused({}, kSend, "E_AUDIO_TARGET", "audio.command.send required without a command mapping");
  refused({{"accepted", "x", GENESIS_MOD_AUDIO_EVENT, kLast, 1U, 1U}}, kSend | kRead, "E_AUDIO_TARGET", "only an event mapping, command required");
  refused({{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}}, kSend | kRead, "E_AUDIO_TARGET", "audio.event.read required without an event mapping");
  const struct { GenesisModAudioEntry row; const char *what; } bad[] = {
      {{"voice", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}, "a command mapping outside sfx/music"},
      {{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, 0xFFFFU, 2U, 1U}, "a mailbox outside work RAM"},
      {{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, 0x21U, 2U, 1U}, "an odd word mailbox"},
      {{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 3U, 1U}, "width 3"},
      {{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 256U}, "a value wider than the mailbox"},
      {{"sfx", "", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}, "an empty name"},
      {{"sfx", "a-name-that-is-longer-than-thirty-two-bytes", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}, "an over-long name"},
      {{"sfx", "x", 3U, kSfx, 1U, 1U}, "an unknown kind"}};
  for (const auto &b : bad) refused({b.row}, kSend, "E_AUDIO_MAPPING", b.what);
  refused({{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}, {"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 2U}}, kSend, "E_AUDIO_MAPPING", "a repeated name");
  refused({{"sfx", "x", GENESIS_MOD_AUDIO_COMMAND, kSfx, 1U, 1U}, {"sfx", "y", GENESIS_MOD_AUDIO_COMMAND, kSfx + 2U, 1U, 2U}}, kSend, "E_AUDIO_MAPPING", "one mapping at two locations");
  refused(interface_rows(), kSend, "E_AUDIO_MAPPING", "event rows without audio.event.read");

  {   // an OPTIONAL audio capability without a mapping is ABSENT, never a guess
    Rig r;
    const bool optional_attached = r.attach(0U, kSend | kRead, {});
    check(optional_attached, "optional audio capabilities without mappings attach: " + r.rejection);
    bool table = true, send = true;
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *audio) { table = audio != nullptr; send = sega_mod_is_granted(host, SEGA_MOD_CAP_AUDIO_COMMAND_SEND) != 0; };
    r.frames(1);
    check(!table && !send, "neither capability is granted and the audio table is absent");
  }
}

void commands_and_causes() {
  {
    Rig r;
    const bool attached = r.attach(kSend, 0U, command_rows());
    check(attached, "commands attach: " + r.rejection);
    std::vector<int> status;
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *audio) {
      if (host->frame != 1U || audio == nullptr) return;
      status = {audio->play_sfx("drum"), audio->play_music("theme")};
      check(r.rt->work_ram[kSfx] == 0U, "a command is staged: guest memory is unchanged inside the callback");
    };
    r.frames(2);
    check(status == std::vector<int>({SEGA_MOD_OK, SEGA_MOD_OK}), "two declared commands answer OK");
    check(r.rt->work_ram[kSfx] == 4U && r.rt->work_ram[kMusic] == 1U && r.rt->work_ram[kMusic + 1U] == 2U, "committed to the declared mailboxes, big-endian");
    check(r.sched->write_total == 2U && !r.sched->fault.faulted, "both writes entered the replay witness");
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *audio) {
      if (host->frame == 3U && audio != nullptr) check(audio->stop_music() == SEGA_MOD_OK, "stop_music is the declared value \"stop\"");
    };
    r.frames(4);
    check(r.rt->work_ram[kMusic] == 0x7FU && r.rt->work_ram[kMusic + 1U] == 0x7FU, "stop_music wrote the declared stop value");
  }
  const struct {
    uint32_t phase;
    uint64_t caps;
    const char *cause;
    std::function<int(const SegaModAudioApi *)> call;
    const char *what;
  } cases[] = {
      {SEGA_MOD_PHASE_FRAME_BEGIN, kSend, "audio-name", [](const SegaModAudioApi *a) { return a->play_sfx("nope"); }, "an unknown effect name"},
      {SEGA_MOD_PHASE_FRAME_BEGIN, kSend, "audio-name", [](const SegaModAudioApi *a) { return a->play_sfx("blip-and-a-tail-that-is-longer-than-the-bound"); }, "an over-long name"},
      {SEGA_MOD_PHASE_FRAME_BEGIN, kSend, "audio-name", [](const SegaModAudioApi *a) { return a->play_music("blip"); }, "an sfx name on the music mapping"},
      {SEGA_MOD_PHASE_FRAME_BEGIN, kSend, "range", [](const SegaModAudioApi *a) { return a->play_sfx(nullptr); }, "a NULL name"},
      {SEGA_MOD_PHASE_INPUT, kSend, "phase", [](const SegaModAudioApi *a) { return a->play_sfx("blip"); }, "a command in input"},
      {SEGA_MOD_PHASE_RENDER, kSend, "phase", [](const SegaModAudioApi *a) { return a->play_sfx("blip"); }, "a command in render"},
      {SEGA_MOD_PHASE_RESET, kSend, "phase", [](const SegaModAudioApi *a) { return a->stop_music(); }, "a command in reset"},
      {SEGA_MOD_PHASE_POST_INPUT, kRead, "phase", [](const SegaModAudioApi *a) { char n[SEGA_MOD_AUDIO_NAME_BYTES]; return a->read_event("accepted", n); }, "an event read in post-input"},
      {SEGA_MOD_PHASE_FRAME_END, kRead, "audio-target", [](const SegaModAudioApi *a) { char n[SEGA_MOD_AUDIO_NAME_BYTES]; return a->read_event("sfx", n); }, "an undeclared event mapping"},
      {SEGA_MOD_PHASE_FRAME_END, kRead, "range", [](const SegaModAudioApi *a) { return a->read_event("accepted", nullptr); }, "a NULL output"},
      {SEGA_MOD_PHASE_FRAME_END, kRead, "capability", [](const SegaModAudioApi *a) { return a->play_sfx("blip"); }, "a command without audio.command.send"},
      {SEGA_MOD_PHASE_FRAME_BEGIN, kSend, "capability", [](const SegaModAudioApi *a) { char n[SEGA_MOD_AUDIO_NAME_BYTES]; return a->read_event("accepted", n); }, "an event read without audio.event.read"}};
  for (const auto &c : cases) {
    Rig r;
    const bool attached = r.attach(c.caps, 0U, c.caps == kSend ? command_rows() : event_rows(), bit(c.phase));
    check(attached, std::string(c.what) + " attaches: " + r.rejection);
    int status = SEGA_MOD_OK;
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *audio) {
      (void)host;
      if (audio == nullptr || status != SEGA_MOD_OK) return;
      if (c.caps == kSend) (void)audio->play_sfx("blip"); /* a valid call first: the fault must discard it too */
      status = c.call(audio);
    };
    r.frames(2);
    check(status != SEGA_MOD_OK, std::string(c.what) + " answers a non-zero status");
    check(r.sched->fault.faulted != 0U && r.sched->fault.cause != nullptr && std::string(r.sched->fault.cause) == c.cause,
          std::string(c.what) + ": fault cause " + c.cause + ", got " + (r.sched->fault.faulted != 0U ? r.sched->fault.cause : "none"));
    check(r.sched->write_total == 0U && r.rt->work_ram[kSfx] == 0U, std::string(c.what) + ": the faulted callback committed nothing");
  }
  check(std::string(genesis_mod_status_cause(SEGA_MOD_E_AUDIO_TARGET)) == "audio-target" && std::string(genesis_mod_status_cause(SEGA_MOD_E_AUDIO_NAME)) == "audio-name", "the two audio causes are stable");
}

void events_and_render_copy() {
  const uint32_t phases = bit(SEGA_MOD_PHASE_FRAME_END) | bit(SEGA_MOD_PHASE_RENDER);
  {
    Rig r;
    const bool attached = r.attach(kRead, 0U, event_rows(), phases);
    check(attached, "events attach: " + r.rejection);
    std::vector<std::string> seen;
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *audio) {
      char name[SEGA_MOD_AUDIO_NAME_BYTES];
      std::memset(name, 'x', sizeof name);
      if (audio == nullptr || host->frame > 2U || audio->read_event("accepted", name) != SEGA_MOD_OK) return;
      seen.push_back(std::string(genesis_mod_phase_name(host->phase)) + ":" + std::to_string(host->frame) + ":" + name);
      /* change what the guest holds AFTER the frame-end read: the render copy is taken after frame-end, so render sees the new value */
      if (host->phase == SEGA_MOD_PHASE_FRAME_END) r.rt->work_ram[kLast] = host->frame == 0U ? 3U : host->frame == 1U ? 9U : 1U;
    };
    r.frames(4);
    const std::vector<std::string> want = {"frame-end:0:", "render:0:blip", "frame-end:1:blip", "render:1:", "frame-end:2:", "render:2:theme"};
    std::string got;
    for (const std::string &x : seen) got += x + " ";
    check(seen == want, "a level read: the FIRST declared name of the value now held, empty when none; render observes the after-frame-end copy: " + got);
    check(r.sched->fault.faulted == 0U && r.sched->write_total == 0U, "event reads mutate nothing and fault nothing");
  }
  {   // audio.event.read grants no work-RAM read: only the declared locations are observed
    Rig r;
    const bool attached = r.attach(kRead, 0U, event_rows(), phases);
    check(attached, "events attach again: " + r.rejection);
    int status = SEGA_MOD_OK;
    r.body = [&](const SegaModHost *host, const SegaModAudioApi *) {
      uint32_t value = 0;
      if (host->phase == SEGA_MOD_PHASE_RENDER) status = host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, kLast, 1U, &value);
    };
    r.frames(2);
    check(status == SEGA_MOD_E_CAPABILITY && r.sched->fault.faulted != 0U, "a render work-RAM read under audio.event.read alone is a capability fault");
  }
}

// Commands at function hooks (C11 only): committed from hook-entry and hook-exit; discarded with a DECLINED replacement.
void hook_commands() {
  const auto run_hook = [](uint32_t mode, int provider_result, uint8_t &after_entry, uint8_t &after_exit, bool &faulted) {
    GenesisHookSiteInfo site{};
    site.id = "h";
    site.entry = 0x200U;
    site.mode = mode;
    const GenesisModHookBinding binding = {"h", 1U};
    Rig r;
    r.hooks = &binding;
    const bool attached = r.attach(kSend, 0U, command_rows(), bit(SEGA_MOD_PHASE_HOOK_ENTRY) | bit(SEGA_MOD_PHASE_HOOK_EXIT));
    check(attached, "a hooked audio mod attaches: " + r.rejection);
    if (!attached) return;
    g_hook_result = provider_result;
    r.frames(1); /* starts the scheduler */
    (void)r.rt->function_hooks->enter(r.rt->function_hooks->ctx, r.rt.get(), &site, 0xFFFE00U);
    after_entry = r.rt->work_ram[kSfx];
    r.rt->work_ram[kSfx] = 0U;
    r.rt->function_hooks->leave(r.rt->function_hooks->ctx, r.rt.get(), &site, 0xFFFE00U);
    after_exit = r.rt->work_ram[kSfx];
    faulted = r.sched->fault.faulted != 0U;
  };
  uint8_t entry = 0xEE, exit_value = 0xEE;
  bool faulted = true;
  run_hook(GENESIS_HOOK_MODE_BEFORE | GENESIS_HOOK_MODE_AFTER, 0, entry, exit_value, faulted);
  check(!faulted && entry == 3U && exit_value == 4U, "commands commit from hook-entry and hook-exit");
  run_hook(GENESIS_HOOK_MODE_REPLACE, 0, entry, exit_value, faulted);
  check(!faulted && entry == 0U, "a declined replacement discards its command");
}

// Exact-frame render: events come from the producer's previous-onset copy, and only under frame.work_ram.
void exact_frame_events() {
  for (const bool with_ram : {true, false}) {
    Rig r;
    SegaGenesisVdpHelpers helpers{};
    helpers.size = sizeof helpers;
    r.helpers = &helpers;
    const uint64_t caps = kRead | cap(SEGA_MOD_CAP_FRAME_VDP) | (with_ram ? cap(SEGA_MOD_CAP_FRAME_WORK_RAM) : 0U);
    const bool attached = r.attach(caps, 0U, event_rows(), bit(SEGA_MOD_PHASE_RENDER));
    check(attached, "an exact-frame audio provider attaches: " + r.rejection);
    std::string seen = "(none)";
    int status = -1;
    r.body = [&](const SegaModHost *, const SegaModAudioApi *audio) {
      char name[SEGA_MOD_AUDIO_NAME_BYTES];
      status = audio->read_event("accepted", name);
      seen = name;
    };
    r.frames(1);
    std::vector<uint8_t> ram(65536U, 0U), vram(65536U, 0U);
    ram[kLast] = 3U;
    GenesisModExactFrame f{};
    f.work_ram = with_ram ? ram.data() : nullptr;
    f.vram = vram.data();
    f.width = 320U;
    f.height = 224U;
    (void)genesis_mod_scheduler_present(r.sched.get(), &f);
    if (with_ram) check(status == SEGA_MOD_OK && seen == "blip", "the event of the frame's own copy: " + seen);
    else check(status == SEGA_MOD_E_CAPABILITY && r.sched->fault.faulted != 0U, "without frame.work_ram an exact-frame render read is a capability fault, never a silent \"no event\"");
  }
}

// The declared interface is part of the save-state compatibility digest.
void state_compatibility() {
  const auto digest = [](const std::vector<GenesisModAudioEntry> &rows) {
    Rig r;
    const bool attached = r.attach(kSend, 0U, rows);
    check(attached, "attaches: " + r.rejection);
    GenesisSnapshotModState state{};
    genesis_mod_snapshot_state(r.sched.get(), &state);
    return std::string(reinterpret_cast<const char *>(state.compat), sizeof state.compat);
  };
  std::vector<GenesisModAudioEntry> moved = command_rows(), revalued = command_rows();
  moved[0].offset += 8U;
  moved[1].offset += 8U;
  revalued[0].value = 9U;
  check(digest(command_rows()) == digest(command_rows()), "the same interface, the same digest");
  check(digest(command_rows()) != digest(moved) && digest(command_rows()) != digest(revalued), "a moved mailbox or another value is another compatibility digest");
}

}  // namespace

int main() {
  attach_validation();
  commands_and_causes();
  events_and_render_copy();
  hook_commands();
  exact_frame_events();
  state_compatibility();
  if (failures != 0) {
    std::printf("genesis_mod_audio_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_mod_audio_tests: ok\n");
  return 0;
}
