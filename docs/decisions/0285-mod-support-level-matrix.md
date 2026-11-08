# ADR 0285: Mod support-level matrix in the product-truth gate

- Status: accepted (SEG-146-T005, combined delivery F01). Page: [mod-support-matrix.md](../product/mod-support-matrix.md); data: `docs/product/mod-support-matrix.json`; gate: `tools/check_product_truth.py`.

## Decision

1. The per-platform, per-capability support level is a machine-readable JSON file beside the compatibility ledger, checked by the existing product-truth gate (no new gate, no new framework).
2. Levels: `supported-and-tested`, `supported-with-limitations`, `experimental`, `unsupported`; targets add `spike-gated` for function hooks.
3. A row may claim a `supported-*` level only with at least one existing test file and resolvable evidence; an `unsupported` row cites no test; every row names an owning test (existing file or planned task id).
4. The public-beta test matrix is rendered from the JSON into the documentation page, and the gate fails if the page differs.
5. The ledger's own schema is untouched.

## Consequences

Raising a level and adding its test happen in the same change. The initial rows come from the gap inventory and the audio capability matrix and are deliberately conservative: only the pre-roadmap Genesis baseline is above `unsupported`.
Rejected alternatives: extending the ledger schema (it describes games and packages, not capabilities); a separate gate (extra maintenance); prose-only tables (cannot be enforced).
