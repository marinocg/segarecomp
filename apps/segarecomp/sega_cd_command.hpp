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
    "                          [--partition-window <bytes>] [--disc-image main|sub:<system-area-offset>:<base>:<length>]...\n"
    "                          [--application-entry <address>]\n"
    "      (SEG-064 BIOS-image profile, ADR 0142; run it with --bios <image> as well; --materialize-dir <dir> makes a build pass;\n"
    "       --partition-window writes one translation unit per processor PC window next to <program.c>, listed in the report;\n"
    "       --disc-image admits a disc system-area extent the BIOS places in a processor's RAM as an executable image (SEG-064-T004);\n"
    "       --application-entry ends the run, with the typed stop application_handoff, at the first main instruction there)\n";

int segarecomp_sega_cd_command(int argc, char **argv);
