// SEG-138-T003/T004 (ADR 0298): see function_hooks.hpp. Pure analysis over lifted operations; deterministic (ordered containers only).

#include "segarecomp/cpu/m68k/function_hooks.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "segarecomp/cpu/m68k/effects.hpp"

namespace segarecomp {
namespace {

struct Node {
  const M68kIrOperation *operation{};
  M68kControlSuccessors flow;
  std::uint32_t address{};
  std::uint32_t length{};
  bool alias{false};
};

M68kHookResolution reject(std::string code, std::size_t site, std::string message) {
  M68kHookResolution out;
  out.ok = false;
  out.rejection = {std::move(code), site, std::move(message)};
  return out;
}

bool is_call_edge(const M68kControlSuccessor &s) { return s.kind == M68kControlSuccessorKind::call_target; }

bool is_dynamic_transfer(M68kDynamicControlFamily f) {
  return f != M68kDynamicControlFamily::none && f != M68kDynamicControlFamily::return_from_subroutine;
}

// Everything reachable from `starts` following every static edge, a call's callee and its continuation. Only instructions that exist in `nodes` are visited.
std::set<std::uint32_t> reachable(const std::map<std::uint32_t, Node> &nodes, const std::vector<std::uint32_t> &starts) {
  std::set<std::uint32_t> seen;
  std::vector<std::uint32_t> work(starts.begin(), starts.end());
  while (!work.empty()) {
    const std::uint32_t at = work.back();
    work.pop_back();
    const auto it = nodes.find(at);
    if (it == nodes.end() || !seen.insert(at).second) continue;
    for (const auto &s : it->second.flow.successors) work.push_back(s.target);
    if (it->second.flow.stacked == M68kStackedContinuationKind::call_continuation) work.push_back(it->second.flow.stacked_address);
  }
  return seen;
}

// The body of a function: instructions reachable from `entry` without entering a callee. Returns "" on success or the reason the body cannot be classified.
std::string body_of(const std::map<std::uint32_t, Node> &nodes, std::uint32_t entry, std::set<std::uint32_t> &body) {
  std::vector<std::uint32_t> work{entry};
  while (!work.empty()) {
    const std::uint32_t at = work.back();
    work.pop_back();
    if (body.count(at) != 0U) continue;
    const auto it = nodes.find(at);
    if (it == nodes.end()) return "the body runs into code the build did not compile";
    if (body.size() >= kM68kHookMaxBodyInstructions) return "the body exceeds the 4096-instruction classification bound";
    body.insert(at);
    const Node &n = it->second;
    const auto &op = *n.operation;
    if (op.kind == M68kIrKind::jump_general) return "the body contains a JMP (tail jump or jump table)";
    if (n.flow.always_raises_exception) return "the body contains an instruction that always raises an exception";
    if (n.flow.stacked == M68kStackedContinuationKind::exception_continuation) return "the body contains a TRAP form";
    if (n.flow.stacked == M68kStackedContinuationKind::pushed_code_address) return "the body pushes a code address (possible computed return)";
    if (n.flow.dynamic == M68kDynamicControlFamily::return_from_exception || n.flow.dynamic == M68kDynamicControlFamily::return_restore_condition_codes)
      return "the body returns with RTE or RTR";
    if (n.flow.dynamic == M68kDynamicControlFamily::unclassified) return "the body contains a control transfer the classifier does not understand";
    if (op.kind == M68kIrKind::write_movea) {
      const auto effect = m68k_operation_effect(op);
      if (effect.address_register_write && *effect.address_register_write == AddressRegister::a7) return "the body replaces the stack pointer (MOVEA into A7)";
    }
    for (const auto &s : n.flow.successors) {
      if (is_call_edge(s)) continue;  // the callee is another function
      work.push_back(s.target);
    }
    if (n.flow.stacked == M68kStackedContinuationKind::call_continuation) work.push_back(n.flow.stacked_address);
  }
  return {};
}

}  // namespace

bool m68k_hook_id_ok(const std::string &id) {
  if (id.empty() || id.size() > 64U) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; });
}

M68kHookResolution classify_m68k_function_hooks(const std::vector<M68kHookCodeInsn> &code, const std::vector<std::uint32_t> &roots,
                                                const std::vector<std::uint32_t> &vectors, const std::vector<M68kHookSite> &sites) {
  M68kHookResolution out;
  if (sites.size() > kM68kHookMaxSites) return reject("E_HOOK_COUNT", 0, "a build accepts at most 64 hook sites");

  // Pass 0: declarations that need no code.
  for (std::size_t i = 0; i < sites.size(); ++i) {
    const M68kHookSite &s = sites[i];
    if (!m68k_hook_id_ok(s.id)) return reject("E_HOOK_ID", i, "a hook id is a token of 1 to 64 characters");
    if (s.cpu != "m68k") return reject("E_HOOK_CPU_UNSUPPORTED", i, "function hooks exist only for the M68K (" + s.cpu + " has no succeeded feasibility spike)");
    for (std::size_t j = 0; j < i; ++j)
      if (sites[j].entry == s.entry) return reject("E_HOOK_OVERLAP", i, "two hooks claim the same site");
    if ((s.reads & ~kM68kHookReadableMask) != 0U) return reject("E_HOOK_REPLACE_CONTRACT", i, "a hook reads a register-view slot that does not exist");
    if (s.mode == M68kHookMode::replace) {
      if (!s.replace_declared) return reject("E_HOOK_REPLACE_CONTRACT", i, "a replace hook needs a replacement contract (reads, writes, flags, cost)");
      if (s.cost == 0U || s.cost > kM68kHookMaxReplaceCycles) return reject("E_HOOK_REPLACE_CONTRACT", i, "the declared replacement cost must be 1 to 65535 guest cycles");
      if ((s.writes & ~kM68kHookWritableMask) != 0U) return reject("E_HOOK_REPLACE_CONTRACT", i, "a replacement may write only D0-D7, A0-A6 and CCR");
      if (((s.writes >> kM68kHookRegCcr) & 1U) != (s.flags_written ? 1U : 0U)) return reject("E_HOOK_REPLACE_CONTRACT", i, "the flags effect disagrees with the declared writes");
    } else if (s.replace_declared || s.writes != 0U || s.cost != 0U || s.flags_written) {
      return reject("E_HOOK_REPLACE_CONTRACT", i, "only a replace hook may declare writes, flags or a cost");
    }
  }

  std::map<std::uint32_t, Node> nodes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;  // [address, address + length) of every compiled instruction
  for (const auto &c : code) {
    if (c.operation == nullptr) continue;
    Node n;
    n.operation = c.operation;
    n.address = c.operation->provenance.source.address.value;
    n.length = c.operation->provenance.length.value;
    n.alias = c.execution_alias;
    n.flow = m68k_control_successors(*c.operation);
    spans.emplace_back(n.address, n.length);
    nodes.emplace(n.address, std::move(n));
  }

  const std::set<std::uint32_t> from_reset = reachable(nodes, roots);
  const std::set<std::uint32_t> from_vectors = reachable(nodes, vectors);
  const std::set<std::uint32_t> vector_set(vectors.begin(), vectors.end());
  bool dynamic_transfer = false;
  for (const std::uint32_t a : from_reset) dynamic_transfer = dynamic_transfer || is_dynamic_transfer(nodes.at(a).flow.dynamic);

  out.sites.resize(sites.size());
  std::vector<std::set<std::uint32_t>> bodies(sites.size());
  for (std::size_t i = 0; i < sites.size(); ++i) {
    const std::uint32_t entry = sites[i].entry;
    if (entry >= UINT32_C(0x00FF0000)) return reject("E_HOOK_MUTABLE_CODE", i, "the site lies in work RAM; only immutable ROM code can be hooked");
    const auto at = nodes.find(entry);
    if (at != nodes.end() && at->second.alias) return reject("E_HOOK_MUTABLE_CODE", i, "the site executes from work RAM");
    // The build compiles an independent identity at every decodable address of the image; "inside an instruction" is therefore judged against the instructions the program actually reaches.
    const bool inside = std::any_of(spans.begin(), spans.end(), [&](const auto &s) {
      return entry > s.first && entry < s.first + s.second && (from_reset.count(s.first) != 0U || from_vectors.count(s.first) != 0U);
    });
    if (inside) return reject("E_HOOK_MID_INSTRUCTION", i, "the site is inside a reachable compiled instruction, not at its start");
    if (at == nodes.end()) return reject("E_HOOK_NOT_COMPILED", i, "the site is in code the build did not compile");
    if (vector_set.count(entry) != 0U || from_vectors.count(entry) != 0U) return reject("E_HOOK_INTERRUPT_CONTEXT", i, "the site is a vector target or reachable from an exception/interrupt handler");

    std::vector<std::uint32_t> continuations;
    bool incoming_other = false;
    for (const std::uint32_t a : from_reset) {
      const Node &n = nodes.at(a);
      for (const auto &s : n.flow.successors) {
        if (s.target != entry) continue;
        if (is_call_edge(s)) continuations.push_back(n.flow.stacked_address);
        else incoming_other = true;
      }
      if (n.flow.stacked == M68kStackedContinuationKind::call_continuation && n.flow.stacked_address == entry) incoming_other = true;  // entered by a callee's return
    }
    for (const std::uint32_t r : roots) incoming_other = incoming_other || r == entry;
    if (continuations.empty()) {
      if (incoming_other) return reject("E_HOOK_NOT_FUNCTION_ENTRY", i, "the site is reached by a branch, fallthrough or the reset entry but by no direct call");
      if (dynamic_transfer) return reject("E_HOOK_COMPUTED_TARGET", i, "no direct call reaches the site and the program has computed transfers; the site is not statically resolved");
      return reject("E_HOOK_NOT_FUNCTION_ENTRY", i, "no direct JSR/BSR in reachable code targets the site");
    }
    if (incoming_other) return reject("E_HOOK_NOT_FUNCTION_ENTRY", i, "the site is also reached by a branch or fallthrough, so it is not only a call entry");

    if (const std::string why = body_of(nodes, entry, bodies[i]); !why.empty()) return reject("E_HOOK_UNCLASSIFIED_RETURN", i, why);
    std::sort(continuations.begin(), continuations.end());
    continuations.erase(std::unique(continuations.begin(), continuations.end()), continuations.end());
    out.sites[i].continuations = std::move(continuations);
    out.sites[i].body_instructions = static_cast<std::uint32_t>(bodies[i].size());
  }

  // A site that only a replaced function (its body or any function it statically calls) can reach would silently never run: overlap.
  for (std::size_t i = 0; i < sites.size(); ++i) {
    if (sites[i].mode != M68kHookMode::replace) continue;
    const std::set<std::uint32_t> extent = reachable(nodes, {sites[i].entry});
    for (std::size_t j = 0; j < sites.size(); ++j)
      if (i != j && extent.count(sites[j].entry) != 0U) return reject("E_HOOK_OVERLAP", j, "the site lies inside the extent (body or callees) of a replaced function");
  }
  return out;
}

}  // namespace segarecomp
