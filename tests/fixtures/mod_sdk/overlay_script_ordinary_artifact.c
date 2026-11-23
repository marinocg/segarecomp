/* The constants a built program's native_artifact.c carries, hand-written for the overlay-script-ordinary adapter tests. */
const char segarecomp_mod_identity[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char segarecomp_mod_capabilities[] = "overlay.rgba,scenery.words,frame.work_ram";
const char segarecomp_mod_id[] = "overlay-script-ordinary";
/* ids: frame.work_ram 2, frame.vdp 3, scenery.words 5, overlay.rgba 6 */
const unsigned long long segarecomp_mod_caps_required[2] = {(1ULL << 2) | (1ULL << 5) | (1ULL << 6), 0ULL}, segarecomp_mod_caps_optional[2] = {0ULL, 0ULL};
const char segarecomp_mod_effective_sha256[] = "0000000000000000000000000000000000000000000000000000000000000000";
const unsigned long segarecomp_mod_state_schema = 0UL;
const unsigned long segarecomp_mod_range_count = 0UL;
const unsigned long segarecomp_mod_ranges[][4] = {{0, 0, 0, 0}};
