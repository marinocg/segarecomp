#pragma once

// `segarecomp mod apply --cue | verify-disc | disc-build | disc-diff-run`: the Sega CD disc commands of the modding platform (ADR 0328). See mod_disc_command.cpp.

namespace segarecomp_cli {

inline constexpr const char *kModDiscUsage =
    "       segarecomp mod apply <dir|package.segapatch> --cue <original.cue> --out <patched-disc-dir> [--bios <image>]   (sega-cd, data edits only)\n"
    "       segarecomp mod verify-disc <dir|package.segapatch> --cue <original.cue> --patched <patched.cue>\n"
    "       segarecomp mod disc-build <dir|package.segapatch> --cue <original.cue> --bios <image> --out <new-dir> --runtime-dir <source-root> --cc <compiler> --budget <instructions> [sega-cd build options]\n"
    "       segarecomp mod disc-diff-run <disc-build-dir> --cue <original.cue> --bios <image> --budget <instructions> --timeout <seconds>\n";

// argv[2] is the subcommand.
int run_mod_disc_command(int argc, char **argv);

}  // namespace segarecomp_cli
