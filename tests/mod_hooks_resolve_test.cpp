// SEG-138-T004 (ADR 0298): hook resolution against an exact image digest, the register view names and the deterministic hook-table text. Synthetic data only.
#include "segarecomp/mod.hpp"
#include "segarecomp/sha256.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
}  // namespace

int main() {
  using namespace segarecomp::mod;
  const std::vector<std::uint8_t> image_a{1, 2, 3}, image_b{4, 5, 6}, image_c{7, 8, 9};
  Manifest m;
  m.platform = "genesis";
  m.revisions = {{"rev-a", segarecomp::sha256_hex(image_a), ""}, {"rev-b", segarecomp::sha256_hex(image_b), ""}};
  m.symbols = {{"fn", "rev-a", "verified", 0x220}, {"fn", "rev-b", "verified", 0x320}, {"hint", "rev-a", "imported-hint", 0x240}};
  Hook h;
  h.id = "h1"; h.symbol = "fn"; h.mode = "replace"; h.cpu = "m68k"; h.reads = {"d0", "a7"}; h.writes = {"d0", "ccr"}; h.has_replace = true; h.cost = 24;
  m.hooks = {h};
  m.has_hooks = true;

  check(!check_hooks(m), "a valid declaration passes");
  const ResolvedHooks a = resolve_hooks_for_image(m, image_a), b = resolve_hooks_for_image(m, image_b);
  check(a.ok && a.revision == "rev-a" && a.hooks.size() == 1 && a.hooks[0].entry == 0x220, "revision A resolves to its own verified address");
  check(b.ok && b.revision == "rev-b" && b.hooks[0].entry == 0x320, "revision B resolves independently");
  check(a.hooks[0].reads == ((1U << 0) | (1U << 15)) && a.hooks[0].writes == ((1U << 0) | (1U << 16)) && a.hooks[0].flags_written && a.hooks[0].replace_declared && a.hooks[0].cost == 24, "masks follow the register view");
  const ResolvedHooks c = resolve_hooks_for_image(m, image_c);
  check(!c.ok && c.rejection.code == "E_HOOK_DIGEST", "an image that is none of the declared revisions is E_HOOK_DIGEST");
  check(hook_table_text(a) == hook_table_text(resolve_hooks_for_image(m, image_a)) && hook_table_text(a) != hook_table_text(b), "the hook table text is deterministic and revision-specific");
  check(hook_table_text(c).empty(), "no table for a rejected resolution");

  Manifest hint = m;
  hint.hooks[0].symbol = "hint";
  const auto rejected = check_hooks(hint);
  check(rejected && rejected->code == "E_SYMBOL_NOT_VERIFIED", "an imported hint never resolves");

  // register names: readable vs writable
  check(hook_register_bit("d0", true) == 0U && hook_register_bit("a6", true) == 14U && !hook_register_bit("a7", true) && hook_register_bit("a7", false) == 15U, "A7 is readable, never writable");
  check(hook_register_bit("ccr", true) == 16U && hook_register_bit("cycles", false) == 17U && !hook_register_bit("cycles", true) && hook_register_bit("stack7", false) == 31U && !hook_register_bit("stack0", true), "ccr/cycles/stack slots");
  check(!hook_register_bit("d8", false) && !hook_register_bit("stack8", false) && !hook_register_bit("", false) && !hook_register_bit("D0", false), "unknown names are refused");

  if (failures != 0) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("mod_hooks_resolve_test: ok");
  return 0;
}
