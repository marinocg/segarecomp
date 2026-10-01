"""The host C compiler policy shared by the SMS/Z80 tests and tools (SEG-033-T007).

One definition of what every test-built C11 translation unit shares: the C standard, the strict warning set and the Windows CRT
define. Before this module six tests and `z80_conformance.py` each spelled the list out (and `sms_psg_differential_test.py`
had silently dropped the Windows define). Nothing else is shared here: include paths, optimization level and executable
names stay with the caller.
"""

C_STANDARD = "-std=c11"
STRICT_WARNINGS = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
# MSVC's CRT deprecates fopen/getenv and friends; the generated and runtime C is portable C11 and must still build clean.
WINDOWS_CRT_DEFINE = "-D_CRT_SECURE_NO_WARNINGS"

# `cc <STRICT_C11> ...`: portable C11, strict warnings as errors, Windows CRT compatibility.
STRICT_C11 = [C_STANDARD, *STRICT_WARNINGS, WINDOWS_CRT_DEFINE]
