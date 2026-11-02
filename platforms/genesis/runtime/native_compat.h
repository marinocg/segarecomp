#ifndef SEGARECOMP_GENESIS_NATIVE_COMPAT_H
#define SEGARECOMP_GENESIS_NATIVE_COMPAT_H

/* Host side of the native patch ABI (ADR 0267): the public SDK header plus the host's helper table. The interface a package provider sees is segarecomp_genesis_patch.h only. */
#include "../sdk/segarecomp_genesis_patch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The host helper table (backed by the runtime's own VDP functions); defined in vdp_render.c. */
const GenesisCompatHost *genesis_native_compat_host(void);

#ifdef __cplusplus
}
#endif
#endif
