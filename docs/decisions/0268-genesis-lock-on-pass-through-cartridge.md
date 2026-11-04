# ADR 0268: Declared Genesis lock-on pass-through cartridge

- Status: Accepted.
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

1. **Declared, never inferred.** `segarecomp build --mapper lock_on` (Genesis only; the emitter's `--genesis-lock-on`) declares the
   board. Nothing about a title, a checksum or a header text selects it; without the declaration every image keeps ADR 0049/0098
   behavior byte for byte. Any other Genesis mapper declaration is still refused (`MAPPER_NOT_APPLICABLE`).
2. **Image shapes.** A declared lock-on image is exactly 2 MiB (the base alone: an empty slot) or exactly 4 MiB (the base followed by
   the attached cartridge). Any other size is refused at build time (`LOCK_ON_IMAGE_SIZE`).
3. **No mirror into the slot.** The base image is never mirrored into `$200000-$3FFFFF` (the slot is not a decode alias of the base
   chip). A 4 MiB composite is mapped linearly, as before.
4. **Empty slot.** With a 2 MiB image a DATA read wholly inside the slot answers `$FF` in every byte
   (`SEGARECOMP_GENESIS_EMPTY_SLOT_BYTE`). On hardware nothing drives the data bus there (open bus, whose value depends on the bus
   history); the fixed value is a deterministic project compatibility policy, not reconstructed open bus. The one property software
   can rely on, that no cartridge header is visible in an empty slot, holds. Writes and instruction fetches in the slot stay
   fail-closed.
5. **Attached extra memory.** For a 4 MiB composite whose base header declares no extra memory, the attached cartridge's own header
   (at `$2001B0`) declares it, with its bus addresses unchanged. The extent then lies over the attached ROM, so ADR 0098's
   `$A130F1` map bit selects SRAM or ROM there (never "always mapped"); ADR 0136's EEPROM form over ROM is recorded as declared but
   unsupported (typed fail-closed). A base-cartridge declaration takes precedence.

## 3. Non-goals

No bank switching, no lock-on patch ROM (a third chip some boards map only through `$A130F1`), no combining of separate images (the
composite is the user's own single image), no detection of the board from image contents, no other cartridge sizes, no
battery persistence (unchanged from ADR 0098). The launcher's Genesis build configuration does not yet offer the declaration; the
CLI and headless `--build --mapper lock_on` paths do.

## 4. Evidence

- `genesis_cartridge_sram_test` (the lock-on extra-memory rule: attached declaration, ROM beneath, base precedence, EEPROM over ROM
  unsupported, other sizes declare nothing) and `genesis_lock_on_cartridge_test` (project-authored images through the production
  driver: the undeclared mirror, the declared empty slot answering `$FF` with no mirror rows, the attached composite's bytes, the
  fail-closed write into the empty slot, the refused 1 MiB declaration).
- Two locally held images of the board class (class-level evidence only): the 2 MiB base alone boots to its own title and gameplay
  instead of the attached-cartridge path; the 4 MiB composite runs past the former stop through the save-slot screen into gameplay.
