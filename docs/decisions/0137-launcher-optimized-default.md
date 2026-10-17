# ADR 0137: The launcher defaults to Optimized (Genesis gen2, Master System where offered)

- Status: accepted (SEG-069-T002).
- Context: the launcher opened every game in Compatibility; Genesis Optimized needed the separate *Experimental model* checkbox (gen2), and Master System
  Optimized was an opt-in tile. Maintainer testing recommends Optimized (Genesis + gen2, Master System) as the first thing to try.
- Decision:
  1. A newly imported launcher library entry stores `aot_policy = optimized` for both platforms. This is a launcher user-experience default only:
     `segarecomp build` keeps `--aot-policy compatibility`, the lower-level `--aot-model v1|gen2` options remain for reproducible CLI/headless use,
     and the SEG-049 `OPT-IN RETAINED` classification of gen2 for the product CLI (ADR 0109) is unchanged.
  2. Genesis Optimized is always requested as gen2 by the launcher (`--aot-model gen2`); the Experimental checkbox is removed. The cache identity of a new
     Genesis Optimized build equals the former Optimized + Experimental one, so an existing gen2 build stays reusable. A legacy Optimized/v1 selection is
     migrated to gen2 on load; its v1 build has a different identity and is shown as needing a rebuild, never reused.
  3. Master System keeps its own product policy and the product's capability statement (`aot-policy-identity --platform master-system`). The stored
     intent is Optimized; availability is resolved separately and never rewrites it. While the probe is pending a build waits for the answer (it is not
     started as Compatibility); where the product does not offer it, the game builds as Compatibility (no `--aot-policy`), the card says so, and the
     Genesis model option is never sent.
  4. Explicit Compatibility selections of existing libraries are preserved. A catalog record with no stored selection takes the default.
- Consequences: the Optimized fallback and restart contracts (ADR 0096, 0119) are unchanged and matter more because they now apply to default builds.
