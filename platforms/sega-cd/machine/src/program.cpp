#include "segarecomp/machine/sega_cd/program.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/codegen/c11/source_relative.hpp"
#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/sha256.hpp"

#include "emit_shared.hpp"

namespace segarecomp::machine::sega_cd {
namespace {

using detail::hex;
using detail::SegaCdM68kRuntimeCEmitter;

struct Reached {
  std::uint32_t pc{};
  std::optional<M68kDecodedInstruction> decoded;
  std::optional<M68kIrOperation> operation;
};

struct Traversal {
  std::map<std::uint32_t, Reached> instructions;  // ordered: deterministic emission
  std::uint32_t out_of_image{};
  std::uint32_t vector_roots{};
};

// One compilation unit: one mapping of one image of a processor's set (ADR 0132). The same bytes at two bases are two units (their
// compiled code differs in every absolute PC); two images at one PC are two units of one overlay group.
struct Unit {
  std::uint32_t image_index{};
  std::uint32_t mapping_index{};
  std::uint32_t start{};  // execution address of the mapping's first byte
  std::uint32_t end{};    // one past the last
  std::vector<std::uint32_t> roots;
  std::string prefix;     // generated-symbol prefix, unique per processor
  Traversal traversal;
};

Traversal traverse(const ProcessorImages &processor, const Unit &unit) {
  Traversal out;
  const auto &image = processor.images.images[unit.image_index];
  const auto &mapping = image.mappings[unit.mapping_index];
  const auto bytes = executable_image_bytes(processor.images, image);
  const std::uint32_t base = unit.start;
  const std::uint32_t end = unit.end;
  std::deque<std::uint32_t> work(unit.roots.begin(), unit.roots.end());
  std::set<std::uint32_t> seen;
  std::set<std::uint32_t> outside;
  while (!work.empty()) {
    const std::uint32_t pc = work.front();
    work.pop_front();
    if (!seen.insert(pc).second) continue;
    if (pc < base || pc >= end || (pc & 1U) != 0U) {
      outside.insert(pc);
      continue;
    }
    Reached reached;
    reached.pc = pc;
    const std::uint32_t offset = pc - base + mapping.first_offset;
    // The decoder sees only this mapping's bytes: an instruction may not straddle the end of the mapped range.
    const auto visible = bytes.subspan(0, static_cast<std::size_t>(mapping.first_offset) + mapping.length);
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(visible, source, M68kDecodeProfile::general_startup);
    if (const auto *decoded = std::get_if<M68kDecodedInstruction>(&result)) {
      auto operation = lift_m68k_instruction(*decoded);
      const bool lifted = !(decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq);
      if (lifted) {
        const auto successors = m68k_control_successors(operation);
        for (const auto &successor : successors.successors) work.push_back(successor.target);
        if (successors.stacked == M68kStackedContinuationKind::call_continuation ||
            successors.stacked == M68kStackedContinuationKind::exception_continuation)
          work.push_back(successors.stacked_address);
        // Static proof of an installed handler: MOVE.L #handler,(vector).W/L into the sub processor's vector table (PRG RAM
        // $000008-$0003FF is ordinary RAM holding the sub's vectors, contract section 4). The handler becomes a root.
        if (processor.processor == Processor::sub && operation.kind == M68kIrKind::write_move &&
            operation.size == M68kMemoryAccessWidth::long_word && operation.source_ea.mode == M68kEaMode::immediate &&
            (operation.destination_ea.mode == M68kEaMode::absolute_long || operation.destination_ea.mode == M68kEaMode::absolute_word)) {
          const std::uint32_t slot = operation.destination_ea.absolute_address & 0x00FFFFFFU;
          if (slot >= 8U && slot < 0x400U && (slot & 3U) == 0U) {
            work.push_back(operation.source_ea.immediate_value);
            ++out.vector_roots;
          }
        }
        reached.decoded = *decoded;
        reached.operation = std::move(operation);
      }
    }
    out.instructions.emplace(pc, std::move(reached));
  }
  out.out_of_image = static_cast<std::uint32_t>(outside.size());
  return out;
}

bool compiled_bytes(const Reached &reached) { return reached.operation.has_value(); }

// ---- SEG-096-T002 (ADR 0207): source-relative bodies -------------------------------------------------------------------------------
// One body per (image, source offset) of an image with several mappings. The body is the CPU lowering of the instruction, lowered at three
// distinct sentinel PCs and reduced to a single text in which each PC-derived literal is `scd_pc0 + constant` (libs/codegen/c11/
// source_relative.hpp); it runs under the actual guest PC the dispatcher selected and verifies the live bytes at that PC like every body.
struct SharedBody {
  std::string name;
  std::vector<std::uint8_t> raw;
  detail::LoweredInstruction lowered;
  std::uint32_t instances{};
};

struct PiPlan {
  std::map<std::pair<std::uint32_t, std::uint32_t>, SharedBody> shared;    // (image index, source offset)
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> refused;  // (image index, source offset) -> reason
};

std::optional<SharedBody> derive_shared_body(const Reached &reached, std::uint32_t offset, const SegaCdM68kRuntimeCEmitter &emitter,
                                             const std::string &lookup_symbol, const std::string &name, std::string &reason) {
  constexpr std::array<std::uint32_t, 3> kSentinel{0x00200000U, 0x00300000U, 0x00480000U};
  const auto &raw = reached.decoded->raw_bytes;
  // The decoder reads the instruction at the image offset it is given: a window holding exactly its bytes there.
  std::vector<std::uint8_t> window(static_cast<std::size_t>(offset) + raw.size(), 0U);
  std::copy(raw.begin(), raw.end(), window.begin() + offset);
  std::array<std::uint32_t, 3> pcs{};
  std::array<detail::LoweredInstruction, 3> lowered;
  for (std::size_t i = 0; i < 3; ++i) {
    pcs[i] = kSentinel[i] + offset;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pcs[i]}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(window, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr || decoded->kind != reached.decoded->kind || decoded->raw_bytes != raw) {
      reason = "redecode";
      return std::nullopt;
    }
    const auto operation = lift_m68k_instruction(*decoded);
    lowered[i] = detail::lower_instruction(operation, pcs[i], emitter, lookup_symbol);
    if (!lowered[i].compiled) {
      reason = "not_compiled";
      return std::nullopt;
    }
  }
  SharedBody body;
  body.name = name;
  body.raw = raw;
  body.lowered.compiled = true;
  const auto derive = [&](auto member, std::string &target) {
    const auto derived = derive_source_relative_text({lowered[0].*member, lowered[1].*member, lowered[2].*member}, pcs, "scd_pc0");
    if (derived.status != SourceRelativeStatus::ok) {
      reason = source_relative_status_name(derived.status);
      return false;
    }
    target = derived.text;
    return true;
  };
  if (!derive(&detail::LoweredInstruction::slots, body.lowered.slots) || !derive(&detail::LoweredInstruction::body, body.lowered.body) ||
      !derive(&detail::LoweredInstruction::timing, body.lowered.timing) ||
      !derive(&detail::LoweredInstruction::refresh_skip, body.lowered.refresh_skip))
    return std::nullopt;
  return body;
}

// The generated function that serves (unit, pc): the shared body when the plan has one, else the unit's own.
std::string function_name(const PiPlan &plan, const Unit &unit, std::uint32_t first_offset, std::uint32_t pc) {
  const auto it = plan.shared.find({unit.image_index, pc - unit.start + first_offset});
  return it != plan.shared.end() ? it->second.name : unit.prefix + "i_" + hex(pc);
}

void emit_bytes(std::ostringstream &out, const std::string &name, const std::vector<std::uint8_t> &raw) {
  out << "static const uint8_t " << name << "_bytes[] = {";
  for (std::size_t i = 0; i < raw.size(); ++i) out << (i ? ", " : "") << "0x" << hex(raw[i], 2);
  out << "};\n";
}

// Emit one unit's per-instruction functions. Returns false for an instruction emitted as the typed instruction_unsupported stop.
void emit_unit_functions(std::ostringstream &out, unsigned cpu, const SegaCdM68kRuntimeCEmitter &emitter, const std::string &lookup_symbol,
                         const Unit &unit, std::uint32_t first_offset, const PiPlan &plan, ProcessorEmissionStats &stats,
                         ImageEmissionStats &image_stats, std::set<std::uint32_t> &unsupported) {
  const std::string k = std::to_string(cpu) + "U";
  for (const auto &[pc, reached] : unit.traversal.instructions) {
    if (plan.shared.count({unit.image_index, pc - unit.start + first_offset}) != 0U) {  // served by a shared source-relative body
      ++stats.compiled;
      ++image_stats.compiled;
      ++stats.shared_instances;
      ++image_stats.shared_instances;
      continue;
    }
    if (!plan.shared.empty() || !plan.refused.empty()) {
      if (plan.refused.count({unit.image_index, pc - unit.start + first_offset}) != 0U) {
        ++stats.fallback_instances;
        ++image_stats.fallback_instances;
      }
    }
    const std::string fn = unit.prefix + "i_" + hex(pc);
    detail::LoweredInstruction lowered;
    if (reached.operation) lowered = detail::lower_instruction(*reached.operation, pc, emitter, lookup_symbol);
    const bool compiled = reached.operation && lowered.compiled;
    if (!compiled) {
      ++stats.unsupported;
      ++image_stats.unsupported;
      unsupported.insert(pc);
      out << "static int " << fn << "(ScdMachine *m) { return scd_fail(m, " << k << ", SCD_STOP_INSTRUCTION_UNSUPPORTED, UINT32_C(0x"
          << hex(pc) << "), UINT32_C(0x" << hex(pc) << ")); }\n";
      continue;
    }
    ++stats.compiled;
    ++image_stats.compiled;
    ++stats.bodies;
    const auto &raw = reached.decoded->raw_bytes;
    emit_bytes(out, fn, raw);
    out << "static int " << fn << "(ScdMachine *m) {\n"
        << "  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n"
        << "  uint32_t pc = cpu->pc;\n"
        << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << fn << "_bytes, " << raw.size()
        << "U) != 0) return 1;\n"
        << lowered.slots << lowered.body << "  cpu->pc = pc;\n"
        << "  return scd_retire(m, " << k << ", (uint32_t)(" << lowered.timing << "));\n}\n";
  }
}

// Overlay groups (ADR 0132): connected components of overlapping units that contain at least one module unit. Boot-only components
// keep the SEG-062 per-PC dispatch unchanged.
struct Group {
  std::uint32_t start{};
  std::uint32_t end{};
  std::vector<std::size_t> units;  // indices into the processor's unit list, registry (build) order
};

std::uint32_t granule_of(std::uint32_t address) { return address >> kSegaCdOverlayGranuleShift; }

// First eight bytes (big-endian) of SHA-256 over the bytes of `unit` inside granule `g` (the runtime computes the same over live bytes).
std::string granule_digest(std::span<const std::uint8_t> bytes, const ImageMapping &mapping, const Unit &unit, std::uint32_t g) {
  const std::uint32_t lo = std::max(unit.start, g << kSegaCdOverlayGranuleShift);
  const std::uint32_t hi = static_cast<std::uint32_t>(std::min<std::uint64_t>(unit.end, (static_cast<std::uint64_t>(g) + 1U) << kSegaCdOverlayGranuleShift));
  const auto piece = bytes.subspan(mapping.first_offset + (lo - unit.start), hi - lo);
  return sha256_hex(piece).substr(0, 16);
}

std::string c_string(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) >= 0x7FU) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\%03o", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buffer;
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}


struct ProcessorOutput {
  std::string functions;  // entries, lookup, instruction functions, dispatch (the SEG-062 layout)
  std::string tables;     // overlay candidate tables (empty without groups)
  std::string groups;     // ScdOverlayGroup initializers
  std::uint32_t group_count{};
};

EmissionError plan_units(const ProcessorImages &processor, unsigned cpu, std::vector<Unit> &units) {
  const auto &set = processor.images;
  if (processor.roles.size() != set.images.size()) return EmissionError::roles_mismatch;
  for (std::uint32_t i = 0; i < set.images.size(); ++i) {
    const auto &image = set.images[i];
    const auto &role = processor.roles[i];
    for (std::uint32_t j = 0; j < image.mappings.size(); ++j) {
      const auto &mapping = image.mappings[j];
      Unit unit;
      unit.image_index = i;
      unit.mapping_index = j;
      unit.start = mapping.execution_base + mapping.first_offset;
      unit.end = unit.start + mapping.length;
      if (!role.module) {
        if (i != 0U || j != 0U || set.images.front().mappings.size() != 1U) return EmissionError::boot_image_shape;
        unit.roots = {processor.entry_pc};
        unit.prefix = "scd_p" + std::to_string(cpu) + "_";
      } else {
        for (const std::uint32_t offset : role.entry_offsets)
          if (offset >= mapping.first_offset && offset < mapping.first_offset + mapping.length)
            unit.roots.push_back(mapping.execution_base + offset);
        unit.prefix = "scd_p" + std::to_string(cpu) + "_m" + std::to_string(image.id.value) + "_";
        if (image.mappings.size() > 1U) unit.prefix += "map" + std::to_string(j) + "_";
      }
      units.push_back(std::move(unit));
    }
  }
  return EmissionError::none;
}

EmissionError plan_groups(const ProcessorImages &processor, const std::vector<Unit> &units, std::vector<Group> &groups,
                          std::vector<int> &group_of) {
  std::vector<std::size_t> order(units.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return units[a].start < units[b].start; });
  group_of.assign(units.size(), -1);
  std::vector<Group> components;
  for (const std::size_t u : order) {
    if (!components.empty() && units[u].start < components.back().end) {
      components.back().end = std::max(components.back().end, units[u].end);
      components.back().units.push_back(u);
    } else {
      components.push_back(Group{units[u].start, units[u].end, {u}});
    }
  }
  for (auto &component : components) {
    bool module = false;
    for (const std::size_t u : component.units) module = module || processor.roles[units[u].image_index].module;
    if (!module) continue;
    std::sort(component.units.begin(), component.units.end());  // registry order = image order, then mapping order
    if (component.units.size() > kSegaCdOverlayMaxCandidates) return EmissionError::too_many_candidates;
    if (granule_of(component.end - 1U) - granule_of(component.start) + 1U > kSegaCdOverlayMaxGranules)
      return EmissionError::group_too_large;
    for (const std::size_t u : component.units) group_of[u] = static_cast<int>(groups.size());
    groups.push_back(std::move(component));
  }
  return EmissionError::none;
}

ProcessorOutput emit_processor(unsigned cpu, const ProcessorImages &processor, std::vector<Unit> &units, const std::vector<Group> &groups,
                               const std::vector<int> &group_of, const SegaCdEmitOptions &options, ProcessorEmissionStats &stats,
                               std::vector<ImageEmissionStats> &image_stats) {
  const SegaCdM68kRuntimeCEmitter emitter(cpu);
  const std::string p = "scd_p" + std::to_string(cpu);
  const std::string lookup_symbol = p + "_lookup"; /* outlives every emitter context that views it */
  const std::string k = std::to_string(cpu) + "U";
  std::set<std::uint32_t> all;  // the union of every unit's compiled addresses: the computed-control entry authority
  std::size_t reached_total = 0;
  for (auto &unit : units) {
    unit.traversal = traverse(processor, unit);
    reached_total += unit.traversal.instructions.size();
    for (const auto &[pc, reached] : unit.traversal.instructions) all.insert(pc);
  }
  std::ostringstream out;
  out << "/* ---- processor " << cpu << " (" << processor_name(processor.processor) << "): " << reached_total
      << " reached instructions ---- */\n";
  out << "static const uint32_t " << p << "_entries[] = {";
  bool first = true;
  for (const std::uint32_t pc : all) {
    out << (first ? "" : ",") << (first ? "" : " ") << "UINT32_C(0x" << hex(pc) << ")";
    first = false;
  }
  out << "};\n";
  out << "static const uint32_t *" << p << "_lookup(uint32_t pc) {\n"
      << "  size_t lo = 0U, hi = sizeof " << p << "_entries / sizeof " << p << "_entries[0];\n"
      << "  while (lo < hi) { const size_t mid = lo + (hi - lo) / 2U; if (" << p << "_entries[mid] == pc) return &" << p
      << "_entries[mid]; if (" << p << "_entries[mid] < pc) lo = mid + 1U; else hi = mid; }\n"
      << "  return NULL;\n}\n";
  PiPlan plan;
  if (options.position_independent) {
    std::map<std::uint32_t, std::vector<std::size_t>> by_image;
    for (std::size_t u = 0; u < units.size(); ++u)
      if (processor.roles[units[u].image_index].module) by_image[units[u].image_index].push_back(u);
    for (const auto &[image_index, unit_indices] : by_image) {
      if (unit_indices.size() < 2U) continue;  // one mapping: nothing to share
      const auto &image = processor.images.images[image_index];
      std::map<std::uint32_t, const Reached *> first;
      std::map<std::uint32_t, std::uint32_t> instances;
      for (const std::size_t u : unit_indices) {
        const auto &unit = units[u];
        const auto first_offset = image.mappings[unit.mapping_index].first_offset;
        for (const auto &[pc, reached] : unit.traversal.instructions) {
          if (!compiled_bytes(reached)) continue;
          const std::uint32_t offset = pc - unit.start + first_offset;
          first.emplace(offset, &reached);
          ++instances[offset];
        }
      }
      for (const auto &[offset, reached] : first) {
        std::string reason;
        const std::string name = "scd_p" + std::to_string(cpu) + "_m" + std::to_string(image.id.value) + "_s" + hex(offset, 6);
        auto body = derive_shared_body(*reached, offset, emitter, lookup_symbol, name, reason);
        if (body) {
          body->instances = instances[offset];
          plan.shared.emplace(std::make_pair(image_index, offset), std::move(*body));
        } else {
          plan.refused.emplace(std::make_pair(image_index, offset), reason);
        }
      }
    }
    std::map<std::string, std::uint32_t> reasons;
    for (const std::size_t u : [&] { std::vector<std::size_t> all; for (const auto &[ii, us] : by_image) all.insert(all.end(), us.begin(), us.end()); return all; }()) {
      const auto &unit = units[u];
      const auto first_offset = processor.images.images[unit.image_index].mappings[unit.mapping_index].first_offset;
      if (by_image[unit.image_index].size() < 2U) continue;
      for (const auto &[pc, reached] : unit.traversal.instructions) {
        const auto it = plan.refused.find({unit.image_index, pc - unit.start + first_offset});
        if (it != plan.refused.end() && compiled_bytes(reached)) ++reasons[it->second];
      }
    }
    for (const auto &[reason, count] : reasons) stats.fallback_reasons.emplace_back(reason, count);
    for (const auto &[key, body] : plan.shared) {
      ++stats.bodies;
      ++stats.shared_bodies;
      emit_bytes(out, body.name, body.raw);
      out << "static int " << body.name << "(ScdMachine *m) {\n"
          << "  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n"
          << "  uint32_t pc = cpu->pc;\n"
          << "  const uint32_t scd_pc0 = pc; /* the guest address this execution was dispatched at (ADR 0207) */\n"
          << "  if (scd_code_guard(m, " << k << ", scd_pc0, " << body.name << "_bytes, " << body.raw.size() << "U) != 0) return 1;\n"
          << body.lowered.slots << body.lowered.body << "  cpu->pc = pc;\n"
          << "  return scd_retire(m, " << k << ", (uint32_t)(" << body.lowered.timing << "));\n}\n";
    }
  }
  std::vector<std::set<std::uint32_t>> unsupported(units.size());
  for (std::size_t u = 0; u < units.size(); ++u) {
    const auto &unit = units[u];
    const auto &image = processor.images.images[unit.image_index];
    ImageEmissionStats istats;
    istats.processor = processor.processor;
    istats.image_id = image.id.value;
    istats.name = processor.roles[unit.image_index].name;
    istats.producer = image.provenance.producer;
    istats.mapping_index = unit.mapping_index;
    istats.start = unit.start;
    istats.length = unit.end - unit.start;
    istats.roots = static_cast<std::uint32_t>(unit.roots.size());
    istats.overlay_group = group_of[u];
    istats.out_of_image_targets = unit.traversal.out_of_image;
    emit_unit_functions(out, cpu, emitter, lookup_symbol, unit, image.mappings[unit.mapping_index].first_offset, plan, stats, istats, unsupported[u]);
    stats.out_of_image_targets += unit.traversal.out_of_image;
    stats.vector_roots += unit.traversal.vector_roots;
    image_stats.push_back(std::move(istats));
  }
  out << "static int " << p << "_dispatch(ScdMachine *m) {\n  (void)" << p << "_lookup; /* referenced by computed control */\n"
      << "  switch (m->cpu[" << cpu << "].pc) {\n";
  for (std::size_t u = 0; u < units.size(); ++u) {
    if (group_of[u] >= 0) continue;  // overlay candidates are selected by the platform (scd_overlay_dispatch), never by PC alone
    for (const auto &[pc, reached] : units[u].traversal.instructions)
      out << "  case UINT32_C(0x" << hex(pc) << "): return "
          << function_name(plan, units[u], processor.images.images[units[u].image_index].mappings[units[u].mapping_index].first_offset, pc)
          << "(m);\n";
  }
  if (groups.empty())
    out << "  default: return scd_fail(m, " << k << ", SCD_STOP_ENTRY_NOT_COMPILED, m->cpu[" << cpu << "].pc, m->cpu[" << cpu
        << "].pc);\n  }\n}\n";
  else
    out << "  default: return scd_overlay_dispatch(m, " << k << ");\n  }\n}\n";

  ProcessorOutput result;
  std::ostringstream tables, group_text;
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const auto &group = groups[g];
    const std::string gp = p + "_g" + std::to_string(g);
    std::vector<std::string> candidates;
    for (const std::size_t u : group.units) {
      const auto &unit = units[u];
      const auto &image = processor.images.images[unit.image_index];
      const auto &mapping = image.mappings[unit.mapping_index];
      const auto bytes = executable_image_bytes(processor.images, image);
      const std::string cp = unit.prefix + "c";
      const std::uint32_t g0 = granule_of(unit.start), g1 = granule_of(unit.end - 1U);
      tables << "static const uint64_t " << cp << "_digests[] = {";
      for (std::uint32_t gi = g0; gi <= g1; ++gi)
        tables << (gi == g0 ? "" : ", ") << "UINT64_C(0x" << granule_digest(bytes, mapping, unit, gi) << ")";
      tables << "};\n";
      const auto &instructions = unit.traversal.instructions;
      tables << "static const uint32_t " << cp << "_entries[] = {";
      first = true;
      for (const auto &[pc, reached] : instructions) {
        tables << (first ? "" : ", ") << "UINT32_C(0x" << hex(pc) << ")";
        first = false;
      }
      if (instructions.empty()) tables << "0U";
      tables << "};\n";
      tables << "static const uint8_t *const " << cp << "_entry_bytes[] = {";
      first = true;
      for (const auto &[pc, reached] : instructions) {
        const bool has = compiled_bytes(reached) && unsupported[u].count(pc) == 0U;
        tables << (first ? "" : ", ") << (has ? function_name(plan, unit, mapping.first_offset, pc) + "_bytes" : std::string("NULL"));
        first = false;
      }
      if (instructions.empty()) tables << "NULL";
      tables << "};\n";
      tables << "static const uint8_t " << cp << "_entry_lengths[] = {";
      first = true;
      for (const auto &[pc, reached] : instructions) {
        const bool has = compiled_bytes(reached) && unsupported[u].count(pc) == 0U;
        tables << (first ? "" : ", ") << (has ? reached.decoded->raw_bytes.size() : 0U) << "U";
        first = false;
      }
      if (instructions.empty()) tables << "0U";
      tables << "};\n";
      std::vector<std::uint32_t> roots(unit.roots.begin(), unit.roots.end());
      std::sort(roots.begin(), roots.end());
      roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
      tables << "static const uint32_t " << cp << "_roots[] = {";
      for (std::size_t r = 0; r < roots.size(); ++r) tables << (r ? ", " : "") << "UINT32_C(0x" << hex(roots[r]) << ")";
      if (roots.empty()) tables << "0U";
      tables << "};\n";
      tables << "static int " << cp << "_dispatch(ScdMachine *m) {\n  switch (m->cpu[" << cpu << "].pc) {\n";
      for (const auto &[pc, reached] : instructions)
        tables << "  case UINT32_C(0x" << hex(pc) << "): return " << function_name(plan, unit, mapping.first_offset, pc) << "(m);\n";
      tables << "  default: return SCD_IMAGE_NO_ENTRY;\n  }\n}\n";
      std::ostringstream c;
      c << "{" << image.id.value << "U, " << c_string(processor.roles[unit.image_index].name) << ", UINT32_C(0x" << hex(unit.start)
        << "), UINT32_C(0x" << hex(unit.end - unit.start) << "), " << cp << "_digests, " << g0 << "U, " << (g1 - g0 + 1U) << "U, " << cp
        << "_entries, " << instructions.size() << "U, " << cp << "_entry_bytes, " << cp << "_entry_lengths, " << cp << "_roots, "
        << roots.size() << "U, " << cp << "_dispatch}";
      candidates.push_back(c.str());
    }
    tables << "static const ScdOverlayCandidate " << gp << "_candidates[] = {\n";
    for (const auto &c : candidates) tables << "  " << c << ",\n";
    tables << "};\n";
    group_text << "  {" << k << ", UINT32_C(0x" << hex(group.start) << "), UINT32_C(0x" << hex(group.end) << "), " << gp
               << "_candidates, " << candidates.size() << "U, " << granule_of(group.start) << "U, "
               << (granule_of(group.end - 1U) - granule_of(group.start) + 1U) << "U},\n";
  }
  result.functions = out.str();
  result.tables = tables.str();
  result.groups = group_text.str();
  result.group_count = static_cast<std::uint32_t>(groups.size());
  return result;
}

}  // namespace

const char *emission_error_name(EmissionError error) noexcept {
  switch (error) {
  case EmissionError::none: return "none";
  case EmissionError::roles_mismatch: return "roles_mismatch";
  case EmissionError::boot_image_shape: return "boot_image_shape";
  case EmissionError::too_many_overlay_groups: return "too_many_overlay_groups";
  case EmissionError::too_many_candidates: return "too_many_candidates";
  case EmissionError::group_too_large: return "group_too_large";
  case EmissionError::image_validation: return "image_validation";
  }
  return "unknown";
}

ProcessorsEmission emit_sega_cd_processors(const BootImages &images, const SegaCdEmitOptions &options) {
  ProcessorsEmission result;
  const ProcessorImages *processors[2] = {&images.main, &images.sub};
  std::ostringstream functions, tables, groups;
  for (unsigned cpu = 0; cpu < 2; ++cpu) {
    if (!validate_executable_image_set(processors[cpu]->images).ok()) {
      result.error = EmissionError::image_validation;
      return result;
    }
    std::vector<Unit> units;
    std::vector<Group> planned;
    std::vector<int> group_of;
    if (const auto error = plan_units(*processors[cpu], cpu, units); error != EmissionError::none) {
      result.error = error;
      return result;
    }
    if (const auto error = plan_groups(*processors[cpu], units, planned, group_of); error != EmissionError::none) {
      result.error = error;
      return result;
    }
    ProcessorEmissionStats stats;
    stats.processor = processors[cpu]->processor;
    const auto output = emit_processor(cpu, *processors[cpu], units, planned, group_of, options, stats, result.images);
    functions << output.functions;
    tables << output.tables;
    groups << output.groups;
    result.overlay_groups += output.group_count;
    result.stats.push_back(stats);
  }
  if (result.overlay_groups > kSegaCdOverlayMaxGroups) {
    result.error = EmissionError::too_many_overlay_groups;
    return result;
  }
  result.c_source = functions.str();
  if (result.overlay_groups != 0U)
    result.overlay_source = "/* ---- overlay registry (ADR 0127/0132): finite, build-time; selected by live-byte verification ---- */\n" +
                            tables.str() + "static const ScdOverlayGroup scd_overlay_groups[] = {\n" + groups.str() + "};\n";
  return result;
}

ProgramEmission emit_sega_cd_program(const DiscImage &disc, const BootImages &images, std::string_view module_identity,
                                     const SegaCdEmitOptions &options) {
  ProgramEmission result;
  std::ostringstream out;
  std::string identity_text = std::string(kSegaCdGeneratorVersion) + "\n" + disc.identity() + "\n";
  if (!module_identity.empty()) identity_text += "modules " + std::string(module_identity) + "\n";
  if (options.position_independent) identity_text += "emitter position_independent.v1\n";  // generated-code identity only when not the reference
  result.program_identity =
      sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(identity_text.data()), identity_text.size()));
  auto processors = emit_sega_cd_processors(images, options);
  result.error = processors.error;
  if (processors.error != EmissionError::none) return result;
  result.stats = std::move(processors.stats);
  result.images = std::move(processors.images);
  result.overlay_groups = processors.overlay_groups;
  out << "/* Generated by segarecomp (" << kSegaCdGeneratorVersion << ", SEG-062). Sega CD synthetic post-BIOS profile.\n"
      << " * Disc identity " << disc.identity() << ". Do not edit. */\n";
  if (!module_identity.empty())
    out << "/* SEG-063: disc-loaded modules from manifest " << module_identity << " (" << result.overlay_groups << " overlay groups). */\n";
  out << "#include <stddef.h>\n#include <stdint.h>\n#include \"scd_runtime.h\"\n\n";
  out << processors.c_source;
  out << processors.overlay_source;
  out << "static const ScdDiscFile scd_disc_files[] = {\n";
  for (const auto &file : disc.files())
    out << "  {" << c_string(file.name) << ", UINT64_C(" << file.size << "), \"" << file.sha256 << "\"},\n";
  out << "};\nstatic const ScdDiscTrack scd_disc_tracks[] = {\n";
  for (const auto &track : disc.tracks())
    out << "  {" << track.number << "U, " << static_cast<unsigned>(track.type) << "U, " << track.file_index << "U, " << track.sector_size
        << "U, UINT64_C(" << track.file_offset << "), " << track.first_lba << "U, " << track.sector_count << "U},\n";
  out << "};\n";
  const auto image_fields = [&](const ProcessorImages &p, const SegaCdBootDescriptor &d, unsigned cpu) {
    const auto &mapping = p.images.images.front().mappings.front();
    std::ostringstream text;
    text << "{scd_p" << cpu << "_entries, (uint32_t)(sizeof scd_p" << cpu << "_entries / sizeof scd_p" << cpu << "_entries[0]), scd_p"
         << cpu << "_dispatch, UINT32_C(0x" << hex(p.entry_pc) << "), UINT32_C(0x" << hex(mapping.execution_base) << "), UINT32_C(0x"
         << hex(d.disc_address) << "), UINT32_C(0x" << hex(mapping.length) << ")}";
    return text.str();
  };
  out << "static const ScdProgram scd_program = {\n  {" << image_fields(images.main, images.header.ip, 0) << ",\n   "
      << image_fields(images.sub, images.header.sp, 1) << "},\n"
      << "  {scd_disc_files, " << disc.files().size() << "U, scd_disc_tracks, " << disc.tracks().size() << "U, \"" << disc.identity()
      << "\", {0}},\n"
      << "  UINT32_C(0x" << hex(kMainInitialSsp) << "),\n  \"" << result.program_identity << "\"\n};\n\n";
  if (result.overlay_groups == 0U) {
    out << "int main(int argc, char **argv) { return scd_program_main(&scd_program, argc, argv); }\n";
  } else {
    out << "static const ScdOverlayProgram scd_overlays = {scd_overlay_groups, " << result.overlay_groups << "U, \""
        << module_identity << "\"};\n\n"
        << "int main(int argc, char **argv) { return scd_program_main_overlays(&scd_program, &scd_overlays, argc, argv); }\n";
  }
  result.c_source = out.str();
  return result;
}

}  // namespace segarecomp::machine::sega_cd
