# SEGARECOMP Post-SEG-007 Architecture Refactor and Evolution Contract

**Status:** Proposed architecture contract for a future refactor  
**Intended consumer:** A coding/refactoring agent working in `marinocg/segarecomp` after SEG-007 is complete  
**Primary goal:** Refactor the current codebase into enforceable modules without changing behavior, while leaving a clean path for Genesis, Sega CD, 32X, Master System, Game Gear, and Saturn support.


> **SEG-018-T002 update (physical layout supersession).** The responsibility rules in this contract
> (core / cpu / device / machine composition / recompiler / codegen / runtime, and the dependency
> direction between them) remain valid. Their *physical* layout was subsequently productized by
> SEG-018-T002 as a multi-platform workspace:
>
> ```text
> libs/       reusable implementation: core, cpu/m68k, recompiler, codegen/c11,
>             device/sega/genesis, media, legacy_compat (each: local CMakeLists.txt,
>             include/segarecomp/..., src/)
> platforms/  concrete machine/product composition: platforms/genesis/{machine,runtime,viewer,compat}
>             (other platforms are documentation only until they have code)
> apps/       applications (apps/segarecomp CLI)
> tests/  tools/  docs/
> ```
>
> `machine/<name>` composition in the sections below now lives at `platforms/<name>/machine/`, the
> generated-program runtime at `platforms/<name>/runtime/`, and machine-level code stays out of
> `libs/`. `platforms/` is not a generic abstraction layer: it is the concrete product-composition
> area. CPU and device semantics remain below it and must not depend upward on product composition.
> Paths in the historical sketches below (`include/segarecomp/...`, `src/...`, `runtime/...`) name
> logical modules; map them to the locations above. Include names (`segarecomp/<module>/...`) are
> unchanged. Known pre-existing couplings (machine frontend -> codegen headers, codegen -> machine
> link, address types ownership) are deliberately left for later SEG-018 tasks.

---

## 1. Purpose

This document defines the architectural direction for `segarecomp` after SEG-007 reaches its acceptance goal.

It exists because the current implementation has grown organically while proving difficult technical concepts:

- a reusable MC68000 static pipeline,
- static discovery and partial-program generation,
- fail-closed frontiers,
- Genesis memory/device resolution,
- generated strict-C11 execution,
- a persistent Genesis runtime bridge,
- deterministic reports and provenance,
- commercial-ROM validation without committing commercial data.

The conceptual architecture is largely sound, but its **physical source layout has not kept pace**. Several files now own too many unrelated concerns, and Genesis-specific behavior has leaked into MC68000-oriented code.

The refactor must preserve the semantic route established by SEG-012 and the behavior validated by SEG-007. It is a **modularization and ownership refactor**, not a rewrite.

---

## 2. Timing

Do this refactor **after SEG-007 is complete and validated**, and preferably before substantial SEG-011 gameplay expansion.

Do not perform the structural migration while SEG-007 is still moving rapidly unless an immediate blocker forces a localized extraction.

At the start of the refactor, the agent must re-scan the current repository. File names and symbols described here are a snapshot of the architecture that motivated this document and may have changed by then.

Create a dedicated architecture/refactor milestone or backlog item using the next available project ID. Do not assume a specific ID is still free.

---

## 3. Mandatory preservation rules

The refactor must preserve these project-level invariants.

### 3.1 Static recompilation remains the core model

Generated programs must not depend on a target CPU interpreter at execution time.

Generated runtime code must never:

- fetch target opcodes,
- decode target opcodes,
- dynamically reinterpret the original ROM as CPU instructions.

All accepted target instructions are understood at translation time.

### 3.2 Provenance remains explicit

Preserve typed provenance across the pipeline:

- target CPU address,
- source image offset,
- source bytes,
- source instruction length,
- relevant memory/bus evidence.

Do not replace provenance with untyped strings or lossy debug-only metadata.

### 3.3 Fail closed

Unknown or unsafe conditions must remain typed frontiers/rejections rather than silently guessed behavior.

Examples include:

- unknown instruction forms,
- unresolved control flow,
- unsupported devices,
- unsupported memory regions,
- invalid address mapping,
- scheduling/interrupt behavior that has not yet been modeled.

### 3.4 Deterministic generation remains mandatory

Equivalent inputs and options must produce deterministic generated artifacts and deterministic reports.

### 3.5 Preserve independent validation

Musashi or other independent reference/oracle validation remains valuable and must not be weakened by the refactor.

### 3.6 Preserve one semantic route

SEG-012 established a shared MC68000 route.

The refactor must **split that route into modules**, not recreate multiple compiler paths.

Do not reintroduce:

- startup-only decoders,
- opcode-specific bypass compilers,
- alternate CPU execution semantics for individual scenarios,
- duplicate lowering implementations.

---

# 4. Core architectural model

The mature architecture should distinguish these concerns:

1. **Core contracts**
2. **CPU architecture semantics**
3. **Hardware devices**
4. **Machine composition**
5. **Image/media ingestion**
6. **Recompiler orchestration**
7. **C11 code generation**
8. **Generated native runtime**
9. **CLI/tools**
10. **Validation/tests**

The critical distinction is:

> CPUs implement ISA semantics.  
> Devices implement hardware semantics.  
> Machines wire CPUs, buses, memory, and devices together.  
> The recompiler composes discovered CPU programs with machine knowledge.  
> Codegen emits already-understood programs.  
> The native runtime provides dynamic machine behavior required by those generated programs.

---

# 5. Host side versus generated-program side

The source tree must clearly separate the **translation host** from the **generated native runtime**.

Conceptually:

```text
                    HOST / TRANSLATION SIDE

 image/media
     |
     v
 cpu/* ---------> recompiler ---------> codegen/c11
                       ^                    |
                       |                    v
                 machine/*             generated C
                       ^
                       |
                  device/*

------------------------------------------------------------
               GENERATED-PROGRAM ABI BOUNDARY
------------------------------------------------------------

                       |
                       v
               native runtime/*
                       |
             RAM / bus / devices /
           scheduling / synchronization
```

The generated runtime is not merely a test helper. It is part of the native program architecture.

---

# 6. Current structural problems to correct

The exact code will have evolved by the time this is implemented, but the current design problems that motivated this contract include:

- `include/segarecomp/m68k_pipeline.hpp` acting as a god-header.
- Large `src/m68k_pipeline*.cpp` files owning decode, frontend, discovery, machine policy, startup behavior, and lowering concerns.
- Genesis controller-I/O and related machine policy appearing under the MC68000 pipeline layer.
- `main.cpp` knowing too much about CPU semantics, startup profiles, analysis composition, and emission.
- CMake building most production C++ into one broad core target, so physical dependency boundaries are not enforced.
- partial-program/frontier concepts being closely coupled to the MC68000 frontend even though they are higher-level recompiler concepts.
- runtime bridge C code living under `tools/` even though it defines and implements the generated program's persistent runtime ABI.

The refactor should solve ownership, not merely reduce line counts.

---

# 7. Target long-term tree

This is the intended mature shape once the project supports the planned Sega machines.

Do **not** create every empty directory during the first refactor. Grow toward this structure as real implementations appear.

```text
segarecomp/
|
+-- include/segarecomp/
|   |
|   +-- core/
|   |   +-- address.hpp
|   |   +-- provenance.hpp
|   |   +-- diagnostic.hpp
|   |   +-- bus.hpp
|   |   +-- image.hpp
|   |
|   +-- cpu/
|   |   +-- m68k/
|   |   |   +-- instruction.hpp
|   |   |   +-- effective_address.hpp
|   |   |   +-- decode.hpp
|   |   |   +-- ir.hpp
|   |   |   +-- lift.hpp
|   |   |   +-- effects.hpp
|   |   |   +-- static_program.hpp
|   |   |   +-- discovery.hpp
|   |   |   +-- frontier.hpp
|   |   |
|   |   +-- z80/
|   |   +-- sh2/
|   |
|   +-- device/
|   |   +-- sega/
|   |       +-- genesis/
|   |       +-- sega_cd/
|   |       +-- x32/
|   |       +-- sega_8bit/
|   |       +-- saturn/
|   |
|   +-- machine/
|   |   +-- genesis/
|   |   +-- sega_cd/
|   |   +-- x32/
|   |   +-- master_system/
|   |   +-- game_gear/
|   |   +-- saturn/
|   |
|   +-- media/
|   |   +-- cartridge.hpp
|   |   +-- cd_image.hpp
|   |   +-- cue_bin.hpp
|   |
|   +-- recompiler/
|   |   +-- program.hpp
|   |   +-- frontier.hpp
|   |   +-- result.hpp
|   |   +-- compilation_plan.hpp
|   |   +-- pipeline.hpp
|   |
|   +-- codegen/
|       +-- c11/
|
+-- src/
|   +-- core/
|   +-- cpu/
|   |   +-- m68k/
|   |   +-- z80/
|   |   +-- sh2/
|   +-- device/sega/
|   +-- machine/
|   +-- media/
|   +-- recompiler/
|   +-- codegen/c11/
|
+-- runtime/
|   +-- common/
|   +-- components/
|   |   +-- sega/
|   |       +-- genesis/
|   |       +-- sega_cd/
|   |       +-- x32/
|   |       +-- sega_8bit/
|   |       +-- saturn/
|   |
|   +-- machines/
|       +-- genesis/
|       +-- sega_cd/
|       +-- x32/
|       +-- master_system/
|       +-- game_gear/
|       +-- saturn/
|
+-- apps/
|   +-- segarecomp/
|       +-- main.cpp
|       +-- commands/
|
+-- tools/
|   +-- bridge/
|   +-- backlog/
|   +-- inventory/
|   +-- ghidra/
|
+-- tests/
    +-- unit/
    |   +-- cpu/
    |   +-- device/
    |   +-- machine/
    +-- integration/
    +-- differential/
    +-- generated_runtime/
    +-- bridge/
    +-- cli/
    +-- fixtures/
```

---

# 8. Module responsibilities

## 8.1 `core/`

Own only genuinely cross-target contracts.

Examples:

- target address types,
- image offsets,
- provenance,
- generic diagnostics infrastructure,
- generic bus access width/direction where it is truly CPU-independent,
- deterministic utility types.

`core` must not know:

- MC68000,
- Genesis,
- Saturn,
- startup scenarios,
- controller I/O,
- generated C details.

Keep it small.

---

## 8.2 `cpu/m68k/`

Own MC68000 architecture semantics only.

Examples:

- registers and CCR semantics,
- instruction representation,
- effective-address decoding,
- legal EA combinations,
- decode,
- CPU-specific IR,
- instruction effects,
- CPU control-flow behavior,
- call/return semantics,
- CPU-level static program representation,
- CPU discovery behavior.

It must not know why a caller selected a given capability.

Forbidden dependency:

```text
cpu/m68k -> machine/genesis
cpu/m68k -> device/sega/genesis
```

The MC68000 should not know that it is executing Sonic, Genesis startup, Sega CD code, or Saturn sound code.

---

## 8.3 `cpu/z80/`

When introduced, it should use the same architectural style but keep its own CPU state and semantics explicit.

Reuse generic infrastructure where appropriate:

- provenance,
- block/control-flow contracts,
- generic bus requests,
- reporting conventions.

Do **not** force MC68000 and Z80 into one universal CPU state or universal semantic IR.

---

## 8.4 `cpu/sh2/`

The SH-2 implementation is particularly important because it can be reused by both 32X and Saturn.

It should own:

- SH-2 decode,
- SH-2 instruction representation,
- SH-2-specific IR/effects,
- SH-2 registers and status,
- SH-2 control flow,
- SH-2 static discovery,
- SH-2 C11 lowering inputs.

It must contain no 32X or Saturn hardware behavior.

---

## 8.5 `device/`

Devices implement hardware behavior that is reusable across machine configurations.

Examples:

```text
device/sega/genesis/
    vdp/
    io/
    ym2612/
    psg/
    z80_subsystem/

device/sega/sega_cd/
    gate_array/
    word_ram/
    pcm/
    cd_controller/
    graphics/
    communication/

device/sega/x32/
    video/
    communication/
    pwm/
    interrupts/
    bus_control/

device/sega/sega_8bit/
    vdp/
    psg/
    controller/
    mapper/

device/sega/saturn/
    scu/
    smpc/
    vdp1/
    vdp2/
    scsp/
    cd_block/
```

Do not put complete-machine orchestration inside a device.

---

# 9. `machine/` is composition

Use a `machine/` composition concept, not a vague abstraction layer, once multiple related Sega systems exist. (Physically, per the SEG-018-T002 update above, it lives under the concrete `platforms/<machine>/machine/` product area.)

The machine layer defines:

- which CPU instances exist,
- which devices exist,
- memory maps,
- address routing,
- interrupt wiring,
- bus ownership,
- scheduling,
- shared memory,
- machine boot/reset composition.

It should not redefine CPU instruction semantics or duplicate device implementations.

---

# 10. Genesis base machine

Genesis becomes the reusable base for plain Genesis, Sega CD, and 32X configurations.

Conceptually:

```text
GenesisBase
+-- MC68000 main CPU
+-- Z80 subsystem
+-- Genesis VDP
+-- YM2612
+-- PSG
+-- controller I/O
+-- work RAM
+-- Z80 RAM
+-- base interrupt wiring
+-- base bus/address behavior
```

Suggested structure:

```text
machine/genesis/
+-- base/
|   +-- bus.*
|   +-- memory_map.*
|   +-- composition.*
|   +-- reset.*
|
+-- standalone.*
```

The distinction between `base` and the standalone configuration prevents Sega CD and 32X from copying a full Genesis implementation.

---

# 11. Sega CD composition

Sega CD should be modeled as:

```text
SegaCdMachine
|
+-- GenesisBase
|
+-- SegaCdExtension
    +-- sub MC68000
    +-- program RAM
    +-- Word RAM
    +-- gate-array / communication registers
    +-- CD subsystem
    +-- PCM audio
    +-- graphics/stamp hardware
    +-- BIOS/boot behavior
    +-- Genesis <-> Sega CD arbitration
```

Unique Sega CD hardware belongs in:

```text
device/sega/sega_cd/
```

Sega CD machine wiring belongs in:

```text
machine/sega_cd/
+-- extension.*
+-- address_overlay.*
+-- memory_map.*
+-- boot.*
+-- machine.*
```

Do not copy Genesis VDP, Genesis controller I/O, Genesis work RAM behavior, or the MC68000 implementation into Sega CD-specific code.

---

# 12. 32X composition

32X should be modeled similarly at the machine level, while adding very different hardware:

```text
X32Machine
|
+-- GenesisBase
|
+-- X32Extension
    +-- master SH-2
    +-- slave SH-2
    +-- SDRAM
    +-- framebuffer RAM
    +-- 32X video hardware
    +-- communication registers
    +-- PWM audio
    +-- interrupt/synchronization logic
    +-- Genesis <-> 32X bus arbitration
```

Unique 32X hardware belongs in:

```text
device/sega/x32/
```

The SH-2 implementation belongs only in:

```text
cpu/sh2/
```

because Saturn reuses it.

---

# 13. Preventing duplication in add-on machines

The project must distinguish three forms of reuse.

## 13.1 CPU reuse

Examples:

```text
MC68000:
    Genesis      -> main CPU
    Sega CD      -> main + sub CPU
    32X          -> main CPU
    Saturn       -> sound CPU

SH-2:
    32X          -> master + slave
    Saturn       -> master + slave
```

CPU implementation appears exactly once per CPU architecture.

## 13.2 Device reuse

Genesis devices used by Genesis/Sega CD/32X should be implemented once and composed into each machine.

## 13.3 Machine-composition reuse

The reusable Genesis base wiring should exist once.

Do not create:

```text
SegaCdBus = copied GenesisBus + modifications
X32Bus    = copied GenesisBus + modifications
```

Instead, model address-space overlays.

Conceptually:

```text
MC68000 request
      |
      v
MachineAddressRouter
      |
      +-- add-on overlay claims address?
      |       |
      |       +-- yes -> extension
      |
      +-- otherwise -> GenesisBase bus
```

Sega CD and 32X overlays are separate concrete implementations.

---

# 14. Prefer composition, not inheritance

Do not build machine inheritance such as:

```cpp
class SegaCdMachine : public GenesisMachine;
class X32Machine : public GenesisMachine;
```

Prefer concrete composition.

Conceptually:

```cpp
struct SegaCdMachine {
    GenesisBase genesis;
    SegaCdExtension cd;
};

struct X32Machine {
    GenesisBase genesis;
    X32Extension x32;
};
```

Likewise, do not prematurely introduce a generic template/plugin abstraction such as:

```cpp
template<class Base, class Addon>
class AddonMachine;
```

The project should share concrete implementation, not invent a framework before multiple real uses prove one is needed.

---

# 15. Potential combined add-ons

The composition model should not artificially prevent:

```text
GenesisBase
    + SegaCdExtension
    + X32Extension
```

if that configuration is ever required.

Do **not** build generic optional-add-on machinery now solely for this possibility.

Choose boundaries that do not make it impossible later.

---

# 16. Master System and Game Gear

Master System and Game Gear should share real 8-bit Sega components without being forced into one giant abstract framework.

Possible structure:

```text
device/sega/sega_8bit/
+-- vdp/
+-- psg/
+-- controller/
+-- mapper/
```

Then machine-specific composition:

```text
machine/master_system/
+-- memory_map.*
+-- ports.*
+-- machine.*

machine/game_gear/
+-- memory_map.*
+-- ports.*
+-- machine.*
+-- game_gear_specific_video_or_lcd_behavior.*
```

Extract shared components when the second machine proves the need.

Avoid inventing a speculative `Sega8BitFramework`.

---

# 17. Saturn remains a distinct machine

Do not model Saturn as an evolution of Genesis.

Conceptually:

```text
Saturn
+-- master SH-2
+-- slave SH-2
+-- SCU
+-- SMPC
+-- VDP1
+-- VDP2
+-- SCSP
+-- sound MC68000
+-- CD block
+-- work RAM
+-- video RAM
+-- arbitration
+-- scheduling
```

Suggested machine structure:

```text
machine/saturn/
+-- memory_map.*
+-- machine.*
+-- scheduler.*
+-- arbitration.*
+-- boot.*
+-- checkpoint.*
```

Saturn-specific devices belong under:

```text
device/sega/saturn/
```

SH-2 remains under `cpu/sh2/`.

---

# 18. Why 32X is a useful Saturn stepping stone

32X and Saturn both use two SH-2 CPU instances.

Therefore 32X can prove several major architectural capabilities before Saturn:

- SH-2 decode/lift/effects,
- generated SH-2 C11,
- multiple instances of the same CPU architecture,
- independent CPU state,
- shared memory,
- inter-CPU signaling,
- scheduling/yield points,
- CPU-to-machine-runtime handoff,
- synchronization and contention concepts.

A future recompiled 32X machine might look conceptually like:

```text
RecompiledMachine
+-- CpuProgram[0] : m68k
+-- CpuProgram[1] : sh2 master
+-- CpuProgram[2] : sh2 slave
+-- shared memory
+-- devices
+-- execution plan
```

Saturn can then reuse the SH-2 and multi-CPU architecture while adding very different machine devices.

32X does **not** solve Saturn's VDP1, VDP2, SCU, SCSP, SMPC, CD block, or Saturn-specific arbitration.

Treat 32X as a CPU/multi-processor architecture stepping stone, not as a "small Saturn."

---

# 19. Recompiler layer

Higher-level program composition belongs in:

```text
recompiler/
```

This includes concepts that are not intrinsically MC68000 semantics:

- partial programs,
- multiple unresolved frontiers,
- machine compilation plans,
- mapping of CPU programs into machine programs,
- runtime-selected frontier exits,
- compilation result composition.

A useful conceptual model is:

```text
RecompiledProgram
+-- cpuProgram
+-- machinePlan
+-- unresolvedFrontiers[]
+-- provenance
```

For multi-CPU machines:

```text
RecompiledMachine
+-- cpuPrograms[]
+-- machinePlan
+-- unresolvedFrontiers[]
+-- provenance
```

Do not create one universal CPU IR to make this possible. The recompiler can hold CPU-specific program variants/types.

---

# 20. Frontier ownership

SEG-007 demonstrated that a partial program may have:

- multiple exits,
- CPU frontiers,
- device frontiers,
- known-but-unemitted targets,
- runtime-selected exits.

These are higher-level recompilation/program concerns.

The MC68000 module may produce CPU-specific unresolved control-flow facts, but promotion into a machine-level partial program belongs above it.

Possible conceptual frontier categories:

```text
CpuFrontier
DeviceFrontier
ControlFlowFrontier
MappingFrontier
SchedulingFrontier
```

Do not collapse these into an untyped text reason.

Preserve existing privacy/reporting boundaries.

---

# 21. Profiles and startup scenarios

Current names such as:

```text
genesis_rom_startup
general_startup
```

represent workflows/scenarios, not MC68000 ISA semantics.

Long term, scenario composition should move out of the CPU layer.

Conceptually:

```text
GenesisStartupWorkflow
    |
    +-- Genesis machine/address policy
    +-- selected MC68000 capabilities
    +-- discovery policy
    +-- entry/reset state
```

It then invokes the MC68000 pipeline.

During migration, preserve established CLI/report contracts using compatibility wrappers if necessary.

Do not break wire/report schemas merely to achieve prettier naming.

---

# 22. MC68000 decode split

Do not create one source file per opcode.

Split by cohesive instruction families.

For example:

```text
cpu/m68k/
+-- decode.cpp
+-- decode_data.cpp
+-- decode_arithmetic.cpp
+-- decode_control.cpp
+-- decode_bit.cpp
+-- decode_system.cpp
+-- lift.cpp
+-- effects.cpp
+-- discovery.cpp
+-- call_return.cpp
+-- frontier.cpp
```

Possible grouping:

```text
decode_data:
    MOVE
    MOVEA
    MOVEQ
    MOVEM
    CLR
    LEA
    related data-transfer forms

decode_arithmetic:
    ADD
    SUB
    CMP
    quick/immediate families

decode_bit:
    BTST
    BCHG
    BCLR
    BSET

decode_control:
    Bcc
    BRA
    BSR
    DBcc
    JSR
    JMP
    RTS

decode_system:
    LINK
    UNLK
    USP
    RESET
    STOP
    etc.
```

Line count is a warning signal, not an architecture rule.

A rough preference of hundreds rather than several thousand lines per cohesive source file is reasonable, but cohesion matters more than arbitrary size.

---

# 23. Generated native runtime is a first-class module

The current bridge runtime files are architecturally important:

```text
tools/genesis_startup_bridge_runtime.h
tools/genesis_startup_bridge_runtime.c
```

They define/implement, among other things:

- persistent generated CPU/runtime state,
- memory access ABI,
- stop classes,
- diagnostic categories,
- provenance,
- control transfer,
- finite dispatch,
- runtime memory/device routing,
- sanitized/full report writing.

This is not ordinary tooling.

After SEG-007, move this concept into a first-class strict-C11 runtime module.

Initial shape may be:

```text
runtime/
+-- genesis/
    +-- include/
    |   +-- segarecomp_runtime/
    |       +-- genesis.h
    +-- src/
        +-- runtime.c
        +-- memory.c
        +-- controller_io.c
        +-- dispatcher.c
        +-- report.c
```

Do not over-split on day one if a smaller number of cohesive C files is cleaner.

The public generated-program ABI may still use one umbrella header.

---

# 24. Runtime versus codegen

Do not put the runtime implementation inside `codegen/c11`.

The distinction is:

```text
codegen/c11
    -> emits native program source

runtime/*
    -> provides the native machine ABI/services used by that source
```

Generated program:

```text
generated blocks
    + generated dispatcher/glue
    + native machine runtime
    = native recompiled executable
```

This distinction becomes essential once more machines exist.

---

# 25. Runtime composition for Genesis, Sega CD, and 32X

Do not create copied full runtimes:

```text
runtime/genesis.c
runtime/sega_cd.c   # copied Genesis + CD
runtime/x32.c       # copied Genesis + 32X
```

Instead, mature toward reusable runtime components:

```text
runtime/
+-- common/
|
+-- components/
|   +-- sega/
|       +-- genesis/
|       |   +-- bus.c
|       |   +-- ram.c
|       |   +-- io.c
|       |   +-- vdp.c
|       |
|       +-- sega_cd/
|       |   +-- gate_array.c
|       |   +-- word_ram.c
|       |   +-- pcm.c
|       |   +-- ...
|       |
|       +-- x32/
|           +-- communication.c
|           +-- framebuffer.c
|           +-- pwm.c
|           +-- ...
|
+-- machines/
    +-- genesis/
    +-- sega_cd/
    +-- x32/
```

Then:

```text
Genesis runtime
    = Genesis components

Sega CD runtime
    = Genesis components + Sega CD components

32X runtime
    = Genesis components + 32X components
```

Machine runtimes own the concrete composition.

---

# 26. Static policy and runtime policy must not drift

The current implementation has both:

- translation-time Genesis device resolution,
- generated-runtime Genesis device routing.

These must not evolve as two independent semantic definitions.

Controller I/O is the clearest current example.

Desired ownership:

```text
              canonical Genesis device policy
                     /             \
                    /               \
            static resolution    runtime routing
             when foldable         when dynamic
```

Because host code is C++ and generated runtime is strict C11, "single owner" does not necessarily mean literally one callable function.

Acceptable strategies include:

- shared C-compatible descriptors/constants,
- generated policy tables,
- one canonical specification consumed by both implementations,
- small shared ABI contracts.

The invariant is:

> CPU instruction code does not decide controller behavior, and static/runtime paths do not independently invent controller behavior.

---

# 27. Bridge Python driver remains tooling

The current:

```text
tools/genesis_startup_bridge.py
```

is architecturally different from the runtime C files.

Its responsibilities include orchestration such as:

```text
invoke segarecomp
    ->
generate C
    ->
compile generated C + runtime
    ->
execute binary
    ->
validate canonical reports
    ->
compare runs / evidence
```

That belongs under tooling, for example:

```text
tools/bridge/genesis.py
```

It should consume the first-class runtime library rather than own it.

If this workflow later becomes a supported end-user application, it can migrate toward `apps/`.

---

# 28. CMake must enforce architecture

The physical module boundaries should be backed by build targets.

A post-refactor initial set could resemble:

```text
segarecomp_base
segarecomp_image
segarecomp_m68k
segarecomp_genesis_devices
segarecomp_genesis_machine
segarecomp_recompiler
segarecomp_c11
segarecomp_genesis_runtime
segarecomp_cli
```

As targets are added, a rough dependency direction is:

```text
CLI
 |
 v
recompiler
 |   \____________________
 |                        \
 v                         v
CPU modules            machine modules
                          |
                          v
                     device modules

recompiler -> codegen/c11

generated output -> native runtime
```

Critical build-time invariants:

```text
m68k MUST NOT depend on genesis
sh2 MUST NOT depend on x32 or saturn
device modules MUST NOT implement CPU instruction semantics
codegen MUST NOT discover instructions
CLI MUST NOT implement CPU or hardware semantics
native runtime MUST NOT depend on the host C++ recompiler
```

Use CMake targets to make forbidden dependencies difficult or impossible.

---

# 29. Add architecture checks

Add simple deterministic checks for architectural regressions.

Useful examples:

- fail if `cpu/m68k/` includes `machine/genesis/`,
- fail if `cpu/sh2/` includes `machine/x32/` or `machine/saturn/`,
- fail if `core/` imports CPU or machine headers,
- compile public headers standalone,
- keep warnings-as-errors and clang-tidy,
- optionally report unusually large files as a soft warning.

Do not use file-size limits as the primary architecture enforcement mechanism.

---

# 30. CLI structure

The CLI should become thin.

Possible structure:

```text
apps/segarecomp/
+-- main.cpp
+-- command.hpp
+-- commands/
    +-- inspect.cpp
    +-- analyze.cpp
    +-- m68k_frontend.cpp
    +-- genesis_startup.cpp
    +-- emit.cpp
    +-- probe.cpp
```

CLI responsibilities:

- parse arguments,
- load files,
- choose workflow,
- invoke public application/recompiler APIs,
- present results.

CLI must not own:

- instruction semantics,
- Genesis controller policy,
- memory-map rules,
- frontier construction,
- C lowering.

---

# 31. Testing shape

Tests should mirror architecture without weakening existing validation.

Suggested mature shape:

```text
tests/
+-- unit/
|   +-- core/
|   +-- cpu/
|   |   +-- m68k/
|   |   +-- z80/
|   |   +-- sh2/
|   +-- device/
|   +-- machine/
|
+-- integration/
|   +-- genesis/
|   +-- sega_cd/
|   +-- x32/
|   +-- master_system/
|   +-- game_gear/
|   +-- saturn/
|
+-- differential/
|   +-- m68k/
|   +-- z80/
|   +-- sh2/
|
+-- generated_runtime/
+-- bridge/
+-- cli/
+-- fixtures/
```

Preserve:

- independent CPU oracle checks,
- strict-C11 compilation tests,
- deterministic output tests,
- malformed/fail-closed tests,
- provenance/report-schema validation,
- commercial-ROM tests that consume only local authorized inputs.

Do not reorganize tests merely for cosmetic directory symmetry if doing so breaks stable fixture/document references without value.

---

# 32. First post-SEG-007 refactor: do not build the whole future tree

The first migration should introduce only boundaries justified by existing Genesis code.

A practical first target could be:

```text
include/segarecomp/
+-- core/
+-- cpu/m68k/
+-- device/sega/genesis/
+-- machine/genesis/
+-- recompiler/
+-- codegen/c11/

src/
+-- core/
+-- cpu/m68k/
+-- device/sega/genesis/
+-- machine/genesis/
+-- recompiler/
+-- codegen/c11/

runtime/
+-- genesis/

apps/
+-- segarecomp/

tools/
+-- bridge/
+-- existing project tools
```

Do not create empty Z80/SH-2/Saturn frameworks merely to match the mature diagram.

The architecture should be **extensible by shape**, not pre-built by speculation.

---

# 33. Migration strategy

The migration must separate structural change from semantic expansion.

Core rule:

> **MOVE CODE FIRST. CHANGE DESIGN SECOND.**

Do not combine this refactor with new opcodes, new hardware, new compatibility policies, or new commercial-game behavior unless a tiny fix is required to preserve equivalence.

Recommended sequence:

## Phase 1 - Freeze behavior and establish contracts

- Re-scan repository after SEG-007 completion.
- Record current generated outputs/reports for project-authored fixtures.
- Record relevant deterministic hashes/snapshots.
- Ensure all relevant tests pass.
- Document the target module dependency rules.
- Add/refine ADR if needed.

## Phase 2 - Establish CMake module targets

Create the new targets and migrate files gradually.

Do not require perfect final file decomposition before target boundaries exist.

## Phase 3 - Extract MC68000 ISA layer

Move:

- instruction types,
- EA representation,
- decode,
- IR,
- operation effects,
- CPU control-flow semantics.

Preserve existing behavior exactly.

## Phase 4 - Extract MC68000 static discovery/program layer

Move:

- static blocks/edges,
- CPU discovery,
- call/return CPU behavior,
- CPU-specific unresolved facts.

Keep machine promotion above this layer.

## Phase 5 - Extract Genesis hardware policy

Move Genesis-specific:

- address map,
- controller I/O,
- device resolution,
- Genesis-specific bus policy,
- runtime state policy.

Remove Genesis-specific concepts from MC68000 headers.

## Phase 6 - Extract recompiler-level partial-program/frontier composition

Move:

- partial program,
- multi-exit promotion,
- machine frontier classification,
- compilation plan.

## Phase 7 - Promote bridge runtime to first-class strict-C11 runtime

Move the architectural content of:

```text
tools/genesis_startup_bridge_runtime.h
tools/genesis_startup_bridge_runtime.c
```

into `runtime/`.

Keep ABI behavior unchanged.

## Phase 8 - Split C11 codegen

Codegen should consume already-understood program/machine structures and emit deterministic C11.

It must not rediscover instructions.

## Phase 9 - Thin the CLI

Move workflow implementation out of `main.cpp`.

## Phase 10 - Reorganize tests where useful

Do this after production module boundaries stabilize.

## Phase 11 - Remove compatibility facades

A temporary short `m68k_pipeline.hpp` facade may be used during migration to avoid a massive all-at-once change.

Remove it once internal users/tests migrate.

Do not leave the god-header permanently as a forwarding dumping ground.

---

# 34. Compatibility strategy

During migration, preserve established public/project contracts.

Acceptable temporary techniques:

- forwarding headers,
- type aliases,
- wrapper functions,
- deprecated compatibility APIs.

These are migration tools, not permanent architecture.

Do not alter:

- CLI wire/report outputs,
- stop categories,
- provenance shapes,
- deterministic artifact behavior,

unless there is a separately approved semantic task.

---

# 35. What not to do

The future agent must explicitly avoid these failure modes.

## 35.1 No universal CPU state

Do not invent:

```text
UniversalCpuState
UniversalInstruction
UniversalIrOpcode
```

merely because several CPUs are planned.

MC68000, Z80, SH-2, SH-4, etc. should retain explicit semantics.

## 35.2 No giant "Sega framework"

Do not create a broad `SegaMachineFramework` filled with extension points for hardware that does not yet exist.

Extract shared concepts after real duplication pressure appears.

## 35.3 No machine inheritance hierarchy

Avoid:

```text
Genesis -> SegaCD -> ...
Genesis -> 32X -> ...
```

Use composition.

## 35.4 No codegen-driven discovery

Codegen renders a known program. It does not decide what instructions exist.

## 35.5 No device behavior in opcode handlers

A `TST`, `MOVE`, or any individual instruction implementation must not know Genesis controller values or VDP behavior.

CPU operations issue typed memory/bus effects; machine/device layers resolve them.

## 35.6 No copied base machine runtimes

Sega CD and 32X must reuse Genesis runtime components rather than fork Genesis runtime code.

## 35.7 No premature plugin system

Use concrete composition until multiple implemented machines prove a more generic abstraction is necessary.

## 35.8 No semantic expansion hidden inside the refactor

The architecture migration is not permission to add unsupported behavior.

---

# 36. Architectural rules to encode in agent instructions

The future refactoring agent should treat the following as hard constraints:

```text
RULE 1:
CPU modules own ISA semantics only.

RULE 2:
Device modules own device semantics only.

RULE 3:
Machine modules own composition, mapping, routing, and scheduling.

RULE 4:
Recompiler modules own promotion/composition of CPU programs into machine programs.

RULE 5:
Codegen only renders an already-understood compilation plan.

RULE 6:
Generated runtime is strict C11 and independent of the host C++ recompiler.

RULE 7:
Static resolution and runtime routing must share one authoritative machine/device policy.

RULE 8:
Fail-closed behavior and provenance are preserved.

RULE 9:
CMake dependency boundaries are architectural enforcement, not convenience.

RULE 10:
Avoid speculative abstractions. Generalize only when at least two real consumers justify it.
```

---

# 37. Definition of success for the refactor

The refactor is successful when all of the following are true.

### Structure

- `m68k_pipeline.hpp` is no longer a god-header.
- Genesis device policy no longer lives inside the MC68000 semantic module.
- `main.cpp` is thin.
- generated runtime implementation no longer lives as incidental `tools/` C source.
- CMake has meaningful architectural targets.
- partial-program/frontier composition has an appropriate recompiler-level owner.

### Behavior

- existing SEG-007 accepted behavior is unchanged,
- generated C remains strict C11,
- deterministic generation remains stable,
- commercial-ROM policy remains unchanged,
- no target opcode decoding occurs in the generated runtime,
- stop/fail-closed behavior is preserved,
- provenance/report contracts remain valid.

### Extensibility

Adding the next real machine should not require violating existing module ownership.

Specifically:

- adding Z80 should not alter MC68000 state models,
- adding Master System should not require cloning Genesis,
- adding Sega CD should compose Genesis base + Sega CD extension,
- adding 32X should compose Genesis base + SH-2s + 32X extension,
- adding Saturn should reuse SH-2 while remaining its own machine.

---

# 38. Guidance for the next machines

A sensible architecture-validation progression is:

```text
Genesis
   |
   +--> Master System / Game Gear
   |       proves another CPU and shared 8-bit Sega devices
   |
   +--> Sega CD
   |       proves Genesis-base add-on composition and second MC68000
   |
   +--> 32X
           proves SH-2 and heterogeneous multi-CPU execution
             |
             v
           Saturn
           reuses SH-2 + multi-CPU lessons,
           adds Saturn-specific devices and scheduling complexity
```

This is an architectural progression, not a mandatory product roadmap.

---

# 39. Instructions to the future refactoring agent

Before changing code:

1. Read this document.
2. Read current architecture ADRs/contracts, especially those governing:
   - shared MC68000 route,
   - startup ownership,
   - partial-program/frontier behavior,
   - bridge/runtime ABI,
   - privacy/reporting.
3. Re-scan:
   - `include/segarecomp/`,
   - `src/`,
   - `runtime/` if it already exists,
   - `tools/genesis_startup_bridge*`,
   - root/subdirectory CMake files,
   - tests tied to SEG-007.
4. Compare actual current symbols with this document.
5. Produce a concrete symbol-to-module migration map before moving code.
6. Preserve behavior and public/report contracts during the first migration.
7. Make small reviewable moves with tests after each architectural boundary.
8. If this document conflicts with a newer accepted ADR, task contract, or explicit project decision, the newer accepted project contract wins. Record the conflict rather than silently choosing.
9. Do not create speculative modules for unimplemented machines solely to satisfy the long-term diagram.
10. Keep the codebase lean.

---

# 40. Final architectural summary

The intended long-term mental model is:

```text
CPU
    "What does this instruction mean?"

Device
    "What does this hardware component do?"

Machine
    "What exists in this machine and how is it connected?"

Recompiler
    "What statically understood native program can we construct?"

Codegen
    "How do we deterministically render that program as C11?"

Runtime
    "What dynamic machine behavior must the generated native program call?"

CLI / tools
    "How does a human or validation workflow invoke all of the above?"
```

For the closely related machines:

```text
Genesis
    = GenesisBase

Sega CD
    = GenesisBase + SegaCdExtension

32X
    = GenesisBase + X32Extension

Master System / Game Gear
    = shared real 8-bit Sega components + separate machine compositions

Saturn
    = independent Saturn machine composition
      + reusable SH-2 CPU module
      + reusable generic recompiler/runtime principles
```

The governing design principle is:

> **Share semantics and concrete components where the hardware is genuinely shared; keep machine composition explicit where the hardware differs. Avoid both copy/paste duplication and speculative universal frameworks.**

The post-SEG-007 refactor should therefore keep SEG-012's proven semantic architecture, but turn it into a real enforceable modular architecture that can grow naturally into the planned Sega family.
