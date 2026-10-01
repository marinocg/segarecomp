#include "sms_pad.h"

#define IOC_A_TR_DIR 0x01u
#define IOC_A_TH_DIR 0x02u
#define IOC_B_TR_DIR 0x04u
#define IOC_B_TH_DIR 0x08u
#define IOC_A_TR_LVL 0x10u
#define IOC_A_TH_LVL 0x20u
#define IOC_B_TR_LVL 0x40u
#define IOC_B_TH_LVL 0x80u

/* Pad mask bit `bit` (pressed = 1) as an active-low line value. */
static unsigned line(uint8_t mask, unsigned bit) { return (mask & (1u << bit)) != 0u ? 0u : 1u; }

uint8_t sms_pad_th_level(uint8_t io_control, unsigned port) {
  if (port == 0u) return (io_control & IOC_A_TH_DIR) != 0u ? 1u : (uint8_t)((io_control & IOC_A_TH_LVL) != 0u);
  return (io_control & IOC_B_TH_DIR) != 0u ? 1u : (uint8_t)((io_control & IOC_B_TH_LVL) != 0u);
}

uint8_t sms_pad_port_dc(const SmsInputState *input, uint8_t io_control) {
  const uint8_t p1 = input->p1, p2 = input->p2;
  unsigned tr = line(p1, 5); /* A.TR = button 2 */
  if ((io_control & IOC_A_TR_DIR) == 0u) tr = (io_control & IOC_A_TR_LVL) != 0u;
  return (uint8_t)(line(p1, 0) | (line(p1, 1) << 1) | (line(p1, 2) << 2) | (line(p1, 3) << 3) | (line(p1, 4) << 4) |
                   (tr << 5) | (line(p2, 0) << 6) | (line(p2, 1) << 7));
}

uint8_t sms_pad_port_dd(const SmsInputState *input, uint8_t io_control) {
  const uint8_t p2 = input->p2;
  unsigned tr = line(p2, 5); /* B.TR = button 2 */
  if ((io_control & IOC_B_TR_DIR) == 0u) tr = (io_control & IOC_B_TR_LVL) != 0u;
  return (uint8_t)(line(p2, 2) | (line(p2, 3) << 1) | (line(p2, 4) << 2) | (tr << 3) | (1u << 4) /* no reset button */ |
                   (1u << 5) | ((unsigned)sms_pad_th_level(io_control, 0) << 6) |
                   ((unsigned)sms_pad_th_level(io_control, 1) << 7));
}

void sms_pad_reset(SmsPad *pad) {
  pad->io_control = 0xFFu;
  pad->h_latches = 0;
}

uint8_t sms_pad_read(SmsPad *pad, SmsPortClass cls, uint64_t cycles) {
  (void)cycles;
  if (pad->input == NULL) return 0xFFu;
  if (cls == SMS_PORT_PAD_DC) return sms_pad_port_dc(pad->input, pad->io_control);
  if (cls == SMS_PORT_PAD_DD) return sms_pad_port_dd(pad->input, pad->io_control);
  return 0xFFu;
}

void sms_pad_write(SmsPad *pad, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  unsigned port;
  if (cls != SMS_PORT_IO_CONTROL) return;
  for (port = 0; port < 2u; ++port) {
    if (sms_pad_th_level(pad->io_control, port) == 0u && sms_pad_th_level(value, port) == 1u) {
      ++pad->h_latches;
      if (pad->vdp != NULL) sms_vdp_latch_hcounter(pad->vdp, cycles);
    }
  }
  pad->io_control = value;
}

void sms_pad_digest(const SmsPad *pad, SmsSha256 *sha) {
  sms_sha256_update_u8(sha, pad->io_control);
  sms_sha256_update_u32(sha, pad->h_latches);
}

static void seam_reset(void *context) { sms_pad_reset((SmsPad *)context); }
static uint8_t seam_read(void *context, SmsPortClass cls, uint64_t cycles) {
  return sms_pad_read((SmsPad *)context, cls, cycles);
}
static void seam_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  sms_pad_write((SmsPad *)context, cls, value, cycles);
}
static void seam_digest(void *context, SmsSha256 *sha) { sms_pad_digest((const SmsPad *)context, sha); }

void sms_pad_install(SmsMachine *m, SmsPad *pad, SmsVdp *vdp) {
  pad->input = &m->input;
  pad->vdp = vdp;
  m->pad.context = pad;
  m->pad.reset = seam_reset;
  m->pad.read = seam_read;
  m->pad.write = seam_write;
  m->pad.digest = seam_digest;
}

void sms_pad_attach(SmsMachine *m, SmsPad *pad) { sms_pad_install(m, pad, sms_vdp_from_machine(m)); }
