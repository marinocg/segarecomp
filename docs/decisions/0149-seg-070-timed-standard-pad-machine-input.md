# ADR 0149: SEG-070 - the timed standard-pad machine input of the Sega CD machine

- Status: Accepted (SEG-070-T002). Serves the Checkpoint G schedule fixed in ADR 0148 but is independent of it: it knows no game, no
  address and no frame number.
- Scope: how a project-authored schedule of button transitions reaches the processors of the Sega CD machine, with what time base and
  ordering, what is refused, and how it is verified. It adds no pad protocol; it supplies the state of the existing Genesis controller
  model.
- Privacy: only project-authored fixtures and hand-derived expectations.

## Context

The Sega CD main bus routes the Genesis I/O block (`$A10000-$A1001F`) to the Genesis runtime's device host, whose controller pin
model (data latch, direction register, TH multiplexing, `0 = pressed`) already serves every pad read. The Sega CD machine never set a
pad, so every read was "all released" (ADR 0143 fixtures read `$7F`/`$33`). Checkpoint G (ADR 0148) needs one START press at a
deterministic machine time, observed by the real application through that protocol, with no game-specific trigger and with the
SEG-065 behaviour unchanged when no input is given. The Genesis machine applies its pad between runner slices, which is not
tick-exact; that is not reused.

## Decisions

1. **One optional machine input, not a replay system.** `--input-script FILE` (BIOS-image profile only; the synthetic profile has no
   controller and refuses it). `--input-log FILE` additionally writes the observation log (local diagnostic). Neither exists in the
   default run: no `input` report field, no event, no state; reports are byte-identical to SEG-065 (verified: a header-only script
   changes no other report field).
2. **Script format.** ASCII text, LF or CRLF, `#` comment lines and blank lines ignored, then the exact header
   `segarecomp-input-script 1` and lines `<tick> <port> <press|release> <BUTTON>[,<BUTTON>...]`. Fields are separated by exactly one
   space. Buttons: `UP DOWN LEFT RIGHT A B C START`, upper case, no repeat in a list. `MODE X Y Z` are refused as unsupported (the
   6-button protocol is not modelled); the machine therefore presents a 3-button pad. Limits: 64 KiB, 256-byte lines, 4096 events.
3. **One canonical time.** `tick` is an absolute machine tick since reset (machine time 0 at power-on), the unit of every device in the
   machine: 500,000 ticks per master clock, 448,020,000,000 per NTSC frame (ADR 0145), 0 to 2^63 - 1. Times relative to a handoff or a
   named event are not part of the format: a tool that wants them converts them from measured machine times (as the report's
   `epoch_tick`, onset ticks and capture facility already expose them). No script can express a program counter or any application
   state, so no game-specific trigger exists.
4. **Validation before execution.** The whole script is parsed and checked before any guest instruction; a refusal is typed: exit 23,
   `{"result":"input_refused","error":CLASS,"line":N}`. Classes: `missing_header`, `bad_header`, `bad_event`, `bad_tick`,
   `tick_range`, `bad_port` (only 1 and 2), `bad_action`, `bad_button`, `unsupported_button`, `duplicate_button`, `tick_order` (ticks
   never decrease in file order), `redundant_transition` (a press of a held button or release of a released one - the script is
   simulated), `same_tick_conflict` (one button of one port touched twice at one tick), `too_many_events`, `too_large`,
   `line_too_long`, `binary`, `unreadable`, `unsupported_profile`. Nothing is applied from a refused script.
5. **Semantics and ordering.** Each event is a device event at its tick: it sets the pad mask of its port in the device host. It is
   processed before every other device event at that tick (CDD tick, timer, CDC DMA, graphics ASIC, vertical-blank onset) and, by the
   scheduler's existing rule (a device event at or before a processor's ready time runs first), before any processor instruction or
   interrupt acceptance at or after that tick. Events of one tick apply in file order. An access observes the pad state in force at
   its instruction's start tick, like every other device register in this machine (section 12 of the contract). Because transitions
   are device events at absolute ticks, the outcome cannot depend on how a run is partitioned into deadlines; the capture observer
   instant also treats a due pad event as a device event (by code reading; no test combines the capture observer with a script).
6. **Observation.** Byte reads of the data ports `$A10003`/`$A10005` (a word read reaches the odd byte) are counted per port and, with
   `--input-log`, logged (tick, vertical-blank onsets processed, port, value; bounded at 65,536, the overflow counted). The report's
   `input` object carries the event and applied counts, the pad masks at the end, the read counts, the logged count and an FNV digest
   of the log, so that equality over repeats and partitions is comparable in one value.
7. **No new protocol, no weakened stop.** Pressed bits reach a processor only through the existing pin model; every access shape that
   was a typed stop (`main_device_unsupported`: an even-address byte access of a data register, a version-register write, the
   unmodelled block above the pad ports) still is, with or without a script, and is never counted as an observation.

## Verification (project-authored)

- `sega_cd_input_test`: `tests/tools/sega_cd_input_table.c` (hand-derived from the documented 3-button protocol and this grammar): all
  eight buttons in both TH states, CTRL all-input / all-output / TH-only / partial outputs and the data latch (including bit 7), the
  word read, port independence; the transition timeline at one tick before, exactly at and after each event, an event at tick 0,
  three events at one tick across two ports, a release and a press at one tick, changes between reads, CRLF/comment/blank lines;
  arbitrary advance instants and eight strides give one timeline; the refusal rows (every class, with its line), the boundaries (4,096 events,
  64 KiB, 256-byte lines, 2^63 - 1); typed stops of unsupported shapes. A generated-native fixture
  (`tools/sega_cd_bios_input_fixture.py`, ordinary MC68000 instructions, both ports, both TH states, 32 polls): the values the program
  stored, the machine's own read log and the hand-derived values agree; a press exactly at a read's tick is seen by that read, one
  tick later it is not, one tick earlier it is; two events at one tick; identical report and read log over repeats, 8 seeded
  deadline partitions and 4 fixed slicings; every refusal exits 23 before any instruction; the typed stops are unchanged with a
  script present.

## Consequences

- The Checkpoint G schedule is expressed as two absolute ticks computed from measured vertical-blank onset instants; the runtime
  never learns where they came from.
- A game that uses the 6-button pad protocol would see a 3-button pad (documented non-goal).
- A later live-input front end can produce the same events; nothing here is specific to a script.
