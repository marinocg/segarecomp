# ADR 0131: ML reuse for Sega CD

- Status: Accepted (SEG-062-T001). No training is authorized.
- Related: ADR 0103 (admission authority and model lifecycle), ADR 0109 (M3 adoption), ADR 0126/0127.

## Decision

1. The frozen M3 M68K region model is reused only for **code-region discovery inside an established executable image**
   (an IP, SP or module whose bytes and mappings an image producer already fixed). It never decides which images
   exist or execute: that is the disc/overlay problem of ADR 0127, answered by disc structure and live-byte identity.
2. SEG-062 uses no model: its images are small and fully discovered by static traversal.
3. **Evidence gate (SEG-063-T002)**: on synthetic and user-supplied module images, compare frozen M3 against static
   traversal alone (recall of executed instructions, false admissions, build cost). Adopt only if recall improves with
   zero false admissions that change behaviour.
4. **If retraining is ever proposed**, it needs: a corpus of user-supplied discs (never committed), labels from
   generated-native execution coverage, a title-level holdout, leakage control across shared engines, the static
   traversal baseline, and a model identity (SHA-256 of weights) recorded per ADR 0103. Not authorized here.
