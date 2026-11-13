#ifndef SEGARECOMP_GENESIS_MOD_SCHEDULER_H
#define SEGARECOMP_GENESIS_MOD_SCHEDULER_H

/*
 * SEG-138-T001/T002 (ADR 0297): the ONE Genesis mod phase scheduler, shared by native C11 (ABI 2) providers and the Luau adapter.
 *
 * It implements docs/architecture/mod-execution-phases.md for the phases `reset`, `frame-begin`, `input`, `post-input`, `frame-end` and `render` (the hook phases are SEG-138-T004
 * and are refused). Callbacks run only on the dispatch boundaries the runtime exposes through `GenesisFrameHooks`, take zero guest time and never change the guest clock. Every host
 * service is checked against the capability granted, the phase and the declared range; a violation is a fault: the callback's staged writes/input/log are discarded, the mod is disabled
 * for the rest of the session, a stable `E_MOD_FAULT` diagnostic is surfaced and the guest continues unmodified.
 *
 * Without a scheduler (`GenesisRuntime::frame_hooks == NULL`) the runtime is byte-for-byte what it was. With one attached but no mod registered for `input`, the guest reads the physical pad
 * exactly as before. Strict C11 (this library never links C++); one scheduler per process (the ABI 2 service functions carry no context pointer).
 */
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"
#include <segarecomp_mod_genesis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_MOD_WRITE_OPS_MAX 1024U     /* per callback (phases section 4) */
#define GENESIS_MOD_WRITE_BYTES_MAX 16384U  /* per callback */
#define GENESIS_MOD_LOG_LINES_MAX 16U       /* per callback (host.log) */
#define GENESIS_MOD_LOG_LINE_BYTES 256U
#define GENESIS_MOD_WRITE_LOG_MAX 4096U     /* committed writes kept for diagnostics/replay */
#define GENESIS_MOD_RANGES_MAX 16U
#define GENESIS_MOD_WORK_RAM_BYTES 65536U

/* Access modes of a declared range. */
#define GENESIS_MOD_ACCESS_READ 1U
#define GENESIS_MOD_ACCESS_WRITE 2U

/* A range a mod declared (per revision) for one space and the modes it may use on it. Only SEGA_GEN_SPACE_M68K_WORK_RAM is accessible in this version. */
typedef struct GenesisModRange {
  uint32_t space; /* SegaGenesisSpace */
  uint32_t offset;
  uint32_t length;
  uint32_t modes; /* GENESIS_MOD_ACCESS_* bits */
} GenesisModRange;

typedef void (*GenesisModLogSink)(void *ctx, const char *line, uint32_t length);

typedef struct GenesisModConfig {
  const char *mod_id; /* printable ASCII, used in diagnostics */
  const SegaModProvider *provider;
  void *provider_state;
  uint32_t phase_mask;  /* registered phases: bit (1u << SegaModPhase); hook phases are refused */
  uint64_t required[2]; /* capability ids (bitset); a required capability the scheduler does not support is refused, never dropped */
  uint64_t optional[2]; /* granted only when supported */
  const GenesisModRange *ranges;
  uint32_t range_count;
  GenesisModLogSink log_sink; /* host.log lines and fault diagnostics; NULL = discarded */
  void *log_ctx;
} GenesisModConfig;

typedef struct GenesisModWriteRecord {
  uint64_t frame;
  uint32_t phase; /* SegaModPhase */
  uint32_t space;
  uint32_t offset;
  uint32_t width; /* bytes */
  uint32_t value;
} GenesisModWriteRecord;

typedef struct GenesisModStagedWrite {
  uint32_t offset, width, value;
} GenesisModStagedWrite;

typedef struct GenesisModScheduler {
  /* ---- fixed after attach ---- */
  GenesisModConfig config;
  GenesisFrameHooks hooks;
  SegaModHost host;
  SegaGenesisFeatures features;
  GenesisModRange ranges[GENESIS_MOD_RANGES_MAX];
  GenesisRuntime *runtime;
  /* ---- session ---- */
  uint8_t started, disabled;
  uint64_t callbacks_run;
  struct {
    uint8_t faulted;
    const char *cause; /* capability|phase|space|range|alignment|readonly|budget|width|value|provider */
    uint32_t phase;
    uint64_t frame;
    char text[192]; /* "E_MOD_FAULT cause=<c> mod=<id> phase=<p> frame=<n>" */
  } fault;
  uint8_t sampled[2], final_pad[2], previous_final[2]; /* index 0 = port 1 */
  /* ---- the callback in flight ---- */
  uint8_t in_callback;
  uint32_t phase;
  uint64_t frame;
  uint8_t staged_pad[2], staged_pad_set[2];
  GenesisModStagedWrite staged[GENESIS_MOD_WRITE_OPS_MAX];
  uint32_t staged_count, staged_bytes;
  char staged_log[GENESIS_MOD_LOG_LINES_MAX][GENESIS_MOD_LOG_LINE_BYTES + 1U];
  uint32_t staged_log_count;
  int fault_status; /* SegaModStatus of the first violation (SEGA_MOD_OK = none) */
  const char *external_cause; /* a fault reported by a host-side adapter (genesis_mod_fault_current), NULL = none */
  /* ---- render copy (only when frame.work_ram is granted) ---- */
  uint8_t render_copy[GENESIS_MOD_WORK_RAM_BYTES];
  uint8_t render_copy_valid;
  /* ---- committed write log ---- */
  GenesisModWriteRecord write_log[GENESIS_MOD_WRITE_LOG_MAX];
  uint32_t write_log_count;
  uint64_t write_log_dropped;
  GenesisSha256 write_stream; /* streaming SHA-256 over EVERY committed write (retained or not), see genesis_mod_write_log_digest */
  uint64_t write_total;       /* committed writes, retained + dropped */
} GenesisModScheduler;

/* Attaches `scheduler` to `runtime` (sets runtime->frame_hooks). Returns 0, or 1 with *rejection set to a stable class: the SegaModNegotiation classes (E_ABI_*, E_CAPABILITY_UNSUPPORTED),
 * E_PHASE_UNSUPPORTED (no phase, an unknown phase, or a hook phase), E_RANGE_DECLARATION (a declared range outside the space, empty, or with a mode whose capability is not granted) and
 * E_PROVIDER_ENTRY (a provider without a `phase` entry or an invalid config). Nothing is attached on rejection. The runtime must not be running. */
int genesis_mod_scheduler_attach(GenesisModScheduler *scheduler, GenesisRuntime *runtime, const GenesisModConfig *config, const char **rejection);
/* Detaches: clears the hooks and the pad interception. The scheduler may then be discarded. */
void genesis_mod_scheduler_detach(GenesisModScheduler *scheduler);

/* The capabilities (bitset by id) this scheduler can grant: host.log, memory.read/write, input.read/intercept, frame.work_ram (render copy), overlay.rgba (render output, collected by the adapter). */
void genesis_mod_scheduler_supported(uint64_t out[2]);

/* For a host-side adapter (the Luau binding) running INSIDE a callback: reports that the callback failed for a reason the scheduler did not see (a script error, memory, budget or watchdog
 * fault). `cause` must be a static string. The callback is then handled exactly like any violation: staged effects discarded, mod disabled, E_MOD_FAULT surfaced. No effect outside a callback. */
void genesis_mod_fault_current(const char *cause);

/* The name of a phase ("reset", "frame-begin", ...); "" for an unknown value. */
const char *genesis_mod_phase_name(uint32_t phase);
/* The stable fault cause of a SegaModStatus ("capability", "phase", ...). */
const char *genesis_mod_status_cause(int status);

/* The replay witness of T002: SHA-256 over EVERY committed write in commit order (frame u64, phase, space, offset, width, value as big-endian u32s), streamed as each write commits so it also covers
 * records the bounded diagnostic log no longer retains, followed by the total count. Staged writes of a faulted callback never enter it. */
void genesis_mod_write_log_digest(const GenesisModScheduler *scheduler, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
