// Variant identity of a single built mod (SEG-135-T005, ADR 0289): which inputs select a build, and which of them changed. Pure functions; no file access, no compiler.
#include "segarecomp/mod.hpp"
#include "segarecomp/patch.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>

namespace segarecomp::mod {

namespace {
std::string digest_of(std::string_view text) { return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}); }
std::string join(const std::vector<std::string> &v) {
  std::string o;
  for (const std::string &s : v) o += s + ",";
  return o;
}
}  // namespace

VariantIdentity variant_identity(const Manifest &m, std::string_view original_sha256, std::string_view effective_sha256) {
  VariantIdentity v;
  // Source digest: the digests of the executable-class payloads (native C11 and Luau) in path order, each bound to its path.
  std::vector<std::string> sources;
  for (const Payload &p : m.payloads)
    if (p.kind == "native-c11" || p.kind == "luau-source") sources.push_back(p.path + ":" + p.sha256);
  std::sort(sources.begin(), sources.end());
  std::vector<std::string> caps = m.capabilities, optional = m.capabilities_optional;
  std::sort(caps.begin(), caps.end());
  std::sort(optional.begin(), optional.end());
  v.components = {{"original_image", std::string(original_sha256)},
                  {"effective_image", std::string(effective_sha256)},
                  {"package_identity", digest_of(serialize_manifest(m))},
                  {"source_digest", digest_of(join(sources))},
                  {"abi", std::to_string(m.abi_native)},
                  {"capabilities", digest_of(join(caps) + "|" + join(optional))}};
  v.key = digest_of(serialize_components(v.components));
  return v;
}

std::vector<std::string> changed_components(const std::vector<VariantComponent> &previous, const std::vector<VariantComponent> &current) {
  std::vector<std::string> changed;
  for (const VariantComponent &c : current) {
    const auto it = std::find_if(previous.begin(), previous.end(), [&](const VariantComponent &p) { return p.name == c.name; });
    if (it == previous.end() || it->value != c.value) changed.push_back(c.name);
  }
  for (const VariantComponent &p : previous)
    if (std::none_of(current.begin(), current.end(), [&](const VariantComponent &c) { return c.name == p.name; })) changed.push_back(p.name);
  return changed;
}

std::string serialize_components(const std::vector<VariantComponent> &components) {
  std::string o;
  for (const VariantComponent &c : components) o += c.name + "=" + c.value + "\n";
  return o;
}

std::vector<VariantComponent> parse_components(std::string_view text) {
  std::vector<VariantComponent> out;
  while (!text.empty()) {
    const auto nl = text.find('\n');
    if (nl == std::string_view::npos) return {};
    const std::string_view line = text.substr(0, nl);
    const auto eq = line.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == line.size()) return {};
    out.push_back({std::string(line.substr(0, eq)), std::string(line.substr(eq + 1))});
    text.remove_prefix(nl + 1);
  }
  return out;
}

EffectiveImage effective_image(const Package &package, std::span<const std::uint8_t> original) {
  EffectiveImage e;
  e.original_sha256 = sha256_hex(original);
  const bool declared = std::any_of(package.manifest.revisions.begin(), package.manifest.revisions.end(), [&](const Revision &r) { return r.sha256 == e.original_sha256; });
  if (!declared) { e.rejection = {"E_IMAGE_MISMATCH", "the original image is not one of the declared target revisions"}; return e; }
  e.effective_sha256 = e.original_sha256;
  for (const Payload &p : package.manifest.payloads) {
    if (p.kind != "bps-image") continue;
    const auto it = package.files.find(p.path);
    if (it == package.files.end()) { e.rejection = {"E_IMAGE_MISMATCH", "the bps-image payload is missing"}; return e; }
    const patch::BpsResult r = patch::apply_bps(it->second, original);
    if (!r.ok || sha256_hex(r.output) != p.target_sha256) { e.rejection = {"E_IMAGE_MISMATCH", "the bps-image payload does not produce its declared target"}; return e; }
    e.effective_sha256 = p.target_sha256;
  }
  e.ok = true;
  return e;
}

}  // namespace segarecomp::mod
