// Renders the frames of the light panel of the Station 2 with the REAL code of the daemon (hw/led_matrix.cpp, hw/led_fx.cpp) and writes them as a JavaScript file for the page
// tools/led-emulator/index.html. Nothing here talks to hardware. Build and run: python tools/led-emulator/build.py
// usage: led_sim <output frames.js>
// A frame is the 84 dots of the panel followed by the 5 dots of the ring of the Alice button (the framebuffer of the speaker has 89), as bytes after the gamma step of Leds::to_px.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "hw/led_fx.h"
#include "hw/led_matrix.h"

using namespace cs::fx;

namespace {

constexpr int kLogo = 5;
constexpr int kAll = mx::kLeds + kLogo;

// What Leds::to_px does on the way to the framebuffer in a bright room: gamma 1.5, bytes 0..255 (the dimming for a dark room is left to the page).
std::string frame_hex(const Col *c) {
  std::string s;
  char b[8];
  for (int i = 0; i < kAll; i++) {
    const float v[3] = {c[i].r, c[i].g, c[i].b};
    for (float x : v) {
      x = clamp01(x);
      snprintf(b, sizeof b, "%02x", int(std::lround(std::pow(x, 1.5f) * 255.f)));
      s += b;
    }
  }
  return s;
}

// What Leds::render does with the logo: the voice states have their own pattern, a muted microphone a steady red, otherwise the average of the panel.
void logo_voice(Voice v, int64_t t, Col *o) { voice_logo(v, t, o + mx::kLeds, kLogo); }
void logo_mute(Col *o) { std::fill(o + mx::kLeds, o + kAll, mute_logo()); }
void logo_avg(Col *o) {
  Col avg;
  for (int i = 0; i < mx::kLeds; i++) avg = avg + o[i] * (1.f / mx::kLeds);
  std::fill(o + mx::kLeds, o + kAll, avg);
}

template <class F>
void scene(FILE *f, const char *id, const char *label, bool loop, int fps, int ms, F render, bool &first) {
  fprintf(f, "%s\n  %s: {label: %s, loop: %s, fps: %d, frames: [", first ? "" : ",", id, label, loop ? "true" : "false", fps);
  first = false;
  std::vector<Col> out(kAll);
  const int n = std::max(1, ms * fps / 1000);
  for (int i = 0; i < n; i++) {
    const int64_t t = int64_t(i) * 1000 / fps;
    std::fill(out.begin(), out.end(), Col{});
    render(t, out.data());
    fprintf(f, "%s\"%s\"", i ? "," : "", frame_hex(out.data()).c_str());
  }
  fprintf(f, "]}");
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: led_sim <frames.js>\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "w");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fprintf(f, "// made by led_sim.cpp from hw/led_matrix.cpp: do not edit\nwindow.LED_FRAMES = {\n points: [");
  const mx::Pt *p = mx::points();
  for (int i = 0; i < mx::kLeds; i++) fprintf(f, "%s[%.4f,%.4f]", i ? "," : "", p[i].x, p[i].y);
  fprintf(f, "],\n logo: %d,\n aspect: %.3f,\n scenes: {", kLogo, mx::kAspect);
  bool first = true;
  const int fps = 25;
  auto voice_scene = [&](const char *id, const char *label, Voice v, int ms, bool loop) {
    scene(f, id, label, loop, fps, ms, [v](int64_t t, Col *o) {
      mx::voice(v, t, o, mx::kLeds);
      logo_voice(v, t, o);
    }, first);
  };
  voice_scene("listening", "\"Слушаю\"", Voice::LISTENING, 3200, true);
  voice_scene("thinking", "\"Думаю\"", Voice::THINKING, 3000, true);
  voice_scene("speaking", "\"Говорю\"", Voice::SPEAKING, 4800, true);
  voice_scene("error", "\"Ошибка\"", Voice::ERROR, int(kErrorMs), false);
  voice_scene("timer_ring", "\"Таймер звонит\"", Voice::TIMER_RING, 2600, true);
  scene(f, "timer_started", "\"Таймер запущен (15 с)\"", false, 5, int(kTimerStartedMs), [](int64_t t, Col *o) {
    mx::timer_started(t, o, mx::kLeds);
    logo_avg(o);
  }, first);
  scene(f, "mute_on", "\"Микрофон выключен\"", false, fps, 1800, [](int64_t t, Col *o) {
    if (t < kMuteSweepMs) mx::mute_sweep(true, t, o, mx::kLeds);
    else mx::mute_steady(o, mx::kLeds);
    logo_mute(o);
  }, first);
  scene(f, "mute_off", "\"Микрофон включён\"", false, fps, kMuteSweepMs + 400, [](int64_t t, Col *o) {
    if (t < kMuteSweepMs) mx::mute_sweep(false, t, o, mx::kLeds);
    logo_avg(o);
  }, first);
  scene(f, "volume", "\"Громкость 0 → 100 %\"", true, 10, 6000, [](int64_t t, Col *o) {
    const float v = t < 3000 ? float(t) / 3000.f : 1.f - float(t - 3000) / 3000.f;     // up, then down
    mx::volume(v, o, mx::kLeds);
    logo_avg(o);
  }, first);
  fprintf(f, "\n }\n};\n");
  fclose(f);
  return 0;
}
