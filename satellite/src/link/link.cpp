#include "link/link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "core/log.h"
#include "update/https.h"
#include "update/tls.h"

namespace cs {
namespace {

constexpr const char *kDir = "/data/lostos";
constexpr size_t kMaxOut = 1 << 20;             // what may wait to be sent before the local sockets are not read any more
constexpr size_t kMaxFrame = 4 << 20;

std::string b64(const uint8_t *d, size_t n) {
  static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    o += t[v >> 18 & 63];
    o += t[v >> 12 & 63];
    o += i + 1 < n ? t[v >> 6 & 63] : '=';
    o += i + 2 < n ? t[v & 63] : '=';
  }
  return o;
}

std::string b64_decode(const std::string &s) {
  std::string o;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (v < 0) continue;
    acc = acc << 6 | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o += char(acc >> bits & 0xff);
    }
  }
  return o;
}

std::string hex_decode(const std::string &h) {
  std::string o;
  for (size_t i = 0; i + 1 < h.size(); i += 2) o += char(strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return o;
}

// "key":"value" or "key":123 out of a flat JSON object (the server's messages are tiny and flat)
std::string jget(const std::string &j, const std::string &key) {
  size_t p = j.find("\"" + key + "\"");
  if (p == std::string::npos) return "";
  p = j.find(':', p);
  if (p == std::string::npos) return "";
  p++;
  while (p < j.size() && j[p] == ' ') p++;
  if (p < j.size() && j[p] == '"') {
    size_t e = j.find('"', p + 1);
    return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
  }
  size_t e = j.find_first_of(",}", p);
  return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::string json_str(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) >= 32) o += c;
  }
  return o + "\"";
}

bool random_bytes(uint8_t *p, size_t n) {
  FILE *r = fopen("/dev/urandom", "rb");
  if (!r) return false;
  size_t got = fread(p, 1, n, r);
  fclose(r);
  return got == n;
}

bool read_key(uint8_t key[32], const char *file = "link.key") {
  const std::string path = std::string(kDir) + "/" + file;
  if (FILE *f = fopen(path.c_str(), "rb")) {
    size_t n = fread(key, 1, 32, f);
    fclose(f);
    if (n == 32) return true;
  }
  if (!random_bytes(key, 32)) return false;
  int fd = open((path + ".new").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  bool ok = write(fd, key, 32) == 32;
  fsync(fd);
  close(fd);
  return ok && rename((path + ".new").c_str(), path.c_str()) == 0;
}

struct Forward {
  std::string name, host;
  int port = 0;
};

// forward=ha=192.168.1.5:8123  (any number of lines): hosts of the home network that the server may reach through the speaker. Only private IPv4
// addresses are accepted, so the speaker cannot be turned into a door to the internet or to itself.
bool private_ipv4(const std::string &h) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (sscanf(h.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

std::vector<Forward> read_forwards() {
  std::vector<Forward> out;
  std::ifstream f(std::string(kDir) + "/link.conf");
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("forward=", 0) != 0) continue;
    const std::string v = line.substr(8);
    const size_t eq = v.find('='), colon = v.rfind(':');
    if (eq == std::string::npos || colon == std::string::npos || colon < eq) continue;
    Forward fw{v.substr(0, eq), v.substr(eq + 1, colon - eq - 1), atoi(v.c_str() + colon + 1)};
    bool name_ok = !fw.name.empty() && fw.name.size() <= 16;
    for (char ch : fw.name) name_ok = name_ok && isalnum(static_cast<unsigned char>(ch));
    if (name_ok && fw.port > 0 && fw.port < 65536 && private_ipv4(fw.host)) out.push_back(fw);
    else LOGW("link: ignoring the forward line '%s' (a name of letters and digits, a private IPv4 address, a port)", v.c_str());
  }
  return out;
}

std::string read_server() {
  std::ifstream f(std::string(kDir) + "/link.conf");
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("server=", 0) == 0) return line.substr(7);
  }
  return "";
}

// One WebSocket frame out (client frames are masked).
void ws_frame(std::string &out, int opcode, const char *p, size_t n) {
  out += char(0x80 | opcode);
  if (n < 126) {
    out += char(0x80 | n);
  } else if (n < 65536) {
    out += char(0x80 | 126);
    out += char(n >> 8);
    out += char(n);
  } else {
    out += char(0x80 | 127);
    for (int i = 7; i >= 0; i--) out += char((uint64_t(n) >> (8 * i)) & 0xff);
  }
  uint8_t mask[4];
  if (!random_bytes(mask, 4)) memset(mask, 0x5a, 4);
  out.append(reinterpret_cast<char *>(mask), 4);
  for (size_t i = 0; i < n; i++) out += char(uint8_t(p[i]) ^ mask[i & 3]);
}

struct Local {
  int fd = -1;
  std::string out;   // bytes for the local socket that it did not take yet
};

}  // namespace

LinkClient::~LinkClient() { stop(); }

void LinkClient::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void LinkClient::send_wake(const std::string &station, const std::string &wake, bool busy, float level) {
  if (state_ != 3) return;   // not confirmed or not connected: nobody to tell
  char lv[16];
  snprintf(lv, sizeof lv, "%.1f", level);
  std::string f = "{\"t\":\"wake\",\"station\":" + json_str(station) + ",\"word\":" + json_str(wake) + ",\"busy\":" + (busy ? "1" : "0") + ",\"level\":\"" + lv + "\"}";
  std::lock_guard<std::mutex> lk(wake_mu_);
  if (wake_out_.size() < 8) wake_out_.push_back(std::move(f));
  if (wake_fd_ >= 0) {
    const uint64_t one = 1;
    if (write(wake_fd_, &one, sizeof one) < 0) { /* the thread wakes within a second anyway */ }
  }
}

std::string LinkClient::state() const {
  static const char *names[] = {"off", "connecting", "pending", "online"};
  return names[std::clamp(state_.load(), 0, 3)];
}

void LinkClient::start(const std::string &station_name) {
  name_ = station_name;
  fresh_ = access("/tmp/lostos-boot-done", F_OK) != 0;
  if (wake_fd_ < 0) wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (read_server().empty()) {
    LOGI("link: off (no server in %s/link.conf)", kDir);
    return;
  }
  if (!ed25519_available()) {
    LOGW("link: the firmware's OpenSSL has no Ed25519, the link stays off");
    return;
  }
  thread_ = std::thread([this] { run(); });
}

void LinkClient::run() {
  int delay = 3;
  while (!stop_) {
    state_ = 1;
    bool was_up = session();
    state_ = 1;
    if (stop_) break;
    delay = was_up ? 3 : std::min(delay * 2, 60);
    for (int i = 0; i < delay * 10 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  state_ = 0;
}

bool LinkClient::session() {
  const std::string server = read_server();
  std::string host = server, port = "443";
  if (host.rfind("https://", 0) != 0) {
    LOGW("link: only https:// servers, got '%s'", server.c_str());
    return false;
  }
  host.erase(0, 8);
  if (size_t sl = host.find('/'); sl != std::string::npos) host.erase(sl);
  if (size_t c = host.find(':'); c != std::string::npos) {
    port = host.substr(c + 1);
    host.erase(c);
  }
  uint8_t priv[32], pub[32];
  if (!read_key(priv) || !ed25519_pub(priv, pub)) {
    LOGW("link: no key");
    return false;
  }
  const std::string id = sha256_hex(std::string(reinterpret_cast<char *>(pub), 32)).substr(0, 20);
  if (FILE *f = fopen((std::string(kDir) + "/link.id").c_str(), "w")) {
    fprintf(f, "%s\n", id.c_str());
    fclose(f);
  }

  TlsSock t;
  std::string err;
  const std::string ca = std::string(kDir) + "/link-ca.crt";
  if (!tls_connect(host, port, t, err, access(ca.c_str(), F_OK) == 0 ? ca : "")) {
    LOGW("link: %s", err.c_str());
    return false;
  }
  auto wait_fd = [&](short ev, int ms) {
    pollfd p{t.fd, ev, 0};
    return poll(&p, 1, ms) > 0;
  };
  std::string out, in;
  {   // WebSocket handshake
    uint8_t rnd[16];
    if (!random_bytes(rnd, 16)) memset(rnd, 0x33, 16);
    std::string req = "GET /link/ws HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + b64(rnd, 16) +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    size_t off = 0;
    while (off < req.size()) {
      int n = 0;
      TlsIo r2 = tls_write(t, req.data() + off, int(req.size() - off), n);
      if (r2 == TlsIo::OK) off += size_t(n);
      else if (r2 == TlsIo::WANT_WRITE || r2 == TlsIo::WANT_READ) wait_fd(r2 == TlsIo::WANT_WRITE ? POLLOUT : POLLIN, 5000);
      else {
        tls_close(t);
        return false;
      }
    }
  }
  auto fill = [&](int ms) -> int {   // 1 = something read, 0 = nothing yet, -1 = closed or failed
    char tmp[16384];
    for (;;) {
      int n = 0;
      TlsIo r = tls_read(t, tmp, sizeof tmp, n);
      if (r == TlsIo::OK) {
        in.append(tmp, size_t(n));
        return 1;
      }
      if (r == TlsIo::WANT_READ || r == TlsIo::WANT_WRITE) {
        if (!wait_fd(r == TlsIo::WANT_WRITE ? POLLOUT : POLLIN, ms)) return 0;
        continue;
      }
      return -1;
    }
  };
  for (int tries = 0; in.find("\r\n\r\n") == std::string::npos; tries++) {
    if (tries > 10 || in.size() > 8192 || fill(2000) < 0) {
      LOGW("link: no WebSocket answer");
      tls_close(t);
      return false;
    }
  }
  if (in.compare(0, 12, "HTTP/1.1 101") != 0) {
    LOGW("link: the server did not accept the WebSocket (%.20s)", in.c_str());
    tls_close(t);
    return false;
  }
  in.erase(0, in.find("\r\n\r\n") + 4);

  std::map<uint32_t, Local> streams;
  const std::vector<Forward> forwards = read_forwards();
  bool up = false;
  auto send_text = [&](const std::string &s) { ws_frame(out, 1, s.data(), s.size()); };
  auto drop = [&](uint32_t sid, bool tell) {
    auto it = streams.find(sid);
    if (it == streams.end()) return;
    if (it->second.fd >= 0) close(it->second.fd);
    streams.erase(it);
    if (tell) send_text("{\"t\":\"close\",\"sid\":" + std::to_string(sid) + "}");
  };
  auto last_rx = std::chrono::steady_clock::now();
  bool alive = true;
  // The window in which the Alice button confirms this station (the server opened it: "press"). Only a nonce the server gave is answered.
  std::string press_nonce;
  std::chrono::steady_clock::time_point press_until;
  auto press_end = [&] {
    if (press_nonce.empty()) return;
    press_nonce.clear();
    if (on_press_end) on_press_end();
  };
  press_hit_ = false;
  while (alive && !stop_) {
    // ---- frames from the server
    for (;;) {
      if (in.size() < 2) break;
      const uint8_t b0 = uint8_t(in[0]), b1 = uint8_t(in[1]);
      size_t hl = 2, len = b1 & 0x7f;
      if (len == 126) {
        if (in.size() < 4) break;
        len = size_t(uint8_t(in[2])) << 8 | uint8_t(in[3]);
        hl = 4;
      } else if (len == 127) {
        if (in.size() < 10) break;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | uint8_t(in[2 + i]);
        hl = 10;
      }
      if (len > kMaxFrame || (b1 & 0x80) || !(b0 & 0x80)) {   // a masked server frame or a fragmented message: not what our server sends
        alive = false;
        break;
      }
      if (in.size() < hl + len) break;
      const int op = b0 & 15;
      std::string payload = in.substr(hl, len);
      in.erase(0, hl + len);
      last_rx = std::chrono::steady_clock::now();
      if (op == 9) {
        ws_frame(out, 10, payload.data(), payload.size());
      } else if (op == 8) {
        alive = false;
      } else if (op == 2 && payload.size() > 4) {
        uint32_t sid = uint32_t(uint8_t(payload[0])) << 24 | uint32_t(uint8_t(payload[1])) << 16 | uint32_t(uint8_t(payload[2])) << 8 | uint8_t(payload[3]);
        auto it = streams.find(sid);
        if (it != streams.end()) {
          it->second.out.append(payload, 4, std::string::npos);
          if (it->second.out.size() > kMaxOut) drop(sid, true);
        }
      } else if (op == 1) {
        const std::string kind = jget(payload, "t");
        if (kind == "challenge") {
          const std::string nonce = hex_decode(jget(payload, "nonce"));
          uint8_t sig[64];
          if (!ed25519_sign(priv, nonce + "lostos-link-v1", sig)) {
            alive = false;
            break;
          }
          send_text(std::string("{\"t\":\"hello\",\"ver\":2,\"fresh\":") + (fresh_ ? "1" : "0") + ",\"pub\":\"" + b64(pub, 32) + "\",\"sig\":\"" + b64(sig, 64) + "\",\"name\":" + json_str(name_) + "}");
        } else if (kind == "wake") {
          if (on_wake) on_wake(jget(payload, "station"), jget(payload, "word"), jget(payload, "busy") == "1" || jget(payload, "busy") == "true",
                               float(atof(jget(payload, "level").c_str())));
        } else if (kind == "peers") {
          if (on_peers) on_peers(atoi(jget(payload, "n").c_str()));
        } else if (kind == "say") {
          const std::string wav = b64_decode(jget(payload, "wav"));
          if (wav.size() > 44 && wav.size() < (2u << 20) && on_say) {
            const std::string path = "/tmp/lostos-say.wav";
            if (FILE *f = fopen(path.c_str(), "wb")) {
              fwrite(wav.data(), 1, wav.size(), f);
              fclose(f);
              on_say(path);
            }
          }
        } else if (kind == "press") {
          const std::string nonce = jget(payload, "nonce");
          const int secs = std::clamp(atoi(jget(payload, "seconds").c_str()), 10, 120);
          const std::string wav = b64_decode(jget(payload, "wav"));
          std::string path;
          if (wav.size() > 44 && wav.size() < (2u << 20)) {
            path = "/tmp/lostos-press.wav";
            if (FILE *f = fopen(path.c_str(), "wb")) {
              fwrite(wav.data(), 1, wav.size(), f);
              fclose(f);
            } else {
              path.clear();
            }
          }
          if (!nonce.empty()) {
            press_nonce = nonce;
            press_until = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
            press_hit_ = false;
            LOGI("link: the owner confirmed this station: press the Alice button within %d s", secs);
            if (on_press) on_press(jget(payload, "code"), path);
          }
        } else if (kind == "welcome" || kind == "state") {
          const std::string st = jget(payload, "state");
          if (kind == "welcome") fresh_ = false;            // the server has heard it: later hellos (a reconnect) are not "just started"
          if (st == "confirmed") press_end();
          state_ = st == "confirmed" ? 3 : 2;
          LOGI("link: station %s, %s", id.c_str(), st == "confirmed" ? "confirmed by the owner" : "waits for the owner's confirmation in the panel");
          up = true;
          if (st == "confirmed") {
            // the vault key: made on this speaker, it opens the encrypted keys of the owner's server for as long as the speaker is connected
            uint8_t vk[32];
            for (const Forward &fw : forwards) send_text("{\"t\":\"forward\",\"name\":" + json_str(fw.name) + ",\"addr\":" + json_str(fw.host + ":" + std::to_string(fw.port)) + "}");
            if (read_key(vk, "vault.key")) send_text("{\"t\":\"vaultkey\",\"key\":\"" + b64(vk, 32) + "\"}");
            memset(vk, 0, sizeof vk);
            // the server of the owner needs the key of the speaker's API (Noise) to talk to it; it travels only inside this TLS connection
            std::ifstream kf(std::string(kDir) + "/api_key");
            std::string key;
            std::getline(kf, key);
            while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
            if (key.size() == 44) send_text("{\"t\":\"apikey\",\"key\":\"" + key + "\"}");
          }
        } else if (kind == "open") {
          const uint32_t sid = uint32_t(atol(jget(payload, "sid").c_str()));
          int p = atoi(jget(payload, "port").c_str());
          uint32_t target = htonl(INADDR_LOOPBACK);
          const std::string fwname = jget(payload, "forward");
          if (!fwname.empty()) {                       // a forward: the address comes from OUR link.conf, never from the server
            p = 0;
            for (const Forward &fw : forwards) {
              if (fw.name != fwname) continue;
              in_addr ia{};
              if (inet_pton(AF_INET, fw.host.c_str(), &ia) == 1) {
                target = ia.s_addr;
                p = fw.port;
              }
            }
          }
          if (fwname.empty() && p != 6053 && p != 7000 && p != 49153) p = 0;
          if (p == 0) {
            send_text("{\"t\":\"fail\",\"sid\":" + std::to_string(sid) + "}");
          } else {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(uint16_t(p));
            a.sin_addr.s_addr = target;
            timeval tv{2, 0};
            if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) {
              fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
              int one = 1;
              setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
              streams[sid].fd = fd;
              send_text("{\"t\":\"opened\",\"sid\":" + std::to_string(sid) + "}");
            } else {
              if (fd >= 0) close(fd);
              send_text("{\"t\":\"fail\",\"sid\":" + std::to_string(sid) + "}");
            }
          }
        } else if (kind == "close") {
          drop(uint32_t(atol(jget(payload, "sid").c_str())), false);
        }
      }
    }
    if (!alive) break;
    if (!press_nonce.empty()) {
      if (press_hit_.exchange(false)) {
        send_text("{\"t\":\"pressed\",\"nonce\":" + json_str(press_nonce) + "}");
        LOGI("link: the Alice button was pressed");
      } else if (std::chrono::steady_clock::now() > press_until) {
        send_text("{\"t\":\"press_timeout\",\"nonce\":" + json_str(press_nonce) + "}");
        LOGI("link: the button was not pressed in time");
        press_end();
      }
    }
    {   // the wake claims of this station (up only while the owner has confirmed it: an unconfirmed station has no household)
      std::vector<std::string> frames;
      {
        std::lock_guard<std::mutex> lk(wake_mu_);
        frames.swap(wake_out_);
      }
      if (state_ == 3)
        for (const std::string &f : frames) send_text(f);
    }
    // ---- what waits to go to the server
    while (!out.empty()) {
      int n = 0;
      TlsIo r = tls_write(t, out.data(), int(std::min<size_t>(out.size(), 16384)), n);
      if (r == TlsIo::OK) {
        out.erase(0, size_t(n));
      } else if (r == TlsIo::WANT_READ || r == TlsIo::WANT_WRITE) {
        break;
      } else {
        alive = false;
        break;
      }
    }
    if (!alive) break;
    // ---- wait for something to happen
    std::vector<pollfd> fds;
    fds.push_back({t.fd, short(POLLIN | (out.empty() ? 0 : POLLOUT)), 0});
    std::vector<uint32_t> ids;
    for (auto &kv : streams) {
      short ev = short((out.size() < kMaxOut ? POLLIN : 0) | (kv.second.out.empty() ? 0 : POLLOUT));
      fds.push_back({kv.second.fd, ev, 0});
      ids.push_back(kv.first);
    }
    if (wake_fd_ >= 0) fds.push_back({wake_fd_, POLLIN, 0});   // last: the stream indices above stay as they are
    if (!tls_pending(t)) poll(fds.data(), nfds_t(fds.size()), 1000);
    if (wake_fd_ >= 0 && (fds.back().revents & POLLIN)) {
      uint64_t v;
      if (read(wake_fd_, &v, sizeof v) < 0) { /* drained or not, the claim is sent in the next round */ }
    }
    if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP)) || tls_pending(t)) {
      if (fill(0) < 0) break;
    }
    for (size_t i = 0; i < ids.size(); i++) {
      const uint32_t sid = ids[i];
      auto it = streams.find(sid);
      if (it == streams.end()) continue;
      const short re = fds[i + 1].revents;
      if (re & POLLOUT) {
        ssize_t n = send(it->second.fd, it->second.out.data(), it->second.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
          it->second.out.erase(0, size_t(n));
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
          drop(sid, true);
          continue;
        }
      }
      if (re & POLLIN) {
        char buf[16384];
        ssize_t n = recv(it->second.fd, buf, sizeof buf, 0);
        if (n > 0) {
          std::string frame(4, '\0');
          frame[0] = char(sid >> 24);
          frame[1] = char(sid >> 16);
          frame[2] = char(sid >> 8);
          frame[3] = char(sid);
          frame.append(buf, size_t(n));
          ws_frame(out, 2, frame.data(), frame.size());
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
          drop(sid, true);
        }
      } else if (re & (POLLERR | POLLHUP)) {
        drop(sid, true);
      }
    }
    if (std::chrono::steady_clock::now() - last_rx > std::chrono::seconds(90)) {
      LOGW("link: the server is silent for 90 s, reconnecting");
      break;
    }
  }
  for (auto &kv : streams)
    if (kv.second.fd >= 0) close(kv.second.fd);
  press_end();
  tls_close(t);
  if (up) LOGI("link: connection ended");
  return up;
}

}  // namespace cs
