/* Host-side negotiation harness (SEG-146-T003): links ONE synthetic provider and proves negotiation, rejection of mismatched platform/ABI/size/minor/capability, and the typed table.
 * Build: -DHARNESS_HEADER='"segarecomp_mod_<p>.h"' -DHARNESS_FEATURES=<type> -DHARNESS_PLATFORM_ID=<n>. Exit 0 when every expectation holds. */
#include HARNESS_HEADER

#include <stdio.h>
#include <string.h>

static int failures;
#define EXPECT(cond, text) do { if (!(cond)) { printf("FAIL: %s\n", text); ++failures; } } while (0)

int main(void) {
  void *state = NULL;
  const SegaModProvider *provider = segarecomp_mod_provider(&state);
  HARNESS_FEATURES features;
  SegaModHost host;
  SegaModProvider copy;
  uint64_t required[2] = {0U, 0U};
  uint64_t none[2] = {0U, 0U};
  memset(&features, 0, sizeof features);
  features.size = sizeof features;
  memset(&host, 0, sizeof host);
  host.size = sizeof host;
  host.abi_major = SEGA_MOD_ABI_MAJOR;
  host.abi_minor = SEGA_MOD_ABI_MINOR;
  host.platform = HARNESS_PLATFORM_ID;
  host.platform_features = &features;
  host.platform_features_size = (uint32_t)sizeof features;
  host.granted[0] = (1ULL << SEGA_MOD_CAP_HOST_LOG);
  required[0] = (1ULL << SEGA_MOD_CAP_HOST_LOG);

  EXPECT(provider != NULL && state != NULL, "the entry symbol returns the provider and its state");
  EXPECT(sega_mod_negotiate(provider, &host, required) == SEGA_MOD_NEG_OK, "a matching host negotiates");
  EXPECT(provider->init(state, &host) == 0, "init succeeds on its own platform");
  provider->phase(state, &host);

  { SegaModHost other = host; unsigned p; for (p = 1; p <= 3; ++p) { if (p == HARNESS_PLATFORM_ID) continue; other.platform = p;
      EXPECT(sega_mod_negotiate(provider, &other, required) == SEGA_MOD_NEG_E_ABI_PLATFORM, "another platform's host is E_ABI_PLATFORM");
      EXPECT(strcmp(sega_mod_negotiation_class(sega_mod_negotiate(provider, &other, required)), "E_ABI_PLATFORM") == 0, "the stable class name is E_ABI_PLATFORM"); }
    other.platform = 9U; EXPECT(sega_mod_negotiate(provider, &other, required) == SEGA_MOD_NEG_E_ABI_PLATFORM, "an unknown platform id is rejected"); }
  { SegaModHost abi3 = host; abi3.abi_major = 3U; EXPECT(sega_mod_negotiate(provider, &abi3, required) == SEGA_MOD_NEG_E_ABI_UNSUPPORTED, "a host ABI 3 is E_ABI_UNSUPPORTED"); }
  copy = *provider; copy.abi_major = 1U;
  EXPECT(sega_mod_negotiate(&copy, &host, required) == SEGA_MOD_NEG_E_ABI_UNSUPPORTED, "a provider ABI 1 table is E_ABI_UNSUPPORTED");
  copy = *provider; copy.required_minor = SEGA_MOD_ABI_MINOR + 1U;
  EXPECT(sega_mod_negotiate(&copy, &host, required) == SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW, "a provider needing a newer minor is E_ABI_MINOR_TOO_NEW");
  { SegaModHost newer = host; newer.abi_minor = SEGA_MOD_ABI_MINOR + 1U; copy = *provider; copy.required_minor = SEGA_MOD_ABI_MINOR + 1U;
    EXPECT(sega_mod_negotiate(&copy, &newer, required) == SEGA_MOD_NEG_OK, "a newer host accepts a provider requiring an older or equal minor"); }
  copy = *provider; copy.size = (uint32_t)sizeof(SegaModProvider) - (uint32_t)sizeof(void *);
  EXPECT(sega_mod_negotiate(&copy, &host, required) == SEGA_MOD_NEG_E_ABI_TABLE_SIZE, "a short provider table is E_ABI_TABLE_SIZE");
  copy = *provider; copy.size = (uint32_t)sizeof(SegaModProvider) + 1U;
  EXPECT(sega_mod_negotiate(&copy, &host, required) == SEGA_MOD_NEG_E_ABI_TABLE_SIZE, "a misaligned provider table size is E_ABI_TABLE_SIZE");
  copy = *provider; copy.size = (uint32_t)sizeof(SegaModProvider) + (uint32_t)sizeof(void *);
  EXPECT(sega_mod_negotiate(&copy, &host, required) == SEGA_MOD_NEG_OK, "a larger aligned provider table is accepted (append-only evolution)");
  EXPECT(sega_mod_negotiate(NULL, &host, required) == SEGA_MOD_NEG_E_ABI_ENTRY_MISSING, "a missing entry is E_ABI_ENTRY_MISSING");
  required[0] |= (1ULL << SEGA_MOD_CAP_MEMORY_WRITE);
  EXPECT(sega_mod_negotiate(provider, &host, required) == SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED, "a required capability that is not granted is never dropped");
  host.granted[0] |= (1ULL << SEGA_MOD_CAP_MEMORY_WRITE);
  EXPECT(sega_mod_negotiate(provider, &host, required) == SEGA_MOD_NEG_OK, "granting it negotiates");
  EXPECT(sega_mod_negotiate(provider, &host, none) == SEGA_MOD_NEG_OK, "an empty requirement negotiates");
  EXPECT(sega_mod_is_granted(&host, 200U) == 0, "ids outside the bitset are not granted");
  EXPECT(host.platform_features_size == sizeof features && features.size == sizeof features, "the typed table carries its size");
  if (failures == 0) printf("ok\n");
  return failures == 0 ? 0 : 1;
}
