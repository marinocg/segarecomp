# ADR 0311: an optional anchor for a function-hook site

- Status: accepted (W04). Extends ADR 0298/0308. No game-specific behavior.

## Context

A hook site must be a function entry reached by a direct call in code the program statically reaches from the reset entry (ADR 0298). A real game's main loop is entered through computed dispatch (state tables), so most of its code is not statically reachable from reset: every site inside it is rejected `E_HOOK_COMPUTED_TARGET` ("no direct call reaches the site and the program has computed transfers"), although the site has plain `JSR/BSR` callers in compiled code. Evidence (private, aggregate): the sprite-piece clip routine of a commercial title, its caller and its caller's caller were all rejected for that reason, so no hook could observe the sprite builder.

## Decision

1. A `[[hook]]` may name an optional `anchor`: a **verified** symbol of every applicable revision (resolved exactly like `symbol`, `E_SYMBOL_*` otherwise). The build adds the anchor address to the reachability roots of the classification and writes an `anchor <site id> <hex8>` line after the site's line of the build-private plan (parsed fail-closed: exactly three fields, an earlier site, at most one).
2. **An anchor is evidence for its own site only.** The classifier keeps two kinds of evidence apart:
   - *Per-site admission evidence* = the original reset roots plus **only that site's own anchor** (when it is compiled code). It alone decides the site's direct callers and continuations, whether the site is also reached by a branch, a fall-through, a return or as a root (`E_HOOK_NOT_FUNCTION_ENTRY`), and whether the program has computed transfers (`E_HOOK_COMPUTED_TARGET`). A site without an anchor is therefore classified exactly as before, whatever other sites declare; one site's anchor never makes another site classifiable and never turns another site into a "non-call entry" because the two addresses coincide.
   - *Global integrity evidence* = the reset roots plus every compiled anchor (each is asserted executed), the vector reachability and the replaced-function extents. It is used only for cross-site safety judgements: `E_HOOK_MID_INSTRUCTION` (a site inside an instruction of code asserted executed), `E_HOOK_INTERRUPT_CONTEXT` (vector reachability, independent of anchors) and `E_HOOK_OVERLAP`. It can only turn an acceptance into a stable rejection; it never authorizes a site.
   An anchor that is not compiled code is ignored; an anchor that does not statically reach the site changes nothing (the class stays `E_HOOK_COMPUTED_TARGET`). Every other rule (not a vector target or reachable from an interrupt handler, single call entry, classified returns, mutable code, mid-instruction) is unchanged.
3. Soundness is unchanged: the author asserts that the anchor function is executed; the runtime still proves every call by its pushed return address (a call from an unproven caller is skipped and counted, never altered). A wrong anchor can therefore only make a site classifiable that never fires, never make a hook fire wrongly.
4. The anchor is part of the package identity text and the content digest only when declared (existing identities are unchanged) and of the hook table text.

## Consequences

- One optional manifest key, one optional plan line, one extra root per site in the classifier (per-site admission evidence) and one union list for the global safety checks. No new hook mode, phase, capability or ABI.
- Tests: `tests/genesis_function_hook_reject_test.py` (a site called only from an unreachable function: rejected without an anchor, accepted with the caller as anchor, unchanged for an anchor that does not reach it or is not code; two sites sharing one unreachable caller: only the anchored site is accepted, a wrong, uncompiled or foreign anchor authorizes no other site, an anchor equal to another site's entry does not change that site's class, and the global mid-instruction check still refuses a site inside code another site's anchor asserts executed), `tests/mod_hooks_resolve_test.cpp` (resolution per revision, hint and unknown symbols refused, table text).
