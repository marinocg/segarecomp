#pragma once

// `segarecomp sega-cd ...` (SEG-062): Sega CD disc inspection and generated-native program emission. Orchestration only; the disc
// reader is libs/media and every machine decision is platforms/sega-cd/machine.
inline constexpr const char *segarecomp_sega_cd_usage =
    "  segarecomp sega-cd inspect --cue <disc.cue> [--expect-identity <sha256>] [--sector-digests] [--iso-tree] [--modules <list>]\n"
    "  segarecomp sega-cd emit --cue <disc.cue> --out <program.c> [--expect-identity <sha256>] [--modules <list>]\n"
    "      (compile <program.c> with platforms/sega-cd/runtime/*.c and platforms/genesis/runtime/{runtime,vdp_render,frame_export}.c;\n"
    "       run it with --disc-dir <dir> --instruction-budget <n>)\n"
    "  segarecomp sega-cd emit --cue <disc.cue> --bios <image> --out <program.c> [--copy-image main|sub:<base>:<rom-offset>:<length>]...\n"
    "                          [--admit epoch:<pass-identity>:<snapshot> | --admit epoch-recipe:<length>:<source>@<dest>[,...] |\n"
    "                           --admit template:<pass-identity>:main|sub:<pc>:<hex>]...\n"
    "                          [--partition-window <bytes>] [--disc-image main|sub:<system-area-offset>:<base>:<length>]...\n"
    "                          [--application-entry <address>]\n"
    "      (SEG-064 BIOS-image profile, ADR 0142; run it with --bios <image> as well; --materialize-dir <dir> makes a build pass;\n"
    "       --partition-window writes one translation unit per processor PC window next to <program.c>, listed in the report;\n"
    "       --disc-image admits a track-1 extent placed in a processor's RAM as an executable image (SEG-064-T004; main: work RAM\n"
    "       or, SEG-071-T001, the 2M Word RAM view $200000-$23FFFF; sub: PRG RAM);\n"
    "       --application-entry ends the run, with the typed stop application_handoff, at the first main instruction there)\n"
    "  segarecomp sega-cd resolve-source --cue <disc.cue> --cpu main|sub --pc <address> --region <file> --region-base <address>\n"
    "                                    [--delivered <first-lba>:<count>[,...]] [--delivered-overflow 1]\n"
    "  segarecomp sega-cd build --cue <disc.cue> --bios <image> --output <dir> --runtime-dir <runtime root> --cc <compiler> [--cc-arg <arg>]...\n"
    "                           [--viewer 1 --sdl3-include <dir> --sdl3-lib <dir>] [--cache-dir <dir>] [--jobs <n>] [--budget <instructions>] [--until-tick <t>]\n"
    "                           [--pass-timeout <seconds>] [--input-script <file>] [--backup-ram <file>] [--optimize 0|1|2] [--link-arg <arg>]...\n"
    "      (SEG-078-T003, ADR 0205: the self-service build - emit, compile, run a build pass, admit the epoch, operand templates and discovered\n"
    "       disc extents, confirm; writes <output>/{program[,program-viewer],status.json,build.log}; progress as @stage/@round/@result lines)\n"
    "  segarecomp sega-cd resolve-source --cue <disc.cue> --cpu main|sub --pc <address> --region <file> --region-base <address>\n"
    "                                    [--delivered <first-lba>:<count>[,...]] [--delivered-overflow 1]\n"
    "      (SEG-078-T003, ADR 0205: find the track-1 span the live RAM window is an exact copy of, among the blocks the build pass's CDC read;\n"
    "       prints the --disc-image extent or a typed refusal; exit 0 resolved, 8 refused)\n"
    "  segarecomp sega-cd discover-modules --cue <disc.cue> [--out <modules.txt>]\n"
    "      (SEG-097-T001, ADR 0206: static discovery of disc-loaded modules from the sub boot image's own CD-load operations, including modules\n"
    "       no run has loaded; writes the module list admitted by `emit --modules`; prints the typed report; exit 0 found, 9 none or incomplete)\n";

int segarecomp_sega_cd_command(int argc, char **argv);
// SEG-078-T003: `segarecomp sega-cd build` (sega_cd_build.cpp).
int segarecomp_sega_cd_build(int argc, char **argv);
