// Per-model hardware description of the supported Yandex Stations. The model is detected once
// from the devicetree ("yandex,<codename>" in /proc/device-tree/compatible).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

struct Model {
  std::string codename;       // devicetree codename: "cucumber", "plum", "bergamot"
  std::string host_prefix;    // ESPHome node name prefix, the MAC suffix is appended
  std::string friendly_name;  // shown in Home Assistant / Music Assistant
  std::string model_name;

  // Capture: interleaved S16 frames from capture_pcm; VQE gets mic_channels / spk_channels.
  // capture_pcm is an ALSA name, or "tinyalsa:<card>,<device>" for the system libtinyalsa.so of the
  // Android stations (the Android build links its own alsa-lib, but tinyalsa is what maind's audio path uses there). capture_format "s32x2": the device gives S32 slots that each
  // carry two S16 lanes (Station 1: 5 slots = 7 mics + 3 speaker references); lane 2k is the high half
  // of slot k, 2k+1 the low half, and capture_channels / mic_channels / spk_channels count lanes.
  std::string capture_pcm;
  std::string capture_format;  // "" = s16
  unsigned capture_rate = 0, capture_channels = 0;
  // Optional second capture device with more speaker references (Station 1: the Loopback stream maind
  // writes, 2 ch S32 at 48 kHz, besides the 3 references on the mic ADCs). It is converted to S16, resampled
  // to capture_rate (an integer divisor of ref_rate) and appended as lanes capture_channels..+ref_channels-1,
  // which spk_channels may name. ref_format "s32" = plain S32 samples (not packed like s32x2).
  std::string ref_pcm, ref_format;
  unsigned ref_rate = 0, ref_channels = 0;
  std::vector<unsigned> mic_channels, spk_channels;
  std::string vqe_preset;

  // LED ring (+ optional logo), each an Nx1 BGR framebuffer behind an ledfb trigger, with
  // per-channel sysfs LEDs "<led_prefix>:<color>:<ring|logo><index>" as fallback.
  struct LedStrip {
    int count = 0;
    int first_index = 0;      // sysfs numbering starts at 0 (Midi) or 1 (Lite 2)
    std::string fb_dev;       // "/dev/fb0"
    std::string fb_name;      // expected /sys/class/graphics/fbN/name
    std::string trigger;      // LED trigger that routes the framebuffer to the LEDs
    std::string sysfs_kind;   // "ring" / "logo"
    // The framebuffer of a flat LED panel (Station 2): format "rgbw" = 4 bytes per dot (R, G, B, 0) instead of the 3 bytes B, G, R of the rings, fb_pixels = the dots of
    // the framebuffer when it has more than count (the rest stays dark). trigger "none": no sysfs LEDs and no trigger to check. count 84 + "rgbw" = the matrix of led_matrix.h.
    std::string format;
    int fb_pixels = 0;
  };
  std::string led_prefix;
  LedStrip ring, logo;        // logo.count == 0: no logo

  // 4-digit 7-segment clock: "<clock_dir><segment><position>" sysfs LEDs.
  std::string clock_dir;      // e.g. "/sys/class/leds/cucumber:white:clock-"
  char clock_seg_base = 'a';  // name of segment a ('a' or 'A')
  int clock_pos_base = 1;     // name of the leftmost digit position
  std::vector<std::string> clock_colon, clock_aux;  // colon LEDs; LEDs kept dark
  // true: segments switch 0/255, dimming via the driver chips' "current" attribute;
  // false: dimming via the per-segment brightness value itself.
  bool clock_chip_current = true;

  std::string keys_input, mute_input;  // evdev device names
  bool mute_key = false;               // the mute device has no switch (SW_MUTE_DEVICE): a KEY_MUTE press toggles the mute (Station Micro)
  std::string error_sound;             // relative to /system/vendor/quasar/sounds/
  // Keys on plain GPIOs (sysfs /sys/class/gpio/gpioN/value, edge interrupts) instead of evdev (Station 1):
  //   action_gpio "<pin>,<low|high>": the Alice button and the level it has while pressed;
  //   mute_gpio   "<pin>,<low|high>": the level that means "microphones muted" (a switch or the
  //               flip-flop behind a mute button);
  //   wheel_gpio  "<pin a>,<pin b>,<transitions per step>": quadrature volume wheel, a turn from A
  //               towards B is volume up. Empty: none.
  std::string action_gpio, mute_gpio, wheel_gpio;

  // Stock volume curve: platform level -> raw value (0..255) of the softvol "Master Playback
  // Volume" on the speakers PCM, piecewise linear between the points; the first point is level 0
  // (raw 0), the last one the top level. quasar.cfg volume_capability.volume_manager_settings
  // .platformToAlsaMap; the platform level steps by 5 % (changeStepPercent).
  struct VolumePoint {
    int level, raw;
  };
  std::vector<VolumePoint> volume_map;
  // Output: the GStreamer sink of the player ("" = alsasink on the "speakers" PCM). mixer "soft": no ALSA
  // master control; the raw value from volume_map is applied as -60..0 dB gain in the player. mixer
  // "hardvol" (Station 1): the raw value is the percent of the stock audio HAL's volume table
  // hardvol_file, applied to its codec controls on card hardvol_card (profile: hardvol=tinyalsa:<card>,<file>).
  std::string audio_sink, mixer;
  std::string hardvol_file;
  int hardvol_card = -1;

  // Zigbee radio: Telink co-processor on a UART; its reset and boot lines are GPIO value files
  // (active low, 1 = asserted) exported by the kernel's zb_control driver. zigbee_tty empty: none.
  std::string zigbee_tty;
  std::string zigbee_reset, zigbee_boot;
};

// The detected model; nullptr on unsupported hardware. A profile file /data/lostos/models/<codename>.conf (the format that
// dump_profile() writes, see docs/models.md) wins over the profiles built into the binary (Midi, Lite 2, Mini 3).
const Model *detect_model();

// A profile as text (key=value lines) and back; parse_profile() fills only the keys that are present.
std::string dump_profile(const Model &m);
bool parse_profile(const std::string &text, Model &m, std::string *error = nullptr);

// The profiles built into the binary (for the tool that writes them out as files).
std::vector<Model> builtin_models();

}  // namespace cs
