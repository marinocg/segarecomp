// Ordered mod sets (SEG-141, ADR 0326): definition, direct compatibility check, verified composition and composite identity. Pure functions; no file access, no compiler.
#include "segarecomp/mod_set.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace segarecomp::mod {

namespace {

std::string digest_of(std::string_view text) { return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}); }

std::string hex_text(std::uint64_t v) {
  std::string out;
  do { out.insert(out.begin(), "0123456789abcdef"[v & 15U]); v >>= 4U; } while (v != 0);
  return out;
}

std::string named(const std::string &id) { return "\"" + id + "\""; }

bool has_payload(const Manifest &m, std::string_view kind) {
  return std::any_of(m.payloads.begin(), m.payloads.end(), [&](const Payload &p) { return p.kind == kind; });
}

bool changes_image(const Manifest &m) { return has_target_edits(m) || has_payload(m, "bps-image"); }

const Revision *revision_for(const Manifest &m, const std::string &sha256) {
  for (const Revision &r : m.revisions) if (r.sha256 == sha256) return &r;
  return nullptr;
}

SetDefinitionResult bad_definition(std::string code, std::string message) {
  SetDefinitionResult r;
  r.rejection = {std::move(code), std::move(message)};
  return r;
}

}  // namespace

bool set_name_ok(std::string_view name) {
  if (name.empty() || name.size() > 64 || name.front() == '-' || name.back() == '-') return false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '-') { if (name[i - 1] == '-') return false; continue; }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

std::string serialize_set_definition(const SetDefinition &d) {
  std::string out = "segarecomp-modset=" + std::to_string(kSetDefinitionFormat) + "\nplatform=" + d.platform + "\n";
  for (const std::string &m : d.members) out += "member=" + m + "\n";
  return out;
}

SetDefinitionResult parse_set_definition(std::string_view text) {
  if (text.size() > kMaxSetDefinitionBytes) return bad_definition("E_SET_SYNTAX", "the mod set definition is larger than allowed");
  const std::vector<VariantComponent> lines = parse_components(text);   // closed "name=value\n" form; anything malformed yields nothing
  if (lines.empty() || lines.front().name != "segarecomp-modset") return bad_definition("E_SET_SYNTAX", "this is not a mod set definition");
  if (lines.front().value != std::to_string(kSetDefinitionFormat)) return bad_definition("E_SET_VERSION", "the mod set definition has a format this release does not read");
  if (lines.size() < 2 || lines[1].name != "platform") return bad_definition("E_SET_SYNTAX", "a mod set definition names its platform first");
  SetDefinitionResult out;
  out.definition.platform = lines[1].value;
  if (find_adapter(out.definition.platform) == nullptr) return bad_definition("E_PLATFORM_UNKNOWN", "the mod set names a platform that has no registered adapter");
  for (std::size_t i = 2; i < lines.size(); ++i) {
    const std::string &v = lines[i].value;
    if (lines[i].name != "member" || v.size() != 64 || v.find_first_not_of("0123456789abcdef") != std::string::npos) return bad_definition("E_SET_SYNTAX", "a mod set member is the 64 digit identity of a package");
    if (std::find(out.definition.members.begin(), out.definition.members.end(), v) != out.definition.members.end()) return bad_definition("E_SET_DUPLICATE_MOD", "a mod set names a package twice");
    out.definition.members.push_back(v);
  }
  if (out.definition.members.empty() || out.definition.members.size() > kMaxSetMembers) return bad_definition("E_SET_SIZE", "a mod set holds 1 to 16 mods");
  out.ok = true;
  return out;
}

VerifiedPackage verify_package_bytes(std::vector<std::uint8_t> bytes, std::string_view identity) {
  VerifiedPackage out;
  out.package = read_package(bytes);
  if (!out.package.ok) out.rejection = out.package.rejection;
  else if (out.package.identity != identity) out.rejection = {"E_PACKAGE_CHANGED", "the installed package no longer has the identity " + std::string(identity)};
  else { out.ok = true; out.bytes = std::move(bytes); return out; }
  out.package = Package{};
  return out;
}

std::vector<SetViolation> check_set_relations(std::span<const Manifest *const> members) {
  std::vector<SetViolation> out;
  if (members.empty() || members.size() > kMaxSetMembers) {
    out.push_back({"E_SET_SIZE", {}, "a mod set holds 1 to 16 mods"});
    return out;
  }
  for (std::size_t i = 0; i < members.size(); ++i) {
    const Manifest &m = *members[i];
    for (std::size_t j = 0; j < i; ++j)
      if (members[j]->id == m.id) {
        out.push_back({"E_SET_DUPLICATE_MOD", {m.id}, "the set holds the mod " + named(m.id) + " twice (versions " + members[j]->version + " and " + m.version + ")"});
        break;
      }
    if (m.platform != members.front()->platform)
      out.push_back({"E_SET_PLATFORM", {m.id, members.front()->id}, "the mod " + named(m.id) + " is for " + m.platform + " but " + named(members.front()->id) + " is for " + members.front()->platform});
  }
  const auto range_text = [](const ModRelation &r) { return r.version.empty() ? std::string("any version") : r.version; };
  for (const Manifest *member : members) {
    const Manifest &m = *member;
    for (const ModRelation &d : m.dependencies) {
      std::string found;
      bool satisfied = false;
      for (const Manifest *other : members) {
        if (other->id != d.id) continue;
        satisfied = satisfied || version_satisfies(other->version, d.version);
        found += (found.empty() ? "" : ", ") + other->version;
      }
      if (satisfied) continue;
      if (found.empty()) out.push_back({"E_SET_MISSING_DEPENDENCY", {m.id, d.id}, "the mod " + named(m.id) + " requires " + named(d.id) + " (" + range_text(d) + "), which is not in the set"});
      else out.push_back({"E_SET_VERSION_MISMATCH", {m.id, d.id}, "the mod " + named(m.id) + " requires " + named(d.id) + " " + range_text(d) + ", but the set holds version " + found});
    }
    for (const ModRelation &x : m.incompatibilities)
      for (const Manifest *other : members)
        if (other->id == x.id && version_satisfies(other->version, x.version))
          out.push_back({"E_SET_INCOMPATIBLE", {m.id, x.id}, "the mod " + named(m.id) + " is incompatible with " + named(x.id) + " (" + range_text(x) + "); the set holds version " + other->version});
  }
  return out;
}

std::optional<Rejection> check_standalone(const Manifest &manifest) {
  const Manifest *one = &manifest;
  const std::vector<SetViolation> violations = check_set_relations({&one, 1});
  if (violations.empty()) return std::nullopt;
  return Rejection{violations.front().code, violations.front().message};
}

std::string source_identity_text(const SetSource &s) {
  if (s.kind == "disc-image") return "disc=" + s.disc_identity + ";bios=" + s.bios_requirement;
  return "image=" + s.image_sha256 + ";mapper=" + (s.mapper.empty() ? std::string("none") : s.mapper);
}

std::string adapter_capability_digest(const Adapter &adapter) {
  std::string text;
  for (const CapabilityInfo &c : capability_catalogue()) text += std::string(c.name) + "=" + support_name(capability_support(adapter, c)) + "\n";
  return digest_of(text);
}

std::vector<SetOverlap> find_overlaps(std::span<const SetClaim> claims) {
  std::vector<const SetClaim *> sorted;
  for (const SetClaim &c : claims) if (c.start < c.end) sorted.push_back(&c);
  std::sort(sorted.begin(), sorted.end(), [](const SetClaim *x, const SetClaim *y) { return std::tie(x->space, x->start, x->end, x->member, x->kind) < std::tie(y->space, y->start, y->end, y->member, y->kind); });
  // key: (first member, second member, space, first writes, second writes). A sweep over the sorted claims, so the work follows the claims that actually touch.
  std::map<std::tuple<std::size_t, std::size_t, std::string, bool, bool>, SetOverlap> found;
  for (std::size_t i = 0; i < sorted.size(); ++i)
    for (std::size_t j = i + 1; j < sorted.size() && sorted[j]->space == sorted[i]->space && sorted[j]->start < sorted[i]->end; ++j) {
      const SetClaim *a = sorted[i], *b = sorted[j];
      if (a->member == b->member || (a->kind != "write" && b->kind != "write")) continue;
      if (b->member < a->member) std::swap(a, b);
      const std::uint64_t start = std::max(a->start, b->start), end = std::min(a->end, b->end);
      const auto key = std::make_tuple(a->member, b->member, a->space, a->kind == "write", b->kind == "write");
      const auto it = found.find(key);
      if (it == found.end()) { found.emplace(key, SetOverlap{*a, *b, start, end, 1}); continue; }
      ++it->second.ranges;
      if (std::tie(start, end) < std::tie(it->second.start, it->second.end)) { it->second.first = *a; it->second.second = *b; it->second.start = start; it->second.end = end; }
    }
  std::vector<SetOverlap> out;
  for (auto &entry : found) out.push_back(std::move(entry.second));
  std::stable_sort(out.begin(), out.end(), [](const SetOverlap &x, const SetOverlap &y) {
    return std::tie(x.first.member, x.second.member, x.first.space, x.start, x.end) < std::tie(y.first.member, y.second.member, y.first.space, y.start, y.end);
  });
  return out;
}

SetComposition compose_set(std::span<const Package *const> members, std::span<const std::uint8_t> original, GuestAdmitter admit) {
  SetComposition out;
  std::vector<const Manifest *> manifests;
  for (const Package *p : members) manifests.push_back(&p->manifest);
  out.violations = check_set_relations(manifests);
  for (const SetViolation &v : out.violations)
    if (v.code == "E_SET_SIZE" || v.code == "E_SET_PLATFORM") return out;   // there is no common platform to compose on
  const std::string platform = manifests.front()->platform;
  out.original_sha256 = sha256_hex(original);
  out.source.kind = "cartridge-image";
  out.source.image_sha256 = out.original_sha256;

  // one exact source: every member declares this image, with one mapper configuration
  bool source_ok = true;
  const std::string *mapper_of = nullptr;
  for (const Manifest *m : manifests) {
    const Revision *r = revision_for(*m, out.original_sha256);
    if (r == nullptr) {
      out.violations.push_back({"E_SET_SOURCE", {m->id}, "the mod " + named(m->id) + " does not declare this exact original image"});
      source_ok = false;
    } else if (mapper_of == nullptr) {
      out.source.mapper = r->mapper;
      mapper_of = &m->id;
    } else if (r->mapper != out.source.mapper) {
      out.violations.push_back({"E_SET_SOURCE", {*mapper_of, m->id}, "the mods " + named(*mapper_of) + " and " + named(m->id) + " declare different cartridge mappers for this image"});
      source_ok = false;
    }
  }
  if (!source_ok) return out;

  // every member ALONE against the unmodified original: assertions, free ranges, admission, header policy
  std::vector<EffectiveImage> alone;
  std::vector<TargetImage> targets(members.size());
  bool members_ok = true;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const Package &p = *members[i];
    alone.push_back(effective_image(p, original, admit));
    if (alone.back().ok && has_target_edits(p.manifest)) {
      targets[i] = build_target_image(p.manifest, p.files, original, admit);
      if (!targets[i].ok) { alone.back().ok = false; alone.back().rejection = targets[i].rejection; }
    }
    if (!alone.back().ok) {
      std::string message = "the mod " + named(p.manifest.id) + " does not apply to the original image: " + alone.back().rejection.message;
      if (alone.back().rejection.code == "E_EDIT_EXPECTED_MISMATCH") message += " (expected values are asserted against the unmodified original; a mod cannot depend on bytes another mod writes)";
      out.violations.push_back({alone.back().rejection.code, {p.manifest.id}, message});
      members_ok = false;
    }
    out.member_effective.push_back(alone.back().effective_sha256);
  }
  if (!members_ok) return out;

  // claims: what each member writes, and the entry instruction word of each function it hooks
  std::vector<bool> hooks(members.size(), false);
  struct Hooked { std::size_t member; std::string cpu; std::uint32_t entry; bool replaces; };
  std::vector<Hooked> hooked;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const Manifest &m = *manifests[i];
    std::string cpu;
    for (const Payload &p : m.payloads) if (p.kind == "guest-asm") cpu = p.cpu;
    for (const Range &r : targets[i].written) out.claims.push_back({i, "cartridge-image", cpu, "write", r.start, r.end});
    if (!m.hooks.empty()) {
      const ResolvedHooks resolved = resolve_hooks(m, revision_for(m, out.original_sha256)->label);
      if (!resolved.ok) {
        out.violations.push_back({resolved.rejection.code, {m.id}, "the mod " + named(m.id) + ": " + resolved.rejection.message});
        return out;
      }
      for (const ResolvedHook &h : resolved.hooks) {
        out.claims.push_back({i, "cartridge-image", h.cpu, "hook-site", h.entry, static_cast<std::uint64_t>(h.entry) + 2U});
        hooks[i] = true;
        hooked.push_back({i, h.cpu, h.entry, h.mode == "replace"});
      }
    }
    if (platform == "genesis" && cpu == "z80") {   // the sound CPU runs these blocks from its RAM: another member's code at the same address is a conflict even when the cartridge bytes are disjoint
      const GuestListingResult l = guest_listing(m, members[i]->files, revision_for(m, out.original_sha256)->label);
      if (!l.ok) { out.violations.push_back({l.rejection.code, {m.id}, "the mod " + named(m.id) + ": " + l.rejection.message}); return out; }
      for (const GuestBlock &b : l.listing.blocks) out.claims.push_back({i, "z80.sound_ram", cpu, "write", b.address, static_cast<std::uint64_t>(b.address) + b.bytes.size()});
    }
  }

  // a bps-image declares no write ranges and was made from the original: it cannot share the image with another change, and no hook site can be shown to survive it
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (!has_payload(*manifests[i], "bps-image")) continue;
    for (std::size_t j = 0; j < members.size(); ++j) {
      if (j == i || (!changes_image(*manifests[j]) && !hooks[j]) || (j < i && has_payload(*manifests[j], "bps-image"))) continue;
      out.violations.push_back({"E_SET_BPS_CONFLICT", {manifests[i]->id, manifests[j]->id},
                                "the mod " + named(manifests[i]->id) + " is a whole-image BPS patch, which cannot be composed with " + named(manifests[j]->id) +
                                    (changes_image(*manifests[j]) ? ", which also changes the image" : ", whose hook sites cannot be verified in the patched image") + "; BPS ranges are never merged (use typed ROM edits)"});
    }
  }

  // exclusive hook ownership, per CPU and site (ADR 0327): a replacement admits no other member at that entry; observing (before / after) hooks of several members may share it. Hooks exist only
  // where the platform adapter supports them (a member of another platform that declares one was already refused when it was built alone).
  for (std::size_t a = 0; a < hooked.size(); ++a)
    for (std::size_t b = a + 1; b < hooked.size(); ++b) {
      const Hooked &x = hooked[a], &y = hooked[b];
      if (x.member == y.member || x.cpu != y.cpu || x.entry != y.entry || (!x.replaces && !y.replaces)) continue;
      const std::string &first = manifests[x.member]->id, &second = manifests[y.member]->id;
      out.violations.push_back({"E_SET_HOOK_OWNER", {first, second}, "the mods " + named(first) + " and " + named(second) + " both hook the " + x.cpu + " function at " + image_location(platform, x.entry) +
                                                                         " and a replacement owns its function exclusively; only before/after hooks of several mods can share a site"});
    }

  const std::vector<SetOverlap> overlaps = find_overlaps(out.claims);
  for (const SetOverlap &o : overlaps) {
    const std::string &a = manifests[o.first.member]->id, &b = manifests[o.second.member]->id;
    const std::string where = (o.first.space == "cartridge-image" ? image_location(platform, o.start) : o.first.space + " address 0x" + hex_text(o.start)) + ", " + std::to_string(o.end - o.start) + " byte(s)" +
                              (o.ranges > 1 ? " (the first of " + std::to_string(o.ranges) + " overlapping ranges)" : "");
    if (o.first.kind == "write" && o.second.kind == "write") {
      out.violations.push_back({"E_SET_EDIT_OVERLAP", {a, b}, "the mods " + named(a) + " and " + named(b) + " both write " + where + "; overlapping changes are rejected, never merged"});
    } else {
      const bool first_writes = o.first.kind == "write";
      const std::string &writer = first_writes ? a : b, &hooker = first_writes ? b : a;
      out.violations.push_back({"E_SET_HOOK_SITE", {writer, hooker}, "the mod " + named(writer) + " writes the entry of a function that " + named(hooker) + " hooks (" + where + ")"});
    }
  }

  // Master System: the CPU owner admitted each member's injected code alone; two members' blocks must also be admitted TOGETHER (a control target of one may not land inside, or in the padding of, the other's code)
  if (platform == "master-system" && overlaps.empty()) {
    std::vector<std::pair<std::size_t, GuestListing>> listings;
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (!has_payload(*manifests[i], "guest-asm")) continue;
      GuestListingResult l = guest_listing(*manifests[i], members[i]->files, revision_for(*manifests[i], out.original_sha256)->label);
      if (!l.ok) { out.violations.push_back({l.rejection.code, {manifests[i]->id}, l.rejection.message}); return out; }
      listings.emplace_back(i, std::move(l.listing));
    }
    for (std::size_t a = 0; a < listings.size() && admit != nullptr; ++a)
      for (std::size_t b = a + 1; b < listings.size(); ++b) {
        GuestListing both = listings[a].second;
        both.blocks.insert(both.blocks.end(), listings[b].second.blocks.begin(), listings[b].second.blocks.end());
        both.free_ranges.insert(both.free_ranges.end(), listings[b].second.free_ranges.begin(), listings[b].second.free_ranges.end());
        if (const auto refusal = admit(both)) {
          const std::string &x = manifests[listings[a].first]->id, &y = manifests[listings[b].first]->id;
          out.violations.push_back({"E_SET_GUEST_ADMISSION", {x, y}, "the injected code of " + named(x) + " and " + named(y) + " is not statically admitted together: " + refusal->message});
        }
      }
  }

  // one header policy for the composed image
  std::string opted_out, repairing, repairing_policy;
  bool explicit_repair = false;
  for (std::size_t i = 0; i < members.size(); ++i) {
    const Manifest &m = *manifests[i];
    if (!has_target_edits(m)) continue;
    if (m.header_repair == "none") {
      if (opted_out.empty()) opted_out = m.id;
    } else {
      if (repairing.empty()) repairing = m.id;
      if (!m.header_repair.empty() && !explicit_repair) { explicit_repair = true; repairing = m.id; repairing_policy = m.header_repair; }
    }
  }
  if (!opted_out.empty() && !repairing.empty())
    out.violations.push_back({"E_SET_HEADER_POLICY", {opted_out, repairing},
                              "the mod " + named(opted_out) + " declares header_repair = \"none\" but " + named(repairing) + " expects the header checksum to be handled by the build; one image has one header policy"});
  if (!out.violations.empty()) return out;

  std::vector<std::uint8_t> image(original.begin(), original.end());
  std::vector<Range> authored;
  for (std::size_t i = 0; i < members.size(); ++i) {   // set order; the writes are disjoint, so the bytes do not depend on it
    if (has_payload(*manifests[i], "bps-image")) { image = alone[i].image; continue; }   // the only image change of the set (checked above)
    for (const Range &r : targets[i].written)
      std::copy(targets[i].image.begin() + static_cast<std::ptrdiff_t>(r.start), targets[i].image.begin() + static_cast<std::ptrdiff_t>(r.end), image.begin() + static_cast<std::ptrdiff_t>(r.start));
    authored.insert(authored.end(), targets[i].written.begin(), targets[i].written.end());
  }
  const bool any_edits = std::any_of(manifests.begin(), manifests.end(), [](const Manifest *m) { return has_target_edits(*m); });
  if (any_edits) {
    const std::string policy = !opted_out.empty() ? std::string("none") : repairing_policy;
    for (std::size_t i = 0; i < members.size(); ++i) {   // name the member whose write the combined repair would discard
      std::vector<std::uint8_t> probe = image;
      const HeaderRepair one = repair_header(platform, policy, original, probe, targets[i].written);
      if (!one.ok && one.rejection.code == "E_HEADER_FIELD_COLLISION") {   // any other refusal concerns the image or the policy, not this member's writes
        std::vector<std::string> mods = {manifests[i]->id};
        if (explicit_repair && repairing != manifests[i]->id) mods.push_back(repairing);
        out.violations.push_back({one.rejection.code, mods, "the mod " + named(manifests[i]->id) + ": " + one.rejection.message});
        return out;
      }
    }
    const HeaderRepair header = repair_header(platform, policy, original, image, authored);
    if (!header.ok) {
      out.violations.push_back({header.rejection.code, repairing.empty() ? std::vector<std::string>{} : std::vector<std::string>{repairing}, header.rejection.message});
      return out;
    }
    out.header_repaired = header.repaired;
    out.warnings = header.warnings;
  }
  out.image = std::move(image);
  out.effective_sha256 = sha256_hex(out.image);
  out.ok = true;
  return out;
}

VariantIdentity set_identity(std::span<const Package *const> members, std::span<const VariantIdentity> member_variants, const SetComposition &composition) {
  if (!composition.ok || members.empty() || members.size() != member_variants.size()) return {};
  const Adapter *adapter = find_adapter(members.front()->manifest.platform);
  if (adapter == nullptr) return {};
  if (members.size() == 1) return member_variants.front();
  std::vector<std::string> sorted;
  for (const Package *p : members) sorted.push_back(p->identity);
  std::sort(sorted.begin(), sorted.end());
  std::string membership;
  for (const std::string &s : sorted) membership += s + "\n";
  VariantIdentity v;
  v.components = {{"set_format", std::to_string(kSetIdentityFormat)},
                  {"platform", std::string(adapter->platform)},
                  {"source_kind", composition.source.kind},
                  {"source_identity", source_identity_text(composition.source)},
                  {"adapter_capabilities", adapter_capability_digest(*adapter)},
                  {"membership", digest_of(membership)},
                  {"effective_image", composition.effective_sha256}};
  for (std::size_t i = 0; i < members.size(); ++i) v.components.push_back({"member." + std::to_string(i), members[i]->identity + ":" + member_variants[i].key});
  v.key = digest_of(serialize_components(v.components));
  return v;
}

}  // namespace segarecomp::mod
