// Native test of the MediaHub arbitration: g++ -std=c++17 -Isrc tests/hub_test.cpp -o /tmp/hub_test && /tmp/hub_test
#include <cstdio>
#include <vector>

#include "media/hub.h"

using namespace cs;

static int failures = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      printf("FAIL line %d: %s\n", __LINE__, #c);             \
      failures++;                                             \
    }                                                         \
  } while (0)

int main() {
  MediaHub h;
  std::vector<MediaSource> paused, assumed;
  h.send_pause = [&](MediaSource s) { paused.push_back(s); };
  h.on_assumed = [&](MediaSource s) { assumed.push_back(s); };

  // Spotify plays, AirPlay starts: Spotify gets a pause, AirPlay is active.
  h.report(MediaSource::SPOTIFY, true, 0);
  CHECK(h.active() == MediaSource::SPOTIFY && paused.empty());
  h.report(MediaSource::AIRPLAY, true, 1000);
  CHECK(h.active() == MediaSource::AIRPLAY);
  CHECK(paused.size() == 1 && paused[0] == MediaSource::SPOTIFY);

  // Spotify confirms: nothing more is sent.
  h.report(MediaSource::SPOTIFY, false, 1200);
  h.tick(5000);
  CHECK(paused.size() == 1 && assumed.empty());
  CHECK(!h.playing(MediaSource::SPOTIFY) && h.playing(MediaSource::AIRPLAY));

  // A silent source: asked twice, then taken for paused.
  paused.clear();
  h.report(MediaSource::SPOTIFY, true, 6000);                 // the user starts Spotify again
  CHECK(h.active() == MediaSource::SPOTIFY && paused.size() == 1 && paused[0] == MediaSource::AIRPLAY);
  h.tick(6000 + 1000);                                         // too early
  CHECK(paused.size() == 1);
  h.tick(6000 + 1500);                                         // no answer: the second try
  CHECK(paused.size() == 2);
  h.tick(6000 + 3000);                                         // still silent: it is taken for paused
  CHECK(assumed.size() == 1 && assumed[0] == MediaSource::AIRPLAY);
  CHECK(!h.playing(MediaSource::AIRPLAY) && h.target() == MediaSource::SPOTIFY);

  // A pause of the active source keeps it as the target (the buttons resume it).
  h.report(MediaSource::SPOTIFY, false, 10000);
  CHECK(!h.any_playing() && h.target() == MediaSource::SPOTIFY);

  // Three sources: the last one wins, both others are asked.
  paused.clear();
  h.report(MediaSource::AIRPLAY, true, 11000);
  h.report(MediaSource::BLUETOOTH, true, 11100);
  CHECK(h.active() == MediaSource::BLUETOOTH);
  CHECK(paused.size() == 1 && paused[0] == MediaSource::AIRPLAY);   // only AirPlay was playing
  // The same source reporting "playing" again does not start a new round.
  paused.clear();
  h.report(MediaSource::BLUETOOTH, true, 11200);
  CHECK(paused.empty());

  printf(failures ? "%d FAILED\n" : "all hub tests passed\n", failures);
  return failures ? 1 : 0;
}
