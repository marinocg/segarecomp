# ADR 0267: SEG-124 - self-contained trusted native-source patch providers, package schema 2

## Status

Accepted (SEG-124, one product PR). Builds on ADR 0262-0266. It does not replace BPS patches (ADR 0266). The first revision of this ADR admitted one title-specific provider built into the product; that
ownership was corrected in the same PR (still unreleased, so schema 2 is revised in place): **segarecomp provides the generic patch ABI, frame access, validated build integration and rendering hooks; a patch package
owns all game-specific implementation.**

## Context

ADR 0266 classified game problems as A (renderer defect), B (game logic limitation: BPS), C (original hardware capacity limit: a game-aware presentation extension may be considered) and D (unavailable information). A BPS patch
cannot make the VDP show world the game never prepared, nor add a HUD beyond the original viewport; a generated-native program can present authentic information the hardware never displayed because the game's own level data
is resident in guest RAM. That knowledge is per game. Baking one game into the product would make every further game a product release, and community patch authors could not contribute.

## Decision

1. **Three sources stay separate.** Original guest execution (unchanged); generic presentation (the VDP-backed Level 2/3 extended surface, ADR 0262-0265); and a *native patch provider* that reads a bounded, frame-synchronized,
   read-only view of guest state and reports presentation data (extra name-table words, an overlay). A provider never writes guest state, never runs guest code, never changes CPU/VDP/Z80 semantics; the generic VDP still decodes
   patterns, palettes, flips, priorities and sprites.
2. **Stable generic ABI (version 1).** `platforms/genesis/sdk/segarecomp_genesis_patch.h` is the one public header (stdint/stddef only). A provider exports `segarecomp_patch_provider`, returning a table with `prepare`,
   `scenery_word` and `hud`. Host abilities are **capabilities** (`frame.work_ram`, `frame.vdp`, `scenery.words`, `overlay.rgba`), never a game name. The host grants exactly the capabilities the verified package declared
   (an undeclared one is passed as NULL), supplies pure VDP helper functions (`GenesisCompatHost`) instead of cloning VDP decoding, and offers no memory-write or device-control API. The runtime has no game switch and no
   provider index; the runtime-internal `native_compat.h` only includes the SDK header and declares the host table.
3. **Frame synchronization.** The frame is published at the VBlank onset, before the VBlank handler runs, so the state the game used to prepare the displayed frame is the work RAM at the PREVIOUS onset. The runtime copies the
   64 KiB work RAM into an adapter-owned buffer right after each publication and stamps it; a provider that needs it receives it only if it was taken exactly one frame earlier, otherwise the frame is served by the generic path.
4. **No second renderer.** Scenery is delivered as name-table words, never pixels. Provenance: only a frame whose scenery came from a provider that accepted that frame is `SV_EXTSRC_AUTHORED`; a declined or partially
   failed frame is `GENERIC` and receives the normal Comfort treatment. The HUD is a separate RGBA overlay drawn by the host after side treatment.
5. **Package schema 2 (`docs/product/patch-packages.md`).** The `native` payload is ONE portable C11 source file `provider.c` (`format: "c11-source"`, SHA-256, ABI version, required capabilities, explicit
   `executable: "yes"`), next to or instead of the BPS. The manifest also carries author and optional license. No compiler arguments, linker flags, header paths, scripts, binaries or fetch URLs exist in the format; only the
   declared payload types are accepted; the retired descriptor-style schema 2 (a built-in capability table) is refused explicitly. Schema 1 is unchanged.
6. **Build integration.** `segarecomp build --patch-package P --patch-identity ID` takes a *reference*. The build re-reads and re-verifies the package (shared parser), checks the approved identity (digest of the manifest) and the
   image digest (a preview in the launcher is not trusted: files may change), extracts `provider.c` into `<output>/native/`, re-hashes it, compiles it with isolated flags (`-std=c11 -I <sdk dir>` plus the bundled compiler's own
   arguments; the runtime/viewer private headers are not on its include path) and links it into that variant's program; the viewer hook calls `segarecomp_patch_provider`. No dynamic library, JIT, interpreter or runtime plugin;
   no product rebuild. A failed provider compile/link fails that variant's build only (the launcher publishes builds atomically).
7. **Identities.** The variant cache key and the build record contain the original and effective image digests, the package identity, the provider source digest, the ABI version and the capability set. A provider source change
   invalidates the build; another ABI is refused or rebuilt; Vanilla never resolves to a patched program; removing a patch touches no Vanilla build or global setting.
8. **Trust.** This is a **trusted native-source patch** mechanism and it is **not a sandbox**. Bounded archive parsing, source hashes, a restricted interface and compiler flags are NOT a security sandbox and must never be
   described as one. The launcher shows an explicit sheet (executable source, author, license, package identity, source digest, declared capabilities, "not sandboxed, install only from a trusted source"), installs a
   code-bearing package only after the user approves that exact package identity, never compiles a new code-bearing patch from scanning or updates, revalidates the identity at build and refuses an unapproved record. The launcher records the user's approval; the CLI `--patch-identity` only checks that the supplied value equals the package's identity and is not authenticated consent. Hashes give integrity, not author authenticity; there are no signatures. Archive strings
   are data and never reach a command line. If a deployment required patches that are safe to run untrusted, trusted C cannot satisfy it; a separately designed sandboxed format (WebAssembly AOT) would be a different capability and
   is not part of this ADR.
9. **Failure.** A provider that declines a frame, reports a fault for a cell it accepted, or returns another ABI version produces the generic result with an accurate status; Vanilla and BPS-only builds contain no provider code.
10. **Non-goals.** No dynamic-library loader, JIT, interpreter, scripting VM, opcode decoder, second VDP renderer, universal guest-state introspection, game-specific behaviour in the generic runtime, marketplace, dependency
    resolution, package build scripts, automatic execution of untrusted C or universal gameplay API.

## Amendments (SEG-124, same PR)

* **First delivered provider.** Sonic the Hedgehog 1 (native widescreen scenery, relocated HUD) is the first package written against this ABI. Its source, authoring tooling and real-game evidence live in the private harness; the product
  ships no Sonic source, capability, ROM digest or registry. Verifying it is the harness's job; the product tests use synthetic providers.
* **HUD replacement.** A package may remove the game's own HUD sprites with its BPS (counters and update logic untouched) and redraw the same pieces from the frame's VRAM tiles and CRAM through `overlay.rgba`, anchored to the left
  edge of the visible side or at the original place inside the picture when there is no visible side.

## Consequences

Further titles need only their own investigation and package, not a product release. The price is that code-bearing packages need an explicit trust decision, which the launcher makes unavoidable and honest. The ABI is versioned;
a change that cannot be expressed compatibly gets a new version, which packages must declare.
