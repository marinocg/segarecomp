# Release readiness (next release)

Status at SEG-129 (2026-11-07). **No tag has been created and nothing has been published.** Publishing is an operator action; this page is the checklist and the record of what was and was not verified.

* Latest published release: **v0.7.0** (2026-11-01), built from commit `e387909` (SEG-116). Archives: Windows x86_64 zip, Linux x86_64 tar.gz (glibc 2.35+), macOS arm64 zip, `SHA256SUMS.txt`.
* On `main` since then: SEG-117 to SEG-128 (widescreen, patch packages and SDK, lock-on recognition, RAM-thunk AOT, profiling, throughput, static-first Z80). The draft notes are in the [changelog](../../CHANGELOG.md).
* The release workflow derives the version from the tag (`vX.Y.Z`); no source version bump is needed.

## What the package contains and what was checked

| Item | Shipped | Verified by |
| --- | --- | --- |
| Launcher (`Segarecomp`) and CLI (`bin/segarecomp`) | yes | release workflow package smoke test on each host |
| Public native patch SDK header `runtime/platforms/genesis/sdk/segarecomp_genesis_patch.h` | yes (new since v0.7.0: not in the v0.7.0 archives) | smoke test requires it |
| SDK documents, synthetic sample provider and guide (`sdk/`) | yes (added by SEG-129) | smoke test requires them, checks the not-sandboxed statement, and **compiles the packaged sample with the bundled compiler against the packaged header** |
| Bundled C toolchain (pinned Zig 0.15.2), SDL3 3.4.16, Dear ImGui, libusb (Windows) | yes | existing SDL/dependency checks and smoke build with a scrubbed PATH |
| Third-party notices and licenses (`licenses/`) | yes | `dependency_check.py` and workflow license checks |
| Package user guide (`README.txt`) | yes, updated by SEG-129 for widescreen, patches, saves and the security warning | reviewed |
| Game-specific patch packages, ROMs, BIOS, discs, programs | **no** (never) | `dependency_check.py` refuses commercial and Python/ML content |

## Qualification gap (record for SEG-130)

* The package smoke test builds and runs a **synthetic** image on every host and, new in SEG-129, compiles the **synthetic SDK sample** with the bundled compiler on every host.
* A **complete patch-to-generated-native-executable build** (`genesis_patch_provider_build_test`) runs on **Linux only**. It has not been run on Windows x64 or macOS ARM64, so end-to-end patched builds are **not claimed** on those hosts.
* No physical-controller, real-display or HiDPI validation is claimed on any host. No commercial game was built from the release archives as part of SEG-129.
* The Linux package depends on glibc 2.35+; macOS and Windows packages are unsigned and not notarized.

## Operator publication checklist

1. Merge the SEG-129 pull request (operator decision) and let `main` CI finish green.
2. Run the release workflow by hand on `main` (**Actions > release > Run workflow**, a dry run that uploads artifacts only). Confirm all three package jobs, including the new SDK smoke step, pass.
   Download the artifacts, compare their names with the expected `segarecomp-<version>-<os>-<arch>` pattern and inspect one archive for `sdk/`, `licenses/` and `README.txt`.
3. Decide the version number. Move the "Unreleased" section of [CHANGELOG.md](../../CHANGELOG.md) under that version and date in a small follow-up commit, and update `latest_release` and `released_in` in the
   [ledger](compatibility-ledger.json) (features that shipped become `released_in: "<version>"`). Run `python3 tools/check_product_truth.py`.
4. Create and push the tag `vX.Y.Z` (this triggers build, smoke test and publication of archives plus `SHA256SUMS.txt`). The workflow's generic release text is short; replace it with the notes from the changelog
   (`gh release edit vX.Y.Z --notes-file <file>`), keeping the not-sandboxed warning and the "no ROMs included" line.
5. Verify the published archives against `SHA256SUMS.txt`, smoke-launch one build per host if possible, and record the result.
6. Do not describe the release as including game-specific widescreen packages; they are distributed separately if at all, and only after their own review.

## Not done in SEG-129

No tag, no release, no distribution of binaries, no signing or notarization, no new platform feature. See the qualification gap above.
