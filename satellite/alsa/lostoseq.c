/* lostOS equalizer: an ALSA external plugin (pcm type "lostoseq") that sits in front of the speaker mixer.
 *
 *   pcm.!speakers { type lostoseq; slave.pcm "dmix_speakers"; }
 *   pcm_type.lostoseq { lib "/data/lostos/alsa/libasound_module_pcm_lostoseq.so" }
 *
 * Ten peaking filters (31 Hz ... 16 kHz, RBJ biquads, Q = 1.4) and a preamp, applied to 48 kHz S16 stereo. The settings are read from
 * /data/lostos/eq.conf and re-read when the file changes, so lostos only has to write that file:
 *
 *   enabled=1
 *   preamp=-2.0                       (dB)
 *   gains=0,0,3,2,0,0,0,1,2,3        (dB per band, ten values)
 *
 * With the equalizer off or flat, the samples pass through untouched. */
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BANDS 10
#define CONF_FILE "/data/lostos/eq.conf"
#define CHECK_EVERY_FRAMES 24000   /* look at the file twice a second at 48 kHz */

static const double kFreq[BANDS] = {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

struct biquad {
  double b0, b1, b2, a1, a2;
  double z1[2], z2[2];   /* per channel state (direct form II transposed) */
};

typedef struct {
  snd_pcm_extplug_t ext;
  struct biquad band[BANDS];
  double preamp;          /* linear */
  int active;             /* 0: pass through */
  unsigned rate;
  time_t mtime;
  long mtime_ns;
  long frames_since_check;
} lostoseq_t;

static void design(struct biquad *q, double f0, double gain_db, double fs, double Q) {
  const double A = pow(10.0, gain_db / 40.0), w0 = 2.0 * M_PI * f0 / fs, alpha = sin(w0) / (2.0 * Q), c = cos(w0);
  const double a0 = 1.0 + alpha / A;
  q->b0 = (1.0 + alpha * A) / a0;
  q->b1 = (-2.0 * c) / a0;
  q->b2 = (1.0 - alpha * A) / a0;
  q->a1 = (-2.0 * c) / a0;
  q->a2 = (1.0 - alpha / A) / a0;
}

static void load_conf(lostoseq_t *eq) {
  int enabled = 0;
  double preamp_db = 0, gains[BANDS] = {0};
  FILE *f = fopen(CONF_FILE, "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "enabled=", 8)) enabled = atoi(line + 8);
      else if (!strncmp(line, "preamp=", 7)) preamp_db = atof(line + 7);
      else if (!strncmp(line, "gains=", 6)) {
        char *p = line + 6;
        for (int i = 0; i < BANDS && *p; i++) {
          gains[i] = strtod(p, &p);
          if (*p == ',') p++;
        }
      }
    }
    fclose(f);
  }
  int flat = 1;
  for (int i = 0; i < BANDS; i++)
    if (fabs(gains[i]) > 0.05) flat = 0;
  if (fabs(preamp_db) > 0.05) flat = 0;
  eq->preamp = pow(10.0, preamp_db / 20.0);
  for (int i = 0; i < BANDS; i++) {
    if (kFreq[i] < eq->rate / 2.2) design(&eq->band[i], kFreq[i], gains[i], eq->rate, 1.4);
    else design(&eq->band[i], kFreq[i], 0.0, eq->rate, 1.4);   /* a band above the Nyquist limit stays flat */
  }
  eq->active = enabled && !flat;
}

static void check_conf(lostoseq_t *eq) {
  struct stat st;
  const int ok = stat(CONF_FILE, &st) == 0;
  const time_t mt = ok ? st.st_mtime : 0;
  const long ns = ok ? st.st_mtim.tv_nsec : 0;
  if (mt != eq->mtime || ns != eq->mtime_ns) {
    eq->mtime = mt;
    eq->mtime_ns = ns;
    load_conf(eq);
  }
}

static snd_pcm_sframes_t eq_transfer(snd_pcm_extplug_t *ext, const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
                                     const snd_pcm_channel_area_t *src_areas, snd_pcm_uframes_t src_offset, snd_pcm_uframes_t size) {
  lostoseq_t *eq = (lostoseq_t *)ext;
  const int16_t *src = (const int16_t *)((const char *)src_areas[0].addr + src_areas[0].first / 8) + src_offset * 2;
  int16_t *dst = (int16_t *)((char *)dst_areas[0].addr + dst_areas[0].first / 8) + dst_offset * 2;
  eq->frames_since_check += (long)size;
  if (eq->frames_since_check >= CHECK_EVERY_FRAMES) {
    eq->frames_since_check = 0;
    check_conf(eq);
  }
  if (!eq->active) {
    memcpy(dst, src, size * 4);
    return (snd_pcm_sframes_t)size;
  }
  for (snd_pcm_uframes_t n = 0; n < size; n++) {
    for (int ch = 0; ch < 2; ch++) {
      double x = src[n * 2 + ch] * eq->preamp;
      for (int b = 0; b < BANDS; b++) {
        struct biquad *q = &eq->band[b];
        const double y = q->b0 * x + q->z1[ch];
        q->z1[ch] = q->b1 * x - q->a1 * y + q->z2[ch];
        q->z2[ch] = q->b2 * x - q->a2 * y;
        x = y;
      }
      if (x > 32767.0) x = 32767.0;
      else if (x < -32768.0) x = -32768.0;
      dst[n * 2 + ch] = (int16_t)lrint(x);
    }
  }
  return (snd_pcm_sframes_t)size;
}

static int eq_init(snd_pcm_extplug_t *ext) {
  lostoseq_t *eq = (lostoseq_t *)ext;
  eq->rate = ext->rate;
  memset(eq->band, 0, sizeof eq->band);
  eq->mtime = (time_t)-1;           /* force the first load */
  load_conf(eq);
  struct stat st;
  if (stat(CONF_FILE, &st) == 0) {
    eq->mtime = st.st_mtime;
    eq->mtime_ns = st.st_mtim.tv_nsec;
  } else {
    eq->mtime = 0;
    eq->mtime_ns = 0;
  }
  return 0;
}

static int eq_close(snd_pcm_extplug_t *ext) {
  free(ext);
  return 0;
}

static const snd_pcm_extplug_callback_t callbacks = {
    .transfer = eq_transfer,
    .init = eq_init,
    .close = eq_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(lostoseq) {
  snd_config_iterator_t i, next;
  snd_config_t *slave = NULL;
  int err;
  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0) continue;
    if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
    if (!strcmp(id, "slave")) {
      slave = n;
      continue;
    }
    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }
  if (!slave) {
    SNDERR("No slave configuration for lostoseq");
    return -EINVAL;
  }
  lostoseq_t *eq = calloc(1, sizeof *eq);
  if (!eq) return -ENOMEM;
  eq->ext.version = SND_PCM_EXTPLUG_VERSION;
  eq->ext.name = "lostOS equalizer";
  eq->ext.callback = &callbacks;
  eq->ext.private_data = eq;
  err = snd_pcm_extplug_create(&eq->ext, name, root, slave, stream, mode);
  if (err < 0) {
    free(eq);
    return err;
  }
  snd_pcm_extplug_set_param(&eq->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_slave_param(&eq->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 2);
  snd_pcm_extplug_set_param(&eq->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  snd_pcm_extplug_set_slave_param(&eq->ext, SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_S16);
  *pcmp = eq->ext.pcm;
  return 0;
}
SND_PCM_PLUGIN_SYMBOL(lostoseq);
