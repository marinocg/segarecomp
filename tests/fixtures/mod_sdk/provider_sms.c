/* Synthetic ABI 2 provider for the master-system platform (SEG-146-T003): lifecycle and capability reporting only; it touches no guest state. */
#include <segarecomp_mod_sms.h>

static int calls;

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  (void)host;
  ++calls;
}

static const SegaModProvider provider = {
    sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "synthetic-sms", 1U, provider_init, provider_phase, NULL, NULL, NULL, {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &calls;
  return &provider;
}
