#pragma once

// `segarecomp build`: ROM -> generated C (the existing emit route) -> host C compiler -> native executable.
// Consumer/launcher-facing driver; it owns no recompilation logic, only orchestration of the same
// `emit-general-startup-bridge-c` route the developer bridge tool uses.
inline constexpr const char *segarecomp_build_usage =
    "  segarecomp build --rom <image> --output <dir> --cc <compiler> [--cc-arg <arg>]... [--link-arg <arg>]...\n                   [--cxx <compiler> [--cxx-arg <arg>]...] [--keep-work 1]\n                   --runtime-dir <dir>\n"
    "                   [--sdl3-include <dir> --sdl3-lib <dir>] [--optimize <0|1|2>] [--runtime-optimize <0|1|2>] [--jobs <n>]\n"
    "                   [--platform <genesis|master-system>] [--mapper <sega|rom_only|lock_on>] [--mapper-manifest <file>]\n"
    "                   (Genesis: --mapper lock_on declares a lock-on pass-through cartridge, ADR 0268; never inferred)\n"
    "                   [--admission-plan <file>] (Genesis M68K hybrid admission candidate, SEG-031; broad by default)\n"
    "                   [--aot-policy <compatibility|optimized>] (default compatibility; optimized = native selective admission, broad fallback)\n"
    "                   [--aot-model <v1|gen2>] (EXPERIMENTAL opt-in, only with --aot-policy optimized; default v1 = the SEG-047 producer)\n"
    "                   (Master System: optimized = EXPERIMENTAL native D512HP selective admission, SEG-057; any failure builds the broad program;\n"
    "                    --aot-model is a Genesis option and is refused for Master System; a start the build omitted stops the program with a typed outcome)\n"
    "                   [--patch-package <file.segapatch> --patch-identity <sha256>] (Genesis viewer builds, SEG-124: a trusted native-source package the user approved by identity;\n"
    "                    the build re-verifies the package, extracts and compiles its provider against the public SDK only and links it into this program; --rom is the image the package declares)\n"
    "                   [--performance-report <file.json>] (SEG-125: write a versioned build performance report; off by default; the build is otherwise unchanged)\n"
    "                   (Master System: --runtime-dir is <root>/platforms/master-system; the mapper is never inferred)\n";

int run_cli(int argc, char **argv);
int segarecomp_build_command(int argc, char **argv);
