// What the speaker tells about itself for the system widgets of the panel: the load of the processor, its temperature, the memory and the free room
// on /data. One line of "key=value" pairs divided by ';' (sizes in kilobytes, the temperature in degrees Celsius, missing values left out).
#pragma once

#include <cstdint>
#include <string>

namespace cs {

class SysInfo {
 public:
  // Who the speaker is, for the "About" page of the panel (the values are put into the line as they are, ';' becomes ',').
  struct Who {
    std::string name, model, codename, version;
  };
  // The line for the text sensor. The load is counted from the previous call (the first call has nothing to compare with and says 0).
  std::string line(const Who &who);

 private:
  uint64_t busy_ = 0, total_ = 0;
  bool have_prev_ = false;
};

}  // namespace cs
