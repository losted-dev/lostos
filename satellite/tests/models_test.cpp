// Native test of the model profiles: every built-in model survives dump -> parse -> dump unchanged.
// With an argument it also writes the profiles into that directory (satellite/models/*.conf):
//   g++ -std=c++17 -Isrc tests/models_test.cpp src/model.cpp src/core/log.cpp -o /tmp/models_test && /tmp/models_test models
#include <cstdio>
#include <fstream>

#include "model.h"

using namespace cs;

int main(int argc, char **argv) {
  int bad = 0;
  for (const Model &m : builtin_models()) {
    const std::string text = dump_profile(m);
    Model back;
    std::string err;
    if (!parse_profile(text, back, &err)) {
      printf("FAIL %s: does not parse: %s\n", m.codename.c_str(), err.c_str());
      bad++;
      continue;
    }
    if (dump_profile(back) != text) {
      printf("FAIL %s: the profile changes on the way back\n", m.codename.c_str());
      bad++;
      continue;
    }
    if (argc > 1) std::ofstream(std::string(argv[1]) + "/" + m.codename + ".conf") << text;
    printf("ok %s (%zu bytes)\n", m.codename.c_str(), text.size());
  }
  Model junk;
  std::string err;
  if (parse_profile("nonsense\n", junk, &err)) { printf("FAIL: junk accepted\n"); bad++; }
  if (parse_profile("codename=x\nbogus=1\n", junk, &err)) { printf("FAIL: unknown key accepted\n"); bad++; }
  // The AirPlay icon: the big speakers name the big HomePod, the small ones keep the program's own (the HomePod mini); the name must keep its length (the program is patched in place).
  for (const Model &m : builtin_models()) {
    const bool big = m.codename == "cucumber";
    if (big != (m.airplay_model == "AudioAccessory6,1") || (!big && !m.airplay_model.empty())) {
      printf("FAIL %s: airplay_model '%s'\n", m.codename.c_str(), m.airplay_model.c_str());
      bad++;
    }
  }
  const std::string base = dump_profile(builtin_models()[0]);
  Model fine;
  if (!parse_profile(base + "airplay_model=AudioAccessory1,1\n", fine, &err) || fine.airplay_model != "AudioAccessory1,1") { printf("FAIL: a right airplay_model refused (%s)\n", err.c_str()); bad++; }
  for (const char *wrong : {"HomePod", "AudioAccessory12,1", "AudioAccessory6.1", "audioaccessory6,1", "AudioAccessoryX,1"})
    if (parse_profile(base + "airplay_model=" + wrong + "\n", junk, &err)) { printf("FAIL: airplay_model '%s' accepted\n", wrong); bad++; }
  return bad ? 1 : 0;
}
