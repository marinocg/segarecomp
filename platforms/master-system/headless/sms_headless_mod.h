#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_HEADLESS_MOD_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_HEADLESS_MOD_H

/* SEG-147-T004/T005 (ADR 0325): what the headless driver of a build WITH a scheduled mod adds (compiled only with SEGARECOMP_SMS_MOD):
 *   --mod-ram <offset:length>[,...]   work-RAM samples after each of the first 64 frames (with the presented-picture digests) in MOD_SUMMARY
 *   --mod-checkpoint <frame>          at that frame boundary: capture, restore into the same machine, capture again (must be identical); the run then continues
 *   --mod-state-save <file>           writes the checkpoint's state (needs --mod-checkpoint)
 *   --mod-state-load <file>           restores a state before the first instruction of this run
 * A refused mod or state prints MOD_REFUSED {"class":..} and the program exits 5 without running the guest. */
#include "sms_machine.h"
#include "sms_pad.h"
#include "sms_psg.h"

/* Consumes argv[*index] when it is one of the options above. Returns 1 consumed, 0 not ours, -1 malformed. */
int sms_headless_mod_option(int argc, char **argv, int *index);
/* After the devices are attached, before `sms_machine_reset`. 0 = ok. */
int sms_headless_mod_attach(SmsMachine *machine, SmsPad *pad);
/* After `sms_machine_reset` and the input script: loads and/or checkpoints. `frames` is the run's frame bound (SMS_NO_LIMIT for none). 0 = ok; the caller exits 5 otherwise. */
int sms_headless_mod_before_run(SmsMachine *machine, SmsPsg *psg, SmsPad *pad, uint64_t frames);
/* After the run: MOD_SUMMARY on stderr and, with an artifact directory, mod.json and presented.rgb. Returns 0 if a write failed. */
int sms_headless_mod_finish(const char *artifacts);

#endif
