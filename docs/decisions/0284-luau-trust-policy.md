# ADR 0284: Luau trust policy

- Status: accepted (SEG-133-T005, combined delivery F01). Normative text: [luau-trust-policy.md](../architecture/luau-trust-policy.md).
- Related: ADR 0267 (native trust), 0281, 0282, 0283.

## Decision

1. Trust classes: BPS-only (data), Luau-only, native; a mixed package is native. Approval: no code approval, informed consent to the exact identity, native approval to the exact identity.
2. Source only is packaged; bytecode comes only from the pinned trusted compiler at build; no bytecode is accepted or loaded at runtime; JIT is not built.
3. One VM per mod; a named library whitelist; no `os`, `io`, `debug`, `require`, dynamic loaders or coroutines in v1; deterministic stateless `math.random`; the VM is rebuilt on restore and its state is one serialized value.
4. Concrete limits: 4 MiB memory, 100000 budget units per callback, 400000 per frame, 200 call depth, a wall-time watchdog as a host-hang backstop only (a watchdog fault marks the session non-deterministic and fails determinism tests, never counts as success); packages may request less, never more.
5. Bindings are capability-scoped, copy-in/copy-out, never alias memory and are read-only during render.
6. Safety is claimed only for what adversarial tests demonstrate; documentation says "restricted", never "safe".

## Consequences

SEG-137 implements the VM against these rules; SEG-144-T003 turns each rule into a named test. Rejected alternatives: trusting Luau's sandbox alone; accepting package bytecode; allowing coroutines and `os.clock` (determinism and budget complications).
