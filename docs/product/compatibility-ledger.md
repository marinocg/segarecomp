# Compatibility ledger

[`compatibility-ledger.json`](compatibility-ledger.json) is the project's one public, versioned record of **which platforms exist, which games were actually tried, how well, and on what evidence**.
It is a *reporting* document. It does not change the launcher's capability detection, the build admission rules or any runtime behavior, and it contains no ROM, BIOS, screenshot, trace or
other commercial-derived data.

`python3 tools/check_product_truth.py` validates it (the `product_truth_test` CTest case runs the same gate). The gate uses only public files of this repository and needs no network.

## Two separate dimensions

The ledger keeps **how far a game got** apart from **how good the evidence for that is**. They are different questions and one never implies the other.

| `execution_level` | Meaning |
| --- | --- |
| `unknown` | nothing is claimed |
| `boot` | the program starts and runs its start-up |
| `title` | a title screen is reached |
| `gameplay` | gameplay is reached |
| `playable` | the game can be played for a meaningful stretch |
| `completed` | the game was played from start to finish |

| Evidence `category` | Meaning | Highest level it supports alone |
| --- | --- | --- |
| `synthetic_implementation_tests` | project-authored tests with no commercial software | `unknown` for a game |
| `reference_comparison` | checkpoints compared with an independent reference | `gameplay` |
| `automated_gameplay_route` | a deterministic scripted route on a commercial game | `gameplay` |
| `maintainer_reported_playthrough` | a human playthrough reported by the maintainer; no independent replay | `completed` |
| `independently_reproduced_completion` | completed again by someone else, or by a recorded and replayed run | `completed` |
| `untested` | nothing known | `unknown` |

A human completion is therefore recorded as `maintainer_reported_playthrough`. It never silently becomes an automated or independently validated run: the gate rejects `playable` or `completed`
resting only on automated checkpoints, `completed` without a human report, and a human completion with an unknown build mode that does not say which conditions are unknown.

## Sections

* `platforms[]`: `id`, `name`, `status` (`implemented` | `experimental` | `planned`), `cpu_mode`, `capabilities`, `limitations`, `evidence`. A planned platform lists no capability and has `cpu_mode` `none`.
* `games[]`: `id`, `platform`, `title`, `edition` (or `null` when unknown), `execution_level`, `evidence[]` (`category`, `scope`, `refs`), `build_mode` (`compatibility` | `optimized` | `file_first_structural` | `unknown`),
  `input` (`original`; a patched run is a `patch_packages` entry), `blockers`, `untested`, and `unknown_conditions` for human reports whose conditions were not recorded.
* `patch_packages[]`: game-specific `.segapatch` packages. Their `distribution` is `private_harness_only` unless a package is actually attached to a release; the gate refuses `release` until a release asset is recorded.
  Each entry states what it provides, whether it changes gameplay, the validated scope and what was not tested.
* `features[]`: optional product features with `status` (`implemented` | `partial` | `experimental`), `released_in` (the latest published release, or `null` when the feature exists only on `main`) and `available_on_main`.

`evidence.refs` are repository paths (checked to exist), `ADR NNNN` (the decision file must exist) or `PR #N`. A broken reference fails the gate.

## Updating it

1. Change the JSON together with the prose that makes the same claim (README, platform pages). `check_product_truth.py` checks that they agree on the facts listed in its header.
2. When a release is published, set `latest_release` and move the features it contains to `released_in`.
3. Do not raise an `execution_level` without evidence in the right category. Do not describe a human report as an automated run.

The ledger records what the evidence supports today; it is not a promise about any title that is not listed.
