# Technical References

Record sources used to justify target behavior here. Each entry should include title, author or
publisher, revision, URL or publication identifier, access date, affected console/CPU, and the
specific implementation claim it supports.

Primary manuals and hardware test results are preferred. Emulator source may guide questions but
must not be copied, and its license must be reviewed before adapting implementation details.

## Indexed contracts

- [Bounded Genesis and SMS/Game Gear header classification](header-classification-contract.md)
  records the cited header facts and the project classification policy for SEG-000-T001.
- [MC68000 `MOVEQ` contract](moveq-contract.md) records the primary-manual facts,
  strict project decode policy, synthetic vectors, and pinned independent oracle for SEG-002-T001.
- [MC68000 multi-unit frontend contract](m68k-multi-function-frontend-contract.md)
  records SEG-004-T001's inherited direct-flow subset, two-SCC static lowering
  recipe, provenance/order rules, and pinned-oracle test matrix.
- [MC68000 word-division timing contract](m68k-word-division-timing-contract.md)
  records the MC68000-only `DIVU.W`/`DIVS.W` timing evidence, generated
  operand-derived accounting, and vector-5 timing boundary.
