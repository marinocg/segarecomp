# SEG-014-T004 Genesis machine/device seam

## Delivered ownership inventory

| Responsibility | Owner |
| --- | --- |
| Genesis scenario composition, mapping-claim instruction source, static memory routing, C4 fact enrichment, frontier classification and eligibility | `machine/genesis` (`platforms/genesis/machine/src/frontend.cpp`) |
| MC68000 decoding, lifting, static graph traversal, calls, and CPU memory-access requests | `cpu/m68k` |
| Generic retained-program/frontier container and sort/dedup/bound operation | `recompiler` |
| Selected controller-I/O selector/value constants | `device/sega/genesis/controller_io_contract.h`, consumed by host policy and strict-C11 runtime |
| Existing controller/VDP runtime ABI and C11 routing implementation | Retained runtime support; physical runtime migration remains T005 scope |

The moved Genesis adapter implements `M68kStaticDiscoveryEnvironment`; it supplies mapping and
machine-policy facts but never makes traversal/successor decisions. Its frontier promotion keeps the
existing strict category/class gate: a populated access record alone never makes an exit runtime eligible.
`M68kStaticMemoryFact`, MOVEM-adjacent-LEA, and owned-cartridge-region facts remain machine/recompiler
analysis facts and do not enter `cpu/m68k`.

The C-compatible controller contract is intentionally a small shared specification, not a shared C++
runtime object or a new bus framework. It pins only the pre-existing supported controller selectors and
policy value, so host translation and generated-runtime routing cannot silently diverge while T005 keeps
runtime physical relocation separate.

`m68k_pipeline.hpp` remains a temporary compatibility export during SEG-014. It has no implementation
in the legacy `src/m68k_pipeline_frontend.cpp` path, which is removed by this task.
