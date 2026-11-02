#include "native_compat_registry.h"

#include "sonic1_widescreen.h"

const GenesisNativeCompat *genesis_native_compat_select(unsigned index, unsigned implementation, void **state_out) {
  if (index == 0U && implementation == SONIC1_NATIVE_IMPLEMENTATION) return sonic1_widescreen_provider(state_out);
  return NULL;
}
