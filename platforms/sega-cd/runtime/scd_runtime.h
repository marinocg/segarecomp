#ifndef SEGARECOMP_RUNTIME_SEGA_CD_SCD_RUNTIME_H
#define SEGARECOMP_RUNTIME_SEGA_CD_SCD_RUNTIME_H

/* SEG-062 (ADR 0128): the Sega CD machine runtime ABI. Strict C11; links the Genesis runtime only for the VDP device, frame
 * export and the C SHA-256; never links C++.
 *
 * One ScdMachine holds two MC68000 processor instances (index 0 = main, 1 = sub) of the same generated semantics. Generated code
 * for processor k names only m->cpu[k] and passes the constant k to the bus; every function returns 0 to continue or 1 after
 * recording a typed stop in m->stop. Nothing here decodes guest instructions: computed control selects among the generated
 * program's compiled entries (ScdProgram tables), and RAM-resident code is checked by byte comparison only. */

#include <stddef.h>
#include <stdint.h>

#include "../../../libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h"
#include "../../../libs/cpu/m68k/include/segarecomp/cpu/m68k/timing_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCD_MAIN 0U
#define SCD_SUB 1U

/* Integer clock domain (contract section 2): ticks per second = lcm(53693175, 12500000, 75). */
#define SCD_TICKS_PER_SECOND UINT64_C(26846587500000)
#define SCD_MAIN_CYCLE_TICKS UINT64_C(3500000)
#define SCD_SUB_CYCLE_TICKS UINT64_C(2147727)
#define SCD_CDD_TICK_TICKS UINT64_C(357954500000)
#define SCD_TIMER_UNIT_SUB_CYCLES UINT64_C(384)
#define SCD_DMA_SUB_CYCLES_PER_BYTE UINT64_C(4)

#define SCD_PRG_RAM_BYTES 0x80000U
#define SCD_WORD_RAM_BYTES 0x40000U
#define SCD_WORK_RAM_BYTES 0x10000U
#define SCD_CDC_BUFFER_BYTES 0x4000U

typedef enum ScdStopClass {
  SCD_STOP_NONE = 0,
  SCD_STOP_UNMAPPED,
  SCD_STOP_BIOS_ABSENT,
  SCD_STOP_PRG_RAM_WINDOW_WITHOUT_BUS,
  SCD_STOP_WORDRAM_NOT_OWNED,
  SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED,
  SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED,
  SCD_STOP_WORDRAM_1M_DMNA_UNSUPPORTED,
  SCD_STOP_WORDRAM_PRIORITY_MODE_UNSUPPORTED,
  SCD_STOP_BACKUP_RAM_UNSUPPORTED,
  SCD_STOP_PCM_UNSUPPORTED,
  SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
  SCD_STOP_PERIPHERAL_RESET_UNSUPPORTED,
  SCD_STOP_BUSREQ_WHILE_SUB_STOPPED_UNSUPPORTED,
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
  SCD_STOP_CDC_DMA_DESTINATION_NOT_OWNED,
  SCD_STOP_CDC_REGISTER_UNSUPPORTED,
  SCD_STOP_CDD_BAD_CHECKSUM,
  SCD_STOP_CDD_COMMAND_UNSUPPORTED,
  SCD_STOP_CD_READ_OUT_OF_RANGE,
  SCD_STOP_CD_READ_AUDIO_TRACK,
  SCD_STOP_CD_READ_FAILED,
  SCD_STOP_INTERNAL,
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
  SCD_DISC_IO
} ScdDiscError;

const char *scd_disc_error_name(ScdDiscError error);
ScdDiscError scd_disc_open(ScdDisc *disc, const char *directory);
void scd_disc_close(ScdDisc *disc);
/* One Mode-1 user block by LBA (2048 bytes); raw sectors are checked for sync and header. */
ScdDiscError scd_disc_read_user_data(ScdDisc *disc, uint32_t lba, uint8_t out[2048]);
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

typedef struct ScdProgram {
  ScdProcessorProgram processors[2];
  ScdDisc disc;
  uint32_t main_initial_ssp;
  const char *program_identity;   /* build identity: disc identity + generator version */
} ScdProgram;

/* --- Gate Array / devices ------------------------------------------------------------------------------------------------ */

typedef struct ScdGateArray {
  uint8_t sub_reset;        /* SRES = 0: sub held in reset */
  uint8_t sub_busreq;       /* SBRQ readback */
  uint8_t ifl2;             /* main -> sub level-2 request flag ($A12000 bit 0) */
  uint8_t led;              /* sub $FF8000 */
  uint8_t write_protect;    /* WP, units of $200 bytes */
  uint8_t prg_bank;         /* BK1:BK0 */
  uint8_t mode_1m;          /* MODE */
  uint8_t ret;              /* RET */
  uint8_t dmna;             /* DMNA */
  uint8_t main_flag;
  uint8_t sub_flag;
  uint16_t comcmd[8];
  uint16_t comstat[8];
  uint8_t interrupt_enable; /* $FF8033 bits 1-6 */
  uint8_t pending;          /* pending sub interrupt levels, bit n = level n */
  uint8_t timer_reload;     /* $FF8031 */
  uint64_t timer_next;      /* tick of the next INT3, 0 = stopped */
  uint8_t cdd_hock;         /* $FF8037 bit 2 */
} ScdGateArray;

typedef struct ScdCdc {
  uint8_t address;          /* $FF8005 */
  uint8_t destination;      /* $FF8004 bits 0-2 */
  uint8_t edt, dsr;         /* $FF8004 bits 7, 6 */
  uint16_t dma_address;     /* $FF800A */
  uint8_t ifstat, ifctrl, ctrl0, ctrl1;
  uint16_t dbc, dac, pt, wa;
  uint8_t head[4];
  uint8_t stat0, stat2, stat3;
  uint8_t irq_line;         /* active CDC /INT sources (DTEI | DECI as in IFCTRL) */
  /* DMA in flight */
  uint8_t dma_active;
  uint32_t dma_total;       /* bytes */
  uint32_t dma_done;
  uint64_t dma_start;       /* tick */
  uint8_t buffer[SCD_CDC_BUFFER_BYTES + 2352];
} ScdCdc;

typedef struct ScdCdd {
  uint8_t tx[10];
  uint8_t rx[10];
  uint8_t status;           /* drive status code */
  uint8_t reading;          /* play in progress */
  uint32_t lba;             /* next sector to deliver */
  uint32_t latency;         /* ticks before the first delivery */
  uint64_t next_tick;       /* tick of the next 75 Hz event */
  uint64_t delivered;       /* sectors delivered */
} ScdCdd;

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
  uint64_t sub_interrupts_taken[8];
} ScdMachine;

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
int scd_compiled(const ScdMachine *m, uint32_t cpu, uint32_t address);

/* --- machine ---------------------------------------------------------------------------------------------------------------- */

/* Establish the synthetic post-BIOS profile (contract section 9): IP and SP loaded from the disc's system area. */
int scd_machine_init(ScdMachine *m, const ScdProgram *program, ScdDisc *disc, uint64_t instruction_budget);
ScdRunResult scd_machine_run(ScdMachine *m, uint64_t deadline);
uint64_t scd_machine_time(const ScdMachine *m);
void scd_machine_release(ScdMachine *m);

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
void scd_vdp_release(ScdMachine *m);
int scd_cdc_sub_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_word_ram_access(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_word_ram_dma_store(ScdMachine *m, uint32_t offset, uint16_t word);

/* Program driver: parses argv, opens the disc, runs, writes the report and the frame. */
int scd_program_main(const ScdProgram *program, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
