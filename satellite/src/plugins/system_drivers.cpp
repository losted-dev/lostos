// The drivers of the modules: what each of them needs on this speaker (configuration, system files) and which programs it runs.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <limits.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "plugins/asound_conf.h"
#include "plugins/system.h"

namespace cs {

namespace {

constexpr const char *kNameFile = "/data/lostos/speaker-name";
constexpr const char *kSelf = "/data/lostos/lostos";     // the event hook of librespot starts this very program
constexpr const char *kAsoundConf = "/etc/asound.conf";
constexpr const char *kAsoundOrig = "/data/lostos/asound.orig";
constexpr const char *kAsoundEq = "/data/lostos/asound.eq";   // the configuration lostos runs on (the name stays from the time it was only the equaliser)
constexpr const char *kEqLib = "/data/lostos/alsa/libasound_module_pcm_lostoseq.so";
constexpr const char *kAplay = "/usr/bin/aplay";
constexpr const char *kBlueAplay = "/usr/bin/bluealsa-aplay";

std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }
bool executable(const std::string &path) { return access(path.c_str(), X_OK) == 0; }

// Written next to the target and renamed: a reader never sees half a file.
bool put_file(const std::string &path, const std::string &text, mode_t mode = 0644) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    if (!f) return false;
  }
  chmod(tmp.c_str(), mode);
  return rename(tmp.c_str(), path.c_str()) == 0;
}

// Runs a program to the end (at most `limit_ms`); returns its exit status, -1 when it did not start or was killed.
int run_wait(const std::vector<std::string> &argv_in, int limit_ms) {
  std::vector<char *> argv;
  for (auto &a : argv_in) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    const int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
      dup2(nul, 0);
      dup2(nul, 1);
      dup2(nul, 2);
    }
    for (int fd = 3; fd < 1024; fd++) close(fd);
    execv(argv[0], argv.data());
    _exit(127);
  }
  for (int waited = 0; waited <= limit_ms; waited += 50) {
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    usleep(50 * 1000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  return -1;
}

// Every process whose program is `exe`.
std::vector<pid_t> pids_of(const std::string &exe_path) {
  char real[PATH_MAX];
  const std::string exe = realpath(exe_path.c_str(), real) ? std::string(real) : exe_path;   // /proc shows the real path
  std::vector<pid_t> out;
  DIR *d = opendir("/proc");
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char buf[256];
    const ssize_t n = readlink((std::string("/proc/") + e->d_name + "/exe").c_str(), buf, sizeof buf - 1);
    if (n > 0 && std::string(buf, size_t(n)) == exe) out.push_back(pid_t(atoi(e->d_name)));
  }
  closedir(d);
  return out;
}

pid_t pid_of_comm(const std::string &comm) {
  pid_t found = 0;
  DIR *d = opendir("/proc");
  if (!d) return 0;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    std::string c = slurp(std::string("/proc/") + e->d_name + "/comm");
    while (!c.empty() && c.back() == '\n') c.pop_back();
    if (c == comm) {
      found = pid_t(atoi(e->d_name));
      break;
    }
  }
  closedir(d);
  return found;
}

}  // namespace

// How the speaker calls itself to the world (AirPlay, Spotify): the owner's name in /data/lostos/speaker-name, else the name of the model.
std::string speaker_name(const Model &model) {
  std::string n = slurp(kNameFile);
  while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
  if (!n.empty()) return n;
  const std::string &c = model.codename;
  if (c == "bergamot") return "Я.Станция Мини 3";
  if (c == "plum") return "Я.Станция Лайт 2";
  if (c == "cucumber") return "Я.Станция Миди";
  if (c == "station-micro") return "Я.Станция Лайт";
  if (c == "station-mini2") return "Я.Станция Мини 2";
  return "Я.Станция";
}

// The owner's name for the speaker ("" brings the name of the model back). False when it cannot be written.
bool set_speaker_name(const std::string &name) {
  if (name.empty()) {
    unlink(kNameFile);
    return true;
  }
  return put_file(kNameFile, name + "\n");
}

namespace {

// AirPlay needs a stable 48-bit device id: the MAC of the Wi-Fi.
std::string device_id_hex() {
  std::string mac = slurp("/sys/class/net/wlan0/address");
  if (mac.empty()) mac = slurp("/sys/class/net/eth0/address");
  std::string hex;
  for (char ch : mac)
    if (isxdigit(static_cast<unsigned char>(ch))) hex += char(toupper(static_cast<unsigned char>(ch)));
  return hex.size() == 12 ? hex : "ACBA00000001";
}

bool remount_root(bool rw) { return run_wait({"/bin/mount", "-o", rw ? "remount,rw" : "remount,ro", "/"}, 5000) == 0; }

// ALSA reads $HOME/.asoundrc. Both receivers play through a softvol of their own: lostos lowers it while the assistant
// listens or talks (the control names are the ones the AirPlayLink writes).
void write_asoundrc(const std::string &dir) {
  const bool system_pcm = slurp(kAsoundConf).find("pcm.speakers") != std::string::npos;
  // the codec clocks at 48 kHz even if it accepts 44.1 kHz, so never hand it 44.1 directly
  const std::string out = system_pcm ? "speakers" : "sps_out";
  std::ostringstream o;
  if (!system_pcm) o << "pcm.sps_out {\n  type plug\n  slave { pcm \"hw:0,0\"; rate 48000 }\n}\n";
  for (const auto &v : {std::make_pair("sps_duck", "AirPlay Duck"), std::make_pair("sps_spotify", "Spotify Duck")}) {
    o << "pcm." << v.first << " {\n  type softvol\n  slave.pcm \"" << out << "\"\n  control { name \"" << v.second
      << "\"; card 0 }\n  min_dB -40.0\n  max_dB 0.0\n  resolution 256\n}\n";
  }
  put_file(dir + "/.asoundrc", o.str(), 0644);
}

bool asound_mounted() { return slurp("/proc/self/mounts").find(std::string(" ") + kAsoundConf + " ") != std::string::npos; }

struct SoundState {
  bool eq = false;                    // the equaliser is in front of the mixer
  std::string keeper_pcm = "speakers";  // what the process that holds the mixer plays to
};

// The sound configuration of lostos (plugins/asound_conf.h): the one of the firmware, plus the PCM "speakers" where the firmware has none, plus the
// equaliser where it fits. /etc/asound.conf is read-only firmware: it is covered by a copy (bind mount). A process reads it once, so this runs before
// anything of lostos touches ALSA. The mixer must still open afterwards, else the original comes back.
SoundState prepare_asound() {
  SoundState st;
  const std::string current = slurp(kAsoundConf);
  if (!exists(kAsoundOrig)) put_file(kAsoundOrig, current);       // the original of the firmware, kept for the next starts
  const std::string orig = slurp(kAsoundOrig);
  const AsoundPlan plan = adapt_asound(orig, exists(kEqLib) ? kEqLib : "");
  if (plan.speakers_added) st.keeper_pcm = plan.slave;            // the keeper must not create the volume control of the mixer: that is for lostos
  if (plan.text == orig) {                                        // nothing to change on this firmware
    if (asound_mounted() && current != orig) umount(kAsoundConf);   // a copy of an older lostos that is not needed any more
    return st;
  }
  st.eq = plan.eq;
  if (asound_mounted() && current == plan.text) return st;       // an earlier lostos has put just this in
  if (asound_mounted()) umount(kAsoundConf);
  if (!put_file(kAsoundEq, plan.text)) return SoundState{};
  if (mount(kAsoundEq, kAsoundConf, nullptr, MS_BIND, nullptr) != 0) return SoundState{};
  if (run_wait({kAplay, "-q", "-D", "speakers", "-d", "1", "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2", "/dev/zero"}, 4000) != 0) {
    LOGW("audio: the changed asound.conf does not open, the original stays");
    umount(kAsoundConf);
    return SoundState{};
  }
  if (plan.speakers_added) LOGI("audio: the firmware has no PCM \"speakers\": added over \"%s\"", plan.slave.c_str());
  if (plan.eq) LOGI("audio: equaliser mounted over %s", kAsoundConf);
  return st;
}

// avahi needs a user "avahi" and a dbus policy on the (read-only) system: put them in once.
bool ensure_avahi(const std::string &dir) {
  const std::string policy = "/etc/dbus-1/system.d/avahi-dbus.conf";
  const bool have_user = slurp("/etc/passwd").find("\navahi:") != std::string::npos || slurp("/etc/passwd").rfind("avahi:", 0) == 0;
  if (have_user && exists(policy)) return true;
  if (!remount_root(true)) return false;
  bool ok = true;
  if (!have_user) {
    std::ofstream pw("/etc/passwd", std::ios::app);
    pw << "avahi:x:105:105:avahi:/var/run/avahi-daemon:/bin/false\n";
    ok = ok && bool(pw);
  }
  if (slurp("/etc/group").find("\navahi:") == std::string::npos && slurp("/etc/group").rfind("avahi:", 0) != 0) {
    std::ofstream gr("/etc/group", std::ios::app);
    gr << "avahi:x:105:\n";
  }
  ok = ok && put_file(policy, slurp(dir + "/avahi-dbus.conf"));
  remount_root(false);
  if (const pid_t dbus = pid_of_comm("dbus-daemon")) kill(dbus, SIGHUP);
  if (ok) sleep(1);   // dbus reads the policy
  return ok;
}

}  // namespace

// ---- audio: the equaliser over the system mixer and the process that holds the mixer

void add_audio_core(SystemPlugins &mods, std::vector<SysPlugin> &out) {
  SysPlugin m;
  m.id = "audio";
  m.title = "Звук";
  m.dir = "/data/lostos";                                  // the keeper only needs a place to stand: not a folder of the plugin list
  m.core = true;
  m.usable = [] { return executable(kAplay); };
  static SoundState sound;   // decided once per start of lostos, before anybody opens ALSA
  m.prepare = [] {
    const bool was_mounted = asound_mounted();
    const std::string before = slurp(kAsoundConf);
    sound = prepare_asound();
    // the Bluetooth player of the system read the old file: it is started again (below) to read the new one
    if (sound.eq && (!was_mounted || before != slurp(kAsoundConf))) {
      for (pid_t p : pids_of(kBlueAplay)) kill(p, SIGTERM);
    }
    return true;
  };
  m.plan = [] {
    std::vector<SysProc> v;
    // The dmix mixer lives inside whichever process opened it first: restarting that process (lostos, librespot) used to freeze
    // everybody else. A long-lived silent stream owns it instead.
    SysProc keeper;
    keeper.name = "mixer-keeper";
    keeper.exe = kAplay;
    keeper.args = {"-q", "-D", sound.keeper_pcm, "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2", "/dev/zero"};
    keeper.match = "/dev/zero";
    keeper.primary = true;
    v.push_back(keeper);
    if (sound.eq && executable(kBlueAplay)) {
      SysProc bt;
      bt.name = "bluealsa-aplay";
      bt.exe = kBlueAplay;
      bt.args = {"-D", "bluetooth", "-M", "disable", "-S"};
      bt.delay_ms = 1500;
      bt.primary = true;
      v.push_back(bt);
    }
    return v;
  };
  (void)mods;
  out.push_back(std::move(m));
}

// ---- AirPlay 2: nqptp (the clock), avahi (the announcement), shairport-sync (the receiver)

void add_airplay_plugin(SystemPlugins &mods, std::vector<SysPlugin> &out) {
  SysPlugin m;
  m.id = "ru.lostos.airplay";
  m.title = "AirPlay 2";
  m.dir = std::string(kPluginsDir) + "/ru.lostos.airplay";
  const std::string dir = m.dir;
  m.usable = [dir] { return executable(dir + "/shairport-sync-ap2"); };
  m.prepare = [&mods, dir] {
    write_asoundrc(dir);
    if (!ensure_avahi(dir)) LOGW("airplay: could not set up avahi (user, dbus policy)");
    // output: lostos does the buttons, LEDs and display; the receiver plays straight to the speakers through its softvol
    std::ostringstream c;
    c << "general = {\n  name = \"" << speaker_name(mods.model()) << "\";\n  output_backend = \"alsa\";\n  mdns_backend = \"avahi\";\n"
      << "  drift_tolerance_in_seconds = 0.005;\n"
      << "  ignore_volume_control = \"yes\";   // the volume is the speaker's own, lostos moves it\n"
      << "  volume_range_db = 60;\n  airplay_device_id = 0x" << device_id_hex() << "L;\n};\n"
      << "alsa = {\n  output_device = \"sps_duck\";\n  output_format = \"S16\";\n  use_mmap_if_available = \"no\";\n};\n"
      << "sessioncontrol = { allow_session_interruption = \"yes\"; };\n"
      << "diagnostics = { log_verbosity = 0; };\n"
      << "metadata = { enabled = \"yes\"; include_cover_art = \"yes\"; pipe_name = \"/tmp/shairport-sync-ap2-metadata\"; };\n";
    return put_file(dir + "/shairport.conf", c.str());
  };
  m.plan = [dir] {
    const std::vector<std::pair<std::string, std::string>> env = {{"LD_LIBRARY_PATH", dir + "/libs"}, {"HOME", dir}};
    SysProc nq;
    nq.name = "nqptp";
    nq.exe = dir + "/nqptp";
    nq.env = env;
    SysProc av;
    av.name = "avahi-daemon";
    av.exe = dir + "/avahi-daemon";
    av.args = {"--no-drop-root", "--no-chroot", "-f", dir + "/avahi-daemon.conf"};
    av.env = env;
    SysProc sp;
    sp.name = "shairport-sync";
    sp.exe = dir + "/shairport-sync-ap2";
    sp.args = {"-c", dir + "/shairport.conf"};
    sp.env = env;
    sp.delay_ms = 2000;
    sp.primary = true;
    return std::vector<SysProc>{nq, av, sp};
  };
  out.push_back(std::move(m));
}

// ---- Spotify Connect: librespot. The phone's Spotify app logs in over zeroconf, nothing is typed on the speaker.

void add_spotify_plugin(SystemPlugins &mods, std::vector<SysPlugin> &out) {
  SysPlugin m;
  m.id = "ru.lostos.spotify";
  m.title = "Spotify Connect";
  m.dir = std::string(kPluginsDir) + "/ru.lostos.spotify";
  const std::string dir = m.dir;
  m.usable = [dir] { return executable(dir + "/librespot"); };
  m.prepare = [dir] {
    mkdir((dir + "/cache").c_str(), 0700);
    write_asoundrc(dir);
    return true;
  };
  m.plan = [&mods, dir] {
    SysProc lr;
    lr.name = "librespot";
    lr.exe = dir + "/librespot";
    lr.args = {"--name", speaker_name(mods.model()), "--backend", "alsa", "--device", "sps_spotify", "--device-type", "speaker",
               "--cache", dir + "/cache", "--disable-audio-cache", "--volume-ctrl", "fixed", "--autoplay", "on", "--bitrate", "320",
               "--zeroconf-backend", "libmdns", "--onevent", std::string(kSelf) + " --spotify-event", "--quiet"};
    lr.env = {{"HOME", dir}};
    lr.primary = true;
    return std::vector<SysProc>{lr};
  };
  out.push_back(std::move(m));
}

// ---- the event hook of librespot (it runs as a program of its own, once per event)

int run_spotify_event() {
  constexpr const char *kFifo = "/tmp/spotify-events";
  const char *ev = getenv("PLAYER_EVENT");
  if (!ev) return 0;
  const std::string e = ev;
  auto env = [](const char *k) { const char *v = getenv(k); return std::string(v ? v : ""); };
  auto first_line = [](const std::string &s) { return s.substr(0, s.find('\n')); };
  auto last_line = [](std::string s) {
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s.substr(s.rfind('\n') == std::string::npos ? 0 : s.rfind('\n') + 1);
  };

  std::string out;
  const std::string pos = env("POSITION_MS");
  if (e == "seeked" || e == "position_correction") {
    out = "pos " + pos + "\n";
  } else if (e == "playing" || e == "paused" || e == "stopped" || e == "session_disconnected") {
    if (!pos.empty()) out += "pos " + pos + "\n";
    out += e + "\n";
    put_file("/tmp/spotify-state", e + "\n", 0644);
  } else if (e == "volume_changed") {
    out = "volume " + env("VOLUME") + "\n";
  } else if (e == "loading") {
    out = "loading\n";
  } else if (e == "track_changed") {
    out = "track\n";
    const std::string name = env("NAME");
    if (!name.empty()) {   // "Artist - Title <TAB> cover <TAB> uri <TAB> duration": lostos reads it again when it restarts during the music
      const std::string line = first_line(env("ARTISTS")) + " - " + name + "\t" + last_line(env("COVERS")) + "\t" + env("URI") + "\t" + env("DURATION_MS");
      out += "now " + line + "\n";
      put_file("/tmp/spotify-now", line, 0644);
    }
  }
  if (out.empty()) return 0;
  const int fd = open(kFifo, O_WRONLY | O_NONBLOCK);   // nobody reads (lostos is not running): nothing to tell
  if (fd < 0) return 0;
  if (write(fd, out.data(), out.size()) < 0) {}
  close(fd);
  return 0;
}

}  // namespace cs
