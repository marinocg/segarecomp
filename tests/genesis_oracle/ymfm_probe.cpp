// SEG-032-T001 (ADR 0074): the same trace through the production candidate (ymfm YM2612, BSD-3-Clause), compiled with
// -fno-exceptions -fno-rtti and linked WITHOUT a C++ runtime (see ymfm_shim.c).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "ymfm_opn.h"
#include "trace.h"

// The host owns time: busy and timers are interface callbacks (ADR 0074, decision 3). The probe only records that the engine asked.
struct ProbeInterface : public ymfm::ymfm_interface {
  unsigned busy_requests = 0;
  void ymfm_set_busy_end(uint32_t) override { ++busy_requests; }
};

int main(int argc, char **argv) {
  ProbeInterface intf;
  ymfm::ym2612 chip(intf);
  chip.reset();
  const bool key_on = argc > 1 && std::atoi(argv[1]) != 0;
  const unsigned status_idle = chip.read(0);
  for (unsigned i = 0; i < TRACE_COUNT; ++i) {
    chip.write(trace_writes[i].port * 2 + 0, trace_writes[i].reg);
    chip.write(trace_writes[i].port * 2 + 1, trace_writes[i].value);
  }
  chip.write(0, 0x28);
  chip.write(1, key_on ? 0xF0 : 0x00);
  const unsigned busy = intf.busy_requests > 0 ? 1u : 0u;
  long distinct = 0;
  int32_t last_l = 0, last_r = 0;
  uint64_t fnv = 0xcbf29ce484222325ULL;
  for (unsigned s = 0; s < TRACE_SAMPLES; ++s) {
    ymfm::ym2612::output_data out;
    chip.generate(&out, 1);
    if (s == 0 || out.data[0] != last_l || out.data[1] != last_r) ++distinct;
    last_l = out.data[0]; last_r = out.data[1];
    fnv = (fnv ^ static_cast<uint32_t>(out.data[0])) * 0x100000001b3ULL;
    fnv = (fnv ^ static_cast<uint32_t>(out.data[1])) * 0x100000001b3ULL;
  }
  std::printf("samples=%u distinct=%ld fnv=%016llx busy_after_write=%u status_idle=%u\n", TRACE_SAMPLES, distinct,
              static_cast<unsigned long long>(fnv), busy, status_idle);
  return 0;
}
