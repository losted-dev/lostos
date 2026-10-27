// A UPnP / DLNA MediaRenderer on the speaker. Home Assistant ("DLNA Digital Media Renderer") finds it by SSDP and shows the
// speaker as a media player that knows what plays (title, artist, cover; from AirPlay, Spotify and Bluetooth alike) and has the
// play / pause / next / previous / volume controls. No PC is involved.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/loop.h"

namespace cs {

class DlnaRenderer {
 public:
  struct Now {
    std::string source, artist, title, album, cover;  // source is empty when nothing plays and nothing can be resumed
    bool playing = false;
    bool paused = false;
  };

  explicit DlnaRenderer(Loop &loop);
  ~DlnaRenderer();

  // Listens on `port` (HTTP) and announces itself over SSDP. uuid is "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
  bool start(const std::string &friendly_name, const std::string &uuid, uint16_t port);

  std::function<Now()> get_now;
  std::function<void(const std::string &)> on_action;  // "play" | "pause" | "stop" | "next" | "previous"
  std::function<float()> get_volume;                   // 0..1
  std::function<void(float)> set_volume;
  std::function<bool()> get_mute;
  std::function<void(bool)> set_mute;

  // The state, the track or the volume may have changed: the subscribers get an event when something really did.
  void changed();

 private:
  struct Impl;
  Impl *impl_;
};

}  // namespace cs
