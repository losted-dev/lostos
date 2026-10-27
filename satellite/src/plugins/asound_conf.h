// The sound configuration lostos runs on, made from the one of the firmware (/etc/asound.conf).
//
// Everything of lostos plays through the PCM "speakers" (the player, the mixer, Sendspin, AirPlay, Spotify). Some firmware has it (Mini 3, Midi), another one
// does not (Mini 2, Lite "Micro": there the stock player uses "default", a softvol "Master Playback Volume" over the dmix "dmix_spk_plug"). For those the
// missing PCM is added, built the way the stock "default" is, so that the volume control and the mixer are the same as the stock ones.
// The equaliser is put in front of the mixer where the layout is the one it knows ("dmix_speakers").
#pragma once

#include <string>

namespace cs {

struct AsoundPlan {
  std::string text;            // the configuration to run on (equal to the original when nothing has to change)
  bool speakers_added = false;
  bool eq = false;
  std::string slave;           // the PCM that "speakers" was built over, when added
};

// orig: the configuration of the firmware. eq_lib: path of the equaliser plugin ("" = no equaliser).
AsoundPlan adapt_asound(const std::string &orig, const std::string &eq_lib);

}  // namespace cs
