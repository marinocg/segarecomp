# ADR 0278: Genesis save states, cartridge persistence and visual rewind

- Status: accepted and implemented (SEG-131, one combined product PR, checkpoints A-H).
- Related: ADR 0098 (header-declared SRAM), ADR 0136 (X24C01 EEPROM), ADR 0252 (Sega CD backup RAM persistence, unchanged), ADR 0072/0073/0075 (Z80 machine, image registry, mixer),
  ADR 0267 (native patch providers), ADR 0040/0042 (runner allowance, bounded diagnostics).
- Contract: [genesis-save-states-persistence-and-rewind.md](../architecture/genesis-save-states-persistence-and-rewind.md) (frozen at Checkpoint A; the as-built differences are listed below).
- User documentation: [docs/product/save-states.md](../product/save-states.md).
- Scope: Genesis / Mega Drive only. Not an interpreter, JIT or runtime decoder (a restore continues the generated-native program from its PC); not a generic CPU serializer; not Sega CD or Master System
  whole-machine states; no new SRAM/EEPROM profile; no cloud, no netplay, no disk-backed rewind, no background save thread.

## Decision

1. **A Genesis-owned, explicit snapshot** (`platforms/genesis/runtime/genesis_snapshot.[ch]`): a versioned little-endian `SGSTATE` container of ten sections (M68K, work RAM, VDP incl. DMA, I/O + Z80 bus + scheduling,
   cartridge memory and EEPROM protocol, Z80, PSG, YM2612, mixer, audio counters) with a SHA-256 over the whole file, fixed section maxima and a 4 MiB file limit. Nothing writes `sizeof(struct)`;
   host pointers, callbacks, immutable ROM references and observers stay attached to the initialized machine. The container carries a source digest, a variant (patch) digest, the region, machine-profile flags
   (sound / SRAM / EEPROM), an execution fingerprint, the guest frame and master clock, and a bounded RGB565 thumbnail.
2. **Two-phase restore.** Parse and validate everything (structure, integrity, identity, profile, every value that indexes memory or bounds a device) into a *staged shallow clone* of the runtime, Z80 machine and
   audio devices, then commit with plain copies that cannot fail. A failed load leaves the running machine byte-identical.
3. **The barrier.** Capture and restore happen only between two `genesis_runtime_run` slices (the viewer's once-per-slice service point): the M68K is on a dispatch boundary, no host callback is running, and
   the Z80 / audio devices may lag the master clock exactly as they do at any other synchronization (that lag is part of the state).
4. **Complete audio state.** The YM2612 gets a paired bounded save/check/load (`ym2612_save/check/load`: ymfm's own blob plus the wrapper's clock, next sample start, both timer expiries, busy end and sample count,
   validated by invariants), the PSG a `sn76489_load_state_bytes`, and the deterministic mixer its windows, FIFOs, digests and counters. The host-only playback ring is emptied by a restore; guest-time mixer
   state is exact. Chip phase is never derived by replaying register writes.
5. **Z80 images by content hash.** The bound image is stored as its content hash (a new always-present `genesis_z80_image_content_hash`), never as an ordinal, and mapped back through the current registry; a state
   needing an image the program does not contain is refused. The mutable-code guard is untouched.
6. **Persistence** (`genesis_persistence.[ch]`): `cartridge.srm` is the dense `storage_bytes`; `cartridge.eep` the 128-byte array (protocol lines are not persisted; they are part of states and rewind). The viewer
   hook loads before the first guest instruction (a wrong-size, non-regular, symlinked or unreadable file is refused before any guest instruction and never rewritten) and commits **only when the player ended the
   session** (`GENESIS_VIEWER_WINDOW_CLOSED`): candidate in the same directory, flush, validate, keep `<name>.prev`, one rename.
7. **The clean-exit finding.** Every Genesis end - clean window close, exhausted runner budget, typed stop - prints a report and exits 0, and `WINDOW_CLOSED` and `RUNNER_EXHAUSTED` both return
   `GENESIS_RUNNER_RESOURCE_LIMIT`. The exit status cannot distinguish them, so the generated `main` argument ABI was left untouched and the clean-end signal is the viewer's own: only
   `WINDOW_CLOSED` commits. A resource-limit exit is never treated as success. No new argv flags exist: the launcher passes `SEGARECOMP_SAVE_DIR` and `SEGARECOMP_SAVE_VARIANT` (and the existing
   `SEGARECOMP_ROM_SHA256`) in the child's environment, and replays, recordings and the build pass never receive them.
8. **Storage identity.** `<library>/saves/genesis/<original image sha256>/{vanilla | patches/<patch identity>}/` with `cartridge.srm|eep` and `states/{quick,slot-1..5}.sgstate`. Full lowercase 64-hex identities
   are validated; a malformed one yields no path and no feature. The tree is outside `<cache>/games/<build hash>`, is counted as user data by Storage, and is unreachable by any cleanup plan.
9. **A machine-independent provider interface in the shared viewer** (`libs/viewer/include/sv_state.h`): availability and reason, six slots, history count / metadata / thumbnails, `restore_entry`,
   `rewind_opened/cancelled`, a history note. The viewer owns the pause-menu rows, slot pages with the focused slot's own thumbnail, overwrite / load confirmations focused on *No*, quick keys F5 / F8, the rewind
   surface (F7, the pause-menu row, a two-trigger chord) and never reads a machine. Master System and Sega CD register no provider and see accurate disabled rows.
10. **Rewind** is the same snapshots in a bounded in-memory ring: every 500 ms of **guest** time at the barrier, 60 s target, **96 MiB** hard budget, oldest-first eviction by age and by bytes, no worker, no disk.
    Entering rewind freezes the guest and references the live machine as the anchor (cancel mutates nothing); confirming restores two-phase, drops every newer entry (a new branch) and runs one *sink reset*
    (host audio queue, presenter, pacing deadline, raster capture, published-frame baseline rendered from the restored VDP, held inputs). A slot load starts a fresh history.

## As built, relative to the frozen contract

- Identity is supplied by the launcher through the environment (`SEGARECOMP_ROM_SHA256`, `SEGARECOMP_SAVE_DIR`, `SEGARECOMP_SAVE_VARIANT`), not through generated-program flags (point 7).
- The execution fingerprint covers the state schema, section sizes and machine flags; the Z80 registry is deliberately **not** in it (images are referenced by content hash), so a rebuild keeps its states.
- The contract's wording "validated Z80 image ordinal" became "content hash"; "frame-observer baseline" is a frame rendered from the restored VDP by the single-snapshot producer.
- Rewind does not need a save location or a verified identity (it is in memory); save states do.
- F5, F7 and F8 became reserved host keys (`sv_key_reserved`); a saved binding using one of them is cleared on load, like any other reserved key.
- The viewer loop gained one optional host callback (`discontinuity`) so pacing and presentation baselines are re-based after a restore instead of treating the clock jump as frames to wait for.

## Rejected alternatives

- A generic, reflection-style serializer or `sizeof(GenesisRuntime)` dumps: breaks across builds, embeds pointers, and cannot be validated field by field.
- Replaying inputs from power-on or deriving chip state from register-write history: not byte-exact and not bounded.
- Committing cartridge saves on any exit 0: a budget or typed-stop exit would overwrite progress with an uncertain machine state.
- Storing player data under the build hash: a rebuild or cache cleanup would lose it.
- Delta/compressed rewind and a capture thread: measured unnecessary (a snapshot is ~150 KB and ~0.4 ms; a full minute is ~21 MiB; overhead ~1.2 %).
- Authenticating states (signing): states are the player's own data, like a save; integrity (SHA-256) and value validation are the contract.

## Consequences and limitations

- A state format change (`state_schema`) invalidates older states; there is no migration yet. A newer program cannot load a state it cannot prove compatible, and says why.
- A state taken in a game that has latched a typed stop (a Z80 view stop, a mixer fault) is refused at capture rather than saved wrongly.
- The first frame after a restore is rendered from the restored VDP state with the single-snapshot producer (not the per-scanline capture of the original frame), so a mid-frame raster effect can differ for
  that one baseline frame; guest behaviour is unaffected and the next frame is the game's own.
- Disk durability is the OS flush before a rename; power-loss atomicity is not claimed. Windows persistence code paths (wide-character file API) are exercised by CI only.
- Native patch providers are presentation-only by ABI version 1; a future ABI giving a provider guest-visible effects must refuse state features until it can capture its state.
- Sega CD and Master System have no whole-machine states; the shared viewer exposes the capability only where an adapter registers a provider.

## Validation

`genesis_snapshot_test` (restore reproduces the future, byte-exact audio, corruption / identity / profile / image refusals, empty Z80 registry), `genesis_persistence_test` and
`genesis_states_test` (separate real viewer processes), `genesis_rewind_test` (cadence, eviction, budget, branching, cancel, replay equivalence), `genesis_rewind_session_test`,
`genesis_production_states_test` (the production `segarecomp build` chain: a state saved while audio plays resumes the identical sample stream), `sv_state_test`, `sv_rewind_test`,
`sv_state_gallery_test`, launcher policy and storage tests, and the independent adversarial review recorded in the pull request.
