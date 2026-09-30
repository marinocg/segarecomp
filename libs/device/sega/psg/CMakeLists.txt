# Platform-neutral Sega PSG (SN76489-family) device and deterministic PCM stream (SEG-009-T007, ADR 0063 placement).
# Plain C11 with no platform, runtime or CPU dependency: the Master System runtime consumes it today and the Genesis
# platform can consume it later (SEG-032) without depending on the Master System.
add_library(segarecomp_device_psg STATIC src/sn76489.c)
add_library(segarecomp::device_psg ALIAS segarecomp_device_psg)
set_target_properties(segarecomp_device_psg PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(segarecomp_device_psg PUBLIC include)
segarecomp_enable_warnings(segarecomp_device_psg)
