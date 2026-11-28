# Mod sets (developer preview)

A **mod set** is an ordered list of up to 16 schema 3 mods for one platform and one exact original image. This preview **checks, composes and identifies** a set. It does **not** build or run a game with more than one mod yet ([support matrix](mod-support-matrix.md)). Decisions: [ADR 0326](../decisions/0326-ordered-mod-sets-verified-composition-and-relations.md).

```text
segarecomp mod set check <dir|package.segapatch>... [--rom <original>]       relations; with --rom also composition
segarecomp mod set apply <dir|package.segapatch>... --rom <original> --out <composed-image>
segarecomp mod set save --library <dir> --name <set-name> --identity <package-identity>...   installed packages, in set order
segarecomp mod set show --library <dir> --name <set-name>
segarecomp mod set variant --library <dir> --name <set-name> --rom <original> [--cc <compiler> --sdk-dir <platforms-dir>]
```

A refusal prints `class` and `error` (the first violation) and, for set rules, `violations`: every violated rule with its class, the mod ids concerned and a message.

## Declaring requirements

```toml
[[dependency]]
id = "base-lib"
version = ">=1.2.0 <2.0.0"      # optional: every version when absent

[[incompatibility]]
id = "old-hud"
```

A range is 1 to 4 comparators (`=`, `>=`, `>`, `<=`, `<`) separated by one space; all must hold. Nothing is resolved or downloaded: you choose the set, and it is checked. A required mod may stand anywhere in the set. A package with an unmet requirement cannot be built alone either.

## What composes

| Combination | Result |
| --- | --- |
| Typed edits and injected code writing different bytes (adjacent is fine) | composed; one header repair over the result |
| Any two mods writing the same byte, even the same value | `E_SET_EDIT_OVERLAP` |
| A mod whose expected value is what another mod writes | `E_EDIT_EXPECTED_MISMATCH` (assertions always use the unmodified original) |
| A `bps-image` mod with any other image-changing or hooking mod | `E_SET_BPS_CONFLICT` |
| A `bps-image` mod with mods that change no bytes and declare no hook | composed (the BPS target) |
| A write over another mod's hooked function entry | `E_SET_HOOK_SITE` |
| Master System injected code that is admitted alone but not together | `E_SET_GUEST_ADMISSION` |
| `header_repair = "none"` with an image-changing mod that does not say `none` | `E_SET_HEADER_POLICY` |
| Another image or mapper declaration; two platforms; one mod id twice | `E_SET_SOURCE`, `E_SET_PLATFORM`, `E_SET_DUPLICATE_MOD` |

**BPS-only packages** are treated conservatively because a BPS patch does not say which bytes it owns. To make a mod composable, express its changes as [typed ROM edits](mod-projects.md#rom-edits-and-symbols).

Composition proves that the writes do not collide. It does **not** prove that the mods work together: two mods with disjoint bytes can still break each other's behaviour. Two mods hooking the same function are not judged yet.

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
