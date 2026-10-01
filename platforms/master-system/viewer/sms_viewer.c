#include "sms_viewer.h"

#include <stdio.h>
#include <string.h>

#define NS_PER_SEC UINT64_C(1000000000)
#define CLOCK_NUM UINT64_C(39375000)
#define CLOCK_DEN UINT64_C(11)
/* period = frame T x 11 / 39,375,000 s = PERIOD_NUM / CLOCK_NUM ns */
#define PERIOD_NUM (SMS_CYCLES_PER_FRAME * CLOCK_DEN * NS_PER_SEC)

uint64_t sms_pacer_period_floor_ns(void) { return PERIOD_NUM / CLOCK_NUM; }

void sms_pacer_init(SmsPacer *pacer, int unthrottled) {
  if (pacer == NULL) return;
  memset(pacer, 0, sizeof *pacer);
  pacer->unthrottled = unthrottled ? 1u : 0u;
}

int sms_pacer_wait(SmsPacer *pacer, const SmsViewerHost *host) {
  uint64_t now, period;
  if (pacer == NULL || host == NULL || host->now_ns == NULL) return -1;
  if (!pacer->unthrottled && host->sleep_ns == NULL) return -1;
  now = host->now_ns(host->ctx);
  if (pacer->started && now < pacer->last_now_ns) return -1;
  period = sms_pacer_period_floor_ns();
  if (!pacer->started) {
    pacer->started = 1;
    pacer->deadline_ns = now;
    pacer->rem = 0;
  } else if (now > pacer->deadline_ns && now - pacer->deadline_ns > SMS_PACER_MAX_LATE_PERIODS * (period + 1u)) {
    pacer->deadline_ns = now; /* bounded late-stall resync: no burst, no drift */
    pacer->rem = 0;
    pacer->resyncs++;
  }
  if (!pacer->unthrottled && now < pacer->deadline_ns) {
    host->sleep_ns(host->ctx, pacer->deadline_ns - now);
    pacer->sleep_calls++;
  }
  pacer->last_now_ns = now;
  pacer->deadline_ns += period;
  pacer->rem += PERIOD_NUM % CLOCK_NUM;
  if (pacer->rem >= CLOCK_NUM) {
    pacer->rem -= CLOCK_NUM;
    pacer->deadline_ns++;
  }
  return 0;
}

uint8_t sms_viewer_pad_from_keys(const SmsViewerKeys *k) {
  if (k == NULL) return 0u;
  return (uint8_t)((k->up ? SMS_PAD_UP : 0u) | (k->down ? SMS_PAD_DOWN : 0u) | (k->left ? SMS_PAD_LEFT : 0u) |
                   (k->right ? SMS_PAD_RIGHT : 0u) | (k->button1 ? SMS_PAD_BUTTON1 : 0u) | (k->button2 ? SMS_PAD_BUTTON2 : 0u));
}

static int parse_u64(const char *s, uint64_t *out) {
  uint64_t v = 0;
  if (s == NULL || *s == '\0') return -1;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10u) return -1;
    v = v * 10u + (uint64_t)(*s - '0');
  }
  *out = v;
  return 0;
}

int sms_viewer_options_parse(int argc, const char *const *argv, SmsViewerOptions *out) {
  SmsViewerOptions o;
  uint64_t v;
  int i;
  if (out == NULL || argc < 0 || (argc > 0 && argv == NULL)) return -1;
  memset(&o, 0, sizeof o);
  o.frames = SMS_NO_LIMIT;
  o.scale = 3u;
  for (i = 0; i < argc; ++i) {
    if (argv[i] == NULL) return -1;
    if (strcmp(argv[i], "--viewer-unthrottled") == 0) o.unthrottled = 1u;
    else if (strcmp(argv[i], "--viewer-mute") == 0) o.mute = 1u;
    else if (strcmp(argv[i], "--viewer-frames") == 0) {
      if (i + 1 >= argc || parse_u64(argv[i + 1], &o.frames) != 0) return -1;
      ++i;
    } else if (strcmp(argv[i], "--viewer-scale") == 0) {
      if (i + 1 >= argc || parse_u64(argv[i + 1], &v) != 0 || v == 0u || v > 32u) return -1;
      o.scale = (uint32_t)v;
      ++i;
    } else if (strcmp(argv[i], "--viewer-record") == 0) {
      if (i + 1 >= argc) return -1;
      o.record = argv[++i];
    }
  }
  *out = o;
  return 0;
}

/* ---- rig ------------------------------------------------------------------------------------------------------ */

void sms_viewer_rig_reset(SmsViewerRig *rig) {
  sms_renderer_init(&rig->renderer, &rig->vdp, rig->records, SMS_VIEWER_FRAME_RECORDS);
  sms_renderer_attach(&rig->renderer);
  sn76489_ring_clear(&rig->ring);
  rig->event_count = 0;
  memset(&rig->recorded, 0, sizeof rig->recorded);
  sms_machine_set_input(&rig->machine, rig->events, 0);
  sms_machine_reset(&rig->machine);
}

int sms_viewer_rig_init(SmsViewerRig *rig, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family, SmsError *error) {
  SmsError e = sms_machine_init(&rig->machine, rom, rom_size, family);
  if (error != NULL) *error = e;
  if (e != SMS_OK) return 0;
  sms_machine_set_traces(&rig->machine, rig->irq_trace, SMS_VIEWER_TRACE_CAPACITY, rig->mapper_trace, SMS_VIEWER_TRACE_CAPACITY);
  memset(&rig->vdp, 0, sizeof rig->vdp);
  sms_vdp_set_trace(&rig->vdp, rig->vdp_trace, SMS_VIEWER_TRACE_CAPACITY);
  sms_vdp_install(&rig->machine, &rig->vdp);
  sn76489_ring_init(&rig->ring, rig->ring_storage, SMS_VIEWER_RING_SAMPLES);
  if (!sms_psg_attach(&rig->psg, &rig->machine, sn76489_ring_sink, &rig->ring)) {
    if (error != NULL) *error = SMS_ERROR_PROFILE_UNSUPPORTED;
    return 0;
  }
  sms_pad_attach(&rig->machine, &rig->pad);
  rig->machine.stop_on_halt_idle = 0;
  sms_viewer_rig_reset(rig);
  return 1;
}

size_t sms_viewer_format_script(const SmsViewerRig *rig, char *out, size_t cap) {
  static const char keys[] = "UDLR12";
  size_t used = 0;
  uint32_t i;
  for (i = 0; i < rig->event_count; ++i) {
    const SmsInputEvent *e = &rig->events[i];
    char line[64], p1[7], p2[7];
    int n, b;
    for (b = 0; b < 6; ++b) {
      p1[b] = (e->p1 >> b) & 1u ? keys[b] : '-';
      p2[b] = (e->p2 >> b) & 1u ? keys[b] : '-';
    }
    p1[6] = p2[6] = '\0';
    n = snprintf(line, sizeof line, "%llu %s %s %c\n", (unsigned long long)e->frame, p1, p2, e->pause ? 'P' : '-');
    if (n < 0 || used + (size_t)n > cap) return 0;
    memcpy(out + used, line, (size_t)n);
    used += (size_t)n;
  }
  return used;
}

/* ---- run loop ------------------------------------------------------------------------------------------------- */

static SmsViewerResult finish(SmsViewerResult r, SmsViewerOutcome o) {
  r.outcome = o;
  return r;
}

static void drain_audio(SmsViewerRig *rig, const SmsViewerHost *host, SmsViewerResult *r) {
  int16_t buffer[1024];
  uint32_t n;
  while ((n = sn76489_ring_read(&rig->ring, buffer, 1024u, NULL)) != 0u) {
    r->audio_samples += n;
    if (host->audio != NULL && host->audio(host->ctx, buffer, n) != 0) r->audio_failures++;
  }
}

SmsViewerResult sms_viewer_run(SmsViewerRig *rig, const SmsViewerHost *host, SmsPacer *pacer, uint64_t max_frames) {
  SmsViewerResult r;
  uint64_t presented;
  memset(&r, 0, sizeof r);
  if (rig == NULL || host == NULL || pacer == NULL || host->poll == NULL || host->present == NULL)
    return finish(r, SMS_VIEWER_INVALID_ARGUMENT);
  presented = rig->renderer.frames_completed;
  for (;;) {
    SmsViewerInput in;
    SmsInputState want;
    SmsStop stop;
    if (max_frames != SMS_NO_LIMIT && r.frames >= max_frames) return finish(r, SMS_VIEWER_FRAME_LIMIT);
    memset(&in, 0, sizeof in);
    if (host->poll(host->ctx, &in) != 0) return finish(r, SMS_VIEWER_HOST_ERROR);
    if (in.quit) return finish(r, SMS_VIEWER_QUIT);
    if (in.reset) {
      sms_viewer_rig_reset(rig);
      presented = 0;
      r.resets++;
    }
    want.p1 = (uint8_t)(in.p1 & 0x3Fu);
    want.p2 = (uint8_t)(in.p2 & 0x3Fu);
    want.pause = in.pause ? 1u : 0u;
    if (memcmp(&want, &rig->recorded, sizeof want) != 0) {
      SmsInputEvent *ev;
      if (rig->event_count >= SMS_VIEWER_INPUT_CAPACITY) return finish(r, SMS_VIEWER_INPUT_FULL);
      ev = &rig->events[rig->event_count++];
      /* the first frame-start event the machine has not applied yet */
      ev->frame = (rig->machine.next_line + SMS_LINES_PER_FRAME - 1u) / SMS_LINES_PER_FRAME;
      ev->p1 = want.p1;
      ev->p2 = want.p2;
      ev->pause = want.pause;
      rig->machine.input_count = rig->event_count;
      rig->recorded = want;
    }
    stop = sms_run_until_frame(&rig->machine, rig->machine.rt.state.cycles / SMS_CYCLES_PER_FRAME + 1u);
    r.stop = stop;
    r.frames++;
    sms_psg_sync(&rig->psg, stop.cycles);
    drain_audio(rig, host, &r);
    if (sms_pacer_wait(pacer, host) != 0) return finish(r, SMS_VIEWER_HOST_ERROR);
    if (rig->renderer.frames_completed != presented && rig->renderer.last_height != 0u) {
      if (host->present(host->ctx, rig->renderer.last, rig->renderer.last_height) != 0) return finish(r, SMS_VIEWER_HOST_ERROR);
      presented = rig->renderer.frames_completed;
      r.frames_presented++;
    }
    if (!sms_stop_is_resumable(stop.kind)) return finish(r, SMS_VIEWER_GUEST_ERROR);
  }
}
