# SEG-051 (ADR 0116): offline research helper that prints the Z80-owned decode-token report. A z80-named target so the ADR 0059
# dependency boundary (cpu_z80 only from z80 targets) stays intact; it is not part of the product build route.
add_executable(segarecomp_z80_token_report z80_token_report_main.cpp)
target_link_libraries(segarecomp_z80_token_report PRIVATE segarecomp::cpu_z80)
segarecomp_enable_warnings(segarecomp_z80_token_report)
