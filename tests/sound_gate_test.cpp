// SEG-145-T003 (ADR 0323): the evidence of `mod diff-run --audio` fails closed. The readers and verdicts here are the ones the command calls (apps/segarecomp/sound_gate.cpp): starting from the shapes the
// build and the runtime write, every required field is removed or corrupted in turn, and for Vanilla, Modded and the repeated Modded run none of those may yield a passing verdict.
#include "sound_gate.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace gate = segarecomp_cli::sound_gate;

int failures = 0;
void check(bool value, const std::string &message) {
  if (!value) {
    std::printf("FAIL: %s\n", message.c_str());
    ++failures;
  }
}

const std::string kPcm(64, 'a');
const std::string kStatus =
    "{\"status\":\"ok\",\"stage\":\"done\",\"message\":\"\",\"rom_sha256\":\"x\",\"genesis_audio\":\"supported\",\"audio_implementation\":{\"schema\":2,\"requested\":\"auto\",\"kind\":\"original_z80_aot\","
    "\"selection\":\"automatic\",\"audio_status\":\"supported\",\"structural_alternatives\":{\"requested\":false}},\"z80\":{\"outcome\":\"converged\"}}";
const std::string kSummary =
    "{\"epochs\":1,\"bound_image\":1,\"sound_fault\":0,\"sound_fault_epoch\":0,\"psg_writes\":4,\"classes\":{\"reset_assert\":1},\"audio\":{\"rate_hz\":44100,\"sha256\":\"" + kPcm +
    "\",\"range_sha256\":\"" + std::string(64, 'b') + "\",\"fault\":0,\"pcm_file_errors\":0},\"native\":{\"bound\":0,\"driver\":\"\",\"version\":\"\",\"fault\":\"none\",\"activations\":0}}";

// `from` must occur in `text`: a mutation that changes nothing would make its case vacuous.
std::string mutate(const std::string &text, const std::string &from, const std::string &to) {
  const std::size_t at = text.find(from);
  check(at != std::string::npos, "the mutation applies: " + from);
  if (at == std::string::npos) return text;
  return text.substr(0, at) + to + text.substr(at + from.size());
}

void builds() {
  const auto good = gate::parse_build_sound(kStatus);
  check(good && good->status == "supported" && good->implementation == "original_z80_aot", "the build's own status shape is read");
  check(!gate::judge_builds(good, good), "two working builds with the same implementation pass");
  const std::string kind = "\"kind\":\"original_z80_aot\",";
  const std::vector<std::pair<std::string, std::string>> bad = {
      {"no sound status", mutate(kStatus, "\"genesis_audio\":\"supported\",", "")},
      {"an empty sound status", mutate(kStatus, "\"genesis_audio\":\"supported\"", "\"genesis_audio\":\"\"")},
      {"an unknown sound status", mutate(kStatus, "\"genesis_audio\":\"supported\"", "\"genesis_audio\":\"fine\"")},
      {"a numeric sound status", mutate(kStatus, "\"genesis_audio\":\"supported\"", "\"genesis_audio\":1")},
      {"no implementation block", mutate(kStatus, "\"audio_implementation\":{", "\"other\":{")},
      {"no implementation kind", mutate(kStatus, kind, "")},
      {"an empty implementation kind", mutate(kStatus, kind, "\"kind\":\"\",")},
      {"an unknown implementation kind", mutate(kStatus, kind, "\"kind\":\"something\",")},
      {"an unterminated implementation kind", mutate(kStatus, kind, "\"kind\":\"original_z80_aot,")},
      {"a kind outside the implementation block", mutate(mutate(kStatus, kind, ""), "\"z80\":{", "\"z80\":{\"kind\":\"original_z80_aot\",")},
      {"an empty file", ""}};
  for (const auto &b : bad) {
    const auto parsed = gate::parse_build_sound(b.second);
    check(!parsed, b.first + ": not evidence");
    for (const auto &verdict : {gate::judge_builds(parsed, good), gate::judge_builds(good, parsed), gate::judge_builds(parsed, parsed)})   // both missing must not compare equal
      check(verdict && verdict->code == "E_RUN_FAILED", b.first + ": E_RUN_FAILED on either side, and on both");
  }
  auto degraded = good, native = good;
  degraded->status = "degraded";
  native->status = "native";
  native->implementation = "native_driver";
  check(gate::judge_builds(degraded, good)->code == "E_AUDIO_BASELINE", "a degraded Vanilla is E_AUDIO_BASELINE");
  check(gate::judge_builds(good, degraded)->code == "E_AUDIO_STATIC_MATERIALIZATION", "a degraded Modded is E_AUDIO_STATIC_MATERIALIZATION");
  check(gate::judge_builds(good, native)->code == "E_AUDIO_IMPLEMENTATION_CHANGED", "another implementation is E_AUDIO_IMPLEMENTATION_CHANGED");
  check(!gate::judge_builds(native, native), "the same native replacement on both sides compares");
}

void runs() {
  const auto good = gate::parse_run_sound(kSummary);
  check(good && good->sound_fault == 0 && good->mixer_fault == 0 && good->native_fault == "none" && good->pcm_sha256 == kPcm, "the runtime's own summary shape is read");
  const std::vector<std::pair<std::string, std::string>> bad = {
      {"no sound fault", mutate(kSummary, "\"sound_fault\":0,", "")},
      {"an empty sound fault", mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":,")},
      {"a textual sound fault", mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":\"none\",")},
      {"a sound fault with trailing text", mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":0x,")},
      {"a negative sound fault", mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":-1,")},
      {"an overlong sound fault", mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":99999999999999999999,")},
      {"no mixer fault", mutate(kSummary, "\"fault\":0,", "")},
      {"a textual mixer fault", mutate(kSummary, "\"fault\":0,", "\"fault\":\"x\",")},
      {"an empty mixer fault", mutate(kSummary, "\"fault\":0,", "\"fault\":,")},
      {"no native fault", mutate(kSummary, "\"fault\":\"none\",", "")},
      {"an empty native fault", mutate(kSummary, "\"fault\":\"none\"", "\"fault\":\"\"")},
      {"a numeric native fault", mutate(kSummary, "\"fault\":\"none\"", "\"fault\":0")},
      {"an unterminated native fault", mutate(kSummary, "\"fault\":\"none\",", "\"fault\":\"none,")},
      {"no native object", mutate(kSummary, "\"native\":{", "\"other\":{")},
      {"no PCM digest", mutate(kSummary, "\"sha256\":\"" + kPcm + "\",", "")},
      {"a non-hexadecimal PCM digest of the right length", mutate(kSummary, kPcm, std::string(64, 'g'))},
      {"an upper-case PCM digest", mutate(kSummary, kPcm, std::string(64, 'A'))},
      {"a short PCM digest", mutate(kSummary, kPcm, std::string(63, 'a'))},
      {"a cut line", kSummary.substr(0, kSummary.size() - 1)},
      {"no summary", ""}};
  for (const auto &b : bad) {
    const auto parsed = gate::parse_run_sound(b.second);
    check(!parsed, b.first + ": not evidence");
    for (const bool vanilla : {true, false}) {
      const auto verdict = gate::judge_run(parsed, vanilla, "run");
      check(verdict && verdict->code == "E_RUN_FAILED", b.first + ": E_RUN_FAILED for " + (vanilla ? "Vanilla" : "Modded and the repeat"));
    }
  }
  check(!gate::judge_run(good, true, "vanilla") && !gate::judge_run(good, false, "modded") && !gate::judge_run(good, false, "modded (repeat)"), "complete fault-free evidence passes every run");
  const auto code = [](const std::string &summary, bool vanilla) {
    const auto verdict = gate::judge_run(gate::parse_run_sound(summary), vanilla, "run");
    return verdict ? verdict->code : std::string("pass");
  };
  const std::string mismatch = mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":57,"), other = mutate(kSummary, "\"sound_fault\":0,", "\"sound_fault\":53,"),
                    mixer = mutate(kSummary, "\"fault\":0,", "\"fault\":2,"), native = mutate(kSummary, "\"fault\":\"none\"", "\"fault\":\"unsupported_operation\"");
  check(code(mismatch, false) == "E_AUDIO_STATIC_MATERIALIZATION", "a modded code fault keeps its class");
  check(code(other, false) == "E_AUDIO_RUNTIME_FAULT" && code(mixer, false) == "E_AUDIO_RUNTIME_FAULT" && code(native, false) == "E_AUDIO_RUNTIME_FAULT", "other modded sound faults are E_AUDIO_RUNTIME_FAULT");
  for (const std::string &s : {mismatch, other, mixer, native}) check(code(s, true) == "E_AUDIO_BASELINE", "any Vanilla fault is E_AUDIO_BASELINE");
}

}  // namespace

int main() {
  builds();
  runs();
  if (failures != 0) {
    std::printf("sound_gate_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("sound_gate_test: ok\n");
  return 0;
}
