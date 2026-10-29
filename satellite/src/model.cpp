#include "model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "core/log.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace cs {

namespace {

// Station Midi: 64-bit processor, 32-bit userland on the firmware of 2026-10 (the installer takes the programs by the loader it finds, see
// userlandArch), 4 mics + 4 speaker reference channels at 96 kHz, 24-LED ring + 6-LED
// logo, clock on two aw21036/aw21012 chips.
Model midi() {
  Model m;
  m.codename = "cucumber";
  m.host_prefix = "yandex-midi-";
  m.friendly_name = "Яндекс Станция Миди";
  m.model_name = "Yandex Station Midi";
  m.capture_pcm = "hw:0,2";
  m.capture_rate = 96000;
  m.capture_channels = 8;
  m.mic_channels = {0, 1, 2, 3};
  m.spk_channels = {4, 5, 6, 7};
  m.vqe_preset = "cucumber";
  m.led_prefix = "cucumber";
  m.ring = {24, 0, "/dev/fb0", "ring", "ledfb-ring", "ring"};
  m.logo = {6, 0, "/dev/fb1", "logo", "ledfb-logo", "logo"};
  m.clock_dir = "/sys/class/leds/cucumber:white:clock-";
  m.clock_seg_base = 'a';
  m.clock_pos_base = 1;
  // p3 + q3 form the colon; the rest are auxiliary dots/icons maind never lights for the time.
  m.clock_colon = {"p3", "q3"};
  m.clock_aux = {"deg", "h1", "h2", "l1", "l2", "m1", "m2", "p1", "p2", "p4", "p5", "q1", "q2", "q4", "q5"};
  m.clock_chip_current = true;
  m.keys_input = "awinic_sar";
  m.mute_input = "mute";
  m.error_sound = "ru-RU/vins_error.wav";
  // { "0": 0, "1": 49, "20": 255 }: softvol min_dB -60, so 2.6 dB per level, level 1 at -48.5 dB.
  m.volume_map = {{0, 0}, {1, 49}, {20, 255}};
  // Telink B91 with Yandex's gateway firmware (quasar.cfg zigbee.tty.port). Reset and boot are
  // GPIOZ_11 / GPIOZ_9 (devicetree node "zigbee", driver zb_control); maind drives them through
  // these files when quasar.cfg zigbee.zbControl is set, as it is on this firmware.
  m.zigbee_tty = "/dev/ttyS2";
  m.zigbee_reset = "/sys/devices/platform/zigbee/zb-reset/value";
  m.zigbee_boot = "/sys/devices/platform/zigbee/zb-boot/value";
  return m;
}

// Station Lite 2: 32-bit userland, 2 PDM mics (capture channels 0 and 2) + speaker feedback
// (channel 6) at 48 kHz on hw:0,1 (layout of multi_capture_raw in /etc/asound.conf), 3-LED ring,
// clock LEDs "plum:white:clock<A..H><0..3>" + DH/DL (colon), DM, DEGREE (quasar.cfg /leds/clock).
Model lite2() {
  Model m;
  m.codename = "plum";
  m.host_prefix = "yandex-lite2-";
  m.friendly_name = "Яндекс Станция Лайт 2";
  m.model_name = "Yandex Station Lite 2";
  m.capture_pcm = "hw:0,1";
  m.capture_rate = 48000;
  m.capture_channels = 8;
  m.mic_channels = {0, 2};
  m.spk_channels = {6};
  m.vqe_preset = "plum";
  m.led_prefix = "plum";
  m.ring = {3, 1, "/dev/fb0", "YNDX LED ring", "led_ring_fb", "ring"};
  m.clock_dir = "/sys/class/leds/plum:white:clock";
  m.clock_seg_base = 'A';
  m.clock_pos_base = 0;
  m.clock_colon = {"DH", "DL"};
  m.clock_aux = {"H0", "H1", "H2", "H3", "DM", "DEGREE"};
  m.clock_chip_current = false;
  m.keys_input = "aw9610x";
  m.mute_input = "gpio-keys";
  m.error_sound = "ru-RU/playback_error.mp3";
  m.volume_map = {{0, 0}, {1, 49}, {20, 255}};  // quasar.cfg: the same map as the Midi
  return m;
}

// Station Mini 3 (bergamot): 32-bit userland (glibc 2.30), 237 MB RAM. Capture is the firmware's
// "default" PCM (asym -> multi_capture in /etc/asound.conf: three PDM mics on channels 0..2 and the
// speaker reference as channel 3, 48 kHz; quasar.cfg audioInput.audioDevice: inRate 48000,
// micChannels 3, spkChannels 1, preset "bergamot"). The 6-LED RGB ring is the "logo" framebuffer
// (ledfb-logo trigger, sysfs LEDs "bergamot:<color>:logo<0..5>"); clock LEDs
// "bergamot:white:clock<A..G><0..3>" + DH/DL (colon), DM, DEGREE, dimmed by the brightness value itself.
Model mini3() {
  Model m;
  m.codename = "bergamot";
  m.host_prefix = "yandex-mini3-";
  m.friendly_name = "Яндекс Станция Мини 3";
  m.model_name = "Yandex Station Mini 3";
  m.capture_pcm = "default";
  m.capture_rate = 48000;
  m.capture_channels = 4;
  m.mic_channels = {0, 1, 2};
  m.spk_channels = {3};
  m.vqe_preset = "bergamot";
  m.led_prefix = "bergamot";
  m.ring = {6, 0, "/dev/fb0", "logo", "ledfb-logo", "logo"};
  m.clock_dir = "/sys/class/leds/bergamot:white:clock";
  m.clock_seg_base = 'A';
  m.clock_pos_base = 0;
  m.clock_colon = {"DH", "DL"};
  m.clock_aux = {"DM", "DEGREE"};
  m.clock_chip_current = false;
  m.keys_input = "aw933xx";
  m.mute_input = "gpio-keys";
  m.error_sound = "ru-RU/playback_error.mp3";
  m.volume_map = {{0, 0}, {1, 33}, {20, 255}};  // quasar.cfg: platformToAlsaMap { "0": 0, "1": 33, "20": 255 }
  return m;
}

}  // namespace

std::vector<Model> builtin_models() { return {midi(), lite2(), mini3()}; }

namespace {

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty() || !out.empty()) out.push_back(cur);
  return out;
}

std::string join(const std::vector<std::string> &v, char sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) out += (i ? std::string(1, sep) : std::string()) + v[i];
  return out;
}

std::vector<unsigned> numbers(const std::string &s) {
  std::vector<unsigned> out;
  for (const std::string &p : split(s, ',')) out.push_back(unsigned(strtoul(p.c_str(), nullptr, 10)));
  return out;
}

// "count,first_index,fb_dev,fb_name,trigger,sysfs_kind"
bool strip_from(const std::string &s, Model::LedStrip &st) {
  const std::vector<std::string> f = split(s, ',');
  if (f.size() != 6 && f.size() != 8) return false;       // 6 fields; or 8 with the format and the number of dots of the framebuffer (a flat panel)
  st = {atoi(f[0].c_str()), atoi(f[1].c_str()), f[2], f[3], f[4], f[5]};
  if (f.size() == 8) {
    st.format = f[6];
    st.fb_pixels = atoi(f[7].c_str());
  }
  return true;
}

std::string strip_to(const Model::LedStrip &st) {
  std::string o = std::to_string(st.count) + "," + std::to_string(st.first_index) + "," + st.fb_dev + "," + st.fb_name + "," + st.trigger + "," +
                  st.sysfs_kind;
  if (!st.format.empty()) o += "," + st.format + "," + std::to_string(st.fb_pixels);
  return o;
}

}  // namespace

std::string dump_profile(const Model &m) {
  std::ostringstream o;
  std::vector<std::string> mics, spks, vm;
  for (unsigned n : m.mic_channels) mics.push_back(std::to_string(n));
  for (unsigned n : m.spk_channels) spks.push_back(std::to_string(n));
  for (const auto &p : m.volume_map) vm.push_back(std::to_string(p.level) + ":" + std::to_string(p.raw));
  o << "# lostOS model profile: one key=value per line, '#' starts a comment (see docs/models.md)\n"
    << "codename=" << m.codename << "\nhost_prefix=" << m.host_prefix << "\nfriendly_name=" << m.friendly_name << "\nmodel_name=" << m.model_name << "\n"
    << "capture_pcm=" << m.capture_pcm << "\ncapture_rate=" << m.capture_rate << "\ncapture_channels=" << m.capture_channels << "\n"
    << (m.capture_format.empty() ? "" : "capture_format=" + m.capture_format + "\n")
    << (m.ref_pcm.empty() ? "" : "ref_pcm=" + m.ref_pcm + "\nref_rate=" + std::to_string(m.ref_rate) + "\nref_channels=" + std::to_string(m.ref_channels) + "\n")
    << (m.ref_format.empty() ? "" : "ref_format=" + m.ref_format + "\n")
    << "mic_channels=" << join(mics, ',') << "\nspk_channels=" << join(spks, ',') << "\nvqe_preset=" << m.vqe_preset << "\n"
    << "led_prefix=" << m.led_prefix << "\nring=" << strip_to(m.ring) << "\n";
  if (m.logo.count > 0) o << "logo=" << strip_to(m.logo) << "\n";
  o << "clock_dir=" << m.clock_dir << "\nclock_seg_base=" << m.clock_seg_base << "\nclock_pos_base=" << m.clock_pos_base << "\n"
    << "clock_colon=" << join(m.clock_colon, ',') << "\nclock_aux=" << join(m.clock_aux, ',') << "\nclock_chip_current=" << (m.clock_chip_current ? 1 : 0)
    << "\n"
    << "keys_input=" << m.keys_input << "\nmute_input=" << m.mute_input << (m.mute_key ? "\nmute_key=1" : "") << "\nerror_sound=" << m.error_sound << "\nvolume_map=" << join(vm, ',') << "\n"
    << (m.action_gpio.empty() ? "" : "action_gpio=" + m.action_gpio + "\n") << (m.mute_gpio.empty() ? "" : "mute_gpio=" + m.mute_gpio + "\n")
    << (m.wheel_gpio.empty() ? "" : "wheel_gpio=" + m.wheel_gpio + "\n")
    << (m.audio_sink.empty() ? "" : "audio_sink=" + m.audio_sink + "\n") << (m.mixer.empty() ? "" : "mixer=" + m.mixer + "\n")
    << (m.hardvol_file.empty() ? "" : "hardvol=tinyalsa:" + std::to_string(m.hardvol_card) + "," + m.hardvol_file + "\n");
  if (!m.zigbee_tty.empty())
    o << "zigbee_tty=" << m.zigbee_tty << "\nzigbee_reset=" << m.zigbee_reset << "\nzigbee_boot=" << m.zigbee_boot << "\n";
  return o.str();
}

bool parse_profile(const std::string &text, Model &m, std::string *error) {
  std::istringstream in(text);
  std::string line;
  int n = 0;
  auto fail = [&](const std::string &why) {
    if (error) *error = "line " + std::to_string(n) + ": " + why;
    return false;
  };
  while (std::getline(in, line)) {
    n++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return fail("no '='");
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "codename") {
      m.codename = v;
    } else if (k == "host_prefix") {
      m.host_prefix = v;
    } else if (k == "friendly_name") {
      m.friendly_name = v;
    } else if (k == "model_name") {
      m.model_name = v;
    } else if (k == "capture_pcm") {
      m.capture_pcm = v;
    } else if (k == "capture_rate") {
      m.capture_rate = unsigned(strtoul(v.c_str(), nullptr, 10));
    } else if (k == "capture_channels") {
      m.capture_channels = unsigned(strtoul(v.c_str(), nullptr, 10));
    } else if (k == "capture_format") {
      if (v != "s16" && v != "s32x2") return fail("capture_format must be s16 or s32x2");
      m.capture_format = v == "s16" ? "" : v;
    } else if (k == "ref_pcm") {
      m.ref_pcm = v;
    } else if (k == "ref_rate") {
      m.ref_rate = unsigned(strtoul(v.c_str(), nullptr, 10));
    } else if (k == "ref_channels") {
      m.ref_channels = unsigned(strtoul(v.c_str(), nullptr, 10));
    } else if (k == "ref_format") {
      if (v != "s16" && v != "s32") return fail("ref_format must be s16 or s32");
      m.ref_format = v == "s16" ? "" : v;
    } else if (k == "mic_channels") {
      m.mic_channels = numbers(v);
    } else if (k == "spk_channels") {
      m.spk_channels = numbers(v);
    } else if (k == "vqe_preset") {
      m.vqe_preset = v;
    } else if (k == "led_prefix") {
      m.led_prefix = v;
    } else if (k == "ring") {
      if (!strip_from(v, m.ring)) return fail("ring needs 6 or 8 fields");
    } else if (k == "logo") {
      if (!strip_from(v, m.logo)) return fail("logo needs 6 or 8 fields");
    } else if (k == "clock_dir") {
      m.clock_dir = v;
    } else if (k == "clock_seg_base") {
      m.clock_seg_base = v.empty() ? 'a' : v[0];
    } else if (k == "clock_pos_base") {
      m.clock_pos_base = atoi(v.c_str());
    } else if (k == "clock_colon") {
      m.clock_colon = split(v, ',');
    } else if (k == "clock_aux") {
      m.clock_aux = split(v, ',');
    } else if (k == "clock_chip_current") {
      m.clock_chip_current = v == "1";
    } else if (k == "keys_input") {
      m.keys_input = v;
    } else if (k == "mute_input") {
      m.mute_input = v;
    } else if (k == "mute_key") {
      m.mute_key = v == "1";
    } else if (k == "action_gpio" || k == "mute_gpio") {
      unsigned pin;
      char level[8];
      if (sscanf(v.c_str(), "%u,%7s", &pin, level) != 2 || (strcmp(level, "low") && strcmp(level, "high")))
        return fail(k + " is <pin>,<low|high>");
      (k == "action_gpio" ? m.action_gpio : m.mute_gpio) = v;
    } else if (k == "wheel_gpio") {
      unsigned a, b, steps;
      if (sscanf(v.c_str(), "%u,%u,%u", &a, &b, &steps) != 3 || a == b || steps == 0 || steps > 4)
        return fail("wheel_gpio is <pin a>,<pin b>,<1..4 transitions per step>");
      m.wheel_gpio = v;
    } else if (k == "audio_sink") {
      m.audio_sink = v;
    } else if (k == "mixer") {
      if (v != "alsa" && v != "soft" && v != "hardvol") return fail("mixer must be alsa, soft or hardvol");
      m.mixer = v == "alsa" ? "" : v;
    } else if (k == "hardvol") {
      const size_t c = v.find(',');
      if (v.rfind("tinyalsa:", 0) != 0 || c == std::string::npos || c == 9 || v.find_first_not_of("0123456789", 9) != c ||
          c + 1 >= v.size() || v[c + 1] != '/')
        return fail("hardvol is tinyalsa:<card>,</absolute/path/of/the/table.xml>");
      m.hardvol_card = atoi(v.c_str() + 9);
      m.hardvol_file = v.substr(c + 1);
    } else if (k == "error_sound") {
      m.error_sound = v;
    } else if (k == "volume_map") {
      m.volume_map.clear();
      for (const std::string &p : split(v, ',')) {
        const size_t c = p.find(':');
        if (c == std::string::npos) return fail("volume_map entries are level:raw");
        m.volume_map.push_back({atoi(p.substr(0, c).c_str()), atoi(p.substr(c + 1).c_str())});
      }
    } else if (k == "zigbee_tty") {
      m.zigbee_tty = v;
    } else if (k == "zigbee_reset") {
      m.zigbee_reset = v;
    } else if (k == "zigbee_boot") {
      m.zigbee_boot = v;
    } else {
      return fail("unknown key '" + k + "'");
    }
  }
  if (m.codename.empty() || m.capture_pcm.empty() || m.capture_rate == 0 || m.mic_channels.empty())
    return fail("codename, capture_pcm, capture_rate and mic_channels are required");
  if (m.capture_format == "s32x2" && m.capture_channels % 2) return fail("capture_format s32x2 needs an even capture_channels (two lanes per slot)");
  for (unsigned c : m.mic_channels)
    if (c >= m.capture_channels) return fail("mic channel " + std::to_string(c) + " is not below capture_channels");
  if (!m.ref_pcm.empty() &&
      (m.ref_channels == 0 || m.ref_rate == 0 || m.ref_rate % m.capture_rate != 0 || m.ref_rate / m.capture_rate > 8))
    return fail("ref_pcm needs ref_channels and a ref_rate of 1..8 times capture_rate");
  if ((m.mixer == "hardvol") != !m.hardvol_file.empty()) return fail("mixer=hardvol and hardvol= go together");
  if (m.mixer == "hardvol" && (m.volume_map.size() < 2 || m.volume_map.back().raw > 100))
    return fail("with mixer=hardvol volume_map gives the HAL's percent (0..100)");
  const unsigned lanes = m.capture_channels + (m.ref_pcm.empty() ? 0 : m.ref_channels);
  for (unsigned c : m.spk_channels)
    if (c >= lanes) return fail("speaker channel " + std::to_string(c) + " is not below capture_channels + ref_channels");
  return true;
}

const Model *detect_model() {
  static const Model *model = [] () -> const Model * {
    std::ifstream f("/proc/device-tree/compatible", std::ios::binary);
    std::string compat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
#ifdef __ANDROID__
    // Station 1 (Allwinner, kernel 3.10) exposes no devicetree; Android names the board in
    // ro.product.device ("tulip-d1"), taken as if it were "yandex,<device>".
    if (compat.empty()) {
      char dev[PROP_VALUE_MAX] = "";
      if (__system_property_get("ro.product.device", dev) > 0) compat = std::string("yandex,") + dev + '\0';
    }
#endif
    // NUL-separated list, e.g. "yandex,cucumber-dvt2\0yandex,cucumber\0amlogic, a5\0".
    for (size_t pos = 0; pos < compat.size();) {
      std::string entry = compat.c_str() + pos;
      pos += entry.size() + 1;
      if (entry.rfind("yandex,", 0) != 0) continue;
      const std::string name = entry.substr(7);
      // A profile file wins over the built-in one: a new station is added with a file, no rebuild.
      std::ifstream pf("/data/lostos/models/" + name + ".conf");
      if (pf) {
        std::string text((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
        Model m;
        std::string err;
        if (parse_profile(text, m, &err)) {
          LOGI("model profile /data/lostos/models/%s.conf: %s", name.c_str(), m.friendly_name.c_str());
          return new Model(m);
        }
        LOGE("model profile %s.conf is not usable (%s)", name.c_str(), err.c_str());
      }
      if (entry == "yandex,cucumber") return new Model(midi());
      if (entry == "yandex,plum") return new Model(lite2());
      if (entry == "yandex,bergamot") return new Model(mini3());
    }
    for (char &c : compat)
      if (c == '\0') c = ' ';
    LOGE("unsupported device (devicetree compatible: %s); add /data/lostos/models/<name>.conf", compat.c_str());
    return nullptr;
  }();
  return model;
}

}  // namespace cs
