# ADR 0074: YM2612 Implementation Selection, Independent Oracle and Toolchain Consequences

- Status: Accepted (SEG-032-T001); T007 implements it.
- Date: 2026-10-01
- Related: ADR 0072, `docs/architecture/genesis-z80-audio-contract.md` §9-11, `packaging/README.md`, `packaging/trim-zig.sh`.

## Candidates (evaluated at exact commits)

| candidate | commit | licence | provenance | YM2612 features | DAC | timers / status | clock interface | determinism | output | C vs C++ | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **ymfm** (A. Giles) | `81aec25ccbb98f4873a255f7551ac4dadac59b4a` | BSD-3-Clause | author-written, used by MAME and others | full YM2612 incl. SSG-EG, LFO, CSM/special mode, per-channel pan | DAC enable + 9-bit DAC data with the YM2612 DAC discontinuity (`ym2612::dac_discontinuity`) and 9-bit per-channel clipping | timer A/B via `ymfm_interface::set_timer` (host schedules), busy/status via `read()` | `sample_rate(input_clock) = clock/144`, one `generate()` per sample | integer only, no globals | `output_data{int32 data[2]}` per sample | C++ (`std::array`, `unique_ptr`, `string`, `vector`) | vendored sources `ymfm.h ymfm_fm.h ymfm_fm.ipp ymfm_opn.{h,cpp} ymfm_adpcm.{h,cpp} ymfm_ssg.{h,cpp}` |
| **Nuked-OPN2** (nukeykt) | `335747d78cb0abbc3b55b004e62dad9763140115` | LGPL-2.1 | die-shot derived, cycle-accurate | full YM2612 | exact | exact busy/timers | per-clock `OPN2_Clock` | deterministic | per-clock 9-bit | C | LGPL: linking into a distributed static executable adds relinking obligations; used as a **test-only oracle**, never linked or distributed |
| MAME FM cores | not pinned | per-file, not audited here | MAME's current YM2612 is ymfm-based | n/a | n/a | n/a | MAME device framework | n/a | n/a | C++ in MAME framework | not independent of ymfm; not evaluated further |
| Genesis Plus GX `ym2612.c` | `939ce4f0...` | non-commercial | MAME lineage | full | yes | yes | yes | yes | int | C | licence forbids production use; behavioural reference only |
| ares `ym2612` | `4cb8d92b...` | ISC | independent rewrite by the ares authors | full | yes | yes | yes | yes | int | C++ (nall) | tied to ares' node/scheduler; cross-check only |

## Decision

1. **Production: ymfm YM2612** behind a small C ABI in `libs/device/sega/ym2612` (T007), sources vendored at the pinned commit
   with the BSD-3-Clause text and a `THIRD-PARTY-NOTICES` entry. No Yamaha framework: only the `ym2612` class and the files it needs.
2. **Oracle: Nuked-OPN2** (independent implementation, different method, and the one source that is not the production code
   or its lineage). ares' YM2612 and GPGX are cross-checks. The production implementation never serves as its own oracle.
3. **Timers.** The host owns time: the C ABI converts ymfm timer requests into expiry timestamps in master ticks
   (one YM clock = 7 master ticks) and processes expiries and sample generation in one deterministic order up to the target time.

## Measurements (2026-10-01, Zig 0.15.2, `-O2 -fno-exceptions -fno-rtti -std=c++14`)

- The YM2612 part of ymfm is `ymfm_opn.cpp + ymfm_adpcm.cpp + ymfm_ssg.cpp` (the OPN translation unit contains all OPN variants).
  Compiling those three files plus a 15-line C ABI takes ~4-7 s on one core for each of x86_64-linux, aarch64-linux, x86_64-macos,
  aarch64-macos and x86_64-windows-gnu; object size 0.85-1.15 MB per target; a linked probe executable is ~108 KB.
- **No C++ runtime is needed at link time.** A 9-line C shim defines `operator new/delete` (plain, array, sized),
  `__cxa_pure_virtual` and `std::__1::__libcpp_verbose_abort`; the linked probe depends only on `libSystem`/libc (verified with
  clang on macOS and with `zig cc` for x86_64-linux). `-fno-exceptions -fno-rtti` is sufficient.
- **Toolchain cost:** compiling needs libc++ *headers*. `packaging/trim-zig.sh` removes `lib/libcxx`, `lib/libcxxabi`, `lib/libunwind`
  from the bundled Zig, so `zig c++` currently fails with `'cassert' file not found`. Keeping `lib/libcxx/include` (9.5 MB) and
  `lib/libcxxabi/include` (16 KB) and dropping everything else restores header-only C++ compilation; `lib/libcxx` sources, libunwind and
  libcxxabi sources stay removed (~2-3 MB compressed per package). T007 makes that packaging change and tests it.
- Reproducibility: two builds of the same sources produce byte-identical objects (checked by T007).

## Alternatives rejected

- Hand-writing FM synthesis (risk, no benefit). Linking Nuked-OPN2 (LGPL). Porting ymfm to C (maintenance fork of ~5 KLOC).
- Shipping prebuilt per-target ymfm objects instead of headers: workable fallback if the header packaging cost proves
  unacceptable; recorded as the fallback, not chosen (provenance of binaries is harder to audit).

## Consequences

- The distribution gains BSD-3-Clause third-party code and ~3 MB of libc++ headers; the final generated executable gains ~100-150 KB.
- A vendored-source determinism and licence test (T007) and the packaging smoke test cover both.

## T007 implementation record (2026-10-01)

- **Vendored ymfm** (BSD-3-Clause, pinned commit) lives in `libs/device/sega/ym2612/third_party` with its licence text; only the OPN, ADPCM
  and SSG translation units are built. A SHA-256 manifest (`tests/fixtures/ymfm-vendored-sha256.txt`) is checked by `genesis_ym2612_device_test`
  so the sources cannot drift silently. `THIRD-PARTY-NOTICES` and `packaging/README.md` list ymfm.
- **C ABI** in `libs/device/sega/ym2612/include`; `src/ym2612.cpp` implements the ymfm interface with host-owned busy (192 input clocks after a
  data write) and timers (A: `(1024 - NA) x 144`, B: `(256 - NB) x 2304` input clocks, ymfm's own scaling) in master ticks.
- **No C++ runtime at link time.** `src/cxx_runtime_shim.c` supplies `operator new/delete`, `__cxa_pure_virtual` and the libc++ verbose-abort
  hook; the generated program is linked as C. The C++ files are built with `-fno-exceptions -fno-rtti` by `tools/genesis_ym2612_build.py`
  (`zig c++` in the package, the host C++ compiler in development).
- **Oracle.** Nuked-OPN2 driven by `tests/genesis_oracle/opn2_nuked_driver.c` from `SEGARECOMP_GENESIS_ORACLE_CHECKOUT` (never linked into the
  product). Tone, FM-algorithm, chained-operator, LFO/AMS and SSG-EG streams are compared after gain and integer-lag alignment (Nuked emits a
  fixed 3-4 sample later): normalized RMS error <= 0.12 (documented tolerance: ymfm and Nuked differ in output rounding and envelope
  quantisation, not in algorithm); the DAC stream is held to <= 0.05 at lag 0. Busy and timer expiries are compared with the oracle within
  its polling step. This closes open fact U1 within that bound.
- **Packaging.** `packaging/trim-zig.sh` now keeps only `lib/libcxx/include` and `lib/libcxxabi/include` (libc++ headers); sources, tests,
  modules, libunwind stay removed. `tests/packaging_trim_zig_test.py` checks all three host families on a fake tree. The release-workflow
  package smoke test remains the real-toolchain check.
