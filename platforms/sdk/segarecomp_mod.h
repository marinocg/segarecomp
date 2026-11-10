/* segarecomp_mod.h -- native SDK ABI 2 core (docs/architecture/native-sdk-abi-2.md, ADR 0283).
 *
 * Strict C11 plus <stddef.h> and <stdint.h> only. This is NOT a sandbox: a native provider is trusted code compiled into the program. ABI 1 (platforms/genesis/sdk/segarecomp_genesis_patch.h)
 * is unchanged and coexists; one source exports exactly one entry symbol. A provider includes its platform header (segarecomp_mod_genesis.h, segarecomp_mod_sms.h or segarecomp_mod_scd.h),
 * which includes this file and pins the platform; the host passes -DSEGA_MOD_TARGET_PLATFORM=<id> and -DSEGA_MOD_TARGET_ABI_MAJOR=2, so a provider written for another platform or ABI
 * fails to compile with a stable class name in the message.
 *
 * Evolution (ABI 2 section 8): append-only. New fields go at the end or in `reserved`; new enum values and capability ids only at the end; nothing is removed, reordered or changes meaning.
 */
#ifndef SEGARECOMP_MOD_H
#define SEGARECOMP_MOD_H

#include <stddef.h>
#include <stdint.h>

#define SEGA_MOD_ABI_MAJOR 2U
#define SEGA_MOD_ABI_MINOR 0U /* a host with a larger minor accepts a provider requiring a smaller or equal minor */

#if defined(SEGA_MOD_TARGET_ABI_MAJOR) && (SEGA_MOD_TARGET_ABI_MAJOR != SEGA_MOD_ABI_MAJOR)
#error "E_ABI_HEADER_MISMATCH: the manifest abi.native differs from SEGA_MOD_ABI_MAJOR of this header"
#endif

/* Preprocessor-visible platform ids (an enum constant cannot be tested by #if); equal to SegaModPlatform. */
#define SEGA_MOD_PLATFORM_ID_GENESIS 1
#define SEGA_MOD_PLATFORM_ID_MASTER_SYSTEM 2
#define SEGA_MOD_PLATFORM_ID_SEGA_CD 3

typedef enum SegaModPlatform {
  SEGA_MOD_PLATFORM_GENESIS = 1,
  SEGA_MOD_PLATFORM_MASTER_SYSTEM = 2,
  SEGA_MOD_PLATFORM_SEGA_CD = 3
} SegaModPlatform;

typedef enum SegaModPhase {
  SEGA_MOD_PHASE_RESET = 1,
  SEGA_MOD_PHASE_FRAME_BEGIN,
  SEGA_MOD_PHASE_INPUT,
  SEGA_MOD_PHASE_POST_INPUT,
  SEGA_MOD_PHASE_HOOK_ENTRY,
  SEGA_MOD_PHASE_HOOK_EXIT,
  SEGA_MOD_PHASE_FRAME_END,
  SEGA_MOD_PHASE_RENDER
} SegaModPhase;

typedef enum SegaModStatus {
  SEGA_MOD_OK = 0,
  SEGA_MOD_E_CAPABILITY = 1,
  SEGA_MOD_E_SPACE = 2,
  SEGA_MOD_E_RANGE = 3,
  SEGA_MOD_E_ALIGNMENT = 4,
  SEGA_MOD_E_READONLY = 5,
  SEGA_MOD_E_NOT_OWNER = 6,
  SEGA_MOD_E_BUDGET = 7,
  SEGA_MOD_E_PHASE = 8
} SegaModStatus;

/* Capability ids (ABI 2 section 4; append-only; 0 and unlisted ids are reserved). `granted` is a bitset indexed by id. */
enum {
  SEGA_MOD_CAP_HOST_LOG = 1,
  SEGA_MOD_CAP_FRAME_WORK_RAM = 2,
  SEGA_MOD_CAP_FRAME_VDP = 3,
  SEGA_MOD_CAP_FRAME_SMS_VDP = 4,
  SEGA_MOD_CAP_SCENERY_WORDS = 5,
  SEGA_MOD_CAP_OVERLAY_RGBA = 6,
  SEGA_MOD_CAP_MEMORY_READ = 7,
  SEGA_MOD_CAP_MEMORY_WRITE = 8,
  SEGA_MOD_CAP_INPUT_READ = 9,
  SEGA_MOD_CAP_INPUT_INTERCEPT = 10,
  SEGA_MOD_CAP_HOOK_FUNCTION = 11,
  SEGA_MOD_CAP_STATE_MOD = 12,
  SEGA_MOD_CAP_AUDIO_EVENT_READ = 13,
  SEGA_MOD_CAP_AUDIO_COMMAND_SEND = 14,
  SEGA_MOD_CAP_AUDIO_HOST_PLAY = 15
};

/* Host services. The host fills it; `size` is the host's sizeof; a provider reads only fields below `size`. Every pointer may be NULL when its capability was not granted. */
typedef struct SegaModHost {
  uint32_t size, abi_major, abi_minor, platform; /* platform: SegaModPlatform */
  uint64_t granted[2];                           /* required capabilities are always set, optional ones only if granted */
  const void *platform_features;                 /* the typed per-platform table of the platform header; never NULL for a supported platform */
  uint32_t platform_features_size;
  uint32_t phase;                                /* SegaModPhase of the current callback */
  uint64_t frame;                                /* guest frame counter, deterministic */
  void (*log)(const char *ascii, uint32_t len);  /* host.log: <= 256 bytes per line, <= 16 lines per callback */
  int (*read)(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value);
  int (*write)(uint32_t space, uint32_t offset, uint32_t width, uint32_t value);
  int (*input_get)(uint32_t port, uint32_t *buttons);
  int (*input_set)(uint32_t port, uint32_t buttons);
  uint32_t reserved[16]; /* zero; future append-only fields follow */
} SegaModHost;

typedef struct SegaModProvider {
  uint32_t size, abi_major, required_minor, platform;
  const char *name;
  uint32_t implementation_version;
  int (*init)(void *state, const SegaModHost *host);
  void (*phase)(void *state, const SegaModHost *host);
  int (*hook)(void *state, const SegaModHost *host, uint32_t hook_id);
  int (*save_state)(void *state, uint8_t *out, uint32_t cap, uint32_t *len);
  int (*load_state)(void *state, const uint8_t *in, uint32_t len);
  void *reserved[8]; /* NULL */
} SegaModProvider;

/* The ABI 2 entry symbol (ABI 1 uses segarecomp_patch_provider). */
const SegaModProvider *segarecomp_mod_provider(void **state_out);

/* Load-time negotiation (ABI 2 section 5, classes in the table below). The manifest's required capability set arrives as `required` (bitset by id). */
typedef enum SegaModNegotiation {
  SEGA_MOD_NEG_OK = 0,
  SEGA_MOD_NEG_E_ABI_UNSUPPORTED,
  SEGA_MOD_NEG_E_ABI_PLATFORM,
  SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW,
  SEGA_MOD_NEG_E_ABI_TABLE_SIZE,
  SEGA_MOD_NEG_E_ABI_ENTRY_MISSING,
  SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED
} SegaModNegotiation;

static inline int sega_mod_is_granted(const SegaModHost *host, unsigned id) {
  return id < 128U && ((host->granted[id / 64U] >> (id % 64U)) & 1U) != 0U;
}

static inline SegaModNegotiation sega_mod_negotiate(const SegaModProvider *provider, const SegaModHost *host, const uint64_t required[2]) {
  unsigned i;
  if (provider == NULL) return SEGA_MOD_NEG_E_ABI_ENTRY_MISSING;
  if (provider->abi_major != SEGA_MOD_ABI_MAJOR || host->abi_major != SEGA_MOD_ABI_MAJOR) return SEGA_MOD_NEG_E_ABI_UNSUPPORTED;
  if (provider->platform != host->platform) return SEGA_MOD_NEG_E_ABI_PLATFORM;
  if (provider->required_minor > host->abi_minor) return SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW;
  if (provider->size < sizeof(SegaModProvider) || (provider->size % sizeof(void *)) != 0U) return SEGA_MOD_NEG_E_ABI_TABLE_SIZE;
  for (i = 0; i < 128U; ++i)
    if (((required[i / 64U] >> (i % 64U)) & 1U) != 0U && !sega_mod_is_granted(host, i)) return SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED; /* a required capability is never silently dropped */
  return SEGA_MOD_NEG_OK;
}

/* The stable diagnostic class of a negotiation result ("" for SEGA_MOD_NEG_OK). */
static inline const char *sega_mod_negotiation_class(SegaModNegotiation n) {
  switch (n) {
    case SEGA_MOD_NEG_E_ABI_UNSUPPORTED: return "E_ABI_UNSUPPORTED";
    case SEGA_MOD_NEG_E_ABI_PLATFORM: return "E_ABI_PLATFORM";
    case SEGA_MOD_NEG_E_ABI_MINOR_TOO_NEW: return "E_ABI_MINOR_TOO_NEW";
    case SEGA_MOD_NEG_E_ABI_TABLE_SIZE: return "E_ABI_TABLE_SIZE";
    case SEGA_MOD_NEG_E_ABI_ENTRY_MISSING: return "E_ABI_ENTRY_MISSING";
    case SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED: return "E_CAPABILITY_UNSUPPORTED";
    case SEGA_MOD_NEG_OK: break;
  }
  return "";
}

#endif /* SEGARECOMP_MOD_H */
