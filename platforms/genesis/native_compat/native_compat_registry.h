#ifndef SEGARECOMP_GENESIS_NATIVE_COMPAT_REGISTRY_H
#define SEGARECOMP_GENESIS_NATIVE_COMPAT_REGISTRY_H

/* SEG-124 (ADR 0267): the closed, audited, statically linked set of native compatibility providers. A build links this only when a verified package selects a
 * capability; the lookup is an exact id match (no search path, no loading). */
#include "native_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The provider at `index` of the audited table (index = position of the capability in segarecomp::patch::kNativeCapabilities, checked by a test) whose implementation
 * version equals `implementation`, or NULL (unknown index or different version). Never searches, loads or resolves a name at run time. */
const GenesisNativeCompat *genesis_native_compat_select(unsigned index, unsigned implementation, void **state_out);

#ifdef __cplusplus
}
#endif
#endif
