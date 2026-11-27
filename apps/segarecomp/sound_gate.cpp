// See sound_gate.hpp. The evidence is read with the repository's strict closed-JSON reader (libs/media): the WHOLE document must be one well-formed value, a repeated key anywhere is an error, and every required
// member is read from the object that must contain it, never by searching the text.
#include "sound_gate.hpp"

#include "patch_archive.hpp"

#include <algorithm>

namespace segarecomp_cli::sound_gate {

namespace {

namespace json = segarecomp::patch::detail;
using Value = json::Value;

// One of this product's own documents. The bounds are far above what the build and the runtime write; exceeding one is "no evidence".
std::optional<Value> document(std::string_view text) {
  json::JsonLimits limits;
  limits.max_items = 1024;
  limits.max_members = 256;
  limits.max_string = 4096;
  limits.max_depth = 8;
  limits.max_number = 9999999999ULL;
  limits.allow_booleans = true;
  Value root;
  std::string error;
  if (text.size() > (std::size_t{8} << 20U) || !json::parse_json(text, limits, root, error) || root.kind != Value::Kind::object) return std::nullopt;
  return root;
}

// The member `key` of THIS object, only if it has the expected kind.
const Value *member(const Value &object, const char *key, Value::Kind kind) {
  const Value *v = object.find(key);
  return v != nullptr && v->kind == kind ? v : nullptr;
}

// A non-empty token string of [a-z0-9_], at most 64 bytes.
std::optional<std::string> token(const Value &object, const char *key) {
  const Value *v = member(object, key, Value::Kind::string);
  if (v == nullptr || v->text.empty() || v->text.size() > 64 ||
      !std::all_of(v->text.begin(), v->text.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
    return std::nullopt;
  return v->text;
}

bool working(const std::string &status) { return status == "supported" || status == "native"; }

}  // namespace

std::optional<BuildSound> parse_build_sound(std::string_view status_json) {
  const auto root = document(status_json);
  if (!root) return std::nullopt;
  const auto status = token(*root, "genesis_audio");
  const Value *implementation = member(*root, "audio_implementation", Value::Kind::object);
  if (!status || implementation == nullptr) return std::nullopt;
  const auto kind = token(*implementation, "kind");
  if ((*status != "supported" && *status != "native" && *status != "degraded") || !kind || (*kind != "original_z80_aot" && *kind != "native_driver")) return std::nullopt;
  return BuildSound{*status, *kind, token(*root, "z80_audio_outcome").value_or(std::string())};
}

std::optional<RunSound> parse_run_sound(std::string_view summary_json) {
  const auto root = document(summary_json);
  if (!root) return std::nullopt;
  const Value *sound_fault = member(*root, "sound_fault", Value::Kind::number), *audio = member(*root, "audio", Value::Kind::object), *native = member(*root, "native", Value::Kind::object);
  if (sound_fault == nullptr || audio == nullptr || native == nullptr) return std::nullopt;
  const Value *mixer_fault = member(*audio, "fault", Value::Kind::number), *pcm = member(*audio, "sha256", Value::Kind::string);
  const auto native_fault = token(*native, "fault");
  if (mixer_fault == nullptr || pcm == nullptr || !json::is_hex64(pcm->text) || !native_fault) return std::nullopt;
  return RunSound{sound_fault->number, mixer_fault->number, *native_fault, pcm->text};
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
