#include "audio/frontend.h"

#include <alsa/asoundlib.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>

#include "audio/spectrum.h"
#include "core/log.h"

// Signatures of libyandex_evil.so (recovered by reverse engineering maind, verified on device):
//   VQE functions return a YandexVqeC_Error* (nullptr on success).
//   Spotter functions take a trailing spotter_error** (set on failure).
//   VQE I/O is float in [-1, 1]; mics/speakers are frame-interleaved; Process consumes
//   GetInputChunkSize() frames (Midi: 3072 @ 96 kHz, Lite 2: 1536 @ 48 kHz) and yields
//   GetProcessingFrame() samples (512 @ 16 kHz) per output channel. Output channel types follow
//   voicetech.AudioInputChannelType.
//   Spotter input is s16le interleaved in the channel order passed to spotter_create_multichannel.

namespace cs {
namespace {

constexpr const char *kLibDir = "/system/vendor/quasar/libs/";
constexpr const char *kVqeModels = "/system/vendor/quasar/vqe_models";

// voicetech.AudioInputChannelType (VQE output channel types)
enum : int { RAW_MAIN_MIC_SYNC = 2, VQE_OMNI = 6, VQE_OMNI_WITH_GAIN = 9 };
// Spotter channel type (0..5) -> VQE output feeding it. maind maps spotter types to
// {UNKNOWN(main), RAW_MAIN_MIC_SYNC, VQE_OMNI_WITH_GAIN, RAW_AUX_MIC_SYNC, VQE_OMNI, VQE_BEAMFORMING};
// the cucumber preset has no AUX/BEAMFORMING outputs, so the closest available ones are used.
constexpr int kSpotterToVqe[6] = {VQE_OMNI_WITH_GAIN, RAW_MAIN_MIC_SYNC, VQE_OMNI_WITH_GAIN,
                                  RAW_MAIN_MIC_SYNC,  VQE_OMNI,          VQE_OMNI};
// Spotter events (event_id_to_str). Hits are confirmations; 0/4/7/9 mean "need more data / finished".
enum : int { EV_CONFIRM_1ST = 2, EV_CONFIRM_2ND = 5 };
constexpr unsigned kEventsStopMask = (1u << 0) | (1u << 4) | (1u << 7) | (1u << 9);

// A capture device. read() returns frames read (> 0), 0 when nothing came in time, kGap after an
// overrun it recovered from (the samples before it do not join the ones after), kLost when the device
// is gone (it is closed and reopened later). Frames are interleaved: S16 (channels lanes) or S32
// (channels / 2 slots, see Model::capture_format).
struct CaptureSource {
  enum : long { kGap = -1, kLost = -2 };
  virtual ~CaptureSource() = default;
  virtual long read(void *buf, size_t frames) = 0;
};

class AlsaCapture : public CaptureSource {
 public:
  // Fails loudly if the device does not give the format/rate the VQE preset expects: a silent
  // rate mismatch would feed the VQE and spotters wrongly timed audio. The period may differ
  // (the FIFO decouples ALSA periods from VQE chunks).
  static std::unique_ptr<CaptureSource> open(const char *dev, bool s32, unsigned nch, unsigned want_rate, size_t period) {
    snd_pcm_t *pcm = nullptr;
    // Non-blocking: reads never wait inside ALSA, so stop() is never stuck in snd_pcm_readi.
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
      LOGE("snd_pcm_open %s: %s", dev, snd_strerror(err));
      return nullptr;
    }
    auto fail = [&](const char *what, int e) -> std::unique_ptr<CaptureSource> {
      LOGE("capture %s: %s: %s", dev, what, e < 0 ? snd_strerror(e) : "unexpected value");
      snd_pcm_close(pcm);
      return nullptr;
    };
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) return fail("hw_params_any", err);
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return fail("set_access", err);
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, s32 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE)) < 0) return fail("set_format", err);
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, nch)) < 0) return fail("set_channels", err);
    unsigned rate = want_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr)) < 0) return fail("set_rate_near", err);
    if (rate != want_rate) {
      LOGE("capture %s: rate %u Hz, need %u", dev, rate, want_rate);
      return fail("rate", 0);
    }
    snd_pcm_uframes_t per = period, buf = period * 8;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, nullptr)) < 0) return fail("set_period_size_near", err);
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf)) < 0) return fail("set_buffer_size_near", err);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) return fail("snd_pcm_hw_params", err);
    LOGI("capture %s: %u Hz, %u ch %s, period %lu, buffer %lu", dev, rate, nch, s32 ? "S32" : "S16", (unsigned long)per,
         (unsigned long)buf);
    auto c = std::make_unique<AlsaCapture>();
    c->pcm_ = pcm;
    c->hw_period_ = std::min<snd_pcm_uframes_t>(per, period);
    return c;
  }
  ~AlsaCapture() override {
    if (pcm_) snd_pcm_close(pcm_);
  }
  long read(void *buf, size_t frames) override {
    snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf, std::min<snd_pcm_uframes_t>(frames, hw_period_));
    if (n == -EAGAIN) {
      int w = snd_pcm_wait(pcm_, 200);
      if (w >= 0) return 0;  // ready, or timeout (re-checks running_)
      n = w;
    }
    if (n >= 0) return long(n);
    LOGW("capture: %s, recovering", snd_strerror((int)n));
    return snd_pcm_recover(pcm_, (int)n, 1) < 0 ? kLost : kGap;
  }

 private:
  snd_pcm_t *pcm_ = nullptr;
  snd_pcm_uframes_t hw_period_ = 0;
};

// The system tinyalsa of the Android stations (Station 1: Android 6), loaded with dlopen like the
// Yandex libraries. The struct is the Android 6 one, padded because later tinyalsa versions append
// fields. pcm_read blocks until a full period came (64 ms on Station 1), so stop() waits at most that
// long while the device runs; it recovers overruns itself (the gap is not reported).
class TinyalsaCapture : public CaptureSource {
 public:
  static std::unique_ptr<CaptureSource> open(const std::string &dev, bool s32, unsigned nch, unsigned rate, size_t period) {
    unsigned card = 0, device = 0;
    if (sscanf(dev.c_str(), "tinyalsa:%u,%u", &card, &device) != 2) {
      LOGE("capture %s: expected tinyalsa:<card>,<device>", dev.c_str());
      return nullptr;
    }
    auto c = std::make_unique<TinyalsaCapture>();
    static void *lib = dlopen("libtinyalsa.so", RTLD_NOW);
    if (!lib) {
      LOGE("capture %s: %s", dev.c_str(), dlerror());
      return nullptr;
    }
    auto open_ = (void *(*)(unsigned, unsigned, unsigned, Config *))dlsym(lib, "pcm_open");
    auto ready = (int (*)(void *))dlsym(lib, "pcm_is_ready");
    c->read_ = (int (*)(void *, void *, unsigned))dlsym(lib, "pcm_read");
    c->error_ = (const char *(*)(void *))dlsym(lib, "pcm_get_error");
    c->close_ = (int (*)(void *))dlsym(lib, "pcm_close");
    if (!open_ || !ready || !c->read_ || !c->error_ || !c->close_) {
      LOGE("capture %s: libtinyalsa.so lacks pcm_open/pcm_read", dev.c_str());
      return nullptr;
    }
    Config cfg{};
    cfg.channels = nch, cfg.rate = rate, cfg.period_size = unsigned(period), cfg.period_count = 4;
    cfg.format = s32 ? 1 : 0;  // PCM_FORMAT_S32_LE / PCM_FORMAT_S16_LE
    c->pcm_ = open_(card, device, kPcmIn, &cfg);
    if (!c->pcm_ || !ready(c->pcm_)) {
      LOGE("capture %s: pcm_open: %s", dev.c_str(), c->pcm_ ? c->error_(c->pcm_) : "failed");
      return nullptr;
    }
    c->frame_bytes_ = nch * (s32 ? 4 : 2);
    c->period_ = period;
    LOGI("capture %s: %u Hz, %u ch %s, period %zu x 4", dev.c_str(), rate, nch, s32 ? "S32" : "S16", period);
    return c;
  }
  ~TinyalsaCapture() override {
    if (pcm_) close_(pcm_);
  }
  long read(void *buf, size_t frames) override {
    frames = std::min(frames, period_);
    if (read_(pcm_, buf, unsigned(frames * frame_bytes_)) == 0) return long(frames);
    LOGW("capture: %s", error_(pcm_));
    return kLost;
  }

 private:
  struct Config {
    unsigned channels, rate, period_size, period_count;
    int format;
    unsigned start_threshold, stop_threshold, silence_threshold;
    int avail_min;
    unsigned pad[8];
  };
  static constexpr unsigned kPcmIn = 0x10000000;
  void *pcm_ = nullptr;
  size_t frame_bytes_ = 0, period_ = 0;
  int (*read_)(void *, void *, unsigned) = nullptr;
  const char *(*error_)(void *) = nullptr;
  int (*close_)(void *) = nullptr;
};

// Integer-factor decimation of interleaved S16 frames: Blackman-windowed sinc low-pass at 0.9 x the
// output Nyquist (32 taps per output sample), per channel. Factor 1 passes the frames through.
class Decimator {
 public:
  Decimator(unsigned factor, unsigned channels) : d_(factor), ch_(channels) {
    if (d_ < 2) return;
    const int n = 32 * int(d_) + 1;
    const double fc = 0.45 / d_, pi = 3.14159265358979323846;
    double sum = 0;
    for (int i = 0; i < n; i++) {
      const double x = i - (n - 1) / 2.0;
      const double s = x == 0 ? 2 * fc : std::sin(2 * pi * fc * x) / (pi * x);
      const double w = 0.42 - 0.5 * std::cos(2 * pi * i / (n - 1)) + 0.08 * std::cos(4 * pi * i / (n - 1));
      taps_.push_back(float(s * w));
      sum += s * w;
    }
    for (float &t : taps_) t = float(t / sum);
  }
  // Appends the output frames for `frames` more input frames to out.
  void process(const int16_t *in, size_t frames, std::vector<int16_t> &out) {
    if (d_ < 2) {
      out.insert(out.end(), in, in + frames * ch_);
      return;
    }
    buf_.insert(buf_.end(), in, in + frames * ch_);
    const size_t n = taps_.size(), total = buf_.size() / ch_;
    size_t pos = 0;
    for (; pos + n <= total; pos += d_)
      for (unsigned c = 0; c < ch_; c++) {
        float acc = 0;
        for (size_t k = 0; k < n; k++) acc += taps_[k] * buf_[(pos + k) * ch_ + c];
        out.push_back(int16_t(std::clamp(acc, -32768.f, 32767.f)));
      }
    buf_.erase(buf_.begin(), buf_.begin() + pos * ch_);
  }

 private:
  unsigned d_, ch_;
  std::vector<float> taps_, buf_;
};

std::unique_ptr<CaptureSource> open_capture(const std::string &dev, bool s32, unsigned nch, unsigned rate, size_t period) {
  if (dev.rfind("tinyalsa:", 0) == 0) return TinyalsaCapture::open(dev, s32, nch, rate, period);
  return AlsaCapture::open(dev.c_str(), s32, nch, rate, period);
}

// Wake word level window: 1.5 s of 32 ms VQE frames (512 samples @ 16 kHz on both models), long
// enough to cover the phrase when the spotter confirms; the loudest frames are the phrase itself.
constexpr size_t kLevelChunks = 47;
constexpr size_t kLevelTop = 8;

// Rounded to 0.1 dB: stations compare levels received as "%.1f" text, so ours must be the same
// number the peers see, or ties could resolve differently on each side.
float wake_level_db(const std::array<float, kLevelChunks> &h) {
  std::array<float, kLevelChunks> s = h;
  std::partial_sort(s.begin(), s.begin() + kLevelTop, s.end(), std::greater<float>());
  float sum = 0;
  for (size_t i = 0; i < kLevelTop; i++) sum += s[i];
  float db = 10.f * std::log10(sum / kLevelTop + 1e-10f);
  db = std::clamp(db, -100.f, 0.f);
  return std::round(db * 10.f) / 10.f;
}

using P = void *;

}  // namespace

struct AudioFrontend::Engine {
  void *lib = nullptr;
  // VQE
  P (*cfg_create)(const char *, P *) = nullptr;
  void (*cfg_free)(P) = nullptr;
  int (*cfg_chunk)(P) = nullptr;
  int (*cfg_frame)(P) = nullptr;
  P (*vqe_create)(P, const char *, size_t, const void *, P *) = nullptr;
  void (*vqe_free)(P) = nullptr;
  P (*vqe_process)(P, const float *, size_t, const float *, size_t) = nullptr;
  P (*vqe_out)(P, int, size_t, const float **, size_t *) = nullptr;
  const char *(*vqe_errmsg)(P) = nullptr;
  void (*vqe_errfree)(P) = nullptr;
  // spotter
  P (*sp_conf_read)(const char *, P *) = nullptr;
  void (*sp_conf_free)(P, P *) = nullptr;
  int (*sp_conf_rate)(P, P *) = nullptr;
  long (*sp_conf_chans)(P, long, P *) = nullptr;
  P (*sp_create)(P, int, int, const int *, bool, P *) = nullptr;
  void (*sp_destroy)(P, P *) = nullptr;
  void (*sp_submit)(P, const void *, size_t, P *) = nullptr;
  int (*sp_next)(P, P *) = nullptr;
  const char *(*sp_phrase)(P, P *) = nullptr;
  const char *(*sp_errmsg)(P) = nullptr;
  void (*sp_errfree)(P) = nullptr;

  struct Spotter {
    explicit Spotter(const char *w) : what(w) {}
    const char *what;
    P sp = nullptr;
    std::vector<int> vqe;  // per spotter input channel: VQE output type
    std::vector<int16_t> buf;
    int64_t last_hit_ms = 0;
  };

  P cfg = nullptr, vqe = nullptr;
  int chunk = 0, frame = 0;
  Spotter wake{"wake word"}, cmd{"command"};

  template <class F>
  bool bind(F &f, const char *name) {
    f = reinterpret_cast<F>(dlsym(lib, name));
    if (!f) LOGE("libyandex_evil: missing %s", name);
    return f != nullptr;
  }

  bool load() {
    // libyandex_evil.so needs libtflite.so (same dir); preload it so no LD_LIBRARY_PATH is required.
    std::string dir = kLibDir;
    if (!dlopen((dir + "libtflite.so").c_str(), RTLD_NOW | RTLD_GLOBAL)) LOGW("libtflite: %s", dlerror());
    lib = dlopen((dir + "libyandex_evil.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
      LOGE("dlopen libyandex_evil: %s", dlerror());
      return false;
    }
    return bind(cfg_create, "YandexVqeC_Config_CreateFromPreset") && bind(cfg_free, "YandexVqeC_Config_Free") &&
           bind(cfg_chunk, "YandexVqeC_Config_GetInputChunkSize") &&
           bind(cfg_frame, "YandexVqeC_Config_GetProcessingFrame") && bind(vqe_create, "YandexVqeC_Create") &&
           bind(vqe_free, "YandexVqeC_Free") && bind(vqe_process, "YandexVqeC_Process") &&
           bind(vqe_out, "YandexVqeC_GetOutputChannelData") && bind(vqe_errmsg, "YandexVqeC_Error_GetMessage") &&
           bind(vqe_errfree, "YandexVqeC_Error_Free") && bind(sp_conf_read, "spotter_conf_read") &&
           bind(sp_conf_free, "spotter_conf_free") && bind(sp_conf_rate, "spotter_conf_native_sample_rate") &&
           bind(sp_conf_chans, "spotter_conf_get_required_channel_count") &&
           bind(sp_create, "spotter_create_multichannel") && bind(sp_destroy, "spotter_destroy") &&
           bind(sp_submit, "spotter_submit_raw_data") && bind(sp_next, "spotter_get_next_event") &&
           bind(sp_phrase, "spotter_get_current_phrase") && bind(sp_errmsg, "spotter_error_get_message") &&
           bind(sp_errfree, "spotter_error_free");
  }

  bool vqe_check(P err, const char *what) {
    if (!err) return true;
    LOGE("%s: %s", what, vqe_errmsg(err));
    vqe_errfree(err);
    return false;
  }
  bool sp_check(P err, const char *what) {
    if (!err) return true;
    LOGE("%s: %s", what, sp_errmsg(err));
    sp_errfree(err);
    return false;
  }

  bool init_vqe(const std::string &preset, const Model &model) {
    if (!vqe_check(cfg_create(preset.c_str(), &cfg), "VQE config")) return false;
    chunk = cfg_chunk(cfg);
    frame = cfg_frame(cfg);
    if (!vqe_check(vqe_create(cfg, kVqeModels, 0, nullptr, &vqe), "VQE create")) return false;
    LOGI("VQE '%s': %d input frames -> %d output samples per chunk", preset.c_str(), chunk, frame);
    // What the preset expects, to catch a profile with the wrong channels. The getters are assumed to
    // be int(cfg) like GetInputChunkSize (not verified on a device yet), so a mismatch only warns.
    auto mics = (int (*)(P))dlsym(lib, "YandexVqeC_Config_GetMicsCount");
    auto spks = (int (*)(P))dlsym(lib, "YandexVqeC_Config_GetSpeakersCount");
    auto rate = (int (*)(P))dlsym(lib, "YandexVqeC_Config_GetInputSamplingRate");
    if (mics && spks && rate) {
      const int m = mics(cfg), s = spks(cfg), r = rate(cfg);
      LOGI("VQE '%s' expects %d mics + %d speaker refs at %d Hz", preset.c_str(), m, s, r);
      if (m != int(model.mic_channels.size()) || s != int(model.spk_channels.size()) || r != int(model.capture_rate))
        LOGW("VQE '%s': the model profile gives %zu mics + %zu speaker refs at %u Hz", preset.c_str(), model.mic_channels.size(),
             model.spk_channels.size(), model.capture_rate);
    }
    return true;
  }

  void free_spotter(Spotter &s) {
    if (!s.sp) return;
    P e = nullptr;
    sp_destroy(s.sp, &e);
    sp_check(e, "spotter_destroy");
    s.sp = nullptr;
  }

  // main_vqe: VQE output for the spotter's main channel (type 0). Measured offline on the device:
  // the wake-word model works with VQE_OMNI_WITH_GAIN, the command model only with VQE_OMNI
  // (10/14 synthetic commands vs 0/14 with the AGC'd channel).
  bool init_spotter(Spotter &s, const std::string &dir, int main_vqe) {
    free_spotter(s);
    P e = nullptr;
    P conf = sp_conf_read(dir.c_str(), &e);
    if (!sp_check(e, "spotter_conf_read") || !conf) return false;
    std::vector<int> types;
    s.vqe.clear();
    for (int t = 0; t < 6; t++) {
      long n = sp_conf_chans(conf, t, &e);
      if (!sp_check(e, "spotter_conf_get_required_channel_count")) n = 0;
      for (long i = 0; i < n && n < 8; i++) {
        types.push_back(t);
        s.vqe.push_back(t == 0 ? main_vqe : kSpotterToVqe[t]);
      }
    }
    int rate = sp_conf_rate(conf, &e);
    sp_check(e, "spotter_conf_native_sample_rate");
    s.sp = sp_create(conf, 16000, (int)types.size(), types.data(), false, &e);
    bool ok = sp_check(e, "spotter_create_multichannel") && s.sp;
    sp_conf_free(conf, &e);
    sp_check(e, "spotter_conf_free");
    if (ok) {
      std::string layout;
      for (size_t i = 0; i < types.size(); i++)
        layout += (i ? "," : "") + std::to_string(types[i]) + "<-vqe" + std::to_string(s.vqe[i]);
      LOGI("%s spotter %s: native rate %d, channels [%s]", s.what, dir.c_str(), rate, layout.c_str());
    }
    return ok;
  }

  // Feeds the current VQE chunk to a spotter; returns the phrases it confirmed.
  std::vector<std::string> feed(Spotter &s) {
    std::vector<std::string> hits;
    const size_t nch = s.vqe.size();
    if (!s.sp || !nch) return hits;
    const float *src[8];
    size_t n = frame;
    for (size_t k = 0; k < nch; k++) {
      size_t len = 0;
      if (!vqe_check(vqe_out(vqe, s.vqe[k], 0, &src[k], &len), "VQE output")) return hits;
      n = std::min(n, len);
    }
    s.buf.resize(n * nch);
    for (size_t i = 0; i < n; i++)
      for (size_t k = 0; k < nch; k++) {
        float v = src[k][i] * 32768.f;
        s.buf[i * nch + k] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : std::lrint(v));
      }
    P err = nullptr;
    sp_submit(s.sp, s.buf.data(), s.buf.size() * sizeof(int16_t), &err);
    if (!sp_check(err, "spotter_submit_raw_data")) return hits;
    for (int guard = 0; guard < 16; guard++) {
      int ev = sp_next(s.sp, &err);
      if (!sp_check(err, "spotter_get_next_event") || ev < 0 || ev > 9 || (kEventsStopMask >> ev & 1)) break;
      if (ev != EV_CONFIRM_1ST && ev != EV_CONFIRM_2ND) continue;
      const char *ph = sp_phrase(s.sp, &err);
      sp_check(err, "spotter_get_current_phrase");
      int64_t now = Loop::now_ms();
      if (now - s.last_hit_ms < 1200) continue;  // one hit per utterance
      s.last_hit_ms = now;
      hits.emplace_back(ph ? ph : "");
      LOGI("%s detected: '%s' (event %d)", s.what, hits.back().c_str(), ev);
    }
    return hits;
  }

  ~Engine() {
    free_spotter(wake);
    free_spotter(cmd);
    if (vqe) vqe_free(vqe);
    if (cfg) cfg_free(cfg);
  }
};

AudioFrontend::AudioFrontend(Loop &loop, const Model &model) : loop_(loop), model_(model) {}

AudioFrontend::~AudioFrontend() { stop(); }

std::vector<int16_t> AudioFrontend::recent(size_t samples) const {
  if (samples > kRingSamples) samples = kRingSamples;
  std::vector<int16_t> out(samples);
  std::lock_guard<std::mutex> lk(ring_mu_);
  for (size_t i = 0; i < samples; i++) out[i] = ring_[(ring_pos_ + kRingSamples - samples + i) % kRingSamples];
  return out;
}

bool AudioFrontend::start(const std::string &model_dir, const std::string &command_model_dir) {
  eng_ = std::make_unique<Engine>();
  if (!eng_->load() || !eng_->init_vqe(model_.vqe_preset, model_) ||
      !eng_->init_spotter(eng_->wake, model_dir, VQE_OMNI_WITH_GAIN)) {
    eng_.reset();
    return false;
  }
  if (!command_model_dir.empty() && !eng_->init_spotter(eng_->cmd, command_model_dir, VQE_OMNI))
    LOGW("command spotter unavailable; voice commands without the wake word are disabled");
  running_ = true;
  thr_ = std::thread([this] { run(); });
  return true;
}

void AudioFrontend::stop() {
  {
    std::lock_guard<std::mutex> lk(stop_mu_);
    running_ = false;
  }
  stop_cv_.notify_all();
  if (thr_.joinable()) thr_.join();
  eng_.reset();
}

void AudioFrontend::set_model(const std::string &model_dir) {
  std::lock_guard<std::mutex> lk(model_mu_);
  pending_model_ = model_dir;
}

// Two threads: a capture thread that only drains ALSA into a FIFO, and this processing thread
// (VQE + spotters). The Lite 2's PDM capture allows at most ~42 ms of buffer (2048 frames at
// 8 ch S16); VQE chunks occasionally take longer than the remaining slack, which overran the
// device buffer when reading and processing shared one thread.
void AudioFrontend::run() {
  Engine &e = *eng_;
  const size_t period = size_t(e.chunk);
  const unsigned nch = model_.capture_channels;
  const auto &mic_ch = model_.mic_channels, &spk_ch = model_.spk_channels;
  const size_t nmic = mic_ch.size(), nspk = spk_ch.size();
  const size_t fifo_max = size_t(model_.capture_rate) * nch;  // 1 s of audio
  const size_t chunk_samples = period * nch;

  std::mutex fifo_mu;
  std::condition_variable fifo_cv;
  std::vector<int16_t> fifo;  // interleaved frames, oldest first

  std::thread reader([&] {
    const bool s32 = model_.capture_format == "s32x2";
    const unsigned dev_ch = s32 ? nch / 2 : nch;
    std::unique_ptr<CaptureSource> src;
    std::vector<int32_t> raw(s32 ? period * dev_ch : 0);
    std::vector<int16_t> in(period * nch);
    while (running_) {
      if (!src && !(src = open_capture(model_.capture_pcm, s32, dev_ch, model_.capture_rate, period))) {
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::seconds(2), [this] { return !running_.load(); });
        continue;
      }
      long n = src->read(s32 ? (void *)raw.data() : (void *)in.data(), period);
      if (n == 0) continue;
      if (n < 0) {
        {
          // The chunk being assembled has a gap in it: drop it (the processing thread takes
          // whole chunks from the front, so the partial one is the tail).
          std::lock_guard<std::mutex> lk(fifo_mu);
          fifo.resize(fifo.size() - fifo.size() % chunk_samples);
        }
        if (n == CaptureSource::kLost) src.reset();
        continue;
      }
      if (s32)
        for (size_t i = 0; i < size_t(n) * dev_ch; i++) {
          const uint32_t v = uint32_t(raw[i]);
          in[2 * i] = int16_t(v >> 16), in[2 * i + 1] = int16_t(v & 0xFFFF);
        }
      std::lock_guard<std::mutex> lk(fifo_mu);
      fifo.insert(fifo.end(), in.begin(), in.begin() + n * nch);
      if (fifo.size() > fifo_max) {
        LOGW("capture: processing behind, dropping %zu frames", (fifo.size() - fifo_max) / nch);
        fifo.erase(fifo.begin(), fifo.end() - fifo_max);
      }
      fifo_cv.notify_one();
    }
  });

  // Speaker references from a second device (Model::ref_pcm), resampled to the capture rate. Its
  // clock is not the mic ADC's (Station 1: the Loopback runs on a kernel timer), so the FIFO is kept
  // between one and four chunks: the processing thread pads a missing chunk with silence, excess
  // beyond four chunks is dropped.
  const unsigned nref = model_.ref_pcm.empty() ? 0 : model_.ref_channels;
  std::vector<int16_t> ref_fifo;  // guarded by fifo_mu
  std::thread ref_reader;
  if (nref)
    ref_reader = std::thread([&] {
      const unsigned factor = model_.ref_rate / model_.capture_rate;
      const bool s32 = model_.ref_format == "s32";
      const size_t ref_period = period * factor;
      Decimator dec(factor, nref);
      std::unique_ptr<CaptureSource> src;
      std::vector<int32_t> raw(s32 ? ref_period * nref : 0);
      std::vector<int16_t> in16(ref_period * nref), out;
      while (running_) {
        if (!src && !(src = open_capture(model_.ref_pcm, s32, nref, model_.ref_rate, ref_period))) {
          std::unique_lock<std::mutex> lk(stop_mu_);
          stop_cv_.wait_for(lk, std::chrono::seconds(2), [this] { return !running_.load(); });
          continue;
        }
        long n = src->read(s32 ? (void *)raw.data() : (void *)in16.data(), ref_period);
        if (n == 0) continue;
        if (n < 0) {
          if (n == CaptureSource::kLost) src.reset();
          continue;
        }
        if (s32)
          for (size_t i = 0; i < size_t(n) * nref; i++) in16[i] = int16_t(raw[i] >> 16);
        out.clear();
        dec.process(in16.data(), size_t(n), out);
        std::lock_guard<std::mutex> lk(fifo_mu);
        ref_fifo.insert(ref_fifo.end(), out.begin(), out.end());
        if (ref_fifo.size() > 4 * period * nref) {
          LOGW("reference capture: %zu frames ahead of the mics, dropped", ref_fifo.size() / nref - period);
          ref_fifo.erase(ref_fifo.begin(), ref_fifo.end() - period * nref);
        }
      }
    });
  std::vector<int16_t> ref(period * nref);
  bool ref_short_logged = false;

  std::vector<int16_t> in(chunk_samples);
  std::vector<float> mics(period * nmic), spk(period * nspk);
  std::array<float, kLevelChunks> level_hist{};  // mean square per VQE frame
  size_t level_pos = 0;
  // The analyser wants 512 samples at 16 kHz per call; when the VQE chunk is another size (Mini 3: 10 ms) the
  // speaker reference is collected up to that size first.
  const size_t ana_frames = model_.capture_rate >= 16000 && model_.capture_rate % 16000 == 0 ? 512 * (model_.capture_rate / 16000) : 0;
  SpectrumAnalyzer ana(model_.capture_rate, ana_frames ? ana_frames : period);
  if (!ana.ok()) LOGW("music analysis: unsupported capture layout");
  std::vector<float> ana_acc;
  bool music_was_on = false;

  while (running_) {
    {
      std::lock_guard<std::mutex> lk(model_mu_);
      if (!pending_model_.empty()) {
        if (!e.init_spotter(e.wake, pending_model_, VQE_OMNI_WITH_GAIN))
          LOGE("failed to switch wake word model to %s", pending_model_.c_str());
        pending_model_.clear();
      }
    }
    // Take exactly one VQE chunk.
    {
      std::unique_lock<std::mutex> lk(fifo_mu);
      if (!fifo_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return fifo.size() >= in.size(); }))
        continue;
      std::copy(fifo.begin(), fifo.begin() + in.size(), in.begin());
      fifo.erase(fifo.begin(), fifo.begin() + in.size());
      if (nref) {
        const size_t have = std::min(ref.size(), ref_fifo.size());
        std::copy(ref_fifo.begin(), ref_fifo.begin() + have, ref.begin());
        std::fill(ref.begin() + have, ref.end(), 0);
        ref_fifo.erase(ref_fifo.begin(), ref_fifo.begin() + have);
        if (have < ref.size() && !ref_short_logged) {
          LOGW("reference capture behind the mics: silence in its place");
          ref_short_logged = true;
        }
      }
    }

    for (size_t i = 0; i < period; i++) {
      const int16_t *f = &in[i * nch];
      for (size_t c = 0; c < nmic; c++) mics[i * nmic + c] = f[mic_ch[c]] * (1.0f / 32768.f);
      for (size_t c = 0; c < nspk; c++) {
        const unsigned l = spk_ch[c];
        spk[i * nspk + c] = (l < nch ? f[l] : ref[i * nref + (l - nch)]) * (1.0f / 32768.f);
      }
    }
    const bool music_on = music_.load() && ana.ok();
    if (music_on && !music_was_on) ana.reset();
    music_was_on = music_on;
    MusicFrame mf;
    if (music_on && period != ana_frames) {
      ana_acc.insert(ana_acc.end(), spk.begin(), spk.end());
      if (ana_acc.size() >= ana_frames * nspk) {
        if (ana.process(ana_acc.data(), nspk, mf))
          loop_.post([this, mf] {
            if (music_cb_) music_cb_(mf);
          });
        ana_acc.erase(ana_acc.begin(), ana_acc.begin() + ana_frames * nspk);
      }
    } else if (music_on && ana.process(spk.data(), nspk, mf)) {
      loop_.post([this, mf] {
        if (music_cb_) music_cb_(mf);
      });
    }
    if (!music_on) ana_acc.clear();
    if (!e.vqe_check(e.vqe_process(e.vqe, mics.data(), mics.size(), spk.data(), spk.size()), "VQE process")) continue;

    // VQE_OMNI is cleaned of the station's own playback but has no AGC, so it keeps the distance to
    // the talker; VQE_OMNI_WITH_GAIN, which the spotter hears, normalizes it away.
    {
      const float *d = nullptr;
      size_t n = 0;
      if (e.vqe_check(e.vqe_out(e.vqe, VQE_OMNI, 0, &d, &n), "VQE output") && n) {
        double sq = 0;
        for (size_t i = 0; i < n; i++) sq += (double)d[i] * d[i];
        level_hist[level_pos++ % kLevelChunks] = (float)(sq / n);
      }
    }

    {
      const float *d = nullptr;
      size_t n = 0;
      if (e.vqe_check(e.vqe_out(e.vqe, VQE_OMNI_WITH_GAIN, 0, &d, &n), "VQE output") && n) {
        auto chunk = std::make_shared<std::vector<int16_t>>(n);
        for (size_t i = 0; i < n; i++) {
          float v = d[i] * 32768.f;
          (*chunk)[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : std::lrint(v));
        }
        {  // the history behind recent()
          std::lock_guard<std::mutex> lk(ring_mu_);
          for (size_t i = 0; i < n; i++) {
            ring_[ring_pos_] = (*chunk)[i];
            ring_pos_ = (ring_pos_ + 1) % kRingSamples;
          }
        }
        if (stream_.load() && audio_cb_)
          loop_.post([this, chunk] {
            if (audio_cb_) audio_cb_(chunk->data(), chunk->size());
          });
      }
    }

    if (detect_.load()) {
      auto phrases = e.feed(e.wake);
      if (!phrases.empty()) {
        float level = wake_level_db(level_hist);
        for (auto &phrase : phrases)
          loop_.post([this, phrase, level] {
            if (wake_cb_) wake_cb_(phrase, level);
          });
      }
    }
    if (commands_.load()) {
      for (auto &phrase : e.feed(e.cmd))
        loop_.post([this, phrase] {
          if (command_cb_) command_cb_(phrase);
        });
    }
  }
  reader.join();
  if (ref_reader.joinable()) ref_reader.join();
}

}  // namespace cs
