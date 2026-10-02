# ymfm (vendored subset)

- Upstream: https://github.com/aaronsgiles/ymfm (Aaron Giles), BSD-3-Clause (see `LICENSE`, unmodified).
- Commit: `81aec25ccbb98f4873a255f7551ac4dadac59b4a`.
- Files: `ymfm.h ymfm_fm.h ymfm_fm.ipp ymfm_opn.h ymfm_opn.cpp ymfm_adpcm.h ymfm_adpcm.cpp ymfm_ssg.h ymfm_ssg.cpp` copied byte for byte
  from `src/` (the OPN translation unit contains every OPN variant; the YM2612 is the `ymfm::ym2612` class). No file is modified; the
  header-only parts of the library are not used by SEG-032.
- Why: ADR 0074 (Genesis YM2612 selection). Compiled with `-fno-exceptions -fno-rtti`; the C++ runtime library is not needed at link time
  (the shim `../../src/cxx_runtime_shim.c` replaces `operator new/delete` for the bundled-toolchain link).
- Verification: `tests/genesis_ym2612_device_test.py` checks the vendored files against the SHA-256 list below and (when the pinned checkout
  is available) against upstream.

```
SHA256SUMS are recorded in `tests/fixtures/ymfm-vendored-sha256.txt`.
```
