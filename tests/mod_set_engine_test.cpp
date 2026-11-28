// SEG-141 (ADR 0326): the pure rules of the one mod-set engine that no readable package can exercise from the CLI: version ranges, the persisted definition, conflict claims in disc address spaces,
// the disc source identity and the per-platform adapter capability digest, plus the one-member identity rule. Synthetic data only.
#include "segarecomp/mod_set.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace segarecomp::mod;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
}  // namespace

int main() {
  // version ranges: numeric (not lexicographic) comparison, every comparator must hold
  expect(version_satisfies("1.10.0", ">=1.9.0") && !version_satisfies("1.9.0", ">=1.10.0"), "versions compare numerically");
  expect(version_satisfies("1.2.0", ">=1.2.0 <2.0.0") && !version_satisfies("2.0.0", ">=1.2.0 <2.0.0") && !version_satisfies("1.1.999999999", ">=1.2.0"), "a half-open range");
  expect(version_satisfies("3.0.1", "=3.0.1") && !version_satisfies("3.0.2", "=3.0.1") && version_satisfies("3.0.2", ">3.0.1") && !version_satisfies("3.0.1", ">3.0.1") && version_satisfies("3.0.1", "<=3.0.1"), "each operator");
  expect(version_satisfies("0.0.0", "") && !version_satisfies("1.0", "") && !version_satisfies("1.0.0", "*") && !version_range_ok("") && !version_range_ok(" >=1.0.0"), "no range means every valid version; anything malformed never matches");

  // the definition round-trips in order and only in its canonical form
  const SetDefinition definition{"genesis", {std::string(64, 'b'), std::string(64, 'a')}};
  const SetDefinitionResult back = parse_set_definition(serialize_set_definition(definition));
  expect(back.ok && back.definition == definition, "the definition round-trips with its order");
  expect(!parse_set_definition(serialize_set_definition(definition) + "\n").ok && !parse_set_definition("").ok && parse_set_definition(std::string(kMaxSetDefinitionBytes + 1, 'x')).rejection.code == "E_SET_SYNTAX", "trailing and oversized text is refused");
  expect(set_name_ok("my-set-2") && !set_name_ok("") && !set_name_ok("-a") && !set_name_ok("a--b") && !set_name_ok("A") && !set_name_ok("a/b") && !set_name_ok(std::string(65, 'a')), "set names are path-safe tokens");

  // conflicts are per address space: equal offsets in different disc files, or in a disc file and a cartridge, do not collide
  const std::vector<SetClaim> claims = {
      {0, "disc-file:level1.bin", "m68k", "write", 0x100, 0x200}, {1, "disc-file:level2.bin", "m68k", "write", 0x100, 0x200},
      {2, "disc-file:level1.bin", "sub-m68k", "write", 0x1ff, 0x201}, {3, "disc-file:level1.bin", "z80", "write", 0x201, 0x300}, {1, "cartridge-image", "", "write", 0x100, 0x200}};
  const std::vector<SetOverlap> overlaps = find_overlaps(claims);
  expect(overlaps.size() == 1 && overlaps[0].first.member == 0 && overlaps[0].second.member == 2 && overlaps[0].start == 0x1ff && overlaps[0].end == 0x200, "only the shared bytes of one space overlap, whatever the CPU");
  const std::vector<SetClaim> hooks = {{0, "cartridge-image", "m68k", "hook-site", 0, 2}, {1, "cartridge-image", "m68k", "hook-site", 0, 2}, {1, "cartridge-image", "m68k", "write", 4, 8}, {1, "cartridge-image", "m68k", "write", 5, 6}};
  expect(find_overlaps(hooks).empty(), "two hook sites are not judged here (hook ownership is SEG-141-T003), and a member never conflicts with itself");
  expect(find_overlaps(std::vector<SetClaim>{{0, "s", "", "write", 0, 9}, {1, "s", "", "write", 0, 9}, {2, "s", "", "write", 0, 9}}, 2).size() == 2, "the report is bounded");

  // source identity: cartridge image + mapper configuration, or disc identity + BIOS requirement
  const SetSource plain{"cartridge-image", std::string(64, '1'), "", "", ""}, mapped{"cartridge-image", std::string(64, '1'), "sega", "", ""};
  const SetSource disc{"disc-image", "", "", std::string(64, '2'), "class:us"}, other_bios{"disc-image", "", "", std::string(64, '2'), "class:jp"};
  expect(source_identity_text(plain) != source_identity_text(mapped) && source_identity_text(disc) != source_identity_text(other_bios) && source_identity_text(disc) != source_identity_text(plain), "every part of the source identity matters");
  std::vector<std::string> digests;
  for (const Adapter &a : adapters()) digests.push_back(adapter_capability_digest(a));
  expect(digests.size() == 3 && digests[0] != digests[1] && digests[1] != digests[2] && digests[0] != digests[2] && digests[0] == adapter_capability_digest(*find_adapter("genesis")), "each platform's capability set has its own stable digest");

  // identity: one member is that member's variant identity, unchanged; a mismatched or failed composition yields no key
  Package a, b;
  a.manifest.platform = b.manifest.platform = "genesis";
  a.identity = std::string(64, 'a');
  b.identity = std::string(64, 'b');
  const Package *one[] = {&a}, *two[] = {&a, &b}, *swapped[] = {&b, &a};
  const std::vector<VariantIdentity> va = {{{{"package_identity", a.identity}}, "key-a"}}, vab = {va[0], {{{"package_identity", b.identity}}, "key-b"}}, vba = {vab[1], vab[0]};
  SetComposition composition;
  composition.source = plain;
  composition.effective_sha256 = std::string(64, '3');
  expect(set_identity(two, vab, composition).key.empty(), "a refused composition has no identity");
  composition.ok = true;
  expect(set_identity(one, va, composition).key == "key-a" && set_identity(one, va, composition).components == va[0].components, "a one-member set is the single mod");
  const VariantIdentity ab = set_identity(two, vab, composition);
  expect(ab.key.size() == 64 && ab.key == set_identity(two, vab, composition).key && ab.key != set_identity(swapped, vba, composition).key, "deterministic, and order matters");
  SetComposition changed = composition;
  changed.source = mapped;
  expect(set_identity(two, vab, changed).key != ab.key, "another source configuration is another identity");
  expect(set_identity(two, va, composition).key.empty(), "one identity per member");

  if (failures != 0) return 1;
  std::cout << "mod_set_engine_test: ok\n";
  return 0;
}
