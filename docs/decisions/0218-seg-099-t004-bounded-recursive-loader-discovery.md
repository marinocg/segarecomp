# ADR 0218: SEG-099-T004 - bounded recursive loader discovery

- Status: Accepted (pre-registration, then result and decision). Milestone SEG-099 (M3). Related: ADR 0214 (`complete` unreachable because loaders inside admitted modules were not analyzed), 0216, 0217.
- Privacy: project-authored fixtures; authentic measurements are counts and classes only.

## Question and pre-registered mechanism

Can the existing walk discover the loads performed *inside* a module it has already proven, without a second analyzer, a manual target list, execution, or walking data that merely decodes?

Option `recursive_modules` (`discover-modules --recursive-modules`; the build enables it; requires `work_state`). Reuse, not a new framework:

1. The existing entry proof is the only gate. When a transfer lands in a range the walk loaded on this path, the entry is recorded exactly as before **and**, if the bytes at the target are known to be the disc's (immutable disc content that a modelled CDCTRN delivered and nothing has since rewritten), the walk continues *into that module* with the live abstract state (registers, work-area memory, condition codes, return stack).
   The module's instructions are decoded from the abstract memory, so the decoded address is the actual guest address of that mapping: a module loaded at two bases is walked at each base separately, and a self-modified or in-place-decoded module simply stops being decodable.
2. A transfer into a loaded range whose bytes at the target are **not** known to be the disc's (decoded in place, overwritten, forgotten) is the typed refusal `loaded_bytes_unknown`; no entry and no source claim is made. (The previous behaviour in the non-model modes, proposing and relying on the run-time byte guard, is unchanged.)
3. Termination and bounds: the visited set (states are compared exactly, so a loading cycle reaches a fixed point), the existing state/step/call-depth bounds, at most 64 distinct proven module entries whose code is walked (`kLoaderScanMaxModuleEntries`; more is `scan_limit_exceeded`), the existing operation and module bounds. No image is analyzed unless a proven transfer enters it; sources are de-duplicated by (disc sector, destination); mappings by guest base.
4. Nothing is executed, no module list is an input, and no model prediction takes part.
5. Not covered: loaders of the main processor, loaders reached through a transfer the entry proof does not accept, transformations (decompression, relocation) - reported, never guessed.

| # | Case | Expected |
|---|---|---|
| 1 | boot SP loads A; A (on a route the build pass does not take) loads R | R proven only with recursion; the supported build admits it before any pass; the same binary runs R |
| 2 | A and B load each other (jump chain) | fixed point, each module once |
| 3 | module stored encoded and decoded in place before it runs | `loaded_bytes_unknown`, no module |
| 4 | two sources at one guest base; one source at two bases; data loaded and never executed | both kept (selected by live bytes); one module two mappings; `loaded_not_executed` |
| 5 | recursion on a disc with no nested loader | no module added |
| 6 | determinism, four exploration orders | identical modules |
| 7 | authentic Sonic CD, Earthworm Jim SE | measured |

Gate: `RECURSIVE_DISCOVERY_GO` = rows 1-6 hold through the supported build and launcher; `PARTIAL`; `BLOCKED`.

## Result

`tests/sega_cd_recursive_discovery_test.py` (24 checks): rows 1-6 hold. In the hermetic chain the build pass loads and runs A only; the scan proves R inside A; the build admits A, B and R statically (2 resolved extents: IP and SP); route 3 then runs A, which loads and runs R (never loaded or compiled by any build pass) from the unchanged binary; the control build without the static producer stops typed at R. A loading cycle terminates in 722 states with each module once.
Authentic: no proven module exists on either title, so no module entry is walked (0 entries, scan results unchanged: Sonic CD 13 states / 0 modules; Earthworm Jim SE 3,940 states / 0 modules).

## Decision: `RECURSIVE_DISCOVERY_GO` (mechanism); authentic benefit none

The mechanism is bounded, deterministic, reuses the M1/M2/T002 walk and entry proof, and finds the module-internal loads that make `complete` unreachable by construction. It has no authentic effect because no authentic module is proven to start from (ADR 0215/0216). Inventory completeness stays `partial`: the main processor's loaders are not analyzed (T005 names that class instead of the former `nested_loaders_not_analyzed`).
