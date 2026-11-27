# ADR 0322: ABI 1 deprecation, compatibility policy and removal gate

- Status: **proposed** (SEG-152-T012). It takes effect only when it is accepted and merged, which requires every deprecation condition below. Builds on [ADR 0267](0267-seg-124-native-widescreen-compatibility-provider.md), [ADR 0283](0283-native-sdk-abi-2.md) and [ADR 0320](0320-abi-2-exact-frame-presentation-and-parity-contract.md).
- Scope: documentation, SDK header comments, a synthetic ABI 2 sample and CI ordering. No behaviour change; no code removal; no ABI 3.

## Context

ABI 2 (minor 3) provides the exact-frame view, VDP helpers, bitmap overlay, non-fault refusal and the scenery extensions the production ports needed. Two native interfaces cost two sets of documentation and tests, and new work should use one of them. Published ABI 1 packages exist.

## Decision

1. ABI 1 is **deprecated, not removed**. The header and documents say so and point to the [migration guide](../product/native-abi-migration.md). There is no compiler attribute or warning.
2. The [compatibility policy](../product/native-abi-migration.md#compatibility-policy) applies: schema 2 / ABI 1 packages keep loading and running unchanged in all supported releases until a removal decision; new work uses schema 3 / ABI 2.
3. CI lists and runs ABI 2 parity suites (CTest label `abi2-parity`) ahead of ABI 1 compatibility suites (label `abi1-compat`). Both remain mandatory.

## Deprecation conditions

All must hold before this ADR is accepted. State when it was proposed:

| Condition | State when proposed |
| --- | --- |
| Product parity features merged and the ABI 1 / ABI 2 differential gate in public CI | met |
| Sonic 1, Sonic 2 (both revisions) and Sonic 3 & Knuckles ABI 2 ports accepted, with no unapproved difference from ABI 1 | **not met**: the port pull requests are not merged |
| Adaptive native-picture placement accepted for all three games (Sonic 2 per revision) | **not met**: Sonic 2 not started; Sonic 1 and Sonic 3 & Knuckles not merged |
| Migration path, compatibility policy, ABI 2 sample and CI order delivered | delivered by this change |
| Removal gate recorded | recorded below |

## Removal gate (recorded, not scheduled)

Removing any part of ABI 1 needs **all** of the following. No removal work is planned by this ADR.

| Criterion | State when proposed |
| --- | --- |
| At least one published release containing the ABI 2 replacements | unmet |
| No recommended catalog package with `native_abi = 1` | unmet |
| A published support window and migration notice, and the window has elapsed | unmet |
| An assessment of the impact on third-party packages | unmet |
| An explicit operator decision to remove | unmet |

## Consequences

Providers, packages, tests and the schema 2 reader are unchanged. Maintainers carry both interfaces until the gate is met and a separate decision is taken.
