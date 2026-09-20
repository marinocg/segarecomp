# Reusable Genesis-family device policy (consumable by future Sega CD / 32X platforms).
add_library(segarecomp_device_genesis
  src/controller_io.cpp
)
add_library(segarecomp::device_genesis ALIAS segarecomp_device_genesis)
target_link_libraries(segarecomp_device_genesis PUBLIC segarecomp::cpu_m68k)
target_include_directories(segarecomp_device_genesis PUBLIC include)
segarecomp_enable_warnings(segarecomp_device_genesis)
