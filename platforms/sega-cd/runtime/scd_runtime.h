#ifndef SEGARECOMP_RUNTIME_SEGA_CD_SCD_RUNTIME_H
#define SEGARECOMP_RUNTIME_SEGA_CD_SCD_RUNTIME_H

/* SEG-062 (ADR 0128): the Sega CD machine runtime ABI. Strict C11; links the Genesis runtime only for the VDP device, frame
 * export and the C SHA-256; never links C++.
 *
 * One ScdMachine holds two MC68000 processor instances (index 0 = main, 1 = sub) of the same generated semantics. Generated code
 * for processor k names only m->cpu[k] and passes the constant k to the bus; every function returns 0 to continue or 1 after
 * recording a typed stop in m->stop. Nothing here decodes guest instructions: computed control selects among the generated
 * program's compiled entries (ScdProgram tables), and RAM-resident code is checked by byte comparison only. */

/* Windows host support, as in the Genesis runtime.h: this header is the first project include of every Sega CD runtime and generated
 * translation unit, so it owns the CRT feature selection before any CRT header is seen. */
#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS 1
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../../../libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h"
#include "../../../libs/cpu/m68k/include/segarecomp/cpu/m68k/timing_core.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GenesisFrameArtifact; /* SEG-078-T001: presented frames (genesis runtime frame artifact) */

#define SCD_MAIN 0U
#define SCD_SUB 1U

/* SEG-078-T002 (ADR 0204): the version of the program's host-facing contract (command line, exit statuses, `--describe` and the report
 * fields a launcher reads). A launcher accepts exactly the versions it knows; an older program is stale, never guessed at. */
#define SCD_RUNTIME_ABI 1U

/* Integer clock domain (contract section 2): ticks per second = lcm(53693175, 12500000, 75). */
#define SCD_TICKS_PER_SECOND UINT64_C(26846587500000)
#define SCD_MAIN_CYCLE_TICKS UINT64_C(3500000)
#define SCD_SUB_CYCLE_TICKS UINT64_C(2147727)
#define SCD_CDD_TICK_TICKS UINT64_C(357954500000)
/* SEG-071-T002 (ADR 0177): the BIOS-image profile locks the sub clock to the video line as the independent reference does: 796 sub
 * cycles per 1,710,000,000-tick line (3,420 master clocks), and the 75 Hz drive tick examined only at line ends (an accumulator that
 * gains 9,552 per line and fires at 2,000,000). The synthetic profile keeps the exact 12.5 MHz clock and the exact 75 Hz grid. */
#define SCD_SUB_CYCLE_TICKS_LINE_LOCKED UINT64_C(2148241)
/* SEG-071-T002 (ADR 0177): the main bus's DRAM refresh delay (BIOS-image profile): when an instruction is about to execute at or after
 * the refresh deadline the main processor loses 2 cycles (14 master clocks) and the next deadline is 128 cycles (896 master clocks)
 * after that instant (the independent reference's "68K bus access refresh delay (Mega Drive / Genesis specific)"). */
#define SCD_MAIN_REFRESH_PERIOD_TICKS (UINT64_C(896) * UINT64_C(500000))
#define SCD_MAIN_REFRESH_DELAY_TICKS (UINT64_C(14) * UINT64_C(500000))
/* The Z80 bus access latency (the reference's z80_read_byte/z80_write_byte): each main access to the Z80 window $A00000-$A0FFFF takes
 * one more cycle (7 master clocks); a long access reaches the bus as two. */
#define SCD_MAIN_Z80_ACCESS_TICKS (UINT64_C(7) * UINT64_C(500000))
/* The BIOS-image profile's video phase (ADR 0177 decision 4): the main processor leaves reset 229 lines + 1,004 master clocks (784,184)
 * into the 262-line frame whose origin is the vertical-blank onset (the reference's measured-on-hardware hard-reset point, GPGX
 * core/genesis.c gen_reset), so (the reset point rounded down to a multiple of 7, 784,182) the first onset is 896,040 - 784,182 = 111,858 master clocks after power-on,
 * and the vertical interrupt is requested 788 (H40) / 770 (H32) master clocks after each onset (core/vdp_ctrl.c VINT_Hxx_MCYCLE). */
#define SCD_BIOS_FIRST_ONSET_MCLK UINT64_C(111858)
#define SCD_VINT_H40_MCLK UINT64_C(788)
#define SCD_VINT_H32_MCLK UINT64_C(770)
#define SCD_LINE_TICKS_MACHINE UINT64_C(1710000000)
#define SCD_CDD_ACC_PER_LINE INT64_C(9552)
#define SCD_CDD_ACC_PER_TICK INT64_C(2000000)
#define SCD_CDD_ACC_PER_SUB_CYCLE INT64_C(12)
#define SCD_TIMER_UNIT_SUB_CYCLES UINT64_C(384)
#define SCD_DMA_SUB_CYCLES_PER_BYTE UINT64_C(4)
#define SCD_STOPWATCH_UNIT_TICKS (SCD_TIMER_UNIT_SUB_CYCLES * SCD_SUB_CYCLE_TICKS) /* 384 sub cycles = 30.72 us per count */

#define SCD_PRG_RAM_BYTES 0x80000U
#define SCD_WORD_RAM_BYTES 0x40000U
#define SCD_WORK_RAM_BYTES 0x10000U
#define SCD_CDC_BUFFER_BYTES 0x4000U
#define SCD_BACKUP_RAM_BYTES 0x2000U   /* SEG-065-T002: 8 KiB internal backup RAM (sub $FE0000-$FEFFFF, odd bytes, mirrored) */
#define SCD_CDD_TOC_STATUS 0x09U   /* GPGX CD_TOC: drive stopped with a disc loaded */
#define SCD_CDD_PLAY 0x01U
#define SCD_CDD_SEEK 0x02U
#define SCD_CDD_END 0x0CU

typedef enum ScdStopClass {
  SCD_STOP_NONE = 0,
  SCD_STOP_UNMAPPED,
  SCD_STOP_BIOS_ABSENT,
  SCD_STOP_OPEN_BUS_READ,
  SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED,
  SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED,
  SCD_STOP_BACKUP_RAM_UNSUPPORTED,
  SCD_STOP_PCM_UNSUPPORTED,
  SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
  SCD_STOP_VDP_ACCESS_UNSUPPORTED,
  SCD_STOP_VDP_DMA_UNSUPPORTED,
  SCD_STOP_ADDRESS_ERROR,
  SCD_STOP_ENTRY_NOT_COMPILED,
  SCD_STOP_RETURN_TARGET_NOT_COMPILED,
  SCD_STOP_CODE_BYTES_MISMATCH,
  SCD_STOP_INSTRUCTION_UNSUPPORTED,
  SCD_STOP_EXCEPTION_VECTOR_NOT_COMPILED,
  SCD_STOP_EXCEPTION_STACK_INVALID,
  SCD_STOP_TRACE_UNSUPPORTED,
  SCD_STOP_INTERRUPT_SOURCE_UNSUPPORTED,
  SCD_STOP_CDC_DESTINATION_UNSUPPORTED,
  SCD_STOP_CDD_COMMAND_UNSUPPORTED,
  SCD_STOP_CD_READ_AUDIO_TRACK,
  SCD_STOP_CD_READ_FAILED,
  SCD_STOP_INTERNAL,
  /* SEG-063-T001 (ADR 0127, ADR 0132): overlay selection among a group's candidate images. */
  SCD_STOP_IMAGE_UNKNOWN,              /* the instruction at PC is no admitted candidate's: an unlisted module or other bytes */
  SCD_STOP_IMAGE_BYTES_MISMATCH,       /* a candidate's instruction is at PC but its image holds bytes no admitted image explains */
  SCD_STOP_IMAGE_PARTIALLY_OVERWRITTEN,/* the live extent is a mixture of admitted images: no one image is coherent */
  SCD_STOP_AMBIGUOUS_IMAGE,            /* more than one overlapping candidate is fully live */
  SCD_STOP_IMAGE_ENTRY_NOT_COMPILED,   /* the selected candidate has no compiled entry at PC */
  SCD_STOP_STALE_IMAGE_SELECTION,      /* the selected image was overwritten and execution re-entered it outside a declared root */
  /* SEG-064-T002 (ADR 0142): the BIOS-image profile. */
  SCD_STOP_MATERIALIZED_IMAGE_UNVERIFIED, /* at the sub runnable epoch, live PRG RAM equals no admitted materialized image */
  SCD_STOP_MATERIALIZATION_CAPTURED,   /* build pass only: an unknown epoch image or a RAM frontier was written for admission */
  /* SEG-064-T003: main-side Genesis devices of the BIOS-image profile. */
  SCD_STOP_MAIN_DEVICE_UNSUPPORTED,    /* a main access to the I/O, Z80 or sound area outside the modelled shapes, or one that would
                                          observe the unmodelled Z80 execution */
  /* SEG-064-T004: the BIOS-image profile's accepted boundary (a deliberate end, not a failure). */
  SCD_STOP_APPLICATION_HANDOFF,        /* the main processor is about to execute its first instruction at the documented IP entry */
  SCD_STOP_CLASS_COUNT
} ScdStopClass;

const char *scd_stop_class_name(ScdStopClass stop_class);

typedef struct ScdStop {
  ScdStopClass stop_class;
  uint32_t cpu;         /* processor index, or 2 for a device-side stop */
  uint32_t pc;          /* instruction address (generated source) */
  uint32_t address;     /* accessed address or other detail */
  uint32_t width;       /* 1, 2, 4 or 0 */
  uint32_t is_write;
  uint64_t tick;
} ScdStop;

typedef enum ScdRunResult {
  SCD_RUN_IDLE = 0,       /* success: no processor can run and no source can wake one */
  SCD_RUN_STOPPED,        /* typed stop in m->stop */
  SCD_RUN_BUDGET,         /* --instruction-budget exhausted */
  SCD_RUN_DEADLINE        /* paused at the deadline; resumable */
} ScdRunResult;

typedef struct ScdCpu {
  uint32_t d[8];
  uint32_t a[8];
  uint32_t usp;           /* inactive stack pointer slot (ADR 0043 §6) */
  uint32_t pc;
  uint16_t sr;
  SegarecompM68kInterruptState irq;
  uint64_t next;          /* tick at which the next instruction starts */
  uint64_t cycle_ticks;   /* ticks per CPU cycle */
  uint64_t retired;
  uint64_t cycles;
  uint64_t trace;         /* FNV-1a over (pc, registers, sr) after every retirement: per-processor execution trace digest */
  uint32_t prefetch;      /* SEG-064-T003 (BIOS-image profile): instruction-stream address of the word the processor's prefetch holds
                             while the current instruction reads its operands (set by the generated instruction; 0 = not established) */
} ScdCpu;

/* Build-time disc table (ADR 0129): emitted as constant data by the generated program. */
typedef struct ScdDiscFile {
  const char *name;
  uint64_t size;
  const char *sha256;     /* lowercase hex */
} ScdDiscFile;

typedef struct ScdDiscTrack {
  uint32_t number;
  uint32_t type;          /* 0 = mode1_2048, 1 = mode1_2352, 2 = audio */
  uint32_t file_index;
  uint32_t sector_size;
  uint64_t file_offset;
  uint32_t first_lba;
  uint32_t sector_count;
} ScdDiscTrack;

typedef struct ScdDisc {
  const ScdDiscFile *files;
  uint32_t file_count;
  const ScdDiscTrack *tracks;
  uint32_t track_count;
  const char *identity;
  void *handles[8];       /* runtime-owned FILE * per file */
} ScdDisc;

typedef enum ScdDiscError {
  SCD_DISC_OK = 0,
  SCD_DISC_TABLE_INVALID,
  SCD_DISC_FILE_MISSING,
  SCD_DISC_SIZE_MISMATCH,
  SCD_DISC_IDENTITY_MISMATCH,
  SCD_DISC_LBA_OUT_OF_RANGE,
  SCD_DISC_LBA_AUDIO,
  SCD_DISC_RAW_SECTOR_INVALID,
  SCD_DISC_IO,
  SCD_DISC_CDDA_FORMAT /* SEG-104: the sector is not in a 2,352-byte audio track */
} ScdDiscError;

const char *scd_disc_error_name(ScdDiscError error);
ScdDiscError scd_disc_open(ScdDisc *disc, const char *directory);
void scd_disc_close(ScdDisc *disc);
/* One Mode-1 user block by LBA (2048 bytes); raw sectors are checked for sync and header. */
ScdDiscError scd_disc_read_user_data(ScdDisc *disc, uint32_t lba, uint8_t out[2048]);
/* SEG-104 (ADR 0244): one CD-DA sector (2,352 bytes = 588 stereo frames) of an audio track by LBA, as canonical interleaved L0,R0,L1,R1,...
 * signed 16-bit samples (the file's little-endian pairs decoded explicitly, so the result is host-endianness independent). Refused with a
 * typed error for an LBA outside every track (SCD_DISC_LBA_OUT_OF_RANGE), a data track (SCD_DISC_CDDA_FORMAT: a Mode-1 sector is never
 * interpreted as audio), a track that is not 2,352-byte, a range beyond the verified file (SCD_DISC_IO) or a short read (SCD_DISC_IO). */
#define SCD_CDDA_SECTOR_BYTES 2352U
#define SCD_CDDA_SECTOR_FRAMES 588U
ScdDiscError scd_disc_read_cdda(ScdDisc *disc, uint32_t lba, int16_t out[1176]);
/* The track containing lba, or NULL. */
const ScdDiscTrack *scd_disc_track_of(const ScdDisc *disc, uint32_t lba);

struct ScdMachine;
typedef int (*ScdDispatchFunction)(struct ScdMachine *m);

/* The per-processor compiled program: a sorted list of compiled instruction addresses (the entry authority) and the dispatcher. */
typedef struct ScdProcessorProgram {
  const uint32_t *entries;
  uint32_t entry_count;
  ScdDispatchFunction dispatch;   /* dispatches m->cpu[k].pc */
  uint32_t entry_pc;              /* profile entry (main: IP entry; sub: SP entry) */
  uint32_t image_base;            /* execution base of the boot image */
  uint32_t image_disc_offset;     /* byte offset of the image inside the 32 KiB system area */
  uint32_t image_length;
} ScdProcessorProgram;

/* SEG-063-T001 (ADR 0127, ADR 0132): the finite build-time overlay registry. A candidate is one mapping of one image of the processor's
 * set; a group is a maximal run of overlapping candidates. The runtime compares live bytes with the candidate's granule digests
 * (comparison of known images only) and then calls the candidate's own dispatcher; it never decodes guest bytes. */
#define SCD_OVERLAY_GRANULE_SHIFT 6U
#define SCD_OVERLAY_MAX_GROUPS 16U
#define SCD_OVERLAY_MAX_CANDIDATES 16U
#define SCD_OVERLAY_MAX_GRANULES 8192U
#define SCD_OVERLAY_LOG 64U
#define SCD_IMAGE_NO_ENTRY 2  /* a candidate dispatcher's result for an address without a compiled entry */

typedef struct ScdOverlayCandidate {
  uint32_t image_id;               /* ImageId (1-based) in the processor's ExecutableImageSet */
  const char *name;                /* manifest module name, boot_ip or boot_sp */
  uint32_t start;                  /* execution range [start, start + length) */
  uint32_t length;
  const uint64_t *granule_digests; /* per 64-byte granule of the range: first 8 bytes (big-endian) of SHA-256 of its bytes */
  uint32_t granule_first;          /* start >> 6 */
  uint32_t granule_count;
  const uint32_t *entries;         /* sorted compiled instruction addresses of this candidate */
  uint32_t entry_count;
  const uint8_t *const *entry_bytes;   /* per entry: the instruction's verification bytes (NULL: a typed-stop entry) */
  const uint8_t *entry_lengths;
  const uint32_t *roots;           /* declared entry roots (sorted) */
  uint32_t root_count;
  ScdDispatchFunction dispatch;    /* this candidate's entries only; SCD_IMAGE_NO_ENTRY otherwise */
} ScdOverlayCandidate;

typedef struct ScdOverlayGroup {
  uint32_t cpu;
  uint32_t start;                  /* union of the candidates' ranges */
  uint32_t end;
  const ScdOverlayCandidate *candidates;  /* registry order */
  uint32_t candidate_count;
  uint32_t granule_first;
  uint32_t granule_count;
} ScdOverlayGroup;

typedef struct ScdOverlayProgram {
  const ScdOverlayGroup *groups;
  uint32_t group_count;
  const char *manifest_identity;   /* SHA-256 of the admitted module manifest */
} ScdOverlayProgram;

typedef struct ScdOverlayState {
  int32_t selected;                /* candidate index, or -1 */
  int32_t stale_from;              /* the candidate whose selection was invalidated by a write, until the next selection; or -1 */
  uint8_t all_dirty;               /* the whole range may have changed (a bank, window or Word RAM ownership change) */
  uint8_t any_dirty;
  uint32_t dirty_lo, dirty_hi;     /* bounds (granule indices relative to the group) of the set bits */
  uint8_t dirty[SCD_OVERLAY_MAX_GRANULES / 8U];
  uint64_t selections;
  uint64_t revalidations;
  uint64_t invalidations;
  uint64_t retired[SCD_OVERLAY_MAX_CANDIDATES];  /* instructions retired through each candidate's compiled entries */
} ScdOverlayState;

typedef struct ScdOverlayEvent {
  uint64_t tick;
  uint32_t cpu, group, candidate, pc, kind;  /* kind: 1 selected, 2 invalidated */
} ScdOverlayEvent;

/* SEG-064-T002 (ADR 0142): the BIOS-image profile's build-time facts. A materialized sub image is identified by the length and
 * SHA-256 of PRG RAM [0, length) at the sub runnable epoch (length: end of the last non-zero byte, rounded up to 256 bytes). */
typedef struct ScdMaterializedIdentity {
  uint32_t length;
  const char *sha256;             /* lowercase hex */
} ScdMaterializedIdentity;

typedef struct ScdAuthentic {
  const char *bios_sha256;        /* the BIOS image the program was compiled from (lowercase hex) */
  uint32_t bios_size;
  const ScdMaterializedIdentity *sub_images;
  uint32_t sub_image_count;
  uint32_t ip_entry;              /* SEG-064-T004: the documented IP entry (main work RAM; contract section 9); 0 = no handoff boundary */
} ScdAuthentic;

/* SEG-100 (ADR 0221): file-first dynamic images. A dynamic image is an immutable disc file (a track-1 extent) whose instruction bodies were
 * compiled before launch for source offsets, with no guest base. At run time the machine may *bind* one to the live bytes the original game
 * loaded (scd_dynamic.c): the binding chooses which precompiled body serves a guest PC. It never decodes, lowers or compiles anything, and
 * every body still compares its instruction's bytes with the live bytes before it runs. */
#define SCD_DYN_MAX_BINDINGS 16U
#define SCD_DYN_MAX_IMAGE_BYTES 0x80000U /* PRG RAM: the largest executable region of a processor */
#define SCD_DYN_GRANULE_SHIFT 6U
#define SCD_DYN_GRANULES (SCD_DYN_MAX_IMAGE_BYTES >> SCD_DYN_GRANULE_SHIFT)
#define SCD_DYN_ANCHOR_BYTES 32U
#define SCD_DYN_MIN_RUN 64U
#define SCD_DYN_MAX_CANDIDATES 64U
#define SCD_DYN_LOG 64U

typedef struct ScdDynamicImage {
  uint32_t id;                /* 1-based, unique in the program (a body is processor-neutral: the image serves whichever processor loads it) */
  uint32_t first_lba;         /* absolute LBA of the first sector of the file (data track 1) */
  uint32_t length;            /* bytes */
  const char *sha256;         /* lowercase hex of the bytes (verified against the disc when the program was built) */
  const uint32_t *body_map;   /* per even source offset: 1-based index into the processor's body table, 0 = no compiled body */
} ScdDynamicImage;

typedef struct ScdDynamicProgram {
  const ScdDynamicImage *images;
  uint32_t image_count;
  const ScdDispatchFunction *const *body_slices;  /* slices of the body table, shared by both processors (index / body_slice_size selects the slice) */
  uint32_t body_slice_size;
  uint32_t body_count;
  const uint8_t *body_lengths;    /* per body (index - 1): the byte length of its instruction, 0 when the encoding has no body */
  uint32_t candidate_bytes;       /* report: source bytes of all images */
  uint32_t compiled_offsets;      /* report: image source offsets that have a body */
  uint32_t unique_bodies;         /* report */
  const char *policy;             /* report: the build-time candidate policy */
} ScdDynamicProgram;

typedef struct ScdDynamicBinding {
  uint8_t active;
  uint32_t image;          /* index into ScdDynamicProgram::images */
  uint32_t base;           /* guest address of the image's byte 0 */
  uint32_t lo, hi;         /* the image's extent at that base, guest addresses [lo, hi): what the binding can serve */
  uint32_t run_lo, run_hi; /* the exact-copy run that identified the image (report) */
  uint32_t granule_first;  /* lo >> SCD_DYN_GRANULE_SHIFT */
  uint64_t order, retired;   /* order: recency of use; the most recently used binding that still explains an instruction wins; the least recently used is evicted */
  uint8_t stale;             /* an instruction in the extent was not the image's when last tried (the binding is skipped for it, kept for the rest) */
  uint8_t dirty[SCD_DYN_GRANULES / 8U + 2U];    /* bit set: the 64-byte granule is not known to equal the image (outside the run, or written since) */
  uint8_t mutated[SCD_DYN_GRANULES / 8U + 2U];  /* bit set: a whole-granule comparison found it different (data the loader or the module changed); cleared by a write */
} ScdDynamicBinding;

enum {
  SCD_DYN_REFUSE_NONE = 0,
  SCD_DYN_REFUSE_NOT_EXECUTABLE,   /* the PC is not RAM the processor can fetch from, or is odd */
  SCD_DYN_REFUSE_LEDGER_OVERFLOW,  /* the delivery evidence is incomplete */
  SCD_DYN_REFUSE_NO_DELIVERY,      /* no candidate image has a delivered sector */
  SCD_DYN_REFUSE_NO_SOURCE,        /* no delivered candidate explains the live bytes */
  SCD_DYN_REFUSE_TOO_SHORT,        /* the best explanation is shorter than SCD_DYN_MIN_RUN */
  SCD_DYN_REFUSE_AMBIGUOUS,        /* several candidates explain the live bytes equally */
  SCD_DYN_REFUSE_BODY_NOT_COMPILED,/* bound, but the image has no precompiled body at this source offset */
  SCD_DYN_REFUSE_DISC_UNREADABLE,
  SCD_DYN_REFUSE_INSTRUCTION_DIFFERS, /* bound, but the live bytes of the instruction at the PC are not the image's: the instruction is not the image's */
  SCD_DYN_REFUSE_COUNT
};

typedef struct ScdDynamicEvent {
  uint64_t tick;
  uint32_t cpu, kind, pc, image, base;  /* kind: 1 bound, 2 invalidated (an instruction no longer equals the image), 3 replaced, 4 refused (image = reason) */
} ScdDynamicEvent;

#define SCD_DYN_MAX_IMAGES 4096U
#define SCD_DYN_CACHE_SECTORS 4U

typedef struct ScdDynamicState {
  uint8_t enabled;         /* the program registered dynamic images (scd_dynamic_register); idle otherwise */
  uint32_t cpu;            /* the processor whose instruction the dispatched body executes (read by the body into its `scd_k`) */
  ScdDynamicBinding binding[2][SCD_DYN_MAX_BINDINGS];
  uint64_t order;
  uint64_t binds, invalidations, replacements, rechecks, recheck_failures, granule_compares, granule_upgrades, lookups, lookup_hits;
  uint64_t binds_by_cpu[2];
  uint64_t scan_bytes, scans, retired;
  uint64_t refused[SCD_DYN_REFUSE_COUNT];
  uint32_t last_refusal;
  uint32_t ambiguity_run, ambiguity_count, ambiguity_images[8];   /* the last tie: the length of the equal runs, how many candidates tied, and (up to eight of) their image ids */
  uint32_t events;
  ScdDynamicEvent log[SCD_DYN_LOG];
  uint8_t bound_ever[SCD_DYN_MAX_IMAGES / 8U];   /* images identified at least once */
  uint8_t exercised[SCD_DYN_MAX_IMAGES / 8U];    /* images through which at least one instruction retired */
  /* SEG-101 (ADR 0227): per-image accounting of what actually retired. Observer-only (written after the body returned, read only by the report): the number
   * of instructions that retired through the image's bodies and the number of distinct even source offsets among them. `seen` is allocated on an image's
   * first retired instruction and released by scd_dynamic_release. */
  uint64_t image_retired[SCD_DYN_MAX_IMAGES];
  uint32_t image_offsets[SCD_DYN_MAX_IMAGES];
  uint8_t *image_seen[SCD_DYN_MAX_IMAGES];
  uint32_t cache_lba[SCD_DYN_CACHE_SECTORS];     /* a small direct-mapped cache of image sectors for the instruction and granule comparisons */
  uint8_t cache_valid[SCD_DYN_CACHE_SECTORS];
  uint8_t cache[SCD_DYN_CACHE_SECTORS][2048];
} ScdDynamicState;

typedef struct ScdProgram {
  ScdProcessorProgram processors[2];
  ScdDisc disc;
  uint32_t main_initial_ssp;
  const char *program_identity;   /* build identity: disc identity + generator version */
} ScdProgram;

/* --- Gate Array / devices ------------------------------------------------------------------------------------------------ */

/* Memory-mode register low byte (sub view $FF8003 bits 4-0; the main view masks PM1/PM0 and adds BK1/BK0). */
#define SCD_MODE_RET 0x01U
#define SCD_MODE_DMNA 0x02U
#define SCD_MODE_1M 0x04U

typedef struct ScdGateArray {
  uint8_t ctl;              /* $A12001 readback: bit 1 SBRQ, bit 0 SRES (1 = sub released from reset) */
  uint8_t sub_halted;       /* the sub's /HALT input (SRES = 0 or SBRQ = 1) */
  uint8_t sub_wait;         /* the sub waits for DTACK (2M Word RAM it does not own); main DMNA = 1 releases it */
  uint8_t prg_window;       /* main $020000-$03FFFF mapped to PRG RAM (follows the SBRQ readback when the halt state changes) */
  uint8_t ifl2;             /* main -> sub level-2 request flag ($A12000 bit 0) */
  uint8_t ien2_mirror;      /* $A12000 bit 7: IEN2 as last written to $FF8033 (kept across a peripheral reset) */
  uint8_t led;              /* $FF8000 (also seen at $FF8006 and $A12006) */
  uint8_t write_protect;    /* WP, units of $200 bytes */
  uint8_t prg_bank;         /* BK1:BK0 */
  uint8_t mode;             /* PM1 PM0 MODE DMNA RET (SCD_MODE_*) */
  uint8_t dmna_int;         /* internal swap/return request latched by main DMNA writes */
  uint8_t main_flag;
  uint8_t sub_flag;
  uint16_t comcmd[8];
  uint16_t comstat[8];
  uint8_t interrupt_enable; /* $FF8033 as written */
  uint8_t reg32_hi;         /* $FF8032 (storage only) */
  uint8_t pending;          /* pending sub interrupt levels, bit n = level n */
  uint8_t timer_reload;     /* $FF8031 */
  uint64_t timer_next;      /* tick of the next timer expiry, 0 = stopped */
  uint8_t cdd_ctrl_hi;      /* $FF8036 (1 = no audio track playing) */
  uint8_t cdd_ctrl_lo;      /* $FF8037 (bit 2 HOCK) */
  uint8_t scratch[0x98];    /* $FF8068-$FF80FF: register storage without function */
  /* SEG-064-T004: the $FF800C stopwatch is a free-running 12-bit count of 384-sub-cycle units on a grid anchored at `origin` (power-on,
   * or the last peripheral reset); the register reads the count minus `base`, the count at the last write (which clears it). */
  uint64_t stopwatch_origin;
  uint32_t stopwatch_base;
} ScdGateArray;

#define SCD_DMA_NONE 0U
#define SCD_DMA_RUNNING 1U
#define SCD_DMA_HALTED 2U
#define SCD_DMA_TARGET_2M 1U
#define SCD_DMA_TARGET_BANK0 2U
#define SCD_DMA_TARGET_BANK1 3U

typedef struct ScdCdc {
  uint8_t address;          /* $FF8005 */
  uint8_t destination;      /* $FF8004 bits 0-2 */
  uint8_t edt, dsr;         /* $FF8004 bits 7, 6 */
  uint16_t dma_address;     /* $FF800A */
  uint8_t ifstat, ifctrl, ctrl0, ctrl1;
  uint8_t irq;              /* asserted CDC /INT sources (DTEI, DECI) */
  uint16_t dbc, dac, pt, wa;
  uint8_t head[2][4];       /* [0] block header, [1] sub-header (selected by CTRL1 SHDREN) */
  uint8_t stat0, stat2, stat3;
  uint64_t decoder_end;     /* end tick of the decoder active period */
  uint16_t host_word;       /* $FF8008/$A12008: the CDC host-data register (reset value $FFFF; SEG-064-T004) */
  uint8_t dma_state;        /* SCD_DMA_*: the writer of a started transfer (none while the destination is invalid) */
  uint8_t dma_target;       /* SCD_DMA_TARGET_*, latched when the writer is selected */
  uint64_t dma_base;        /* tick from which the next bytes are counted */
  uint32_t dma_total;       /* bytes requested by the last DTRG (report) */
  uint32_t dma_done;        /* bytes moved since the last DTRG (report) */
  uint8_t buffer[SCD_CDC_BUFFER_BYTES + 2352];
} ScdCdc;

typedef struct ScdCdd {
  uint8_t tx[10];           /* $FF8042-$FF804B as written */
  uint8_t rx[10];           /* $FF8038-$FF8041 */
  uint8_t status;           /* drive status */
  uint8_t pending;          /* status taken when a play/seek starts on the next tick */
  uint32_t latency;         /* ticks before the drive delivers */
  int32_t lba;              /* current position (may be negative inside the lead-in) */
  uint32_t index;           /* current track index (== track count: past the last track) */
  uint64_t anchor;          /* origin of the 75 Hz grid (power-on, or the last peripheral reset) */
  uint64_t grid_line0;      /* BIOS-image profile (ADR 0177): the video line containing the anchor */
  int64_t grid_acc0;        /* BIOS-image profile: the drive accumulator at the anchor (-12 x the sub cycles already run in that line) */
  uint64_t next_tick;       /* next grid point to process while the drive is active */
  uint64_t delivered;       /* data blocks read while playing */
} ScdCdd;

/* SEG-064-T004 / SEG-102 (ADR 0233): the RF5C164 PCM chip's CPU-visible state (scd_pcm.c): the register file, the 64 KiB wave RAM and,
 * since SEG-102, the running wave-address pointer of each channel and the sample clock that advances them. No audio is generated (no
 * mixer, output buffer or callback exists); only state the sub processor can read back is kept exact. Reads other than the channel
 * pointers and the unverified RAM read path remain the typed stop pcm_unsupported. */
typedef struct ScdPcm {
  uint8_t env[8], pan[8], st[8];
  uint16_t fd[8], ls[8];
  uint8_t index;            /* CTRL: selected channel (0-7) */
  uint8_t bank;             /* CTRL: wave RAM bank (0-15, 4 KiB each) */
  uint8_t enabled;          /* CTRL bit 7 */
  uint8_t onoff;            /* ON/OFF register as written (a set bit turns the channel off) */
  uint8_t running;          /* channel run mask: bit n set = channel n ON (the complement of the last ON/OFF write; 0 after a reset) */
  uint32_t addr[8];         /* running wave address of each channel: 16.11 fixed point (bits 26-11 the wave RAM byte, 10-0 the fraction) */
  uint64_t origin;          /* tick of the last chip reset: sample events fall at origin + k x period (k = 0, 1, ...) */
  uint64_t samples;         /* sample events already applied (events strictly before the last synchronization instant) */
  uint64_t syncs, advanced, channel_steps, max_catchup; /* cost accounting (kept across a reset) */
  uint8_t ram[0x10000];
} ScdPcm;

/* SEG-064-T004: the graphics ASIC (scd_gfx.c): the host-visible registers, the parameters latched at the start, and the progress. */
typedef struct ScdGfx {
  uint8_t busy;
  uint8_t ctl;                 /* $FF8058 bits 2-0 */
  uint16_t map_base, cell_v, buffer_origin, offset, h_dots, trace_base; /* $5A, $5C, $5E, $60, $62, $66 as written */
  uint32_t trace_ptr, map_ptr, dot_mask, buffer_offset, buffer_start;
  uint8_t stamp_shift, map_shift;
  uint8_t lines_left;          /* the live $FF8064 low byte: written by the sub CPU (any time), counts down while running, 0 after completion (ADR 0249) */
  uint64_t base;               /* tick from which the next line's cost is counted */
  uint64_t line_ticks;
  uint64_t operations, lines_rendered;
  uint64_t vdot_reads;         /* byte reads of $FF8065 (an observation counter, no behavior) */
} ScdGfx;

/* SEG-078-T003 (ADR 0205): the delivery ledger - the disc blocks the CDC decoder has read into its buffer, as merged LBA ranges. A fact of the
 * device (what the machine itself read from the user's disc), the evidence a build pass offers when it asks where code in RAM came from. */
#define SCD_LEDGER_MAX 256U
typedef struct ScdLedgerRange {
  uint32_t first_lba, count;
} ScdLedgerRange;

/* SEG-104 (ADR 0244): the CD-DA output derived from the CD drive's own 75 Hz tick (scd_cdda.c). Each tick that plays an audio sector
 * (drive status PLAY, an audio track at/after its INDEX 01, no command pending) produces exactly that sector's 588 stereo frames, scaled by the
 * fader, and hands them to the optional host sink. The sink is a one-way output: nothing it does is visible to the machine, and with no sink and
 * `enabled` clear nothing is read from the disc at all. No clock, queue or host state enters the machine. */
typedef int (*ScdCddaSink)(void *context, const int16_t *frames, uint32_t count); /* interleaved L,R frames; 0 = accepted */

typedef struct ScdCdda {
  ScdCddaSink sink;         /* host output, or NULL */
  void *sink_context;
  uint8_t enabled;          /* produce samples (set by the host with a sink or an artifact); clear = the drive state alone, as before SEG-104 */
  uint8_t playing;          /* the last audio tick produced a sector */
  int32_t next_lba;         /* the LBA a contiguous continuation would play */
  uint32_t last_index;      /* track index of the last produced sector */
  uint16_t fader_register;  /* $FF8034 as written (12-bit volume in bits 15-4) */
  uint16_t fader_target;    /* 0-1024 (1024 = unity) */
  uint16_t fader_volume;    /* current volume; moves one step per produced sample frame toward the target */
  uint64_t sectors, frames;                     /* produced */
  uint64_t segments, interruptions, discontinuities, track_changes; /* playback runs started / ended / non-contiguous sectors / track boundaries crossed */
  uint64_t pregap_ticks;                        /* audio-track ticks before INDEX 01 (the drive reports "no audio playing": silent) */
  uint64_t suppressed_pending;                  /* play ticks silenced because a play/seek command was pending */
  uint64_t sink_refused;                        /* sectors the sink refused (the machine is unaffected) */
  uint64_t commands[16];                        /* CDD commands received, by code (plain counters) */
  uint64_t first_frame, last_frame;             /* vertical-blank onset counts at the first and the latest produced sector */
  FILE *log;                                    /* local diagnostic (--cdda-log): one line per run start / interruption / discontinuity; never read back */
} ScdCdda;

typedef struct ScdMachine {
  ScdCpu cpu[2];
  const ScdProgram *program;
  ScdGateArray ga;
  ScdCdc cdc;
  ScdCdd cdd;
  ScdDisc *disc;
  uint8_t prg_ram[SCD_PRG_RAM_BYTES];
  uint8_t word_ram[SCD_WORD_RAM_BYTES];   /* 2M layout; 1M banks are the word interleave of this array */
  uint8_t work_ram[SCD_WORK_RAM_BYTES];
  void *vdp_host;                         /* GenesisRuntime used only as the VDP device host (scd_vdp.c) */
  ScdStop stop;
  uint64_t now;                           /* tick of the instruction being executed */
  uint64_t instruction_budget;            /* remaining; 0 = exhausted */
  uint64_t device_log;                    /* FNV-1a over every device register access and device event */
  uint64_t prg_write_protect_drops;       /* sub writes ignored by the write protect */
  uint64_t ignored_writes;                /* writes with no effect (unused registers, unmapped Word RAM / PRG window, BIOS) */
  uint64_t sub_waits;                     /* sub instructions abandoned to wait for DTACK */
  uint8_t sub_wait_request;               /* set by a sub access that must wait for DTACK (the instruction is retried) */
  uint64_t sub_interrupts_taken[8];
  const ScdOverlayProgram *overlays;      /* NULL without disc-loaded modules (SEG-062 programs) */
  ScdOverlayState overlay[SCD_OVERLAY_MAX_GROUPS];
  ScdOverlayEvent overlay_log[SCD_OVERLAY_LOG];
  uint32_t overlay_events;                /* total events (the log keeps the first SCD_OVERLAY_LOG) */
  /* SEG-064-T002 (ADR 0142): BIOS-image profile state (all zero for the synthetic profile). */
  const ScdAuthentic *authentic;          /* the program's BIOS-image profile facts, or NULL */
  const uint8_t *bios;                    /* the verified BIOS image mapped at main $000000, or NULL */
  uint32_t bios_size;
  uint8_t sub_pristine;                   /* the sub has been reset and has not yet become runnable */
  uint8_t materialize;                    /* build pass: capture unknown epochs and RAM frontiers instead of stopping */
  uint32_t epochs;                        /* sub runnable epochs verified */
  int32_t epoch_image;                    /* index of the materialized image the last epoch matched, or -1 */
  uint64_t epoch_tick;                    /* tick of the last verified epoch */
  uint32_t capture_kind;                  /* 1 epoch snapshot, 2 RAM frontier (build pass) */
  uint32_t capture_length;                /* epoch snapshot length */
  uint8_t capture_bytes[8];               /* frontier live bytes */
  /* SEG-064-T003: main-side devices of the BIOS-image profile (scd_vdp.c). */
  uint32_t main_refresh_hold;             /* BIOS-image profile: the refresh delay was just charged; the instruction runs before an interrupt is taken */
  uint32_t main_irq_delay;                /* BIOS-image profile: instructions to execute before a main interrupt is recognised (VDP register #1 IE0 set while a vertical interrupt is pending) */
  uint64_t main_extra_ticks;              /* BIOS-image profile: bus wait added to the executing main instruction (Z80-window accesses), charged at its retire */
  uint32_t refresh_skip_cycles;           /* BIOS-image profile: the executing main instruction's refresh-skip cycles, set by its generated variant (ADR 0177) */
  uint64_t main_refresh_due;              /* BIOS-image profile: the instant from which the next main instruction loses the bus-refresh cycles */
  uint64_t vint_at;                       /* BIOS-image profile: tick at which the vertical interrupt of the last onset is requested (0: none due) */
  uint64_t vdp_onset;                     /* tick of the next VDP vertical-blank onset */
  uint8_t vint_pending;                   /* the VDP vertical interrupt is pending (requests level 6 while register #1 IE0 is set) */
  uint8_t z80_ran;                        /* the Z80 has been runnable (reset released, bus not requested): its RAM is not modelled */
  uint8_t hint_counter;                   /* SEG-106 (ADR 0250): the H-interrupt line counter (reloaded from VDP register #10) */
  uint8_t hint_pending;                   /* the H-interrupt request is latched (requests level 4 while register #0 IE1 is set) */
  uint64_t hint_next;                     /* the next scan line (absolute index on the first-onset grid) the counter has not stepped */
  uint64_t hint_at;                       /* tick of the next line that latches the request while one could rise, else 0 */
  uint64_t hint_latched;                  /* requests latched (rising edges of hint_pending) */
  uint16_t hint_vector;                   /* $A12006: the low word of the main level-4 autovector (BIOS ROM $72) */
  uint64_t vdp_frames;                    /* vertical-blank onsets processed */
  uint64_t main_interrupts_taken[8];
  /* SEG-064-T004 */
  ScdPcm pcm;
  ScdGfx gfx;
  void *access_trace;                     /* SEG-064-T004 diagnostic (--access-trace): run-length-coded sub Gate Array accesses, or NULL */
  void *write_log;                        /* SEG-079 diagnostic (--ram-write-log): main BIOS reads and PRG/work RAM writes with the writer PC, or NULL */
  void *pcm_trace;                        /* SEG-102 diagnostic (--pcm-trace): one line per pointer read, or NULL */
  void *event_log;                        /* SEG-064-T004 diagnostic (--event-log): "<vblank count> <event>" lines, or NULL */
  uint8_t verify_only;                    /* SEG-064-T004: the generated variants verify their bytes and return (the handoff entry check) */
  void *coverage;                         /* SEG-064-T004 diagnostic (--coverage): executed instruction starts per processor, or NULL */
  /* SEG-065-T002 (ADR 0147): the internal backup RAM is a machine input. Absent (the default), every access is the typed stop
   * backup_ram_unsupported; present (--backup-ram), sub accesses read and write these bytes (contract section 4). */
  /* SEG-065-T002 (ADR 0147, BIOS-image profile): a 68K->VDP DMA freezes the main processor until the transfer ends (GPGX
   * vdp_dma_update: m68k.cycles = dma_endCycles); the instruction that armed it then completes from that tick. */
  uint64_t main_dma_until;                /* 0, or the tick at which the armed DMA ends */
  uint8_t main_dma_multi;                 /* the transfer spans more than one slot window (GPGX re-anchors at its end) */
  uint64_t main_dma_bank_until;           /* SEG-103: end tick of the last transfer read from the 1M bank (0: the last transfer was not); observer for RET/MODE changes during the wait */
  uint64_t vdp_dma_bank_count;            /* SEG-103: transfers read from the 1M bank */
  uint64_t wordram_mode_changes_in_dma;   /* SEG-103: sub-side RET/MODE changes that took effect while such a transfer waited (an unmodelled hazard, ADR 0238 question 11) */
  uint64_t vdp_dma_count, vdp_dma_stall_ticks;
  uint8_t backup_ram_present;
  uint8_t backup_ram[SCD_BACKUP_RAM_BYTES];
  uint64_t backup_ram_reads, backup_ram_writes;
  /* SEG-070-T002 (ADR 0149): the optional timed standard-pad machine input (--input-script), or NULL: then no pad event exists and
   * every behaviour and report is that of SEG-065. */
  struct ScdInput *input;
  ScdDynamicState dyn;                    /* SEG-100: runtime bindings of the program's dynamic images (idle without them) */
  ScdLedgerRange delivered[SCD_LEDGER_MAX];
  uint32_t delivered_count;
  uint8_t delivered_overflow;             /* more ranges than the ledger holds: the evidence is incomplete and is never used */
  ScdCdda cdda;                           /* SEG-104: CD-DA output (inert unless a host enabled it) */
} ScdMachine;

/* The duration of one sub cycle in ticks for this machine's profile (ADR 0177). */
static inline uint64_t scd_sub_cycle_ticks(const ScdMachine *m) {
  return m->bios != NULL ? SCD_SUB_CYCLE_TICKS_LINE_LOCKED : SCD_SUB_CYCLE_TICKS;
}

/* Physical memories whose bytes may hold executable images (for write invalidation). */
#define SCD_MEMORY_PRG 0U
#define SCD_MEMORY_WORD 1U  /* offset in the 2M layout */
#define SCD_MEMORY_WORK 2U

/* --- generated-code ABI ---------------------------------------------------------------------------------------------------- */

int scd_read(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t *value, uint32_t pc);
int scd_write(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t value, uint32_t pc);
/* Compare the live bytes at `address` with the compiled instruction's image bytes (byte_identity; comparison only). */
int scd_code_guard(ScdMachine *m, uint32_t cpu, uint32_t address, const uint8_t *expected, uint32_t length);
/* Retire one instruction of `cycles` CPU cycles. */
int scd_retire(ScdMachine *m, uint32_t cpu, uint32_t cycles);
/* Synchronous exception: frame on the SSP, handler from the processor's vector table (must be a compiled entry). */
int scd_raise(ScdMachine *m, uint32_t cpu, uint32_t vector, uint32_t stacked_pc, uint32_t entry_cycles, uint32_t pc);
int scd_rte(ScdMachine *m, uint32_t cpu, uint32_t *restored_pc, uint32_t pc);
int scd_rtr(ScdMachine *m, uint32_t cpu, uint32_t *restored_pc, uint32_t pc);
void scd_enter_stop(ScdMachine *m, uint32_t cpu);
int scd_fail(ScdMachine *m, uint32_t cpu, ScdStopClass stop_class, uint32_t pc, uint32_t detail);
int scd_fail_access(ScdMachine *m, uint32_t cpu, ScdStopClass stop_class, uint32_t pc, uint32_t address, uint32_t width, int is_write);
int scd_compiled(const ScdMachine *m, uint32_t cpu, uint32_t address);
/* SEG-064-T002 (ADR 0142): 1 when the live bytes at `address` equal `expected` (comparison only; never a stop). */
int scd_code_matches(ScdMachine *m, uint32_t cpu, uint32_t address, const uint8_t *expected, uint32_t length);
/* 1 when `address` of `cpu` reads Word RAM: a code fetch there is a Word RAM observation point (the CDC DMA and the graphics ASIC are brought up first). */
int scd_is_word_ram(uint32_t cpu, uint32_t address);
/* Side-effect-free view of executable RAM (work RAM, the PRG-RAM window, PRG RAM, owned Word RAM) for byte comparison, or NULL. */
const uint8_t *scd_code_byte(const ScdMachine *m, uint32_t cpu, uint32_t address);
/* The byte the main processor's instruction stream holds at `address` (BIOS image or RAM), or NULL where it is neither. */
const uint8_t *scd_main_stream_byte(const ScdMachine *m, uint32_t address);
/* Overlay selection for an address the per-PC dispatch does not own (scd_overlay.c). */
int scd_overlay_dispatch(ScdMachine *m, uint32_t cpu);
/* Write invalidation: `length` bytes at `offset` of `memory` changed; and: which memory a processor sees at an address changed. */
void scd_overlay_note_write(ScdMachine *m, uint32_t memory, uint32_t offset, uint32_t length);
void scd_overlay_note_view_change(ScdMachine *m);
/* The write hook as the runtime calls it: free for programs without modules (no overlay registry attached). */
#define SCD_OVERLAY_NOTE_WRITE(m, memory, offset, length) \
  do { \
    if ((m)->overlays != NULL) scd_overlay_note_write((m), (memory), (offset), (length)); \
    if ((m)->dyn.enabled) scd_dynamic_note_write((m), (memory), (offset), (length)); \
  } while (0)
void scd_overlay_attach(ScdMachine *m, const ScdOverlayProgram *overlays);
/* The address at which processor `cpu` sees byte `offset` of `memory`; *visible = 0 when it does not see it (scd_overlay.c). */
uint32_t scd_overlay_view_of(const ScdMachine *m, uint32_t cpu, uint32_t memory, uint32_t offset, int *visible);

/* SEG-100 (ADR 0221, scd_dynamic.c): the file-first dynamic images. Dispatch of a PC no static case and no overlay group owns; a computed-control
 * or exception-handler query; the write and view-change notifications that keep bindings honest; the end-of-run report. */
int scd_dynamic_dispatch(ScdMachine *m, uint32_t cpu, ScdStopClass refusal_stop);
uint32_t scd_dynamic_exec_cpu(void); /* the processor whose instruction a dynamic body is executing (the neutral computed-control authority's key) */
const uint32_t *scd_dynamic_lookup(uint32_t cpu, uint32_t pc);
int scd_dynamic_known(ScdMachine *m, uint32_t cpu, uint32_t pc);
void scd_dynamic_note_write(ScdMachine *m, uint32_t memory, uint32_t offset, uint32_t length);
void scd_dynamic_note_view_change(ScdMachine *m);
/* A program with dynamic images registers them before it starts (generated main); the field is not part of ScdProgram so that every other program is unchanged. */
void scd_dynamic_register(const ScdDynamicProgram *program);
void scd_dynamic_attach(ScdMachine *m);
void scd_dynamic_release(ScdMachine *m);
void scd_dynamic_report(const ScdMachine *m, FILE *out);

/* --- machine ---------------------------------------------------------------------------------------------------------------- */

/* Establish the synthetic post-BIOS profile (contract section 9): IP and SP loaded from the disc's system area. */
int scd_machine_init(ScdMachine *m, const ScdProgram *program, ScdDisc *disc, uint64_t instruction_budget);
/* SEG-064-T002 (ADR 0142): the BIOS-image profile: `bios` (already verified against program->authentic) is mapped at main $000000,
 * the main processor resets from its vectors, the sub is held in reset; nothing is placed from the disc. */
int scd_machine_init_bios(ScdMachine *m, const ScdProgram *program, const ScdAuthentic *authentic, ScdDisc *disc, const uint8_t *bios,
                          uint32_t bios_size, uint64_t instruction_budget);
/* The sub's first runnable transition after a reset (ADR 0142 decision 3): verify live PRG RAM against the admitted images. */
int scd_sub_runnable_epoch(ScdMachine *m, uint32_t pc);
/* Length of the materialization snapshot of PRG RAM (end of the last non-zero byte, rounded up to 256). */
uint32_t scd_prg_snapshot_length(const ScdMachine *m);
ScdRunResult scd_machine_run(ScdMachine *m, uint64_t deadline);
/* ADR 0177 (BIOS-image profile main-processor bus timing; exposed for the synthetic fixtures): the refresh gate and the long-instruction
 * rule, the dynamic cycles of the instruction at the main pc that follow their own refresh bookkeeping, and the cycles of an autovector
 * interrupt exception of `cpu`. */
int scd_main_refresh_gate(ScdMachine *m, uint64_t ready);
void scd_main_refresh_note_instruction(ScdMachine *m, uint64_t ready, uint32_t cycles);
uint32_t scd_interrupt_entry_cycles(const ScdMachine *m, uint32_t cpu, uint32_t level);
uint64_t scd_machine_time(const ScdMachine *m);
void scd_machine_release(ScdMachine *m);

/* SEG-065-T002: install the backup RAM machine input (exactly SCD_BACKUP_RAM_BYTES bytes); 0 on success, -1 on a wrong length. */
int scd_backup_ram_install(ScdMachine *m, const uint8_t *bytes, size_t length);

/* --- SEG-070-T002 (ADR 0149): timed standard-pad machine input (scd_input.c) ------------------------------------------------- */

#define SCD_INPUT_MAX_EVENTS 4096U
#define SCD_INPUT_MAX_SCRIPT_BYTES 65536U
#define SCD_INPUT_MAX_LINE 256U
#define SCD_INPUT_LOG_MAX 65536U
typedef struct ScdInputEvent {
  uint64_t tick;  /* absolute machine tick since reset (500,000 ticks per master clock) */
  uint8_t port;   /* 1 or 2 */
  uint8_t press;  /* 1 press, 0 release */
  uint8_t mask;   /* GENESIS_PAD_* bits */
} ScdInputEvent;
typedef struct ScdInputRead {
  uint64_t tick;   /* machine time of the reading instruction */
  uint64_t onset;  /* vertical-blank onsets processed before it */
  uint8_t port;
  uint8_t value;
} ScdInputRead;
typedef struct ScdInput {
  ScdInputEvent *events;
  size_t count, next;
  uint8_t pad[2];                    /* the state applied so far, per port */
  ScdInputRead *log;                 /* data-port reads, in order (bounded; NULL when not logging) */
  size_t log_count, log_dropped;
  uint64_t reads[2];
  /* SEG-078-T001 (ADR 0204): live recording. A live input is an ordinary script whose events are appended at the machine's own
   * frame boundaries; `live_pad` is the state after the last appended event and `script_bytes` the size of its serialisation. */
  uint8_t live;
  uint8_t recording;                 /* the events are kept for a script; otherwise applied events are discarded (a long session has no limit) */
  size_t compacted;                  /* events discarded so far (the report counts them) */
  uint64_t live_last_tick;           /* the tick of the last appended event */
  uint8_t live_pad[2];
  size_t script_bytes;
} ScdInput;
/* Parse and validate a script (`error` receives the refusal class and `line` its 1-based line); `events` has room for
 * SCD_INPUT_MAX_EVENTS. Returns 0 on success. Nothing is applied. */
int scd_input_parse(const char *text, size_t length, ScdInputEvent *events, size_t *count, const char **error, unsigned *line);
/* Install a script into the machine (0 on success, otherwise the refusal class in *error); `log` enables the read log. */
int scd_input_install(ScdMachine *m, const char *text, size_t length, int log, const char **error, unsigned *line);
uint64_t scd_input_next_event(const ScdMachine *m);
/* Apply every event due at or before `until`, in script order. */
void scd_input_advance(ScdMachine *m, uint64_t until);
void scd_input_note_read(ScdMachine *m, uint32_t port, uint32_t value);
void scd_input_report(const ScdMachine *m, FILE *out);
int scd_input_write_log(const ScdMachine *m, const char *path);
void scd_input_release(ScdMachine *m);
/* --- SEG-078-T001 (ADR 0204): live pad input and its recording --------------------------------------------------------------- */
typedef enum ScdLiveSample {
  SCD_LIVE_SAMPLE_OK = 0,
  SCD_LIVE_SAMPLE_CAPACITY,   /* the transition would exceed the script grammar's event or byte limit: nothing was appended */
  SCD_LIVE_SAMPLE_PASSED,     /* the tick is not strictly after every device event and instruction already executed */
  SCD_LIVE_SAMPLE_NO_INPUT    /* no live input is installed */
} ScdLiveSample;
/* Install an empty (header-only) script that accepts live samples; `log` enables the data-port read log. `record` keeps the events (and applies the
 * grammar's limits); without it an applied event is discarded at the next sample, so an unrecorded session is unbounded. 0 on success. */
int scd_input_live_begin(ScdMachine *m, int log, int record);
/* Record that the host's port-1 pad is `pad1` from machine tick `tick` on: the differing buttons become transitions appended to the
 * script (releases then presses, canonical button order) and are applied by the ordinary script machinery when the machine reaches
 * `tick`. The tick must be greater than the time of every instruction and device event already executed (m->now) and than the last
 * recorded tick (so one tick never touches a button twice). All or nothing: at the grammar's limits (4,096 events, 65,536 bytes) nothing is appended. */
ScdLiveSample scd_input_live_sample(ScdMachine *m, uint64_t tick, uint8_t pad1);
/* The recorded script as grammar text; returns its length, or 0 when `capacity` is too small or no live input is installed. The text
 * is re-parsed by the script validator before it is returned (a recorder defect is an error, never an invalid file). */
size_t scd_input_serialize(const ScdMachine *m, char *out, size_t capacity);
int scd_input_live_write(const ScdMachine *m, const char *path);
/* The device host's pad mask for a port (scd_vdp.c owns the Genesis runtime). */
void scd_vdp_set_pad(ScdMachine *m, uint32_t port, uint8_t mask);

/* Device internals shared between runtime translation units. */
void scd_log(ScdMachine *m, uint32_t kind, uint32_t address, uint32_t value);
int scd_ga_main_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_ga_sub_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
void scd_update_sub_interrupt(ScdMachine *m);
void scd_raise_sub_level(ScdMachine *m, uint32_t level);
void scd_devices_advance(ScdMachine *m, uint64_t until);
uint64_t scd_devices_next_event(const ScdMachine *m);
/* 1 when a device can still raise sub interrupt `level` without processor action (the STOP wake rule). */
int scd_devices_can_raise(const ScdMachine *m, uint32_t level);
int scd_vdp_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_vdp_init(ScdMachine *m);
/* SEG-064-T003 (BIOS-image profile): the main processor's Genesis devices besides the VDP ports, the vertical-blank event, the main
 * interrupt request level and its acknowledge, and the open-bus value. */
int scd_main_device_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
/* SEG-065-T003 (ADR 0147): capture the frame whose active display ends at vertical-blank onset number `onset` (>= 2, BIOS-image
 * profile, at most 8) into `dir`: the per-line VDP state is sampled at each active line's start and rendered by the existing renderer. */
/* ADR 0177: master clocks (7 per main cycle) and ticks since the origin of the frame containing tick `t` (the last onset). */
uint64_t scd_frame_ticks_since_origin(const ScdMachine *m, uint64_t t);
int scd_vdp_capture_request(ScdMachine *m, uint64_t onset, const char *dir);
/* SEG-078-T001 (ADR 0204): continuous completed-frame presentation. Every frame (not only chosen onsets) has its per-line state sampled
 * by the capture path's observer instants and is rendered once at its vertical-blank onset; the machine's behaviour and every report
 * field other than the presentation counters are unchanged. The first frame (the power-on run-in) never has its first lines and is
 * counted skipped. Only the BIOS-image profile; 0 on success. */
int scd_vdp_present_enable(ScdMachine *m);
/* The frame completed at the latest onset (valid until the machine next runs), or NULL when none is new since the last call. */
const struct GenesisFrameArtifact *scd_vdp_present_take(ScdMachine *m, uint64_t *onset);
void scd_vdp_present_counts(const ScdMachine *m, uint64_t *rendered, uint64_t *skipped, uint64_t *rejected);
/* The canonical frame digest (ADR 0146 decision 5), the same SHA-256 as a capture report's `sha256`. */
void scd_frame_sha256(const struct GenesisFrameArtifact *frame, char out[65]);
/* Ticks of one video frame of this machine (the distance between successive vertical-blank onsets). */
uint64_t scd_frame_ticks(void);
void scd_vdp_capture_report(const ScdMachine *m, FILE *out);
int scd_vdp_capture_line_due(const ScdMachine *m, uint64_t until);
uint64_t scd_vdp_next_event(const ScdMachine *m);
void scd_vdp_event(ScdMachine *m);
uint32_t scd_main_request_level(const ScdMachine *m);
uint32_t scd_main_wake_level(const ScdMachine *m);
void scd_main_interrupt_acknowledge(ScdMachine *m, uint32_t level);
int scd_open_bus(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t *value, uint32_t pc);
void scd_vdp_release(ScdMachine *m);
/* SEG-064-T004: a word read of the CDC host-data register by the main (cpu_access 2) or sub (3) processor; *word is the value read. */
void scd_cdc_host_read(ScdMachine *m, uint32_t cpu_access, uint32_t *word);
/* Bring an in-flight CDC DMA up to the current instruction tick (every Word RAM / CDC observation point). */
int scd_cd_observe(ScdMachine *m);
void scd_ledger_note(ScdMachine *m, uint32_t lba);
int scd_cdc_sub_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
/* SEG-064-T004 diagnostics: executed-instruction-start coverage (scd_coverage.c). Enabled only by the program driver's --coverage flag. */
int scd_event_open(ScdMachine *m, const char *path);
void scd_event(ScdMachine *m, const char *kind, uint32_t detail);
void scd_event_close(ScdMachine *m);
int scd_trace_open(ScdMachine *m, const char *path);
/* SEG-079 diagnostic (--ram-write-log, local only, outside the program identity): 16-byte little-endian records
 * {u8 kind, u8 width, u8 cpu, u8 0, u32 offset, u32 value, u32 pc}; kind 1 = PRG RAM write, 2 = work RAM write, 3 = main BIOS-window read. */
int scd_wlog_open(ScdMachine *m, const char *path);
void scd_wlog(ScdMachine *m, uint32_t kind, uint32_t cpu, uint32_t offset, uint32_t width, uint32_t value, uint32_t pc);
void scd_wlog_close(ScdMachine *m);
void scd_trace_ga(ScdMachine *m, uint32_t cpu, uint32_t offset, uint32_t width, int is_write, uint32_t value);
void scd_trace_close(ScdMachine *m);
int scd_coverage_enable(ScdMachine *m);
void scd_coverage_note(ScdMachine *m, uint32_t cpu, uint32_t pc);
int scd_coverage_write(const ScdMachine *m, const char *path);
void scd_coverage_release(ScdMachine *m);
/* SEG-064-T004: the graphics ASIC (scd_gfx.c). */
int scd_gfx_register_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
void scd_gfx_sync(ScdMachine *m, uint64_t now);
uint64_t scd_gfx_next_event(const ScdMachine *m);
int scd_gfx_can_raise(const ScdMachine *m);
void scd_gfx_reset(ScdMachine *m);
/* SEG-064-T004: the sub's PCM area ($FF0000-$FF7FFF; offset within it). */
void scd_pcm_reset(ScdMachine *m);
/* Apply every sample event strictly before `tick` (ADR 0233). Exposed for the focused device test; the machine calls it from each access. */
void scd_pcm_sync(ScdMachine *m, uint64_t tick);
int scd_pcm_trace_open(ScdMachine *m, const char *path);
void scd_pcm_trace_close(ScdMachine *m);
int scd_pcm_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_word_ram_access(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
/* Word RAM ownership changes driven by the Gate Array (scd_word_ram.c). */
int scd_word_ram_main_mode_write(ScdMachine *m, uint32_t value, uint32_t pc);
/* SEG-103-T002: in 1M mode, the 2M-layout byte offset of the main processor's bank at 128 KiB bank offset `offset` (the view of scd_word_ram_access). */
uint32_t scd_word_ram_main_bank_physical(const ScdMachine *m, uint32_t offset);
int scd_word_ram_sub_mode_write(ScdMachine *m, uint32_t value, uint32_t pc);
/* CDC DMA control shared with the Word RAM logic (scd_cd.c). */
void scd_cdc_dma_halt_2m(ScdMachine *m);
void scd_cdc_dma_resume_2m(ScdMachine *m);
/* Peripheral reset (sub writes RES0 = 0) and CDD activity bookkeeping. */
void scd_peripheral_reset(ScdMachine *m);
int scd_cdd_active(const ScdMachine *m);
void scd_cdd_refresh(ScdMachine *m, int was_active);
/* ADR 0177: (re)anchor the 75 Hz drive grid at tick `at` and the first drive-tick instant strictly after `t` (t >= the anchor). */
void scd_cdd_anchor(ScdMachine *m, uint64_t at);
uint64_t scd_cdd_grid_after(const ScdMachine *m, uint64_t t);
void scd_devices_power_on(ScdMachine *m);
/* SEG-104 (scd_cdda.c). Reset restores the fader and ends playback; the host's sink and `enabled` survive. */
void scd_cdda_reset(ScdMachine *m);
void scd_cdda_fader_write(ScdMachine *m, uint32_t offset, uint32_t width, uint32_t value);
/* Called by the drive tick: produce `lba` (an audio sector). Returns 1 on a typed stop (the sector could not be read). */
int scd_cdda_play(ScdMachine *m, int32_t lba, uint32_t track_index);
void scd_cdda_silent_tick(ScdMachine *m);
void scd_cdda_interrupt(ScdMachine *m);

/* Program driver: parses argv, opens the disc, runs, writes the report and the frame. */
int scd_program_main(const ScdProgram *program, int argc, char **argv);
int scd_program_main_overlays(const ScdProgram *program, const ScdOverlayProgram *overlays, int argc, char **argv);
/* SEG-064-T002 (ADR 0142): the BIOS-image profile's driver: additionally --bios <image> (required) and --materialize-dir <dir>. */
int scd_program_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
