# Mod sets (developer preview)

A **mod set** is an ordered list of up to 16 schema 3 mods for one platform and one exact original image. This developer preview checks, composes, identifies and **builds** a set for Genesis and Master System games ([support matrix](mod-support-matrix.md)). Sega CD sets of more than one mod are refused (`E_SET_PLATFORM_UNSUPPORTED`). The launcher cannot select or play a set yet. Decisions: [ADR 0326](../decisions/0326-ordered-mod-sets-verified-composition-and-relations.md), [ADR 0327](../decisions/0327-mod-set-programs-dispatch-hook-ownership-and-save-namespaces.md).

```text
segarecomp mod set check <dir|package.segapatch>... [--rom <original>]       relations; with --rom also composition
segarecomp mod set apply <dir|package.segapatch>... --rom <original> --out <composed-image>
segarecomp mod set save --library <dir> --name <set-name> --identity <package-identity>...   installed packages, in set order
segarecomp mod set show --library <dir> --name <set-name>
segarecomp mod set variant --library <dir> --name <set-name> --rom <original> [--cc <compiler> --sdk-dir <platforms-dir>]
segarecomp mod set build (<dir|package.segapatch>... | --library <dir> --name <set-name>) --rom <original> --out <build-dir> --cc <compiler> --sdk-dir <platforms-dir>
                         [--approve <package-identity>]... [--sdl3-include <dir> --sdl3-lib <dir>] [--optimize 0|1|2] [--runtime-optimize 0|1|2] [--jobs <n>]
```

## Building and running a set

`mod set build` writes one program to `<build-dir>`. No file the build writes or removes there may be an input: if the original image or a mod file is one of those files (under any path or link) or sits in a folder the build replaces, the build refuses with `E_OUTPUT_IS_INPUT` and changes nothing. Each native member needs `--approve <its package identity>` (a stored set uses the library's recorded approvals): native code is host code and is **not sandboxed**. Run the headless program with an explicit bound (Genesis: `--instruction-budget`; Master System: `--frames` and `--cycle-budget`).

**What a built set supports** — anything else is refused before a program exists:

| Rule | Refusal |
| --- | --- |
| Members: typed edits, injected code, a BPS under the composition rules, Luau scripts, native ABI 2 providers; any number of each | each member's own class |
| At most **one** member draws (overlay, virtual sprites, scenery, exact frame, Master System picture transforms, or a geometry declaration of a script or native member) | `E_SET_PRESENTATION_CONFLICT` |
| A data-only member declares no geometry (a built set would not apply it) | `E_SET_GEOMETRY_UNSUPPORTED` |
| No member uses the audio API (`audio.command.send`, `audio.event.read`) | `E_SET_AUDIO_UNSUPPORTED` |
| Native members must not define the same external symbol (make helpers `static`) | `E_SET_NATIVE_LINK` |
| Function hooks (Genesis native only): a replaced function has one owner; `before`/`after` hooks may share it, with the same anchor | `E_SET_HOOK_OWNER`, `E_SET_HOOK_ANCHOR` |

**Order.** Every phase runs for all mods in set order before the next phase. A mod sees the guest memory earlier mods committed. Controller input is sampled once and passed from mod to mod; from `post-input` on, every mod sees the final pad. A mod that faults is disabled **alone**; what it committed earlier remains.

**Saves.** A state restores only into the same set (same members, order, builds and image); another is refused. The played program keeps its battery memory and state slots in `<save folder>/sets/<set identity>/`, whatever `SEGARECOMP_SAVE_VARIANT` says, so sets never share or overwrite each other's files. Genesis recordings (`--record` / `--replay`), headless or played, carry the set's identity too: a recording replays verified in either mode of the same set and is refused by another set. Each mod's saved values are its own: two mods may use the same keys. Mods have no separate user settings in this release.

**Outputs stay outside the inputs.** `mod set apply --out` (like `mod apply` and `mod bps`) refuses with `E_OUTPUT_IS_INPUT` an output, or its `<out>.partial` staging file, that is the original image or a package file, lies inside a member's project folder, or links to a file there. Nothing is written.

A refusal prints `class` and `error` (the first violation) and, for set rules, `violations`: every violated rule with its class, the mod ids concerned and a message.

## Declaring requirements

```toml
[[dependency]]
id = "base-lib"
version = ">=1.2.0 <2.0.0"      # optional: every version when absent

[[incompatibility]]
id = "old-hud"
```

A range is 1 to 4 comparators (`=`, `>=`, `>`, `<=`, `<`) separated by one space; all must hold. Nothing is resolved or downloaded: you choose the set, and it is checked. A required mod may stand anywhere in the set. A package with an unmet requirement cannot be built or run alone either (`mod variant`, `segarecomp build`, `mod diff-run`, `mod test`, `mod session`). `mod apply` and `mod bps` still export that one mod's image or patch.

## What composes

| Combination | Result |
| --- | --- |
| Typed edits and injected code writing different bytes (adjacent is fine) | composed; one header repair over the result |
| Any two mods writing the same byte, even the same value | `E_SET_EDIT_OVERLAP` (one entry per pair of mods, with the number of overlapping ranges) |
| Genesis injected Z80 code of two mods assembled for the same sound-RAM address | `E_SET_EDIT_OVERLAP` |
| Disjoint blocks in a shared free range, or an edit in unused bytes of another mod's free range | composed |
| A mod whose expected value is what another mod writes | `E_EDIT_EXPECTED_MISMATCH` (assertions always use the unmodified original) |
| A `bps-image` mod with any other image-changing or hooking mod | `E_SET_BPS_CONFLICT` |
| A `bps-image` mod with mods that change no bytes and declare no hook | composed (the BPS target) |
| A write over another mod's hooked function entry | `E_SET_HOOK_SITE` |
| Two mods hooking one function where either replaces it | `E_SET_HOOK_OWNER` |
| Two mods with `before`/`after` hooks on one function | composed |
| Master System injected code that is admitted alone but not together | `E_SET_GUEST_ADMISSION` |
| `header_repair = "none"` with an image-changing mod that does not say `none` | `E_SET_HEADER_POLICY` |
| Another image or mapper declaration; two platforms; one mod id twice | `E_SET_SOURCE`, `E_SET_PLATFORM`, `E_SET_DUPLICATE_MOD` |

**BPS-only packages** are treated conservatively because a BPS patch does not say which bytes it owns. To make a mod composable, express its changes as [typed ROM edits](mod-projects.md#rom-edits-and-symbols).

Composition proves that the writes do not collide. It does **not** prove that the mods work together: two mods with disjoint bytes can still break each other's behaviour, and two mods may write the same guest memory at run time (the later one wins).

## Identity and reuse

`mod set variant` prints the composite components and key. Changing the members, their order, any member package, the original image or mapper, or the platform's capability table changes the key. The set's name does not: an identical set reuses the earlier record (`"cache":"hit"`). A one-member set has exactly the identity and cache entry of `mod variant` for that mod. Native members each need their own approval and, on a rebuild, a compiler. Like `mod variant`, this is a validated record cache, not a game executable.

## Stored definitions

`<library>/mod-sets/<name>.modset` (the launcher library keeps one folder per game: `mod-sets/<game sha256>/`):

```text
segarecomp-modset=1
platform=genesis
member=<package identity>
member=<package identity>
```
