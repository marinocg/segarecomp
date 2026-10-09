# NMOS Z80 ISA decode/legality/timing metadata; links only against segarecomp::base (ADR 0059).
add_library(segarecomp_cpu_z80
  src/forms.cpp
  src/decode.cpp
  src/effects.cpp
)
add_library(segarecomp::cpu_z80 ALIAS segarecomp_cpu_z80)
target_link_libraries(segarecomp_cpu_z80 PUBLIC segarecomp::base)
target_include_directories(segarecomp_cpu_z80 PUBLIC include)
segarecomp_enable_warnings(segarecomp_cpu_z80)
