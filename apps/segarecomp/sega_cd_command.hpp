#pragma once

// `segarecomp sega-cd ...` (SEG-062): Sega CD disc inspection and generated-native program emission. Orchestration only; the disc
// reader is libs/media and every machine decision is platforms/sega-cd/machine.
inline constexpr const char *segarecomp_sega_cd_usage =
    "  segarecomp sega-cd inspect --cue <disc.cue> [--expect-identity <sha256>] [--sector-digests]\n";

int segarecomp_sega_cd_command(int argc, char **argv);
