#pragma once

// `segarecomp sega-cd ...` (SEG-062): Sega CD disc inspection and generated-native program emission. Orchestration only; the disc
// reader is libs/media and every machine decision is platforms/sega-cd/machine.
inline constexpr const char *segarecomp_sega_cd_usage =
    "  segarecomp sega-cd inspect --cue <disc.cue> [--expect-identity <sha256>] [--sector-digests] [--iso-tree] [--modules <list>]\n"
    "  segarecomp sega-cd emit --cue <disc.cue> --out <program.c> [--expect-identity <sha256>] [--modules <list>]\n"
    "      (compile <program.c> with platforms/sega-cd/runtime/*.c and platforms/genesis/runtime/{runtime,vdp_render,frame_export}.c;\n"
    "       run it with --disc-dir <dir> --instruction-budget <n>)\n";

int segarecomp_sega_cd_command(int argc, char **argv);
