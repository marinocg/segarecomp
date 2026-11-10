/*
 * Verified GEMS driver images (ADR 0291). One line per image the native replacement may stand in for:
 *   GEMS_VERIFIED_IMAGE(driver, version, verification, activation-signature)
 * `verification` is the strongest evidence tier reached for THIS image and THIS replacement version: "build_qualified" (identity, address map and
 * protocol checked; compiles and runs) or "reference_checked" (additionally compared operation by operation against an independent reference
 * implementation, docs/testing/genesis-native-gems-validation.md). It never says that a particular play session sounded right.
 * The activation signature is the lowercase-hex SHA-256 the runtime already computes for every Z80 image epoch
 * (genesis_z80_activation_signature: the 68000-written extents of the sound RAM and their bytes). It identifies the exact driver bytes the
 * game uploads; it is not title information and no title is compared anywhere. An image is added only after its address map, tables and
 * protocol were checked against the published driver source and the replacement was validated on it (docs/architecture/genesis-native-gems.md).
 * This file is read by the C runtime (compiled in) and by the build tool (the `--sound-driver gems` identity check): one source of truth.
 */
GEMS_VERIFIED_IMAGE("gems", "2.5", "reference_checked", "f68e547d7dec54d2e00a4f292c1986a07d6bc8de14cc417e5e376b93a9f86cf7")
