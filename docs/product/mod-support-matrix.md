# Mod platform support matrix

[`mod-support-matrix.json`](mod-support-matrix.json) is the machine-readable record of **what modding support exists, per platform and capability, and on what test evidence**. It lives next to the
[compatibility ledger](compatibility-ledger.md) and is checked by the same gate (`python3 tools/check_product_truth.py`, run by the `product_truth_test` CTest case); there is no separate gate.
It is a *reporting* document: it never changes the launcher, the build admission rules or any runtime behavior, and contains no commercial-derived data.

The plan behind it is the Modding Platform contract set: [gap inventory](../architecture/modding-platform-gap-inventory.md), [adapters](../architecture/mod-platform-adapters.md),
[schema 3](../architecture/segapatch-schema-3.md), [phases](../architecture/mod-execution-phases.md), [ABI 2](../architecture/native-sdk-abi-2.md), [Luau policy](../architecture/luau-trust-policy.md) and
[audio](../architecture/modding-audio-architecture.md). **Nothing in those contracts is implemented by this page.** The only implemented mod capabilities are the pre-roadmap baseline: schema 1/2 Genesis packages and the read-only ABI 1 provider.

## Levels

| Level | Meaning |
| --- | --- |
| `supported-and-tested` | works and is covered by named tests on this platform; no known limitation that users must be told about |
| `supported-with-limitations` | works and is covered by named tests; the limitations are documented |
| `experimental` | partly works or is only design-backed; no support promise |
| `unsupported` | not available; a package that needs it is rejected |

`beta_target` is the level the public beta intends to reach; `spike-gated` marks function hooks, whose level depends on the named feasibility spike (GO, PARTIAL or NO-GO).

## Rules enforced by the gate

* Every platform x capability pair has exactly one row; levels and targets come from the closed lists above.
* **A row may claim `supported-and-tested` or `supported-with-limitations` only with cited test evidence**: `existing_tests` must list at least one existing test file, and `evidence` must resolve (a repository path or an ADR). A level is raised in the same change that adds the test.
* An `unsupported` row cites no passing test. Every row names its owning test: an existing test file, or the planned task id (`planned_owner`) that will create it.
* The table below is rendered from the JSON; the gate fails if it differs (`python3 tools/check_product_truth.py --render-matrix` prints the expected text).

## Public-beta test matrix

<!-- matrix:begin -->
| Capability | Platform | Current level | Public-beta target | Owning test |
| --- | --- | --- | --- | --- |
| Package identity and build | Genesis / Mega Drive | supported-with-limitations | supported-and-tested | `tests/segapatch_package_test.py`, `tests/segapatch_native_package_test.py` |
| Package identity and build | Master System | unsupported | supported-and-tested | planned: SEG-147-T001 |
| Package identity and build | Sega CD | unsupported | experimental | planned: SEG-148-T002 |
| Guest data/code modifications | Genesis / Mega Drive | supported-with-limitations | supported-and-tested | `tests/segapatch_package_test.py` |
| Guest data/code modifications | Master System | unsupported | supported-with-limitations | planned: SEG-147-T002 |
| Guest data/code modifications | Sega CD | unsupported | experimental | planned: SEG-148-T002 |
| Luau lifecycle and gameplay scripting | Genesis / Mega Drive | unsupported | supported-and-tested | planned: SEG-137-T006 |
| Luau lifecycle and gameplay scripting | Master System | unsupported | supported-with-limitations | planned: SEG-147-T004 |
| Luau lifecycle and gameplay scripting | Sega CD | unsupported | experimental | planned: SEG-148-T006 |
| Native C11 extension support | Genesis / Mega Drive | supported-with-limitations | supported-and-tested | `tests/genesis_patch_provider_build_test.py`, `tests/segapatch_native_package_test.py` |
| Native C11 extension support | Master System | experimental | supported-with-limitations | `tests/mod_sources_test.py` |
| Native C11 extension support | Sega CD | experimental | experimental | `tests/mod_sources_test.py` |
| Input modifications | Genesis / Mega Drive | unsupported | supported-and-tested | planned: SEG-138-T001 |
| Input modifications | Master System | unsupported | supported-with-limitations | planned: SEG-147-T004 |
| Input modifications | Sega CD | unsupported | experimental | planned: SEG-148-T005 |
| Memory access | Genesis / Mega Drive | supported-with-limitations | supported-and-tested | `tests/genesis_native_compat_viewer_sdl_test.c` |
| Memory access | Master System | unsupported | supported-with-limitations | planned: SEG-147-T004 |
| Memory access | Sega CD | unsupported | experimental | planned: SEG-148-T005 |
| Function hooks | Genesis / Mega Drive | unsupported | spike-gated (SEG-138-T003) | planned: SEG-138-T004 |
| Function hooks | Master System | unsupported | spike-gated (SEG-147-T003) | planned: SEG-147-T008 |
| Function hooks | Sega CD | unsupported | spike-gated (SEG-148-T004) | planned: SEG-148-T008 |
| Asset authoring | Genesis / Mega Drive | experimental | supported-and-tested | `tests/mod_sources_test.py`, `tests/mod_audio_test.py` |
| Asset authoring | Master System | unsupported | supported-with-limitations | planned: SEG-147-T007 |
| Asset authoring | Sega CD | unsupported | supported-with-limitations | planned: SEG-148-T002 |
| Graphics and HUD | Genesis / Mega Drive | supported-with-limitations | supported-and-tested | `tests/genesis_native_compat_viewer_sdl_test.c` |
| Graphics and HUD | Master System | unsupported | supported-with-limitations | planned: SEG-147-T006 |
| Graphics and HUD | Sega CD | unsupported | experimental | planned: SEG-148-T009 |
| Widescreen presentation (16:9 today; up to 32:9 planned) | Genesis / Mega Drive | supported-with-limitations | supported-with-limitations | `tests/genesis_extension_test.cpp` |
| Widescreen presentation (16:9 today; up to 32:9 planned) | Master System | experimental | unsupported | planned: SEG-147-T009 |
| Widescreen presentation (16:9 today; up to 32:9 planned) | Sega CD | experimental | unsupported | planned: SEG-148-T010 |
| Guest-native audio | Genesis / Mega Drive | unsupported | supported-and-tested | planned: SEG-145-T003 |
| Guest-native audio | Master System | unsupported | supported-with-limitations | planned: SEG-145-T004 |
| Guest-native audio | Sega CD | unsupported | experimental | planned: SEG-145-T006 |
| Enhanced audio | Genesis / Mega Drive | unsupported | unsupported | planned: SEG-145-T009 |
| Enhanced audio | Master System | unsupported | unsupported | planned: SEG-145-T009 |
| Enhanced audio | Sega CD | unsupported | unsupported | planned: SEG-145-T009 |
| Multiple-mod composition | Genesis / Mega Drive | unsupported | supported-with-limitations | planned: SEG-141-T006 |
| Multiple-mod composition | Master System | unsupported | supported-with-limitations | planned: SEG-141-T007 |
| Multiple-mod composition | Sega CD | unsupported | unsupported | planned: SEG-141-T008 |
| Save states / rewind with mods | Genesis / Mega Drive | experimental | supported-and-tested | planned: SEG-138-T006 |
| Save states / rewind with mods | Master System | unsupported | supported-with-limitations | planned: SEG-147-T005 |
| Save states / rewind with mods | Sega CD | unsupported | experimental | planned: SEG-148-T007 |
| Developer tooling | Genesis / Mega Drive | supported-with-limitations | supported-with-limitations | `tests/segapatch_package_test.py` |
| Developer tooling | Master System | unsupported | supported-with-limitations | planned: SEG-147-T010 |
| Developer tooling | Sega CD | unsupported | experimental | planned: SEG-148-T011 |
| Public docs and patch distribution | Genesis / Mega Drive | supported-with-limitations | supported-with-limitations | `tests/product_truth_test.py` |
| Public docs and patch distribution | Master System | unsupported | supported-with-limitations | planned: SEG-143-T007 |
| Public docs and patch distribution | Sega CD | unsupported | experimental | planned: SEG-143-T007 |
<!-- matrix:end -->
