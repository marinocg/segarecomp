// Function-hook declarations: register names, per-revision resolution through VERIFIED symbols, declaration-level checks (SEG-138-T004, ADR 0298; contract in
// docs/architecture/mod-execution-phases.md section 5). Pure text/number processing, no image bytes, no clock: every host produces the identical table.
// The build-time classification against compiled code (E_HOOK_NOT_FUNCTION_ENTRY, E_HOOK_UNCLASSIFIED_RETURN, ...) is the code generator's (libs/cpu/m68k/function_hooks.cpp).
#include "segarecomp/mod.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <set>

namespace segarecomp::mod {

namespace {

constexpr std::uint32_t kReadableMask = 0xFF03FFFFU, kWritableMask = 0x00017FFFU;   // mirrors GENESIS_HOOK_READABLE_MASK / _WRITABLE_MASK (a test pins the equality)
constexpr std::uint32_t kMaxReplaceCycles = 65535;

bool id_ok(const std::string &id) { return symbol_name_ok(id); }

bool known_mode(const std::string &mode) { return std::any_of(std::begin(kHookModes), std::end(kHookModes), [&](std::string_view m) { return m == mode; }); }

std::uint32_t mask_of(const std::vector<std::string> &names, bool writable) {
  std::uint32_t mask = 0;
  for (const std::string &n : names) mask |= 1U << *hook_register_bit(n, writable);
  return mask;
}

bool sorted_unique(const std::vector<std::string> &v) {
  for (std::size_t i = 1; i < v.size(); ++i)
    if (!(*hook_register_bit(v[i - 1], false) < *hook_register_bit(v[i], false))) return false;
  return true;
}

std::optional<Rejection> check_one(const Manifest &m, const Hook &h) {
  if (!id_ok(h.id)) return Rejection{"E_HOOK_ID", "a hook id is a token of 1 to 64 characters"};
  if (!known_mode(h.mode)) return Rejection{"E_HOOK_MODE", "the hook \"" + h.id + "\" has an unknown mode (before, after, before-after or replace)"};
  const std::string cpu = h.cpu.empty() ? "m68k" : h.cpu;
  if (m.platform == "sega-cd") return Rejection{"E_HOOK_OVERLAY_UNRESOLVED", "Sega CD code lives in modules and overlays that no source identity verifies; the hook \"" + h.id + "\" cannot be resolved"};
  if (m.platform != "genesis" || cpu != "m68k") return Rejection{"E_HOOK_CPU_UNSUPPORTED", "function hooks exist only for the Genesis M68K; the hook \"" + h.id + "\" targets " + m.platform + "/" + cpu};
  for (const std::string &n : h.reads)
    if (!hook_register_bit(n, false)) return Rejection{"E_HOOK_REGISTER", "the hook \"" + h.id + "\" reads \"" + n + "\", which is not a register-view name"};
  for (const std::string &n : h.writes)
    if (!hook_register_bit(n, true)) return Rejection{"E_HOOK_REGISTER", "the hook \"" + h.id + "\" writes \"" + n + "\", which is not a writable register-view name (d0..d7, a0..a6, ccr)"};
  if (!sorted_unique(h.reads) || !sorted_unique(h.writes)) return Rejection{"E_FIELD_VALUE", "hook register lists must be sorted in register-view order without duplicates"};
  if (h.mode == "replace") {
    if (!h.has_replace) return Rejection{"E_HOOK_REPLACE_CONTRACT", "the replace hook \"" + h.id + "\" has no replacement contract (writes and cost)"};
    if (h.cost == 0 || h.cost > kMaxReplaceCycles) return Rejection{"E_HOOK_REPLACE_CONTRACT", "the replacement cost of \"" + h.id + "\" must be 1 to 65535 guest cycles"};
  } else if (h.has_replace || !h.writes.empty() || h.cost != 0) {
    return Rejection{"E_HOOK_REPLACE_CONTRACT", "only a replace hook may declare writes or a cost (\"" + h.id + "\")"};
  }
  return std::nullopt;
}

ResolvedHook resolve_one(const Hook &h, std::uint32_t entry) {
  ResolvedHook r;
  r.id = h.id;
  r.cpu = h.cpu.empty() ? "m68k" : h.cpu;
  r.mode = h.mode;
  r.entry = entry;
  r.reads = mask_of(h.reads, false);
  r.writes = mask_of(h.writes, true);
  r.cost = h.cost;
  r.replace_declared = h.has_replace;
  r.flags_written = (r.writes >> 16U & 1U) != 0U;
  return r;
}

}  // namespace

std::optional<std::uint32_t> hook_register_bit(std::string_view name, bool writable) {
  std::uint32_t bit = 0;
  const auto digit = [&](std::string_view s, unsigned max) -> int { return s.size() == 1 && s[0] >= '0' && s[0] < static_cast<char>('0' + max + 1) ? s[0] - '0' : -1; };
  if (name.size() == 2 && name[0] == 'd' && digit(name.substr(1), 7) >= 0) bit = static_cast<std::uint32_t>(digit(name.substr(1), 7));
  else if (name.size() == 2 && name[0] == 'a' && digit(name.substr(1), 7) >= 0) bit = 8U + static_cast<std::uint32_t>(digit(name.substr(1), 7));
  else if (name == "ccr") bit = 16;
  else if (name == "cycles") bit = 17;
  else if (name.size() == 6 && name.substr(0, 5) == "stack" && digit(name.substr(5), 7) >= 0) bit = 24U + static_cast<std::uint32_t>(digit(name.substr(5), 7));
  else return std::nullopt;
  const std::uint32_t mask = writable ? kWritableMask : kReadableMask;
  if (((mask >> bit) & 1U) == 0U) return std::nullopt;
  return bit;
}

ResolvedHooks resolve_hooks(const Manifest &m, std::string_view revision) {
  ResolvedHooks out;
  out.revision = std::string(revision);
  for (const Hook &h : m.hooks) {
    if (!h.revision.empty() && h.revision != revision) continue;
    Rejection why;
    const std::optional<std::uint32_t> address = verified_symbol(m, h.symbol, std::string(revision), why);
    if (!address) { out.rejection = why; return out; }
    out.hooks.push_back(resolve_one(h, *address));
  }
  out.ok = true;
  return out;
}

std::optional<Rejection> check_hooks(const Manifest &m) {
  if (m.hooks.empty()) return std::nullopt;
  if (m.hooks.size() > kMaxHooks) return Rejection{"E_HOOK_COUNT", "a package declares at most 64 hooks"};
  std::set<std::string> ids;
  for (std::size_t i = 0; i < m.hooks.size(); ++i) {
    const Hook &h = m.hooks[i];
    if (auto r = check_one(m, h)) return r;
    if (!ids.insert(h.id).second) return Rejection{"E_HOOK_OVERLAP", "the hook id \"" + h.id + "\" is declared twice"};
    if (i > 0 && !(m.hooks[i - 1].id < h.id)) return Rejection{"E_FIELD_VALUE", "hooks must be sorted by id"};
    if (!h.revision.empty() && std::none_of(m.revisions.begin(), m.revisions.end(), [&](const Revision &r) { return r.label == h.revision; }))
      return Rejection{"E_HOOK_REVISION", "the hook \"" + h.id + "\" names a revision that is not declared"};
  }
  for (const Revision &rev : m.revisions) {   // every declared revision resolves independently
    const ResolvedHooks resolved = resolve_hooks(m, rev.label);
    if (!resolved.ok) return resolved.rejection;
    std::set<std::uint32_t> sites;
    for (const ResolvedHook &r : resolved.hooks) {
      if ((r.entry & 1U) != 0U) return Rejection{"E_HOOK_MID_INSTRUCTION", "the hook \"" + r.id + "\" resolves to an odd address, which cannot start an M68K instruction (revision " + rev.label + ")"};
      if (!sites.insert(r.entry).second) return Rejection{"E_HOOK_OVERLAP", "two hooks resolve to the same site in revision " + rev.label + " (\"" + r.id + "\")"};
    }
  }
  return std::nullopt;
}

ResolvedHooks resolve_hooks_for_image(const Manifest &m, std::span<const std::uint8_t> image) {
  const std::string digest = sha256_hex(image);
  for (const Revision &rev : m.revisions)
    if (rev.sha256 == digest) return resolve_hooks(m, rev.label);
  ResolvedHooks out;
  out.rejection = {"E_HOOK_DIGEST", "the image is not one of the declared target revisions, so no hook site can be resolved"};
  return out;
}

std::string hook_table_text(const ResolvedHooks &r) {
  if (!r.ok || r.hooks.empty()) return {};
  std::string text = "segarecomp.mod.hooks.v1\nrevision " + r.revision + "\n";
  for (const ResolvedHook &h : r.hooks)
    text += "hook " + h.id + " " + h.cpu + " " + h.mode + " entry " + std::to_string(h.entry) + " reads " + std::to_string(h.reads) + " writes " + std::to_string(h.writes) + " cost " + std::to_string(h.cost) + "\n";
  return text;
}

}  // namespace segarecomp::mod
