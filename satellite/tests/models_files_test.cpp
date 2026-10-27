// Every profile file in models/ (and the template is skipped) must parse: g++ -std=c++17 -Isrc tests/models_files_test.cpp src/model.cpp src/core/log.cpp -o /tmp/mft && /tmp/mft models/*.conf
#include <cstdio>
#include <fstream>
#include <iterator>

#include "model.h"

using namespace cs;

int main(int argc, char **argv) {
  int bad = 0;
  for (int i = 1; i < argc; i++) {
    std::ifstream f(argv[i]);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Model m;
    std::string err;
    if (!parse_profile(text, m, &err)) {
      printf("FAIL %s: %s\n", argv[i], err.c_str());
      bad++;
      continue;
    }
    printf("ok %s: %s, capture %s %u Hz x%u, %zu mic channel(s), ring %d LEDs, keys '%s', mute '%s'%s\n", argv[i], m.codename.c_str(), m.capture_pcm.c_str(), m.capture_rate,
           m.capture_channels, m.mic_channels.size(), m.ring.count, m.keys_input.c_str(), m.mute_input.c_str(), m.mute_key ? " (key)" : "");
  }
  return bad ? 1 : 0;
}
