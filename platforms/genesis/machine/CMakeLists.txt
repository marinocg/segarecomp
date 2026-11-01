# Genesis address-space/frontend composition over reusable CPU and device libraries.
add_library(segarecomp_machine_genesis
  src/address_space.cpp
  src/cartridge_sram.cpp
  src/frontend.cpp
  src/hybrid_admission.cpp
  src/ml_region_producer.cpp
  src/ml_region_gen2_producer.cpp
  src/ml_admission.cpp
  src/reachability_challenger.cpp
  src/startup.cpp
)
add_library(segarecomp::machine_genesis ALIAS segarecomp_machine_genesis)
target_link_libraries(segarecomp_machine_genesis PUBLIC segarecomp::recompiler segarecomp::cpu_m68k segarecomp::device_genesis)
target_include_directories(segarecomp_machine_genesis PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_genesis)
target_link_libraries(segarecomp_machine_genesis PUBLIC segarecomp::media)
# SEG-047 (ADR 0096): vendored zlib 1.3.1 deflate subset for the frozen b_zlib feature; private, and the producer must never be
# compiled with floating-point contraction or fast-math (exact folded-scorer determinism).
target_link_libraries(segarecomp_machine_genesis PRIVATE segarecomp_zlib)
if(MSVC)
  # cl.exe / clang-cl: /fp:precise (no fast-math). MSVC is neither a supported nor a tested toolchain for this evaluator (CI and releases use clang on Windows); a
  # contraction would be detected by the bitwise golden digests, not prevented here.
  set_source_files_properties(src/ml_region_gen2_producer.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()
if(NOT MSVC)
  set_source_files_properties(src/ml_region_producer.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
  # SEG-049-T003 (ADR 0102): the gen2 binary32 evaluator has the same contract (bit-identical logits on every IEEE-754 host), and is always optimized (-O2) so the
  # Debug developer/CI builds can still evaluate synthetic fixtures quickly; optimization cannot change IEEE results without fast-math/contraction.
  set_source_files_properties(src/ml_region_gen2_producer.cpp PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-fast-math")
endif()

# SEG-028-T005 (ADR 0077): consumer-route ADR 0049 alias preparation (stop record, verbatim-copy proposal, bounded loop). Byte
# comparison only: no CPU, emitter or device dependency.
add_library(segarecomp_machine_genesis_copy_alias src/m68k_copy_alias.cpp)
add_library(segarecomp::machine_genesis_copy_alias ALIAS segarecomp_machine_genesis_copy_alias)
target_include_directories(segarecomp_machine_genesis_copy_alias PUBLIC include)
target_link_libraries(segarecomp_machine_genesis_copy_alias PUBLIC segarecomp::base)
segarecomp_enable_warnings(segarecomp_machine_genesis_copy_alias)

# SEG-032-T003 (ADR 0073): the materialized Z80 images of a Genesis program (content hash, activation signature, deterministic
# registry, RAM-backed ImageSet and the registry translation unit). A separate library: it needs only the Z80 emitter and the
# hash, never the 68000 side.
add_library(segarecomp_machine_genesis_z80 src/z80_images.cpp src/z80_materialization.cpp)
add_library(segarecomp::machine_genesis_z80 ALIAS segarecomp_machine_genesis_z80)
target_link_libraries(segarecomp_machine_genesis_z80 PUBLIC segarecomp::codegen_c11_z80 segarecomp::codegen_c11_z80_image segarecomp::media
  segarecomp::machine_genesis_copy_alias segarecomp::cpu_z80)
target_include_directories(segarecomp_machine_genesis_z80 PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_genesis_z80)
