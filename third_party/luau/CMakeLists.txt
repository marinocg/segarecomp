# SEG-137-T001 (ADR 0284, luau-trust-policy.md): the pinned Luau VM + compiler, built reproducibly from the vendored sources in this directory (no network at configure or build time).
# Provenance and the retained/omitted file list: PIN.txt. License: LICENSE.txt (MIT, Roblox) and lua_LICENSE.txt (Lua, PUC-Rio), also shipped in the product package.
#
# Only the pieces the policy needs are built: Common, Ast, Bytecode, Compiler and the VM core. NOT vendored at all: CodeGen (native code generation / JIT), Analysis, Config, Require, Inliner,
# the CLI and the tests. Of the vendored VM sources, linit.cpp (luaL_openlibs), loslib.cpp (`os`), ldblib.cpp (`debug`) and lcorolib.cpp (`coroutine`) are deliberately not compiled: the host
# opens each allowed library by name and the disallowed libraries do not exist in the binary.
# Third-party C++: no project warning flags and no project warning-as-error.
set(_luau_root "${CMAKE_CURRENT_SOURCE_DIR}")
set(_luau_dirs Common Ast Bytecode Compiler VM)
set(_luau_sources)
foreach(_d ${_luau_dirs})
  file(GLOB _s CONFIGURE_DEPENDS "${_luau_root}/${_d}/src/*.cpp")
  list(APPEND _luau_sources ${_s})
endforeach()
list(FILTER _luau_sources EXCLUDE REGEX "/VM/src/(linit|loslib|ldblib|lcorolib)\\.cpp$")
list(SORT _luau_sources)

add_library(segarecomp_luau STATIC ${_luau_sources})
add_library(segarecomp::luau ALIAS segarecomp_luau)
target_include_directories(segarecomp_luau SYSTEM PUBLIC
  "${_luau_root}/Common/include" "${_luau_root}/Ast/include" "${_luau_root}/Bytecode/include" "${_luau_root}/Compiler/include" "${_luau_root}/VM/include")
target_include_directories(segarecomp_luau PRIVATE "${_luau_root}/VM/src" "${_luau_root}/Bytecode/src" "${_luau_root}/Compiler/src")
target_compile_features(segarecomp_luau PUBLIC cxx_std_17)
set_target_properties(segarecomp_luau PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Native code generation is not built; this macro lets the host's own sources and tests state (and a static_assert prove) that it is off.
target_compile_definitions(segarecomp_luau PUBLIC SEGARECOMP_LUAU_CODEGEN=0)
if(MSVC)
  target_compile_options(segarecomp_luau PRIVATE /w /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(segarecomp_luau PRIVATE -w)
endif()
