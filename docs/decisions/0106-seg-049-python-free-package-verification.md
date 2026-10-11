# ADR 0106: SEG-049-T007 release/package verification without Python or ML packages

- Status: accepted (checkpoint C5; the per-platform results are the release-workflow checks of the final product SHA, recorded in the harness evidence). No default or packaging redesign.
- Predecessors: ADR 0050 (consumer launcher + bundled C toolchain), ADR 0096 (vendored zlib, native v1), ADR 0102/0103/0105.

## 1. What is verified, on the extracted package of each platform (release workflow, pull-request runs included)

1. `packaging/dependency_check.py` (CI glue, not shipped):
   - **Native dynamic dependencies** of the launcher and of `segarecomp` (ELF `NEEDED` via `readelf`, Mach-O via `otool -L`, PE import table parsed in pure Python) are each packaged (found in the
     package) or an operating-system library; anything else, and anything matching the forbidden list (Python, libtorch/c10, NumPy, ONNX Runtime, TensorFlow, scikit-learn/SciPy, MKL/OpenBLAS/BLAS/LAPACK,
     CUDA runtimes, and a *dynamic zlib*), fails the job. The legitimate zlib contract is unchanged: zlib 1.3.1 deflate is vendored and **statically** linked into `segarecomp` (ADR 0096), so no
     `libz`/`zlib1.dll` dependency may appear.
   - **No forbidden file** anywhere in the package: no `.py/.pyc/.pyd`, interpreter, `site-packages`, ML framework file, `.onnx/.pt/.npz/.safetensors`, `*.model.json` or `segarecomp_ml_region*` research artifact.
   - **Embedded models**: the frozen gen2 model digest and the v1 feature-schema digest are present in the CLI binary, i.e. the models ship inside the executable, not as files.
2. `packaging/smoke_test.py` (existing synthetic Compatibility and Master System flows, scrubbed environment: empty `PATH`, decoy `cc/gcc/clang/zig`, throw-away `HOME`) now also drives the packaged launcher's
   headless mode for Compatibility, Optimized (v1) and Optimized + the experimental model on a 512-byte synthetic Genesis cartridge: each builds with the bundled compiler and runs to its sanitized stop, repeats as a
   cache **hit** with its own entry, and must report **exactly the development values**: effective producer, model family, validator `accepted`, `selected_ppm`/`admitted_ppm`, the admitted-set digest `k_sha256`
   (bit-identical native inference on every platform), the model/schema/token-contract/rank-plan identities equal to the frozen research record, and a `policy_identity` equal to an independent Python re-derivation.
   Because the same goldens must hold on Linux, macOS and Windows, a cross-platform numeric divergence of the native model fails the package job, not just the unit test.
3. `tests/packaging_dependency_check_test.py` (hermetic, in the normal test suite): the forbidden/allowed classification and the PE import parser on synthetic inputs, so the policy itself is tested.

## 2. Local Linux evidence (this checkpoint)

Linux x86-64 package staged with `cmake --install` (Release, SDL3 3.4.16 from the pinned source script) and exercised with the same scripts:

- Launcher: `libSDL3.so.0` (packaged), `libstdc++`, `libm`, `libgcc_s`, `libc` (system). CLI: `libstdc++`, `libm`, `libgcc_s`, `libc` (system). No other dependency; no `libz`.
- 4,944 files, no forbidden file; both model digests embedded in the CLI.
- Package smoke test: OK (the local run substituted a gcc shim for the bundled Zig, because the host's glibc is newer than the SDL3 build's Zig target; the *real* bundled Zig runs in the release workflow).

## 3. Limits

- Windows and macOS results exist only as workflow results of the final SHA; nothing in this ADR claims them in advance.
- The smoke image is a synthetic stop program; it proves packaging, identity and determinism, not game behaviour.
