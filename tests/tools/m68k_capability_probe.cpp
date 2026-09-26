// SEG-021-T002: per-primary-word pipeline capability probe. Calls only public pipeline entry points
// (decoder, lifter, effect owner, timing owner, C emitter, immutable-ROM AOT admission predicate and the
// CPU-owned static discovery walker). It holds no instruction-legality knowledge: the legal-form list is
// consumed by tools/m68k_capability_coverage.py, which asks this probe about all 65,536 primary words
// under one fixed, documented extension-word pattern.
//
// stdout: one line per primary word:
//   XXXX decode lift effects ea_footprint ccr_declared timing emit_direct emit_routed aot static exception_vector
//        word_class
// (each field 0/1 except exception_vector and word_class; SEG-021-T019: word_class is the production
// generation-time classification `m68k_classify_primary_word` -- 0 legal, 1 line 1010, 2 line 1111, 3 illegal -- so
// the driver can compare it with the independent legal-form partition). With --emit-dir, direct-route C for every emitting word is
// written as batched translation units chunk_NNN.c, and Genesis runtime-routed C as rchunk_NNN.c (bounded
// functions per unit), for compilation/execution by the driver.
#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/cpu/m68k/static_discovery.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace segarecomp;

namespace {

constexpr std::uint32_t kBase = 0x00002000U;
constexpr std::size_t kFunctionsPerChunk = 1500U;

// Fixed extension pattern: every extension word is 0x0004 (a well-formed brief index word, a small
// displacement, an even absolute address inside the 1 MiB conformance window, a non-empty MOVEM mask).
std::vector<std::uint8_t> make_image(std::uint16_t word) {
  std::vector<std::uint8_t> image;
  image.push_back(static_cast<std::uint8_t>(word >> 8));
  image.push_back(static_cast<std::uint8_t>(word & 0xFFU));
  for (unsigned i = 0; i < 4U; ++i) {
    const std::uint16_t ext = 0x0004U;
    image.push_back(static_cast<std::uint8_t>(ext >> 8));
    image.push_back(static_cast<std::uint8_t>(ext & 0xFFU));
  }
  return image;
}

class FlatEnvironment final : public M68kStaticDiscoveryEnvironment {
 public:
  FlatEnvironment(std::vector<std::uint8_t> image, std::uint32_t tail) : image_(std::move(image)), tail_(tail) {}
  M68kInstructionSourceResult instruction_source(M68kProgramAddress pc) override {
    if (pc.value < kBase || pc.value >= kBase + image_.size()) {
      M68kInstructionSourceIssue issue{};
      issue.kind = M68kInstructionSourceIssueKind::unmapped;
      issue.address = pc;
      return issue;
    }
    const auto local = static_cast<std::size_t>(pc.value - kBase);
    const DecodeSource source{CpuVariant::mc68000, pc, {local}};
    return M68kInstructionSource{std::span<const std::uint8_t>(image_), source, {local}};
  }
  std::optional<M68kMappingIssue> admit_target(M68kProgramAddress, M68kDiscoveryTargetRole) override {
    return std::nullopt;
  }
  std::optional<DirectFlowDiagnostic> classify_memory_access(const M68kCpuMemoryAccessRequest &) override {
    return std::nullopt;
  }
  // The RTS tail after the probed instruction terminates discovery cleanly; it is not part of the probed form.
  bool is_completion_rts(const InstructionProvenance &rts) override { return rts.source.address.value >= tail_; }
  std::optional<M68kImmutableCartridgeBytes> read_immutable_cartridge_bytes(M68kProgramAddress,
                                                                            std::uint32_t) override {
    return std::nullopt;
  }

 private:
  std::vector<std::uint8_t> image_;
  std::uint32_t tail_;
};

std::string hex4(unsigned value) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
  return out.str();
}

} // namespace

namespace {

// One batched-translation-unit writer; `prefix` is chunk (direct) or rchunk (runtime-routed).
struct ChunkWriter {
  std::string dir, prefix, header, fn_type, touch;
  std::vector<std::string> functions, names;
  std::size_t index = 0;
  void add(const std::string &name, std::string function) {
    names.push_back(name);
    functions.push_back(std::move(function));
    if (functions.size() >= kFunctionsPerChunk) flush();
  }
  void flush() {
    if (dir.empty() || functions.empty()) return;
    std::ostringstream digits;
    digits << std::setw(3) << std::setfill('0') << index++;
    const auto suffix = digits.str();
    std::ofstream out(dir + "/" + prefix + "_" + suffix + ".c");
    out << header;
    for (const auto &fn : functions) out << fn;
    out << "int cap_touch_" << suffix << "(void) { return " << touch << "; }\n";
    out << "typedef " << fn_type << " (*cap_fn)(" << (prefix == "chunk" ? "cap_state" : "GenesisRuntime") << " *);\n"
        << "const cap_fn cap_table_" << suffix << "[] = {";
    for (const auto &n : names) out << n << ",";
    out << "};\nconst unsigned short cap_words_" << suffix << "[] = {";
    for (const auto &n : names) out << "0x" << n.substr(n.size() - 4U) << ",";
    out << "};\nconst unsigned cap_count_" << suffix << " = " << names.size() << ";\n";
    functions.clear();
    names.clear();
  }
};

} // namespace

int main(int argc, char **argv) {
  std::string emit_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--emit-dir" && i + 1 < argc) emit_dir = argv[++i];
    else { std::cerr << "usage: m68k_capability_probe [--emit-dir DIR]\n"; return 2; }
  }
  ChunkWriter direct{emit_dir, "chunk",
      "#include <stdint.h>\n#include <stddef.h>\n"
      "typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;\n"
      "extern uint32_t frame_ids[64]; extern uint32_t frame_continuations[64]; extern uint32_t frame_depth; /* owned and reset per word by the runner */\n",
      "int", "(int)(frame_ids[0] + frame_continuations[0] + frame_depth)", {}, {}, 0};
  ChunkWriter routed_writer{emit_dir, "rchunk",
      "#include \"runtime.h\"\n"
      "extern uint32_t frame_ids[64]; extern uint32_t frame_continuations[64]; extern uint32_t frame_depth; /* owned and reset per word by the runner */\n"
      "/* The generated-program prelude helpers the routed lowering calls (mirrors the emitted definitions). */\n"
      "static GenesisControlTransfer genesis_static_stop(GenesisStopClass c, GenesisDiagnosticCategory d, const GenesisInstructionProvenance *p, uint8_t h, uint32_t a, GenesisAccessWidth w, GenesisAccessDirection x) { GenesisControlTransfer t = {0}; t.kind = GENESIS_STOP; t.stop.stop_class = c; t.stop.diagnostic_category = d; t.stop.provenance.has_instruction_provenance = 1U; t.stop.provenance.instruction = *p; t.stop.provenance.has_access = h; t.stop.provenance.access_address = a; t.stop.provenance.access_width = w; t.stop.provenance.access_direction = x; return t; }\n"
      "static void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *p) { (void)stop; (void)p; }\n"
      "#define pc runtime->pc\n",
      "GenesisControlTransfer", "((void)genesis_static_stop, (void)genesis_attach_route_provenance, (int)(frame_ids[0] + frame_continuations[0] + frame_depth))", {}, {}, 0};

  for (unsigned word = 0; word < 0x10000U; ++word) {
    const auto image = make_image(static_cast<std::uint16_t>(word));
    const M68kProgramAddress pc{TargetAddressSpace::m68k_program, kBase};
    const DecodeSource source{CpuVariant::mc68000, pc, {0U}};
    const auto decoded_result =
        decode_m68k_instruction(std::span<const std::uint8_t>(image), source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&decoded_result);
    bool decode = decoded != nullptr, lift = false, effects = false, footprint = false, ccr = false, timing = false;
    unsigned exception_vector = 0U;
    bool emit_direct = false, emit_routed = false, aot = false, statik = false;
    if (decode) {
      const auto operation = lift_m68k_instruction(*decoded);
      lift = decoded->kind == M68kInstructionKind::moveq || operation.kind != M68kIrKind::write_moveq;
      const auto effect = m68k_operation_effect(operation);
      effects = lift && effect.pc != M68kPcEffectKind::none;
      footprint = lift && effect.register_write_footprint_complete;
      ccr = lift && effect.affects_condition_codes;
      exception_vector = lift && effect.may_raise_synchronous_exception ? effect.exception_vector : 0U;
      timing = lift && m68k_instruction_cycles(operation).has_value();
      emit_direct = lift && m68k_operation_has_complete_c_emission(operation);
      if (lift) {
        M68kMemoryEmissionContext routed{"runtime->work_ram", "runtime->a", "frame_ids", "frame_continuations",
                                         "frame_depth", 0U, M68kOperandAccess::runtime_routed, 0U, {}, {}};
        routed.user_stack_pointer = "runtime->usp";
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        routed.linear_memory_begin = 0x00FF0000U;
        routed.linear_memory_end = 0x01000000U;
        routed.runtime_emitter = &genesis_m68k_runtime_c_emitter();
        const auto routed_body = emit_m68k_operation_c(operation, "runtime->d", "runtime->sr", "  ", &routed);
        // An indentation-only body is a declined operation (the emitter writes the indent before it knows).
        emit_routed = routed_body.find_first_not_of(" \n") != std::string::npos;
        if (emit_routed && !emit_dir.empty()) {
          const auto name = "rw_" + hex4(word);
          routed_writer.add(name, "static GenesisControlTransfer " + name + "(GenesisRuntime *runtime) {\n" + routed_body +
              "  { GenesisControlTransfer done = {0}; done.kind = GENESIS_CONTINUE_AT_PC; done.next_pc = pc; return done; }\n}\n");
        }
        aot = m68k_operation_is_immutable_rom_aot_safe(operation, true);
        const auto length = static_cast<std::size_t>(operation.provenance.length.value);
        auto flat = std::vector<std::uint8_t>(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(
            decoded->raw_bytes.empty() ? length : decoded->raw_bytes.size()));
        const auto tail = static_cast<std::uint32_t>(kBase + flat.size());
        for (unsigned i = 0; i < 512U; ++i) { flat.push_back(0x4EU); flat.push_back(0x75U); }
        FlatEnvironment environment(std::move(flat), tail);
        const M68kStaticDiscoveryLimits limits{64U, 64U, 2U};
        const auto graph = discover_m68k_static_graph(pc, limits, environment);
        statik = !graph.decode_order.empty() && !graph.primary_issue.has_value();
      }
      if (emit_direct && !emit_dir.empty()) {
        M68kMemoryEmissionContext memory{"s->ram", "s->a", "frame_ids", "frame_continuations", "frame_depth", 0U,
                                         M68kOperandAccess::linear_memory, 0U, {}, {}};
        memory.linear_memory_begin = 0U;
        memory.linear_memory_end = 0x100000U;
        memory.user_stack_pointer = "s->usp";
        const auto body = emit_m68k_operation_c(operation, "s->d", "s->sr", "  ", &memory);
        const auto name = "w_" + hex4(word);
        direct.add(name, "static int " + name + "(cap_state *s) {\n  uint32_t pc = s->pc;\n" + body +
                             "  s->pc = pc;\n  return 0;\n}\n");
      }
    }
    const auto word_class = m68k_classify_primary_word(static_cast<std::uint16_t>(word));
    const unsigned word_class_code = word_class == M68kPrimaryWordClass::legal             ? 0U
                                     : word_class == M68kPrimaryWordClass::line_a_emulator ? 1U
                                     : word_class == M68kPrimaryWordClass::line_f_emulator ? 2U
                                                                                           : 3U;
    std::printf("%s %d %d %d %d %d %d %d %d %d %d %u %u\n", hex4(word).c_str(), decode, lift, effects, footprint, ccr,
                timing, emit_direct, emit_routed, aot, statik, exception_vector, word_class_code);
  }
  direct.flush();
  routed_writer.flush();
  return 0;
}
