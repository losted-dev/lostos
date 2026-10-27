// The sound configuration of lostos on the layouts of the real firmware (shortened to what matters).
// g++ -std=c++17 -Isrc tests/asound_conf_test.cpp src/plugins/asound_conf.cpp -o /tmp/act && /tmp/act
#include <cstdio>
#include <string>

#include "plugins/asound_conf.h"

using namespace cs;

static int bad = 0;
static void check(const char *name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) bad++;
}
static bool has(const std::string &t, const char *s) { return t.find(s) != std::string::npos; }

// Mini 3: "speakers" is there, over "dmix_speakers": only the equaliser goes in.
static const char *kMini3 = R"(pcm.!default { type asym playback.pcm "speakers" capture.pcm "multi_capture" }
pcm.speakers {
  type plug
  slave.pcm "dmix_speakers"
}
pcm.dmix_speakers {
  type dmix
  slave { pcm "hw:0,0" }
}
)";

// Mini 2 (2026-10 firmware): no "speakers"; "default" plays through a softvol over "dmix_spk_plug".
static const char *kMini2 = R"(pcm.!default {
  type asym
  playback.pcm {
    type softvol
    slave.pcm "dmix_spk_plug"
    control { name "Master Playback Volume" }
  }
}
pcm.dmix_spk_plug { type plug slave.pcm "dmix_spk" }
pcm.dmix_spk { type dmix slave.pcm "hw:0,0" }
)";

// Lite "Micro" (OTA 0.369.1.46): the same, with the softvol named "soft_volume".
static const char *kMicro = R"(pcm.!default { type asym playback.pcm "soft_volume" capture.pcm "multi_capture" }
pcm.soft_volume { type softvol slave.pcm "dmix_spk_plug" control { name "Master Playback Volume" } }
pcm.dmix_spk_plug { type plug slave.pcm "dmix_spk" }
pcm.dmix_spk { type dmix slave { pcm "hw:0,0" } }
)";

// Midi: "speakers" is there, over a plug over "mixer" (no "dmix_speakers": no equaliser).
static const char *kMidi = R"(pcm.mixer { type dmix slave { pcm "hw:0,0" channels 4 } }
pcm.speakers {
  type softvol
  slave.pcm { type plug slave.pcm "mixer" }
  control { name "Master Playback Volume" }
}
pcm.ultrasound { type softvol slave.pcm { type plug slave.pcm "mixer" } }
)";

int main() {
  const std::string lib = "/data/lostos/alsa/libasound_module_pcm_lostoseq.so";

  AsoundPlan p = adapt_asound(kMini3, lib);
  check("Mini 3: speakers is there, nothing added", !p.speakers_added);
  check("Mini 3: the equaliser goes in front of the mixer", p.eq && has(p.text, "slave.pcm \"eq_dmix\"") && has(p.text, "pcm.eq_dmix"));
  check("Mini 3: without the plugin the file stays as it is", adapt_asound(kMini3, "").text == kMini3);

  p = adapt_asound(kMini2, lib);
  check("Mini 2: speakers is added over dmix_spk_plug", p.speakers_added && p.slave == "dmix_spk_plug" && has(p.text, "pcm.speakers {"));
  check("Mini 2: the volume control is the stock one", has(p.text, "control { name \"Master Playback Volume\" }"));
  check("Mini 2: no equaliser (another layout)", !p.eq && !has(p.text, "eq_dmix"));
  check("Mini 2: the original text is kept whole", p.text.compare(0, std::string(kMini2).size(), kMini2) == 0);

  p = adapt_asound(kMicro, lib);
  check("Micro: speakers is added over dmix_spk_plug", p.speakers_added && p.slave == "dmix_spk_plug" && !p.eq);

  p = adapt_asound(kMidi, lib);
  check("Midi: speakers is there, nothing added, no equaliser", !p.speakers_added && !p.eq && p.text == kMidi);

  p = adapt_asound(kMini2, lib);
  check("running it again on its own result adds nothing", !adapt_asound(p.text, lib).speakers_added && adapt_asound(p.text, lib).text == p.text);
  check("a name that only starts like speakers does not count", adapt_asound("pcm.speakers_old { type null }\npcm.dmix_spk_plug { type plug slave.pcm \"x\" }\n", "").speakers_added);
  check("nothing to build on: nothing is added", !adapt_asound("pcm.!default { type null }\n", lib).speakers_added);
  return bad ? 1 : 0;
}
