// Link to the AirPlay 2 receiver (shairport-sync + nqptp + avahi, the system plugin ru.lostos.airplay) that
// runs next to lostos on the same speaker. lostos stays the owner of the buttons, LEDs, display and voice;
// this class only
//   - reads shairport's metadata pipe: the sender's volume slider and whether a session / playback is active,
//   - sends transport commands (play, pause, next, previous) to the sender through shairport's remote channel,
//   - switches the AirPlay stack on and off (voice command "turn AirPlay off").
#pragma once

#include <functional>
#include <string>

#include "core/loop.h"

namespace cs {

class SystemPlugins;

class AirPlayLink {
 public:
  AirPlayLink(Loop &loop, SystemPlugins &plugins);
  ~AirPlayLink();

  // Starts watching the metadata pipe (retries until shairport creates it).
  void start();

  bool installed() const;             // the AirPlay plugin is installed
  bool session() const { return session_; }  // a sender is connected (playing or paused)
  bool playing() const { return playing_; }  // audio is flowing

  // "play" | "pause" | "playpause" | "next" | "previous". False when there is no session to talk to.
  bool send(const std::string &cmd);
  // Starts / stops the AirPlay stack (fire and forget).
  bool power(bool on);
  // Ducking while the assistant talks or listens: 1 = normal, <1 attenuates the AirPlay stream only (the ALSA softvol
  // control "AirPlay Duck" in front of the speakers PCM), ramped so that it does not click.
  void set_duck(float gain);

  // Spotify Connect (librespot's --onevent script writes "playing" / "paused" / "stopped" / "volume N" lines into
  // /tmp/spotify-events): lets the stock command spotter listen while Spotify plays, and makes the Spotify slider
  // drive our volume like the AirPlay one.
  bool spotify_playing() const { return spotify_playing_; }
  bool spotify_known() const { return spotify_known_; }
  // The MediaHub asked for a pause and heard nothing: the source is taken for paused.
  void spotify_assume_paused() {
    if (!spotify_playing_) return;
    spotify_playing_ = false;
    if (on_state) on_state();
  }
  void assume_paused() {
    if (!playing_) return;
    playing_ = false;
    if (on_state) on_state();
  }  // librespot has told us something since lostos started

  std::function<void()> on_cover;  // the sender's cover picture arrived (written to kAirCover)
  std::function<void(const std::string &)> on_track;  // "Artist - Title (Album)" of the AirPlay sender's track; "" when it is gone
  std::function<void(const std::string &)> on_spotify_now;  // "Artist - Title" of the Spotify track (from librespot)
  std::function<void(uint32_t, uint32_t)> on_progress;   // AirPlay: position and length of the track, ms (shairport "prgr")
  std::function<void()> on_spotify_loading;       // a track is being loaded: Spotify is about to take the speaker (also when it starts paused)
  std::function<void(uint32_t)> on_spotify_pos;   // position of the Spotify track, ms
  std::function<void()> on_spotify_start;  // Spotify has just started playing
  std::function<void()> on_spotify_stop;   // ... paused / stopped
  std::function<void()> on_spotify_track;  // ... moved on to another track
  std::function<void(float)> on_volume;  // 0..1, the sender's volume slider
  std::function<void()> on_state;        // session / playing changed

 private:
  struct Impl;
  Impl *impl_;
  bool session_ = false, playing_ = false, spotify_playing_ = false, spotify_known_ = false;

};

}  // namespace cs
