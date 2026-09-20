# Genesis address-space/frontend composition over reusable CPU and device libraries.
add_library(segarecomp_machine_genesis
  src/address_space.cpp
  src/frontend.cpp
  src/startup.cpp
)
add_library(segarecomp::machine_genesis ALIAS segarecomp_machine_genesis)
target_link_libraries(segarecomp_machine_genesis PUBLIC segarecomp::recompiler segarecomp::cpu_m68k segarecomp::device_genesis)
target_include_directories(segarecomp_machine_genesis PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_genesis)
target_link_libraries(segarecomp_machine_genesis PUBLIC segarecomp::media)
