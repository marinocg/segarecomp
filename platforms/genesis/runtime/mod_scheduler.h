#ifndef SEGARECOMP_GENESIS_MOD_SCHEDULER_H
#define SEGARECOMP_GENESIS_MOD_SCHEDULER_H

/*
 * SEG-138-T001/T002 (ADR 0297): the ONE Genesis mod phase scheduler, shared by native C11 (ABI 2) providers and the Luau adapter.
 *
 * It implements docs/architecture/mod-execution-phases.md for the phases `reset`, `frame-begin`, `input`, `post-input`, `frame-end` and `render` (the hook phases `hook-entry`/`hook-exit` are the dispatch-boundary phases of the static function hooks, ADR 0298). Callbacks run only on the dispatch boundaries the runtime exposes through `GenesisFrameHooks`, take zero guest time and never change the guest clock. Every host
 * service is checked against the capability granted, the phase and the declared range; a violation is a fault: the callback's staged writes/input/log are discarded, the mod is disabled
 * for the rest of the session, a stable `E_MOD_FAULT` diagnostic is surfaced and the guest continues unmodified.
 *
 * Without a scheduler (`GenesisRuntime::frame_hooks == NULL`) the runtime is byte-for-byte what it was. With one attached but no mod registered for `input`, the guest reads the physical pad
 * exactly as before. Strict C11 (this library never links C++); one scheduler per process (the ABI 2 service functions carry no context pointer).
 */
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"
#include "function_hooks.h"
#include "genesis_snapshot.h"
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
/* SEG-152-T018 (ADR 0321): declared m68k.rom read ranges (SEGA_GEN_SPACE_M68K_ROM, GENESIS_MOD_ACCESS_READ only) are materialised from the effective image at build; bounds below. */
#define GENESIS_MOD_ROM_BYTES_MAX 4096U         /* sum of the ROM ranges' lengths */
#define GENESIS_MOD_ROM_READS_MAX 4096U         /* read operations per callback */
#define GENESIS_MOD_ROM_READ_BYTES_MAX 8192U    /* bytes read per callback */

/* SEG-145-T008: the declared audio interface of this mod, one flat row per (mapping, value). All values are work-RAM mailboxes/observed locations (m68k.work_ram, big-endian). */
#define GENESIS_MOD_AUDIO_ENTRIES_MAX 128U
#define GENESIS_MOD_AUDIO_COMMAND 1U
#define GENESIS_MOD_AUDIO_EVENT 2U
typedef struct GenesisModAudioEntry {
  const char *mapping; /* the mapping id; a command mapping is "sfx" or "music" */
  const char *name;    /* the declared value name */
  uint32_t kind;       /* GENESIS_MOD_AUDIO_COMMAND | GENESIS_MOD_AUDIO_EVENT */
  uint32_t offset, width, value;
} GenesisModAudioEntry;

/* Access modes of a declared range. */
#define GENESIS_MOD_ACCESS_READ 1U
#define GENESIS_MOD_ACCESS_WRITE 2U

/* A range a mod declared (per revision) for one space and the modes it may use on it. SEGA_GEN_SPACE_M68K_WORK_RAM (read/write) and, for the render phase, SEGA_GEN_SPACE_M68K_ROM (read only; `offset` is a byte offset into the effective image). */
typedef struct GenesisModRange {
  uint32_t space; /* SegaGenesisSpace */
  uint32_t offset;
  uint32_t length;
  uint32_t modes; /* GENESIS_MOD_ACCESS_* bits */
} GenesisModRange;

typedef void (*GenesisModLogSink)(void *ctx, const char *line, uint32_t length);

/* A hook site the mod implements: the generated site `id` (GenesisHookSiteInfo::id) and the `hook_id` the provider's `hook` entry is called with. A site no binding names is not this mod's. */
typedef struct GenesisModHookBinding {
  const char *id;
  uint32_t hook_id;
} GenesisModHookBinding;
#define GENESIS_MOD_HOOKS_MAX 64U

typedef struct GenesisModConfig {
  const char *mod_id; /* printable ASCII, used in diagnostics */
  const SegaModProvider *provider;
  void *provider_state;
  uint32_t phase_mask;  /* registered phases: bit (1u << SegaModPhase); hook-entry/hook-exit need hook.function and at least one binding */
  uint64_t required[2]; /* capability ids (bitset); a required capability the scheduler does not support is refused, never dropped */
  uint64_t optional[2]; /* granted only when supported */
  const GenesisModRange *ranges;
  uint32_t range_count;
  const uint8_t *rom_data; /* HOST-INTERNAL (not SDK/ABI): the concatenated bytes of the ROM-space ranges in `ranges` order; rom_data_bytes must equal their length sum (<= GENESIS_MOD_ROM_BYTES_MAX); NULL/0 = none */
  uint32_t rom_data_bytes;
  const GenesisModHookBinding *hooks; /* function hooks this mod implements (T004); NULL/0 = none */
  uint32_t hook_count;
  GenesisModLogSink log_sink; /* host.log lines and fault diagnostics; NULL = discarded */
  void *log_ctx;
  /* ---- mod state (SEG-138-T006, ADR 0299) ---- */
  uint8_t identity_sha256[32]; /* the package identity the state is tagged with; all zero = derived from mod_id (a test/hand-built mod) */
  uint32_t state_schema;       /* the mod's declared state schema, required (>= 1) when `state.mod` is granted; 0 otherwise */
  const GenesisHookSiteInfo *sites; /* the program's generated hook site table: lets hook call frames in flight be saved (by index) and restored; NULL = a state with a frame in flight is not capturable */
  uint32_t site_count;
  const SegaGenesisRenderApi *render_api; /* SEG-138-T011: the host adapter's render-phase output API (typed ABI 2 feature); published as SegaGenesisFeatures::render only when overlay.rgba is granted; NULL = none */
  const SegaGenesisVdpHelpers *vdp_helpers; /* SEG-152-T004 (ADR 0320 decision 5): the host's pure VDP helpers (the ABI 1 GenesisCompatHost functions); published as SegaGenesisFeatures::vdp only when frame.vdp is granted; NULL = none */
  const GenesisModAudioEntry *audio; /* SEG-145-T008: the declared audio mappings (copied at attach); NULL/0 = no verified audio interface */
  uint32_t audio_count;
} GenesisModConfig;

/* ADR 0320 (SEG-152-T003): the producer's data of one produced frame, handed to an exact-frame provider (one that REQUIRES frame.vdp). Every pointer is read during the call only and is COPIED into scheduler-owned buffers
 * (`lines` has the layout of GenesisCompatLine / SegaGenesisFrameLine). work_ram NULL = no valid previous-onset copy or frame.work_ram not granted. */
typedef struct GenesisModExactFrame {
  const uint8_t *work_ram;       /* 65536 bytes (the previous-onset copy) or NULL */
  const uint8_t *vram;           /* 65536 bytes: the frame-start VRAM (vram_frame_start) or the producer's end-of-frame VRAM */
  const SegaGenesisFrameLine *lines;
  uint32_t line_count;
  uint32_t width, height, side, requested;
  uint32_t vram_frame_start;     /* 1 = a journaled frame (vram is the frame-start state) */
} GenesisModExactFrame;

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

typedef struct GenesisModAudioRow {
  char mapping[SEGA_MOD_AUDIO_NAME_BYTES];
  char name[SEGA_MOD_AUDIO_NAME_BYTES];
  uint32_t kind, offset, width, value;
} GenesisModAudioRow;

typedef struct GenesisModScheduler {
  /* ---- fixed after attach ---- */
  GenesisModConfig config;
  GenesisFrameHooks hooks;
  SegaModHost host;
  SegaGenesisFeatures features;
  GenesisModRange ranges[GENESIS_MOD_RANGES_MAX];
  uint32_t rom_base[GENESIS_MOD_RANGES_MAX]; /* offset of range i's bytes in config.rom_data (ROM ranges only) */
  GenesisRuntime *runtime;
  GenesisFunctionHooks function_hooks; /* the callback set and call-frame stack of the static function hooks (runtime->function_hooks points here when hook phases are registered) */
  GenesisModHookBinding bindings[GENESIS_MOD_HOOKS_MAX];
  uint8_t identity[32];     /* the effective identity (config or derived) */
  char fault_cause_store[32]; /* backing storage of fault.cause after a restore (the live value otherwise points at a static string) */
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
  uint32_t rom_reads, rom_read_bytes; /* ROM read budget of the callback in flight */
  char staged_log[GENESIS_MOD_LOG_LINES_MAX][GENESIS_MOD_LOG_LINE_BYTES + 1U];
  uint32_t staged_log_count;
  int fault_status; /* SegaModStatus of the first violation (SEGA_MOD_OK = none) */
  /* ---- the function hook in flight (hook-entry / hook-exit callbacks) ---- */
  const GenesisHookSiteInfo *hook_site;
  uint32_t hook_entry_sp;
  uint8_t hook_replaced;       /* the in-flight hook-entry callback asked to replace the function (decided when the callback returns) */
  uint32_t staged_reg_set;     /* register-view slots a replacement staged (bit per slot) */
  uint32_t staged_reg[32];
  uint64_t hook_calls[2];      /* callbacks delivered: [0] hook-entry, [1] hook-exit */
  uint64_t hook_skips[4];      /* by GenesisFunctionHooks::skipped kind (1 abandoned, 2 unproven caller, 3 frame stack full) */
  uint32_t hook_skip_logged;   /* one diagnostic line per kind (bit per kind), counters count them all */
  const char *external_cause; /* a fault reported by a host-side adapter (genesis_mod_fault_current), NULL = none */
  /* ---- declared audio interface (SEG-145-T008) ---- */
  SegaModAudioApi audio_api;
  GenesisModAudioRow audio[GENESIS_MOD_AUDIO_ENTRIES_MAX];
  uint32_t audio_count;
  uint32_t audio_observed[GENESIS_MOD_AUDIO_ENTRIES_MAX]; /* `render`: the value each event row's location held after frame-end (copied only when audio.event.read is granted; never the whole work RAM) */
  uint8_t audio_observed_valid;
  /* ---- render copy (only when frame.work_ram is granted) ---- */
  uint8_t render_copy[GENESIS_MOD_WORK_RAM_BYTES];
  uint8_t render_copy_valid;
  /* ---- exact-frame provider (ADR 0320; fixed buffers, no per-frame allocation) ---- */
  uint8_t exact_frame;          /* the provider requires frame.vdp: its render callback runs at the producer seam only */
  uint64_t exact_calls;         /* exact-frame callbacks delivered */
  SegaGenesisFrameView view;
  uint8_t view_vram[GENESIS_MOD_WORK_RAM_BYTES];
  SegaGenesisFrameLine view_lines[SEGA_GEN_FRAME_MAX_LINES];
  /* ---- committed write log ---- */
  GenesisModWriteRecord write_log[GENESIS_MOD_WRITE_LOG_MAX];
  uint32_t write_log_count;
  uint64_t write_log_dropped;
  GenesisSha256 write_stream; /* streaming SHA-256 over EVERY committed write (retained or not), see genesis_mod_write_log_digest */
  uint64_t write_total;       /* committed writes, retained + dropped */
} GenesisModScheduler;

/* Attaches `scheduler` to `runtime` (sets runtime->frame_hooks). Returns 0, or 1 with *rejection set to a stable class: the SegaModNegotiation classes (E_ABI_*, E_CAPABILITY_UNSUPPORTED),
 * E_PHASE_UNSUPPORTED (no phase, an unknown phase, or a hook phase without bindings), E_HOOK_BINDING (a bad or repeated hook id), E_RANGE_DECLARATION (a declared range outside the space, empty, or with a mode whose capability is not granted) and
 * E_PROVIDER_ENTRY (a provider without a `phase` entry or an invalid config). Nothing is attached on rejection. The runtime must not be running. */
int genesis_mod_scheduler_attach(GenesisModScheduler *scheduler, GenesisRuntime *runtime, const GenesisModConfig *config, const char **rejection);
/* ADR 0320: runs the exact-frame `render` callback of an exact-frame provider (one that required frame.vdp) for the frame being produced: `render` with host->frame = the produced frame (the runtime's frame count - 1),
 * the previous-onset work-RAM copy and an immutable host-owned view of `frame`; SegaGenesisFeatures::frame is non-NULL only during the call. Returns 1 when the callback ran and did not fault (the caller may then commit what
 * the adapter staged), 0 otherwise (not an exact-frame provider, disabled, no render phase, bad input or a fault). The ordinary after-snapshot `render` never runs for such a provider. */
int genesis_mod_scheduler_present(GenesisModScheduler *scheduler, const GenesisModExactFrame *frame);
/* Detaches: clears the hooks and the pad interception. The scheduler may then be discarded. */
void genesis_mod_scheduler_detach(GenesisModScheduler *scheduler);

/* The capabilities (bitset by id) this scheduler can grant: host.log, memory.read/write, input.read/intercept, frame.work_ram (render copy), overlay.rgba (render output, collected by the adapter),
 * audio.command.send and audio.event.read (SEG-145-T008, only through the declared mappings of the config). */
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

/* SEG-138-T006 (ADR 0299): fills the snapshot seam so the mod's state joins the Genesis save-state / rewind container (`MODSTATE`, GENESIS_SNAPSHOT_FLAG_MOD). Point
 * GenesisSnapshotMachine::mod at `*out`; `*out` references `scheduler` and must not outlive it. What is saved: the disabled/fault state, the input latch and the interception the
 * guest reads through, callback and hook counters, the replay witness (the running write digest), function-hook frames in flight, and -- with `state.mod` -- the mod's own bounded blob
 * (`save_state`, <= 64 KiB). A guest-affecting mod (memory.write, input.intercept, hook.function, audio.command.send) without `state.mod` makes capture fail with
 * GENESIS_SNAPSHOT_MOD_UNAVAILABLE (E_STATE_UNAVAILABLE). Restore fails closed with MOD_MISMATCH (other identity/schema/capabilities/phases) or MOD_REJECTED (malformed or refused state). */
void genesis_mod_snapshot_state(GenesisModScheduler *scheduler, GenesisSnapshotModState *out);

#ifdef __cplusplus
}
#endif
#endif
