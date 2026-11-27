# ADR 0324: Master System target identity, typed edits and BPS, bank-aware Z80 injection with CPU-owned admission, and mod-owned asset adapters

- Status: accepted (combined delivery F17: SEG-147-T001, SEG-147-T002, SEG-147-T007).
- Builds on: ADR 0280 (adapters), 0281/0288 (schema 3), 0289 (variant identity), 0290 (typed edits, verified symbols), 0292/0293 (in-tree assembler, injection, header repair, BPS), 0279 (SMS save states),
  0061/0064 (declared mapper). **Supersedes** the statements of ADR 0292/0293 that Master System injection is `rom_only` only and that other platforms have no header repair.

## T001: target identity and edits

1. A target is the exact image digest **and** the declared mapper (`sega` or `rom_only`). The mapper must fit the image and every image derived from it: `rom_only` is 32 KiB; `sega` is a power of two from 32 to 512 KiB
   (`E_MAPPER_UNSUPPORTED`). An unverified mapper family stays `E_FIELD_VALUE` in the manifest. Nothing infers a mapper.
2. Edits keep ADR 0290 (little-endian default, expected-value assertion). Diagnostics on this platform name `offset`, bank and bank offset (16 KiB banks of the image).
3. **Header repair `sms-checksum`** (manifest/`mod.toml` `header_repair`) exists only for the header the platform defines: `TMR SEGA` at `0x7FF0`, checksum at `0x7FFA` (little-endian) over the range of the size code
   (`0x7FFF` low nibble: A, B, C, E, F, 0, 1; code D is documented as defective and is not accepted) without the 16 header bytes (SMS Power! "ROM header"). Policy mirrors Genesis: explicit repair fails closed
   (`E_HEADER_UNSUPPORTED`) without such a header; `none` opts out and warns when a verifying original no longer verifies; absent repairs only when the original verifies and the image changed. A change that
   destroys the header under the default policy warns (`W_CHECKSUM_NOT_REPAIRED`) instead of guessing. Authoring `0x7FFA-0x7FFB` while repair applies is `E_HEADER_FIELD_COLLISION`. Only the checksum is written:
   never the region, size code or product fields; no header is ever added. The headers some small images carry at `0x1FF0`/`0x3FF0` are not repaired (baseline images are at least 32 KiB).
4. **Build and identity.** `segarecomp build --data-package` accepts a Master System data-only package: it verifies the package identity, derives the effective image from the exact original and compiles it through the
   unchanged generated-native SMS route under the package's declared mapper (the platform is the package's; `--platform`/`--mapper` that disagree are `DATA_PLATFORM_MISMATCH` / `DATA_MAPPER_MISMATCH`).
   `status.json` records `data_mod` (package, original and effective digests). Variant identity is ADR 0289 unchanged (the manifest, so the mapper, is inside the package identity).
5. **Save separation** reuses the Genesis rules: a save state binds to the program's cartridge digest and mod variant (ADR 0279), and the launcher's folder for a Master System mod or patch is
   `saves/master-system/<original sha256>/<mapper>/{mods,patches}/<identity>`, never Vanilla's. The launcher does not yet offer Master System mod installation; this delivery adds only the folder rule.

## T002: injection and admission

6. **Placement on `sega`** (`rom_only`: offset = address, inside the 32 KiB cartridge, below `0x8000`). A block lies inside one 16 KiB bank and does not cross the fixed 1 KiB boundary (`E_GUEST_BANK_BOUNDARY`). A block below `0x400` runs at
   its offset. Any other block runs at an address of a mapper window below `0xC000`, at or above `0x400`, with the same position inside the bank (`at`; required above image offset `0xBFFF`; else `E_GUEST_ADDRESS`);
   beyond 512 KiB is `E_MAPPER_UNSUPPORTED`.
7. **References resolve only where the mapping is guaranteed**: to the fixed first 1 KiB, or, from a banked block, to the same bank through the same window, at or above `0x400` (below it window 0 shows the
   fixed bank 0). Every other label or `@symbol` use (call, jump, relative branch, data pointer), including label arithmetic that leaves that region, is `E_GUEST_BANK_REFERENCE`. On `sega` an `@symbol` is an
   image offset and is translated into the caller's window; work RAM addresses are written as numbers. Direct absolute stores to the mapper registers are refused (they lie outside the writable RAM window),
   and no cross-bank reference form exists; stores through registers can still reach them (Limits). On Master System a label used as a control target must be followed by an instruction of its block
   (`E_GUEST_BRANCH_TARGET`).
   **Direct accesses** through a label or `@symbol` (`ld a,(x)`, `ld (x),a`, `ld hl,(x)`, `ld (x),hl`) must lie wholly, operand width included, inside one such region: the fixed first 1 KiB or the 16 KiB window
   of the first byte (`E_GUEST_BANK_REFERENCE`); on `rom_only`, the 32 KiB cartridge or work RAM `0xC000-0xFFFF`, where a symbol may name a variable (`E_GUEST_ADDRESS`). Numeric addresses are the author's explicit
   CPU addresses and are not judged by this rule; register-indirect accesses are not tracked.
8. **The CPU owner admits the bytes.** `libs/media` may not depend on a CPU library, so it emits a neutral `GuestListing` (bytes and instruction boundaries) and takes a `GuestAdmitter` from the caller; the SMS owner
   provides `admit_z80_guest` (machine library, over the Z80 library's decoder and effect projection). A Master System `guest-asm` payload **without** an admitter is refused (`E_GUEST_ADMISSION_UNAVAILABLE`), so a
   caller that forgets it cannot skip the check. `mod check|pack|build|inspect|apply|bps|verify-bps|install|variant` and the build route pass it. `E_GUEST_NOT_ADMITTED` when: the decoder does not read exactly one
   instruction at an assembled boundary; a successor is not statically known (`jp (hl)`); a form has neither a projected effect nor a place in the short list of straight-line forms the projection omits
   (`push`, `pop`, `inc/dec (hl)`, `add hl,rr`, `ex de,hl`, `rlca`, `rrca`, `cpl`, `scf`, `ccf`; plus `call cc`, whose target is immediate); execution can fall through into anything but the next admitted
   instruction (including `even` padding and the end of the block); a control target lands in injected bytes other than the start of an instruction at its assembled address (below `0x400` always the fixed
   bank 0), in the rest of a declared free range, or outside the declared cartridge (at or above `0x8000` on `rom_only`, `0xC000` on `sega`; on `sega` a translated target is in the fixed area or the running
   block's own bank, which exists because the block lies inside the image). Other targets are existing cartridge code, nameable only through a verified `@symbol`; its content is the author's assertion and is
   not analysed.
9. The admitted image goes through the unchanged generated-native SMS Z80 producer. Nothing decodes, patches or interprets at run time. Genesis M68K and Genesis Z80 paths are unchanged.

**Limits.** Admission covers the control transfers an instruction itself names. Stores through registers (`ld (hl),a`, stack operations) are not bounded statically (as in ADR 0293), so a routine can reach
the mapper registers through them, and a `ret` goes wherever the stack says: `push hl` / `ret` is a computed jump that admission does not see. A target the generated program has no code for stops it fail-closed. A reference rule proves where an injected block's *own* names resolve; it does not prove the game
maps that bank when its code reaches the block, which stays the author's edit. Function hooks are not part of this decision (SEG-147-T003/T008).

## T007: asset replacement

10. There is **no universal asset decoder or encoder** in the product. A mod declares its replacement art, palettes, text and tables as schema 3 `asset` sources of class `data` and owns an adapter that turns them
    into a typed `rom-edit-set` for the one format it understands; the build asserts the original bytes again. The SDK example `platforms/master-system/sdk/examples/asset-replace/` shows the shape on a
    synthetic cartridge: pinned digests of the original image and of every replaced region, exact size and value range checks, deterministic output, and no write on any failure. The adapter is an ordinary
    author-side script; the product never runs it while installing or building a package.

## Evidence

`tests/mod_sms_test.py` (T001/T002 rules through the CLI), `tests/mod_sms_native_test.py` (an injected bank-7 routine executed by the generated-native program; fail-closed build cases),
`tests/mod_sms_assets_test.py` (expected frames computed independently of the C renderer and the adapter; fail-closed assets), `launcher_policy_test` (save folders), `sms_snapshot_test` (states of another
cartridge or variant are refused). Synthetic data only.
