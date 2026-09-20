# Contributing

By contributing you agree your work is licensed under the [Mozilla Public License 2.0](LICENSE).

Keep changes small, focused and reviewable, and state the behavior or acceptance criterion each change delivers. Tests must use synthetic,
homebrew, or otherwise redistributable inputs. Do not submit commercial ROMs, firmware, keys,
proprietary SDK files, or artifacts derived from them.

Local user-owned commercial images may be placed under the fully ignored `games/` directory for
exploratory testing only. They must never be force-added or required by CI. Genesis images commonly
use `.md`, so Markdown-looking files under `games/` are not documentation. See
`docs/testing/commercial-games.md`.

## Repository ownership

`libs/` reusable libraries; `platforms/` machine products; `apps/segarecomp` the CLI; `tests/` tests;
`tools/` developer tools; `docs/` architecture, ADRs, references, testing policy. CPU and device code is never duplicated
under a platform. See the README map.

## Build and test

Supported hosts: macOS, Linux, Windows (see README "Supported hosts"). Requirements: CMake >= 3.25,
Ninja, Python >= 3.10, a C/C++ toolchain.

```sh
cmake --workflow --preset dev-build   # configure + build
cmake --workflow --preset dev-fast    # fast tier
cmake --workflow --preset dev-full    # full hermetic tier
```

Tiers are CTest labels: `fast` (representative subset), `full` (all hermetic tests), `extended`
(Ghidra/environment-heavy). Run a single test with `ctest --preset dev -R <name>`. Format C/C++ with
`clang-format`.

## Pull requests

Use one focused branch and pull request per change; do not develop directly on `main`. CI (CMake/CTest
on macOS, Linux and Windows) must pass; it needs nothing beyond this repository.

## Hardware behavior and design

New hardware behavior needs a citation under `docs/references/`; consequential design changes need
an ADR under `docs/decisions/`.
