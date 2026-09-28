# MC68000 reuse boundary (SEG-021-T023)

This document fixes what the MC68000 CPU work of SEG-021 owns and what a machine product (Genesis today; Sega
CD and 32X, which add a second and third M68K/SH-2 bus master, later) must supply. It records existing
architecture; it selects no new behavior and implements no Sega CD or 32X support.

## What the CPU owns (`libs/cpu/m68k`, `libs/codegen/c11/src/m68k.cpp`)

| Owned by the M68K CPU layer | Where |
| --- | --- |
| Primary/extension-word decode, legality partition (legal, line-A, line-F, illegal) | `decode.cpp`, `legality.cpp` |
| Typed IR and per-operation effect facts (PC, stack, CCR, register footprint, exception vector) | `ir.cpp`, `effects.cpp` |
| Effective-address decoding and control-EA classification | `effective_address.cpp` |
| Static discovery traversal (`discover_m68k_static_graph`) over a caller-supplied environment | `static_discovery.cpp` |
| Instruction timing rules and the data-dependent MULU/MULS/DIVU/DIVS helpers | `timing.cpp`, `timing_core.h` |
| Synchronous-exception entry arithmetic | `exception_core.h`, `timing.cpp` |
| C lowering of every operation against caller-named state (`emit_m68k_operation_c`) | `libs/codegen/c11/src/m68k.cpp` |

The library links only against `segarecomp::base`. Its code names no machine vocabulary. The single recorded
exception is the enumerator `M68kDecodeProfile::genesis_startup`, a decode-policy selector name that holds no
Genesis state or code; renaming it is churn with no ownership effect and is deliberately left.

## What a machine must supply

The emitter takes every piece of state and every platform-specific text as a caller-supplied name or seam:

- **State names** (`M68kMemoryEmissionContext`): data-register array, `SR`, address-register array, the inactive
  stack pointer slot, the memory array and its `[begin, end)` window, the call-frame identity/continuation arrays
  and depth, the program counter, and optional timing outcome slots. Nothing is a hidden global: two CPUs
  are two sets of names (see the guard below).
- **Runtime-routed protocol** (`M68kRuntimeCEmitter`): the platform's own text for a bus access, stop/provenance,
  return-address pop, indirect-target membership failure, call push, divide-by-zero and privilege-violation
  delivery, exception return and deferred-trace stop. The CPU decides *when* one is needed and owns the CPU
  semantics around it (address masking, stack-pointer and PC updates); the machine owns the text and the ABI.
- **Discovery environment** (`M68kStaticDiscoveryEnvironment`): instruction bytes by address, target admission,
  memory-access classification, the completion-`RTS` decision and immutable cartridge bytes.
- **Machine hooks named in ADR 0043**: exception-vector resolution, `reset_devices`, interrupt acknowledge and the
  scheduler that retires the CPU-owned cycle counts.

## What stays machine-owned

Address maps and mirrors, device registers and side effects (VDP, YM2612, PSG, Z80 bus arbitration, controller
I/O), interrupt sources, the bus-master arbiter, cartridge or disc mapping, the generated program prelude
(`GenesisRuntime`, the compiled-entry authority and the indirect-target set), and every fail-closed stop class
that names a device.

## Ownership of future products

- **Sega CD** adds a *second* MC68000 with its own memory map and a shared communications window. The reuse
  requirement is that the CPU layer be instantiated twice with independent state and different machine seams;
  the guard test below proves the state-independence half. The second bus, the gate-array registers and
  the sub-CPU program source are machine work, out of scope here.
- **32X** and Saturn contribute SH-2 cores, which are a different CPU library. This boundary is not a universal CPU
  interface: no shared CPU IR, state struct or execution interface is introduced, and none is justified until two
  CPUs exercise one.
- A machine that needs the *decoded-at-build-time* dispatch of computed jumps supplies its own compiled-entry
  authority (ADR 0009, 0047); the CPU only states that a target is register-computed.

## Guard

`tests/m68k_reuse_boundary_test.py` (labels `full`, `fast`) enforces, without any ROM:

1. `libs/cpu/m68k` links only `segarecomp::base`, includes nothing machine-owned, and names no machine identifier
   in code (comments aside; the one enumerator above is allow-listed).
2. `timing_core.h` and `exception_core.h` compile as strict C11 and define or require no data or external
   symbols (stateless header-only support).
3. Generated direct-route functions for representative operations reference only their caller-supplied state, no
   machine vocabulary and no call-frame globals.
4. Two live CPU states run the same generated functions interleaved in one process and end with exactly their own
   registers, memory and PC (no shared hidden state).

## Known, recorded boundary facts (not defects)

- Generated *call* forms (BSR/JSR/RTS) use the caller-named frame arrays. The conformance harness names them as
  process globals because it runs one CPU at a time; a machine instantiating two CPUs must name them per CPU
  (they are context strings, not emitter globals).
- The runtime-routed and immutable-ROM AOT lowerings are Genesis-owned text over the CPU's semantic emitter; a second
  machine supplies its own `M68kRuntimeCEmitter`.
- Capability gaps that remain after SEG-021-T023, with reasons and impact, are enumerated in
  `docs/testing/m68k-capability-coverage.md` under "Residual accounting".
