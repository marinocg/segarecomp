# ADR 0311: an optional anchor for a function-hook site

- Status: accepted (W04). Extends ADR 0298/0308. No game-specific behavior.

## Context

A hook site must be a function entry reached by a direct call in code the program statically reaches from the reset entry (ADR 0298). A real game's main loop is entered through computed dispatch (state tables), so most of its code is not statically reachable from reset: every site inside it is rejected `E_HOOK_COMPUTED_TARGET` ("no direct call reaches the site and the program has computed transfers"), although the site has plain `JSR/BSR` callers in compiled code. Evidence (private, aggregate): the sprite-piece clip routine of a commercial title, its caller and its caller's caller were all rejected for that reason, so no hook could observe the sprite builder.

## Decision

1. A `[[hook]]` may name an optional `anchor`: a **verified** symbol of every applicable revision (resolved exactly like `symbol`, `E_SYMBOL_*` otherwise). The build adds the anchor address to the reachability roots of the classification and writes an `anchor <site id> <hex8>` line after the site's line of the build-private plan (parsed fail-closed: exactly three fields, an earlier site, at most one).
2. An anchor that is not compiled code is ignored; an anchor that does not statically reach the site changes nothing (the class stays `E_HOOK_COMPUTED_TARGET`). Every other rule (not a vector target or reachable from an interrupt handler, single call entry, classified returns, mutable code, mid-instruction) is unchanged.
3. Soundness is unchanged: the author asserts that the anchor function is executed; the runtime still proves every call by its pushed return address (a call from an unproven caller is skipped and counted, never altered). A wrong anchor can therefore only make a site classifiable that never fires, never make a hook fire wrongly.
4. The anchor is part of the package identity text and the content digest only when declared (existing identities are unchanged) and of the hook table text.

## Consequences

- One optional manifest key, one optional plan line, one extra root list in the classifier. No new hook mode, phase, capability or ABI.
- Tests: `tests/genesis_function_hook_reject_test.py` (a site called only from an unreachable function: rejected without an anchor, accepted with the caller as anchor, unchanged for an anchor that does not reach it or is not code), `tests/mod_hooks_resolve_test.cpp` (resolution per revision, hint and unknown symbols refused, table text).
