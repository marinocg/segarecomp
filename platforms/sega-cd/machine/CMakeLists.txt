# Generation-time Sega CD composition (ADR 0126/0128): boot-image producer over libs/media disc ingestion, and the emission of the
# two-processor generated-native program over the generic M68K lowering. It depends on no other machine's code.
add_library(segarecomp_machine_sega_cd
  src/boot_images.cpp
)
add_library(segarecomp::machine_sega_cd ALIAS segarecomp_machine_sega_cd)
target_link_libraries(segarecomp_machine_sega_cd PUBLIC segarecomp::media segarecomp::recompiler)
target_include_directories(segarecomp_machine_sega_cd PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_sega_cd)
