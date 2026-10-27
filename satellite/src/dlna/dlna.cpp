#include "dlna/dlna.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

#include "core/log.h"

namespace cs {
namespace {

constexpr const char *kSsdpGroup = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
constexpr const char *kServer = "Linux/4.9 UPnP/1.0 lostos/1.0";
constexpr const char *kDevType = "urn:schemas-upnp-org:device:MediaRenderer:1";
constexpr const char *kAvt = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char *kRcs = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char *kCm = "urn:schemas-upnp-org:service:ConnectionManager:1";

std::string xesc(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------- service descriptions
struct Arg {
  const char *name;
  bool in;
  const char *var;
};
struct Act {
  const char *name;
  std::vector<Arg> args;
};
struct Var {
  const char *name;
  const char *type;
  bool events;
  std::vector<const char *> allowed;
  const char *range;  // "min,max,step" or nullptr
};

std::string scpd(const std::vector<Act> &acts, const std::vector<Var> &vars) {
  std::string o =
      "<?xml version=\"1.0\"?>\n<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
      "<actionList>";
  for (const auto &a : acts) {
    o += std::string("<action><name>") + a.name + "</name>";
    if (!a.args.empty()) {
      o += "<argumentList>";
      for (const auto &g : a.args)
        o += std::string("<argument><name>") + g.name + "</name><direction>" + (g.in ? "in" : "out") +
             "</direction><relatedStateVariable>" + g.var + "</relatedStateVariable></argument>";
      o += "</argumentList>";
    }
    o += "</action>";
  }
  o += "</actionList><serviceStateTable>";
  for (const auto &v : vars) {
    o += std::string("<stateVariable sendEvents=\"") + (v.events ? "yes" : "no") + "\"><name>" + v.name + "</name><dataType>" + v.type +
         "</dataType>";
    if (!v.allowed.empty()) {
      o += "<allowedValueList>";
      for (const char *a : v.allowed) o += std::string("<allowedValue>") + a + "</allowedValue>";
      o += "</allowedValueList>";
    }
    if (v.range) {
      int mn = 0, mx = 0, st = 1;
      sscanf(v.range, "%d,%d,%d", &mn, &mx, &st);
      o += "<allowedValueRange><minimum>" + std::to_string(mn) + "</minimum><maximum>" + std::to_string(mx) + "</maximum><step>" +
           std::to_string(st) + "</step></allowedValueRange>";
    }
    o += "</stateVariable>";
  }
  o += "</serviceStateTable></scpd>";
  return o;
}

const std::string &avt_scpd() {
  static const std::string s = scpd(
      {
          {"SetAVTransportURI", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"CurrentURI", true, "AVTransportURI"}, {"CurrentURIMetaData", true, "AVTransportURIMetaData"}}},
          {"SetNextAVTransportURI", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"NextURI", true, "NextAVTransportURI"}, {"NextURIMetaData", true, "NextAVTransportURIMetaData"}}},
          {"GetMediaInfo", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"NrTracks", false, "NumberOfTracks"}, {"MediaDuration", false, "CurrentMediaDuration"}, {"CurrentURI", false, "AVTransportURI"}, {"CurrentURIMetaData", false, "AVTransportURIMetaData"}, {"NextURI", false, "NextAVTransportURI"}, {"NextURIMetaData", false, "NextAVTransportURIMetaData"}, {"PlayMedium", false, "PlaybackStorageMedium"}, {"RecordMedium", false, "RecordStorageMedium"}, {"WriteStatus", false, "RecordMediumWriteStatus"}}},
          {"GetTransportInfo", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"CurrentTransportState", false, "TransportState"}, {"CurrentTransportStatus", false, "TransportStatus"}, {"CurrentSpeed", false, "TransportPlaySpeed"}}},
          {"GetPositionInfo", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Track", false, "CurrentTrack"}, {"TrackDuration", false, "CurrentTrackDuration"}, {"TrackMetaData", false, "CurrentTrackMetaData"}, {"TrackURI", false, "CurrentTrackURI"}, {"RelTime", false, "RelativeTimePosition"}, {"AbsTime", false, "AbsoluteTimePosition"}, {"RelCount", false, "RelativeCounterPosition"}, {"AbsCount", false, "AbsoluteCounterPosition"}}},
          {"GetDeviceCapabilities", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"PlayMedia", false, "PossiblePlaybackStorageMedia"}, {"RecMedia", false, "PossibleRecordStorageMedia"}, {"RecQualityModes", false, "PossibleRecordQualityModes"}}},
          {"GetTransportSettings", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"PlayMode", false, "CurrentPlayMode"}, {"RecQualityMode", false, "CurrentRecordQualityMode"}}},
          {"Stop", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}}},
          {"Play", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Speed", true, "TransportPlaySpeed"}}},
          {"Pause", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}}},
          {"Seek", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Unit", true, "A_ARG_TYPE_SeekMode"}, {"Target", true, "A_ARG_TYPE_SeekTarget"}}},
          {"Next", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}}},
          {"Previous", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}}},
          {"GetCurrentTransportActions", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Actions", false, "CurrentTransportActions"}}},
      },
      {
          {"TransportState", "string", false, {"STOPPED", "PAUSED_PLAYBACK", "PLAYING", "TRANSITIONING", "NO_MEDIA_PRESENT"}, nullptr},
          {"TransportStatus", "string", false, {"OK", "ERROR_OCCURRED"}, nullptr},
          {"PlaybackStorageMedium", "string", false, {}, nullptr},
          {"RecordStorageMedium", "string", false, {}, nullptr},
          {"PossiblePlaybackStorageMedia", "string", false, {}, nullptr},
          {"PossibleRecordStorageMedia", "string", false, {}, nullptr},
          {"CurrentPlayMode", "string", false, {"NORMAL"}, nullptr},
          {"TransportPlaySpeed", "string", false, {"1"}, nullptr},
          {"RecordMediumWriteStatus", "string", false, {}, nullptr},
          {"CurrentRecordQualityMode", "string", false, {}, nullptr},
          {"PossibleRecordQualityModes", "string", false, {}, nullptr},
          {"NumberOfTracks", "ui4", false, {}, "0,1,1"},
          {"CurrentTrack", "ui4", false, {}, "0,1,1"},
          {"CurrentTrackDuration", "string", false, {}, nullptr},
          {"CurrentMediaDuration", "string", false, {}, nullptr},
          {"CurrentTrackMetaData", "string", false, {}, nullptr},
          {"CurrentTrackURI", "string", false, {}, nullptr},
          {"AVTransportURI", "string", false, {}, nullptr},
          {"AVTransportURIMetaData", "string", false, {}, nullptr},
          {"NextAVTransportURI", "string", false, {}, nullptr},
          {"NextAVTransportURIMetaData", "string", false, {}, nullptr},
          {"RelativeTimePosition", "string", false, {}, nullptr},
          {"AbsoluteTimePosition", "string", false, {}, nullptr},
          {"RelativeCounterPosition", "i4", false, {}, nullptr},
          {"AbsoluteCounterPosition", "ui4", false, {}, nullptr},
          {"CurrentTransportActions", "string", false, {}, nullptr},
          {"LastChange", "string", true, {}, nullptr},
          {"A_ARG_TYPE_SeekMode", "string", false, {"REL_TIME", "TRACK_NR"}, nullptr},
          {"A_ARG_TYPE_SeekTarget", "string", false, {}, nullptr},
          {"A_ARG_TYPE_InstanceID", "ui4", false, {}, nullptr},
      });
  return s;
}

const std::string &rcs_scpd() {
  static const std::string s = scpd(
      {
          {"ListPresets", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"CurrentPresetNameList", false, "PresetNameList"}}},
          {"SelectPreset", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"PresetName", true, "A_ARG_TYPE_PresetName"}}},
          {"GetMute", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Channel", true, "A_ARG_TYPE_Channel"}, {"CurrentMute", false, "Mute"}}},
          {"SetMute", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Channel", true, "A_ARG_TYPE_Channel"}, {"DesiredMute", true, "Mute"}}},
          {"GetVolume", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Channel", true, "A_ARG_TYPE_Channel"}, {"CurrentVolume", false, "Volume"}}},
          {"SetVolume", {{"InstanceID", true, "A_ARG_TYPE_InstanceID"}, {"Channel", true, "A_ARG_TYPE_Channel"}, {"DesiredVolume", true, "Volume"}}},
      },
      {
          {"LastChange", "string", true, {}, nullptr},
          {"PresetNameList", "string", false, {}, nullptr},
          {"Mute", "boolean", false, {}, nullptr},
          {"Volume", "ui2", false, {}, "0,100,1"},
          {"A_ARG_TYPE_Channel", "string", false, {"Master"}, nullptr},
          {"A_ARG_TYPE_InstanceID", "ui4", false, {}, nullptr},
          {"A_ARG_TYPE_PresetName", "string", false, {"FactoryDefaults"}, nullptr},
      });
  return s;
}

const std::string &cm_scpd() {
  static const std::string s = scpd(
      {
          {"GetProtocolInfo", {{"Source", false, "SourceProtocolInfo"}, {"Sink", false, "SinkProtocolInfo"}}},
          {"GetCurrentConnectionIDs", {{"ConnectionIDs", false, "CurrentConnectionIDs"}}},
          {"GetCurrentConnectionInfo", {{"ConnectionID", true, "A_ARG_TYPE_ConnectionID"}, {"RcsID", false, "A_ARG_TYPE_RcsID"}, {"AVTransportID", false, "A_ARG_TYPE_AVTransportID"}, {"ProtocolInfo", false, "A_ARG_TYPE_ProtocolInfo"}, {"PeerConnectionManager", false, "A_ARG_TYPE_ConnectionManager"}, {"PeerConnectionID", false, "A_ARG_TYPE_ConnectionID"}, {"Direction", false, "A_ARG_TYPE_Direction"}, {"Status", false, "A_ARG_TYPE_ConnectionStatus"}}},
      },
      {
          {"SourceProtocolInfo", "string", true, {}, nullptr},
          {"SinkProtocolInfo", "string", true, {}, nullptr},
          {"CurrentConnectionIDs", "string", true, {}, nullptr},
          {"A_ARG_TYPE_ConnectionStatus", "string", false, {"OK", "ContentFormatMismatch", "InsufficientBandwidth", "UnreliableChannel", "Unknown"}, nullptr},
          {"A_ARG_TYPE_ConnectionManager", "string", false, {}, nullptr},
          {"A_ARG_TYPE_Direction", "string", false, {"Input", "Output"}, nullptr},
          {"A_ARG_TYPE_ProtocolInfo", "string", false, {}, nullptr},
          {"A_ARG_TYPE_ConnectionID", "i4", false, {}, nullptr},
          {"A_ARG_TYPE_AVTransportID", "i4", false, {}, nullptr},
          {"A_ARG_TYPE_RcsID", "i4", false, {}, nullptr},
      });
  return s;
}

const char *kSinkProtocols =
    "http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*,http-get:*:audio/flac:*,http-get:*:audio/x-wav:*,http-get:*:audio/L16:*";

struct Request {
  std::string method, path, body;
  std::map<std::string, std::string> h;  // lowercased names
};

}  // namespace

struct DlnaRenderer::Impl {
  Loop &loop;
  DlnaRenderer &owner;
  std::string name, uuid;
  uint16_t port = 0;
  std::string ip;
  int ssdp_fd = -1, http_fd = -1;
  uint64_t notify_timer = 0;

  struct Conn {
    int fd = -1;
    std::string in, out;
    size_t off = 0;
  };
  std::map<int, std::shared_ptr<Conn>> conns;

  struct Sub {
    std::string sid, svc, host, path;
    uint16_t port = 80;
    uint32_t seq = 0;
    int64_t expires = 0;
  };
  std::vector<Sub> subs;
  uint64_t sid_counter = 0;

  uint64_t change_timer = 0;
  std::string last_sig;

  Impl(Loop &l, DlnaRenderer &o) : loop(l), owner(o) {}

  // ------------------------------------------------------------ helpers
  std::string local_ip() const {
    std::string out;
    ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs *p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || strcmp(p->ifa_name, "wlan0") != 0) continue;
      char buf[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr, buf, sizeof buf);
      out = buf;
      break;
    }
    freeifaddrs(ifa);
    return out;
  }

  std::string location() const { return "http://" + ip + ":" + std::to_string(port) + "/desc.xml"; }

  std::vector<std::pair<std::string, std::string>> targets() const {  // NT/ST and USN
    const std::string u = "uuid:" + uuid;
    return {{"upnp:rootdevice", u + "::upnp:rootdevice"},
            {u, u},
            {kDevType, u + "::" + kDevType},
            {kAvt, u + "::" + kAvt},
            {kRcs, u + "::" + kRcs},
            {kCm, u + "::" + kCm}};
  }

  // ------------------------------------------------------------ SSDP
  void ssdp_close() {
    if (ssdp_fd >= 0) {
      loop.del_fd(ssdp_fd);
      close(ssdp_fd);
      ssdp_fd = -1;
    }
  }

  bool ssdp_open() {
    ssdp_close();
    const std::string a = local_ip();
    if (a.empty()) return false;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(kSsdpPort);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) {
      LOGW("dlna: ssdp bind: %s", strerror(errno));
      close(fd);
      return false;
    }
    ip_mreq mr{};
    inet_pton(AF_INET, kSsdpGroup, &mr.imr_multiaddr);
    inet_pton(AF_INET, a.c_str(), &mr.imr_interface);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) != 0) {
      LOGW("dlna: ssdp join: %s", strerror(errno));
      close(fd);
      return false;
    }
    in_addr ifa{};
    inet_pton(AF_INET, a.c_str(), &ifa);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    int ttl = 4, loopback = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof loopback);
    ssdp_fd = fd;
    ip = a;
    loop.add_fd(fd, EPOLLIN, [this](uint32_t) { ssdp_readable(); });
    LOGI("dlna: ssdp on %s, %s", ip.c_str(), location().c_str());
    return true;
  }

  void send_to(const std::string &data, const sockaddr_in &to) {
    if (ssdp_fd >= 0) (void)!sendto(ssdp_fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to);
  }

  void notify(const char *nts) {
    if (ssdp_fd < 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kSsdpPort);
    inet_pton(AF_INET, kSsdpGroup, &to.sin_addr);
    for (const auto &t : targets()) {
      std::string m = std::string("NOTIFY * HTTP/1.1\r\nHOST: ") + kSsdpGroup + ":1900\r\n";
      if (strcmp(nts, "ssdp:alive") == 0) m += "CACHE-CONTROL: max-age=1800\r\nLOCATION: " + location() + "\r\n";
      m += "NT: " + t.first + "\r\nNTS: " + nts + "\r\nSERVER: " + kServer + "\r\nUSN: " + t.second + "\r\n\r\n";
      send_to(m, to);
    }
  }

  void ssdp_readable() {
    char buf[2048];
    for (;;) {
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t n = recvfrom(ssdp_fd, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fl);
      if (n <= 0) break;
      buf[n] = 0;
      std::string msg(buf, size_t(n));
      if (msg.rfind("M-SEARCH", 0) != 0) continue;
      std::string st;
      int mx = 2;
      size_t pos = msg.find("\r\n");
      while (pos != std::string::npos && pos + 2 < msg.size()) {
        size_t e = msg.find("\r\n", pos + 2);
        std::string line = msg.substr(pos + 2, e == std::string::npos ? std::string::npos : e - pos - 2);
        pos = e;
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        const std::string k = lower(line.substr(0, c)), v = trim(line.substr(c + 1));
        if (k == "st") st = v;
        if (k == "mx") mx = std::max(1, std::min(5, atoi(v.c_str())));
      }
      for (const auto &t : targets()) {
        if (st != "ssdp:all" && st != t.first) continue;
        const int64_t delay = rand() % (mx * 700);
        loop.add_timer(delay + 1, [this, t, from] {
          if (ssdp_fd < 0) return;
          std::string r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: " + location() + "\r\nSERVER: " + kServer +
                          "\r\nST: " + t.first + "\r\nUSN: " + t.second + "\r\n\r\n";
          send_to(r, from);
        });
      }
    }
  }

  // ------------------------------------------------------------ HTTP server
  bool http_open() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0 || listen(fd, 8) != 0) {
      LOGW("dlna: http port %u: %s", unsigned(port), strerror(errno));
      close(fd);
      return false;
    }
    http_fd = fd;
    loop.add_fd(fd, EPOLLIN, [this](uint32_t) {
      for (;;) {
        int c = accept4(http_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) break;
        auto conn = std::make_shared<Conn>();
        conn->fd = c;
        conns[c] = conn;
        loop.add_fd(c, EPOLLIN, [this, conn](uint32_t ev) { conn_event(conn, ev); });
        loop.add_timer(15000, [this, conn] {   // a stuck client does not stay
          if (conns.count(conn->fd) && conns[conn->fd] == conn) drop(conn);
        });
      }
    });
    return true;
  }

  void drop(const std::shared_ptr<Conn> &c) {
    if (c->fd < 0) return;
    loop.del_fd(c->fd);
    close(c->fd);
    conns.erase(c->fd);
    c->fd = -1;
  }

  void conn_event(const std::shared_ptr<Conn> &c, uint32_t ev) {
    if (c->fd < 0) return;
    if (ev & (EPOLLERR | EPOLLHUP)) return drop(c);
    if (ev & EPOLLIN) {
      char buf[4096];
      for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n > 0) {
          c->in.append(buf, size_t(n));
          if (c->in.size() > 65536) return drop(c);
          continue;
        }
        if (n == 0) return drop(c);
        break;
      }
      if (c->out.empty()) {
        Request rq;
        if (parse(c->in, rq)) {
          c->out = handle(rq);
          c->in.clear();
        }
      }
    }
    if (!c->out.empty() || (ev & EPOLLOUT)) {
      while (c->off < c->out.size()) {
        ssize_t n = send(c->fd, c->out.data() + c->off, c->out.size() - c->off, MSG_NOSIGNAL);
        if (n > 0) {
          c->off += size_t(n);
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          loop.mod_fd(c->fd, EPOLLIN | EPOLLOUT);
          return;
        }
        return drop(c);
      }
      if (!c->out.empty()) drop(c);   // one answer per connection
    }
  }

  bool parse(const std::string &in, Request &rq) const {
    const size_t he = in.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    size_t pos = in.find("\r\n");
    const std::string first = in.substr(0, pos);
    const size_t s1 = first.find(' '), s2 = first.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    rq.method = first.substr(0, s1);
    rq.path = first.substr(s1 + 1, s2 - s1 - 1);
    while (pos != std::string::npos && pos < he) {
      size_t e = in.find("\r\n", pos + 2);
      if (e == std::string::npos || e > he) e = he;
      const std::string line = in.substr(pos + 2, e - pos - 2);
      const size_t c = line.find(':');
      if (c != std::string::npos) rq.h[lower(line.substr(0, c))] = trim(line.substr(c + 1));
      pos = e == he ? std::string::npos : e;
    }
    size_t len = 0;
    if (rq.h.count("content-length")) len = size_t(atoi(rq.h["content-length"].c_str()));
    if (in.size() < he + 4 + len) return false;
    rq.body = in.substr(he + 4, len);
    return true;
  }

  static std::string http(int code, const char *text, const std::string &body, const char *type = "text/xml; charset=\"utf-8\"",
                          const std::string &extra = "") {
    return "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\nCONTENT-TYPE: " + type + "\r\nCONTENT-LENGTH: " +
           std::to_string(body.size()) + "\r\nSERVER: " + kServer + "\r\nEXT:\r\nCONNECTION: close\r\n" + extra + "\r\n" + body;
  }

  // ------------------------------------------------------------ what the speaker plays
  // The cover: Home Assistant could not always fetch the picture from the CDN itself (its proxy answered 404), so the speaker keeps
  // the current cover and serves it from its own address.
  mutable std::string cover_src;
  mutable int cover_id = int(time(nullptr));   // unique across restarts: a file or Home Assistant's cache of an old run is never taken for this one
  mutable bool cover_ready = false;

  static std::string cover_path(int id) { return "/tmp/lostos-cover-" + std::to_string(id) + ".jpg"; }

  void fetch_cover(std::string url, int id) const {
    if (url.rfind("https://", 0) == 0) url.replace(0, 8, "http://");   // the speaker's wget has no TLS; these pictures are public
    const bool local = url.rfind("file://", 0) == 0;                   // AirPlay's own picture, already on the speaker
    if (local) url = url.substr(7, url.find('#') == std::string::npos ? std::string::npos : url.find('#') - 7);
    const std::string file = cover_path(id);
    pid_t pid = fork();
    if (pid == 0) {
      if (fork() == 0) {
        setsid();
        for (int fd = 3; fd < 1024; fd++) close(fd);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
        execl("/bin/sh", "sh", "-c", local ? "cp \"$2\" \"$1.tmp\" && mv \"$1.tmp\" \"$1\" || rm -f \"$1.tmp\""
                          : "wget -q -T 10 -O \"$1.tmp\" \"$2\" && mv \"$1.tmp\" \"$1\" || rm -f \"$1.tmp\"", "sh", file.c_str(),
              url.c_str(), static_cast<char *>(nullptr));
      }
      _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
    auto poll = std::make_shared<std::function<void()>>();
    auto tries = std::make_shared<int>(0);
    *poll = [this, id, poll, tries]() {
      if (id != cover_id) {   // another cover came meanwhile
        *poll = nullptr;
        return;
      }
      if (access(cover_path(id).c_str(), R_OK) == 0) {
        cover_ready = true;
        owner.changed();
        *poll = nullptr;
        return;
      }
      if (++*tries < 30) loop.add_timer(300, *poll);
      else *poll = nullptr;
    };
    loop.add_timer(300, *poll);
  }

  std::string local_cover(const std::string &remote) const {
    if (remote != cover_src) {
      if (cover_id) unlink(cover_path(cover_id).c_str());
      cover_src = remote;
      cover_ready = false;
      ++cover_id;
      unlink(cover_path(cover_id).c_str());   // a leftover with this number is not the picture that is coming
      fetch_cover(remote, cover_id);
    }
    if (!cover_ready && access(cover_path(cover_id).c_str(), R_OK) == 0) cover_ready = true;
    return cover_ready ? "http://" + ip + ":" + std::to_string(port) + "/cover/" + std::to_string(cover_id) + ".jpg" : "";
  }

  Now now() const {
    Now n = owner.get_now ? owner.get_now() : Now();
    if (!n.cover.empty()) n.cover = local_cover(n.cover);
    return n;
  }

  static const char *state_of(const Now &n) { return n.playing ? "PLAYING" : n.paused ? "PAUSED_PLAYBACK" : "NO_MEDIA_PRESENT"; }

  static std::string actions_of(const Now &n) {
    if (n.playing) return "Pause,Stop,Next,Previous";
    if (n.paused) return "Play,Stop,Next,Previous";
    return "";
  }

  static std::string didl(const Now &n) {
    if (n.source.empty()) return "";
    std::string o =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"><item id=\"0\" parentID=\"-1\" restricted=\"1\"><dc:title>" +
        xesc(n.title.empty() ? n.source : n.title) + "</dc:title>";
    if (!n.artist.empty()) o += "<upnp:artist>" + xesc(n.artist) + "</upnp:artist><dc:creator>" + xesc(n.artist) + "</dc:creator>";
    o += "<upnp:album>" + xesc(n.album.empty() ? n.source : n.album) + "</upnp:album>";
    o += "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    if (!n.cover.empty()) o += "<upnp:albumArtURI>" + xesc(n.cover) + "</upnp:albumArtURI>";
    o += "</item></DIDL-Lite>";
    return o;
  }

  static std::string track_uri(const Now &n) { return n.source.empty() ? "" : "lostos://" + lower(n.source); }

  int volume() const { return owner.get_volume ? int(owner.get_volume() * 100.f + 0.5f) : 0; }
  bool muted() const { return owner.get_mute ? owner.get_mute() : false; }

  std::string last_change(const std::string &svc) const {
    if (svc == "avt") {
      const Now n = now();
      auto v = [](const char *k, const std::string &val) { return std::string("<") + k + " val=\"" + xesc(val) + "\"/>"; };
      return "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\">" + v("TransportState", state_of(n)) +
             v("TransportStatus", "OK") + v("PlaybackStorageMedium", "NETWORK") + v("CurrentPlayMode", "NORMAL") +
             v("TransportPlaySpeed", "1") + v("NumberOfTracks", n.source.empty() ? "0" : "1") + v("CurrentTrack", n.source.empty() ? "0" : "1") +
             v("CurrentTrackDuration", "00:00:00") + v("CurrentMediaDuration", "00:00:00") + v("CurrentTrackMetaData", didl(n)) +
             v("CurrentTrackURI", track_uri(n)) + v("AVTransportURI", track_uri(n)) + v("AVTransportURIMetaData", didl(n)) +
             v("CurrentTransportActions", actions_of(n)) + "</InstanceID></Event>";
    }
    if (svc == "rcs") {
      return "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\"><Volume channel=\"Master\" val=\"" +
             std::to_string(volume()) + "\"/><Mute channel=\"Master\" val=\"" + (muted() ? "1" : "0") + "\"/></InstanceID></Event>";
    }
    return "";
  }

  std::string propertyset(const std::string &svc) const {
    std::string body = "<?xml version=\"1.0\"?>\n<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">";
    if (svc == "cm") {
      body += "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property><e:property><SinkProtocolInfo>" + xesc(kSinkProtocols) +
              "</SinkProtocolInfo></e:property><e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property>";
    } else {
      body += "<e:property><LastChange>" + xesc(last_change(svc)) + "</LastChange></e:property>";
    }
    return body + "</e:propertyset>";
  }

  // ------------------------------------------------------------ routes
  std::string handle(const Request &rq) {
    const std::string &p = rq.path;
    if (rq.method == "GET") {
      if (p == "/desc.xml") return http(200, "OK", description());
      if (p == "/avt.xml") return http(200, "OK", avt_scpd());
      if (p == "/rcs.xml") return http(200, "OK", rcs_scpd());
      if (p == "/cm.xml") return http(200, "OK", cm_scpd());
      if (p == "/cover/ver") return http(200, "OK", cover_ready ? std::to_string(cover_id) : "0", "text/plain");   // for the voice server's panel: which picture is ready
      if (p.rfind("/cover/", 0) == 0) {   // the current cover (a picture of the track that plays)
        const int id = p == "/cover/current.jpg" ? cover_id : atoi(p.c_str() + 7);
        std::string data;
        if (id > 0 && id == cover_id) {
          if (FILE *f = fopen(cover_path(id).c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && data.size() < 400000) data.append(buf, n);
            fclose(f);
          }
        }
        return data.empty() ? http(404, "Not Found", "", "text/plain") : http(200, "OK", data, "image/jpeg");
      }
      return http(404, "Not Found", "", "text/plain");
    }
    if (rq.method == "POST" && p.rfind("/ctl/", 0) == 0) return soap(p.substr(5), rq);
    if (rq.method == "SUBSCRIBE" && p.rfind("/evt/", 0) == 0) return subscribe(p.substr(5), rq);
    if (rq.method == "UNSUBSCRIBE" && p.rfind("/evt/", 0) == 0) {
      const std::string sid = rq.h.count("sid") ? rq.h.at("sid") : "";
      subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub &s) { return s.sid == sid; }), subs.end());
      return http(200, "OK", "", "text/plain");
    }
    return http(405, "Method Not Allowed", "", "text/plain");
  }

  std::string description() const {
    auto svc = [](const char *type, const char *id, const char *scpd_url, const char *ctl, const char *evt) {
      return std::string("<service><serviceType>") + type + "</serviceType><serviceId>urn:upnp-org:serviceId:" + id +
             "</serviceId><SCPDURL>" + scpd_url + "</SCPDURL><controlURL>" + ctl + "</controlURL><eventSubURL>" + evt + "</eventSubURL></service>";
    };
    return "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
           "<specVersion><major>1</major><minor>0</minor></specVersion><device><dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
           "<deviceType>" + std::string(kDevType) + "</deviceType><friendlyName>" + xesc(name) +
           "</friendlyName><manufacturer>Yandex</manufacturer><modelName>Station Mini 3</modelName>"
           "<modelDescription>Smart speaker with lostos</modelDescription><modelNumber>1</modelNumber><UDN>uuid:" + uuid + "</UDN>"
           "<serviceList>" + svc(kAvt, "AVTransport", "/avt.xml", "/ctl/avt", "/evt/avt") + svc(kRcs, "RenderingControl", "/rcs.xml", "/ctl/rcs", "/evt/rcs") +
           svc(kCm, "ConnectionManager", "/cm.xml", "/ctl/cm", "/evt/cm") + "</serviceList></device></root>";
  }

  // ------------------------------------------------------------ SOAP
  static std::string arg(const std::string &body, const char *tag) {
    const std::string open = std::string("<") + tag + ">";
    size_t a = body.find(open);
    if (a == std::string::npos) return "";
    a += open.size();
    const size_t b = body.find(std::string("</") + tag + ">", a);
    return b == std::string::npos ? "" : body.substr(a, b - a);
  }

  static std::string reply(const char *urn, const std::string &action, const std::vector<std::pair<std::string, std::string>> &out) {
    std::string o = "<?xml version=\"1.0\"?>\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + "Response xmlns:u=\"" + urn + "\">";
    for (const auto &kv : out) o += "<" + kv.first + ">" + xesc(kv.second) + "</" + kv.first + ">";
    o += "</u:" + action + "Response></s:Body></s:Envelope>";
    return http(200, "OK", o);
  }

  static std::string fault(int code, const char *text) {
    return http(500, "Internal Server Error",
                std::string("<?xml version=\"1.0\"?>\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
                            "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>") +
                    std::to_string(code) + "</errorCode><errorDescription>" + text + "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
  }

  std::string soap(const std::string &svc, const Request &rq) {
    std::string sa = rq.h.count("soapaction") ? rq.h.at("soapaction") : "";
    sa.erase(std::remove(sa.begin(), sa.end(), '"'), sa.end());
    const size_t hash = sa.find('#');
    if (hash == std::string::npos) return fault(401, "Invalid Action");
    const std::string act = sa.substr(hash + 1);
    const std::string &b = rq.body;
    LOGD("dlna: %s %s", svc.c_str(), act.c_str());
    auto act_cb = [&](const char *what) {
      if (owner.on_action) owner.on_action(what);
      return true;
    };
    if (svc == "avt") {
      const Now n = now();
      if (act == "GetTransportInfo")
        return reply(kAvt, act, {{"CurrentTransportState", state_of(n)}, {"CurrentTransportStatus", "OK"}, {"CurrentSpeed", "1"}});
      if (act == "GetPositionInfo")
        return reply(kAvt, act, {{"Track", n.source.empty() ? "0" : "1"}, {"TrackDuration", "00:00:00"}, {"TrackMetaData", didl(n)},
                                 {"TrackURI", track_uri(n)}, {"RelTime", "00:00:00"}, {"AbsTime", "00:00:00"}, {"RelCount", "0"}, {"AbsCount", "0"}});
      if (act == "GetMediaInfo")
        return reply(kAvt, act, {{"NrTracks", n.source.empty() ? "0" : "1"}, {"MediaDuration", "00:00:00"}, {"CurrentURI", track_uri(n)},
                                 {"CurrentURIMetaData", didl(n)}, {"NextURI", ""}, {"NextURIMetaData", ""}, {"PlayMedium", "NETWORK"},
                                 {"RecordMedium", "NOT_IMPLEMENTED"}, {"WriteStatus", "NOT_IMPLEMENTED"}});
      if (act == "GetDeviceCapabilities")
        return reply(kAvt, act, {{"PlayMedia", "NETWORK"}, {"RecMedia", "NOT_IMPLEMENTED"}, {"RecQualityModes", "NOT_IMPLEMENTED"}});
      if (act == "GetTransportSettings") return reply(kAvt, act, {{"PlayMode", "NORMAL"}, {"RecQualityMode", "NOT_IMPLEMENTED"}});
      if (act == "GetCurrentTransportActions") return reply(kAvt, act, {{"Actions", actions_of(n)}});
      if (act == "Play") return act_cb("play") ? reply(kAvt, act, {}) : fault(501, "Action Failed");
      if (act == "Pause") return act_cb("pause") ? reply(kAvt, act, {}) : fault(501, "Action Failed");
      if (act == "Stop") return act_cb("stop") ? reply(kAvt, act, {}) : fault(501, "Action Failed");
      if (act == "Next") return act_cb("next") ? reply(kAvt, act, {}) : fault(501, "Action Failed");
      if (act == "Previous") return act_cb("previous") ? reply(kAvt, act, {}) : fault(501, "Action Failed");
      if (act == "Seek") return fault(710, "Seek mode not supported");
      if (act == "SetAVTransportURI" || act == "SetNextAVTransportURI") return fault(716, "Resource not found");  // it only plays what its sources send
      return fault(401, "Invalid Action");
    }
    if (svc == "rcs") {
      if (act == "GetVolume") return reply(kRcs, act, {{"CurrentVolume", std::to_string(volume())}});
      if (act == "GetMute") return reply(kRcs, act, {{"CurrentMute", muted() ? "1" : "0"}});
      if (act == "SetVolume") {
        const int v = std::max(0, std::min(100, atoi(arg(b, "DesiredVolume").c_str())));
        if (owner.set_volume) owner.set_volume(float(v) / 100.f);
        return reply(kRcs, act, {});
      }
      if (act == "SetMute") {
        const std::string m = arg(b, "DesiredMute");
        if (owner.set_mute) owner.set_mute(m == "1" || lower(m) == "true");
        return reply(kRcs, act, {});
      }
      if (act == "ListPresets") return reply(kRcs, act, {{"CurrentPresetNameList", "FactoryDefaults"}});
      if (act == "SelectPreset") return reply(kRcs, act, {});
      return fault(401, "Invalid Action");
    }
    if (svc == "cm") {
      if (act == "GetProtocolInfo") return reply(kCm, act, {{"Source", ""}, {"Sink", kSinkProtocols}});
      if (act == "GetCurrentConnectionIDs") return reply(kCm, act, {{"ConnectionIDs", "0"}});
      if (act == "GetCurrentConnectionInfo")
        return reply(kCm, act, {{"RcsID", "0"}, {"AVTransportID", "0"}, {"ProtocolInfo", ""}, {"PeerConnectionManager", ""},
                                {"PeerConnectionID", "-1"}, {"Direction", "Input"}, {"Status", "OK"}});
      return fault(401, "Invalid Action");
    }
    return fault(401, "Invalid Action");
  }

  // ------------------------------------------------------------ events (GENA)
  std::string subscribe(const std::string &svc, const Request &rq) {
    if (svc != "avt" && svc != "rcs" && svc != "cm") return http(404, "Not Found", "", "text/plain");
    const int64_t now_ms = Loop::now_ms();
    int timeout = 1800;
    if (rq.h.count("timeout")) {
      const size_t d = rq.h.at("timeout").find('-');
      if (d != std::string::npos) timeout = std::max(60, std::min(3600, atoi(rq.h.at("timeout").c_str() + d + 1)));
    }
    if (rq.h.count("sid")) {  // a renewal
      for (auto &s : subs)
        if (s.sid == rq.h.at("sid")) {
          s.expires = now_ms + int64_t(timeout) * 1000;
          return http(200, "OK", "", "text/plain", "SID: " + s.sid + "\r\nTIMEOUT: Second-" + std::to_string(timeout) + "\r\n");
        }
      return http(412, "Precondition Failed", "", "text/plain");
    }
    // CALLBACK: <http://host:port/path>
    std::string cb = rq.h.count("callback") ? rq.h.at("callback") : "";
    const size_t a = cb.find("<http://");
    const size_t b = cb.find('>', a);
    if (a == std::string::npos || b == std::string::npos) return http(412, "Precondition Failed", "", "text/plain");
    std::string url = cb.substr(a + 8, b - a - 8);
    Sub s;
    s.svc = svc;
    const size_t slash = url.find('/');
    std::string hp = url.substr(0, slash);
    s.path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hp.find(':');
    s.host = hp.substr(0, colon);
    s.port = colon == std::string::npos ? 80 : uint16_t(atoi(hp.c_str() + colon + 1));
    char sid[64];
    snprintf(sid, sizeof sid, "uuid:%s-%llu", uuid.substr(0, 23).c_str(), (unsigned long long)(++sid_counter));
    s.sid = sid;
    s.expires = now_ms + int64_t(timeout) * 1000;
    if (subs.size() > 24) subs.erase(subs.begin());
    subs.push_back(s);
    const std::string id = s.sid;
    loop.add_timer(80, [this, id] {   // the first event follows the answer: the whole state
      for (auto &x : subs)
        if (x.sid == id) send_event(x);
    });
    return http(200, "OK", "", "text/plain", "SID: " + s.sid + "\r\nTIMEOUT: Second-" + std::to_string(timeout) + "\r\n");
  }

  void send_event(Sub &s) {
    const std::string body = propertyset(s.svc);
    const std::string req = "NOTIFY " + s.path + " HTTP/1.1\r\nHOST: " + s.host + ":" + std::to_string(s.port) +
                            "\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\nSID: " + s.sid +
                            "\r\nSEQ: " + std::to_string(s.seq++) + "\r\nCONTENT-LENGTH: " + std::to_string(body.size()) +
                            "\r\nCONNECTION: close\r\n\r\n" + body;
    post_async(s.host, s.port, req);
  }

  void post_async(const std::string &host, uint16_t p, const std::string &data) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(p);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
      close(fd);
      return;
    }
    const int r = connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    if (r != 0 && errno != EINPROGRESS) {
      close(fd);
      return;
    }
    struct Out {
      int fd;
      std::string data;
      size_t off = 0;
      bool sent = false;
    };
    auto o = std::make_shared<Out>();
    o->fd = fd;
    o->data = data;
    auto done = [this, o] {
      if (o->fd < 0) return;
      loop.del_fd(o->fd);
      close(o->fd);
      o->fd = -1;
    };
    loop.add_fd(fd, EPOLLOUT | EPOLLIN, [this, o, done](uint32_t ev) {
      if (o->fd < 0) return;
      if (ev & (EPOLLERR | EPOLLHUP)) return done();
      if ((ev & EPOLLIN) && o->sent) {   // the answer (or the end of it): nothing more to do
        char buf[512];
        ssize_t n = read(o->fd, buf, sizeof buf);
        if (n <= 0) return done();
        return;
      }
      if (ev & EPOLLOUT) {
        while (o->off < o->data.size()) {
          ssize_t n = send(o->fd, o->data.data() + o->off, o->data.size() - o->off, MSG_NOSIGNAL);
          if (n > 0) {
            o->off += size_t(n);
            continue;
          }
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
          return done();
        }
        o->sent = true;
        loop.mod_fd(o->fd, EPOLLIN);
      }
    });
    loop.add_timer(4000, done);
  }

  void emit_all() {
    for (auto &s : subs) send_event(s);
  }

  std::string signature() const {
    const Now n = now();
    return std::string(state_of(n)) + "|" + n.source + "|" + n.artist + "|" + n.title + "|" + n.album + "|" + n.cover + "|" +
           std::to_string(volume()) + "|" + (muted() ? "1" : "0");
  }
};

DlnaRenderer::DlnaRenderer(Loop &loop) : impl_(new Impl(loop, *this)) {}

DlnaRenderer::~DlnaRenderer() {
  impl_->notify("ssdp:byebye");
  delete impl_;
}

bool DlnaRenderer::start(const std::string &friendly_name, const std::string &uuid, uint16_t port) {
  auto &d = *impl_;
  d.name = friendly_name;
  d.uuid = uuid;
  d.port = port;
  if (!d.http_open()) return false;
  // The network may not be up yet: SSDP keeps trying; the address is watched (a new one is announced).
  d.loop.add_timer(1000, [this] {
    auto &d = *impl_;
    if (d.ssdp_fd < 0 || d.local_ip() != d.ip) {
      if (d.ssdp_open()) {
        d.notify("ssdp:alive");
        d.loop.add_timer(250, [&d] { d.notify("ssdp:alive"); });
      }
    }
  }, 10000);
  d.loop.add_timer(300000, [this] { impl_->notify("ssdp:alive"); }, 300000);   // max-age is 1800 s
  d.loop.add_timer(60000, [this] {                                              // the subscriptions that were not renewed
    auto &d = *impl_;
    const int64_t now = Loop::now_ms();
    d.subs.erase(std::remove_if(d.subs.begin(), d.subs.end(), [&](const Impl::Sub &s) { return s.expires < now; }), d.subs.end());
  }, 60000);
  LOGI("dlna: media renderer '%s' on port %u", friendly_name.c_str(), unsigned(port));
  return true;
}

void DlnaRenderer::changed() {
  auto &d = *impl_;
  if (d.change_timer) return;   // coalesce bursts
  d.change_timer = d.loop.add_timer(150, [this] {
    auto &d = *impl_;
    d.change_timer = 0;
    const std::string sig = d.signature();
    if (sig == d.last_sig) return;
    d.last_sig = sig;
    d.emit_all();
  });
}

}  // namespace cs
