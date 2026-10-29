// Captures a few seconds from an ALSA device of Station 1 through the system tinyalsa and writes a WAV;
// prints per-channel level so a dead or swapped microphone is visible without listening.
// Usage: capture_probe CARD DEVICE CHANNELS RATE PERIOD SECONDS OUT.wav   (format is S32_LE, as maind opens it)
// Station 1 (maind): mics 2 0 5 16000 1024, echo reference (Loopback) 4 1 2 48000 3072.
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// tinyalsa of Android 6 (struct padded: later versions appended fields)
struct pcm_config {
  unsigned channels, rate, period_size, period_count;
  int format;  // 1 = PCM_FORMAT_S32_LE
  unsigned start_threshold, stop_threshold, silence_threshold;
  int avail_min;
  unsigned pad[8];
};
#define PCM_IN 0x10000000

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv) {
  if (argc != 8) { fprintf(stderr, "usage: %s CARD DEVICE CHANNELS RATE PERIOD SECONDS OUT.wav\n", argv[0]); return 1; }
  unsigned card = atoi(argv[1]), dev = atoi(argv[2]), ch = atoi(argv[3]), rate = atoi(argv[4]), period = atoi(argv[5]);
  double secs = atof(argv[6]);
  void *t = dlopen("libtinyalsa.so", RTLD_NOW);
  if (!t) { printf("libtinyalsa.so: %s\n", dlerror()); return 1; }
  void *(*open_)(unsigned, unsigned, unsigned, struct pcm_config *) = dlsym(t, "pcm_open");
  int (*ready)(void *) = dlsym(t, "pcm_is_ready");
  const char *(*error)(void *) = dlsym(t, "pcm_get_error");
  int (*read_)(void *, void *, unsigned) = dlsym(t, "pcm_read");
  int (*close_)(void *) = dlsym(t, "pcm_close");
  if (!open_ || !ready || !error || !read_ || !close_) { printf("tinyalsa: symbol missing\n"); return 1; }

  struct pcm_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.channels = ch, cfg.rate = rate, cfg.period_size = period, cfg.period_count = 4, cfg.format = 1;
  void *pcm = open_(card, dev, PCM_IN, &cfg);
  if (!pcm || !ready(pcm)) { printf("pcm_open %u,%u: %s\n", card, dev, pcm ? error(pcm) : "null"); return 2; }
  printf("capture %u,%u: %u ch, %u Hz, S32_LE, period %u\n", card, dev, ch, rate, period);

  FILE *f = fopen(argv[7], "wb");
  if (!f) { perror(argv[7]); return 1; }
  unsigned frames = (unsigned)(secs * rate) / period * period;
  uint32_t data_bytes = frames * ch * 4;
  fwrite("RIFF", 1, 4, f), put32(f, 36 + data_bytes), fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16), put16(f, 1), put16(f, ch), put32(f, rate), put32(f, rate * ch * 4), put16(f, ch * 4), put16(f, 32);
  fwrite("data", 1, 4, f), put32(f, data_bytes);

  int32_t *buf = malloc((size_t)period * ch * 4);
  double *sum = calloc(ch, sizeof *sum);
  int32_t *peak = calloc(ch, sizeof *peak);
  unsigned errors = 0;
  for (unsigned done = 0; done < frames; done += period) {
    if (read_(pcm, buf, period * ch * 4)) { errors++; printf("pcm_read: %s\n", error(pcm)); memset(buf, 0, (size_t)period * ch * 4); }
    for (unsigned i = 0; i < period * ch; i++) {
      int32_t v = buf[i], a = v == INT32_MIN ? INT32_MAX : abs(v);
      sum[i % ch] += (double)v * v;
      if (a > peak[i % ch]) peak[i % ch] = a;
    }
    fwrite(buf, 4, (size_t)period * ch, f);
  }
  fclose(f);
  close_(pcm);
  for (unsigned c = 0; c < ch; c++) {
    double rms = sqrt(sum[c] / (frames ? frames : 1)) / 2147483648.0;
    printf("ch%u: rms %.1f dBFS, peak %.1f dBFS\n", c, rms > 0 ? 20 * log10(rms) : -999.0, peak[c] ? 20 * log10(peak[c] / 2147483648.0) : -999.0);
  }
  printf("%u frames written to %s, read errors %u\n", frames, argv[7], errors);
  return errors ? 3 : 0;
}
