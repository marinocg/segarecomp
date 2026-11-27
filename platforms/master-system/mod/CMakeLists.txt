# SEG-147-T004..T006 (ADR 0325): the Master System mod scheduler and presentation compositor. A separate library: the runtime above, and every Vanilla program, links none of it. `sms_mod_host.c` needs the
# build-generated artifact, so it is only compile-checked here (a built program links it).
add_library(segarecomp_runtime_master_system_mod STATIC sms_mod_scheduler.c sms_mod_present.c)
add_library(segarecomp::runtime_master_system_mod ALIAS segarecomp_runtime_master_system_mod)
set_target_properties(segarecomp_runtime_master_system_mod PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_runtime_master_system_mod PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/platforms/master-system/sdk ${PROJECT_SOURCE_DIR}/platforms/sdk ${PROJECT_SOURCE_DIR}/libs/viewer/include)
target_link_libraries(segarecomp_runtime_master_system_mod PUBLIC segarecomp::runtime_master_system segarecomp::viewer_core)
segarecomp_enable_warnings(segarecomp_runtime_master_system_mod)
add_library(segarecomp_runtime_master_system_mod_host_check OBJECT sms_mod_host.c)
set_target_properties(segarecomp_runtime_master_system_mod_host_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(segarecomp_runtime_master_system_mod_host_check PRIVATE segarecomp::runtime_master_system_mod)
segarecomp_enable_warnings(segarecomp_runtime_master_system_mod_host_check)
