#ifndef SEGARECOMP_VIEWER_SEGA_CD_SCD_VIEWER_H
#define SEGARECOMP_VIEWER_SEGA_CD_SCD_VIEWER_H

/* SEG-078-T001 (ADR 0204): the Sega CD program's entry in the shared SDL3 viewer. A viewer-variant program is the UNMODIFIED generated C
 * compiled with -Dscd_program_main_bios=scd_viewer_main_bios; its main() then reaches this function, which registers the SDL3 host with
 * the Sega CD live driver (scd_live.c) and runs the program's ordinary driver. Headless builds never define the macro or link this
 * library. Presentation, controller mapping, settings, pause overlay and pacing are the Genesis viewer's (platforms/genesis/viewer and
 * libs/viewer); nothing here is a second graphics or input stack. */
#include "scd_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int scd_viewer_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
