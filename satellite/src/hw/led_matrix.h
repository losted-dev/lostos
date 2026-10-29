// The light panel of the Yandex Station 2 (and of any speaker whose LEDs are a flat matrix): 84 dots in 13 rows behind a milky diffuser, with a dark slot in the middle.
// Pure rendering, like led_fx.h: the voice states, the one-shots and the volume as the panel shows them. The rings of the other models are drawn by led_fx.cpp.
//
// The dots, as the driver numbers them (docs/models.md): rows of 7, 8, 7, 8, 7 | 4, 2, 4 (the rows round the slot) | 7, 8, 7, 8, 7. Rows of 8 start at the left edge,
// rows of 7 sit between them (a staggered grid). The positions are x: 0 left .. 1 right, y: 0 top .. 1 bottom of the panel; the panel is 1.35 times wider than high.
#pragma once

#include <cstdint>

#include "hw/led_fx.h"

namespace cs::fx::mx {

constexpr int kLeds = 84;
constexpr float kAspect = 1.35f;  // width / height of the panel

struct Pt {
  float x, y;
};
const Pt *points();  // kLeds entries

// Same meaning as the functions of led_fx.h; n must be kLeds.
void voice(Voice v, int64_t t, Col *out, int n);
void timer_started(int64_t t, Col *out, int n);
void mute_sweep(bool on, int64_t t, Col *out, int n);
void mute_steady(Col *out, int n);
void volume(float v, Col *out, int n);

}  // namespace cs::fx::mx
