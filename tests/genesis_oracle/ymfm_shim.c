/* SEG-032-T001 (ADR 0074): the whole C++ runtime a -fno-exceptions -fno-rtti ymfm YM2612 needs at link time. Itanium ABI
 * names (macOS/Linux/Windows-gnu); the production copy lives with the T007 device. */
#include <stddef.h>
#include <stdlib.h>
void *_Znwm(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void *_Znam(size_t n) { return _Znwm(n); }
void _ZdlPv(void *p) { free(p); }
void _ZdlPvm(void *p, size_t n) { (void)n; free(p); }
void _ZdaPv(void *p) { free(p); }
void _ZdaPvm(void *p, size_t n) { (void)n; free(p); }
void __cxa_pure_virtual(void) { abort(); }
void _ZNSt3__122__libcpp_verbose_abortEPKcz(const char *format, ...) { (void)format; abort(); }
