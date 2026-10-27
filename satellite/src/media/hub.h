// MediaHub: who plays right now, and making the others stand aside.
//
// Every music source (AirPlay, Spotify, Bluetooth) reports only its own state: report(source, playing).
// The hub keeps one rule for all of them:
//   * the source that started last is the active one;
//   * every other source that is still playing gets a "pause", and the hub waits for its answer (a state report);
//   * a source that stays silent is asked once more, and then simply taken for paused (librespot and some phones
//     say nothing when they are pushed aside), so a stale "playing" never takes the buttons away from the real player.
// Pure logic, no I/O and no clock of its own: the caller passes the time, so it can be tested on a PC (tests/hub_test.cpp).
#pragma once

#include <cstdint>
#include <functional>

namespace cs {

enum class MediaSource { NONE = 0, AIRPLAY, SPOTIFY, BLUETOOTH, PLUGIN };

class MediaHub {
 public:
  static constexpr int kSources = 5;                // index = MediaSource
  static constexpr int64_t kAnswerMs = 1500;        // how long a source has to confirm a pause
  static constexpr int kPauseTries = 2;             // the pause is sent this many times before the source is taken for paused

  std::function<void(MediaSource)> send_pause;      // ask a source to pause (the caller knows how: AirPlay, librespot, BlueZ)
  std::function<void(MediaSource)> on_assumed;      // a silent source was taken for paused (its own flags should follow)
  std::function<void()> on_change;                  // active source or the playing state changed

  // A source tells its state ("playing" or not). `now` is any monotonic clock in ms.
  void report(MediaSource s, bool playing, int64_t now) {
    Slot &me = slot(s);
    const bool was = me.playing;
    me.playing = playing;
    me.pending = false;
    if (!playing) {
      if (was && active_ == s) changed();
      return;
    }
    if (active_ != s || !was) {
      active_ = s;
      for (int i = 1; i < kSources; i++) {
        if (i == static_cast<int>(s) || !slots_[i].playing) continue;
        ask_pause(static_cast<MediaSource>(i), now);
      }
      changed();
    }
  }

  // Call every few hundred ms: resolves the pauses that were not answered.
  void tick(int64_t now) {
    for (int i = 1; i < kSources; i++) {
      Slot &o = slots_[i];
      if (!o.pending || now < o.deadline) continue;
      if (!o.playing) {
        o.pending = false;
        continue;
      }
      if (o.tries < kPauseTries) {
        ask_pause(static_cast<MediaSource>(i), now);
        continue;
      }
      o.pending = false;
      o.playing = false;                            // silent: it is not playing (any more)
      if (on_assumed) on_assumed(static_cast<MediaSource>(i));
      changed();
    }
  }

  MediaSource active() const { return active_; }
  bool playing(MediaSource s) const { return slots_[static_cast<int>(s)].playing; }
  bool any_playing() const {
    for (int i = 1; i < kSources; i++)
      if (slots_[i].playing) return true;
    return false;
  }
  // The source that the buttons and the voice act on: the playing one, else the one that played last.
  MediaSource target() const { return active_; }

 private:
  struct Slot {
    bool playing = false;
    bool pending = false;     // a pause was asked and not answered yet
    int64_t deadline = 0;
    int tries = 0;
  };
  Slot &slot(MediaSource s) { return slots_[static_cast<int>(s)]; }

  void ask_pause(MediaSource s, int64_t now) {
    Slot &o = slot(s);
    if (!o.pending) o.tries = 0;
    o.pending = true;
    o.tries++;
    o.deadline = now + kAnswerMs;
    if (send_pause) send_pause(s);
  }
  void changed() {
    if (on_change) on_change();
  }

  Slot slots_[kSources];
  MediaSource active_ = MediaSource::NONE;
};

}  // namespace cs
