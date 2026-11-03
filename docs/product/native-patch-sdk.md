# Native patch SDK (Genesis, ABI 1)

Segarecomp provides a **generic** native-patch interface; a patch package owns every game-specific detail. A new game's widescreen provider can be written, packaged, distributed, imported, approved and built **without
modifying or recompiling segarecomp**: the launcher compiles the package's `provider.c` with the bundled C11 toolchain into a separate generated-native Patched program. The product contains no game-specific provider, registry or
capability table.

The provider runs with the generated program's operating-system permissions. This is a **trusted native-source patch** mechanism, not a sandbox; hashes give integrity, not author authenticity (see `patch-packages.md#trust`). A future sandboxed format (e.g. WebAssembly) would be a separate capability.

## What the host gives a provider

One header: `platforms/genesis/sdk/segarecomp_genesis_patch.h` (installed with the runtime; `<stdint.h>`/`<stddef.h>` only; no private or C++ header is on the provider's include path). A provider exports

```c
const GenesisNativeCompat *segarecomp_patch_provider(void **state_out);
```

and fills a `GenesisNativeCompat` table: `abi_version` (`GENESIS_NATIVE_COMPAT_ABI`), a name and version, the components it serves (`GENESIS_COMPAT_SCENERY`, `GENESIS_COMPAT_HUD`) and three functions:

* `prepare(state, frame, decision)` is called once per produced frame. It validates every input the frame will use and accepts a subset of `frame->requested`, or declines the whole frame (a declined frame is presented by the
  generic path and is never labelled authored). `frame` offers only what the manifest declared: `work_ram` (64 KiB copy taken at the VBlank onset that preceded the frame, so it is the state the game used to prepare it),
  `vram`/`lines` (the frame's VRAM and per-line registers/CRAM/VSRAM), the picture geometry, and the `host` helper table.
* `scenery_word(state, plane, row, x, hscroll, vscroll, &word)` returns the name-table word of the world cell under an off-screen column (the host still decodes patterns, palettes, flips, priority and sprites).
* `hud(state, overlay)` fills a bounded RGBA overlay (at most 128x256) that the viewer draws after side treatment.

Helpers (`GenesisCompatHost`): `read_hscroll`, `read_vscroll`, `vscroll_is_per_column`, `cram_rgb` - the runtime's own VDP functions, so a provider never clones VDP decoding.

There is **no** API to write guest memory, drive a device or reach the host. The interface is read-only by construction; that is design hygiene, not a security boundary.

## Writing and packaging a provider

`docs/product/sdk-sample/provider.c` is a complete synthetic example. Compile-check it like the build does: `cc -std=c11 -Wall -Wextra -pedantic -I platforms/genesis/sdk -c provider.c`. Then

```sh
python3 tools/segapatch.py create --source game.bin [--target game-patched.bin] --id my-game --name "My Game widescreen" --version 1.0.0 \
    --author "Your Name" --license MIT --native-source provider.c --native-caps frame.work_ram,frame.vdp,scenery.words,overlay.rgba \
    --width-max 100 --default-widescreen enhanced_gameplay --default-side-style raw --out my-game.segapatch
```

Distribute that single file. A user imports it in the launcher (Game -> Install patch), reads the warning, approves it, builds the separate Patched variant and plays it. The optional BPS (`--target`) changes the game itself
(for example removing the original HUD that the provider redraws); without it the original image is compiled unchanged.

## Build and identity

`segarecomp build --patch-package P --patch-identity ID` re-verifies the package (archive, manifest, source digest, ABI, capabilities, that `ID` equals the identity of the package it read (the launcher passes the identity the user approved; on the CLI this is a consistency check, not authenticated consent), that the image is the one the package declares),
extracts `provider.c` into the variant's own build folder, re-hashes it, compiles it with isolated flags (`-std=c11 -I <sdk>` only) and links it into that variant's program; the viewer calls `segarecomp_patch_provider` and attaches
the table only if the ABI version matches (otherwise the program presents generically). The variant cache key and the build record contain the original and effective image digests, the package identity, the provider source
digest, the ABI version and the capability set. A failed provider compile or link fails that build only; Vanilla and previously published builds are untouched.

## Non-goals

No dynamic-library loader, JIT, interpreter, scripting VM, second VDP renderer, universal guest-state API, package build scripts, dependency resolution, marketplace or automatic execution of untrusted code.

## Platform coverage

The sample provider is compiled as strict C11 against the SDK header on every CI platform. The complete package-to-generated-native-executable build (`genesis_patch_provider_build_test`) currently runs on Linux only;
the other platforms validate provider compilation, the launcher/package tests and release packaging. End-to-end portability of the package build is therefore not claimed for Windows and macOS.
