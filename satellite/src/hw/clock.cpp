// 4-digit 7-segment clock, one sysfs LED per segment (names per Model).
//
// Station Midi: two aw21036/aw21012 chips on i2c 3-0037 / 3-0021,
// /sys/class/leds/cucumber:white:clock-<seg><digit>.
//
// Segment mapping (derived from the live display driven by maind: at 21:17 local time the lit
// set was a1 b1 d1 e1 g1 | b2 c2 | b3 c3 | a4 b4 c4 plus p3 q3, i.e. "21:17"):
//   clock-<s><n>: n = digit position 1..4 left to right, s = standard 7-segment letter
//
//          aaa
//         f   b
//          ggg          digits: [1][2] : [3][4]
//         e   c
//          ddd
//
//   p3 + q3     colon (upper/lower dot) - the only extra LEDs maind lights for the time.
//   deg         degree sign; h1 h2, l1 l2, m1 m2, p1 p2 p4 p5, q1 q2 q4 q5: auxiliary dots/icons
//               whose position is not known (never lit by maind for the time); kept off here.
//   cucumber:sync:clock-0/1 are the chips' sync pseudo-LEDs (not segments).
// Midi brightness: like maind, segments are switched 0/255 and dimming uses the chips' global
// current attribute (/sys/bus/i2c/devices/{3-0037,3-0021}/current, 0..255; maind wrote 5 for its
// stored brightness 0.0196 = 5/255). The brightness_scale tables are left untouched.
//
// Station Lite 2: /sys/class/leds/plum:white:clock<SEG><pos>, SEG = A..G (standard segments),
// pos = 0..3 left to right, colon = DH + DL; H0..H3, DM and DEGREE stay dark (maind's
// /leds/clock frameBuilder config in quasar.cfg). No global current: maind dims through the
// per-segment brightness value (0..255), and so do we.
//
// Time is local time via localtime_r, i.e. it honours the process TZ (the device has no
// /etc/localtime or zoneinfo, so the daemon must setenv("TZ", "<POSIX TZ>") + tzset(), e.g. from
// HA's GetTimeResponse.timezone; otherwise UTC is shown).
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "audio/music_frame.h"
#include "core/log.h"
#include "hw/ambient.h"
#include "hw/hw.h"
#include "hw/sysfs.h"

namespace cs {

namespace {

constexpr int kDigits = 4;
constexpr int kSegs = 7;  // a..g
constexpr int64_t kTickMs = 250;
constexpr int64_t kFlashMs = 1500;
constexpr int64_t kCodePageMs = 2000;
constexpr int kMinLevel = 2;  // lowest current / brightness that still lights the segments

// Bit i = segment 'a'+i.
constexpr uint8_t kFont[10] = {
    0x3f,  // 0 abcdef
    0x06,  // 1 bc
    0x5b,  // 2 abdeg
    0x4f,  // 3 abcdg
    0x66,  // 4 bcfg
    0x6d,  // 5 acdfg
    0x7d,  // 6 acdefg
    0x07,  // 7 abc
    0x7f,  // 8 abcdefg
    0x6f,  // 9 abcdfg
};
constexpr uint8_t kMinus = 0x40;  // g

// Printable ASCII 0x20..0x7E, from github.com/dmadison/LED-Segment-ASCII 7-Segment-ASCII_HEX.txt
// (MIT, Copyright (c) 2017 David Madison); decimal point bits dropped, '.' drawn like ','.
constexpr uint8_t kAscii[95] = {
    0x00, 0x06, 0x22, 0x7E, 0x6D, 0x52, 0x46, 0x20, 0x29, 0x0B, 0x21, 0x70, 0x10, 0x40, 0x10, 0x52,  // ' '../
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x09, 0x0D, 0x61, 0x48, 0x43, 0x53,  // 0..?
    0x5F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71, 0x3D, 0x76, 0x30, 0x1E, 0x75, 0x38, 0x15, 0x37, 0x3F,  // @..O
    0x73, 0x6B, 0x33, 0x6D, 0x78, 0x3E, 0x3E, 0x2A, 0x76, 0x6E, 0x5B, 0x39, 0x64, 0x0F, 0x23, 0x08,  // P.._
    0x02, 0x5F, 0x7C, 0x58, 0x5E, 0x7B, 0x71, 0x6F, 0x74, 0x10, 0x0C, 0x75, 0x30, 0x14, 0x54, 0x5C,  // `..o
    0x73, 0x67, 0x50, 0x6D, 0x78, 0x1C, 0x1C, 0x14, 0x76, 0x6E, 0x5B, 0x46, 0x30, 0x70, 0x01,        // p..~
};

// Cyrillic А..Я approximations; lower case uses the same glyphs.
constexpr uint8_t kCyrillic[32] = {
    0x77, 0x7D, 0x7F, 0x31, 0x5E, 0x79, 0x76, 0x4F,  // А Б В Г Д Е Ж З
    0x3E, 0x3E, 0x75, 0x37, 0x15, 0x76, 0x3F, 0x37,  // И Й К Л М Н О П
    0x73, 0x39, 0x78, 0x6E, 0x71, 0x76, 0x3E, 0x66,  // Р С Т У Ф Х Ц Ч
    0x2A, 0x2A, 0x7C, 0x7C, 0x7C, 0x4F, 0x3F, 0x67,  // Ш Щ Ъ Ы Ь Э Ю Я
};

uint8_t glyph_for(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return kAscii[cp - 0x20] & 0x7F;
  if (cp >= 0x410 && cp <= 0x44F) return kCyrillic[(cp - 0x410) % 32];
  if (cp == 0x401 || cp == 0x451) return kCyrillic[5];  // Ё ё
  if (cp == 0xB0) return 0x63;                          // °
  return 0;
}

struct Cell {
  uint8_t glyph;
  bool colon_after;
};

// One cell per code point; ':' lights the colon after the previous cell. Malformed UTF-8 bytes
// become blank cells, one per byte.
std::vector<Cell> layout_text(const std::string &utf8) {
  std::vector<Cell> cells;
  const size_t n = utf8.size();
  for (size_t i = 0; i < n;) {
    unsigned char b = utf8[i];
    uint32_t cp = 0xFFFD;
    size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (len == 1) {
      cp = b;
    } else if (len && i + len <= n) {
      uint32_t v = b & (0x7F >> len);
      size_t k = 1;
      for (; k < len && (utf8[i + k] & 0xC0) == 0x80; k++) v = v << 6 | (utf8[i + k] & 0x3F);
      if (k == len) cp = v;
      else len = 0;
    } else {
      len = 0;
    }
    i += len ? len : 1;
    if (cp == ':') {
      if (!cells.empty()) cells.back().colon_after = true;
      continue;
    }
    cells.push_back({glyph_for(cp), false});
  }
  return cells;
}

// Faces from the "LED Studio" page (Text.html), positions left to right.
struct Face {
  const char *name;
  uint8_t g[kDigits];
};
constexpr Face kFaces[] = {
    {"Smiley", {0x40, 0x18, 0x0C, 0x40}},    {"Sad", {0x40, 0x21, 0x03, 0x40}},
    {"Neutral", {0x40, 0x40, 0x40, 0x40}},   {"Surprised", {0x40, 0x09, 0x00, 0x40}},
    {"Angry", {0x41, 0x40, 0x40, 0x41}},     {"Wink", {0x40, 0x18, 0x0C, 0x00}},
    {"Big Smile", {0x40, 0x38, 0x1C, 0x40}}, {"Sleepy", {0x08, 0x40, 0x40, 0x08}},
    {"Dead", {0x74, 0x08, 0x08, 0x74}},      {"Cool", {0x22, 0x18, 0x0C, 0x22}},
    {"Love", {0x30, 0x18, 0x0C, 0x30}},      {"Robot", {0x3F, 0x09, 0x00, 0x3F}},
    {"UwU", {0x1C, 0x1C, 0x1C, 0x1C}},       {"UwU thin", {0x1C, 0x14, 0x14, 0x1C}},
    {"UwU happy", {0x23, 0x1C, 0x1C, 0x23}}, {"OwO", {0x23, 0x08, 0x08, 0x23}},
    {">w<", {0x46, 0x1C, 0x1C, 0x70}},
};
constexpr int kStaticFaces = int(sizeof kFaces / sizeof kFaces[0]);
constexpr int kFaceBlink = kStaticFaces, kFaceAll = kStaticFaces + 1, kFaceCount = kStaticFaces + 2;
constexpr uint8_t kBlinkOpen[kDigits] = {0x23, 0x08, 0x08, 0x23};
constexpr uint8_t kBlinkShut[kDigits] = {0x40, 0x08, 0x08, 0x40};
constexpr int64_t kBlinkPeriodMs = 3000, kBlinkShutMs = 200, kBlinkStepMs = 100;
constexpr int64_t kFaceCycleMs = 2000;
constexpr int64_t kTemporaryMs = 5000, kTemporaryMaxS = 3600;
// Smooth animations (show_temporary_anim): frame step, fades, resolution of the speech envelope.
constexpr int64_t kAnimStepMs = 40, kAnimFadeInMs = 200, kAnimFadeOutMs = 350, kClockFadeMs = 350, kEnvStepMs = 40;
constexpr int kAnimListen = 1, kAnimThink = 2, kAnimSpeak = 3;

}  // namespace

struct Clock::Impl {
  Impl(Loop &l, const Model &m) : loop(l), model(m) {}
  Loop &loop;
  const Model &model;

  int seg_fd[kDigits][kSegs];
  std::vector<int> colon_fd;
  std::vector<int> current_fds;
  // last written state (-1 = unknown)
  int seg_last[kDigits][kSegs];
  int colon_last = -1;
  std::function<void(const std::string &)> on_frame;   // see Clock::set_frame_callback
  std::string frame_last;
  int64_t frame_t = 0;
  int level_last = -1;
  int on_value = 255;  // value of a lit segment (dimmed level when there is no chip current)

  bool enabled = true;
  float user_bri = 1.f;
  float lux = -1.f;  // <0: unknown, no auto-dimming
  AmbientDimmer dimmer;
  int64_t timer_end = 0;     // monotonic ms of a running countdown; 0 = none
  int64_t timer_paused = 0;  // ms left of a paused countdown; 0 = none
  int64_t flash_until = 0;
  int flash_value = 0;
  std::string code;  // pairing code digits shown instead of anything else (empty = none)
  int64_t code_since = 0;
  uint64_t tick = 0;
  unsigned refreshes = 0;

  struct Content {
    enum Kind { NONE, TEXT, FACE, ANIM, SWEEP, EQ, PAUSE, BOOT } kind = NONE;
    std::vector<Cell> cells;  // TEXT
    int face = -1;            // FACE: index into face_names()
    int64_t since = 0;        // origin of scroll / animation phase
    int anim = 0;             // ANIM: kAnimListen / kAnimThink / kAnimSpeak
    std::vector<uint8_t> env; // ANIM speak: loudness, one byte per kEnvStepMs
    int64_t fade_from = 0;    // ANIM: start of the fade-in
    bool degree = false;      // TEXT ended with a degree sign: digits right-aligned, the DEGREE LED lit
    int dir = 1;              // SWEEP: +1 runs right, -1 left
    bool pingpong = false;    // SWEEP: runs left and right again and again (a ringing timer / alarm)
    int then_eq_ms = 0;       // SWEEP: equalizer for this long afterwards
    bool decay = false;       // EQ: bars fall to nothing (pause)
  };
  // Equalizer on the display: 8 bands -> 4 digits, three levels per digit (bottom, middle, top bar).
  std::array<float, 8> eq_t{}, eq_cur{};
  bool eq_always = false, music_on = false, eq_bars = false;
  std::array<float, 32> loud_hist{};   // smoothed loudness history for the wave
  unsigned loud_pos = 0;
  float loud_smooth = 0.f;
  float loud_back(int back) const { return loud_hist[(loud_pos + 31 - unsigned(back)) % 32]; }
  int64_t eq_always_since = 0;
  void eq_step(bool decay) {
    for (int i = 0; i < 8; i++) {
      const float t = decay ? 0.f : eq_t[i];
      const float k = t > eq_cur[i] ? 0.55f : (decay ? 0.22f : 0.18f);
      eq_cur[i] += (t - eq_cur[i]) * k;
    }
  }
  void render_eq(float lv[kDigits][kSegs]) const {
    if (!eq_bars) {  // the wave of the pre-lostos AirPlay: G glows with the loudness, A and D only on loud passages;
                     // the middle digits lead and the outer ones follow, so it ripples outwards
      static const int lag[4] = {3, 0, 0, 3};
      for (int d = 0; d < kDigits; d++) {
        for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
        const float l = loud_back(lag[d]);
        float peak = std::max(0.f, std::min(1.f, (l - 0.45f) / 0.55f));
        lv[d][6] = 0.024f + l * 0.976f;
        lv[d][0] = lv[d][3] = peak * peak;
      }
      return;
    }
    for (int d = 0; d < kDigits; d++) {
      for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
      const float h = std::max(0.f, std::min(1.f, 0.5f * (eq_cur[2 * d] + eq_cur[2 * d + 1])));
      lv[d][3] = std::min(1.f, h * 3.f);
      lv[d][6] = std::max(0.f, std::min(1.f, h * 3.f - 1.f));
      lv[d][0] = std::max(0.f, std::min(1.f, h * 3.f - 2.f));
    }
  }
  // The stock "pause" pattern (player_pause): the two bars of a pause sign in the middle digits swell to full in
  // ~280 ms and die away slowly.
  void render_pause(const Content &c, int64_t now, float lv[kDigits][kSegs]) const {
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
    const float t = float(now - c.since);
    float v = t < 280.f ? 0.07f + 0.93f * t / 280.f : std::max(0.08f, 1.f - (t - 280.f) / 700.f);
    lv[1][1] = lv[1][2] = v;   // right bar of digit 2
    lv[2][4] = lv[2][5] = v;   // left bar of digit 3
  }
  // The boot show (see Clock::show_boot): reveal from the middle, dance to the sound, end as the eyes.
  void render_boot(const Content &c, int64_t now, float lv[kDigits][kSegs]) const {
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
    const int n = int(c.env.size());
    if (!n) return;
    const float t = float(now - c.since);
    const float total = float(n) * float(kEnvStepMs);   // the length of the sound; the eyes stay for a while after it
    auto smooth = [](float x) {
      x = std::max(0.f, std::min(1.f, x));
      return x * x * (3.f - 2.f * x);
    };
    auto level = [&](int back) {
      const int i = std::max(0, std::min(n - 1, int(t / float(kEnvStepMs)) - back));
      return float(c.env[size_t(i)]) / 255.f;
    };
    static const int lag[4] = {3, 0, 0, 3};              // the middle digits lead, the outer ones follow
    static const float delay[4] = {900.f, 0.f, 300.f, 1200.f};   // the line is revealed from the middle outwards
    const float eyes = smooth((t - (total - 1600.f)) / 900.f);   // the last 1.6 s: the bars give way to the eyes
    for (int d = 0; d < kDigits; d++) {
      const float reveal = smooth((t - delay[d]) / 700.f) * (1.f - eyes);
      const float l = level(lag[d]);
      const float peak = std::max(0.f, std::min(1.f, (l - 0.45f) / 0.55f));
      lv[d][6] = reveal * (0.05f + 0.95f * l);
      lv[d][0] = lv[d][3] = reveal * peak * peak;
    }
    float open = 1.f;                                    // the eyes blink every 1.6 s, once they are fully there
    if (t > total - 700.f) {
      const float ph = std::fmod(t - (total - 700.f), 1600.f);
      if (ph > 1400.f) {
        open = std::fabs(2.f * (ph - 1400.f) / 180.f - 1.f);
        open = open * open * (3.f - 2.f * open);
      }
    }
    for (int d : {0, 3}) {                               // the eyes of Alice: A, B, F, G of the outer digits
      lv[d][0] = std::max(lv[d][0], eyes * open);
      lv[d][1] = lv[d][5] = eyes * open;
      lv[d][6] = std::max(lv[d][6], eyes);
    }
  }
  // A glowing head runs across the eight vertical bars (F,E left and B,C right of each digit).
  void render_sweep(const Content &c, int64_t now, float lv[kDigits][kSegs]) const {
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
    float p = std::max(0.f, std::min(1.f, float(now - c.since) / 800.f));
    if (c.pingpong) {
      const float ph = float((now - c.since) % 1800) / 900.f;   // there and back in 1.8 s
      p = ph <= 1.f ? ph : 2.f - ph;
    }
    p = p * p * (3.f - 2.f * p);
    const float x = c.dir >= 0 ? -1.5f + p * 10.f : 8.5f - p * 10.f;   // next (+1) runs left -> right (settled with the user: do not flip again)
    for (int col = 0; col < 8; col++) {
      const float v = std::max(0.f, 1.f - std::fabs(float(col) - x) / 1.7f);
      const int d = col / 2;
      if (col % 2 == 0) lv[d][4] = lv[d][5] = v;
      else lv[d][1] = lv[d][2] = v;
    }
  }
  int deg_fd = -1;            // the DEGREE LED (also the "alarm set" indicator on the Mini 3)
  int deg_last = -1;
  mutable bool deg_wanted = false;
  bool alarm_led = false;
  bool hold = true;       // blank until released (see Clock::set_hold)
  bool ringing = false;   // the ringing animation owns the overlay: track changes and the like do not replace it
  void set_deg(bool on, float scale = 1.f) {
    const int v = on ? int(std::lround(on_value * scale)) : 0;
    if (deg_fd < 0 || deg_last == v) return;
    hwutil::write_int_fd(deg_fd, v);
    deg_last = v;
  }
  int64_t clock_fade_from = 0;  // an animation just ended: the clock fades in from here
  Content base, overlay;  // base: Text/Select entities; overlay: temporary layer (actions)
  int64_t overlay_until = 0;
  int scroll_ms = 400;
  uint64_t anim = 0;  // one-shot timer re-armed at each frame boundary of moving content

  ~Impl() {
    if (tick) loop.cancel_timer(tick);
    if (anim) loop.cancel_timer(anim);
    for (auto &d : seg_fd)
      for (int fd : d)
        if (fd >= 0) close(fd);
    for (int fd : colon_fd)
      if (fd >= 0) close(fd);
    for (int fd : current_fds) close(fd);
  }

  int open_led(const std::string &name) const {
    return ::open((model.clock_dir + name + "/brightness").c_str(), O_WRONLY | O_CLOEXEC);
  }

  std::string seg_name(int digit, int seg) const {
    return std::string(1, char(model.clock_seg_base + seg)) + std::to_string(model.clock_pos_base + digit);
  }

  void forget_written() {
    for (auto &d : seg_last)
      for (int &v : d) v = -1;
    colon_last = -1;
    deg_last = -1;
  }

  void set_seg_v(int d, int s, int v) {
    if (seg_last[d][s] == v || seg_fd[d][s] < 0) return;
    if (hwutil::write_int_fd(seg_fd[d][s], v)) seg_last[d][s] = v;
  }

  void set_seg(int d, int s, bool on, float scale = 1.f) {
    set_seg_v(d, s, on ? int(std::lround(on_value * scale)) : 0);
  }

  void set_colon(bool on, float scale = 1.f) {
    int v = on ? int(std::lround(on_value * scale)) : 0;
    if (colon_last == v) return;
    for (int fd : colon_fd) hwutil::write_int_fd(fd, v);
    colon_last = v;
  }

  // Per-segment brightness 0..1 (needs per-segment dimming: not on chip-current models).
  void show_levels(const float lv[kDigits][kSegs], float scale) {
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) {
        float f = enabled ? std::max(0.f, std::min(1.f, lv[d][s] * scale)) : 0.f;
        set_seg_v(d, s, int(std::lround(on_value * f)));
      }
    set_colon(false);
  }

  bool clock_fading(int64_t now) const { return clock_fade_from && now - clock_fade_from < kClockFadeMs; }
  float clock_scale(int64_t now) const {
    if (!clock_fade_from) return 1.f;
    float s = float(now - clock_fade_from) / float(kClockFadeMs);
    return s >= 1.f ? 1.f : s * s * (3.f - 2.f * s);  // smoothstep
  }

  float ambient() const { return dimmer.factor(); }  // 0.02..1, 1 while lux is unknown

  void apply_level() {
    int lvl = int(std::lround(255.f * user_bri * ambient()));
    lvl = user_bri <= 0 ? 0 : std::max(kMinLevel, std::min(255, lvl));
    if (lvl == level_last) return;
    level_last = lvl;
    LOGD("clock: level=%d (user %.2f, lux %.1f)", lvl, user_bri, lux);
    if (model.clock_chip_current) {
      for (int fd : current_fds) hwutil::write_int_fd(fd, lvl);
    } else {
      on_value = lvl;  // lit segments are rewritten with the new value on the next refresh
      forget_written();
    }
  }

  void publish_frame(int64_t now) {
    if (!on_frame) return;
    static const char *hx = "0123456789abcdef";
    auto q = [&](int v) { return hx[v <= 0 || on_value <= 0 ? 0 : std::min(15, (v * 15 + on_value / 2) / on_value)]; };
    std::string f;
    f.reserve(30);
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) f += q(seg_last[d][s]);
    f += q(colon_last);
    f += q(deg_last);
    if (f == frame_last || now - frame_t < 40) return;     // the next tick sends what is still different
    frame_last = f;
    frame_t = now;
    on_frame(f);
  }

  void show(const uint8_t glyph[kDigits], bool colon, bool force = false, float scale = 1.f) {
    const bool on = enabled || force;
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) set_seg(d, s, on && (glyph[d] >> s & 1), scale);
    set_colon(on && colon, scale);
    set_deg(on && (deg_wanted || alarm_led), deg_wanted ? scale : scale * 0.6f);   // the alarm mark is dimmer than a degree sign
  }

  // Up to 4 digits shown as is; longer codes page through 3-digit groups ("123-", "-456"),
  // matching the 3-3 grouping the Sendspin spec asks for.
  void show_code(int64_t now) {
    uint8_t g[kDigits] = {0, 0, 0, 0};
    auto glyph = [](char c) -> uint8_t { return c >= '0' && c <= '9' ? kFont[c - '0'] : kMinus; };
    if (code.size() <= size_t(kDigits)) {
      for (size_t i = 0; i < code.size(); i++) g[kDigits - code.size() + i] = glyph(code[i]);
    } else {
      size_t groups = (code.size() + 2) / 3;
      size_t page = size_t((now - code_since) / kCodePageMs) % groups;
      std::string grp = code.substr(page * 3, 3);
      if (page == 0) {
        for (size_t i = 0; i < grp.size(); i++) g[i] = glyph(grp[i]);
        g[3] = kMinus;
      } else {
        g[0] = kMinus;
        for (size_t i = 0; i < grp.size(); i++) g[1 + i] = glyph(grp[i]);
      }
    }
    show(g, false, true);
  }

  static void two(uint8_t *g, int v, bool blank_lead) {
    g[0] = (blank_lead && v < 10) ? 0 : kFont[(v / 10) % 10];
    g[1] = kFont[v % 10];
  }

  const Content *shown_content() const {
    if (overlay.kind != Content::NONE) return &overlay;
    if (base.kind != Content::NONE) return &base;
    return nullptr;
  }

  // Levels 0..1 of every segment for an animation at time `now`. Segments: a b c d e f g = 0..6.
  //   listen: the middle bar (g) breathes along the four digits;
  //   think:  a soft highlight sweeps back and forth along it;
  //   speak:  two eyes (a b f g of the outer digits; g alone = closed) that blink and glow with the
  //           speech envelope.
  void render_anim(const Content &c, int64_t now, float lv[kDigits][kSegs]) const {
    for (int d = 0; d < kDigits; d++)
      for (int s = 0; s < kSegs; s++) lv[d][s] = 0.f;
    const float t = float(now - c.since), tau = 6.2831853f;
    if (c.anim == kAnimListen) {
      for (int d = 0; d < kDigits; d++) lv[d][6] = 0.30f + 0.70f * (0.5f + 0.5f * std::sin(tau * t / 1500.f - d * 0.8f));
    } else if (c.anim == kAnimThink) {
      const float p = 1.5f + 1.5f * std::sin(tau * t / 1300.f);
      for (int d = 0; d < kDigits; d++) lv[d][6] = 0.10f + 0.90f * std::exp(-(d - p) * (d - p) / 0.45f);
    } else if (c.anim == kAnimSpeak) {
      float amp = 0.85f;
      if (!c.env.empty()) {
        const float x = t / float(kEnvStepMs);
        const size_t i = size_t(std::max(0.f, x));
        const float f = x - std::floor(x);
        const float a = i < c.env.size() ? c.env[i] : 0.f, b = i + 1 < c.env.size() ? c.env[i + 1] : 0.f;
        amp = 0.55f + 0.45f * ((a + (b - a) * f) / 255.f);
      }
      float open = 1.f;  // blink every 3.2 s, 180 ms long
      const float ph = std::fmod(t, 3200.f);
      if (ph > 3000.f) open = std::fabs(2.f * (ph - 3000.f) / 180.f - 1.f);
      open = open * open * (3.f - 2.f * open);
      for (int d : {0, 3}) {
        lv[d][0] = lv[d][1] = lv[d][5] = open * amp;
        lv[d][6] = amp;
      }
    }
  }

  int64_t anim_step(const Content &c) const {
    if (c.kind == Content::ANIM || c.kind == Content::SWEEP || c.kind == Content::EQ || c.kind == Content::PAUSE || c.kind == Content::BOOT) return kAnimStepMs;
    if (c.kind == Content::TEXT && c.degree) return kAnimStepMs;  // fades in and out
    if (c.kind == Content::FACE && c.face != kFaceBlink && c.face != kFaceAll) return kAnimStepMs;   // the fade-out of a face
    if (c.kind == Content::TEXT && c.cells.size() > size_t(kDigits)) return scroll_ms;
    if (c.kind == Content::FACE && c.face == kFaceBlink) return kBlinkStepMs;
    if (c.kind == Content::FACE && c.face == kFaceAll) return kFaceCycleMs;
    return 0;
  }

  void render(const Content &c, int64_t now, uint8_t g[kDigits], bool &colon) const {
    const int64_t t = now - c.since;
    if (c.kind == Content::TEXT) {
      const int n = int(c.cells.size());
      // Static text is centred; scrolling text enters from the right and ends on a blank frame.
      deg_wanted = c.degree;
      const int first = n <= kDigits ? -(kDigits - n) / (c.degree ? 1 : 2) : int((t / scroll_ms) % (n + kDigits)) - (kDigits - 1);
      for (int p = 0; p < kDigits; p++) {
        int ci = first + p;
        if (ci < 0 || ci >= n) continue;
        g[p] = c.cells[ci].glyph;
        if (p == 1) colon = c.cells[ci].colon_after;
      }
    } else if (c.face == kFaceBlink) {
      memcpy(g, t % kBlinkPeriodMs < kBlinkPeriodMs - kBlinkShutMs ? kBlinkOpen : kBlinkShut, kDigits);
    } else if (c.face == kFaceAll) {
      memcpy(g, kFaces[(t / kFaceCycleMs) % kStaticFaces].g, kDigits);
    } else if (c.face >= 0 && c.face < kStaticFaces) {
      memcpy(g, kFaces[c.face].g, kDigits);
    }
  }

  // Frame timer for moving content; also fires when the temporary layer ends, so a single scroll
  // pass does not show the start of a second one until the next tick.
  void schedule_anim() {
    if (anim) {
      loop.cancel_timer(anim);
      anim = 0;
    }
    if (!tick) return;  // not opened
    const Content *c = shown_content();
    const int64_t now = Loop::now_ms();
    int64_t step = c ? anim_step(*c) : 0;
    int64_t delay = step ? step - (now - c->since) % step : 0;
    if (!step && eq_always && music_on) {  // the equalizer as the clock face
      step = kAnimStepMs;
      delay = step;
    }
    if (!step && clock_fading(now)) {  // the clock fades in after an animation: needs frames too
      step = kAnimStepMs;
      delay = step;
    }
    if (c == &overlay) delay = std::max<int64_t>(1, step ? std::min(delay, overlay_until - now) : overlay_until - now);
    if (!delay) return;
    anim = loop.add_timer(delay, [this] {
      anim = 0;
      refresh();
      schedule_anim();
    });
  }

  void refresh() {
    int64_t now = Loop::now_ms();
    if (overlay.kind != Content::NONE && now >= overlay_until) {
      if (overlay.kind == Content::SWEEP && overlay.then_eq_ms > 0) {  // track change: the bars sweep, then the equalizer
        Content e;
        e.kind = Content::EQ;
        e.since = now;
        overlay_until = now + overlay.then_eq_ms;
        overlay = e;
      } else {
        if (overlay.kind == Content::ANIM || overlay.kind == Content::SWEEP || overlay.kind == Content::EQ || overlay.kind == Content::PAUSE || overlay.kind == Content::BOOT ||
            overlay.kind == Content::FACE)
          clock_fade_from = now;  // fade the clock in instead of cutting to it
        overlay = {};
      }
      schedule_anim();
    }
    if (!code.empty()) return show_code(now);
    deg_wanted = false;
    uint8_t g[kDigits] = {0, 0, 0, 0};
    bool colon = false;
    float lv[kDigits][kSegs];
    bool levels = false;
    float anim_scale = 1.f, text_scale = 1.f;
    if (now < flash_until) {
      // Right-aligned number, '-' for negatives.
      int v = flash_value, n = std::abs(v);
      int pos = kDigits - 1;
      do {
        g[pos--] = kFont[n % 10];
        n /= 10;
      } while (n && pos >= 0);
      if (v < 0 && pos >= 0) g[pos] = kMinus;
    } else if (timer_end || timer_paused) {
      int64_t left = std::max<int64_t>(0, ((timer_end ? timer_end - now : timer_paused) + 999) / 1000);
      if (left >= 3600) {  // h:mm (hours >= 10 show as hh:mm)
        two(g, int(std::min<int64_t>(left / 3600, 99)), true);
        two(g + 2, int(left / 60 % 60), false);
      } else {
        two(g, int(left / 60), true);
        two(g + 2, int(left % 60), false);
      }
      colon = left > 0 || (now / 500) % 2 == 0;  // blink at 00:00
      if (left == 0 && (now / 500) % 2) memset(g, 0, sizeof g);
    } else if (const Content *c = shown_content()) {
      if (c->kind == Content::ANIM) {
        render_anim(*c, now, lv);
        levels = true;
        const float in = std::min(1.f, float(now - c->fade_from) / float(kAnimFadeInMs));
        const float out = c == &overlay ? std::max(0.f, std::min(1.f, float(overlay_until - now) / float(kAnimFadeOutMs))) : 1.f;
        anim_scale = in * out;
      } else if (c->kind == Content::SWEEP || c->kind == Content::EQ || c->kind == Content::PAUSE || c->kind == Content::BOOT) {
        if (c->kind == Content::BOOT) {
          render_boot(*c, now, lv);
        } else if (c->kind == Content::PAUSE) {
          render_pause(*c, now, lv);
        } else if (c->kind == Content::SWEEP) {
          render_sweep(*c, now, lv);
        } else {
          if (c->decay) eq_step(true);
          render_eq(lv);
        }
        levels = true;
        const float in = std::min(1.f, float(now - c->since) / float(kAnimFadeInMs));
        const float out = c == &overlay ? std::max(0.f, std::min(1.f, float(overlay_until - now) / float(kAnimFadeOutMs))) : 1.f;
        anim_scale = in * out;
      } else {
        render(*c, now, g, colon);
        if (c->kind == Content::FACE && c == &overlay)   // an emotion face: out with a fade, the clock fades back in
          text_scale = std::max(0.f, std::min(1.f, float(overlay_until - now) / float(kAnimFadeOutMs)));
        if (c->kind == Content::TEXT && c->degree && c == &overlay) {  // weather: fades in and out
          const float in = std::min(1.f, float(now - c->since) / float(kAnimFadeInMs));
          const float out = std::max(0.f, std::min(1.f, float(overlay_until - now) / float(kAnimFadeOutMs)));
          text_scale = in * out;
        }
      }
    } else if (eq_always && music_on) {
      render_eq(lv);
      levels = true;
      anim_scale = std::min(1.f, float(now - eq_always_since) / 300.f);
    } else {
      time_t t = time(nullptr);
      struct tm tm;
      localtime_r(&t, &tm);
      if (!hold) {
        two(g, tm.tm_hour, false);
        two(g + 2, tm.tm_min, false);
        colon = true;  // steady colon (local patch: it used to blink once a second)
      }
    }
    if (levels) show_levels(lv, anim_scale);
    else show(g, colon, false, clock_scale(now) * text_scale);
    publish_frame(now);
  }
};

Clock::Clock(Loop &loop, const Model &model) : impl_(new Impl(loop, model)) {
  for (auto &d : impl_->seg_fd)
    for (int &fd : d) fd = -1;
  for (auto &d : impl_->seg_last)
    for (int &v : d) v = -1;
}

Clock::~Clock() = default;

bool Clock::open() {
  auto &d = *impl_;
  int found = 0;
  std::vector<std::string> missing;
  for (int dig = 0; dig < kDigits; dig++)
    for (int s = 0; s < kSegs; s++) {
      std::string name = d.seg_name(dig, s);
      d.seg_fd[dig][s] = d.open_led(name);
      if (d.seg_fd[dig][s] >= 0) found++;
      else missing.push_back(name);
    }
  if (!found) {   // a model that comes with a clock and without one (the Mini 2): the LEDs tell, no profile change is needed
    LOGI("clock: no clock display on this speaker (no %s%s)", d.model.clock_dir.c_str(), missing.empty() ? "" : missing[0].c_str());
    return false;
  }
  for (const auto &name : missing) LOGW("clock: missing segment %s", name.c_str());
  for (const auto &name : d.model.clock_colon) {
    int fd = d.open_led(name);
    if (fd >= 0) d.colon_fd.push_back(fd);
  }
  for (const auto &aux : d.model.clock_aux) {
    int fd = d.open_led(aux);
    if (fd >= 0) {
      hwutil::write_int_fd(fd, 0);
      if (aux == "DEGREE") d.deg_fd = fd;
      else close(fd);
    }
  }
  if (d.model.clock_chip_current) {
    // Global current of the clock driver chips, found through the LED device links of the
    // leftmost and rightmost digit (one chip each on the Midi).
    for (const std::string &seg : {d.seg_name(0, 0), d.seg_name(kDigits - 1, 0)}) {
      std::string dev = d.model.clock_dir + seg + "/device/current";
      char real[256];
      if (!realpath(dev.c_str(), real)) continue;
      bool dup = false;
      for (int fd : d.current_fds) {
        char p[64], r2[256];
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(p, r2, sizeof r2 - 1);
        if (n > 0 && std::string(r2, size_t(n)) == real) dup = true;
      }
      if (dup) continue;
      int fd = ::open(real, O_WRONLY | O_CLOEXEC);
      if (fd >= 0) d.current_fds.push_back(fd);
    }
  }
  LOGI("clock: %d segments, %zu colon LEDs, %s dimming", found, d.colon_fd.size(),
       d.model.clock_chip_current ? "chip current" : "segment brightness");
  d.apply_level();
  d.refresh();
  // Every ~5 s rewrite all segments even if unchanged (sysfs state may be changed by others);
  // counted on the tick only, animation frames refresh in between.
  d.tick = d.loop.add_timer(kTickMs, [&d] {
    if (++d.refreshes % 20 == 0) d.forget_written();
    d.refresh();
  }, kTickMs);
  d.schedule_anim();
  return true;
}

void Clock::set_enabled(bool on) {
  impl_->enabled = on;
  impl_->refresh();
}

void Clock::set_brightness(float b) {
  impl_->user_bri = std::max(0.f, std::min(1.f, b));
  impl_->apply_level();
  impl_->refresh();
}

void Clock::set_ambient_lux(float lux) {
  impl_->lux = std::max(0.f, lux);
  impl_->dimmer.update(impl_->lux);
  impl_->apply_level();
  impl_->refresh();
}

void Clock::set_timer(int64_t ms_left, bool running) {
  impl_->timer_end = ms_left > 0 && running ? Loop::now_ms() + ms_left : 0;
  impl_->timer_paused = ms_left > 0 && !running ? ms_left : 0;
  impl_->refresh();
}

void Clock::flash_number(int value) {
  impl_->flash_value = std::max(-999, std::min(9999, value));
  impl_->flash_until = Loop::now_ms() + kFlashMs;
  impl_->refresh();
}

void Clock::show_code(const std::string &digits) {
  impl_->code = digits;
  impl_->code_since = Loop::now_ms();
  impl_->refresh();
}

void Clock::clear_code() {
  impl_->code.clear();
  impl_->refresh();
}

void Clock::set_text(const std::string &utf8) {
  auto &d = *impl_;
  std::vector<Cell> cells = layout_text(utf8);
  if (cells.empty()) {
    // Non-empty text without cells (only ':') still replaces the face, as the controller does.
    if (d.base.kind == Impl::Content::TEXT || !utf8.empty()) d.base = {};
  } else {
    d.base = {Impl::Content::TEXT, std::move(cells), -1, Loop::now_ms()};
  }
  d.refresh();
  d.schedule_anim();
}

void Clock::set_face(int index) {
  auto &d = *impl_;
  if (index < 0 || index >= kFaceCount) {
    if (d.base.kind == Impl::Content::FACE) d.base = {};
  } else {
    d.base = {Impl::Content::FACE, {}, index, Loop::now_ms()};
  }
  d.refresh();
  d.schedule_anim();
}

void Clock::show_track_change(int dir, int eq_seconds) {
  if (impl_->ringing) return;
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  if (d.overlay.kind == Impl::Content::ANIM && now < d.overlay_until) return;  // the assistant has the display
  LOGI("clock: track change sweep dir %d", dir);
  Impl::Content c;
  c.kind = Impl::Content::SWEEP;
  c.since = now;
  c.dir = dir;
  c.then_eq_ms = d.eq_always ? 0 : eq_seconds * 1000;
  d.overlay = c;
  d.overlay_until = now + 900;
  d.refresh();
  d.schedule_anim();
}

void Clock::show_eq(int ms, bool decay) {
  if (impl_->ringing) return;
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  if (d.overlay.kind == Impl::Content::ANIM && now < d.overlay_until) return;
  Impl::Content c;
  c.kind = Impl::Content::EQ;
  c.since = now;
  c.decay = decay;
  d.overlay = c;
  d.overlay_until = now + ms;
  d.refresh();
  d.schedule_anim();
}

int64_t Clock::show_boot(const std::vector<uint8_t> &env) {
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  Impl::Content c;
  c.kind = Impl::Content::BOOT;
  c.since = now;
  c.env = env;
  d.overlay = c;
  const int64_t len = int64_t(env.size()) * kEnvStepMs;
  d.overlay_until = now + len + 3500;   // then the eyes blink for a few seconds, fade out, and the clock fades in
  d.refresh();
  d.schedule_anim();
  return len;
}

void Clock::show_pause() {
  if (impl_->ringing) return;
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  if (d.overlay.kind == Impl::Content::ANIM && now < d.overlay_until) return;
  Impl::Content c;
  c.kind = Impl::Content::PAUSE;
  c.since = now;
  d.overlay = c;
  d.overlay_until = now + 1200;
  d.refresh();
  d.schedule_anim();
}

void Clock::set_hold(bool on) {
  auto &d = *impl_;
  if (d.hold == on) return;
  d.hold = on;
  if (!on) d.clock_fade_from = Loop::now_ms();   // the clock fades in
  d.refresh();
  d.schedule_anim();
}

void Clock::set_ringing(bool on) {
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  if (on) {
    Impl::Content c;
    c.kind = Impl::Content::SWEEP;
    c.since = now;
    c.pingpong = true;
    d.overlay = c;
    d.overlay_until = now + 30 * 60 * 1000;
    d.ringing = true;
  } else if (d.ringing) {
    d.ringing = false;
    if (d.overlay.kind == Impl::Content::SWEEP && d.overlay.pingpong) {
      d.overlay = {};
      d.clock_fade_from = now;
    }
  }
  d.refresh();
  d.schedule_anim();
}

void Clock::set_alarm_led(bool on) {
  if (impl_->alarm_led == on) return;
  impl_->alarm_led = on;
  impl_->refresh();
}

void Clock::set_eq_always(bool on) {
  auto &d = *impl_;
  if (on == d.eq_always) return;
  const int64_t now = Loop::now_ms();
  if (on) d.eq_always_since = now;
  else if (d.music_on) d.clock_fade_from = now;
  d.eq_always = on;
  d.refresh();
  d.schedule_anim();
}

void Clock::set_music_playing(bool on) {
  auto &d = *impl_;
  if (on == d.music_on) return;
  if (!on && d.eq_always) d.clock_fade_from = Loop::now_ms();
  d.music_on = on;
  d.refresh();
  d.schedule_anim();
}

void Clock::music_frame(const MusicFrame &f) {
  auto &d = *impl_;
  d.eq_t = f.bands;
  d.eq_step(false);
  const float l = std::max(0.f, std::min(1.f, (f.loudness - 0.3f) / 0.7f));
  d.loud_smooth = l > d.loud_smooth ? l : d.loud_smooth * 0.8f + l * 0.2f;  // fast attack, slow release
  d.loud_hist[d.loud_pos++ % 32] = d.loud_smooth;
}

void Clock::set_eq_style(bool bars) { impl_->eq_bars = bars; }

void Clock::set_frame_callback(std::function<void(const std::string &)> cb) { impl_->on_frame = std::move(cb); }

void Clock::set_scroll_ms(int ms) {
  impl_->scroll_ms = std::max(100, std::min(2000, ms));
  impl_->refresh();
  impl_->schedule_anim();
}

void Clock::show_temporary_text(const std::string &utf8_in, int seconds) {
  auto &d = *impl_;
  std::string utf8 = utf8_in;
  bool degree = false;
  if (utf8.size() >= 2 && utf8.compare(utf8.size() - 2, 2, "\xC2\xB0") == 0) {  // "15°": digits + the DEGREE LED
    degree = true;
    utf8.resize(utf8.size() - 2);
  }
  std::vector<Cell> cells = layout_text(utf8);
  const int64_t now = Loop::now_ms();
  if (cells.empty()) {
    d.overlay = {};
  } else {
    const int64_t n = int64_t(cells.size());
    d.overlay_until = now + (seconds > 0   ? std::min<int64_t>(seconds, kTemporaryMaxS) * 1000
                             : n > kDigits ? (n + kDigits) * d.scroll_ms
                                           : kTemporaryMs);
    d.overlay = {Impl::Content::TEXT, std::move(cells), -1, now};
    d.overlay.degree = degree;
  }
  d.refresh();
  d.schedule_anim();
}

void Clock::show_temporary_face(int index, int seconds) {
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  if (index < 0 || index >= kFaceCount) {
    d.overlay = {};
  } else {
    d.overlay_until = now + (seconds > 0 ? std::min<int64_t>(seconds, kTemporaryMaxS) * 1000 : kTemporaryMs);
    d.overlay = {Impl::Content::FACE, {}, index, now};
  }
  d.refresh();
  d.schedule_anim();
}

void Clock::show_temporary_anim(const std::string &program, int seconds) {
  auto &d = *impl_;
  const int64_t now = Loop::now_ms();
  const size_t colon_pos = program.find(':');
  const std::string name = program.substr(0, colon_pos);
  if (name.empty() || name == "off") {
    // fade a running animation out instead of cutting it; the clock then fades in
    if (d.overlay.kind == Impl::Content::ANIM) d.overlay_until = std::min(d.overlay_until, now + kAnimFadeOutMs);
    d.refresh();
    d.schedule_anim();
    return;
  }
  Impl::Content c;
  c.kind = Impl::Content::ANIM;
  c.anim = name == "listen" ? kAnimListen : name == "think" ? kAnimThink : name == "speak" ? kAnimSpeak : 0;
  if (!c.anim) {
    LOGW("clock: unknown animation '%s'", name.c_str());
    return;
  }
  if (colon_pos != std::string::npos) {  // "speak:<hex>": one loudness byte per kEnvStepMs
    auto hex = [](char ch) {
      return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    };
    for (size_t i = colon_pos + 1; i + 1 < program.size() && c.env.size() < 4000; i += 2) {
      const int hi = hex(program[i]), lo = hex(program[i + 1]);
      if (hi < 0 || lo < 0) break;
      c.env.push_back(uint8_t(hi * 16 + lo));
    }
  }
  c.since = now;
  // switching straight from one animation to another keeps the brightness (no dip to dark)
  c.fade_from = d.overlay.kind == Impl::Content::ANIM && now < d.overlay_until ? now - kAnimFadeInMs : now;
  d.overlay_until = now + std::min<int64_t>(seconds > 0 ? seconds : 5, kTemporaryMaxS) * 1000;
  d.overlay = std::move(c);
  d.clock_fade_from = 0;
  d.refresh();
  d.schedule_anim();
}

const std::vector<std::string> &Clock::face_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (const Face &f : kFaces) v.emplace_back(f.name);
    v.emplace_back("Blink");
    v.emplace_back("All faces");
    return v;
  }();
  return names;
}

}  // namespace cs
