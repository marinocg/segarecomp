#pragma once

// `segarecomp sega-cd ...` (SEG-062): Sega CD disc inspection and generated-native program emission. Orchestration only; the disc
// reader is libs/media and every machine decision is platforms/sega-cd/machine.
inline constexpr const char *segarecomp_sega_cd_usage =
    "  segarecomp sega-cd inspect --cue <disc.cue> [--expect-identity <sha256>] [--sector-digests] [--iso-tree] [--modules <list>]\n"
    "  segarecomp sega-cd emit --cue <disc.cue> --out <program.c> [--expect-identity <sha256>] [--modules <list>]\n"
    "      (compile <program.c> with platforms/sega-cd/runtime/*.c and platforms/genesis/runtime/{runtime,vdp_render,frame_export}.c;\n"
    "       run it with --disc-dir <dir> --instruction-budget <n>)\n"
    "  segarecomp sega-cd emit --cue <disc.cue> --bios <image> --out <program.c> [--copy-image main|sub:<base>:<rom-offset>:<length>]...\n"
    "                          [--admit epoch:<pass-identity>:<snapshot> | --admit template:<pass-identity>:main|sub:<pc>:<hex>]...\n"
    "      (SEG-064 BIOS-image profile, ADR 0142; run it with --bios <image> as well; --materialize-dir <dir> makes a build pass)\n";

int segarecomp_sega_cd_command(int argc, char **argv);
