#include "plugins/asound_conf.h"

#include <cctype>

namespace cs {

namespace {

// Is there a definition "pcm.<name>" (and not "pcm.<name>_something")?
bool has_pcm(const std::string &text, const std::string &name) {
  const std::string key = "pcm." + name;
  for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
    const size_t end = at + key.size();
    const bool word = end >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_');
    const bool start = at == 0 || text[at - 1] == '\n' || text[at - 1] == ' ' || text[at - 1] == '\t' || text[at - 1] == '!';
    if (word && start) return true;
  }
  return false;
}

}  // namespace

AsoundPlan adapt_asound(const std::string &orig, const std::string &eq_lib) {
  AsoundPlan plan;
  plan.text = orig;

  // "speakers": the stock path of the sound, under the name lostos uses
  if (!has_pcm(plan.text, "speakers")) {
    for (const char *slave : {"dmix_spk_plug", "dmix_spk", "dmix_speakers", "mixer"}) {
      if (!has_pcm(plan.text, slave)) continue;
      plan.text += std::string("\npcm.speakers {\n  type softvol\n  slave.pcm \"") + slave + "\"\n  control { name \"Master Playback Volume\" }\n}\n";
      plan.speakers_added = true;
      plan.slave = slave;
      break;
    }
  }

  // the equaliser, in front of the dmix of the layout of the Mini 3
  const std::string from = "slave.pcm \"dmix_speakers\"";
  if (!eq_lib.empty() && plan.text.find(from) != std::string::npos) {
    size_t at = 0;
    while ((at = plan.text.find(from, at)) != std::string::npos) {
      plan.text.replace(at, from.size(), "slave.pcm \"eq_dmix\"");
      at += 19;
    }
    plan.text += "\npcm_type.lostoseq { lib \"" + eq_lib + "\" }\npcm.eq_dmix { type lostoseq; slave.pcm \"dmix_speakers\" }\n";
    plan.eq = true;
  }
  return plan;
}

}  // namespace cs
