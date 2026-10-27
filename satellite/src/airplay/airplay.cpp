#include "airplay/airplay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fstream>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <alsa/asoundlib.h>

#include "core/log.h"
#include "plugins/system.h"

namespace cs {

namespace {

constexpr const char *kMetaPipe = "/tmp/shairport-sync-ap2-metadata";
constexpr const char *kAirCover = "/tmp/airplay-cover.img";
constexpr const char *kPluginId = "ru.lostos.airplay";   // the system plugin of the receiver
constexpr const char *kSpotifyPluginId = "ru.lostos.spotify";
constexpr const char *kSpotifyFifo = "/tmp/spotify-events";
constexpr uint16_t kCmdPort = 7001;  // shairport's event-channel thread listens on 127.0.0.1:7001
constexpr int64_t kReopenMs = 2000;
constexpr size_t kMaxItemBuf = 3u << 20;   // a cover picture (PICT) is a whole JPEG in base64
// softvol controls in front of the speakers PCM, one per audio source (softvol keeps a custom name as is, no " Playback Volume")
constexpr const char *kDuckCtls[] = {"AirPlay Duck", "Spotify Duck"};
constexpr float kDuckRangeDb = 40.f;       // must match min_dB of the softvol pcms of the system plugins (plugins/system_drivers.cpp)
constexpr float kDuckDownStep = 0.056f;   // linear gain per 20 ms: full scale in ~250 ms
constexpr float kDuckUpStep = 0.023f;     // ... and back in ~600 ms

int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

std::string b64decode(const char *s, size_t n) {
  std::string out;
  int acc = 0, bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64v(s[i]);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((acc >> bits) & 0xff));
    }
  }
  return out;
}

// The sender's slider: -30..0 dB, -144 = mute -> 0..1. The iPhone moves the slider linearly in dB over that range,
// so the same linear mapping puts the speaker's own slider where the iPhone's is (half way = 50 %).
float slider_to_volume(float db) {
  if (db <= -100.f) return 0.f;
  const float v = (db + 30.f) / 30.f;
  return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

}  // namespace

struct AirPlayLink::Impl {
  Impl(Loop &l, AirPlayLink &o, SystemPlugins &m) : loop(l), owner(o), plugins(m) {}
  Loop &loop;
  SystemPlugins &plugins;
  AirPlayLink &owner;
  int fd = -1;
  uint64_t retry = 0;
  std::string buf;
  float duck_cur = 1.f, duck_target = 1.f;
  uint64_t duck_timer = 0, duck_rewrite = 0;
  snd_ctl_t *ctl = nullptr;
  int sfd = -1;  // Spotify events FIFO (opened read-write: never EOF, writers never block on open)
  std::string sbuf;
  bool duck_ready = false, duck_warned = false;

  ~Impl() {
    if (retry) loop.cancel_timer(retry);
    if (duck_timer) loop.cancel_timer(duck_timer);
    if (duck_rewrite) loop.cancel_timer(duck_rewrite);
    if (ctl) snd_ctl_close(ctl);
    if (sfd >= 0) {
      loop.del_fd(sfd);
      close(sfd);
    }
    close_pipe();
  }

  // The softvol control "AirPlay Duck" that shairport's output PCM uses is created and OWNED by us: the kernel only
  // lets the creator of a (user) control write to it while the creator's handle is open, and a control created by
  // shairport itself (when it opens its PCM) was therefore read-only for lostos. When the control already exists,
  // shairport's softvol attaches to it instead of creating its own.
  bool open_duck() {
    if (!ctl) {
      if (snd_ctl_open(&ctl, "hw:0", 0) < 0) {
        ctl = nullptr;
        return false;
      }
    }
    if (duck_ready) return true;
    for (const char *name : kDuckCtls) {
      snd_ctl_elem_id_t *id;
      snd_ctl_elem_id_alloca(&id);
      snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
      snd_ctl_elem_id_set_name(id, name);
      const int rc = snd_ctl_elem_add_integer(ctl, id, 2, 0, 255, 1);
      if (rc < 0 && rc != -EBUSY) LOGW("airplay: cannot create control '%s': %s", name, snd_strerror(rc));
      if (rc == -EBUSY) LOGW("airplay: control '%s' already exists (created by another process), ducking may not work", name);
    }
    duck_ready = true;
    return true;
  }

  void write_duck(float g) {
    if (!open_duck()) return;
    const long max = 255;
    const float db = g <= 0.001f ? -kDuckRangeDb : 20.f * std::log10(g);
    long raw = std::lround(max * (1.f + db / kDuckRangeDb));
    raw = raw < 0 ? 0 : raw > max ? max : raw;
    for (const char *name : kDuckCtls) {
      snd_ctl_elem_value_t *val;
      snd_ctl_elem_value_alloca(&val);
      snd_ctl_elem_value_set_interface(val, SND_CTL_ELEM_IFACE_MIXER);
      snd_ctl_elem_value_set_name(val, name);
      snd_ctl_elem_value_set_integer(val, 0, raw);
      snd_ctl_elem_value_set_integer(val, 1, raw);
      const int rc = snd_ctl_elem_write(ctl, val);
      if (rc < 0 && !duck_warned) {
        duck_warned = true;
        LOGW("airplay: writing '%s': %s", name, snd_strerror(rc));
      }
    }
  }
  void duck_tick() {
    const float step = duck_target < duck_cur ? kDuckDownStep : kDuckUpStep;
    if (std::fabs(duck_target - duck_cur) <= step) {
      duck_cur = duck_target;
      loop.cancel_timer(duck_timer);
      duck_timer = 0;
    } else {
      duck_cur += duck_target > duck_cur ? step : -step;
    }
    write_duck(duck_cur);
  }

  void open_spotify_fifo() {
    mkfifo(kSpotifyFifo, 0666);
    chmod(kSpotifyFifo, 0666);
    sfd = ::open(kSpotifyFifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (sfd < 0) return;
    loop.add_fd(sfd, EPOLLIN, [this](uint32_t) {
      char tmp[512];
      for (;;) {
        ssize_t n = ::read(sfd, tmp, sizeof tmp);
        if (n <= 0) break;
        sbuf.append(tmp, size_t(n));
      }
      size_t nl;
      while ((nl = sbuf.find('\n')) != std::string::npos) {
        handle_spotify(sbuf.substr(0, nl));
        sbuf.erase(0, nl + 1);
      }
      if (sbuf.size() > 1024) sbuf.clear();
    });
  }

  void handle_spotify(const std::string &line) {
    LOGI("spotify: %s", line.c_str());
    owner.spotify_known_ = true;
    if (line.rfind("now ", 0) == 0) {
      if (owner.on_spotify_now) owner.on_spotify_now(line.substr(4));
      return;
    }
    if (line == "loading") {
      if (owner.on_spotify_loading) owner.on_spotify_loading();
      return;
    }
    if (line.rfind("pos ", 0) == 0) {   // the playback position of the track in ms (playing / paused / seeked)
      if (owner.on_spotify_pos) owner.on_spotify_pos(std::strtoul(line.c_str() + 4, nullptr, 10));
      return;
    }
    bool changed = false;
    if (line == "playing" || line == "paused" || line == "stopped" || line == "session_disconnected") {
      const bool p = line == "playing";
      changed = owner.spotify_playing_ != p;
      owner.spotify_playing_ = p;
    } else if (line.rfind("volume ", 0) == 0) {
      const float v = std::strtof(line.c_str() + 7, nullptr) / 65535.f;
      if (owner.on_volume) owner.on_volume(v < 0.f ? 0.f : v > 1.f ? 1.f : v);
    }
    if (changed && owner.on_state) owner.on_state();
    if (changed && line == "playing" && owner.on_spotify_start) owner.on_spotify_start();
    if (changed && line != "playing" && owner.on_spotify_stop) owner.on_spotify_stop();
    if (line == "track" && owner.on_spotify_track) owner.on_spotify_track();
  }

  void close_pipe() {
    if (fd < 0) return;
    loop.del_fd(fd);
    close(fd);
    fd = -1;
  }

  void schedule_open() {
    if (retry) return;
    retry = loop.add_timer(kReopenMs, [this] {
      retry = 0;
      open_pipe();
    });
  }

  void open_pipe() {
    fd = ::open(kMetaPipe, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return schedule_open();  // shairport not started (yet)
    loop.add_fd(fd, EPOLLIN, [this](uint32_t ev) { on_readable(ev); });
  }

  void on_readable(uint32_t ev) {
    char tmp[2048];
    for (;;) {
      ssize_t n = ::read(fd, tmp, sizeof tmp);
      if (n > 0) {
        buf.append(tmp, size_t(n));
        if (buf.size() > kMaxItemBuf) buf.erase(0, buf.size() - kMaxItemBuf / 2);
        continue;
      }
      break;
    }
    parse();
    if (ev & (EPOLLHUP | EPOLLERR)) {  // shairport went away: reopen the pipe when it is back
      close_pipe();
      set_state(false, false);
      schedule_open();
    }
  }

  void set_state(bool session, bool playing) {
    if (owner.session_ == session && owner.playing_ == playing) return;
    if (playing && !owner.playing_ && duck_target < 1.f) {  // the stream (and its softvol control) has just appeared
      if (duck_rewrite) loop.cancel_timer(duck_rewrite);
      duck_rewrite = loop.add_timer(1500, [this] {
        duck_rewrite = 0;
        write_duck(duck_cur);
      });
    }
    owner.session_ = session;
    owner.playing_ = playing;
    LOGI("airplay: session %d playing %d", session, playing);
    if (owner.on_track) {  // the "now playing" text exists only while the sender actually plays
      if (playing) announce_track();
      else owner.on_track("");
    }
    if (owner.on_state) owner.on_state();
  }

  // Items look like <item><type>73736e63</type><code>70766f6c</code><length>N</length> then <data encoding="base64">..</data></item>
  void parse() {
    for (;;) {
      size_t s = buf.find("<item>");
      if (s == std::string::npos) {
        buf.clear();
        return;
      }
      size_t e = buf.find("</item>", s);
      if (e == std::string::npos) {
        buf.erase(0, s);
        return;
      }
      e += 7;
      handle_item(buf.substr(s, e - s));
      buf.erase(0, e);
    }
  }

  std::string t_title, t_artist, t_album;
  void announce_track() {
    if (!owner.playing_) return;
    std::string s = t_artist.empty() ? t_title : t_artist + " - " + t_title;
    if (!t_album.empty() && !s.empty()) s += " (" + t_album + ")";
    if (owner.on_track) owner.on_track(s);
  }

  void handle_item(const std::string &it) {
    if (it.find("<type>636f7265</type>") != std::string::npos) {  // "core": the track's tags (title, artist, album)
      const size_t c = it.find("<code>");
      const size_t d = it.find("<data encoding=\"base64\">");
      if (c == std::string::npos || it.size() < c + 14 || d == std::string::npos) return;
      const std::string code = it.substr(c + 6, 8);
      const size_t from = d + 24, to = it.find("</data>", from);
      if (to == std::string::npos) return;
      const std::string v = b64decode(it.data() + from, to - from);
      if (code == "6d696e6d") t_title = v;            // minm
      else if (code == "61736172") t_artist = v;      // asar
      else if (code == "6173616c") t_album = v;       // asal
      else return;
      announce_track();
      return;
    }
    if (it.find("<type>73736e63</type>") == std::string::npos) return;  // only "ssnc" (shairport's own) items
    const size_t c = it.find("<code>");
    if (c == std::string::npos || it.size() < c + 14) return;
    const std::string code = it.substr(c + 6, 8);  // hex of the four character code
    std::string data;
    const size_t d = it.find("<data encoding=\"base64\">");
    if (d != std::string::npos) {
      const size_t from = d + 24, to = it.find("</data>", from);
      if (to != std::string::npos) data = b64decode(it.data() + from, to - from);
    }
    // Any event from shairport proves that a session exists, even when the start of the session was missed
    // (lostos restarted in the middle of it): the codes below only refine "playing".
    bool session = true, playing = owner.playing_;
    if (code == "61656e64") {  // aend: the sender went away
      session = false;
      playing = false;
      t_title.clear(); t_artist.clear(); t_album.clear();
      unlink(kAirCover);
      if (owner.on_track) owner.on_track("");
    } else if (code == "70656e64" || code == "70666c73") {  // pend / pfls: playback ended, or flushed (paused / seek)
      playing = false;
    } else if (code == "70626567" || code == "7072736d" || code == "70726772") {  // pbeg / prsm / prgr: playing
      playing = true;
      if (code == "70726772" && owner.on_progress) {   // prgr: "start/current/end" in RTP time stamps of 44.1 kHz
        unsigned long long a = 0, b = 0, c = 0;
        if (sscanf(data.c_str(), "%llu/%llu/%llu", &a, &b, &c) == 3 && c > a && b >= a)
          owner.on_progress(uint32_t((b - a) * 1000 / 44100), uint32_t((c - a) * 1000 / 44100));
      }
    } else if (code == "50494354") {  // PICT: the cover of the track, a JPEG / PNG
      if (!data.empty() && data.size() < (2u << 20)) {
        const std::string tmp = std::string(kAirCover) + ".tmp";
        if (FILE *f = fopen(tmp.c_str(), "wb")) {
          fwrite(data.data(), 1, data.size(), f);
          fclose(f);
          rename(tmp.c_str(), kAirCover);
          LOGI("airplay: cover %zu bytes", data.size());
          if (owner.on_cover) owner.on_cover();
        }
      }
    } else if (code == "70766f6c") {  // pvol: "airplay_volume,volume,lowest,highest"
      const float db = std::strtof(data.c_str(), nullptr);
      LOGI("airplay: volume %.1f dB", db);
      if (owner.on_volume) owner.on_volume(slider_to_volume(db));
    } else {
      LOGD("airplay: item %s", code.c_str());
    }
    set_state(session, playing);
  }
};
AirPlayLink::AirPlayLink(Loop &loop, SystemPlugins &plugins) : impl_(new Impl(loop, *this, plugins)) {}
AirPlayLink::~AirPlayLink() { delete impl_; }

void AirPlayLink::start() {
  impl_->plugins.on_stopped = [this](const std::string &id) {
    if (id != kSpotifyPluginId) return;   // AirPlay: shairport's pipe closes, that is noticed by itself
    impl_->handle_spotify("stopped");
    spotify_known_ = false;               // no Spotify player any more: "play" must not wait for it
  };
  impl_->open_spotify_fifo();
  {  // lostos (re)started while Spotify was already playing: librespot's hook left its last state behind
    std::ifstream sf("/tmp/spotify-state");
    std::string st;
    if (sf && std::getline(sf, st)) {
      spotify_known_ = true;
      if (st == "playing") spotify_playing_ = true;
    }
    std::ifstream nf("/tmp/spotify-now");   // "Artist - Title<TAB>cover" of the track that plays
    std::string now;
    if (nf && std::getline(nf, now) && !now.empty() && on_spotify_now) on_spotify_now(now);
  }
  if (!installed()) {
    LOGI("airplay: the AirPlay plugin is not installed, AirPlay integration off");
    return;
  }
  impl_->open_pipe();
  impl_->write_duck(1.0f);  // creates the duck control before shairport opens its PCM
}

bool AirPlayLink::installed() const { return impl_->plugins.installed(kPluginId); }

bool AirPlayLink::send(const std::string &cmd) {
  // Not gated on session_: shairport answers only while a sender is connected, and a missed start event must not
  // make the buttons and the assistant silently useless.
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (s < 0) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kCmdPort);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ssize_t n = sendto(s, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&a), sizeof a);
  close(s);
  LOGI("airplay: command '%s' %s", cmd.c_str(), n < 0 ? "failed" : "sent");
  return n >= 0;
}

void AirPlayLink::set_duck(float gain) {
  auto &d = *impl_;
  d.duck_target = gain < 0.f ? 0.f : gain > 1.f ? 1.f : gain;
  if (d.duck_target == d.duck_cur && !d.duck_timer) return;
  if (!d.duck_timer) d.duck_timer = d.loop.add_timer(20, [&d] { d.duck_tick(); }, 20);
}

// Off stops the receiver only: the clock and the announcement stay (the speaker stays visible to the phones), Spotify goes on.
bool AirPlayLink::power(bool on) {
  if (!impl_->plugins.set_running(kPluginId, on, true)) return false;
  LOGI("airplay: receiver %s requested", on ? "start" : "stop");
  return true;
}

}  // namespace cs
