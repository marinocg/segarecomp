/*
 * SEG-021-T031: production seam between a generated Genesis bridge program and the headless
 * frame capture (frame_capture.h). Mirrors the SEG-007-T254 viewer seam: the capture build
 * (tools/genesis_startup_bridge.py --capture-frames) compiles the UNMODIFIED generated main TU
 * with -Dgenesis_runtime_run=genesis_frame_capture_hook_run, so the generated main hands its own
 * GenesisRuntime, dispatcher and runner allowance to this function. No SDL, no decoding, no
 * second dispatcher. Options arrive via environment variables (already validated by the Python
 * driver and re-validated here) so the generated argv ABI is unchanged:
 *   SEGARECOMP_CAPTURE_DIR, SEGARECOMP_CAPTURE_FIRST, SEGARECOMP_CAPTURE_COUNT,
 *   SEGARECOMP_CAPTURE_STRIDE (optional, default 1), SEGARECOMP_CAPTURE_SLICE (optional, default 65536).
 * SEG-021-T041 / ADR 0049: SEGARECOMP_STOP_WORK_RAM_DUMP (optional) names a private, ephemeral file that receives,
 * only when the guest stops fail-closed, 8 header bytes (big-endian stop PC, big-endian stop class) followed by the
 * 65536 work-RAM bytes at the stop. The tooling uses it to prove a fixed immutable-ROM-to-work-RAM verbatim copy
 * and derive an alias descriptor; the file is local diagnostic material and is never durable evidence.
 * Prints one CAPTURE_SUMMARY line (ordinals/counts/digests of synthetic-or-local frames only;
 * callers decide what, if anything, becomes durable evidence).
 * SEG-068-T007: the option parsing, run, summary and stop dump live in frame_capture.c
 * (genesis_frame_capture_run_from_environment) so the sound hook can delegate to the very same code; this seam is now only the
 * entry symbol the capture build maps genesis_runtime_run to. SEGARECOMP_PAD_SCRIPT adds the optional frame-keyed pad script.
 */
#include "frame_capture.h"
#include "runtime.h"

GenesisControlTransfer genesis_frame_capture_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                      uint32_t dispatch_allowance);

GenesisControlTransfer genesis_frame_capture_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                      uint32_t dispatch_allowance) {
  return genesis_frame_capture_run_from_environment(runtime, dispatch, dispatch_allowance);
}
