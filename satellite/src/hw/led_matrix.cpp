#include "hw/led_matrix.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cs::fx::mx {

namespace {

constexpr float kTau = 6.2831853f;
constexpr uint32_t kViolet = 0x370fff, kSpeaking = 0x7b0ee8, kTimerGreen = 0x00dc00, kTimerStartGreen = 0x00e600, kMuteRed = 0xd20000, kError = 0xef0000;

float breathe(double t, double period) { return 0.55f + 0.45f * std::cos(kTau * float(std::fmod(t, period) / period)); }
float listen_k(float t) { return 0.70f + 0.30f * std::cos(kTau * float(std::fmod(t - 420, 1540.f) / 1540)); }

// Distance of a dot from a point of the panel, in units of the panel height (the panel is wider than high).
float dist(const Pt &p, float x, float y) { return std::hypot((p.x - x) * kAspect, p.y - y); }

void fill(Col *out, int n, Col c) { std::fill(out, out + n, c); }

}  // namespace

const Pt *points() {
  static const std::array<Pt, kLeds> pts = [] {
    static const int kRow[13] = {7, 8, 7, 8, 7, 4, 2, 4, 7, 8, 7, 8, 7};
    std::array<Pt, kLeds> a{};
    int i = 0;
    for (int r = 0; r < 13; r++) {
      const int c = kRow[r];
      const float y = float(r) / 12.f;
      for (int k = 0; k < c; k++) {
        float x;
        if (c == 8) x = (float(k) + 0.5f) / 8.f;
        else if (c == 7) x = float(k + 1) / 8.f;
        else if (c == 4) x = k < 2 ? (float(k) + 0.5f) / 8.f : (float(k) + 4.5f) / 8.f;   // two dots each side of the slot
        else x = k == 0 ? 0.09f : 0.91f;                                                 // one dot each side
        a[size_t(i++)] = {x, y};
      }
    }
    return a;
  }();
  return pts.data();
}

void voice(Voice v, int64_t t, Col *out, int n) {
  if (n != kLeds) return fill(out, n, Col{});
  const Pt *p = points();
  const float ft = float(t);
  switch (v) {
    case Voice::LISTENING: {
      // A violet wave runs from the middle to the edges in 420 ms, then the whole panel breathes 100 % <-> 40 %.
      const float front = ft / 420.f * 0.95f;
      for (int i = 0; i < n; i++) {
        const float k = ft < 420 ? clamp01(1 + (front - dist(p[i], 0.5f, 0.5f)) / 0.2f) : listen_k(ft);
        out[i] = stock(kViolet, k);
      }
      return;
    }
    case Voice::THINKING: {
      // A light chases round the panel round the control strip, one lap per 1.5 s, with a long tail, over a dim violet background.
      const float head = kTau * float(t % 1500) / 1500.f;
      for (int i = 0; i < n; i++) {
        const float dx = (p[i].x - 0.5f) * kAspect, dy = p[i].y - 0.5f;
        float behind = std::fmod(head - std::atan2(dy, dx), kTau);          // how far the head is ahead of this dot, clockwise
        if (behind < 0) behind += kTau;
        const float ring = smoothstep(0.12f, 0.35f, std::hypot(dx, dy));    // the dots near the middle stay out: it runs round the strip
        out[i] = cmax(stock(0x0a0219), stock(kViolet, std::exp(-behind / 0.85f) * ring));
      }
      return;
    }
    case Voice::SPEAKING: {
      // A blob in the middle whose size flickers irregularly (two incommensurate periods).
      const float flick = 0.5f + 0.5f * std::sin(kTau * ft / 530) * (0.6f + 0.4f * std::sin(kTau * ft / 1600));
      const float rw = 0.18f + 0.42f * flick;
      for (int i = 0; i < n; i++) out[i] = stock(kSpeaking, smoothstep(rw + 0.15f, rw - 0.05f, dist(p[i], 0.5f, 0.5f)));
      return;
    }
    case Voice::ERROR: {
      // Three pulses: 80 ms rise, exponential fade.
      const float tau = float(t % 1180);
      const float k = t >= kErrorMs ? 0 : tau < 80 ? tau / 80 : std::exp(-(tau - 80) / 300);
      fill(out, n, stock(kError, k));
      return;
    }
    case Voice::TIMER_RING: {
      // Two green blobs on the opposite sides sway up and down, breathing.
      const float k = breathe(ft, 1300);
      const float cy = 0.5f + 0.35f * std::sin(kTau * ft / 1300);
      for (int i = 0; i < n; i++) {
        const float d = std::min(dist(p[i], 0.12f, cy), dist(p[i], 0.88f, 1.f - cy));
        out[i] = stock(kTimerGreen, k * std::max(0.f, 1.f - d / 0.3f));
      }
      return;
    }
  }
}

void timer_started(int64_t t, Col *out, int n) {
  if (n != kLeds) return fill(out, n, Col{});
  const Pt *p = points();
  const float ft = float(std::min(t, kTimerStartedMs));
  // A green circle round the middle that gets smaller and smaller over 15 s (a bright rim, a soft disc inside), and goes out in the last 600 ms.
  const float radius = 0.9f * (1.f - ft / kTimerStartedMs) + 0.03f;
  const float last = ft < 14400 ? 1.f : (kTimerStartedMs - ft) / 600.f;
  for (int i = 0; i < n; i++) {
    const float r = dist(p[i], 0.5f, 0.5f);
    const float disc = smoothstep(radius + 0.05f, radius - 0.05f, r) * 0.5f;
    const float rim = std::exp(-((r - radius) / 0.06f) * ((r - radius) / 0.06f));
    out[i] = stock(kTimerStartGreen, clamp01(disc + rim) * last);
  }
}

void mute_sweep(bool on, int64_t t, Col *out, int n) {
  if (n != kLeds) return fill(out, n, Col{});
  const Pt *p = points();
  const float ft = float(std::min(t, kMuteSweepMs));
  // Red runs from the middle to the edges (unmute: from the edges to the middle).
  const float reach = 0.9f, h = std::min(1.f, ft / 500.f) * reach;
  const float tail = ft > 500 ? (kMuteSweepMs - ft) / 100.f : 1.f;
  for (int i = 0; i < n; i++) {
    float d = dist(p[i], 0.5f, 0.5f);
    if (!on) d = reach - d;
    out[i] = stock(kMuteRed, d <= h ? clamp01(1.f - (h - d) / 0.3f) * tail : 0.f);
  }
}

void mute_steady(Col *out, int n) {
  if (n != kLeds) return fill(out, n, Col{});
  const Pt *p = points();
  // The microphone is off: a dim red glow in the four corners, the rest of the panel dark.
  for (int i = 0; i < n; i++) {
    const float d = std::min({dist(p[i], 0, 0), dist(p[i], 1, 0), dist(p[i], 0, 1), dist(p[i], 1, 1)});
    out[i] = stock(0x720000, clamp01(1.f - d / 0.22f));
  }
}

void volume(float v, Col *out, int n) {
  if (n != kLeds) return fill(out, n, Col{});
  const Pt *p = points();
  v = clamp01(v);
  // The panel fills along the control strip: from "-" on the left to "+" on the right; the colour goes from green to red with the level.
  const Col c = hsv((1.f / 3.f) * (1.f - v), 1.f, 0.9f);
  const float edge = 0.03f + 0.94f * v;
  for (int i = 0; i < n; i++) out[i] = c * clamp01((edge - p[i].x) * 10.f + 0.5f);
}

}  // namespace cs::fx::mx
