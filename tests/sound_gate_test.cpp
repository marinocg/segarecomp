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

// The complete documents the CURRENT producers wrote for the synthetic sound fixture (platforms/genesis/sdk/examples/sound), verbatim.
const char *const kProducedStatus = R"JSON({"status":"ok","stage":"done","message":"","rom_sha256":"92c51033a77f6af58de65bff8d6b822c14b5d7b0bdb234f69027267f613fdfee","genesis_audio":"supported","audio_implementation":{"schema":2,"requested":"auto","kind":"original_z80_aot","selection":"automatic","reason":"original_supported","catalog_revision":1,"native_implementation_version":1,"replacement_available":false,"save_states":"supported","audio_status":"supported","structural_alternatives":{"requested":false,"images":0,"bytes":0,"entries":0,"forms":0,"images_dropped":0}},"z80":{"outcome":"converged","end":"guest_stop","observed_region":"usa","images":1,"discovery_runs":2,"runs":3,"epochs":1,"window_frames":600,"frames_reached":90,"units":11,"units_compiled":12,"units_reused":0,"generated_bytes":5413475,"object_bytes":3285752,"emit_ms":160,"compile_ms":812,"materialize_ms":1501,"executable_bytes":2954280,"prepare_generations":2,"verification_runs":1},"m68k_alias_preparation":{"rounds":0,"aliases":0,"alias_bytes":0,"termination":"non_alias_frontier"},"aot_policy":{"requested":"compatibility","requested_model":"none","effective":"broad","fallback":false,"reason":"none","fallback_class":"none","model_family":"none","analysis_contract":"segarecomp.admission_contract.v3","validator":"not_run","policy_identity":"3c0cd59937b2cdf8afe794cf878dcc682858e34a7b5f1d7da5aa50154a1abf74","identity":"3fee4622ef7fd8faeb16323ed0e014fc2a3b1a4f670fd4cfa594065b7a0d0dd9","cache_reuse":"none"},"executable_images":{"m68k":{"images":1,"authority":{"immutable_input":1,"static_proof":0,"bounded_build_time_materialization":0},"producers":{"genesis.cartridge":1}},"z80":{"images":1,"authority":{"immutable_input":0,"static_proof":0,"bounded_build_time_materialization":1},"producers":{"genesis.z80_materializer":1}}}})JSON";
const char *const kProducedSummary = R"JSON({"epochs":1,"bound_image":1,"sound_fault":0,"sound_fault_epoch":0,"psg_writes":18,"ym_writes":0,"virtual_frames":64,"result_kind":3,"stop_class":0,"diagnostic":0,"classes":{"reset_assert":1,"reset_release":2,"busreq_assert":2,"busreq_release":2,"bank_writes":0,"banked_reads":0,"ym_dac":0,"ym_dac_enable":0,"ym_key":0,"ym_timer":0,"ym_global":0,"ym_operator":0,"ym_address":0},"audio":{"rate_hz":44100,"channels":2,"frames":47730,"sha256":"cceadaf9fd441b26c81814224aff6e2261e2b3df2ec7685884f3447bdecdefcc","range_frames":0,"range_sha256":"af5570f5a1810b7af78caf4bc70a660f0df51e42baf91d4de5b2328de0e83dfc","clipped":0,"changes":153,"span":1292,"non_silent":true,"fault":0,"ring_dropped":39538,"pcm_file_errors":0},"native":{"bound":0,"driver":"","version":"","fault":"none","commands":0,"sequences":0,"dac_samples":0,"ticks":0,"missed_ticks":0,"activations":0}})JSON";

bool build_refused(const std::string &text) {
  const auto parsed = gate::parse_build_sound(text), good = gate::parse_build_sound(kStatus);
  for (const auto &verdict : {gate::judge_builds(parsed, good), gate::judge_builds(good, parsed), gate::judge_builds(parsed, parsed)})
    if (!verdict || verdict->code != "E_RUN_FAILED") return false;
  return !parsed;
}
bool run_refused(const std::string &text) {
  const auto parsed = gate::parse_run_sound(text);
  for (const bool vanilla : {true, false}) {
    const auto verdict = gate::judge_run(parsed, vanilla, "run");
    if (!verdict || verdict->code != "E_RUN_FAILED") return false;
  }
  return !parsed;
}

void producer_output() {
  const auto build = gate::parse_build_sound(std::string(kProducedStatus) + "\n");   // status.json ends with a newline
  check(build && build->status == "supported" && build->implementation == "original_z80_aot" && !gate::judge_builds(build, build), "the current build's status.json passes");
  const auto run = gate::parse_run_sound(kProducedSummary);
  check(run && run->pcm_sha256.size() == 64 && !gate::judge_run(run, false, "modded"), "the current runtime's summary passes");
}

// A document cut anywhere is no evidence, even after every required member has been written.
void truncation() {
  check(build_refused("{\"genesis_audio\":\"supported\",\"audio_implementation\":{\"kind\":\"original_z80_aot\"}"), "a status whose outer object never closes");
  const std::string status = kProducedStatus, summary = kProducedSummary;
  std::size_t accepted = 0;
  for (std::size_t n = 0; n < status.size(); ++n) accepted += build_refused(status.substr(0, n)) ? 0U : 1U;
  check(accepted == 0, "every proper prefix of the produced status is refused: " + std::to_string(accepted) + " accepted");
  accepted = 0;
  for (std::size_t n = 0; n < summary.size(); ++n) accepted += run_refused(summary.substr(0, n)) ? 0U : 1U;
  check(accepted == 0, "every proper prefix of the produced summary is refused: " + std::to_string(accepted) + " accepted");
  for (const std::string &tail : {std::string("x"), std::string("}"), std::string(",\"a\":1"), status})
    check(build_refused(status + tail), "trailing text after the status document: " + tail.substr(0, 8));
  check(run_refused(summary + summary), "two summary documents on one line");
}

// A member counts only in the object that must contain it.
void nesting() {
  const std::string kind = "\"kind\":\"original_z80_aot\"", digest = "\"sha256\":\"" + kPcm + "\"";
  const std::string audio = "\"audio\":{" + digest + ",\"fault\":0}", native = "\"native\":{\"fault\":\"none\"}";
  check(gate::parse_run_sound("{\"sound_fault\":0," + audio + "," + native + "}").has_value(), "the minimal complete run evidence is read");
  const std::vector<std::pair<std::string, std::string>> runs_bad = {
      {"sound_fault only nested", "{\"nested\":{\"sound_fault\":0}," + audio + "," + native + "}"},
      {"sound_fault only inside audio", "{\"audio\":{" + digest + ",\"fault\":0,\"sound_fault\":0}," + native + "}"},
      {"the audio object only nested", "{\"sound_fault\":0,\"nested\":{" + audio + "}," + native + "}"},
      {"the native object only nested", "{\"sound_fault\":0," + audio + ",\"nested\":{" + native + "}}"},
      {"the mixer fault only in a child of audio", "{\"sound_fault\":0,\"audio\":{" + digest + ",\"x\":{\"fault\":0}}," + native + "}"},
      {"the mixer fault only at the top level", "{\"sound_fault\":0,\"fault\":0,\"audio\":{" + digest + "}," + native + "}"},
      {"the digest only in a child of audio", "{\"sound_fault\":0,\"audio\":{\"x\":{" + digest + "},\"fault\":0}," + native + "}"},
      {"the digest only at the top level", "{\"sound_fault\":0," + digest + ",\"audio\":{\"fault\":0}," + native + "}"},
      {"the native fault only in a child of native", "{\"sound_fault\":0," + audio + ",\"native\":{\"x\":{\"fault\":\"none\"}}}"},
      {"the native fault only elsewhere", "{\"sound_fault\":0," + audio + ",\"fault\":\"none\",\"native\":{}}"},
      {"audio that is not an object", "{\"sound_fault\":0,\"audio\":\"x\"," + native + "}"},
      {"evidence inside a string", "{\"note\":\"{\\\"sound_fault\\\":0}\"," + audio + "," + native + "}"},
      {"an array instead of an object", "[{\"sound_fault\":0," + audio + "," + native + "}]"}};
  for (const auto &b : runs_bad) check(run_refused(b.second), b.first + ": E_RUN_FAILED");
  const std::string status = "\"genesis_audio\":\"supported\"", impl = "\"audio_implementation\":{" + kind + "}";
  check(gate::parse_build_sound("{" + status + "," + impl + "}").has_value(), "the minimal complete build evidence is read");
  const std::vector<std::pair<std::string, std::string>> builds_bad = {
      {"the sound status only nested", "{\"z80\":{" + status + "}," + impl + "}"},
      {"the implementation block only nested", "{" + status + ",\"z80\":{" + impl + "}}"},
      {"the kind only in a child of the implementation", "{" + status + ",\"audio_implementation\":{\"schema\":2,\"structural_alternatives\":{" + kind + "}}}"},
      {"the kind only at the top level", "{" + status + "," + kind + ",\"audio_implementation\":{\"schema\":2}}"},
      {"the kind only in a sibling object", "{" + status + ",\"audio_implementation\":{},\"z80\":{" + kind + "}}"}};
  for (const auto &b : builds_bad) check(build_refused(b.second), b.first + ": E_RUN_FAILED");
}

// A repeated required member is ambiguous, whether or not the copies agree.
void duplicates() {
  const auto twice = [](const std::string &text, const std::string &member, const std::string &second) { return mutate(text, member, member + second + ","); };
  const std::string kind = "\"kind\":\"original_z80_aot\",";
  for (const std::string &b : {twice(kStatus, "\"genesis_audio\":\"supported\",", "\"genesis_audio\":\"degraded\""), twice(kStatus, "\"genesis_audio\":\"supported\",", "\"genesis_audio\":\"supported\""),
                               twice(kStatus, kind, "\"kind\":\"native_driver\""), mutate(kStatus, "\"z80\":{", "\"audio_implementation\":{" + kind + "\"x\":1},\"z80\":{")})
    check(build_refused(b), "a repeated build member is E_RUN_FAILED");
  for (const std::string &b : {twice(kSummary, "\"sound_fault\":0,", "\"sound_fault\":57"), twice(kSummary, "\"sound_fault\":0,", "\"sound_fault\":0"), twice(kSummary, "\"fault\":0,", "\"fault\":2"),
                               twice(kSummary, "\"sha256\":\"" + kPcm + "\",", "\"sha256\":\"" + std::string(64, 'c') + "\""), twice(kSummary, "\"fault\":\"none\",", "\"fault\":\"x\""),
                               mutate(kSummary, "\"native\":{", "\"audio\":{\"fault\":0},\"native\":{"), mutate(kSummary, "\"epochs\":1,", "\"native\":{\"fault\":\"none\"},\"epochs\":1,")})
    check(run_refused(b), "a repeated run member is E_RUN_FAILED");
}

}  // namespace

int main() {
  builds();
  runs();
  producer_output();
  truncation();
  nesting();
  duplicates();
  if (failures != 0) {
    std::printf("sound_gate_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("sound_gate_test: ok\n");
  return 0;
}
