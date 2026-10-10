# ADR 0097: Bounded Genesis compatibility repairs (local commercial-corpus pass)

- Status: Accepted.
- Predecessors: ADR 0043 (exception/interrupt contract), ADR 0049 (immutable copy aliases), ADR 0072 (Z80 platform), ADR 0096 (AOT policy).

## 1. Question and scope

Six locally held Genesis images failed to build, or stopped early, on the existing generated-native route. Every cause was
a bounded gap inside an existing semantic owner (CPU lowering, runtime device models, build-time analysis), not a new
architecture. This ADR records the repairs and, equally important, the explicit non-goals. Only non-reconstructable
classifications are recorded: no address, byte, or instruction from any commercial image appears in this repository.

Non-goals (new architecture, deliberately not decided here): general work-RAM-resident code (anything but a verbatim ADR 0049 alias or one absolute `JMP` thunk), rewritten/self-modifying RAM code, cartridge SRAM, lock-on/bank-switched mappers, any new device family.

## 2. Repairs

| # | Owner | Class of gap | Repair |
| --- | --- | --- | --- |
| 1 | C4 lowering (call push) | A zero initial SSP makes the first `BSR`/`JSR` push wrap the 32-bit A7 to `$FFFFFFFC`; a blanket `A7 < 4` underflow stop rejected it. | Removed. The 24-bit bus address of the wrapped extent (the top work-RAM long word) is still range-checked by the existing `m68k_stack_bus` guard, so only a genuinely out-of-RAM stack extent stops. |
| 2 | C4 lowering (bit ops) | `BCHG/BCLR/BSET` with a routed-device absolute destination (for example `BSET` on the Z80 bus request) was rejected as "lacks retained resolver fact". | Admitted exactly like memory CLR/Scc: both retained facts must be routed-device facts of the same region (`c4_read_then_write_destination_admitted`). |
| 3 | C11 emitter (divide) | `DIVU/DIVS #0` (data decoded as code by the broad immutable-ROM universe) leaves a statically dead division that an optimizing strict build rejects (`-Wdiv-by-zero` under `-Werror`). | For an immediate zero divisor only the architectural divide-by-zero exception arm is emitted. |
| 4 | Machine frontend + runtime | An IRQ6 vector slot pointing into work RAM (RAM jump-table convention) rejected the whole build. | The build proceeds with no IRQ6 root and the generated `main` records `irq6_vector_in_work_ram`. When bounded build-time preparation materialized the stub the slot points at (row 10), the stub address is an ordinary compiled handler entry and the interrupt is delivered by the normal CPU-owned exception entry. Otherwise a recognised VBlank interrupt stops fail-closed with the new typed pair `unsupported_interrupt_or_scheduling_event` / `irq6_vector_in_work_ram` (diagnostic 62); it is never silently dropped. |
| 5 | Controller I/O device | Cartridges read back CTRL1/CTRL2 (`CLR.B`, `BSET`) and read DATA3, and write the idle value to the serial control registers and the mapper SRAM-control register. | CTRL1/CTRL2 BYTE reads return the latch; DATA3 reads use the DATA1/DATA2 pin model with every line released; S-CTRL1/2/3 accept a BYTE write of `0` and `$A130F1` a BYTE write with bit 0 clear (the idle states this model already embodies). Every other value, width, direction, and neighbour stays fail-closed. |
| 6 | VDP | A DMA-fill length of zero was rejected; the 16-bit length counter wraps, so zero means `0x10000` (the rule memory-to-VRAM DMA already used). | Zero arms a 65536-byte fill. |
| 7 | Z80 area (68000 view) | A runtime-computed LONG access to the sound RAM window was rejected. | A LONG access wholly inside the window is two WORD accesses, high half first, each following the window's WORD rule; the bus-grant gate is evaluated once so a rejection mutates nothing. LONG across the window end, and the bank register, stay fail-closed. A statically known absolute LONG operand into the window is unchanged (still an unmapped-data frontier at generation time). |
| 8 | C4 emission | A statically foldable direct `JMP` whose target discovery could not admit (issue dropped with a non-authoritative admitted-unit walk) rejected the whole program as "incomplete C4 static edge". | Retained as a plain terminal; the unchanged dispatcher reaches its typed unemitted-target stop only if control ever gets there. |
| 9 | VDP | After a memory-to-VDP DMA completed, a plain DATA-port write was rejected until a fresh address command (the DMA superseded the selected transfer code). | A finished memory-to-VDP DMA leaves its write target as the selected transfer code and the post-transfer address as the current address (the DMA bit is ignored by the data port), so the cartridge may continue with plain DATA-port writes. A finished fill keeps the previous policy. |
| 10 | Build-time RAM-thunk producer | Control reaching a tiny work-RAM jump-table stub (Flicky, with copy-alias discovery; the IRQ6 vector stub of Sonic & Knuckles) stopped, because no compiled entry exists at a RAM address that is not a verbatim ROM copy. | A narrow producer, `genesis.ram_thunk` (authority `bounded_build_time_materialization`, verification `byte_identity`, no new authority). The existing bounded preparation loop (`segarecomp build`'s `m68k_alias` rounds and the bridge's `--discover-copy-aliases`) observes the frontier work RAM; an ADR 0049 alias keeps priority; otherwise the live bytes at the frontier (or at the cartridge's own IRQ6 vector target) are offered to the CPU-owned MC68000 decoder (`genesis_ram_jump_thunk_bytes`), which accepts exactly one `JMP (xxx).W/.L` and returns its decoded length. The descriptor (exact bytes) is re-decoded and re-lifted by the CPU owner when the emitter builds the entry (`--ram-thunk addr:bytes`), so target computation and retirement timing remain CPU-owned. The generated entry carries the same whole-instruction byte-identity guard as an alias identity: a rewritten stub never executes and stops with the typed unemitted-target stop. |

## 2a. The experiment and its correction

The first version of row 10 (commit `94150fe`) proved that tiny RAM jump-table stubs were the remaining blocker for Flicky and
Sonic & Knuckles, but it did so with a runtime two-opcode decoder in the step runner. That violates ADR 0077 (no runtime guest-opcode
decoder, fully static executable, CPU-owned semantics and timing) and also honoured rewritten stubs. It was replaced by the build-time
producer above: recognition and materialization happen only at build time, final execution is generated native code plus an exact
byte guard, a mutated stub fails closed, and the runtime contains no M68K opcode decoder
(`genesis_runtime_no_m68k_opcode_decoder_test`). The compatibility findings are unchanged: with copy-alias discovery Flicky needs one
49,152-byte alias plus 18 six-byte JMP stubs; S&K needs its IRQ6 vector stub (and any further stubs the preparation finds).

## 3. Evidence

Focused tests: `genesis_compat_repairs_generated_test` (synthetic images through the production emit route, strict C11, `-O2`
for the divide case), the runtime suites for controller I/O, VDP, Z80 RAM/view and IRQ6, and the updated C4 stack
matrix. Commercial images were used only for ephemeral diagnosis; results are recorded as classifications in the pull
request.

## 4. Consequences

Genesis images whose boot sequence needs only the repaired classes now build and run on the existing route. Images blocked by
the non-goals still stop at a typed frontier instead of being rejected at build time or mis-executed, so the next
architectural decision can be taken from an honest, reproducible stop.
