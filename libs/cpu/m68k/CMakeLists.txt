# MC68000 ISA semantics; links only against segarecomp::base, never Genesis code.
add_library(segarecomp_cpu_m68k
  src/decode.cpp
  src/direct_flow.cpp
  src/effective_address.cpp
  src/effects.cpp
  src/ir.cpp
  src/static_discovery.cpp
  src/static_program.cpp
  src/timing.cpp
)
add_library(segarecomp::cpu_m68k ALIAS segarecomp_cpu_m68k)
target_link_libraries(segarecomp_cpu_m68k PUBLIC segarecomp::base)
target_include_directories(segarecomp_cpu_m68k PUBLIC include)
segarecomp_enable_warnings(segarecomp_cpu_m68k)
