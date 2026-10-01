#include "sms_psg.h"

static void psg_reset(void *context) {
  SmsPsg *psg = (SmsPsg *)context;
  sn76489_reset(&psg->chip);
  sn76489_pcm_reset(&psg->pcm);
}

static void psg_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  SmsPsg *psg = (SmsPsg *)context;
  (void)cls;
  sn76489_advance(&psg->chip, &psg->pcm, cycles);
  if (sn76489_write(&psg->chip, value) == SN76489_WRITE_DATA_BEFORE_LATCH) {
    sms_memory_latch_error(&psg->machine->mem, SMS_ERROR_PSG_DATA_BEFORE_LATCH, 0x7Fu, value, cycles);
    psg->machine->rt.state.deadline = 0; /* stop at this instruction boundary, like every other platform error */
  }
}

static void psg_digest(void *context, SmsSha256 *sha) {
  uint8_t bytes[SN76489_STATE_BYTES];
  sn76489_state_bytes(&((SmsPsg *)context)->chip, bytes);
  sms_sha256_update(sha, bytes, sizeof bytes);
}

int sms_psg_attach(SmsPsg *psg, SmsMachine *machine, Sn76489PcmSink sink, void *sink_context) {
  const Sn76489Config config = sn76489_default_config();
  const Sn76489PcmConfig pcm_config = sn76489_pcm_config_44100_sms();
  if (!sn76489_init(&psg->chip, &config)) return 0;
  sn76489_pcm_init(&psg->pcm, &pcm_config, sink, sink_context);
  psg->machine = machine;
  machine->psg.context = psg;
  machine->psg.reset = psg_reset;
  machine->psg.read = NULL;
  machine->psg.write = psg_write;
  machine->psg.digest = psg_digest;
  return 1;
}

int sms_psg_configure(SmsPsg *psg, const Sn76489Config *config) {
  if (!sn76489_init(&psg->chip, config)) return 0;
  sn76489_pcm_reset(&psg->pcm);
  return 1;
}

void sms_psg_sync(SmsPsg *psg, uint64_t cycles) {
  sn76489_advance(&psg->chip, &psg->pcm, cycles);
  sn76489_pcm_flush(&psg->pcm);
}
