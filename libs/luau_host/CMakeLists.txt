# SEG-137-T002/T003 (ADR 0284/0294): the host side of Luau scripting: the trusted bytecode compile step, the provenance check and the restricted per-mod VM. Links the pinned vendored Luau (third_party/luau).
add_library(segarecomp_luau_host
  src/luau_compile.cpp
  src/luau_stored.cpp
  src/luau_vm.cpp
  src/luau_api.cpp
  src/luau_config.cpp
  src/luau_analysis.cpp
  src/luau_session.cpp
  src/luau_phase_provider.cpp
)
add_library(segarecomp::luau_host ALIAS segarecomp_luau_host)
target_link_libraries(segarecomp_luau_host PUBLIC segarecomp::media PRIVATE segarecomp::luau segarecomp::viewer_core)
target_include_directories(segarecomp_luau_host PUBLIC include ${PROJECT_SOURCE_DIR}/platforms/sdk)   # the ABI 2 core header (SegaModProvider) for the phase adapter
target_compile_definitions(segarecomp_luau_host PRIVATE SEGARECOMP_LUAU_PIN="0.746")
segarecomp_enable_warnings(segarecomp_luau_host)
