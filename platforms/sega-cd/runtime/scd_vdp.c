/* SEG-062 (ADR 0128): the main CPU's VDP ports are served by the existing Genesis VDP device. A zero-initialised GenesisRuntime is
 * held only as the device host; main accesses to $C00000-$C00007 go through genesis_route_access unchanged. VDP DMA would read
 * the Genesis host's memory, not the Sega CD's, so any register-1 write that enables DMA (M1, bit 4) is the typed stop
 * vdp_dma_unsupported before it reaches the device. */
#include "scd_runtime.h"

#include <stdlib.h>

#include "runtime.h"

int scd_vdp_init(ScdMachine *m) {
  m->vdp_host = calloc(1U, sizeof(GenesisRuntime));
  return m->vdp_host == NULL ? 1 : 0;
}

void scd_vdp_release(ScdMachine *m) {
  free(m->vdp_host);
  m->vdp_host = NULL;
}

static int scd_vdp_enables_dma(uint32_t word) { return (word & 0xFF00U) == 0x8100U && (word & 0x10U) != 0U; }

int scd_vdp_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  GenesisRuntimeStop stop = {0};
  uint32_t routed = is_write ? *value : 0U;
  scd_log(m, is_write ? 0x41U : 0x40U, address, routed);
  if (is_write && (address & ~3U) == 0xC00004U && width == 2U && scd_vdp_enables_dma(*value))
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, *value);
  if (genesis_route_access(host, address, width == 1U ? GENESIS_ACCESS_BYTE : GENESIS_ACCESS_WORD,
                           is_write ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ, &routed, &stop) != GENESIS_ACCESS_OK)
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address);
  if (!is_write) *value = routed;
  return 0;
}
