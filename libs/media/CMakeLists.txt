add_library(segarecomp_media
  src/disc.cpp
  src/iso9660.cpp
  src/mod_adapter.cpp
  src/mod_audio.cpp
  src/mod_edits.cpp
  src/mod_guest.cpp
  src/mod_hooks.cpp
  src/mod_geometry.cpp
  src/mod_memory.cpp
  src/mod_package.cpp
  src/mod_project.cpp
  src/mod_variant.cpp
  src/patch_archive.cpp
  src/patch_bps.cpp
  src/patch_package.cpp
  src/rom.cpp
  src/sha256.cpp
)
add_library(segarecomp::media ALIAS segarecomp_media)
target_link_libraries(segarecomp_media PUBLIC segarecomp::base)
target_include_directories(segarecomp_media PUBLIC include)
segarecomp_enable_warnings(segarecomp_media)
