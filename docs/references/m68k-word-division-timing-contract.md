# MC68000 DIVU.W/DIVS.W timing contract

## Evidence

**Primary source:** NXP, *MC68000 User Manual*, `MC68000UM`, instruction
timing table 8-4 and exception timing table 8-14, accessed 2026-09-19:
<https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf>.

For the MC68000 (the Genesis main CPU), table 8-4 gives maximum register-EA
times of 140 clocks for `DIVU.W` and 158 clocks for `DIVS.W`, and requires the
word effective-address calculation time to be added. Table 8-14 assigns
divide-by-zero exception processing 38 clocks plus the word EA time. These figures do **not** apply
to later M680x0 variants.

## Selected behavior

`DIVU.W` and `DIVS.W` retire at table 8-4's maximum policy: respectively 140
or 158 clocks plus table 8-1's word source-EA cell. This is a literal public
manual policy, so generated C has no dividend/divisor timing helper and does
not materialize either operand for timing.

Divide by zero transfers synchronously to vector 5 rather than retiring as a
normal DIV. Table 8-14's 38 plus word-EA clocks belong at the exception owner;
that accounting is out of scope here, so this contract does not charge normal
DIV time on the exception path.

No mapper, peripheral, bus-wait-state, self-modifying-code, or later-M680x0
timing behavior is selected by this contract.
