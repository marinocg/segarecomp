# Genesis YM2612 (OPN2) device: a host-clocked C ABI over the vendored ymfm core (SEG-032-T007; ADR 0074).
# C++14 inside (ymfm), plain C ABI outside; no platform, runtime or CPU dependency. The Genesis runtime consumes it through
# platforms/genesis/runtime/genesis_audio.c. The shim `src/cxx_runtime_shim.c` is deliberately NOT part of this target (see its header).
add_library(segarecomp_device_ym2612 STATIC src/ym2612.cpp
  third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_ssg.cpp)
add_library(segarecomp::device_ym2612 ALIAS segarecomp_device_ym2612)
target_include_directories(segarecomp_device_ym2612 PUBLIC include)
target_include_directories(segarecomp_device_ym2612 SYSTEM PRIVATE third_party/ymfm)
set_target_properties(segarecomp_device_ym2612 PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON)
# The vendored upstream sources are compiled as upstream ships them (no -Werror on code we do not own); our own file keeps the project warnings.
set_source_files_properties(third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_ssg.cpp
  PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(segarecomp_device_ym2612 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
