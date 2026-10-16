# ADR 0130: Sega CD BIOS profiles and the SEG-064 go/no-go gate

- Status: Accepted (SEG-062-T001). The gate is answered by SEG-064-T001.
- Related: contract section 9, ADR 0126, ADR 0127.

## Decision

1. **Two profiles.**
   - *Synthetic post-BIOS fixture profile* (SEG-062): no BIOS byte exists anywhere; the runtime establishes the
     documented post-load placement (IP in main work RAM, SP in PRG RAM) from the external disc and starts the main
     program. Only project-authored discs boot under it; the security-code prefix of a real boot area is never reproduced.
   - *Authentic BIOS profile* (SEG-064): a user-supplied BIOS image (ignored `games/` path, never committed, never
     derived from) is statically recompiled for the main and sub processors and boots unmodified discs.
2. **Legal-input rule.** No BIOS bytes, disassembly, hashes-as-content or derived tables enter either repository; tests
   use only synthetic material; the BIOS is identified by SHA-256 supplied by the user.

## Go/no-go gate for SEG-064-T001 (falsifiable)

Answer each with evidence from a user-supplied BIOS:

1. *Static discoverability*: from the main and sub reset vectors and the documented entry tables, the current M68K
   static path admits >= 95 % of the BIOS code bytes executed while booting a synthetic disc to the IP entry, with every
   remaining reached instruction a typed stop (not a crash). NO-GO if computed control in the BIOS needs a dispatch key
   beyond ADR 0127.
2. *Live routines during play*: list the BIOS routines a title calls after boot (sub-CPU CDD/CDC services, main-CPU
   jump table); GO if they are reached only through compiled entries.
3. *Native services*: if a routine cannot be compiled (self-modifying, timing loop on unmodelled hardware), a narrow
   native implementation is allowed only behind a typed boundary with an independent oracle; more than three such
   routines is NO-GO for the authentic profile (fall back to synthetic + documented service shims as a separate decision).
