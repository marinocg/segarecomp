/* SEG-032-T007 (ADR 0074): the whole C++ runtime a `-fno-exceptions -fno-rtti` ymfm YM2612 needs at link time, for links done with a
 * C driver (the bundled Zig toolchain's `zig cc`): operator new/delete and the two diagnostics ymfm's headers can reach. Itanium ABI
 * names (macOS, Linux, Windows-gnu). Not part of the CMake library target: CMake links normally against the platform C++ runtime;
 * the generated-program link sets of `segarecomp build` add this file instead of libc++. */
#include <stddef.h>
#include <stdlib.h>

/* The size parameter mangles as `m` (unsigned long) where size_t is `unsigned long` and as `y` (unsigned long long) on LLP64 Windows. */
#if defined(_WIN64)
#define SHIM_NEW _Znwy
#define SHIM_NEW_NOTHROW _ZnwyRKSt9nothrow_t
#define SHIM_NEW_ARRAY _Znay
#define SHIM_DELETE_SIZED _ZdlPvy
#define SHIM_DELETE_ARRAY_SIZED _ZdaPvy
#else
#define SHIM_NEW _Znwm
#define SHIM_NEW_NOTHROW _ZnwmRKSt9nothrow_t
#define SHIM_NEW_ARRAY _Znam
#define SHIM_DELETE_SIZED _ZdlPvm
#define SHIM_DELETE_ARRAY_SIZED _ZdaPvm
#endif

void *SHIM_NEW(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void *SHIM_NEW_NOTHROW(size_t n, const void *nothrow_tag) { (void)nothrow_tag; return malloc(n ? n : 1); }
void *SHIM_NEW_ARRAY(size_t n) { return SHIM_NEW(n); }
void _ZdlPv(void *p) { free(p); }
void SHIM_DELETE_SIZED(void *p, size_t n) { (void)n; free(p); }
void _ZdaPv(void *p) { free(p); }
void SHIM_DELETE_ARRAY_SIZED(void *p, size_t n) { (void)n; free(p); }
void __cxa_pure_virtual(void) { abort(); }
void _ZNSt3__122__libcpp_verbose_abortEPKcz(const char *format, ...) { (void)format; abort(); }

/* libstdc++ (GCC/Linux native toolchains) headers reach these out-of-line helpers from std::vector/std::allocator; libc++ (Zig) does not.
 * Exceptions are disabled, so each one is a hard stop. Harmless extra symbols when libc++ is the runtime. */
void _ZSt17__throw_bad_allocv(void) { abort(); }
void _ZSt28__throw_bad_array_new_lengthv(void) { abort(); }
void _ZSt25__throw_bad_function_callv(void) { abort(); }
void _ZSt20__throw_length_errorPKc(const char *what) { (void)what; abort(); }
void _ZSt19__throw_logic_errorPKc(const char *what) { (void)what; abort(); }
void _ZSt20__throw_out_of_rangePKc(const char *what) { (void)what; abort(); }
void _ZSt24__throw_out_of_range_fmtPKcz(const char *format, ...) { (void)format; abort(); }
void _ZSt16__throw_bad_castv(void) { abort(); }
