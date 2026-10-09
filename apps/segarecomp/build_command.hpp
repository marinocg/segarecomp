#pragma once

// `segarecomp build`: ROM -> generated C (the existing emit route) -> host C compiler -> native executable.
// Consumer/launcher-facing driver; it owns no recompilation logic, only orchestration of the same
// `emit-general-startup-bridge-c` route the developer bridge tool uses.
inline constexpr const char *segarecomp_build_usage =
    "  segarecomp build --rom <image> --output <dir> --cc <compiler> [--cc-arg <arg>]... [--link-arg <arg>]...\n                   [--cxx <compiler> [--cxx-arg <arg>]...] [--keep-work 1]\n                   --runtime-dir <dir>\n"
    "                   [--sdl3-include <dir> --sdl3-lib <dir>] [--optimize <0|1|2>] [--runtime-optimize <0|1|2>] [--jobs <n>]\n"
    "                   [--platform <genesis|master-system>] [--mapper <sega|rom_only>] [--mapper-manifest <file>]\n"
    "                   [--admission-plan <file>] (Genesis M68K hybrid admission candidate, SEG-031; broad by default)\n"
    "                   [--aot-policy <compatibility|optimized>] (default compatibility; optimized = native selective admission, broad fallback)\n"
    "                   (Master System: --runtime-dir is <root>/platforms/master-system; the mapper is never inferred)\n";

int run_cli(int argc, char **argv);
int segarecomp_build_command(int argc, char **argv);
