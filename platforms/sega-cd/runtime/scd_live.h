#ifndef SEGARECOMP_RUNTIME_SEGA_CD_SCD_LIVE_H
#define SEGARECOMP_RUNTIME_SEGA_CD_SCD_LIVE_H

/* SEG-078-T001 (ADR 0204): the Sega CD live (interactive) driver loop.
 *
 * The Sega CD machine keeps its own scheduler (ScdMachine, deadline slices); the live driver is a host loop over it, not a second
 * runtime. It advances the machine exactly one video frame at a time - from the stop boundary before one vertical-blank onset to the
 * boundary before the next - and at each boundary, before the onset's device events run:
 *
 *   1. polls the host (quit, pause, single step, the port-1 pad),
 *   2. stamps the pad's changes at the onset tick (scd_input_live_sample): a tick the machine has not yet reached, applied by the
 *      ordinary input-script machinery before every other device event and processor instruction at or after it (ADR 0149 decision 5),
 *      and appended to the recorded script,
 *   3. runs the machine to the next boundary,
 *   4. presents the completed frame (if any) and paces by the machine's own frame length.
 *
 * Pause and single step stop the host loop only: no guest time, no input tick and no device state is touched while paused. Nothing
 * here links SDL; the host (the SDL3 viewer, or a test double) is injected. Strict C11. */
#include <stdint.h>

#include "scd_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ScdLiveSummary;
struct GenesisFrameExtension;

typedef struct ScdLivePoll {
  uint8_t pad1;   /* GENESIS_PAD_* mask held now (the standard three-button pad of port 1) */
  uint8_t quit;   /* leave the run */
  uint8_t pause;  /* hold the machine at this boundary */
  uint8_t step;   /* with pause: advance exactly one frame, then hold again */
} ScdLivePoll;

typedef struct ScdLiveHost {
  void *ctx;
  /* Called once per loop iteration. A host may block here (the pause overlay). Nonzero = host failure. */
  int (*poll)(void *ctx, uint64_t boundary, ScdLivePoll *out);
  /* A completed frame (valid only during the call). Nonzero = host failure. */
  int (*present)(void *ctx, const struct GenesisFrameArtifact *frame, uint64_t onset);
  /* Once per frame advanced: the machine's frame length in ticks and its tick rate. The host throttles; it never feeds time back.
   * NULL = unthrottled. Nonzero = host failure. */
  int (*pace)(void *ctx, uint64_t frame_ticks, uint64_t ticks_per_second);
  /* Once, when the loop ends for any reason (after the last frame): a host shows why the machine stopped. Optional. */
  void (*finished)(void *ctx, const struct ScdLiveSummary *summary, const ScdMachine *machine);
  /* SEG-104 (ADR 0244) / SEG-107 (ADR 0251): optional audio output of the 44.1 kHz stereo S16 timeline (interleaved L,R frames, valid only during
   * the call). By default the host receives the mixed RF5C164 PCM + CD-DA timeline in chunks of up to 1,024 frames, delivered as guest time passes
   * (the whole stream, silence included); with `--pcm-audio 0` it receives one drive sector (588 frames) of CD-DA at the moment the guest's drive plays
   * it, as before. A one-way sink: nothing it returns or does reaches the machine (a nonzero return is only counted). NULL = the viewer is silent
   * and no CD-DA is read. */
  int (*audio)(void *ctx, const int16_t *frames, uint32_t count);
  /* SEG-119 (ADR 0262): optional presentation-only Level 2 target (frame_extension.h), queried after each poll (the window may open at the first
   * poll). The driver attaches the returned target to the machine's presentation path (scd_vdp_present_set_extension); NULL detaches. The host
   * sets `request` between frames only. NULL callback = no extension work at all (an older-style host). Nothing here reaches guest state. */
  struct GenesisFrameExtension *(*extension)(void *ctx);
} ScdLiveHost;

typedef struct ScdLiveConfig {
  int sample_pad;           /* poll the host pad and record it; 0 when an input script drives the machine (replay) */
  uint64_t slice_ticks;     /* 0 = none: split each frame's run into deadline slices (the result must not depend on them) */
  int have_seed;
  uint64_t slice_seed;
  uint64_t until_tick;      /* UINT64_MAX = none */
} ScdLiveConfig;

typedef enum ScdLiveOutcome {
  SCD_LIVE_QUIT = 1,            /* the host asked to leave */
  SCD_LIVE_MACHINE_STOP,        /* the machine ended: idle, typed stop, budget (see the run result) */
  SCD_LIVE_DEADLINE,            /* --until-tick reached */
  SCD_LIVE_HOST_ERROR,          /* a host callback failed */
  SCD_LIVE_RECORDING_CAPACITY,  /* the recorded script reached the grammar's limit; the valid prefix is kept */
  SCD_LIVE_RECORDING_REFUSED    /* a recorder invariant failed (a tick already passed): never expected */
} ScdLiveOutcome;

typedef struct ScdLiveSummary {
  ScdLiveOutcome outcome;
  ScdRunResult result;
  uint64_t boundaries;        /* frames advanced */
  uint64_t frames_presented;
  uint64_t paused_polls;      /* polls that held the machine */
  uint64_t steps;             /* single steps taken */
} ScdLiveSummary;

const char *scd_live_outcome_name(ScdLiveOutcome outcome);

/* Run the machine as described above. The machine must be in the BIOS-image profile with its input (live or script) installed by the
 * caller. Enables continuous presentation. */
ScdLiveSummary scd_live_run(ScdMachine *m, const ScdLiveHost *host, const ScdLiveConfig *config);

/* The driver of a live program (scd_main.c): a viewer registers its host here before the program's main runs. NULL = headless. */
extern const ScdLiveHost *scd_live_host_registered;

#ifdef __cplusplus
}
#endif
#endif
