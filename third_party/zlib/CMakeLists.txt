# SEG-047 (ADR 0096): vendored zlib 1.3.1 deflate subset. Third-party C: no project warning flags. Private to the native ML region producer.
add_library(segarecomp_zlib STATIC adler32.c deflate.c trees.c zutil.c segarecomp_crc32.c)
target_include_directories(segarecomp_zlib PUBLIC .)
set_target_properties(segarecomp_zlib PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_definitions(segarecomp_zlib PRIVATE _LARGEFILE64_SOURCE=1 _POSIX_C_SOURCE=200112L)
  target_compile_options(segarecomp_zlib PRIVATE -w)
endif()
