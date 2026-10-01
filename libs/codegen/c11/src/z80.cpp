#include "segarecomp/codegen/c11/z80.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "segarecomp/codegen/c11/compiled_entry_table.hpp"
#include "segarecomp/codegen/c11/translation_units.hpp"
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::DecodedInstruction;
using cpu::z80::StartClassification;
using cpu::z80::StartKind;
using cpu::z80::TableFetch;

constexpr std::size_t kSpace = 65536;

std::string hex(unsigned value, unsigned digits) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%0*X", static_cast<int>(digits), value);
  return buffer;
}

std::string owner_symbol(std::uint32_t identity, std::uint16_t key) { return "z80_o_" + hex(identity, 4) + "_" + hex(key, 4); }
std::string owner_declaration(const std::string& symbol) {
  return "struct Z80OwnerRef " + symbol + "(struct Z80Runtime *rt, uint16_t window_base)";
}

// ---- address classification of the generation-time logical code mapping (ADR 0058 section 5) ----
enum class AddrClass : std::uint8_t { none, invariant, banked };
struct AddrInfo {
  AddrClass klass = AddrClass::none;
  std::size_t image = 0;  // invariant: index of the owning image
};

// One body variant of an owner: the classification shared by a set of window bases (empty for absolute owners).
struct Variant {
  std::vector<std::uint16_t> bases;
  StartClassification classification;
  const LoweringRow* row = nullptr;  // set for a decoded start with a lowering row
  bool absent = false;               // decoded but not lowered
  // Shared body (SEG-033-T005): the instruction's whole effect (R, statements, Q, T-states) is PC-independent text, so it
  // lives once in an external function `z80_fb_<n>(rt)` and every entry with the same text calls it. `flow` is the tail the
  // entry still emits itself.
  static constexpr std::size_t kNoBody = static_cast<std::size_t>(-1);
  std::size_t body = kNoBody;
  Flow flow = Flow::fallthrough;
};

struct Plan {
  std::vector<std::uint8_t> guard;  // RAM-backed image: the exact static instruction bytes verified before the effect
  std::uint32_t identity = 0;
  std::uint16_t key = 0;
  bool relative = false;
  bool partial = false;  // window-relative owner exposed by only some of the image's windows: it must check the base
  std::uint64_t dense = 0;
  bool invariant = false;
  std::vector<Variant> variants;
  static constexpr std::size_t kNoSuccessor = static_cast<std::size_t>(-1);
  std::size_t successor = kNoSuccessor;  // plan index of a directly bound fall-through
};

// Where a fall-through goes (SEG-033-T002). `symbol` non-empty: return the (possibly shared) owner as a direct binding;
// `label` non-empty: the successor is another entry of the same grouped owner (local jump, no trampoline). `set_pc`: the
// target selects its entry from `s->pc`, which a direct binding must therefore store first.
struct Successor {
  std::string symbol;
  std::string label;
  bool set_pc = false;
};

// A grouped owner: consecutive plans of one image and one owner class emitted as one C function (a group of one is the
// historical one-function-per-start owner).
struct Group {
  std::size_t first = 0;
  std::size_t count = 0;
};

bool same_semantics(const StartClassification& a, const StartClassification& b) {
  if (a.kind != b.kind) return false;
  if (a.kind != StartKind::decoded) return true;
  const auto& p = a.instruction.provenance;
  const auto& q = b.instruction.provenance;
  return a.instruction.form == b.instruction.form && a.instruction.extra_prefix_count == b.instruction.extra_prefix_count &&
         p.logical_byte_count == q.logical_byte_count && p.prefix_count == q.prefix_count &&
         p.effective_prefix == q.effective_prefix && p.opcode == q.opcode && p.operands == q.operands &&
         p.operand_count == q.operand_count;
}

Variant make_variant(const StartClassification& c) {
  Variant v;
  v.classification = c;
  if (c.kind == StartKind::decoded) {
    v.row = find_lowering(cpu::z80::form_descriptor(c.instruction.form));
    v.absent = v.row == nullptr;
  }
  return v;
}

OwnerKind kind_of(const Variant& v) {
  switch (v.classification.kind) {
    case StartKind::prefix_lock: return OwnerKind::prefix_lock;
    case StartKind::unresolved_fetch_mapping: return OwnerKind::stub_unresolved_fetch_mapping;
    case StartKind::mutable_code: return OwnerKind::stub_mutable_code;
    case StartKind::excluded_form: return OwnerKind::stub_excluded_form;
    case StartKind::decoded: return OwnerKind::full;
  }
  return OwnerKind::stub_excluded_form;
}

const char* error_name(const Variant& v) {
  switch (v.classification.kind) {
    case StartKind::unresolved_fetch_mapping: return "Z80_ERROR_UNRESOLVED_FETCH_MAPPING";
    case StartKind::mutable_code: return "Z80_ERROR_MUTABLE_CODE";
    case StartKind::excluded_form: return "Z80_ERROR_EXCLUDED_FORM";
    default: return "Z80_ERROR_NO_OWNER";  // an absent (unlowered) variant of a window-relative owner
  }
}

// Distinct shared bodies in first-use order (plan order, so deterministic). The text is the function body below the
// `Z80State *s` line.
struct BodyPool {
  std::map<std::string, std::size_t> ids;
  std::vector<const std::string*> texts;
  std::size_t intern(const std::string& text) {
    const auto [it, inserted] = ids.emplace(text, ids.size());
    if (inserted) texts.push_back(&it->first);
    return it->second;
  }
};

std::string body_symbol(std::size_t id) { return "z80_fb_" + std::to_string(id); }
std::string body_declaration(std::size_t id) { return "void " + body_symbol(id) + "(struct Z80Runtime *rt)"; }

struct Pcs {
  std::string start;
  std::string next;
};

Pcs pcs_for(const Plan& plan, const StartClassification& c, std::uint16_t key) {
  const std::uint32_t length = c.kind == StartKind::decoded ? c.instruction.provenance.logical_byte_count : 1;
  const auto next = static_cast<std::uint16_t>(key + length);
  if (!plan.relative) return {hex_literal(key, 4), hex_literal(next, 4)};
  return {"(uint16_t)(window_base + " + hex_literal(key, 4) + ")", "(uint16_t)(window_base + " + hex_literal(next, 4) + ")"};
}

// The instruction's effect as C text: R accounting, the lowered statements, Q and T-states (everything but the owner prologue
// and the control-flow tail).
std::string effect_text(const DecodedInstruction& insn, const Lowered& lowered, const std::string& indent) {
  std::ostringstream out;
  out << indent << "s->r = z80_r_add(s->r, " << cpu::z80::m1_fetches(insn) << "u);\n";
  std::istringstream body(lowered.statements);
  for (std::string line; std::getline(body, line);) out << indent << line << "\n";
  out << indent << (lowered.writes_flags ? "s->q = s->f;\n" : "s->q = 0u;\n");
  if (lowered.cycles_expression.empty())
    out << indent << "s->cycles += " << cpu::z80::t_states(insn, cpu::z80::TimingOutcome::primary) << "u;\n";
  else
    out << indent << "s->cycles += (uint64_t)(" << lowered.cycles_expression << ");\n";
  return out.str();
}

// Shares the effect of a decoded variant when it is PC-independent. Rows must take every PC-dependent value from the context
// expressions (z80_lowering.hpp), so lowering with two different placeholder PCs yields identical text exactly when the
// effect does not depend on the instruction's address.
void share_body(const Plan& plan, Variant& v, BodyPool& pool) {
  const DecodedInstruction& insn = v.classification.instruction;
  const cpu::z80::FormDescriptor& form = cpu::z80::form_descriptor(insn.form);
  const Pcs pc = pcs_for(plan, v.classification, plan.key);
  const Lowered real = v.row->lower(LowerContext{insn, form, pc.start, pc.next});
  const Lowered probe = v.row->lower(LowerContext{insn, form, "0x7777u", "0x7779u"});
  if (probe.statements != real.statements || probe.cycles_expression != real.cycles_expression ||
      probe.writes_flags != real.writes_flags || probe.flow != real.flow)
    return;
  v.body = pool.intern(effect_text(insn, real, "  "));
  v.flow = real.flow;
}

// Returns true when the body ends in the in-group `goto` of `successor.label`. Bodies the entry calls are added to `bodies`
// (the caller declares them in block scope: the shared header deliberately declares no owner or body, ADR 0058 RSS budget).
bool emit_variant_body(std::ostream& out, const Plan& plan, const Variant& v, std::uint16_t key, const std::string& indent,
                       const Successor& successor, std::set<std::size_t>& bodies) {
  const Pcs pc = pcs_for(plan, v.classification, key);
  switch (v.classification.kind) {
    case StartKind::prefix_lock:
      out << indent << "if (z80_lock_enter(rt, " << pc.start << ")) return Z80_OWNER_STOP;\n"
          << indent << "z80_lock_run(rt, " << pc.start << ");\n"
          << indent << "return Z80_OWNER_STOP;\n";
      return false;
    case StartKind::decoded:
      if (!v.absent) break;
      [[fallthrough]];
    default:
      out << indent << "s->pc = " << pc.start << ";\n"
          << indent << "rt->outcome = " << error_name(v) << ";\n"
          << indent << "return Z80_OWNER_STOP;\n";
      return false;
  }
  Flow flow = v.flow;
  if (v.body != Variant::kNoBody) {
    bodies.insert(v.body);
    out << indent << body_symbol(v.body) << "(rt);\n";
  } else {
    const DecodedInstruction& insn = v.classification.instruction;
    const cpu::z80::FormDescriptor& form = cpu::z80::form_descriptor(insn.form);
    const Lowered lowered = v.row->lower(LowerContext{insn, form, pc.start, pc.next});
    out << effect_text(insn, lowered, indent);
    flow = lowered.flow;
  }
  switch (flow) {
    case Flow::fallthrough:
      if (!successor.label.empty()) {
        out << indent << "goto " << successor.label << ";\n";
        return true;
      }
      if (!successor.symbol.empty()) {
        // Block-scope declaration: the shared header deliberately declares no owner (ADR 0058 RSS budget).
        out << indent << owner_declaration(successor.symbol) << ";\n";
        if (successor.set_pc) out << indent << "s->pc = " << pc.next << ";\n";
        out << indent << "return Z80_OWNER_NEXT(" << successor.symbol << ");\n";
        return false;
      }
      [[fallthrough]];
    case Flow::dispatch_fallthrough:
      out << indent << "s->pc = " << pc.next << ";\n" << indent << "return Z80_OWNER_STOP;\n";
      return false;
    case Flow::dispatch:
      out << indent << "return Z80_OWNER_STOP;\n";
      return false;
  }
  return false;  // unreachable: every Flow is handled above (GCC's -Wreturn-type does not see through the enum switch)
}

// The part of one entry after the function header: owner prologue and the variant body (or the per-window-base switch).
// Returns true when the entry ends in the in-group `goto` of its successor's label.
bool emit_entry(std::ostream& out, const Plan& plan, const std::string& indent, const Successor& successor,
                std::set<std::size_t>& bodies) {
  const Variant& first = plan.variants.front();
  const Pcs pc = pcs_for(plan, first.classification, plan.key);
  const bool lock = plan.variants.size() == 1 && first.classification.kind == StartKind::prefix_lock;
  if (!lock) out << indent << "if (z80_owner_prologue(rt, " << pc.start << ")) return Z80_OWNER_STOP;\n";
  if (!plan.guard.empty()) {  // RAM-backed image: the live bytes must still be the compiled instruction before any effect
    out << indent << "if (z80_code_guard(rt, " << pc.start << ", " << plan.guard.size() << "u";
    for (std::size_t i = 0; i < 4; ++i) out << ", " << (i < plan.guard.size() ? static_cast<unsigned>(plan.guard[i]) : 0u) << "u";
    out << ")) return Z80_OWNER_STOP;\n";
  }
  if (plan.variants.size() == 1 && !plan.partial) return emit_variant_body(out, plan, first, plan.key, indent, successor, bodies);
  out << indent << "switch (window_base) {\n";
  for (const Variant& v : plan.variants) {
    for (const std::uint16_t base : v.bases) out << indent << "  case " << hex_literal(base, 4) << ":\n";
    out << indent << "  {\n";
    if (v.classification.kind == StartKind::prefix_lock)
      out << indent << "    z80_lock_run(rt, " << pcs_for(plan, v.classification, plan.key).start << ");\n"
          << indent << "    return Z80_OWNER_STOP;\n";
    else
      emit_variant_body(out, plan, v, plan.key, indent + "    ", Successor{}, bodies);
    out << indent << "  }\n";
  }
  out << indent << "  default:\n" << indent << "  {\n" << indent << "    s->pc = " << pc.start << ";\n"
      << indent << "    rt->outcome = Z80_ERROR_NO_OWNER;\n" << indent << "    return Z80_OWNER_STOP;\n" << indent << "  }\n"
      << indent << "}\n";
  return false;
}

std::string entry_label(std::uint16_t key) { return "z80_e_" + hex(key, 4); }

void emit_body_declarations(std::ostream& out, const std::set<std::size_t>& bodies) {
  for (const std::size_t id : bodies) out << "  " << body_declaration(id) << ";\n";
}

void emit_owner(std::ostream& out, const Plan& plan, const std::string& symbol, const std::string& runtime_note, const Successor& successor) {
  std::set<std::size_t> bodies;
  std::ostringstream entry;
  emit_entry(entry, plan, "  ", successor, bodies);
  out << "/* " << runtime_note << " */\n";
  out << "static " << owner_declaration(symbol) << " {\n  Z80State *s = &rt->state;\n  (void)s;\n";
  if (!plan.relative) out << "  (void)window_base;\n";
  emit_body_declarations(out, bodies);
  out << entry.str() << "}\n";
}

// A grouped owner: one function holding `count` exact entries. The entry is selected from the PC the dispatcher (or a direct
// binding) stored in `s->pc`: the PC itself for absolute-PC owners, `PC - window_base` (the image offset) for window-relative
// owners. A selector the group does not own fails closed with `no_owner`, never a nearby entry.
void emit_group(std::ostream& out, const std::vector<Plan>& plans, const Group& group, const std::string& symbol,
                const std::string& note, const std::vector<Successor>& successors) {
  const bool relative = plans[group.first].relative;
  // Render every entry first: an entry's label exists only if an earlier or later entry actually jumps to it.
  std::vector<std::string> rendered;
  std::set<std::string> used;
  std::set<std::size_t> bodies;
  for (std::size_t i = group.first; i < group.first + group.count; ++i) {
    std::ostringstream entry;
    if (emit_entry(entry, plans[i], "      ", successors[i], bodies)) used.insert(successors[i].label);
    rendered.push_back(entry.str());
  }
  out << "/* " << note << " */\n";
  out << "static " << owner_declaration(symbol) << " {\n  Z80State *s = &rt->state;\n  (void)s;\n";
  if (!relative) out << "  (void)window_base;\n";
  emit_body_declarations(out, bodies);
  out << "  switch (" << (relative ? "(uint16_t)(s->pc - window_base)" : "s->pc") << ") {\n";
  for (std::size_t i = group.first; i < group.first + group.count; ++i) {
    const Plan& plan = plans[i];
    out << "    case " << hex_literal(plan.key, 4) << ":\n    {\n";
    if (used.contains(entry_label(plan.key))) out << entry_label(plan.key) << ":\n";
    out << rendered[i - group.first] << "    }\n";
  }
  out << "    default:\n    {\n      rt->outcome = Z80_ERROR_NO_OWNER;\n      return Z80_OWNER_STOP;\n    }\n  }\n}\n";
}

}  // namespace

const char* owner_kind_name(OwnerKind kind) {
  switch (kind) {
    case OwnerKind::full: return "full";
    case OwnerKind::prefix_lock: return "prefix_lock";
    case OwnerKind::stub_mutable_code: return "stub_mutable_code";
    case OwnerKind::stub_unresolved_fetch_mapping: return "stub_unresolved_fetch_mapping";
    case OwnerKind::stub_excluded_form: return "stub_excluded_form";
  }
  return "?";
}

EmitResult emit_image_set(const ImageSet& set, const EmitOptions& options) {
  EmitResult result;
  const auto fail = [&](std::string message) {
    result.error = std::move(message);
    return result;
  };

  // ---- validation ----
  std::vector<const CodeImage*> images;
  for (const CodeImage& image : set.images) images.push_back(&image);
  std::ranges::sort(images, [](const CodeImage* a, const CodeImage* b) { return a->identity < b->identity; });
  for (std::size_t i = 0; i < images.size(); ++i) {
    const CodeImage& image = *images[i];
    if (image.identity > 0xFFFFu) return fail("code-image identity exceeds 16 bits");
    if (i > 0 && images[i - 1]->identity == image.identity) return fail("duplicate code-image identity");
    if (image.windows.empty()) return fail("code image without a window");
    if (image.kind == ImageKind::invariant && image.windows.size() != 1) return fail("an invariant image has exactly one window");
    if (image.live_bytes && (image.kind != ImageKind::banked || image.windows.size() != 1))
      return fail("a RAM-backed image is a banked image with exactly one window");
    for (const CodeWindow& w : image.windows) {
      if (w.length == 0 || w.first_offset + w.length > kSpace || static_cast<std::uint32_t>(w.base) + w.first_offset + w.length > kSpace)
        return fail("code window leaves the 16-bit logical address space");
      if (image.bytes.size() < w.first_offset + w.length) return fail("code image shorter than its exposed range");
      for (const CodeWindow& other : image.windows) {  // two placements at one base with overlapping offsets are ambiguous
        if (&other == &w || other.base != w.base) continue;
        if (w.first_offset < other.first_offset + other.length && other.first_offset < w.first_offset + w.length)
          return fail("windows of one image overlap at the same base");
      }
      // Windows of one image may expose different sub-ranges (ADR 0058: SMS slot 0 exposes 0x0400-0x3FFF of a bank whose
      // other slots expose all of it). The owner key is the image offset, so it is well defined for any such set.
    }
  }

  // ---- logical code mapping classes ----
  std::vector<AddrInfo> info(kSpace);
  for (std::size_t n = 0; n < images.size(); ++n) {
    for (const CodeWindow& w : images[n]->windows) {
      for (std::uint32_t o = w.first_offset; o < w.first_offset + w.length; ++o) {
        AddrInfo& slot = info[static_cast<std::uint16_t>(w.base + o)];
        if (images[n]->kind == ImageKind::invariant) {
          if (slot.klass != AddrClass::none) return fail("invariant window overlaps another window");
          slot = {AddrClass::invariant, n};
        } else {
          if (slot.klass == AddrClass::invariant) return fail("banked window overlaps an invariant window");
          slot.klass = AddrClass::banked;
        }
      }
    }
  }
  TableFetch base_table;
  for (std::size_t a = 0; a < kSpace; ++a) {
    const AddrInfo& slot = info[a];
    cpu::z80::FetchedByte b;
    if (slot.klass == AddrClass::invariant) {
      const CodeImage& owner = *images[slot.image];
      const std::uint32_t offset = static_cast<std::uint16_t>(a - owner.windows.front().base);
      b = {cpu::z80::FetchKind::byte, owner.bytes[offset], owner.identity, offset};
    } else {
      b.kind = slot.klass == AddrClass::banked ? cpu::z80::FetchKind::unresolved_mapping : cpu::z80::FetchKind::non_code;
    }
    base_table.set(static_cast<std::uint16_t>(a), b);
  }
  const auto window_fetch = [&](const CodeImage& image, const CodeWindow& w) {
    TableFetch fetch = base_table;
    for (std::uint32_t o = w.first_offset; o < w.first_offset + w.length; ++o)
      fetch.set(static_cast<std::uint16_t>(w.base + o), {cpu::z80::FetchKind::byte, image.bytes[o], image.identity, o});
    return fetch;
  };

  // ---- plans (pass 1) ----
  std::vector<Plan> plans;
  std::uint64_t dense = 0;
  const std::vector<StartClassification> invariant_classes = [&] {
    for (const CodeImage* image : images)
      if (image->kind == ImageKind::invariant) return cpu::z80::classify_all(base_table);
    return std::vector<StartClassification>{};
  }();
  std::map<std::uint32_t, std::size_t> plan_of_invariant_address;  // (identity << 16 | address) -> plan index
  for (std::size_t n = 0; n < images.size(); ++n) {
    const CodeImage& image = *images[n];
    const CodeWindow& first = image.windows.front();
    if (image.kind == ImageKind::invariant) {
      for (std::uint32_t o = first.first_offset; o < first.first_offset + first.length; ++o) {
        const auto address = static_cast<std::uint16_t>(first.base + o);
        Variant v = make_variant(invariant_classes[address]);
        if (v.absent) {
          ++result.stats.unlowered_starts;
          continue;
        }
        Plan plan;
        plan.identity = image.identity;
        plan.key = address;
        plan.invariant = true;
        plan.dense = dense + (o - first.first_offset);
        plan.variants.push_back(std::move(v));
        plan_of_invariant_address.emplace(image.identity << 16 | address, plans.size());
        plans.push_back(std::move(plan));
      }
    } else {
      std::vector<std::vector<StartClassification>> classes;
      for (const CodeWindow& w : image.windows) classes.push_back(cpu::z80::classify_all(window_fetch(image, w)));
      const bool relative = image.windows.size() > 1;
      // One owner per (image, offset) over the union of the offsets any window exposes; a window contributes a variant
      // only for the offsets it actually exposes.
      std::uint32_t lo = image.windows.front().first_offset;
      std::uint32_t hi = lo;
      for (const CodeWindow& w : image.windows) {
        lo = std::min(lo, w.first_offset);
        hi = std::max(hi, w.first_offset + w.length);
      }
      for (std::uint32_t o = lo; o < hi; ++o) {
        Plan plan;
        plan.identity = image.identity;
        plan.relative = relative;
        plan.dense = dense + (o - lo);
        plan.key = relative ? static_cast<std::uint16_t>(o) : static_cast<std::uint16_t>(first.base + o);
        for (std::size_t j = 0; j < image.windows.size(); ++j) {
          const CodeWindow& w = image.windows[j];
          if (o < w.first_offset || o >= w.first_offset + w.length) continue;
          StartClassification c = classes[j][static_cast<std::uint16_t>(w.base + o)];
          if (image.live_bytes && c.kind == StartKind::prefix_lock) {  // an endless prefix run in RAM is never supported code
            c.kind = StartKind::mutable_code;
            c.blocking_address = static_cast<std::uint16_t>(w.base + o);
          }
          if (image.live_bytes && c.kind == StartKind::decoded) {
            const std::uint32_t length = c.instruction.provenance.logical_byte_count;
            if (length == 0 || length > 4 || o + length > image.bytes.size()) return fail("a RAM-backed start longer than four bytes");
            plan.guard.assign(image.bytes.begin() + o, image.bytes.begin() + o + length);
          }
          auto same = std::ranges::find_if(plan.variants, [&](const Variant& v) { return same_semantics(v.classification, c); });
          if (same == plan.variants.end()) {
            plan.variants.push_back(make_variant(c));
            same = plan.variants.end() - 1;
          }
          if (std::ranges::find(same->bases, w.base) == same->bases.end()) same->bases.push_back(w.base);  // no duplicate case labels
        }
        if (plan.variants.empty()) continue;  // an offset no window exposes
        std::size_t exposing = 0;  // distinct window bases exposing this offset vs all of the image's distinct bases
        for (const Variant& v : plan.variants) exposing += v.bases.size();
        std::vector<std::uint16_t> all_bases;
        for (const CodeWindow& w : image.windows)
          if (std::ranges::find(all_bases, w.base) == all_bases.end()) all_bases.push_back(w.base);
        plan.partial = relative && exposing < all_bases.size();  // a base that does not expose this offset must fail closed
        if (relative && std::ranges::any_of(plan.variants, [](const Variant& v) { return v.classification.kind == StartKind::prefix_lock; }))
          return fail("a prefix_lock start cannot occur in a window-relative owner");
        if (std::ranges::all_of(plan.variants, [](const Variant& v) { return v.absent; })) {
          ++result.stats.unlowered_starts;
          continue;
        }
        plans.push_back(std::move(plan));
      }
      dense += hi - lo;
      continue;
    }
    dense += first.length;
  }

  // Direct binding (pass 2): an invariant owner's fall-through to an emitted invariant-window owner.
  for (Plan& plan : plans) {
    if (!plan.invariant) continue;
    const Variant& v = plan.variants.front();
    if (v.classification.kind != StartKind::decoded) continue;
    const auto next = static_cast<std::uint16_t>(plan.key + v.classification.instruction.provenance.logical_byte_count);
    const AddrInfo& target = info[next];
    if (target.klass != AddrClass::invariant) continue;
    const std::uint32_t identity = images[target.image]->identity;
    if (const auto found = plan_of_invariant_address.find(identity << 16 | next); found != plan_of_invariant_address.end())
      plan.successor = found->second;
  }

  // Owner grouping (SEG-033-T002): consecutive plans of one image and one owner class share a bounded host function. Plans are
  // in (image, key) order, so the grouping is a pure function of the plan list and the bound: deterministic and independent of
  // emission order. A bound of one reproduces the historical one-function-per-start emission byte for byte.
  const std::size_t group_bound = std::max<std::size_t>(options.owner_group_entries, 1);
  std::vector<Group> groups;
  std::vector<std::size_t> group_of(plans.size());
  for (std::size_t i = 0; i < plans.size(); ++i) {
    const bool open = !groups.empty() && groups.back().count < group_bound &&
                      plans[groups.back().first].identity == plans[i].identity &&
                      plans[groups.back().first].relative == plans[i].relative;
    if (!open) groups.push_back(Group{i, 0});
    ++groups.back().count;
    group_of[i] = groups.size() - 1;
  }
  const auto group_symbol = [&](std::size_t group) {
    const Plan& head = plans[groups[group].first];
    return owner_symbol(head.identity, head.key);
  };
  std::vector<Successor> successors(plans.size());
  for (std::size_t i = 0; i < plans.size(); ++i) {
    const std::size_t next = plans[i].successor;
    if (next == Plan::kNoSuccessor) continue;
    if (group_of[next] == group_of[i] && groups[group_of[i]].count > 1) {
      successors[i].label = entry_label(plans[next].key);
    } else {
      successors[i].symbol = group_symbol(group_of[next]);
      successors[i].set_pc = groups[group_of[next]].count > 1;
    }
  }

  // Shared bodies (SEG-033-T005): decided before emission so the body translation units can be sized.
  BodyPool pool;
  if (options.share_bodies)
    for (Plan& plan : plans)
      if (plan.variants.size() == 1 && !plan.partial && plan.variants.front().classification.kind == StartKind::decoded &&
          !plan.variants.front().absent)
        share_body(plan, plan.variants.front(), pool);

  // ---- emission ----
  std::size_t shards = static_cast<std::size_t>((dense + 2047) / 2048);
  shards = std::clamp<std::size_t>(shards, 1, 48);
  // Entry table chunking (ADR 0058 budgets): the flat table + all owner declarations in the main TU would grow that TU, and the
  // compiler's peak memory with it, linearly with the owner count. Above kEntryChunk entries the generic chunked form puts
  // each chunk (with its own owner declarations) in an "entry" TU; smaller images keep the flat table unchanged.
  const std::size_t kEntryChunk = std::max<std::size_t>(options.entry_chunk_entries, 1);
  CompiledEntryChunking chunking;
  chunking.chunk_entries = kEntryChunk;
  chunking.declare_owner = [](std::string_view symbol) { return owner_declaration(std::string(symbol)); };
  const bool chunked_entries = plans.size() > kEntryChunk;
  std::vector<TranslationUnitFamily> families{TranslationUnitFamily{"owner", shards, 8}};
  const std::size_t body_shards = std::clamp<std::size_t>((pool.texts.size() + 2047) / 2048, 1, 32);
  if (!pool.texts.empty()) families.push_back(TranslationUnitFamily{"body", body_shards, 0});
  if (chunked_entries) families.push_back(TranslationUnitFamily{"entry", compiled_entry_chunk_count(plans.size(), kEntryChunk), 0});
  TranslationUnitSharder sharder(options.directory, options.stem, std::move(families));
  std::ostream& out = sharder.stream();
  shard_begin_header(out);
  out << "/* Generated Z80 image owners (SEG-008-T003). Deterministic; do not edit. */\n"
      << "#include \"" << options.runtime_include << "\"\n";
  shard_end_header(out);

  const auto entry_note = [&](const Plan& plan) {
    const Variant& v = plan.variants.front();
    std::string note = "image " + hex(plan.identity, 4) + (plan.relative ? " window offset " : " address ") + hex(plan.key, 4) + ": ";
    note += v.classification.kind == StartKind::decoded ? cpu::z80::form_name(v.classification.instruction.form) : owner_kind_name(kind_of(v));
    if (plan.variants.size() > 1) note += " (" + std::to_string(plan.variants.size()) + " window variants)";
    return note;
  };
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const Group& group = groups[g];
    const Plan& head = plans[group.first];
    const std::string symbol = group_symbol(g);
    ShardUnitScope unit(out, "owner", head.dense, owner_declaration(symbol), false);
    if (group.count == 1) {
      emit_owner(out, head, symbol, entry_note(head), successors[group.first]);
    } else {
      const Plan& last = plans[group.first + group.count - 1];
      emit_group(out, plans, group, symbol,
                 "image " + hex(head.identity, 4) + (head.relative ? " window offsets " : " addresses ") + hex(head.key, 4) + ".." +
                     hex(last.key, 4) + ": " + std::to_string(group.count) + " entries",
                 successors);
    }
    ++result.stats.owners;
    result.stats.max_group_entries = std::max(result.stats.max_group_entries, group.count);
  }
  for (std::size_t i = 0; i < plans.size(); ++i) {
    const Plan& plan = plans[i];
    const Variant& v = plan.variants.front();
    OwnerRecord record;
    record.identity = plan.identity;
    record.key = plan.key;
    record.window_relative = plan.relative;
    record.kind = kind_of(v);
    record.variants = static_cast<std::uint32_t>(plan.variants.size());
    record.form = v.classification.kind == StartKind::decoded ? v.classification.instruction.form : cpu::z80::kNoForm;
    record.bound_successor = plan.successor != Plan::kNoSuccessor;
    record.group_entries = static_cast<std::uint32_t>(groups[group_of[i]].count);
    ++result.stats.entries;
    if (plan.variants.size() > 1) ++result.stats.variant_owners;
    if (record.bound_successor) ++result.stats.bound_successors;
    switch (record.kind) {
      case OwnerKind::full: ++result.stats.full_owners; break;
      case OwnerKind::prefix_lock: ++result.stats.prefix_lock_owners; break;
      default: ++result.stats.stub_owners; break;
    }
    if (options.record_owners) result.owners.push_back(record);
  }

  for (std::size_t id = 0; id < pool.texts.size(); ++id) {
    ShardUnitScope unit(out, "body", id, body_declaration(id), false);
    out << body_declaration(id) << " {\n  Z80State *s = &rt->state;\n  (void)s;\n" << *pool.texts[id] << "}\n";
  }
  result.stats.shared_bodies = pool.texts.size();

  // Main translation unit: owner declarations (only here), identity table, entry table, dispatcher.
  out << (chunked_entries ? "\n/* Owner declarations live in the entry-chunk translation units. */\n"
                          : "\n/* Owner declarations: only this translation unit references every owner. */\n");
  std::vector<CompiledEntryBinding> bindings;
  for (std::size_t g = 0; g < groups.size(); ++g)
    if (!chunked_entries) out << owner_declaration(group_symbol(g)) << ";\n";
  for (std::size_t i = 0; i < plans.size(); ++i)
    bindings.push_back({static_cast<std::uint32_t>(plans[i].identity << 16 | plans[i].key), group_symbol(group_of[i])});
  out << "\nstatic const uint32_t z80_identity_ids[] = {\n";
  for (const CodeImage* image : images) out << "  UINT32_C(" << image->identity << "),\n";
  out << "};\nstatic const uint8_t z80_identity_kinds[] = { /* 1 = absolute-PC owners, 2 = window-relative owners */\n";
  for (const CodeImage* image : images) out << "  " << (image->kind == ImageKind::banked && image->windows.size() > 1 ? 2 : 1) << ",\n";
  out << "};\nstatic unsigned z80_identity_kind(uint32_t identity) {\n"
      << "  size_t low = 0U;\n  size_t high = sizeof(z80_identity_ids) / sizeof(z80_identity_ids[0]);\n"
      << "  while (low < high) {\n    const size_t middle = low + (high - low) / 2U;\n"
      << "    if (z80_identity_ids[middle] < identity) low = middle + 1U; else high = middle;\n  }\n"
      << "  return (low < sizeof(z80_identity_ids) / sizeof(z80_identity_ids[0]) && z80_identity_ids[low] == identity) ? z80_identity_kinds[low] : 0U;\n}\n\n";
  CompiledEntryTableNames names;
  names.entry_type = "Z80Owner";
  names.lookup = "z80_entry_lookup";
  names.addresses = "z80_entry_keys";
  names.owner_ids = "z80_entry_owner_ids";
  names.owners = "z80_entry_owners";
  if (const std::string rejected = emit_compiled_entry_table_chunked(out, bindings, names, chunking); !rejected.empty()) return fail(rejected);
  out << "\nZ80Outcome z80_run(Z80Runtime *rt, uint64_t deadline) {\n"
      << "  Z80State *s = &rt->state;\n"
      << "  s->deadline = deadline;\n"
      << "  rt->outcome = Z80_OUTCOME_NONE;\n"
      << "  for (;;) {\n"
      << "    Z80CodeImage image;\n"
      << "    const int boundary = z80_boundary(rt);\n"
      << "    if (boundary == 1) return rt->outcome;\n"
      << "    if (boundary == 2) continue;\n"
      << "    if (!rt->host.code_image(rt->host.context, s->pc, &image)) return rt->outcome = Z80_ERROR_MUTABLE_CODE;\n"
      << "    const unsigned kind = z80_identity_kind(image.identity);\n"
      << "    if (kind == 0U) return rt->outcome = Z80_ERROR_UNKNOWN_IMAGE_IDENTITY;\n"
      << "    const uint16_t key = kind == 2U ? (uint16_t)(s->pc - image.window_base) : s->pc;\n"
      << "    const Z80Owner owner = z80_entry_lookup((image.identity << 16) | key);\n"
      << "    if (owner == NULL) return rt->outcome = Z80_ERROR_NO_OWNER;\n"
      << "    struct Z80OwnerRef ref = owner(rt, image.window_base);\n"
      << "    while (ref.next != NULL) ref = ref.next(rt, 0U);\n"
      << "    if (rt->outcome != Z80_OUTCOME_NONE) return rt->outcome;\n"
      << "  }\n}\n";

  if (const std::string finished = sharder.finish(); !finished.empty()) return fail(finished);
  result.stats.translation_units = sharder.translation_unit_count();
  return result;
}

}  // namespace segarecomp::codegen::z80
