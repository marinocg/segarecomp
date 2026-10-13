# ADR 0119: SEG-057-T002 native D512HP producer, experimental Master System Optimized policy, typed `unadmitted_pc` runtime contract and launcher restart

- Status: Accepted. Delivers path **A** of the SEG-057 continuation: complete experimental SMS opt-in (native inference, product CLI integration, explicit launcher control, typed runtime fallback). It does **not** establish execution completeness; Optimized stays an explicit, experimental, execution-contingent choice and Compatibility stays the default.
- Related: ADR 0117 (the D512HP model, frozen history), ADR 0118 (the admission contract and economics; formal classification `Z80_SELECTIVE_ECONOMICS_INCONCLUSIVE`, engineering assessment favourable), ADR 0096/0103/0105 (the Genesis policy, lifecycle and launcher conventions this reuses), ADR 0080 (execution coverage observer).

## Context

ADR 0118 gave the emitter a sound, fail-closed, identity-bound way to consume an explicit set of Z80 starts and measured 30-37% full-build savings at the 65% budget. The one thing missing was a producer. The user asked for the existing recovered ensemble (three D512HP networks, 43,809 parameters each, weights SHA-256 `6bee7772...4f11`, artifact `81434de0...c2ac`) to become usable as an **experimental Optimized opt-in in the launcher**, provided it can fail closed and recover through broad Compatibility builds, without confusing availability with proven game completeness.

## Decision 1: the native producer (`ml_region_z80_producer`)

`platforms/master-system/machine`: a hand-written evaluator for exactly the one frozen architecture (not a framework): byte embedding (257 x 16 with the PAD token 256), local path (two 5-tap convolutions, ReLU), stride-4 context path (4-tap down-convolution, six residual dilated 5-tap layers 1,2,4,8,16,32), x4 upsample, 1x1 fuse over the concatenation, 1x1 head, three-seed mean of the byte logits, 32-byte cell score = mean of the two largest byte logits, structural root cells (ROM offsets `0x0000`, `0x0038`, `0x0066`) union the top `ceil(65% * cells)` cells, ranking by score descending then lower cell index, in integer arithmetic. Non-finite logits or scores fail closed.

- **Embedded, versioned model data.** The committed `tools/segarecomp_ml_region_z80_gen2.weights.sgz80w` is embedded at build time by a CMake script (no 3 MB generated file in the tree, no user-managed model file). At first use the binary verifies the artifact SHA-256, parses the strict `SGZ80W01` format (names, ranks, shapes, order, finite values, no trailing bytes) and recomputes the original ensemble digest; any deviation is the stable reason `model_identity`.
- **Numeric contract.** IEEE-754 binary32, a fixed accumulation order (bias, then taps ascending, then input channels ascending), each output channel an independent accumulator, no FMA contraction (`-ffp-contract=off`, MSVC `/fp:precise`), no fast-math. Logits are therefore bit-identical on every IEEE host and **independent of the thread count**: scored windows (4096 bytes, 512 bytes of context either side, which exceeds the 504-byte receptive radius) are independent, so threading changes nothing.
- Not bit-identical to PyTorch (different summation order): the gate against PyTorch is a registered tolerance plus exact selection equality, below.

## Decision 2: verification protocol (registered before any parity number was looked at)

Per byte logit `|native - reference| <= 1e-4 + 1e-4 * |reference|`; the retained cell set at the frozen 65% budget must be **exactly** equal; any difference is investigated, not tolerated. Reference: the unmodified research model (`build_model` + `infer`) loaded from the committed artifact, on public **synthetic** inputs (`tools/segarecomp_ml_region_z80_native_parity.py`, never run in CI; the committed fixture `tests/fixtures/sms-ml-z80/parity.smsmlp` holds inputs, reference logits and reference cell sets): seeded random bytes, an **impulse** (receptive-field, dilation and stride-4 probe), constant images, 37- and 65-byte images (mostly PAD), a size that is not a multiple of 4 or 32, and images crossing several window boundaries.

Results (`tests/sms_ml_region_z80_test.cpp`, hermetic, no ROM/PyTorch/Python): 19,859 logits over 7 cases, **worst absolute error 7.6e-6** (13x inside the tolerance); all 7 cell sets exactly equal. Also tested: identity digests; mutated, truncated, extended, bad-magic and empty artifacts fail closed; bit-identical logits across repeated runs, 1 vs 8 threads, central windows 64/1024/4096/20012 (full-image chunk consistency; non-multiple-of-4 sizes; ROM-edge PAD); tie-breaking by lower index; NaN/Inf fail closed; integer `ceil` budget; roots always retained. The clang build reproduces the same numbers.

Real-image parity (private, aggregate only): on the six development images the native logits agree with PyTorch within the registered tolerance (worst error 2.3e-5) and the retained sets are exactly equal (several cells tie exactly in score; both evaluators break ties by lower index, and the one non-tied cut has a margin of 3.9e-4, ten times the error).

## Decision 3: build integration and fallback

`segarecomp build --platform master-system --aot-policy optimized` runs the producer after the cartridge's executable images are projected and **before** emission, maps the retained ROM cells to image-offset admission sets through the platform's own mapping (fixed image identity 1, bank `n` identity `2 + n`; `rom_only` identity 1), binds them with `bind_admission` and passes them to the unchanged T001 emitter, which validates identity, size, digest and offsets. Pipeline: ROM -> validated executable images -> native D512HP -> 32-byte cells + roots -> image-offset sets -> `AdmissionPlan` validation -> existing Z80 C11 emitter. No interpreter, JIT, runtime decoding, ROM-specific heuristic, second emitter or implicit enablement.

- **Any** failure (`model_identity`, `producer_input`, `inference`, an emitter rejection `admission_rejected`) emits the ordinary broad program **byte for byte** (tested for every class with an injected proposer, including an emitter rejection that leaves nothing behind) and records `fallback: true`, the reason and a class in `status.json`. A fully built program is published by the launcher only after the whole build succeeded (it already builds beside the entry and renames).
- `--aot-model` is a Genesis option: for Master System it is refused (`MODEL_NOT_APPLICABLE`) before any generated C; Genesis `v1`/`gen2` are never Z80 model names, and Genesis behaviour and cache keys are unchanged (`aot-policy-identity` without `--platform` prints exactly what it printed).
- Machine-readable status (extends the existing `aot_policy` member minimally): requested/effective/fallback/reason/class, `model_family` `sms_z80_d512hp`, model and artifact digests, architecture, rank policy, schema, cells, selected cells, image offsets, admitted offsets, `selected_ppm`, `admitted_ppm`, `inference_ms`, `plan_sha256`, validator, policy and build identities.

## Decision 4: the runtime contract (the smallest robust typed signal)

A generated Master System program that stops on `Z80_ERROR_UNADMITTED_PC` prints `z80_outcome unadmitted_pc pc <hex4> identity <n>` and exits with the **dedicated status 7** (`SMS_EXIT_Z80_UNADMITTED_PC`), in both the headless driver and the SDL3 viewer (the viewer previously exposed neither the outcome nor a distinct status). Every other stop keeps its status (headless 0 / 2 cycle budget / 3 other Z80 outcome / 4 SMS error / 64 usage; viewer 0 / 3 other guest error / 4 SMS error / 5 host error / 64 usage), so a crash, a signal, any other guest or platform error, a host error and a user quit can never be mistaken for it, and no log is read. Verified on real generated programs for both drivers (status 7 and the structured line for an omitted start; 0 for an adequate plan, whose machine-state digest equals the broad run; a cycle-budget stop stays 2).

## Decision 5: the launcher (thin client)

- Availability is the **product CLI's** statement: `segarecomp aot-policy-identity --platform master-system --aot-policy optimized` succeeds (exit 3 when the embedded model does not verify). The launcher probes it once on a worker and never offers Master System Optimized until the product says yes; a request it cannot honour is reported and built as Compatibility. Nothing about the model, thresholds, schema or admission lives in the launcher.
- The existing Mode control (Compatibility | Optimized) is reused; for Master System Optimized it adds a two-line explanation ("Experimental CNN compilation. May restart in Compatibility if omitted code is reached."). The Genesis-only Experimental model button is neither shown for nor forwarded to Master System. A Genesis choice is never inherited by a Master System ROM: Compatibility is the Master System default.
- Requested and effective policy, the fallback reason and class, the model and fractions are shown through the existing `policy_summary` / "Why?" details from `status.json`; the Running label always says Optimized (experimental), Compatibility, or Compatibility (restarted).
- **Restart contract.** Only an Optimized Master System program that exits with exactly status 7 triggers it: the user sees a notice ("Optimized stopped: the game reached code outside the selected set. Restarting from the beginning in Compatibility."), the mode switches to Compatibility, a matching Compatibility entry is reused or built, and the program is started again **from reset** (a new process; no state is carried over and none is implied). It happens at most once per Play (one decision function, shared by the window and the headless `--run`, returns no restart for a second request); a failing Compatibility build is shown with its real failure and is never retried; every other end (nonzero status, crash, quit) is an ordinary failure or return.
- **Cache identity.** The Master System key adds `aot=optimized:pid=<product policy identity>`, so Compatibility and Optimized never share an entry, an Optimized entry is reusable only when the current product identity equals the recorded one (fail closed when the tool cannot answer), and a changed model, rank policy, schema or admission contract selects a fresh entry (including in packaged releases, whose CLI binary also keys the entry). The Genesis key text is unchanged. The per-build identity binds image, request, effective producer and the admitted-set digest.

## Decision 6: packaging

The model is embedded in the CLI; the package contains no weight file, Python or ML framework (`packaging/dependency_check.py` now also requires both SMS digests in the binary). `packaging/smoke_test.py` builds a synthetic cartridge through the **packaged** launcher and CLI, checks cache separation, the identities against an independent Python re-derivation and a **golden admitted-set digest**, so any cross-platform divergence of the native model fails the package job.

## Evidence (private inputs aggregate-only; development images only; no sealed image used)

| Measurement | Result |
| --- | --- |
| Native inference, Release, 1 thread | 0.89 s (128 KiB), 1.96 s (256 KiB), 3.6 s (512 KiB); user CPU equal |
| Native inference, 8 threads | 0.15-0.18 s (128 KiB), 0.30 s (256 KiB), 0.60-0.63 s (512 KiB); user CPU 1.2 / 2.5 / 4.8-5.1 s |
| Model initialization (verify + layout) | 3.5-5.6 ms |
| Peak memory | 7-9 MiB (1 thread), 18-20 MiB (8 threads) |
| PyTorch CPU proxy (T001, one thread) | 0.42 s (128 KiB), 1.17 s (512 KiB): the native evaluator is 2-3x slower single-threaded and faster with threads; the native producer's 0.6 s wall (3.7 s single-thread CPU) at 512 KiB is under 1% of the Optimized build's wall (about 0.5% of its CPU) |
| Product build wall, `-O2`, 8 jobs, real CLI, including inference | 128 KiB: 31.6 / 29.8 s Compatibility, 22.3 / 21.0 s Optimized (-29% / -30%); 512 KiB: 147.6 / 144.3 s vs 92.4 / 93.1 s (-37% / -35%); CPU -31% (128 KiB), -35% to -37% (512 KiB) |
| Execution probe, 5 buildable development images, 600 and 6,000 frames, no input | all 5 Optimized builds run to the frame limit with a machine-state digest **identical** to Compatibility (Solomon's Key fails to build in either mode, a pre-existing limitation) |
| Unit and hermetic tests | producer/parity/determinism, build fallbacks (5 classes byte-identical to broad), CLI status and identities, real-program exit 7 (headless and SDL3 viewer), launcher gating/cache/decision, headless and **window** end-to-end restart flows (8 scenarios, mutation-checked) |

The inference cost (0.6 s wall for the largest SMS image, 3.7 s CPU) is about 1% of the 55 s of build wall time that Optimized saves; the whole-product saving (29-37%) matches the T001 economics.

## What this does NOT prove

- It does not show that the CNN covers arbitrary execution paths, banking, interrupts or input branches of full games. The six-image probes ran idle/title code (a 600-frame trace touches 0.2-7% of a ROM) and are **not independent coverage evidence**; the SEG-056 sealed games are spent and T001's trace-derived plans are not blind evidence either. The 65% budget is a build-time heuristic; a start it omits stops the program with the typed outcome and the launcher restarts in Compatibility. That is a **recoverable, execution-contingent experimental mode**, not a proof of globally sound selective admission, and a player's progress is lost on restart.
- It does not change the default (Compatibility), the Genesis M68K policy or any Genesis cache key.
- Unverified locally: Windows and macOS runs of the new tests (the CI matrix exercises the producer parity, the launcher tests on the SDL3 job, the viewer test on Linux and the package smoke test on every platform); the window end-to-end test runs on Linux with SDL's dummy video driver. A pre-existing clang + libstdc++ 13 incompatibility in `genesis/frontend.cpp` prevents a local all-targets clang build; every file touched here builds cleanly under clang with warnings as errors.

## What is still required before promoting Optimized beyond an explicit experimental choice

A separate evidence milestone: execution coverage across **independent playthroughs** (banking, interrupts, input branches), a fresh blind source-backed corpus, and a decision on whether a default or a persisted per-ROM "Optimized stopped here" memory is warranted. No new training or model search is needed for the experimental opt-in.
