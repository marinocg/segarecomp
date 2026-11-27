#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MOD_HOST_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MOD_HOST_H

/*
 * SEG-147-T004/T006 (ADR 0325): the program side of a Master System build with a scheduled mod. `segarecomp build --luau-package` / `--native-package` links this file, the ONE scheduler, the presentation
 * compositor, a generated constant artifact (identity, capabilities, declared ranges) and the provider: the package's own C11 units, or the Luau adapter (sms_mod_luau.cpp), which defines the same
 * `segarecomp_mod_provider` entry. Both drivers (headless and viewer) use only this interface.
 */
#include <stdint.h>
#include <stdio.h>

#include "sms_machine.h"
#include "sms_mod_state.h"
#include "sms_pad.h"
#include "sms_render.h"
#include "sms_vdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_MOD_OBSERVE_FRAMES 64U   /* per-frame observations kept (frames 0..63) */
#define SMS_MOD_OBSERVE_REGIONS 8U
#define SMS_MOD_OBSERVE_BYTES 1024U  /* all regions together */

/* Loads the provider and attaches the scheduler. Call once, after the devices are attached and before `sms_machine_reset`. Prints MOD_LOADED, or MOD_REFUSED with a stable class, to stderr. 0 = ok. */
int sms_mod_host_attach(SmsMachine *machine, SmsPad *pad, SmsVdp *vdp, SmsRenderer *renderer);
const SmsModState *sms_mod_host_state(void);
/* The identity a save state of this program is bound to: the effective cartridge image it was built from, and the package (variant) identity. */
void sms_mod_host_identity(uint8_t source_sha256[32], uint8_t variant_sha256[32]);
/* Composes the picture of the frame `framebuffer` with the mod's committed presentation. Returns 1 and writes 256 x height RGB888 when there is something to present; 0 (nothing written) for the
 * Vanilla picture: no render output, an empty one, or a faulted mod. */
int sms_mod_host_compose(const uint8_t *framebuffer, uint32_t height, uint8_t *rgb);

/* Headless evidence (never used by the viewer): after every finished frame, hash the presented picture; for frames 0..63 also keep its digest and the requested work-RAM samples.
 * `regions` is "<offset>:<length>[,...]" decimal, inside the 8 KiB work RAM, or NULL. Returns 0 on a malformed list. */
int sms_mod_host_observe(const char *regions);
/* One `MOD_SUMMARY {json}` line. `extra` is "" or further JSON members starting with a comma. */
void sms_mod_host_summary(FILE *out, const char *extra);
/* The last presented picture (256 x *height RGB888), or NULL before the first finished frame. */
const uint8_t *sms_mod_host_last_presented(uint32_t *height);

#ifdef __cplusplus
}
#endif
#endif
