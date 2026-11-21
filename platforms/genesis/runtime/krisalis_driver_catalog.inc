/*
 * Verified Krisalis driver images (ADR 0317). One line per uploaded Z80 image the native replacement may stand in for:
 *   KRISALIS_VERIFIED_IMAGE(driver, revision, verification, activation-signature)
 * `revision` is the published driver revision whose behaviour the image has (docs/architecture/genesis-native-krisalis.md section 3); `verification` is the strongest
 * evidence tier reached for THIS image and THIS replacement version: "build_qualified" (identity and address map checked; compiles and runs) or "reference_checked" (additionally
 * compared operation by operation against an independent execution of the original driver). It never says that a play session sounded right. The activation signature is
 * the lowercase-hex SHA-256 the runtime already computes for every Z80 image epoch (genesis_z80_activation_signature); it covers the driver bytes AND the voice bank the
 * game uploads with them, so it identifies one exact upload. No title is compared anywhere. KRISALIS_CATALOG_REVISION(n) is bumped whenever an image is added, removed or
 * re-classified and is part of the build's audio identity. Read by the C runtime (compiled in) and by the build tool.
 */
KRISALIS_CATALOG_REVISION(1)
KRISALIS_VERIFIED_IMAGE("krisalis", "1", "build_qualified", "cb738bcff6f8d61e6144aa4bf0be8e105a02eee9d3aef9a137d25c7f0c4ad774")
KRISALIS_VERIFIED_IMAGE("krisalis", "3", "build_qualified", "ea33c5f08477725778c9b524fd0d432fc7e1a0ef3fb1b372713e6419a86b4a1c")
KRISALIS_VERIFIED_IMAGE("krisalis", "3", "build_qualified", "6b424b975219d64a0d1416760184abb49df13491b3765a6d109e963887d7e04a")
KRISALIS_VERIFIED_IMAGE("krisalis", "3", "build_qualified", "c164e951bfa7ea9888b805595c15317dcdaacf822e43f5e4eb6eabf5e780b7bb")
