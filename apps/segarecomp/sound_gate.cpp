// See sound_gate.hpp. The readers are deliberately narrow: they read the exact flat members this repository writes and accept nothing else.
#include "sound_gate.hpp"

#include <algorithm>

namespace segarecomp_cli::sound_gate {

namespace {

constexpr std::size_t npos = std::string_view::npos;

// The text of the flat object that starts at `"<key>":{` up to its first '}' or nested '{' (the members read here all precede any nested object).
std::optional<std::string_view> object_head(std::string_view json, std::string_view key) {
  const std::string open = "\"" + std::string(key) + "\":{";
  const std::size_t at = json.find(open);
  if (at == npos) return std::nullopt;
  const std::size_t from = at + open.size() - 1, end = json.find_first_of("{}", from + 1);
  if (end == npos) return std::nullopt;
  std::string_view head = json.substr(from, end - from + 1);
  return head;
}

// The position right after `"<key>":` where the key starts a member (after '{' or ',').
std::optional<std::size_t> value_of(std::string_view json, std::string_view key) {
  const std::string k = "\"" + std::string(key) + "\":";
  for (std::size_t at = json.find(k); at != npos; at = json.find(k, at + 1))
    if (at != 0 && (json[at - 1] == '{' || json[at - 1] == ',')) return at + k.size();
  return std::nullopt;
}

// A member is terminated by ',' or '}' (or, for the head of an object cut before a nested one, '{').
bool terminated(std::string_view json, std::size_t at) { return at < json.size() && (json[at] == ',' || json[at] == '}' || json[at] == '{'); }

std::optional<std::uint64_t> number(std::string_view json, std::string_view key) {
  auto at = value_of(json, key);
  if (!at) return std::nullopt;
  std::uint64_t v = 0;
  std::size_t i = *at;
  for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i) {
    if (i - *at >= 18) return std::nullopt;
    v = v * 10 + static_cast<std::uint64_t>(json[i] - '0');
  }
  if (i == *at || !terminated(json, i)) return std::nullopt;
  return v;
}

// A non-empty token string of [a-z0-9_].
std::optional<std::string> token(std::string_view json, std::string_view key) {
  const auto at = value_of(json, key);
  if (!at || *at >= json.size() || json[*at] != '"') return std::nullopt;
  const std::size_t from = *at + 1, end = json.find('"', from);
  if (end == npos || end == from || end - from > 64 || !terminated(json, end + 1)) return std::nullopt;
  const std::string_view text = json.substr(from, end - from);
  if (!std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; })) return std::nullopt;
  return std::string(text);
}

bool hex64(const std::string &s) { return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }

bool working(const std::string &status) { return status == "supported" || status == "native"; }

}  // namespace

std::optional<BuildSound> parse_build_sound(std::string_view json) {
  BuildSound s;
  const auto status = token(json, "genesis_audio");
  const auto head = object_head(json, "audio_implementation");
  if (!status || !head) return std::nullopt;
  const auto kind = token(*head, "kind");
  if ((*status != "supported" && *status != "native" && *status != "degraded") || !kind || (*kind != "original_z80_aot" && *kind != "native_driver")) return std::nullopt;
  s.status = *status;
  s.implementation = *kind;
  s.outcome = token(json, "z80_audio_outcome").value_or(std::string());
  return s;
}

std::optional<RunSound> parse_run_sound(std::string_view json) {
  while (!json.empty() && (json.back() == '\r' || json.back() == ' ')) json.remove_suffix(1);
  if (json.size() < 2 || json.front() != '{' || json.substr(json.size() - 2) != "}}") return std::nullopt;   // a complete line, not a cut one
  const auto audio = object_head(json, "audio"), native = object_head(json, "native");
  if (!audio || !native || audio->back() != '}' || native->back() != '}') return std::nullopt;
  const auto sound_fault = number(json.substr(0, json.find("\"audio\":{")), "sound_fault"), mixer_fault = number(*audio, "fault");
  const auto native_fault = token(*native, "fault"), pcm = token(*audio, "sha256");
  if (!sound_fault || !mixer_fault || !native_fault || !pcm || !hex64(*pcm)) return std::nullopt;
  return RunSound{*sound_fault, *mixer_fault, *native_fault, *pcm};
}

std::optional<Refusal> judge_builds(const std::optional<BuildSound> &vanilla, const std::optional<BuildSound> &modded) {
  if (!vanilla || !modded)
    return Refusal{"E_RUN_FAILED", std::string(vanilla ? "modded" : "vanilla") + ": the build's sound status or audio implementation is missing or malformed: there is no evidence to verify"};
  if (!working(vanilla->status))
    return Refusal{"E_AUDIO_BASELINE", "the Vanilla program's sound is \"" + vanilla->status + "\": there is no working original to verify a sound mod against"};
  if (!working(modded->status))
    return Refusal{"E_AUDIO_STATIC_MATERIALIZATION", "the modified sound code or image does not pass the static materialization rules: the modded program's sound is \"" + modded->status + "\"" +
                                                         (modded->outcome.empty() ? std::string() : " (" + modded->outcome + ")") + "; modified Z80 code is never interpreted"};
  if (modded->status != vanilla->status || modded->implementation != vanilla->implementation)
    return Refusal{"E_AUDIO_IMPLEMENTATION_CHANGED", "the modded program selects another sound implementation (" + modded->status + "/" + modded->implementation + ") than Vanilla (" + vanilla->status + "/" +
                                                         vanilla->implementation + "), so their audio is not comparable"};
  return std::nullopt;
}

// The build's observation takes no pad input and no mod command, so a driver that breaks only on a path a compared run reaches (an effect, a sample, a command) is `supported` at build time and isolated at run time.
std::optional<Refusal> judge_run(const std::optional<RunSound> &run, bool vanilla, std::string_view which) {
  constexpr std::uint64_t kFirstCodeFault = 56, kLastCodeFault = 60;   // unknown image, code mismatch, no owner, mutable code, unresolved fetch mapping (runtime.h GenesisDiagnosticCategory)
  const std::string name(which);
  if (!run) return Refusal{"E_RUN_FAILED", name + ": the run's sound evidence (sound, mixer and native-replacement fault state, PCM digest) is missing or malformed"};
  const bool native = run->native_fault != "none";
  if (run->sound_fault == 0 && run->mixer_fault == 0 && !native) return std::nullopt;
  const std::string detail = "sound_fault=" + std::to_string(run->sound_fault) + " mixer_fault=" + std::to_string(run->mixer_fault) + " native_fault=" + run->native_fault;
  if (vanilla) return Refusal{"E_AUDIO_BASELINE", name + ": the original program's sound faults during this run (" + detail + "): there is no working original to verify a sound mod against"};
  if (run->sound_fault >= kFirstCodeFault && run->sound_fault <= kLastCodeFault)
    return Refusal{"E_AUDIO_STATIC_MATERIALIZATION", name + ": the modified sound code does not pass the static materialization rules: the sound CPU was isolated during this run (" + detail + "); modified Z80 code is never interpreted"};
  return Refusal{"E_AUDIO_RUNTIME_FAULT", name + ": the modded program's sound faulted during this run (" + detail + ")"};
}

}  // namespace segarecomp_cli::sound_gate
