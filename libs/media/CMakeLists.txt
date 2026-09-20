add_library(segarecomp_media
  src/rom.cpp
)
add_library(segarecomp::media ALIAS segarecomp_media)
target_link_libraries(segarecomp_media PUBLIC segarecomp::base)
target_include_directories(segarecomp_media PUBLIC include)
segarecomp_enable_warnings(segarecomp_media)
