// The Station's own Bluetooth audio (bluez + bluealsa, started by the stock init scripts) as one more source next to
// AirPlay and Spotify: /data/lostos/bt.sh switches it, sends transport commands to the phone (AVRCP) and, as a child of
// lostos, reports the connection, the player state and the track.
#pragma once

#include <functional>
#include <string>

#include "core/loop.h"

namespace cs {

class BtLink {
 public:
  explicit BtLink(Loop &loop);
  ~BtLink();

  void start();                          // applies the saved on / off and starts watching
  bool installed() const;                // /data/lostos/bt.sh exists
  bool enabled() const { return enabled_; }
  bool connected() const { return connected_; }
  bool playing() const { return playing_; }
  const std::string &now() const { return now_; }   // "Artist - Title" while the phone tells it

  void assume_paused() {   // the MediaHub asked for a pause and the phone said nothing
    if (!playing_) return;
    playing_ = false;
    if (on_state) on_state();
  }
  void set_enabled(bool on);             // on: visible for pairing for five minutes; off: powered down, stays off after a reboot
  void command(const std::string &cmd);  // play | pause | playpause | next | previous (detached child, fire and forget)

  std::function<void(uint32_t, uint32_t)> on_position;   // position and length of the track on the phone, ms
  std::function<void(float)> on_volume;               // the phone moved its volume slider (0..1, AVRCP absolute volume)
  void set_volume(float v);                            // move the phone's slider
  std::function<void()> on_state;                      // connection / playing / enabled changed
  std::function<void(const std::string &)> on_now;     // the track changed

 private:
  struct Impl;
  Impl *impl_;
  bool enabled_ = true, connected_ = false, playing_ = false;
  std::string now_;
};

}  // namespace cs