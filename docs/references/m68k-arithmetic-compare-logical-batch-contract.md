# MC68000 arithmetic, compare, and logical batch contract

## B1: compare foundation

Source: *M68000PM/AD Rev. 1*, §2 and §4, `CMP`, `CMPI`, and `CMPA` instruction entries.

This checkpoint selects only `CMP.B/W/L`, `CMPI.B/W/L`, and `CMPA.W/L` in the existing
`general_startup` decode profile.  It uses the existing typed EA decoder, including its immediate
EA extension parsing, provenance, and bounds checks.  No arithmetic or logical instruction is
selected by this contract.

Each selected operation has an explicit typed compare instruction/IR identity (`cmp`, `cmpi`, or
`cmpa`; and `compare`, `compare_immediate`, or `compare_address` after lifting). TST retains its
separate identity. Each selected operation reads source and destination without writing either, computes
`destination - source` at the selected width, updates N, Z, V, and C from that subtraction, and
preserves X. `CMPA.W` sign-extends its word source before a 32-bit comparison with An; `CMPA.L`
compares 32-bit values. Generated C consumes the shared lifted operation and existing EA lowering;
it contains no target-byte fetch or decode. CMPI destination reads use the existing general static
operand resolver with read direction; CMP/CMPA register destinations do not request memory resolution.

## B1 synthetic pinned-Musashi differential

`tests/m68k_batch_b_musashi_differential_test.py` uses project-authored synthetic vectors for
`CMP.B/W/L`, `CMPI.B/W/L`, and `CMPA.W/L`. It compares generated C with one Musashi instruction for
PC, full SR, all D/A registers, and zero writes; the CMPI memory-destination vector also requires an
oracle RAM read. Generated C is compiled with strict C11 first.

Set `SEGARECOMP_M68K_BATCH_B_MUSASHI_CHECKOUT` to an unmodified local Musashi checkout at
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, then run:

```sh
python3 tests/m68k_batch_b_musashi_differential_test.py \
  build/dev/m68k_batch_b_test_harness "$(xcrun --find cc)"
```

The test builds the committed runner and generated opcode sources in a temporary directory. Without
the environment variable it reports unavailable and makes no differential claim; a supplied missing,
modified, or revision-mismatched checkout fails.

## B2: subtraction

Source: *M68000PM/AD Rev. 1*, §4, `SUB`, `SUBA`, `SUBI`, and `SUBQ` entries.

`general_startup` additionally selects only `SUB <ea>,Dn` opmodes 000/001/010,
`SUB Dn,<ea>` opmodes 100/101/110 with data-alterable destinations, `SUBA.W/L`
opmodes 011/111, `SUBI.B/W/L`, and `SUBQ.B/W/L`.  Byte `SUB` from An and byte
`SUBQ` to An reject; `SUBQ`'s encoded zero count is eight.  `SUBA.W` sign-extends
its source, and both SUBA and SUBQ-to-An leave SR unchanged.  Other selected
subtractions use the shared sized subtraction result and set X from borrow.

Memory destinations are read then written through the existing shared resolver
and C EA lowering.  Emission binds the RMW address once: predecrement and
postincrement mutate An once, not once per read/write.

The existing optional pinned-Musashi B-batch differential reuses its sole
runner and harness for B2 vectors.  It compares full D/A state, PC, SR, and
every seeded RAM long after execution; B2 memory-destination vectors also
require the oracle to report a write.  The project-authored vector set covers
SUB byte/word/long (including memory destination), SUBA word/long, SUBI
byte/word/long (including memory destination), and SUBQ byte/word/long,
encoded quick-eight, and address-register word/long forms.

## B4: ordinary logical operations

Source: *M68000PM/AD Rev. 1*, §4, `AND`, `ANDI`, `OR`, `ORI`, `EOR`, and
`EORI` entries. `general_startup` selects only B/W/L ordinary data forms.
AND/OR EA-to-Dn sources use data EAs excluding An; their reverse Dn-to-EA
forms use memory-alterable destinations only. EOR is Dn-to-data-alterable
only. Ordinary immediate forms use data-alterable destinations. Exact
`003C`/`007C`, `023C`/`027C`, and `0A3C`/`0A7C` CCR/SR primaries reject before
EA parsing and remain outside this batch.

Logical results are sized B/W/L, preserve X and upper SR, set N/Z from the
sized result, and clear V/C. Memory destinations use the existing shared
source materialization and read-modify-write lowering, including one
predecrement/postincrement update.
