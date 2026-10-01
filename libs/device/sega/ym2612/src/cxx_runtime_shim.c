/* SEG-032-T007 (ADR 0074): the whole C++ runtime a `-fno-exceptions -fno-rtti` ymfm YM2612 needs at link time, for links done with a
 * C driver (the bundled Zig toolchain's `zig cc`): operator new/delete and the two diagnostics ymfm's headers can reach. Itanium ABI
 * names (macOS, Linux, Windows-gnu). Not part of the CMake library target: CMake links normally against the platform C++ runtime;
 * the generated-program link sets of `segarecomp build` add this file instead of libc++. */
#include <stddef.h>
#include <stdlib.h>

void *_Znwm(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void *_ZnwmRKSt9nothrow_t(size_t n, const void *nothrow_tag) { (void)nothrow_tag; return malloc(n ? n : 1); }
void *_Znam(size_t n) { return _Znwm(n); }
void _ZdlPv(void *p) { free(p); }
void _ZdlPvm(void *p, size_t n) { (void)n; free(p); }
void _ZdaPv(void *p) { free(p); }
void _ZdaPvm(void *p, size_t n) { (void)n; free(p); }
void __cxa_pure_virtual(void) { abort(); }
void _ZNSt3__122__libcpp_verbose_abortEPKcz(const char *format, ...) { (void)format; abort(); }
