# Master System headless driver sources (ADR 0064 section 4). The generated program compiles `sms_headless.c` (its `main`)
# and `sms_devices_none.c` (replaced by the device wiring once T004/T006/T007 land) together with the SMS runtime and the
# emitted image; the OBJECT library below only proves both translation units are strict C11 at build time.
add_library(segarecomp_headless_master_system OBJECT sms_headless.c sms_audio.c sms_devices_none.c sms_devices_vdp.c)
set_target_properties(segarecomp_headless_master_system PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)
target_link_libraries(segarecomp_headless_master_system PRIVATE segarecomp::runtime_master_system)
segarecomp_enable_warnings(segarecomp_headless_master_system)
# SEG-147-T004/T005 (ADR 0325): the driver of a build with a scheduled mod, compile-checked (a built program compiles these with -DSEGARECOMP_SMS_MOD).
add_library(segarecomp_headless_master_system_mod_check OBJECT sms_headless.c sms_headless_mod.c sms_devices_vdp.c)
set_target_properties(segarecomp_headless_master_system_mod_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(segarecomp_headless_master_system_mod_check PRIVATE SEGARECOMP_SMS_MOD=1)
target_include_directories(segarecomp_headless_master_system_mod_check PRIVATE ${PROJECT_SOURCE_DIR}/platforms/master-system/viewer ${PROJECT_SOURCE_DIR}/libs/viewer/src)
target_link_libraries(segarecomp_headless_master_system_mod_check PRIVATE segarecomp::runtime_master_system_mod)
segarecomp_enable_warnings(segarecomp_headless_master_system_mod_check)
