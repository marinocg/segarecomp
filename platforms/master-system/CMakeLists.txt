# Master System machine product (SEG-009, ADR 0063): generation-time C++ (machine) and the C11 runtime compiled with
# generated programs (runtime). The runtime is a plain C11 library; the machine library depends on it only for the
# shared contract headers (mapper contract table, typed error surface).
add_subdirectory(runtime)
add_subdirectory(machine)
