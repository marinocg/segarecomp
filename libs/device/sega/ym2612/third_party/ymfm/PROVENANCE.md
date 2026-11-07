# ymfm (vendored subset)

- Upstream: https://github.com/aaronsgiles/ymfm (Aaron Giles), BSD-3-Clause (see `LICENSE`, unmodified).
- Commit: `81aec25ccbb98f4873a255f7551ac4dadac59b4a`.
- Files: `ymfm.h ymfm_fm.h ymfm_fm.ipp ymfm_opn.h ymfm_opn.cpp ymfm_adpcm.h ymfm_adpcm.cpp ymfm_ssg.h ymfm_ssg.cpp` copied byte for byte
  from `src/` (the OPN translation unit contains every OPN variant; the YM2612 is the `ymfm::ym2612` class). **Two local bounds checks (SEG-131, ADR 0278) in the state-restore path only**, so a crafted save state cannot index tables out of range: `ymfm.h` `ymfm_saved_state::restore(envelope_state&)` coerces a byte outside EG_ATTACK..EG_RELEASE to EG_RELEASE, and `ymfm_opn.cpp` `ym2612::save_restore` clamps a restored register address to 0x1ff. Nothing else differs from the commit above (a comparison with the upstream checkout will show exactly these two hunks); the
  header-only parts of the library are not used by SEG-032.
- Why: ADR 0074 (Genesis YM2612 selection). Compiled with `-fno-exceptions -fno-rtti`; the C++ runtime library is not needed at link time
  (the shim `../../src/cxx_runtime_shim.c` replaces `operator new/delete` for the bundled-toolchain link).
- Verification: `tests/genesis_ym2612_device_test.py` checks the vendored files against the SHA-256 list below and (when the pinned checkout
  is available) against upstream.

```
SHA256SUMS are recorded in `tests/fixtures/ymfm-vendored-sha256.txt`.
```
