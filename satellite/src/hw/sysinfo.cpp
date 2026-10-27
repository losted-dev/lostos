#include "hw/sysinfo.h"

#include <sys/statvfs.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cs {

namespace {

// The hottest thermal zone of the speaker, in degrees (-1000 when the kernel tells nothing).
double temperature() {
  double best = -1000;
  for (int i = 0; i < 8; i++) {
    std::ifstream f("/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp");
    long milli = 0;
    if (f >> milli && milli > -50000 && milli < 150000) best = std::max(best, milli / 1000.0);
  }
  return best;
}

// A value in the line: no ';' (it divides the pairs) and no line breaks.
std::string clean(std::string v) {
  for (char &c : v)
    if (c == ';' || c == '\n' || c == '\r') c = ',';
  return v;
}

// The version of the firmware of Yandex that runs under lostos (the stock "version_full"), read once.
const std::string &firmware() {
  static const std::string fw = [] {
    std::ifstream f("/system/vendor/quasar/version_full");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
  }();
  return fw;
}

// The chip of the speaker as "Amlogic AXG · 4× Cortex-A53": the maker and the family from the device tree, the cores from /proc/cpuinfo. Read once.
const std::string &chip() {
  static const std::string c = [] {
    std::string soc;
    {
      std::ifstream f("/proc/device-tree/compatible");
      std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), last;
      std::stringstream ss(all);
      for (std::string item; std::getline(ss, item, '\0');)
        if (!item.empty()) last = item;                                   // the last one is the most general: "amlogic, axg"
      const size_t comma = last.find(',');
      std::string maker = comma == std::string::npos ? "" : last.substr(0, comma), family = comma == std::string::npos ? last : last.substr(comma + 1);
      auto trim = [](std::string s) {
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
      };
      maker = trim(maker), family = trim(family);
      for (char &ch : family) ch = char(toupper((unsigned char)ch));
      if (!maker.empty()) maker[0] = char(toupper((unsigned char)maker[0]));
      soc = (maker.empty() ? "" : maker + " ") + family;
    }
    int cores = 0;
    std::string part;
    {
      std::ifstream f("/proc/cpuinfo");
      for (std::string l; std::getline(f, l);) {
        if (l.rfind("processor", 0) == 0) cores++;
        else if (l.rfind("CPU part", 0) == 0 && part.empty()) part = l.substr(l.find(':') + 1);
      }
    }
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    static const struct { const char *id, *name; } kParts[] = {{"0xc07", "Cortex-A7"},  {"0xc09", "Cortex-A9"}, {"0xc0f", "Cortex-A15"}, {"0xd03", "Cortex-A53"},
                                                              {"0xd04", "Cortex-A35"}, {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"}, {"0xd08", "Cortex-A72"}};
    std::string core;
    for (const auto &p : kParts)
      if (part == p.id) core = p.name;
    std::string out = soc;
    if (cores > 0 && !core.empty()) out += (out.empty() ? "" : " · ") + std::to_string(cores) + "× " + core;
    return out;
  }();
  return c;
}

}  // namespace

std::string SysInfo::line(const Who &who) {
  std::ostringstream o;
  // /proc/stat: "cpu user nice system idle iowait irq softirq steal ..."
  {
    std::ifstream f("/proc/stat");
    std::string name;
    uint64_t v[8] = {0};
    if (f >> name && name == "cpu") {
      for (uint64_t &x : v) f >> x;
      const uint64_t idle = v[3] + v[4];
      uint64_t total = 0;
      for (uint64_t x : v) total += x;
      const uint64_t busy = total - idle;
      int pct = 0;
      if (have_prev_ && total > total_) pct = int((busy - busy_) * 100 / (total - total_));
      busy_ = busy;
      total_ = total;
      have_prev_ = true;
      o << "cpu=" << (pct < 0 ? 0 : pct > 100 ? 100 : pct);
    }
  }
  const double t = temperature();
  if (t > -999) {
    char b[16];
    snprintf(b, sizeof b, "%.1f", t);
    o << ";temp=" << b;
  }
  {
    std::ifstream f("/proc/meminfo");
    std::string key;
    long long kb = 0, total = 0, avail = -1, free = 0, buffers = 0, cached = 0;
    std::string unit;
    while (f >> key >> kb >> unit) {
      if (key == "MemTotal:") total = kb;
      else if (key == "MemAvailable:") avail = kb;
      else if (key == "MemFree:") free = kb;
      else if (key == "Buffers:") buffers = kb;
      else if (key == "Cached:") cached = kb;
    }
    if (avail < 0) avail = free + buffers + cached;
    if (total > 0) o << ";mem_total=" << total << ";mem_used=" << (total - avail);
  }
  struct statvfs sv;
  if (statvfs("/data", &sv) == 0 && sv.f_blocks) {
    const unsigned long long frsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    o << ";disk_total=" << (unsigned long long)sv.f_blocks * frsize / 1024 << ";disk_free=" << (unsigned long long)sv.f_bavail * frsize / 1024;
  }
  o << ";name=" << clean(who.name) << ";model=" << clean(who.model) << ";code=" << clean(who.codename) << ";ver=" << clean(who.version);
  if (!firmware().empty()) o << ";fw=" << clean(firmware());
  if (!chip().empty()) o << ";chip=" << clean(chip());
  return o.str();
}

}  // namespace cs
