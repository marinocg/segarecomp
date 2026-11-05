# ADR 0268: Declared Genesis lock-on pass-through cartridge

- Status: Accepted; section 5 added by SEG-122-T009.
- Predecessors: ADR 0049 (cartridge mirroring for reads past a power-of-two chip), ADR 0097 (compatibility repairs; its non-goal
  "lock-on/bank-switched mappers"), ADR 0098 (header-declared cartridge SRAM), ADR 0065 (Master System: the mapper is declared,
  never inferred).

## 1. Question and scope

A lock-on pass-through cartridge carries its own 2 MiB ROM at `$000000-$1FFFFF` and routes the upper half of the cartridge window,
`$200000-$3FFFFF`, to a second cartridge connector on its top. Locally held images of that board class reach two failures with the
current product:

- the base cartridge alone (2 MiB, nothing attached) is mirrored into the slot by ADR 0049, so the base finds its own header in the
  slot, takes it for a foreign attached cartridge and follows the wrong boot path (no failure is reported: the program silently
  runs the wrong game mode);
- the conventional composite image (the base followed by the attached cartridge's 2 MiB image, 4 MiB) stops a few frames after
  power-on at a BYTE write that maps the attached cartridge's SRAM through `$A130F1`: the composite's own header (the base's)
  declares no extra memory, the attached cartridge's header does.

Only non-reconstructable classifications are recorded; nothing from any commercial image appears in this repository.

## 2. Decision

1. **Declared, never inferred, by the build tool.** `segarecomp build --mapper lock_on` (Genesis only; the emitter's `--genesis-lock-on`)
   declares the board. Nothing about a title, a checksum or a header text selects it in the build tool, the recompiler or the runtime; without the
   declaration every image keeps ADR 0049/0098 behavior byte for byte. Any other Genesis mapper declaration is still refused (`MAPPER_NOT_APPLICABLE`).
   The consumer launcher may choose to pass the declaration (section 5).
2. **Image shapes.** A declared lock-on image is exactly 2 MiB (the base alone: an empty slot) or exactly 4 MiB (the base followed by
   the attached cartridge). Any other size is refused at build time (`LOCK_ON_IMAGE_SIZE`).
3. **No mirror into the slot.** The base image is never mirrored into `$200000-$3FFFFF` (the slot is not a decode alias of the base
   chip). A 4 MiB composite is mapped linearly, as before.
4. **Empty slot.** With a 2 MiB image a DATA read wholly inside the slot answers `$FF` in every byte
   (`SEGARECOMP_GENESIS_EMPTY_SLOT_BYTE`). On hardware nothing drives the data bus there (open bus, whose value depends on the bus
   history); the fixed value is a deterministic project compatibility policy, not reconstructed open bus. The one property software
   can rely on, that no cartridge header is visible in an empty slot, holds. Writes and instruction fetches in the slot stay
   fail-closed.
5. **One rule everywhere.** The translation-time memory gate (absolute operands touching the extent are routed to the runtime SRAM owner),
   the analysis and the emitter derive the extra memory from one shared function (`genesis_program_extra_memory`).
6. **Attached extra memory.** For a 4 MiB composite whose base header declares no extra memory, the attached cartridge's own header
   (at `$2001B0`) declares it, with its bus addresses unchanged. The extent then lies over the attached ROM, so ADR 0098's
   `$A130F1` map bit selects SRAM or ROM there (never "always mapped"); ADR 0136's EEPROM form over ROM is recorded as declared but
   unsupported (typed fail-closed). A base-cartridge declaration takes precedence.

## 3. Non-goals

No bank switching, no lock-on patch ROM (a third chip some boards map only through `$A130F1`), no combining of separate images (the
composite is the user's own single image), no detection of the board from image contents in the build tool, recompiler or runtime (the launcher's narrow recognition is section 5), no translation-time folding of empty-slot reads (an absolute-operand DATA read inside an empty slot stays fail-closed at translation time, as the ADR 0049 mirror rows already are; register-addressed reads answer the empty-slot value at runtime), no Z80 banked view of an empty slot (a Z80 bank read there stays fail-closed with the existing typed bank stop; only 68000 DATA reads answer the empty-slot value), no other cartridge sizes, no
battery persistence (unchanged from ADR 0098).

## 4. Evidence

- `genesis_cartridge_sram_test` (the lock-on extra-memory rule: attached declaration, ROM beneath, base precedence, EEPROM over ROM
  unsupported, other sizes declare nothing) and `genesis_lock_on_cartridge_test` (project-authored images through the production
  driver: the undeclared mirror, the declared empty slot answering `$FF` with no mirror rows, the attached composite's bytes, the
  fail-closed write into the empty slot, the refused 1 MiB declaration, the attached cartridge's SRAM reached by absolute operands with and
  without the declaration) and `sms_native_build_test` (`build --mapper lock_on`: exact name, size refusal, recorded declaration, forwarding to the emitter).
- Two locally held images of the board class (class-level evidence only): the 2 MiB base alone boots to its own title and gameplay
  instead of the attached-cartridge path; the 4 MiB composite runs past the former stop through the save-slot screen into gameplay.

## 5. Launcher recognition and overrides (SEG-122-T009)

The operator superseded the earlier instruction that the launcher never infers lock-on hardware: a consumer should import the game, build it and play it without knowing what a lock-on cartridge is.

Three layers, three rules:

- **The build tool never guesses.** `segarecomp build` still acts only on an explicit `--mapper lock_on`. The hardware support stays game-agnostic; no title knowledge exists below the launcher.
- **The launcher may select the explicit declaration from conservative, documented ROM recognition.** `apps/segarecomp-launcher/cartridge_recognition.cpp` is the one implementation, shared by the window, `--build` and `--import`. It
  reads the image's own header fields and returns standard, lock-on or ambiguous. The narrow idea follows Genesis Plus GX (`core/cart_hw/md_cart.c`), which recognizes this board from the header title rather than the image length; the launcher is stricter because the result selects a build declaration.
- **The user outranks both.** Advanced build options offers Automatic (recommended), Standard cartridge and Lock-on pass-through; headless `--mapper lock_on|standard` does the same. `standard` is the launcher's word for "no declaration" and is never forwarded. An explicit choice survives reimports, restarts and rescans.

| Image | Evidence required | Result |
| --- | --- | --- |
| Sonic & Knuckles | exactly 2 MiB; both title fields and the product code name it; the header declares one 2 MiB ROM; the header checksum holds | lock-on |
| Sonic & Knuckles + Sonic 3 | exactly 4 MiB; the first 2 MiB as above; the second 2 MiB has its own Genesis header naming Sonic 3 in both title fields and the product code, one declared 2 MiB ROM, and its own valid checksum | lock-on |
| Resembles the base (a title with both names, or the product code) but anything above fails: other size, disagreeing fields, failed checksum, another or no attached cartridge | none | ambiguous: standard behavior, the reason is shown, the user may choose Lock-on |
| Anything else, at any size, including an ordinary image with a second apparent header | none | standard |

Never consulted: file name, library title, ZIP name, a generic SEGA header alone, a second header without a verified base, an installed patch. There is no digest list, title database or region list.

**Persistence.** `LibraryEntry.mapper` holds the effective declaration; the one added member, `mapper_explicit`, is written only when the user chose (so `mapper` empty with it means an explicit Standard). A library entry from an earlier release is examined once per session when its original ROM is available and its digest matches; a missing or changed file leaves the stored declaration alone. Only an image whose header could change the outcome is read whole.

**Identity.** The declaration joins the cache key (`:mapper=lock_on`), `metadata.json`, the older-build check and the exact-reuse check; a build recording another declaration is never reused, in either direction, and Game Details says when that is why a rebuild is needed. An ordinary game's key, command and metadata are byte-for-byte unchanged. The original ROM stays the library identity: the mapper is a build option, not a separate game. A patched variant takes the declaration from the entry and never influences it.
