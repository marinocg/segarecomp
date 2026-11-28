// SEG-141-T003/T006/T007 (ADR 0327). See mod_set_build.hpp.
#include "mod_set_build.hpp"

#include "segarecomp/luau_api.hpp"
#include "segarecomp/machine/master_system/z80_guest_admission.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace segarecomp_cli {

namespace fs = std::filesystem;
namespace mod = segarecomp::mod;
namespace luau = segarecomp::luau;

namespace {

SetBuild refused(SetBuild out, std::string code, std::string message) {
  out.ok = false;
  out.code = std::move(code);
  out.message = std::move(message);
  if (out.violations.empty()) out.violations.push_back({out.code, {}, out.message});
  return out;
}

bool has(const std::vector<std::string> &v, const char *name) { return std::find(v.begin(), v.end(), name) != v.end(); }

std::string named(const mod::Package &p) { return "\"" + p.manifest.id + "\""; }

// Presentation output: only one member of a built set may have any (the host composes one mod's picture; merging two is not defined).
bool presents(const std::vector<std::string> &granted, const mod::Manifest &m) {
  return has(granted, "overlay.rgba") || has(granted, "scenery.words") || has(granted, "frame.vdp") || has(granted, "present.transform") || !mod::geometry_declaration_text(m).empty();
}

std::uint32_t mode_bits(const std::string &mode) { return mode == "before" ? 1U : mode == "after" ? 2U : mode == "before-after" ? 3U : 4U; }   // GENESIS_HOOK_MODE_*
const char *mode_name(std::uint32_t bits) { return bits == 1U ? "before" : bits == 2U ? "after" : bits == 3U ? "before-after" : "replace"; }

struct MergedSite {
  mod::ResolvedHook hook;   // id, entry and the merged masks
  std::uint32_t modes = 0;
  std::size_t owner = 0;    // set position of the first declaring mod
};

// Every native member's declarations resolve against the exact ORIGINAL image; mods hooking one function share one generated site. Composition already refused a shared replacement (E_SET_HOOK_OWNER).
std::optional<std::pair<std::string, std::string>> merge_hooks(SetBuild &set, const std::vector<std::uint8_t> &original) {
  std::map<std::uint32_t, MergedSite> sites;   // by entry
  std::set<std::string> ids;
  for (SetRuntimeMember &r : set.runtime) {
    const mod::Package &p = set.packages[r.index];
    if (!p.manifest.has_hooks) continue;
    const mod::ResolvedHooks resolved = mod::resolve_hooks_for_image(p.manifest, original);
    if (!resolved.ok) return std::make_pair(resolved.rejection.code, "the mod " + named(p) + ": " + resolved.rejection.message);
    std::uint32_t index = 0;
    for (const mod::Hook &declared : p.manifest.hooks) {   // hook ids are the manifest (id-sorted) index, as in a single-mod program
      const auto found = std::find_if(resolved.hooks.begin(), resolved.hooks.end(), [&](const mod::ResolvedHook &h) { return h.id == declared.id; });
      if (found != resolved.hooks.end()) {
        const std::uint32_t bits = mode_bits(found->mode);
        auto site = sites.find(found->entry);
        if (site == sites.end()) {
          MergedSite merged{*found, bits, r.index};
          if (ids.count(merged.hook.id) != 0) {   // two mods named different functions alike: the later site gets the member's suffix
            const std::string suffix = "-m" + std::to_string(r.index);
            merged.hook.id = merged.hook.id.substr(0, 64 - suffix.size()) + suffix;
            if (ids.count(merged.hook.id) != 0) return std::make_pair(std::string("E_SET_HOOK_SITE_ID"), std::string("the hook ids of the set cannot be made unique"));
          }
          ids.insert(merged.hook.id);
          site = sites.emplace(found->entry, std::move(merged)).first;
        } else {
          MergedSite &m = site->second;
          if (((m.modes | bits) & 4U) != 0U) return std::make_pair(std::string("E_SET_HOOK_OWNER"), "a replaced function cannot be shared (" + named(p) + ")");
          if (m.hook.has_anchor != found->has_anchor || m.hook.anchor != found->anchor)
            return std::make_pair(std::string("E_SET_HOOK_ANCHOR"), "the mods " + named(set.packages[m.owner]) + " and " + named(p) + " declare different anchors for one hooked function");
          m.modes |= bits;
          m.hook.reads |= found->reads;
          m.hook.writes |= found->writes;
          m.hook.flags_written = m.hook.flags_written || found->flags_written;
        }
        r.hooks.push_back({site->second.hook.id, index, bits, found->reads});
        r.hook_phases |= (found->mode == "after" ? 0U : 1U << 5U) | (found->mode == "after" || found->mode == "before-after" ? 1U << 6U : 0U);
      }
      ++index;
    }
    if (r.hooks.empty()) return std::make_pair(std::string("E_HOOK_REVISION"), "the mod " + named(p) + " declares function hooks but none applies to this revision");
  }
  if (sites.empty()) return std::nullopt;
  if (sites.size() > 64) return std::make_pair(std::string("E_HOOK_COUNT"), std::string("the set hooks more than 64 functions"));
  std::ostringstream plan;
  std::set<std::uint32_t> vectors;
  for (std::size_t vector = 2; vector < 64 && vector * 4 + 4 <= set.image.size(); ++vector) {
    const std::uint32_t target = (std::uint32_t{set.image[vector * 4]} << 24U) | (std::uint32_t{set.image[vector * 4 + 1]} << 16U) | (std::uint32_t{set.image[vector * 4 + 2]} << 8U) | set.image[vector * 4 + 3];
    if (target != 0U) vectors.insert(target);
  }
  for (const std::uint32_t target : vectors) {
    char text[24];
    std::snprintf(text, sizeof text, "vector %08x\n", static_cast<unsigned>(target));
    plan << text;
  }
  for (const auto &[entry, m] : sites) {
    char text[160];
    std::snprintf(text, sizeof text, "site %s %08x %s %08x %08x %d %d %u\n", m.hook.id.c_str(), static_cast<unsigned>(entry), mode_name(m.modes), static_cast<unsigned>(m.hook.reads), static_cast<unsigned>(m.hook.writes),
                  m.hook.replace_declared ? 1 : 0, m.hook.flags_written ? 1 : 0, static_cast<unsigned>(m.hook.cost));
    plan << text;
    if (m.hook.has_anchor) {
      std::snprintf(text, sizeof text, "anchor %s %08x\n", m.hook.id.c_str(), static_cast<unsigned>(m.hook.anchor));
      plan << text;
    }
  }
  set.hook_plan = plan.str();
  return std::nullopt;
}

}  // namespace

SetBuild prepare_mod_set(const std::vector<fs::path> &packages, const std::vector<std::string> &pinned, const std::vector<std::uint8_t> &original) {
  SetBuild out;
  if (packages.empty() || packages.size() > mod::kMaxSetMembers || packages.size() != pinned.size()) return refused(out, "E_SET_SIZE", "a mod set holds 1 to 16 mods, each with its identity");
  std::vector<std::vector<std::uint8_t>> archives;
  for (std::size_t i = 0; i < packages.size(); ++i) {   // read once; the verified bytes are the only ones compiled
    std::ifstream in(packages[i], std::ios::binary);
    std::vector<std::uint8_t> bytes;
    if (in) {
      bytes.resize(mod::kMaxArchiveBytes + 1U);
      in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(in.gcount()));
    }
    if (bytes.empty() || bytes.size() > mod::kMaxArchiveBytes) return refused(out, "E_ARCHIVE_ENTRY", "member " + std::to_string(i + 1) + " of the set cannot be read");
    mod::VerifiedPackage verified = mod::verify_package_bytes(std::move(bytes), pinned[i]);
    if (!verified.ok) return refused(out, verified.rejection.code, "member " + std::to_string(i + 1) + " of the set: " + verified.rejection.message);
    out.packages.push_back(std::move(verified.package));
    archives.push_back(std::move(verified.bytes));
  }
  std::vector<const mod::Manifest *> manifests;
  std::vector<const mod::Package *> members;
  for (const mod::Package &p : out.packages) { manifests.push_back(&p.manifest); members.push_back(&p); }
  if (auto violations = mod::check_set_relations(manifests); !violations.empty()) {
    out.violations = std::move(violations);
    return refused(out, out.violations.front().code, out.violations.front().message);
  }
  out.platform = out.packages.front().manifest.platform;
  if (out.platform != "genesis" && out.platform != "master-system")
    return refused(out, "E_SET_PLATFORM_UNSUPPORTED", "mod sets are not built for " + out.platform + " yet");
  mod::SetComposition composition = mod::compose_set(members, original, &segarecomp::machine::master_system::admit_z80_guest);
  if (!composition.ok) {
    out.violations = composition.violations;
    return refused(out, out.violations.front().code, out.violations.front().message);
  }
  out.original_sha256 = composition.original_sha256;
  out.effective_sha256 = composition.effective_sha256;
  out.image = composition.image;

  std::vector<mod::VariantIdentity> alone;
  for (std::size_t i = 0; i < out.packages.size(); ++i) {
    const mod::Package &p = out.packages[i];
    const std::string trust = mod::trust_class(p.manifest);
    const LuauBuild compiled = build_luau(p, archives[i]);
    if (!compiled.ok) return refused(out, compiled.rejection.code, "the mod " + named(p) + ": " + compiled.rejection.message);
    alone.push_back(mod::variant_identity(p, composition.original_sha256, composition.member_effective[i], compiled.compiled.empty() ? std::string_view{} : std::string_view(compiled.compiled.digest())));
    if (trust == "data") {
      if (p.manifest.has_hooks) return refused(out, "E_HOOK_CALLBACK_UNSUPPORTED", "the mod " + named(p) + " declares function hooks but has no native provider");
      continue;
    }
    SetRuntimeMember r;
    r.index = i;
    r.native = trust == "native";
    const auto support = out.platform == "master-system" ? luau::sms_scheduled_support(p, r.native) : r.native ? luau::native_scheduled_support(p) : luau::scheduled_support(p);
    if (support) return refused(out, support->code, "the mod " + named(p) + ": " + support->message);
    r.granted = (out.platform == "master-system" ? luau::sms_scheduled_host_config(p.manifest) : r.native ? luau::native_scheduled_host_config(p.manifest) : luau::scheduled_host_config(p.manifest)).capabilities;
    if (has(r.granted, "audio.command.send") || has(r.granted, "audio.event.read"))
      return refused(out, "E_SET_AUDIO_UNSUPPORTED", "the mod " + named(p) + " uses the audio API, which a built mod set does not support yet");
    if (!r.native)
      if (const auto issues = luau::analyze_mod(p); !issues.empty()) return refused(out, issues.front().cls, "the mod " + named(p) + ": the Luau scripts use the API wrongly: " + issues.front().where.text());
    r.luau = compiled;
    std::string label;
    for (const auto &revision : p.manifest.revisions)
      if (revision.sha256 == out.original_sha256) label = revision.label;
    r.ranges = mod::memory_for_revision(p.manifest, label);
    for (const auto &range : r.ranges)
      if (range.space == "m68k.rom" && static_cast<std::uint64_t>(range.offset) + range.length > out.image.size())
        return refused(out, "E_MEMORY_RANGE", "the mod " + named(p) + " declares an m68k.rom range outside the composed image");
    const std::vector<const char *> spaces = out.platform == "master-system" ? std::vector<const char *>{"z80.work_ram", "z80.cart_ram"} : std::vector<const char *>{"m68k.work_ram"};
    for (const char *space : spaces) {   // a memory capability alone never authorizes an offset
      bool reads = false, writes = false;
      for (const auto &range : r.ranges)
        if (out.platform == "genesis" || range.space == space) { reads = reads || range.access != "write"; writes = writes || range.access != "read"; }
      if ((has(r.granted, (std::string("memory.read.") + space).c_str()) && !reads) || (has(r.granted, (std::string("memory.write.") + space).c_str()) && !writes))
        return refused(out, "E_RANGE_DECLARATION", "the mod " + named(p) + " declares a memory capability but no memory range with that access for this revision");
    }
    if (presents(r.granted, p.manifest)) {
      if (out.drawing >= 0)
        return refused(out, "E_SET_PRESENTATION_CONFLICT", "the mods " + named(out.packages[out.runtime[static_cast<std::size_t>(out.drawing)].index]) + " and " + named(p) +
                                                               " both draw (overlay, scenery, exact frame, picture transform or a geometry declaration); a built set supports one drawing mod");
      out.drawing = static_cast<int>(out.runtime.size());
    }
    out.runtime.push_back(std::move(r));
  }
  const mod::VariantIdentity identity = mod::set_identity(members, alone, composition);
  if (identity.key.empty()) return refused(out, "E_SET_SYNTAX", "the composite identity cannot be derived");
  out.identity = identity.key;
  for (const auto &revision : out.packages.front().manifest.revisions)
    if (revision.sha256 == out.original_sha256) out.mapper = revision.mapper;
  if (const auto problem = merge_hooks(out, original)) return refused(out, problem->first, problem->second);
  out.ok = true;
  return out;
}

bool stage_set_sources(SetBuild &set, const fs::path &dir) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) return false;
  for (SetRuntimeMember &r : set.runtime) {
    if (!r.native) continue;
    const mod::Package &p = set.packages[r.index];
    r.source_dir = dir / ("m" + std::to_string(r.index)) / "src";
    r.units.clear();
    for (const auto &payload : p.manifest.payloads) {   // staged under the package path, private to this build; every file is re-hashed immediately before the compile
      if (payload.kind != "native-c11") continue;
      const auto found = p.files.find(payload.path);
      if (found == p.files.end() || payload.path.empty() || payload.path.front() == '/' || payload.path.find("..") != std::string::npos) return false;
      const fs::path at = r.source_dir / payload.path;
      fs::create_directories(at.parent_path(), ec);
      std::ofstream staged(at, std::ios::binary | std::ios::trunc);
      staged.write(reinterpret_cast<const char *>(found->second.data()), static_cast<std::streamsize>(found->second.size()));
      staged.close();
      if (ec || !staged) return false;
      r.units.emplace_back(at, segarecomp::sha256_hex(found->second));
    }
  }
  return true;
}

std::string set_artifact_source(const SetBuild &set) {
  const bool sms = set.platform == "master-system";
  std::ostringstream c, rows;
  c << "/* GENERATED by `segarecomp build --set-package` (ADR 0327) from verified packages; do not edit. */\n#include \"" << (sms ? "sms_mod_set_artifact.h" : "mod_set_artifact.h") << "\"\n\n";
  const auto bytes = [&](const std::string &name, const std::vector<std::uint8_t> &data) {
    c << "static const unsigned char " << name << "[] = {";
    for (std::size_t i = 0; i < data.size(); ++i) c << (i % 24 == 0 ? "\n  " : "") << static_cast<unsigned>(data[i]) << ",";
    c << "0};\n";
  };
  for (const SetRuntimeMember &r : set.runtime) {
    const mod::Package &p = set.packages[r.index];
    const std::string n = std::to_string(r.index);
    std::string caps;
    unsigned long long required[2] = {0, 0}, optional[2] = {0, 0};
    for (const std::string &name : r.granted) {
      caps += (caps.empty() ? "" : ",") + name;
      const mod::CapabilityInfo *info = mod::find_capability(name);
      if (info == nullptr) continue;   // unreachable: the granted list comes from the catalogue
      (has(p.manifest.capabilities, name.c_str()) ? required : optional)[info->id / 64U] |= 1ULL << (info->id % 64U);
    }
    c << "static const unsigned long ranges_" << n << "[][4] = {";
    for (const auto &range : r.ranges) {
      const unsigned space = sms ? (range.space == "z80.cart_ram" ? 2U : 1U) : (range.space == "m68k.rom" ? 2U : 1U);   // SegaSmsSpace / SegaGenesisSpace
      c << "\n  {" << space << "UL, " << range.offset << "UL, " << range.length << "UL, " << (range.access == "read" ? 1 : range.access == "write" ? 2 : 3) << "UL},";
    }
    c << "\n  {0, 0, 0, 0}};\n";
    std::string native = "0", rom = "0", script = "0", digest = "0", hooks = "0";
    std::size_t script_size = 0;
    if (r.native) {
      c << "const SegaModProvider *segarecomp_mod_provider_m" << n << "(void **state_out);\n";   // the unit is compiled with -Dsegarecomp_mod_provider=segarecomp_mod_provider_m<n>
      native = "segarecomp_mod_provider_m" + n;
      if (!sms) {
        std::vector<std::uint8_t> data;
        for (const auto &range : r.ranges)
          if (range.space == "m68k.rom")
            data.insert(data.end(), set.image.begin() + range.offset, set.image.begin() + range.offset + range.length);
        bytes("rom_" + n, data);
        rom = "rom_" + n;
      }
    } else {
      const std::vector<std::uint8_t> stored = r.luau.compiled.serialize();
      bytes("luau_" + n, stored);
      script = "luau_" + n;
      script_size = stored.size();
      digest = "\"" + r.luau.compiled.digest() + "\"";
    }
    if (!r.hooks.empty()) {
      c << "static const SegarecompModBindingRow hooks_" << n << "[] = {";
      for (const SetHookBinding &h : r.hooks) c << "\n  {\"" << h.id << "\", " << h.hook_id << "UL, " << h.modes << "UL, " << h.reads << "UL},";
      c << "\n};\n";
      hooks = "hooks_" + n;
    }
    rows << "  {\"" << p.manifest.id << "\", \"" << p.identity << "\", \"" << caps << "\", {" << required[0] << "ULL, " << required[1] << "ULL}, {" << optional[0] << "ULL, " << optional[1] << "ULL},\n   "
         << p.manifest.state_schema << "UL, " << r.ranges.size() << "UL, ranges_" << n << ", ";
    if (sms) {
      rows << native << ", " << script << ", " << script_size << "UL, " << digest << ", " << p.manifest.luau_memory_bytes << "UL, " << p.manifest.luau_budget_units << "UL},\n";
    } else {
      rows << rom << ", " << native << ", " << script << ", " << script_size << "UL, " << digest << ", " << p.manifest.luau_memory_bytes << "UL, " << p.manifest.luau_budget_units << "UL, " << r.hooks.size() << "UL, " << r.hook_phases << "UL, "
           << hooks << "},\n";
    }
  }
  const char *prefix = sms ? "segarecomp_sms_mod" : "segarecomp_mod";
  c << "\nconst char " << prefix << "_set_identity[] = \"" << set.identity << "\";\n";
  c << "const char " << prefix << "_effective_sha256[] = \"" << set.effective_sha256 << "\";\n";
  c << "const unsigned long " << prefix << "_set_count = " << set.runtime.size() << "UL;\n";
  c << "const " << (sms ? "SegarecompSmsModMemberRow" : "SegarecompModMemberRow") << " " << prefix << "_set_members[] = {\n" << rows.str() << "};\n";
  return c.str();
}

std::string set_status_json(const SetBuild &set) {
  std::ostringstream o;
  o << ",\"mod_set\":{\"identity\":\"" << set.identity << "\",\"platform\":\"" << set.platform << "\",\"original_sha256\":\"" << set.original_sha256 << "\",\"effective_sha256\":\"" << set.effective_sha256 << "\",\"members\":[";
  for (std::size_t i = 0; i < set.packages.size(); ++i) {
    const mod::Manifest &m = set.packages[i].manifest;
    std::string caps;
    for (const SetRuntimeMember &r : set.runtime)
      if (r.index == i)
        for (const std::string &name : r.granted) caps += (caps.empty() ? "" : ",") + name;
    o << (i ? "," : "") << "{\"id\":\"" << m.id << "\",\"package_identity\":\"" << set.packages[i].identity << "\",\"trust_class\":\"" << mod::trust_class(m) << "\",\"capabilities\":\"" << caps << "\"}";
  }
  o << "]}";
  return o.str();
}

}  // namespace segarecomp_cli
