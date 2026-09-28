#pragma once

// SEG-024-T001 (experiment, report-only): CPU-neutral executable-support fixed point.
//
// Question answered: given a conservative universe U of independently valid executable identities
// (for example Gen-2's broad immutable-ROM AOT candidates), which identities can architectural control
// flow actually select?  The answer L is the LEAST set closed under
//
//   * trusted roots,
//   * fixed successors of live identities (fallthrough, direct branch/call, conditional outcomes,
//     call-return continuations, exception continuations), and
//   * the target domain D(s) of every dynamic-control site s owned by a live identity.
//
// Safety rules enforced by construction:
//   * unknown -> KEEP: a site with no stronger proof must use `TargetDomainKind::any_candidate`, whose
//     membership is the whole universe U.
//   * Target domains are inputs, formed BEFORE the fixed point from CPU/memory/architecture evidence.
//     The fixed point never writes a domain, and `narrow_target_domain` rejects any narrowing whose
//     evidence is derived from a live-set result (anti-circularity) or that is not a subset of the
//     previous domain.
//   * A cycle is not self-justifying: L is the least fixed point from roots, so a disconnected cycle
//     stays out of L.
//   * Deterministic: universe, roots, successors and domain members are sorted; the worklist is
//     processed in rounds of ascending identity; no unordered container is traversed.
//
// Identities are opaque, totally ordered 64-bit values. A CPU/machine layer owns their packing
// (address space, bank, alignment); nothing here assumes even alignment, a flat address space, or a
// particular CPU's PC-effect kinds.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace segarecomp {

using ExecutableIdentity = std::uint64_t;

enum class SupportSourceKind : std::uint8_t {
  root,
  fallthrough,
  direct_branch,
  direct_call,
  conditional_outcome,
  return_continuation,
  exception_continuation,
  dynamic_domain,
};
inline constexpr std::size_t support_source_kind_count = 8U;

enum class TargetDomainKind : std::uint8_t {
  exact_set,               // finite proven members
  return_continuation,     // an existing return-target authority (finite)
  exception_continuation,  // an existing exception-return authority (finite)
  bounded_region,          // proven half-open identity intervals
  any_candidate,           // no stronger proof: the whole universe (unknown -> KEEP)
};
inline constexpr std::size_t target_domain_kind_count = 5U;

// Where a domain's justification comes from. `live_set_derived` exists only so that the rejection of
// circular narrowing is explicit and testable: it is never accepted.
enum class DomainEvidence : std::uint8_t {
  none,
  cpu_semantics,
  architectural_operand_width,
  immutable_memory_provenance,
  existing_runtime_authority,
  live_set_derived,
};

struct IdentityInterval {
  ExecutableIdentity begin{};  // inclusive
  ExecutableIdentity end{};    // exclusive
};

struct TargetDomain {
  TargetDomainKind kind{TargetDomainKind::any_candidate};
  DomainEvidence evidence{DomainEvidence::none};
  std::vector<ExecutableIdentity> members;   // exact_set / *_continuation (sorted, unique)
  std::vector<IdentityInterval> intervals;   // bounded_region (sorted, non-overlapping)
};

struct SupportEdge {
  SupportSourceKind kind{SupportSourceKind::fallthrough};
  ExecutableIdentity target{};
};

struct DynamicControlSite {
  ExecutableIdentity owner{};   // the candidate whose liveness activates the site
  std::uint32_t family{};       // caller-defined generic control family index (for attribution)
  std::uint32_t domain{};       // index into ExecutableSupportInput::domains
};

struct SupportCandidate {
  ExecutableIdentity identity{};
  std::vector<SupportEdge> successors;
};

struct ExecutableSupportInput {
  std::vector<SupportCandidate> universe;    // sorted by identity, unique
  std::vector<ExecutableIdentity> roots;
  std::vector<TargetDomain> domains;
  std::vector<DynamicControlSite> sites;
  std::uint32_t family_count{};
};

// Diagnostic-only switches. Any `ignore_*` switch makes the result an UNSOUND ablation used only to
// attribute live-set growth; `ExecutableSupportResult::sound` is false whenever one is set.
struct ExecutableSupportOptions {
  std::array<bool, target_domain_kind_count> ignore_domain_kind{};
  std::vector<bool> ignore_family;  // indexed by family; missing entries mean "keep"
};

struct ExecutableSupportResult {
  bool sound{true};
  std::vector<ExecutableIdentity> live;  // sorted
  // First support source that made each live identity live (parallel to `live`).
  std::vector<SupportSourceKind> first_reason;
  std::array<std::uint64_t, support_source_kind_count> first_reason_counts{};
  std::array<std::uint64_t, target_domain_kind_count> live_sites_by_domain_kind{};
  std::array<std::uint64_t, target_domain_kind_count> sites_by_domain_kind{};
  std::vector<std::uint64_t> live_sites_by_family;
  std::vector<std::uint64_t> sites_by_family;
  // Candidates first made live by a dynamic domain, split by that domain's kind / site family.
  std::array<std::uint64_t, target_domain_kind_count> first_live_by_domain_kind{};
  std::vector<std::uint64_t> first_live_by_family;
  std::uint64_t rounds{};
  // Typed inconsistencies: a root or fixed successor that has no identity in U. Never invented.
  std::uint64_t roots_outside_universe{};
  std::array<std::uint64_t, support_source_kind_count> successors_outside_universe{};
  std::uint64_t domain_members_outside_universe{};
};

namespace detail {
inline bool identity_in_domain(const TargetDomain &domain, ExecutableIdentity identity) {
  switch (domain.kind) {
  case TargetDomainKind::any_candidate:
    return true;
  case TargetDomainKind::bounded_region:
    for (const auto &interval : domain.intervals)
      if (identity >= interval.begin && identity < interval.end) return true;
    return false;
  default:
    return std::binary_search(domain.members.begin(), domain.members.end(), identity);
  }
}
}  // namespace detail

// True when every identity of `candidate` (restricted to `universe`) is also in `previous`.
[[nodiscard]] inline bool target_domain_subset_of(const TargetDomain &candidate, const TargetDomain &previous,
                                                  const std::vector<ExecutableIdentity> &universe) {
  if (previous.kind == TargetDomainKind::any_candidate) return true;
  if (candidate.kind == TargetDomainKind::any_candidate)
    return std::all_of(universe.begin(), universe.end(),
                       [&](ExecutableIdentity id) { return detail::identity_in_domain(previous, id); });
  if (candidate.kind == TargetDomainKind::bounded_region) {
    for (const auto id : universe)
      if (detail::identity_in_domain(candidate, id) && !detail::identity_in_domain(previous, id)) return false;
    return true;
  }
  return std::all_of(candidate.members.begin(), candidate.members.end(),
                     [&](ExecutableIdentity id) { return detail::identity_in_domain(previous, id); });
}

// The only accepted way to replace a domain with a narrower one. Returns `previous` unchanged (the
// candidate is KEPT in the broader domain) when the proposal lacks independent evidence, is justified by
// a live-set result, or is not a subset of the previous domain.
[[nodiscard]] inline TargetDomain narrow_target_domain(const TargetDomain &previous, const TargetDomain &proposed,
                                                       const std::vector<ExecutableIdentity> &universe) {
  if (proposed.evidence == DomainEvidence::none || proposed.evidence == DomainEvidence::live_set_derived)
    return previous;
  if (!target_domain_subset_of(proposed, previous, universe)) return previous;
  return proposed;
}

[[nodiscard]] inline ExecutableSupportResult compute_executable_support(const ExecutableSupportInput &input,
                                                                        const ExecutableSupportOptions &options = {}) {
  ExecutableSupportResult result{};
  const auto &universe = input.universe;
  const std::size_t n = universe.size();
  result.live_sites_by_family.assign(input.family_count, 0U);
  result.sites_by_family.assign(input.family_count, 0U);
  result.first_live_by_family.assign(input.family_count, 0U);
  for (const auto kind : options.ignore_domain_kind)
    if (kind) result.sound = false;
  for (const auto family : options.ignore_family)
    if (family) result.sound = false;

  const auto index_of = [&](ExecutableIdentity id) -> std::size_t {
    const auto found = std::lower_bound(universe.begin(), universe.end(), id,
                                        [](const SupportCandidate &c, ExecutableIdentity v) { return c.identity < v; });
    return found != universe.end() && found->identity == id ? static_cast<std::size_t>(found - universe.begin()) : n;
  };

  // Sites grouped by owner index (stable, deterministic: input order within an owner).
  std::vector<std::vector<std::uint32_t>> sites_of(n);
  for (std::uint32_t s = 0; s < input.sites.size(); ++s) {
    const auto &site = input.sites[s];
    const auto owner = index_of(site.owner);
    if (owner == n) continue;
    sites_of[owner].push_back(s);
    ++result.sites_by_domain_kind[static_cast<std::size_t>(input.domains[site.domain].kind)];
    if (site.family < input.family_count) ++result.sites_by_family[site.family];
  }
  for (const auto &domain : input.domains)
    for (const auto member : domain.members)
      if (index_of(member) == n) ++result.domain_members_outside_universe;

  std::vector<std::uint8_t> live(n, 0U);
  std::vector<SupportSourceKind> reason(n, SupportSourceKind::root);
  std::vector<std::uint8_t> domain_activated(input.domains.size(), 0U);
  std::vector<std::size_t> frontier;
  std::vector<std::size_t> next;

  struct Attribution { SupportSourceKind kind; std::size_t domain_kind; std::uint32_t family; bool dynamic; };
  const auto mark = [&](std::size_t index, const Attribution &why) {
    if (live[index] != 0U) return;
    live[index] = 1U;
    reason[index] = why.kind;
    if (why.dynamic) {
      ++result.first_live_by_domain_kind[why.domain_kind];
      if (why.family < input.family_count) ++result.first_live_by_family[why.family];
    }
    next.push_back(index);
  };

  for (const auto root : input.roots) {
    const auto index = index_of(root);
    if (index == n) { ++result.roots_outside_universe; continue; }
    mark(index, {SupportSourceKind::root, 0U, 0U, false});
  }
  const auto ignored = [&](const DynamicControlSite &site) {
    const auto kind = static_cast<std::size_t>(input.domains[site.domain].kind);
    if (options.ignore_domain_kind[kind]) return true;
    return site.family < options.ignore_family.size() && options.ignore_family[site.family];
  };

  while (!next.empty()) {
    ++result.rounds;
    std::sort(next.begin(), next.end());
    frontier.swap(next);
    next.clear();
    for (const auto index : frontier) {
      for (const auto &edge : universe[index].successors) {
        const auto target = index_of(edge.target);
        if (target == n) { ++result.successors_outside_universe[static_cast<std::size_t>(edge.kind)]; continue; }
        mark(target, {edge.kind, 0U, 0U, false});
      }
      for (const auto s : sites_of[index]) {
        const auto &site = input.sites[s];
        const auto &domain = input.domains[site.domain];
        // Live-site counts include ablated sites: they answer "which unresolved sites are selectable".
        ++result.live_sites_by_domain_kind[static_cast<std::size_t>(domain.kind)];
        if (site.family < input.family_count) ++result.live_sites_by_family[site.family];
        if (ignored(site)) continue;
        if (domain_activated[site.domain] != 0U) continue;
        domain_activated[site.domain] = 1U;
        const Attribution why{SupportSourceKind::dynamic_domain, static_cast<std::size_t>(domain.kind), site.family, true};
        switch (domain.kind) {
        case TargetDomainKind::any_candidate:
          for (std::size_t i = 0; i < n; ++i) mark(i, why);
          break;
        case TargetDomainKind::bounded_region:
          for (const auto &interval : domain.intervals) {
            auto it = std::lower_bound(universe.begin(), universe.end(), interval.begin,
                                       [](const SupportCandidate &c, ExecutableIdentity v) { return c.identity < v; });
            for (; it != universe.end() && it->identity < interval.end; ++it)
              mark(static_cast<std::size_t>(it - universe.begin()), why);
          }
          break;
        default:
          for (const auto member : domain.members) {
            const auto target = index_of(member);
            if (target != n) mark(target, why);
          }
          break;
        }
      }
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (live[i] == 0U) continue;
    result.live.push_back(universe[i].identity);
    result.first_reason.push_back(reason[i]);
    ++result.first_reason_counts[static_cast<std::size_t>(reason[i])];
  }
  return result;
}

// Deterministic FNV-1a-64 digest over a sorted identity list (little-endian 8-byte identities).
[[nodiscard]] inline std::uint64_t executable_identity_digest(const std::vector<ExecutableIdentity> &identities) {
  std::uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (const auto id : identities)
    for (unsigned byte = 0; byte < 8U; ++byte) {
      hash ^= (id >> (8U * byte)) & 0xFFU;
      hash *= UINT64_C(0x100000001b3);
    }
  return hash;
}

[[nodiscard]] inline const char *support_source_kind_name(SupportSourceKind kind) {
  switch (kind) {
  case SupportSourceKind::root: return "root";
  case SupportSourceKind::fallthrough: return "fallthrough";
  case SupportSourceKind::direct_branch: return "direct_branch";
  case SupportSourceKind::direct_call: return "direct_call";
  case SupportSourceKind::conditional_outcome: return "conditional_outcome";
  case SupportSourceKind::return_continuation: return "return_continuation";
  case SupportSourceKind::exception_continuation: return "exception_continuation";
  case SupportSourceKind::dynamic_domain: return "dynamic_domain";
  }
  return "unknown";
}

[[nodiscard]] inline const char *target_domain_kind_name(TargetDomainKind kind) {
  switch (kind) {
  case TargetDomainKind::exact_set: return "exact_set";
  case TargetDomainKind::return_continuation: return "return_continuation";
  case TargetDomainKind::exception_continuation: return "exception_continuation";
  case TargetDomainKind::bounded_region: return "bounded_region";
  case TargetDomainKind::any_candidate: return "any_candidate";
  }
  return "unknown";
}

}  // namespace segarecomp
