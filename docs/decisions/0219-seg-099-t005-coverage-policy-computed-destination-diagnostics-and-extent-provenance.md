# ADR 0219: SEG-099-T005 - coverage policy, computed-destination diagnostics and the provenance of `static_disc_extent`

- Status: Accepted (pre-registration, results, decisions). Milestone SEG-099 (M3). Related: ADR 0211 (static admission), 0213 (hull / loaded policies, frontier classification), 0214 (inventory status), 0218.
- Privacy: project-authored fixtures; no commercial data.

## 1. Static range policy (`hull` vs `loaded`) on the new inventories

Pre-registered question: does the explicit before/after evidence justify changing the Compatibility default from `hull` (the traversed executable subrange of each proven module) to `loaded` (every decodable aligned start across the proven loaded module)? No default changes without it.

Measurement (the `recursive` fixture: modules A (2 sectors), B and R admitted statically; clean object cache, 8 jobs, same host):

| Policy | Static bytes admitted | Generated C | Program binary | Build CPU | Route 0 / route 3 result |
|---|---|---|---|---|---|
| `hull` (default) | 2,112 | 29.4 MB | 7.58 MB | 62.8 s | A runs / A then R runs |
| `loaded` | 8,192 | 32.4 MB (+10%) | 8.61 MB (+13.5%) | 70.3 s (+12%) | identical results |

With the recursive walk (ADR 0218) a computed transfer to a *proven constant* inside a loaded module is followed and compiled under `hull` (the former `computed_entry` gap of ADR 0213 now needs `--static-recursion 0` to reproduce); `loaded` is only needed for destinations the walk cannot compute. The cost of `loaded` is the header / trailing / unreachable data of every module (here 6 KiB of 8 KiB), i.e. proportional to non-code bytes, and on the authentic titles (modules of 2-32 KiB with long data tails) would be larger than here; no authentic module is proven, so no authentic number exists.
**Decision: `COVERAGE_POLICY_VALIDATED` for keeping `hull` as the Compatibility default; `loaded` stays an explicit option.** Correctness is identical under both; the cost evidence does not justify the change; the benefit it would add (destinations the walk cannot compute) is not demonstrated on any inventory.

## 2. Computed-destination diagnostics

Before: a computed transfer to an uncompiled destination stopped `entry_not_compiled` with the *transfer instruction* as `pc` and the detail field 0 (ADR 0213 decision 3); `final_stop.location` described the instruction. Now the Sega CD emitter stores the computed destination in the stop's detail (the convention the runtime comment already documents for this class), and `status.json` `final_stop` carries
`destination` and `destination_location` (`inside_admitted_extent` | `inside_static_module_window` | `outside_admitted_images`) next to the unchanged `location` (always about the instruction). A destination is classified from its own address against the admitted extents and proven module windows; an unknown or unreachable destination is `outside_admitted_images`, never "inside a known module". The stop class set is unchanged; Genesis emission is untouched. Checked in `tests/sega_cd_bios_scale_test.py` (a destination inside the proven module's loaded window but outside its admitted range, reported separately from the instruction inside the admitted extent).

## 3. Provenance audit of `static_disc_extent`

Where the authority is established: (1) the supported builder gets coordinates only from the static scan run by the build itself (`discover-modules --static-extents`, a file the build deletes first and reads back); `build` has no option that injects an extent. (2) The emitter (`emit --admit static-extent:<cpu>:<offset>:<base>:<length>`, a developer interface) re-reads the bytes from the verified disc and the program guards every instruction against them at run time, so a wrong coordinate can never execute wrong code. But the admission *kind* claimed a scan proof it did not check: any coordinate string was accepted and entered the program identity as `staticextent`, indistinguishable from a scanned one.
**Concrete demonstration (before the guard):** `emit --admit static-extent:sub:<offset of the volume descriptor sector>:0x90000:2048` was accepted, and so was a proven source at a guest base the scan had not proven.
**Guard (minimal):** the emitter recomputes the scan (a pure function of the disc; the strongest options the build uses; once per process) and refuses an extent that no proven module mapping contains, with the typed error `static_extent_unproven` (sub processor only; an incomplete scan proves nothing). Developer assertions remain possible through the explicitly named manual kinds (`--disc-image`, `extent:<pass>:...`); they are not presented as static results. Tests: an extent the scan proved is admitted; the descriptor sector as code, a proven source at an unproven mapping and an extent running past the source are refused (`tests/sega_cd_services_loader_test.py`).

## 4. Inventory labels

`nested_loaders_not_analyzed` now appears only when the recursive walk is off (`--static-recursion 0` or no static discovery). Every inventory names `main_processor_loaders_not_analyzed` (no main-side analyzer exists), so `complete` is unreachable by construction and every build reports `partial` or `blocked`; a scan that needed joins to finish adds `static_scan_joined`. These are honest labels; no authentic title is claimed complete.

## Not claimed

An authentic coverage gain, any change of the Compatibility default, completeness of any title, a main-processor loader analysis.
