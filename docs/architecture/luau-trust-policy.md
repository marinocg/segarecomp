# Luau security and trust policy

Status: accepted contract (SEG-133-T005, ADR [0284](../decisions/0284-luau-trust-policy.md)). **Implemented in part**: the pinned build, the trusted compile step and the restricted VM lifecycle (SEG-137-T001..T003, ADR [0294](../decisions/0294-luau-vendoring-vm-and-trusted-compile.md)); the capability-gated `sega.*` read-only API, the static check, the state seam, the fixture session and the adversarial test corpus (SEG-137-T004..T007, ADR [0295](../decisions/0295-luau-host-api-static-check-and-fixture-session.md)). The minimal live seam (a Luau-only package linked into the played Genesis program, `render(frame)` and the HUD overlay only; SEG-137-T007, ADR [0296](../decisions/0296-luau-only-mod-executable-integration.md)) is implemented; guest-memory services, the shared phase scheduler and the `MODSTATE` container are M05/M07; the independent adversarial review is SEG-144-T003. Inputs: [schema 3](segapatch-schema-3.md), [ABI 2 capability catalogue](native-sdk-abi-2.md), [phases](mod-execution-phases.md).
Consumers: SEG-137-T001..T007, SEG-138-T005, SEG-144-T003 (adversarial testing), the launcher approval text (SEG-143, SEG-135).

**Safety is determined by bindings, configuration and adversarial tests, not by language sandboxing alone.** Luau is a language designed for embedding with a restricted standard library; that reduces attack surface and does not by itself make a script safe.
Every statement below is a rule with a named test category (section 8); a rule without a passing test is not promised to users.

## 1. Trust classes

| Class | Payloads | What it can do | User approval |
|---|---|---|---|
| **BPS-only** (`data`) | `bps-image`, `rom-edit-set`, `guest-code`, `asset` | changes the guest image and assets through verified build tooling; no host code runs | no code approval; exact-identity and image-digest verification; still **untrusted input** to the compiler/runtime parsers |
| **Luau-only** | adds `luau-source` | runs scripts in the restricted VM through granted capabilities | **informed consent** to the exact package identity (the install sheet states that scripts will run and what capabilities they declared); this is *not* the native-code approval |
| **native** | any `native-c11` | arbitrary native code compiled into the program; **not sandboxed** | explicit **native-code approval** of the exact package identity (the existing `trust_native` model, ADR 0267) |
| **mixed** | native + Luau and/or data | same as native | **native** approval: a mixed package is classified native |

Approval never transfers between identities; a changed package needs new consent/approval. A faulted mod (phases section 6) is disabled for the session regardless of class.

## 2. Compiler ownership

1. Packages carry Luau **source** only. Bytecode is produced only by the pinned trusted compiler (version and build pinned by SEG-137-T001) **at build time**, in a bounded compile step.
2. A package never carries executable bytecode and the runtime never loads bytecode from a package. A `luau-source` payload whose bytes are not text (any byte below 0x20 other than TAB, LF, CR, or any byte >= 0x7F, or a bytecode header) is `E_LUAU_BYTECODE`, rejected at `mod check` and at build.
3. Luau native code generation (JIT) is disabled: it is not compiled into the product. There is no `load`, `loadstring`, `dofile` or `require` available to scripts, so no dynamic code can appear at runtime.
4. The compile step is resource-bounded: at most 5 seconds of host time and 256 MiB of memory per mod, source limits of [schema 3](segapatch-schema-3.md) section 4; exceeding either is a build failure `E_LUAU_COMPILE_LIMIT`.
5. Compile output is deterministic (fixed optimization level, debug level 0); its digest is part of the build identity.

## 3. VM policy

* **One VM per mod.** Each mod owns its VM, globals, standard library instance, memory allocator and budget. No cross-mod globals, no shared tables, no shared upvalues, no message passing in v1 (multi-mod channels, if ever, are a SEG-141 decision).
* **State.** Persistent mod state lives only in the one serializable value that the script's `save()` returns and `load(value)` accepts ([phases](mod-execution-phases.md) section 6.2). The VM itself is **not** serialized: on a state restore or rewind the VM is rebuilt from the compiled chunk, the `reset` callback is **not** run, and `load(value)` is called. State kept anywhere else is lost on restore;
  the `state-completeness` test category (section 8) detects divergence.
* **Library whitelist (allowed, by name).** Base: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `rawequal`, `rawget`, `rawset`, `select`, `setmetatable`, `getmetatable` (user tables only; the string metatable is not reachable), `tonumber`, `tostring`, `type`, `typeof`, `unpack`.
  Libraries: `math` (all except `math.random` and `math.randomseed`, which are replaced by the deterministic generator below), `string` (all), `table` (all), `bit32` (all), `utf8` (all), `buffer` (all, under the memory quota). Host: the capability bindings of the [ABI 2 catalogue](native-sdk-abi-2.md) section 4, namespaced under `sega`.
* **Disallowed, with rationale.** `os` (wall-clock and process access breaks determinism and isolation), `io` (filesystem), `debug` (introspection and upvalue tampering), `require` (module loading from the host), `load`/`loadstring`/`dofile`/`loadfile` (runtime code and bytecode), `getfenv`/`setfenv` (environment tampering),
  `collectgarbage`/`gcinfo` (host-visible nondeterminism, memory games), `newproxy` (userdata creation), `print` (replaced by `sega.log`, bounded), `coroutine` (v1: yields interact with callback budgets and state serialization; reconsidered by a later ABI minor), `tick`/`time`/`wait`/`spawn`/`delay` (not provided; wall-clock and scheduling).
  The standard libraries are made read-only (Luau sandbox), so a script cannot alter them, and each mod runs in a sandboxed thread environment.
* **Determinism.** No wall-clock, no host RNG. `math.random` returns values from a stateless generator whose seed for a callback invocation is the first 8 bytes (little-endian) of `SHA-256(package identity || frame counter (u64 LE) || callback index (u32 LE))`, where callback index counts callbacks of that mod in the frame. The generator algorithm is fixed by SEG-137-T002 and covered by a golden-sequence test.
  `pairs` order over tables keyed by non-number/non-string values is **not promised**; state serialization orders keys canonically and a static analysis rule (SEG-137-T005) flags `pairs` over such tables.
* **Faults.** A script error, budget exhaustion, quota failure or capability violation is a fault (phases section 6.1). The host sets a fault flag that `pcall` cannot clear: after the callback returns the mod is disabled whether or not the script caught the error.

## 4. Resource limits (defaults and the request rule)

| Resource | Default (policy maximum) | Fault class |
|---|---|---|
| VM memory (all allocations, hard quota) | 4 MiB per mod (`luau_memory_bytes` <= 4194304) | `memory` |
| Instruction budget per callback, in budget units (one unit per Luau interrupt: function call and loop back-edge) | 100000 per callback (`luau_budget_units` <= 100000) | `budget` |
| Instruction budget per mod per frame | 400000 units | `budget` |
| Wall-time watchdog per callback (host time; a safety backstop for a hung host, **not** part of the determinism guarantee: see below) | 4 ms; per frame total 8 ms | `watchdog` |
| Call depth | 200 | `stack` |
| Source per file / per mod | 128 KiB / 1 MiB (schema 3) | build |
| Compiled bytecode per mod | 512 KiB | build |
| `sega.log` | 256 bytes per line, 16 lines per callback | `budget` |
| One bulk read result | at most 4096 bytes | `range` |
| Serialized state | <= 64 KiB per mod, depth <= 8 | `state` |

**Request rule.** A package may declare lower values through the manifest `limits` object (`luau_memory_bytes`, `luau_budget_units`); any value above the policy maximum is `E_LIMIT_EXCEEDS_POLICY`. The host never raises a limit because a package asks. Hosts may ship lower platform maximums; they never ship higher.
Deterministic limits (memory, budget units, depth) decide faults reproducibly and are the only limits inside the determinism guarantee.
**Watchdog rule.** A watchdog fault is host-timing dependent and therefore non-reproducible. It disables the mod like any fault, and it marks the session **non-deterministic**: the diagnostic (`E_MOD_FAULT`, cause `watchdog`) is shown, recordings and replay/restore/rewind runs that contain it are marked *not replay-verifiable*, and every determinism test (`determinism`, replay-, restore- and rewind-equivalence) treats a watchdog fault as a **failure**, never a pass. Budget units are sized so a conforming mod never reaches the watchdog.

## 5. Host-binding rules

1. **Capability-scoped.** A binding exists only if its capability is granted; otherwise it is absent (calls fault with cause `capability`).
2. **Copy-in/copy-out.** Values crossing the boundary are numbers (integers up to 2^53), strings (at most 4096 bytes per transfer) and, inside the state value, tables of those. No function, thread or userdata crosses.
3. **No pointers or aliasing.** No binding returns a pointer or userdata that aliases guest or runtime memory. Reads return copies; writes are staged by the host ([phases](mod-execution-phases.md) section 4).
4. **Read-only during render.** In the `render` phase every mutating binding (`sega.mem.write`, `sega.input.set`, `sega.audio.send`, hook registration) faults with cause `phase`; only overlay and presentation output bindings work.
5. **Validation.** Every argument is range- and type-checked (space named, offset in range, width legal, value finite, integral where required); NaN, infinity, fractional and negative values where unsigned are required are `range` faults.
6. **No ambient authority.** Scripts get no filesystem, network, process, clock, environment or logging beyond `sega.log`.

## 6. Approval semantics shown to users

| Class | Install sheet states |
|---|---|
| BPS-only | "modifies the game image; no code runs"; identity and source-image digest shown |
| Luau-only | "includes scripts that run while you play; they can change gameplay and draw overlays within the capabilities listed; they cannot access your files or network; **not a security guarantee**"; consent bound to the exact identity |
| native / mixed | "includes native code that can do anything on your computer; install only from a source you trust"; explicit approval of the exact identity |

## 7. What user-facing documentation promises and does not promise

Promised (each backed by a test category): scripts have no file, network, process, clock or environment access; run with fixed memory and instruction limits; cannot write outside declared work RAM; are deterministic for the same input; are disabled when they fault.
**Not promised:** that a Luau-only package is "safe", virus-free or free of VM implementation bugs; that native or mixed packages are contained (they are not); that a script's behavior is correct. Documentation MUST use the words "restricted" and "limited", never "sandboxed" or "safe", for Luau, until SEG-144-T003 has published its test results.

## 8. Required adversarial test categories (M04-T006 and M11-T003 / SEG-144-T003)

| Category | Rules covered |
|---|---|
| `library-surface`: every disallowed name is absent; every allowed name present and read-only | section 3 whitelist/disallow, read-only libraries |
| `escape-metatable`: string metatable, library mutation, `getmetatable` tricks, `rawset` on protected tables | sandbox isolation |
| `memory-exhaustion`: `string.rep`, table growth, `buffer`, recursion; quota fault; process stays healthy | section 4 memory |
| `cpu-exhaustion`: infinite loops, nested loops, `pcall` around budget exhaustion; fault flag not clearable | section 3 faults, section 4 budget |
| `determinism`: `math.random` golden sequence, absence of time/wall-clock, identical digests on every host, `pairs` canonicalization in state | section 3 determinism |
| `binding-abuse`: ungranted binding, out-of-range/NaN/negative/huge arguments, writes in `render`, userdata smuggling, aliasing attempts | section 5 |
| `bytecode-injection`: bytecode as a payload, bytecode strings fed to absent loaders, non-text source | section 2 |
| `state-integrity`: oversized, cyclic, too-deep, function-bearing state; malformed `load(value)`; `state-completeness` (restore equivalence for the synthetic example) | section 3 state, phases section 6.2 |
| `mod-isolation`: two mods cannot observe each other; a faulted mod is disabled while the other continues | section 3 one VM per mod |
| `diagnostic-hygiene`: errors carry no host paths, pointers or addresses; diagnostics are bounded | section 5, stable classes |
| `compiler-input`: adversarial Luau source (deep nesting, huge literals) against the trusted compiler under its bounds | section 2 compile limits |
| `approval-semantics`: the install flow distinguishes BPS-only, Luau-only (consent) and native/mixed (native approval) | section 1, section 6 |

## 9. Implementation status (F06)

| Rule | Where | Test |
|---|---|---|
| pinned compiler, JIT absent, no network | `third_party/luau` (`PIN.txt`, `SHA256SUMS`) | `luau_build_test` |
| source only; bytecode from the trusted compiler; deterministic digest in the build identity | `luau::compile_mod`, variant component `luau_bytecode` | `luau_host_test`, `mod_luau_test` |
| no package or stored bytecode is loaded unless it matches the digest recorded at build | `luau::verify_compiled`, `Vm::load(CompiledMod)` | `luau_host_test`, `mod_luau_test` |
| whitelist, read-only libraries, string metatable unreachable, per-mod globals | `luau::Vm` | `luau_host_test` (`library-surface`, `mod-isolation`) |
| memory quota, budget units, frame budget, call depth, sticky faults, watchdog | `luau::Vm` | `luau_host_test` (`memory-exhaustion`, `cpu-exhaustion`) |
| deterministic `math.random` | `luau::Vm` | `luau_host_test`, `luau_host_golden_test` |
| leak-free teardown | `Vm::close()` | `luau_host_test` (also under ASan/UBSan in CI) |

The compile limits of section 2.4 are enforced by the bounded compile child (5 s kill, metered C++ allocator ending at 256 MiB; ADR 0294, tests in `mod_luau_test`). Known limits of this implementation: the meter counts C++ heap requests, not process RSS or stack; budget units count VM safepoints, not machine instructions, and C library functions that never reach a safepoint are bounded by the memory quota and the watchdog only. These are statements for SEG-144-T003 to attack, not safety claims.

## 9.1 Implementation status (F07)

| Rule | Where | Test |
|---|---|---|
| bindings exist only for declared, supported capabilities; an ungranted one is a sticky `capability` fault | `luau::Vm` with `HostConfig`, `sega_missing` | `luau_api_test` (`binding-abuse`) |
| argument validation (type, NaN, fraction, range, count) = `range`; wrong phase = `phase`; log/overlay rate = `budget`; staged output discarded on any fault | `luau_vm.cpp` bindings | `luau_api_test` |
| copy-in/copy-out only, read-only `sega`, no host handle, bounded and printable diagnostics | `luau_vm.cpp` | `luau_api_test` (`diagnostic-hygiene`) |
| API enumerated in one machine-readable list; declaration file generated from it | `luau::api_entries()`, `mod api` | `mod_luau_api_test` (committed copies cannot drift) |
| static check of declared capabilities and API use | `luau::analyze_mod` in `mod check/pack/build` | `luau_api_test`, `mod_luau_api_test` |
| state value: canonical, bounded, no function/cycle/NaN, strict restore | `Vm::save_state`/`load_state` | `luau_api_test` (`state-integrity`) |
| error taxonomy and per-mod disablement with a stable diagnostic | `luau::run_fixture_session` | `luau_api_test`, `mod_luau_api_test` |
| install sheet per trust class | `mod install` | `mod_luau_api_test` (`approval-semantics`) |
| a verified Luau-only package runs in the played program; the verified artifact is what the VM loads; a fault disables only the mod | `build --luau-package`, `luau_provider.cpp`, launcher executable cache | `mod_luau_executable_test`, `launcher_mod_variant_test` |

Not implemented: the `pairs` lint of section 3 (needs type inference; the Analysis module is not vendored), `sega.mem.*`, `sega.input.*`, `sega.hook`, `sega.state`, `sega.audio.*` (M05/SEG-141/SEG-145), feature detection of optional capabilities by a script.

## 10. Non-goals

Vendoring Luau (SEG-137-T001); claiming Luau-only packages are safe; sandboxing native code; network, filesystem or process APIs for scripts.
