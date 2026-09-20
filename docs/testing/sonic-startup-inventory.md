# Sonic Startup Capability Inventory Scanner (SEG-007-T007)

`tools/sonic_startup_inventory.py` is a project-owned, opt-in, local-only development tool. It
replaces one-opcode-at-a-time capability discovery (the former SEG-006-T001 -> SEG-007-T004 ->
T005/T006 cycle) with a single deterministic, explicitly bounded local scan of the pinned Sonic
ROM under the pinned reference Musashi interpreter, followed by normalization through
segarecomp's own real shared CPU decode pipeline and memory/address-space mapping policy. See
`docs/testing/commercial-games.md` for this repository's general commercial-input policy and the
project's private planning workflow for how a completed scan's
sanitized findings feed later backlog refinement.

This tool is local-only tooling. It is never a production execution path, never linked into
`segarecomp`, and no `ctest --preset dev` gate depends on it.

## Two Separate Classification Authorities

- **CPU instruction support** (`supported` / `partially_supported` / `unsupported`) comes only
  from segarecomp's real shared decode -> lift -> IR-effect -> lowering pipeline
  (`decode_m68k_instruction(..., M68kDecodeProfile::genesis_startup)`, then
  `lift_m68k_instruction`, `m68k_operation_effect`, and `emit_m68k_operation_c`), reached through
  the `probe-genesis-startup-decode` CLI subcommand, which is the sole authority for the
  `"support"` field it returns. This probe supplies `emit_m68k_operation_c` with a real
  (synthetic, project-owned) `M68kMemoryEmissionContext` rather than a null one: with a null
  context, JSR (`call_absolute_long`) and RTS (`return_from_subroutine`) always emit empty C --
  their entire lowering body lives inside `if (memory != nullptr)` with no unconditional fallback
  -- which wrongly demoted both to `partially_supported` even though their
  `m68k_operation_effect` and `emit_m68k_operation_c` cases are both fully implemented once given
  a context; conversely, the two absolute-long MOVE forms
  (`write_d0_absolute_long`/`read_absolute_long_d1`) emit their status-register-update line
  unconditionally even with a null context, so a null-context probe could infer `supported` from
  that partial, context-free text alone, without ever exercising the real RAM read/write portion
  gated on `memory != nullptr`. With a real context, a decoded form that selects a real
  instruction kind but whose shared `m68k_operation_effect` reports an undefined PC transition
  (`M68kPcEffectKind::none`), or whose shared `emit_m68k_operation_c` lowering (now genuinely
  exercising every gated code path) produces empty C, is `partially_supported`; a form decode
  itself does not select is `unsupported`. Under this corrected, context-aware probe, all five
  forms `M68kDecodeProfile::genesis_startup` currently selects (MOVEQ, the two absolute-long MOVE
  forms, JSR, and RTS) classify `supported` -- every one of their `emit_m68k_operation_c` cases
  produces non-empty text and every one of their `m68k_operation_effect` cases already defines a
  non-`none` `pc` kind once given real inputs; see the SEG-007-T007 Evidence entry dated for this
  correction for the full verification. A bounded, inspection-only stage-1 classifier
  (`tools/inventory/stage1_classifier.py`) separately names every observed instruction's public
  `family`/`size`/`addressingMode`/`sourceAddressingMode`/`destinationAddressingMode` fields using
  documented MC68000 encoding rules; it never decides support and is never imported by any
  production or runtime build target (`src/`, `include/`).
- **Memory/device access frontier** (`raw_cartridge_rom` / `synthetic_work_ram` /
  `hardware_frontier`) comes only from segarecomp's real shared production memory/address-space
  mapping policy, reached through the `probe-genesis-startup-mapping` CLI subcommand (the exact
  `raw_cartridge_rom` = `[0, image_length)` and `synthetic_work_ram` =
  `m68k_startup_ram_range_in_range` facts `genesis-rom-startup` already establishes). The probe is
  width-aware: classification always requires the _entire_ `[address, address+width)` access
  range to lie inside a mapping, never just its starting address, so a partially-out-of-range or
  boundary-crossing access correctly falls through to `hardware_frontier` rather than being
  silently accepted. The scanner builds no second, scanner-private Genesis memory map or device
  model, and -- critically -- **the Musashi adapter never makes this decision itself**: it is a
  dumb, bounded, single-instruction-at-a-time stepper with no semantic authority. Only this
  Python driver, calling the real live `probe-genesis-startup-mapping` CLI on every observed
  access of every run, decides `hardware_frontier`. This means a later production mapping-policy
  change changes scanner behavior automatically, with zero adapter changes required.
  `m68k_startup_ram_range_in_range` (the shared predicate backing `synthetic_work_ram`
  membership) is overflow-safe for any `uint32_t` width, not just small values: it rejects any
  width larger than the synthetic work-RAM window's own size before computing the window's
  internal end-address subtraction, so an implausibly large reported access width (e.g.
  `0xFFFFFFFF`) cannot wrap that subtraction and false-accept an address as `synthetic_work_ram`;
  it correctly falls through to `hardware_frontier` instead.

## Local Input Gates

### Gate 1: pinned Sonic ROM

```sh
export SEGARECOMP_SONIC_ROM=/absolute/path/to/local/sonic-image
```

The file must exist and its SHA-256 digest must equal the digest already pinned by
SEG-006-T001/SEG-007-T004:

```text
46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6
```

A missing variable, missing file, or digest mismatch is a graceful "unavailable" outcome
(documented exit code `10`, never a crash, never the configured path echoed back).

### Gate 2: pinned Musashi checkout and adapter

```sh
export SEGARECOMP_MUSASHI_TOOL_DIR=/absolute/path/to/ignored/tool/dir
```

`$SEGARECOMP_MUSASHI_TOOL_DIR` must contain a `musashi` checkout at the pinned revision:

```text
313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
```

and the built adapter executable at
`$SEGARECOMP_MUSASHI_TOOL_DIR/sonic-startup-musashi-scan-oracle`. A missing variable,
missing/mismatched checkout, or missing adapter is also a graceful "unavailable" outcome
(documented exit code `20`).

## Bounded Execution

Every scan is bounded by three literal, documented constants
(`tools/sonic_startup_inventory.py`) plus the real shared mapping policy's `hardware_frontier`
classification. Unlike an earlier version of this scanner, **the adapter decides none of these
stop conditions itself** -- it is a dumb, bounded, single-instruction-at-a-time Musashi stepper
that reports one fully-executed instruction at a time and waits to be told whether to continue.
This Python driver (`run_scan` in `tools/sonic_startup_inventory.py`) is the sole stop-decision
authority, deciding fresh on every run against the real, live production probes. The first
triggered stop condition ends the run, in this exact precedence:

1. An access in the just-reported instruction classifies `hardware_frontier` under the real
   shared, width-aware `probe-genesis-startup-mapping` policy.
2. `MAX_EXECUTED_INSTRUCTIONS = 500` -- total instructions executed.
3. `MAX_UNIQUE_VISITED_PCS = 500` -- unique program counters visited.
4. `MAX_CONTROL_FLOW_DEPTH = 32` -- taken branches/calls (incremented whenever the new PC is not
   `old_pc + consumed_length`, using the adapter-reported real, uncapped instruction byte length).
5. Otherwise, reply `"continue"` and let the adapter execute one more instruction.

These limits are deliberately small and conservative: they keep every local run fast and
deterministic, and they stop at the first meaningful hardware/device boundary rather than
attempting a deep, open-ended scan. A local scan never runs unbounded. The adapter additionally
carries one _technical_ safety ceiling of its own (`HARD_SAFETY_MAX_STEPS`, unrelated to any of
the four bounds above) purely so a broken or hung protocol on the Python side cannot spin the
adapter forever; this is a pure execution safety valve, never a semantic decision, and it is
never expected to trigger during a correctly-driven scan.

The untrusted streaming and probe processes also have literal lifecycle/resource bounds. The
driver waits at most `1` second for each adapter protocol line and retains at most `4096` bytes of
one unfinished/complete protocol line. A blank or whitespace-only adapter line is malformed
protocol, not an ignorable record. Each invocation of either production probe waits at most `1`
second and retains at most `4096` stdout bytes; probe stderr is never retained. A timeout,
oversized probe stdout, nonzero probe exit, or malformed probe JSON is a sanitized adapter failure
(`EXIT_ADAPTER_FAILURE`), leaves any existing cache untouched, and reaps the started process.

## The Streaming Protocol

The adapter and this Python driver communicate over a synchronous, per-instruction, in-memory
streaming protocol on the adapter's stdin/stdout pipes (`subprocess.Popen`, `text=True`,
line-buffered). **No raw trace is ever written to any file, temporary or otherwise, at any
point.**

For each instruction Musashi is about to execute, the adapter first completes the _previous_
instruction (including every memory access Musashi's read/write hooks observed during it), then
writes exactly one JSON line to stdout and flushes:

```text
{"ordinal":N,"pc":"0x........","primary":"0xWWWW","extension":"0xWWWWWWWW"|null,"length":L,
 "accesses":[{"ordinal":N,"kind":"read"|"write","address":"0x........","byteWidth":W},...]}
```

`length` is the real, uncapped instruction byte length Musashi's disassembler reports (used only
by Python to detect taken branches for control-flow-depth counting); `extension` remains capped
to one 32-bit word, `null` when the instruction is shorter than 4 extension bytes, exactly as
before. The adapter then **blocks reading exactly one line from stdin**: `"continue\n"` or
`"stop\n"` (anything else, or EOF, is treated as `"stop"`). Only on `"continue"` does it proceed
to actually execute that instruction and start accumulating the next one's accesses. On `"stop"`
the adapter exits `0` immediately, with no further output.

The very first instruction (ordinal 0) has no predecessor to report; the adapter's first stdout
line naturally describes instruction 0 only after instruction 0 has itself executed and the hook
fires again for instruction 1. This means every instruction Python actually receives has already
run to completion, with its complete, real access list -- Python's `"stop"` reply prevents only
the _next_ instruction from starting.

```text
adapter                                          python driver (run_scan)
--------                                         -------------------------
execute instr N-1 (already approved)
accumulate instr N-1's accesses via hooks
write one JSON line describing instr N-1  ---->  read one line
                                                  classify instr N-1 (stage1_classifier +
                                                  probe-genesis-startup-decode) and each of
                                                  its accesses (probe-genesis-startup-mapping)
                                                  update running instruction/PC/depth counters
                                                  decide continue/stop (5-step precedence above)
block reading one line from stdin         <----  write "continue\n" or "stop\n"; flush
if "continue": execute instr N, loop
if "stop" (or EOF/anything else): exit 0
```

Python's own loop variable holding one parsed JSON line (`entry`) is discarded before the next
line is even read: it is never accumulated into any list, written to any file, printed, or
logged. Only the normalized aggregation dicts persist across loop iterations. This is a
_stronger_ privacy property than an earlier version of this scanner had (which kept the whole raw
trace in memory only briefly): raw content for instruction K is garbage-collected before
instruction K+1's line is even read.

The adapter still needs _some_ backing storage to let Musashi read/write bytes at all (Musashi
cannot run against nothing): it keeps the existing conservative ROM array `[0, rom_len)` and RAM
array `[0x00FF0000, 0x01000000)` (restating, not re-deriving, the exact literal hex constants
`include/segarecomp/m68k_pipeline.hpp`'s `m68k_startup_ram_begin`/`m68k_startup_ram_end` already
define). Every 8-, 16-, and 32-bit backing-store access first uses an overflow-safe, whole-width
range check: an access beginning in RAM but ending outside it is one rejected access, not a
partial RAM access. Rejected reads return an all-ones value of their requested width and rejected
writes are no-ops; both are still recorded at their original address and width. Consequently the
Python driver classifies the recorded whole-width crossing as `hardware_frontier` and sends its
existing protocol `stop` reply before another instruction begins. This is **conservative execution
backing storage only, never a mapping/frontier authority**. The adapter does not decide
`hardware_frontier` and does not track
`max_instructions`/`unique_pcs`/`control_flow_depth` at all; Python owns 100% of that bookkeeping
from the stream it receives.

## Running The Scanner

```sh
python3 tools/sonic_startup_inventory.py --help
python3 tools/sonic_startup_inventory.py --executable build/dev/segarecomp --scan
```

`--help` documents the ROM/Musashi environment variables, the pinned Musashi revision, every
bounded-execution limit and its literal value, the output cache path, and the tool's exit codes.

On a completed bounded scan (any of the four stop conditions above is success, not failure), the
tool:

- writes the full normalized inventory only to the gitignored local cache path
  `.cache/sonic-startup-inventory.json` (never a path that could be committed);
- prints only a bounded, already-sanitized human summary to stdout (counts per `support` value,
  counts per `category` value, the stop reason, and whether a `semantic_classifier_gap`
  instruction was observed -- no raw ROM-derived content).

Two fresh executions against the same local ROM/adapter produce byte-identical
`.cache/sonic-startup-inventory.json` content and sanitized stdout summaries: ordinals and
aggregation are deterministic, and no wall-clock, PID, hostname, or random content is ever written.

### Exit codes

- `0` -- successful bounded scan (including `--help`), with no `semantic_classifier_gap`
  instruction entry observed.
- `2` -- usage error (bad arguments).
- `10` -- ROM unavailable (unset/missing/hash-mismatched `SEGARECOMP_SONIC_ROM`).
- `20` -- Musashi pin unavailable (unset/missing/mismatched `SEGARECOMP_MUSASHI_TOOL_DIR`).
- `30` -- the scan completed (the cache file is written and the summary is printed exactly as on
  a `0` exit) but at least one observed capability normalized to the explicit
  `"family": "semantic_classifier_gap"` marker (see "Explicit Semantic-Classifier-Gap Marker"
  below): the tool fails closed specifically for backlog-generation purposes even though it
  succeeded as a diagnostic scan.
- `40` -- adapter/protocol failure (`EXIT_ADAPTER_FAILURE`): the scan did not complete through a
  documented stop condition (see "Fail-Closed Adapter Lifecycle Contract" below). **No cache file
  is written by a run that exits `40`**; any previous run's cache file is left completely
  untouched (never overwritten, never deleted).

## Fail-Closed Adapter Lifecycle Contract

A successful scan is _only_ one where `run_scan` itself explicitly decided one of the four
documented stop conditions (`hardware_frontier` / `max_executed_instructions` /
`max_unique_visited_pcs` / `max_control_flow_depth`), sent `"stop\n"`, and the adapter process
then exited with code `0`. `run_scan`'s contract is: it either returns a fully successful
`(normalized_inventory, adapter_stderr)` tuple, or it raises `AdapterFailure` -- there is no third,
partial-success outcome, and no code path writes a partial or malformed cache file. Every one of
the following is uniformly a failure, raising `AdapterFailure` with a short, already-sanitized
`reason` tag (never the adapter's raw stderr, which is retained on the exception only for
test-only inspection and must never be printed/logged by any caller):

- **`unexpected_eof_before_stop`** -- the adapter's stdout hits EOF before `run_scan` ever decided
  a documented stop condition and sent `"stop\n"`. This covers an early/unexpected adapter exit, a
  crash, and the adapter's own internal `HARD_SAFETY_MAX_STEPS` safety ceiling firing on its own;
  `run_scan` cannot distinguish these from each other and does not need to -- they are all "the
  protocol ended before a documented stop condition was decided", uniformly a failure.
- **`nonzero_exit`** -- the adapter process's exit code is nonzero after `run_scan` closes the
  pipes and waits for it, checked even in the otherwise-successful path: a clean `"stop\n"` reply
  followed by a nonzero exit is still a failure.
- **`malformed_protocol`** -- a line from the adapter cannot be parsed as JSON, or is missing an
  expected key (`ordinal`, `pc`, `primary`, `length`, `accesses`, and each access's
  `ordinal`/`kind`/`address`/`byteWidth`).
- **`broken_pipe`** -- a broken pipe while writing `"continue\n"`/`"stop\n"` to the adapter's
  stdin.
- **`probe_timeout`**, **`probe_output_limit`**, or **`probe_failure`** -- a production probe did
  not complete within its literal bound, exceeded its retained stdout limit, or returned an
  unusable response. Probe stdout/stderr is never included in the diagnostic.

`main()` catches `AdapterFailure`, prints only a sanitized one-line message using `error.reason`
(e.g. `sonic startup inventory: adapter failure (nonzero_exit)`; never `error.adapter_stderr`),
and returns `EXIT_ADAPTER_FAILURE` **without calling `write_cache`**. `tests/sonic_startup_inventory_adapter_failure_test.py`
covers clean success, early/unexpected EOF, a nonzero exit after a clean stop, a malformed
protocol line, and that none of the failure scenarios ever overwrites a pre-existing cache file,
using small, fully synthetic, project-authored fake adapter scripts (no real Musashi/ROM
required).

## Explicit Semantic-Classifier-Gap Marker

An earlier version of `tools/inventory/stage1_classifier.py` fell back to a coarse
`family = "line_<N>"` bucket (named only from the primary word's top 4-bit "line" nibble) for
every MC68000 form it did not precisely recognize. That bucket falsely implied every unrecognized
form sharing a top nibble was one interchangeable capability, which is not actionable. The
classifier now returns an explicit, fail-closed gap marker instead:
`{"family": "semantic_classifier_gap", "size": None, "addressingMode": None,
"sourceAddressingMode": None, "destinationAddressingMode": None}`. A `semantic_classifier_gap`
entry in a completed scan's normalized inventory is not backlog-actionable on its own: it signals
that the classifier itself needs precise extension for that observed form before a compatibility
batch can safely be authored from this run's output. `tools/sonic_startup_inventory.py`'s `main()`
reflects this by returning exit code `30` (`EXIT_SEMANTIC_GAP`) instead of `0` whenever any
normalized instruction entry has `family == "semantic_classifier_gap"`, while still writing the
full cache file and printing the sanitized summary (the scan itself is not a failure -- consuming
its output for backlog refinement is what must not proceed unexamined).

## Privacy Rules

The scanner never commits, prints, or logs, anywhere (stdout, stderr, the cache file, or any
committed artifact):

- ROM bytes;
- concrete opcode or extension words observed during the scan;
- disassembly text;
- the local ROM filename or path;
- the raw, unnormalized per-instruction JSON line the adapter streams (`{"ordinal":...,
  "pc":..., "primary":..., "extension":..., "length":..., "accesses":[...]}`) -- this is never
  written to any file (temporary or otherwise), printed, or logged; the loop variable holding one
  parsed line is discarded before the next line is even read.

Every value that reaches stdout or the cache file has already passed through normalization:
`tools/inventory/stage1_classifier.classify` for the instruction
`family`/`size`/`addressingMode`/`sourceAddressingMode`/`destinationAddressingMode` fields, and
the `probe-genesis-startup-decode`/`probe-genesis-startup-mapping` CLI probes for
`support`/`category`. Normalized entries carry only sanitized fields plus non-commercial
provenance (`firstObservedOrdinal`, `observationCount`); they are aggregated by the normalized
tuple (`family`/`size`/`addressingMode`/`sourceAddressingMode`/`destinationAddressingMode`/
`support` for instructions, `category`/`kind` for accesses), never by raw opcode bytes or raw
address.

The scanner’s sanitization guarantee applies specifically to the inventory scanner and remains unchanged.

The separate production CLI may provide more detailed local diagnostics. Under `docs/testing/commercial-games.md` and ADR 0005, an authorized agent may consume that output ephemerally for a bounded diagnosis when its execution environment permits it. Before the result is used by another workflow phase or placed in durable evidence, it must be converted into the normalized frontier handoff defined by `sonic-startup-capability-inventory`.

The normalized handoff may identify semantic classes and pipeline progress, but it must not reproduce raw addresses, offsets, instruction words, byte sequences, access addresses, disassembly, local paths, or complete reports.

## Adapter Bootstrap (Local-Only, Never Committed)

The adapter is Musashi-derived tooling and is therefore never committed as a tracked repository
file, mirroring the `musashi-oracle-validation` skill's exact convention (same Docker image
`gcc:13`, same `-v $DIR:/oracle -w /oracle/musashi` mount). A developer/agent who wants to run a
real bounded scan reconstructs the adapter locally from the source embedded below.

1. Bootstrap the pinned Musashi checkout (skip if `$SEGARECOMP_MUSASHI_TOOL_DIR/musashi` is
   already present at the pinned revision):

```sh
export SEGARECOMP_MUSASHI_TOOL_DIR=/absolute/path/to/ignored/tool/dir
git clone https://github.com/kstenerud/Musashi.git "$SEGARECOMP_MUSASHI_TOOL_DIR/musashi"
git -C "$SEGARECOMP_MUSASHI_TOOL_DIR/musashi" checkout --detach \
  313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
docker run --rm \
  -v "$SEGARECOMP_MUSASHI_TOOL_DIR:/oracle" \
  -w /oracle/musashi \
  gcc:13 sh -ceu 'gcc -std=c11 -O2 -o m68kmake m68kmake.c; ./m68kmake'
test -f "$SEGARECOMP_MUSASHI_TOOL_DIR/musashi/m68kops.c"
test -f "$SEGARECOMP_MUSASHI_TOOL_DIR/musashi/m68kops.h"
```

2. Write out the adapter source below to
   `$SEGARECOMP_MUSASHI_TOOL_DIR/sonic-startup-musashi-scan-oracle.c`, then build it against the
   generated Musashi sources:

```sh
docker run --rm \
  -v "$SEGARECOMP_MUSASHI_TOOL_DIR:/oracle" \
  -w /oracle \
  gcc:13 sh -ceu '
    gcc -std=c11 -O2 -DM68K_INSTRUCTION_HOOK=1 -I musashi -o sonic-startup-musashi-scan-oracle \
      sonic-startup-musashi-scan-oracle.c musashi/m68kcpu.c musashi/m68kops.c \
      musashi/m68kdasm.c musashi/softfloat/softfloat.c -lm
  '
test -x "$SEGARECOMP_MUSASHI_TOOL_DIR/sonic-startup-musashi-scan-oracle"
```

3. Verify the pin, then run the scanner:

```sh
test "$(git -C "$SEGARECOMP_MUSASHI_TOOL_DIR/musashi" rev-parse HEAD)" = \
  313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
export SEGARECOMP_SONIC_ROM=/absolute/path/to/local/sonic-image
python3 tools/sonic_startup_inventory.py --executable build/dev/segarecomp --scan
```

### Adapter Source

Two build requirements are load-bearing and are already reflected in the bootstrap command above:
the pinned upstream `musashi/m68kconf.h` ships with `M68K_INSTRUCTION_HOOK` `OFF` by default (the
instruction hook this adapter registers via `m68k_set_instr_hook_callback` would silently never
fire without overriding it), so the build passes `-DM68K_INSTRUCTION_HOOK=1` to override that
`#ifndef`-guarded default before `m68kcpu.h` is included; and `m68kcpu.c`'s FPU opcode handlers
reference libm's `sin`/`cos`/`sincos`, so the build links `-lm`.

The adapter is small and deliberately dumb: it configures `M68K_CPU_TYPE_68000`, maps the ROM
read-only at `[0, image_length)` and a synthetic RAM buffer at `[0x00FF0000, 0x01000000)`
(restating, not re-deriving, the exact literal hex constants that
`include/segarecomp/m68k_pipeline.hpp`'s `m68k_startup_ram_begin`/`m68k_startup_ram_end` already
define) purely as conservative execution backing storage so Musashi has _something_ to read/write
against, rejects (no-ops for writes, returns `0xFF` for reads) any other address rather than
crashing while still recording the attempted access, reads the reset SSP/PC from the ROM's first
two big-endian longwords exactly as `analyze_genesis_reset_image` already establishes, and
single-steps via Musashi's instruction hook, implementing exactly the streaming protocol described
above. It carries no `hardware_frontier`/mapping decision of any kind -- that decision belongs
entirely to the Python driver's live calls to the real `probe-genesis-startup-mapping` CLI -- and
tracks no bounded-execution counters of its own beyond the one unrelated technical safety ceiling
(`HARD_SAFETY_MAX_STEPS`).

```c
/* SEG-007-T007 local-only Musashi execution-tracing adapter. Never committed as a tracked
 * repository file; its full source lives in docs/testing/sonic-startup-inventory.md and is
 * written out locally by that doc's bootstrap instructions. Oracle/tooling only: it never
 * becomes part of the CMake build and is never linked into segarecomp.
 *
 * Args: <rom-path>
 *
 * Protocol: for each instruction about to execute, first report the *previous* instruction
 * (one JSON line on stdout, including every access its execution triggered), then block reading
 * exactly one line from stdin ("continue" or "stop"/anything else/EOF). Only "continue" allows
 * the next instruction to execute. This adapter has zero semantic authority over
 * hardware_frontier/mapping or any bounded-execution stop condition: it is a dumb, bounded,
 * single-instruction-at-a-time stepper. HARD_SAFETY_MAX_STEPS below is a pure execution safety
 * valve for a broken/hung protocol on the Python side, never a semantic decision.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

/* Mirrors include/segarecomp/m68k_pipeline.hpp's m68k_startup_ram_begin/m68k_startup_ram_end
 * exactly; restated here rather than derived differently. Conservative execution backing
 * storage only -- never a mapping/frontier authority. */
#define RAM_BEGIN 0x00FF0000U
#define RAM_END   0x01000000U

/* A pure technical safety valve, completely unrelated to the tunable business bounds Python
 * owns (MAX_EXECUTED_INSTRUCTIONS/MAX_UNIQUE_VISITED_PCS/MAX_CONTROL_FLOW_DEPTH): it exists only
 * so a broken/hung protocol on the Python side cannot spin this adapter forever. */
#define HARD_SAFETY_MAX_STEPS 20000U

static uint8_t *g_rom;
static uint32_t g_rom_len;
static uint8_t g_ram[RAM_END - RAM_BEGIN];

static uint32_t g_ordinal;
static uint32_t g_steps;

typedef struct {
  uint32_t ordinal;
  uint32_t address;
  int is_write;
  uint32_t byte_width;
} pending_access;

static pending_access g_pending_accesses[4096];
static uint32_t g_pending_access_count;

static uint32_t g_prev_ordinal;
static uint32_t g_prev_pc;
static uint16_t g_prev_primary;
static int g_prev_has_extension;
static uint32_t g_prev_extension;
static uint32_t g_prev_length;
static int g_have_prev;
static int g_stopped;

/* Check [address, address + width) without forming an overflow-prone end address. The width
 * precondition also makes the subtraction safe for every uint32_t input. */
static int range_in(uint32_t address, uint32_t width, uint32_t begin, uint32_t end) {
  uint32_t size = end - begin;
  return width <= size && address >= begin && address - begin <= size - width;
}

static int in_rom(uint32_t address, uint32_t width) {
  return range_in(address, width, 0U, g_rom_len);
}
static int in_ram(uint32_t address, uint32_t width) {
  return range_in(address, width, RAM_BEGIN, RAM_END);
}

static uint8_t raw_read_8(uint32_t address) {
  if (in_rom(address, 1U)) return g_rom[address];
  if (in_ram(address, 1U)) return g_ram[address - RAM_BEGIN];
  return 0xFFU;
}
static uint32_t raw_read_16(uint32_t address) {
  uint32_t offset;
  if (in_rom(address, 2U)) {
    return ((uint32_t)g_rom[address] << 8U) | g_rom[address + 1U];
  }
  if (in_ram(address, 2U)) {
    offset = address - RAM_BEGIN;
    return ((uint32_t)g_ram[offset] << 8U) | g_ram[offset + 1U];
  }
  return 0xFFFFU;
}
static uint32_t raw_read_32(uint32_t address) {
  uint32_t offset;
  if (in_rom(address, 4U)) {
    return ((uint32_t)g_rom[address] << 24U) | ((uint32_t)g_rom[address + 1U] << 16U) |
           ((uint32_t)g_rom[address + 2U] << 8U) | g_rom[address + 3U];
  }
  if (in_ram(address, 4U)) {
    offset = address - RAM_BEGIN;
    return ((uint32_t)g_ram[offset] << 24U) | ((uint32_t)g_ram[offset + 1U] << 16U) |
           ((uint32_t)g_ram[offset + 2U] << 8U) | g_ram[offset + 3U];
  }
  return 0xFFFFFFFFU;
}

static void record_access(uint32_t address, int is_write, uint32_t width) {
  if (g_pending_access_count < (uint32_t)(sizeof(g_pending_accesses) / sizeof(g_pending_accesses[0]))) {
    pending_access *entry = &g_pending_accesses[g_pending_access_count++];
    entry->ordinal = g_ordinal++;
    entry->address = address;
    entry->is_write = is_write;
    entry->byte_width = width;
  }
}

/* Musashi memory hooks. Every other address is rejected (no-op for writes, 0xFF for reads)
 * rather than crashing; the attempted access is still recorded. No hook here ever decides
 * hardware_frontier or any other stop condition. */
unsigned int m68k_read_memory_8(unsigned int address) {
  address &= 0xFFFFFFU;
  record_access(address, 0, 1U);
  return raw_read_8(address);
}
unsigned int m68k_read_memory_16(unsigned int address) {
  address &= 0xFFFFFFU;
  record_access(address, 0, 2U);
  return raw_read_16(address);
}
unsigned int m68k_read_memory_32(unsigned int address) {
  address &= 0xFFFFFFU;
  record_access(address, 0, 4U);
  return raw_read_32(address);
}
/* Disassembler-only reads (used solely by m68k_disassemble below to size the current
 * instruction); these must not double-count as observed accesses. */
unsigned int m68k_read_disassembler_8(unsigned int address) { return raw_read_8(address & 0xFFFFFFU); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return raw_read_16(address & 0xFFFFFFU); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return raw_read_32(address & 0xFFFFFFU); }
void m68k_write_memory_8(unsigned int address, unsigned int value) {
  address &= 0xFFFFFFU;
  record_access(address, 1, 1U);
  if (in_ram(address, 1U)) g_ram[address - RAM_BEGIN] = (uint8_t)value;
}
void m68k_write_memory_16(unsigned int address, unsigned int value) {
  uint32_t offset;
  address &= 0xFFFFFFU;
  record_access(address, 1, 2U);
  if (in_ram(address, 2U)) {
    offset = address - RAM_BEGIN;
    g_ram[offset] = (uint8_t)(value >> 8U);
    g_ram[offset + 1U] = (uint8_t)value;
  }
}
void m68k_write_memory_32(unsigned int address, unsigned int value) {
  uint32_t offset;
  address &= 0xFFFFFFU;
  record_access(address, 1, 4U);
  if (in_ram(address, 4U)) {
    offset = address - RAM_BEGIN;
    g_ram[offset] = (uint8_t)(value >> 24U);
    g_ram[offset + 1U] = (uint8_t)(value >> 16U);
    g_ram[offset + 2U] = (uint8_t)(value >> 8U);
    g_ram[offset + 3U] = (uint8_t)value;
  }
}

/* Writes one JSON line describing the just-completed instruction (identified by g_prev_*) and
 * every access recorded while it executed, then flushes stdout. */
static void emit_previous_instruction(void) {
  uint32_t i;
  printf("{\"ordinal\":%u,\"pc\":\"0x%08X\",\"primary\":\"0x%04X\",\"extension\":",
         g_prev_ordinal, g_prev_pc, g_prev_primary);
  if (g_prev_has_extension) printf("\"0x%08X\"", g_prev_extension); else printf("null");
  printf(",\"length\":%u,\"accesses\":[", g_prev_length);
  for (i = 0; i != g_pending_access_count; ++i) {
    const pending_access *entry = &g_pending_accesses[i];
    printf("%s{\"ordinal\":%u,\"kind\":\"%s\",\"address\":\"0x%08X\",\"byteWidth\":%u}",
           i ? "," : "", entry->ordinal, entry->is_write ? "write" : "read", entry->address,
           entry->byte_width);
  }
  printf("]}\n");
  fflush(stdout);
  g_pending_access_count = 0U;
}

/* Blocks reading exactly one line from stdin. Returns 1 for "continue", 0 for "stop" (also 0 for
 * any other content or EOF, per the documented fail-closed default). */
static int read_reply(void) {
  char line[16];
  if (fgets(line, (int)sizeof(line), stdin) == NULL) return 0;
  return strncmp(line, "continue", 8) == 0;
}

static void before_instruction(unsigned int pc_arg) {
  char disassembly[128];
  uint32_t pc = pc_arg & 0xFFFFFFU;
  uint32_t length;

  if (g_stopped) { m68k_end_timeslice(); return; }
  if (g_steps >= HARD_SAFETY_MAX_STEPS) { g_stopped = 1; m68k_end_timeslice(); return; }

  if (g_have_prev) {
    emit_previous_instruction();
    if (!read_reply()) { g_stopped = 1; m68k_end_timeslice(); return; }
  }

  /* m68k_disassemble reports the real, uncapped byte length of the instruction at pc without
   * executing it; used to size the recorded extension and (by the Python driver) to detect the
   * next taken-branch delta. */
  length = m68k_disassemble(disassembly, pc, M68K_CPU_TYPE_68000);
  if (length < 2U) length = 2U;

  /* This pc's own identity, including its own ordinal, is captured now (before any of its
   * accesses are recorded) so it can be emitted as "the previous instruction" the next time this
   * hook fires. The instruction's ordinal is always allocated before any of its own access
   * ordinals, exactly like the original per-access-ordinal-only contract. */
  g_prev_ordinal = g_ordinal++;
  g_prev_pc = pc;
  g_prev_primary = (uint16_t)raw_read_16(pc);
  g_prev_has_extension = length >= 4U;
  g_prev_extension = g_prev_has_extension ? raw_read_32(pc + 2U) : 0U;
  g_prev_length = length;
  g_have_prev = 1;
  ++g_steps;
  m68k_end_timeslice();
}

int main(int argc, char **argv) {
  FILE *rom_file;
  long rom_size;

  if (argc != 2) { fprintf(stderr, "usage: sonic-startup-musashi-scan-oracle <rom>\n"); return 2; }

  rom_file = fopen(argv[1], "rb");
  if (!rom_file) { fprintf(stderr, "sonic-startup-musashi-scan-oracle: cannot open rom\n"); return 2; }
  fseek(rom_file, 0, SEEK_END);
  rom_size = ftell(rom_file);
  fseek(rom_file, 0, SEEK_SET);
  if (rom_size <= 0 || rom_size > (long)RAM_BEGIN) { fprintf(stderr, "sonic-startup-musashi-scan-oracle: unexpected rom size\n"); fclose(rom_file); return 2; }
  g_rom_len = (uint32_t)rom_size;
  g_rom = (uint8_t *)malloc(g_rom_len);
  if (!g_rom || fread(g_rom, 1, g_rom_len, rom_file) != g_rom_len) { fprintf(stderr, "sonic-startup-musashi-scan-oracle: short read\n"); fclose(rom_file); return 2; }
  fclose(rom_file);

  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_init();
  m68k_set_instr_hook_callback(before_instruction);
  m68k_pulse_reset();

  while (!g_stopped) m68k_execute(1000000);

  free(g_rom);
  return 0;
}
```

## Test Coverage

- `tests/probe_genesis_startup_test.py` -- unconditional CLI tests for the two generic startup
  probes (no ROM/Musashi dependency), including width-aware mapping boundary/crossing cases and
  the decode -> lift -> effect -> lowering `"support"` field for all five selected startup forms.
- `tests/stage1_classifier_test.py` -- fully synthetic tests for the stage-1 semantic classifier,
  including the 5-key contract and the `semantic_classifier_gap` marker.
- `tests/stage1_classifier_tooling_only_test.py` -- asserts no production/runtime code imports the
  stage-1 classifier.
- `tests/sonic_startup_inventory_gates_test.py` -- synthetic tests for graceful gate-failure
  behavior (missing/mismatched ROM, missing/mismatched Musashi pin), including the "no partial
  cache file on gate failure" acceptance criterion.
- `tests/sonic_startup_inventory_privacy_test.py` -- the no-commercial-data-leakage test, driving
  the real `run_scan` streaming driver against a small, fully synthetic, project-authored fake
  adapter that implements exactly the documented wire protocol; it also compares independently
  produced cache files and captured sanitized summaries from two fresh full scan executions.
- `tests/sonic_startup_inventory_semantic_gap_test.py` -- synthetic tests for the
  `EXIT_SEMANTIC_GAP` post-scan exit-code decision and for MOVE forms with differing destination
  addressing modes never collapsing into one aggregated entry.
- `tests/sonic_startup_inventory_adapter_failure_test.py` -- also extracts the documented adapter
  into a temporary directory, compiles it against a synthetic Musashi declaration header, and
  executes RAM-end 8/16/32-bit read/write boundary regressions. The 16- and 32-bit crossings stay
  unmodified, return all ones on reads, and retain their original recorded width for the driver's
  normalized `hardware_frontier` classification.

None of these tests require the commercial ROM or a local Musashi checkout to pass; they always
run under plain CI.
