#include "plugins/host.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

#include <thread>

#include "core/log.h"
#include "plugins/trust.h"
#include "update/https.h"

namespace cs {

namespace {

constexpr const char *kRoot = "/data/lostos/plugins";
constexpr const char *kJsRuntime = "/data/lostos/lostos-js";
constexpr const char *kFfmpeg = "/data/lostos/ffmpeg";   // a static aarch64 ffmpeg (tools/build-ffmpeg.sh) that programs of plugins find as /opt/bin/ffmpeg   // plugin-runtime/: the program of every JavaScript plugin
constexpr size_t kMaxKeys = 32, kMaxValue = 256, kMaxLine = 16384, kMaxScript = 1u << 20, kMaxBody = 256u << 10;
constexpr int kMaxTools = 16, kMaxHttp = 4;
constexpr int64_t kResultKeepMs = 20000;

const char *const kKnownPermissions[] = {"device.led", "device.display", "media.control", "device.read", "events.subscribe", "events.publish", "storage",
                                         "network.connect", "network.listen", "audio.play", "assistant.intercept", "assistant.tools", "exec.bin64"};

// The TCP ports a user id listens on (a program of a plugin has a user id of its own): the page of a plugin is behind one of them.
std::vector<int> listening_ports(unsigned want_uid) {
  std::vector<int> out;
  for (const char *file : {"/proc/net/tcp", "/proc/net/tcp6"}) {
    std::ifstream f(file);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
      unsigned port = 0, st = 0, uid = 0;
      if (sscanf(line.c_str(), " %*d: %*64[0-9A-Fa-f]:%x %*64[0-9A-Fa-f]:%*x %x %*x:%*x %*x:%*x %*x %u", &port, &st, &uid) != 3) continue;
      if (st == 0x0A && uid == want_uid && port > 0 && std::find(out.begin(), out.end(), int(port)) == out.end()) out.push_back(int(port));
    }
  }
  return out;
}

int64_t mono_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// JSON: a string literal of s (UTF-8 kept, control characters escaped), and a reader for the flat objects the runtime sends.
std::string json_quote(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else o += char(c);
  }
  return o + "\"";
}

void put_utf8(std::string &o, unsigned cp) {
  if (cp < 0x80) o += char(cp);
  else if (cp < 0x800) o += char(0xC0 | (cp >> 6)), o += char(0x80 | (cp & 0x3F));
  else if (cp < 0x10000) o += char(0xE0 | (cp >> 12)), o += char(0x80 | ((cp >> 6) & 0x3F)), o += char(0x80 | (cp & 0x3F));
  else o += char(0xF0 | (cp >> 18)), o += char(0x80 | ((cp >> 12) & 0x3F)), o += char(0x80 | ((cp >> 6) & 0x3F)), o += char(0x80 | (cp & 0x3F));
}

void skip_ws(const std::string &s, size_t &i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

bool read_string(const std::string &s, size_t &i, std::string &out) {
  if (i >= s.size() || s[i] != '"') return false;
  out.clear();
  for (i++; i < s.size(); i++) {
    char c = s[i];
    if (c == '"') return i++, true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        if (i + 4 >= s.size()) return false;
        unsigned cp = unsigned(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
        i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
          const unsigned lo = unsigned(strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
          if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), i += 6;
        }
        put_utf8(out, cp);
        break;
      }
      default: out += s[i];
    }
  }
  return false;
}

// Skips any JSON value (the parts of a request this firmware does not use).
bool skip_value(const std::string &s, size_t &i) {
  skip_ws(s, i);
  if (i >= s.size()) return false;
  if (s[i] == '"') {
    std::string tmp;
    return read_string(s, i, tmp);
  }
  if (s[i] == '{' || s[i] == '[') {
    int depth = 0;
    for (; i < s.size(); i++) {
      if (s[i] == '"') {
        std::string tmp;
        if (!read_string(s, i, tmp)) return false;
        i--;
      } else if (s[i] == '{' || s[i] == '[') depth++;
      else if ((s[i] == '}' || s[i] == ']') && --depth == 0) return i++, true;
    }
    return false;
  }
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
  return true;
}

// The members of a JSON object: strings as they are, objects of strings as "name: value" lines (headers), the rest skipped.
bool read_object(const std::string &s, std::map<std::string, std::string> &str, std::vector<std::string> *pairs = nullptr) {
  size_t i = 0;
  skip_ws(s, i);
  if (i >= s.size() || s[i] != '{') return false;
  for (i++;;) {
    skip_ws(s, i);
    if (i < s.size() && s[i] == '}') return true;
    std::string key;
    if (!read_string(s, i, key)) return false;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    i++;
    skip_ws(s, i);
    if (i < s.size() && s[i] == '"') {
      std::string v;
      if (!read_string(s, i, v)) return false;
      str[key] = v;
    } else if (i < s.size() && s[i] == '{' && pairs) {
      const size_t start = i;
      if (!skip_value(s, i)) return false;
      std::map<std::string, std::string> inner;
      read_object(s.substr(start, i - start), inner);
      for (const auto &kv : inner) pairs->push_back(kv.first + ": " + kv.second);
    } else if (!skip_value(s, i)) {
      return false;
    }
    skip_ws(s, i);
    if (i < s.size() && s[i] == ',') i++;
  }
}

std::string b64_decode(const std::string &in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string o;
  int bits = 0, acc = 0;
  for (char c : in) {
    const int v = val(c);
    if (v < 0) continue;
    acc = (acc << 6) | v, bits += 6;
    if (bits >= 8) bits -= 8, o += char((acc >> bits) & 0xFF);
  }
  return o;
}

bool same_inode(const std::string &a, const std::string &b) {
  struct stat sa{}, sb{};
  return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0 && sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev;
}

bool valid_id(const std::string &id) {
  if (id.size() < 3 || id.size() > 60) return false;
  for (char c : id)
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
  return id.find("..") == std::string::npos;
}

std::vector<std::string> words(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string join(const std::vector<std::string> &v, char sep) {
  std::string o;
  for (size_t i = 0; i < v.size(); i++) o += (i ? std::string(1, sep) : std::string()) + v[i];
  return o;
}

}  // namespace

// Is this address one the speaker answers itself (loopback, link-local, one of its own)? Plugins must not reach those: that is adb,
// the API of the speaker and every other service of the firmware.
bool is_local_address(const sockaddr_in &a) {
  const uint32_t ip = ntohl(a.sin_addr.s_addr);
  if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE || ip == 0) return true;
  bool own = false;
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (ifaddrs *i = list; i; i = i->ifa_next)
      if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr == a.sin_addr.s_addr) own = true;
    freeifaddrs(list);
  }
  return own;
}

bool resolve_ipv4(const std::string &host, sockaddr_in *out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  *out = *reinterpret_cast<sockaddr_in *>(res->ai_addr);
  freeaddrinfo(res);
  return true;
}

std::string PluginHost::dir(const std::string &id) { return std::string(kRoot) + "/" + id; }

void PluginHost::save(const Plugin &p) const {
  std::ofstream f(dir(p.id) + "/plugin.conf");
  f << "name=" << p.name << "\nenabled=" << (p.enabled ? 1 : 0) << "\ngranted=" << join(p.granted, ',') << "\nuid=" << p.uid << "\n"
    << "asked=" << join(p.asked, ',') << "\nsha=" << p.sha << "\ntrusted=" << (p.trusted ? 1 : 0) << "\nwho=" << p.who << "\ndeclared=" << join(p.declared, ',') << "\n";
  std::ofstream d(dir(p.id) + "/data.txt");
  for (const auto &kv : p.kv) d << kv.first << '\t' << kv.second << '\n';
}

bool PluginHost::load(const std::string &id) {
  std::ifstream f(dir(id) + "/plugin.conf");
  if (!f) return false;
  Plugin p;
  p.id = id;
  for (std::string line; std::getline(f, line);) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "kind" && v == "system") return false;   // run by SystemPlugins (plugins/system.h)
    else if (k == "name") p.name = v;
    else if (k == "enabled") p.enabled = v == "1";
    else if (k == "granted") p.granted = split(v, ',');
    else if (k == "uid") p.uid = unsigned(strtoul(v.c_str(), nullptr, 10));
    else if (k == "asked") p.asked = split(v, ',');
    else if (k == "sha") p.sha = v;
    else if (k == "trusted") p.trusted = v == "1";
    else if (k == "who") p.who = v;
    else if (k == "declared") p.declared = split(v, ',');
  }
  p.js = access((dir(id) + "/main.js").c_str(), F_OK) == 0;
  p.bin64 = access((dir(id) + "/bin64").c_str(), F_OK) == 0;
  p.granted = allowed_for(p, p.granted);   // whatever an earlier firmware granted (the dangerous permissions of a plugin that nobody trusted) is taken back
  std::ifstream d(dir(id) + "/data.txt");
  for (std::string line; std::getline(d, line);) {
    const size_t t = line.find('\t');
    if (t != std::string::npos && p.kv.size() < kMaxKeys) p.kv[line.substr(0, t)] = line.substr(t + 1, kMaxValue);
  }
  if (p.uid < 40000) p.uid = 40000 + unsigned(plugins_.size()) + 1;
  plugins_[id] = p;
  return true;
}

void PluginHost::start() {
  mkdir(kRoot, 0755);
  if (DIR *d = opendir(kRoot)) {
    while (dirent *e = readdir(d)) {
      const std::string id = e->d_name;
      if (id[0] != '.' && valid_id(id) && load(id)) {
        LOGI("plugin %s: %s", id.c_str(), plugins_[id].enabled ? "on" : "off");
      }
    }
    closedir(d);
  }
  system_.start();
  for (auto &kv : plugins_)
    if (kv.second.enabled) run(kv.second);
}

// What the speaker grants of what the server asks: a known permission, harmless, or declared by a package that this speaker has verified (trust.h).
std::vector<std::string> PluginHost::allowed_for(const Plugin &p, const std::vector<std::string> &asked) const {
  std::vector<std::string> known;
  for (const std::string &g : asked)
    for (const char *k : kKnownPermissions)
      if (g == k) known.push_back(g);
  PluginTrust t;
  t.trusted = p.trusted;
  t.declared = p.declared;
  std::vector<std::string> out = permissions_to_grant(known, t);
  if (p.bin64 && std::find(known.begin(), known.end(), "exec.bin64") != known.end() && std::find(out.begin(), out.end(), "exec.bin64") == out.end())
    out.push_back("exec.bin64");                                   // the owner allowed it in his panel (the server asks only for what he allowed): that is his consent for a program of his own plugin
  return out;
}

// Called when a package has been downloaded: the speaker looks at the signature itself, whatever the server says about the plugin.
void PluginHost::trust(Plugin &p) {
  const PluginTrust t = verify_plugin(dir(p.id), p.sha, int64_t(time(nullptr)));
  p.trusted = t.trusted;
  p.who = t.who;
  p.declared = t.declared;
  p.granted = allowed_for(p, p.asked);
  std::vector<std::string> refused;
  for (const std::string &g : p.asked)
    if (std::find(p.granted.begin(), p.granted.end(), g) == p.granted.end()) refused.push_back(g);
  const std::string granted = join(p.granted, ','), no = join(refused, ',');
  if (t.trusted)
    LOGI("plugin %s: signed by %s, the signature is checked here; permissions: %s; refused (not in the signed manifest): %s", p.id.c_str(), t.who.c_str(), granted.c_str(), no.empty() ? "-" : no.c_str());
  else
    LOGW("plugin %s: not signed by a key this speaker trusts (%s); only the harmless permissions: %s; refused: %s", p.id.c_str(), t.why.c_str(), granted.c_str(), no.empty() ? "-" : no.c_str());
}

bool PluginHost::allowed(const Plugin &p, const std::string &perm) const {
  return std::find(p.granted.begin(), p.granted.end(), perm) != p.granted.end();
}

void PluginHost::run(Plugin &p) {
  if (p.proc.pid > 0) return;
  std::string err;
  SandboxLimits lim;
  if (p.js) {                                                      // the shared runtime is the program: a hard link (one copy on the flash), renewed after an update
    const std::string prog = dir(p.id) + "/root/plugin";
    if (access(kJsRuntime, X_OK) != 0) {
      p.detail = "нет среды JavaScript (обновите lostOS)";
      if (state_changed) state_changed(p.id + "\terror\t" + p.detail);
      return;
    }
    if (!same_inode(kJsRuntime, prog)) {
      mkdir((dir(p.id) + "/root").c_str(), 0755);
      unlink(prog.c_str());
      if (link(kJsRuntime, prog.c_str()) != 0) {
        std::ifstream src(kJsRuntime, std::ios::binary);
        std::ofstream dst(prog, std::ios::binary);
        dst << src.rdbuf();
        dst.close();
        chmod(prog.c_str(), 0755);
      }
    }
    lim.address_space = 64u << 20;                                // QuickJS: its 24 MB heap and the program
  }
  bool spawned;
  if (p.bin64) {
    if (!allowed(p, "exec.bin64")) {
      p.detail = "нет разрешения запускать программу (разрешите его в панели)";
      if (state_changed) state_changed(p.id + "\terror\t" + p.detail);
      return;
    }
    if (api_fd_ < 0) start_api();
    unsigned char rnd[12] = {0};
    if (FILE *f = fopen("/dev/urandom", "rb")) { if (fread(rnd, 1, sizeof rnd, f) != sizeof rnd) memset(rnd, 0, sizeof rnd); fclose(f); }
    char tok[32];
    for (int i = 0; i < 12; i++) snprintf(tok + i * 2, 3, "%02x", rnd[i]);
    p.token = tok;
    const std::string data = dir(p.id) + "/root/data";                // the one place the program may write
    mkdir(data.c_str(), 0755);
    if (chown(data.c_str(), p.uid, p.uid) != 0) LOGW("plugin %s: no owner for its data folder", p.id.c_str());
    const std::string url = "http://127.0.0.1:" + std::to_string(api_port_);
    const std::vector<std::string> env = {"YAM_API_URL=" + url, "YAM_API_TOKEN=" + p.token, "LUNA_API_URL=" + url, "LUNA_API_TOKEN=" + p.token, "UWU_STATE=/data/state.json", "HOME=/data", "PATH=/"};
    const std::string logp = dir(p.id) + "/plugin.log";
    struct stat ls{};
    if (stat(logp.c_str(), &ls) == 0 && ls.st_size > (64 << 10)) unlink(logp.c_str());   // a log that does not grow without end
    std::vector<std::string> pargs;
    {
      std::ifstream af(dir(p.id) + "/args");
      for (std::string line; std::getline(af, line);)
        if (!line.empty() && pargs.size() < 16) pargs.push_back(line);
    }
    if (access(kFfmpeg, X_OK) == 0) {                                // the one shared ffmpeg of the speaker, in the program's folder as /opt/bin/ffmpeg (a hard link: one copy on the flash)
      const std::string bindir = dir(p.id) + "/root/opt/bin", target = bindir + "/ffmpeg";
      mkdir((dir(p.id) + "/root/opt").c_str(), 0755);
      mkdir(bindir.c_str(), 0755);
      if (!same_inode(kFfmpeg, target)) {
        unlink(target.c_str());
        if (link(kFfmpeg, target.c_str()) != 0) {
          std::ifstream src(kFfmpeg, std::ios::binary);
          std::ofstream dst(target, std::ios::binary);
          dst << src.rdbuf();
          dst.close();
          chmod(target.c_str(), 0755);
        }
      }
    }
    const std::string tmpd = dir(p.id) + "/root/tmp";                // a /tmp of its own inside its folder (programs expect one)
    mkdir(tmpd.c_str(), 01777);
    chmod(tmpd.c_str(), 01777);
    spawned = sandbox_spawn_bin64(dir(p.id) + "/root", p.uid, pargs, env, logp, &p.proc, &err);
  } else {
    spawned = sandbox_spawn(dir(p.id) + "/root", p.uid, lim, &p.proc, &err);
  }
  if (!spawned) {
    p.detail = "не запустился: " + err;
    state_changed(p.id + "\terror\t" + p.detail);
    return;
  }
  p.detail = "работает";
  p.inbuf.clear();
  p.gen++;
  if (p.bin64) {                                                   // a program of a plugin: once it listens, the panel is told on which port its page is
    const std::string id = p.id;
    const unsigned gen = p.gen, uid = p.uid;
    loop_.add_timer(3000, [this, id, gen, uid] {
      auto it = plugins_.find(id);
      if (it == plugins_.end() || it->second.gen != gen || it->second.proc.pid <= 0) return;
      const std::vector<int> ports = listening_ports(uid);
      if (ports.empty()) return;
      it->second.detail = "работает, порт " + std::to_string(ports[0]);
      if (state_changed) state_changed(id + "\trunning\t" + it->second.detail);
    });
  }
  p.http_busy = 0;
  p.tools.clear();
  if (p.js && !send_script(p)) {
    halt(p);
    p.detail = "не запустился: скрипт не передан";
    if (state_changed) state_changed(p.id + "\terror\t" + p.detail);
    return;
  }
  const std::string id = p.id;
  loop_.add_fd(p.proc.fd, EPOLLIN, [this, id](uint32_t) { on_readable(id); });
  LOGI("plugin %s started (pid %d, uid %u)", id.c_str(), int(p.proc.pid), p.uid);
  if (state_changed) state_changed(p.id + "\trunning\t");
}

// The code of a JavaScript plugin goes to the runtime as its first message (the sandbox cannot open files).
bool PluginHost::send_script(Plugin &p) {
  std::ifstream f(dir(p.id) + "/main.js", std::ios::binary);
  std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (src.empty() || src.size() > kMaxScript) return false;
  const std::string msg = "! script " + std::to_string(src.size()) + "\n" + src;
  size_t off = 0;
  const int64_t until = mono_ms() + 3000;
  while (off < msg.size()) {
    const ssize_t w = send(p.proc.fd, msg.data() + off, msg.size() - off, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w > 0) {
      off += size_t(w);
      continue;
    }
    if (w < 0 && errno != EAGAIN && errno != EINTR) return false;
    if (mono_ms() > until) return false;
    pollfd pf{p.proc.fd, POLLOUT, 0};
    poll(&pf, 1, 200);
  }
  return true;
}

void PluginHost::audio_close(Plugin &p) {
  if (p.aplay > 0) {
    kill(p.aplay, SIGTERM);
    waitpid(p.aplay, nullptr, 0);
    p.aplay = -1;
    if (audio_state) audio_state(false);
  }
}

void PluginHost::audio_stop() {
  for (auto &kv : plugins_) {
    audio_close(kv.second);
    if (kv.second.proc.fd >= 0) {
      const std::string line = "! audio stop\n";
      (void)!send(kv.second.proc.fd, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
  }
}

void PluginHost::release_button(Plugin &p) {
  if (!p.claims_button) return;
  p.claims_button = false;
  bool any = false;
  for (auto &kv : plugins_) any = any || kv.second.claims_button;
  if (button_claim) button_claim(any);
}

void PluginHost::halt(Plugin &p) {
  audio_close(p);
  release_button(p);
  if (p.plays && stop_url) stop_url();
  p.plays = false;
  p.tools.clear();
  if (p.proc.fd >= 0) {
    loop_.del_fd(p.proc.fd);
    close(p.proc.fd);
    p.proc.fd = -1;
  }
  if (p.proc.pid > 0) {
    kill(p.proc.pid, SIGKILL);
    waitpid(p.proc.pid, nullptr, 0);
    p.proc.pid = -1;
  }
  p.subscribed.clear();
}

void PluginHost::reply(Plugin &p, const std::string &n, const std::string &res, int fd) {
  const std::string line = n + " " + res + "\n";
  iovec iov{const_cast<char *>(line.data()), line.size()};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))] = {0};
  if (fd >= 0) {                                                   // the descriptor travels with the answer (SCM_RIGHTS)
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof ctl;
    cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
  }
  ssize_t sent = sendmsg(p.proc.fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
  if (fd >= 0) close(fd);                                          // the plugin has its own copy now
  // A long answer (an HTTP body) does not fit the socket buffer at once: the rest follows as the plugin reads, for up to 2 s;
  // a plugin that does not read gets a broken line, which only hurts itself.
  size_t off = sent > 0 ? size_t(sent) : 0;
  const int64_t until = mono_ms() + 2000;
  while (off < line.size() && (sent > 0 || errno == EAGAIN || errno == EINTR) && mono_ms() < until) {
    pollfd pf{p.proc.fd, POLLOUT, 0};
    poll(&pf, 1, 100);
    sent = send(p.proc.fd, line.data() + off, line.size() - off, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) off += size_t(sent);
  }
}

void PluginHost::on_readable(const std::string &id) {
  auto it = plugins_.find(id);
  if (it == plugins_.end()) return;
  Plugin &p = it->second;
  char buf[2048];
  for (;;) {
    const ssize_t n = recv(p.proc.fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) {
      p.inbuf.append(buf, size_t(n));
      if (p.inbuf.size() > 8 * kMaxLine) {                       // a plugin that never ends a line
        p.detail = "слишком длинные сообщения";
        halt(p);
        p.enabled = false;
        save(p);
        if (state_changed) state_changed(p.id + "\toff\t" + p.detail);
        return;
      }
      continue;
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
      stopped(p);
      return;
    }
    break;
  }
  size_t nl;
  while ((nl = p.inbuf.find('\n')) != std::string::npos) {
    std::string line = p.inbuf.substr(0, nl);
    p.inbuf.erase(0, nl + 1);
    if (line.size() <= kMaxLine) handle(p, line);
  }
}

void PluginHost::stopped(Plugin &p) {
  int st = 0;
  if (p.proc.pid > 0) waitpid(p.proc.pid, &st, 0);
  if (p.proc.fd >= 0) {
    loop_.del_fd(p.proc.fd);
    close(p.proc.fd);
  }
  p.proc = SandboxProc();
  p.subscribed.clear();
  p.tools.clear();
  audio_close(p);
  release_button(p);
  if (p.plays && stop_url) stop_url();
  p.plays = false;
  const std::string why = sandbox_describe_exit(st);
  LOGI("plugin %s ended%s%s", p.id.c_str(), why.empty() ? "" : ": ", why.c_str());
  if (!p.enabled) return;
  p.crashes++;
  if (!why.empty() && p.crashes > 2) {
    p.enabled = false;
    p.detail = "выключен: " + why;
    save(p);
    if (state_changed) state_changed(p.id + "\toff\t" + p.detail);
    return;
  }
  p.detail = why.empty() ? "завершился" : why;
  if (state_changed) state_changed(p.id + "\trestarting\t" + p.detail);
  const std::string id = p.id;
  loop_.add_timer(3000, [this, id] {
    auto it = plugins_.find(id);
    if (it != plugins_.end() && it->second.enabled) run(it->second);
  });
}

void PluginHost::handle(Plugin &p, const std::string &line) {
  const size_t s1 = line.find(' ');
  if (s1 == std::string::npos) return;
  const std::string n = line.substr(0, s1);
  const size_t s2 = line.find(' ', s1 + 1);
  const std::string method = line.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
  const std::string args = s2 == std::string::npos ? "" : line.substr(s2 + 1);
  auto need = [&](const char *perm) {
    if (allowed(p, perm)) return true;
    p.violations++;
    LOGW("plugin %s asked for %s without the permission (%d)", p.id.c_str(), method.c_str(), p.violations);
    reply(p, n, std::string("err permission ") + perm);
    if (p.violations >= 3) {
      p.enabled = false;
      p.detail = std::string("выключен: просил ") + perm + " без разрешения";
      save(p);
      halt(p);
      if (state_changed) state_changed(p.id + "\toff\t" + p.detail);
    }
    return false;
  };
  if (method == "log") {
    LOGI("plugin %s: %.200s", p.id.c_str(), args.c_str());
    reply(p, n, "ok");
  } else if (method == "subscribe") {
    if (!need("events.subscribe")) return;
    if (args == "button" || args == "media" || args == "volume") p.subscribed.push_back(args);
    reply(p, n, "ok");
  } else if (method == "button") {
    if (!need("assistant.intercept")) return;
    if (args != "claim" && args != "release") return reply(p, n, "err arguments");
    if (args == "claim") {
      p.claims_button = true;
      if (button_claim) button_claim(true);
    } else {
      release_button(p);
    }
    reply(p, n, "ok");
  } else if (method == "led") {
    if (!need("device.led")) return;
    int r = 0, g = 0, b = 0, sec = 3;
    if (sscanf(args.c_str(), "%d %d %d %d", &r, &g, &b, &sec) < 3) return reply(p, n, "err arguments");
    if (led) led(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), std::clamp(sec, 1, 30));
    reply(p, n, "ok");
  } else if (method == "display") {
    if (!need("device.display")) return;
    int sec = 3, off = 0;
    if (sscanf(args.c_str(), "%d %n", &sec, &off) < 1 || off <= 0) return reply(p, n, "err arguments");
    if (display) display(args.substr(size_t(off), 12), std::clamp(sec, 1, 30));
    reply(p, n, "ok");
  } else if (method == "segments") {                               // "<ttl ms> <56 hex digits>": the levels of the 28 segments of the clock
    if (!need("device.display")) return;
    int ttl = 0;
    char hex[80] = {0};
    if (sscanf(args.c_str(), "%d %79s", &ttl, hex) < 1 || ttl < 0 || ttl > 3600000) return reply(p, n, "err arguments");
    uint8_t lv[28] = {0};
    if (ttl > 0) {
      if (strlen(hex) != 56) return reply(p, n, "err arguments");
      for (int i = 0; i < 28; i++) {
        unsigned v = 0;
        if (sscanf(std::string(hex + i * 2, 2).c_str(), "%2x", &v) != 1) return reply(p, n, "err arguments");
        lv[i] = uint8_t(v);
      }
    }
    if (clock_segments) clock_segments(lv, ttl);
    reply(p, n, "ok");
  } else if (method == "media") {
    if (!need("media.control")) return;
    int pct = 0;
    if (args.rfind("volume ", 0) == 0 && sscanf(args.c_str() + 7, "%d", &pct) == 1) {   // "volume 0..100"
      if (set_volume) set_volume(std::clamp(pct, 0, 100));
      return reply(p, n, "ok");
    }
    if (args != "play" && args != "pause" && args != "playpause" && args != "next" && args != "previous") return reply(p, n, "err arguments");
    if (media) media(args);
    reply(p, n, "ok");
  } else if (method == "volume") {
    if (!need("device.read")) return;
    reply(p, n, "ok " + std::to_string(volume ? volume() : 0.f));
  } else if (method == "device") {                                // what the speaker is: nothing secret, no permission
    reply(p, n, "ok " + (device_info ? device_info() : std::string()) + " plugin=" + p.id);
  } else if (method == "get") {
    if (!need("storage")) return;
    auto it = p.kv.find(args);
    reply(p, n, it == p.kv.end() ? "ok" : "ok " + it->second);
  } else if (method == "set") {
    if (!need("storage")) return;
    const size_t sp = args.find(' ');
    const std::string key = args.substr(0, sp), val = sp == std::string::npos ? "" : args.substr(sp + 1, kMaxValue);
    if (key.empty() || key.size() > 40 || (!p.kv.count(key) && p.kv.size() >= kMaxKeys)) return reply(p, n, "err storage full");
    p.kv[key] = val;
    save(p);
    reply(p, n, "ok");
  } else if (method == "resolve") {
    if (!need("network.connect")) return;
    sockaddr_in a{};
    if (!resolve_ipv4(args, &a)) return reply(p, n, "err no such host");
    char ip[32];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    reply(p, n, std::string("ok ") + ip);
  } else if (method == "connect") {
    if (!need("network.connect")) return;
    char host[128] = {0};
    unsigned port = 0;
    if (sscanf(args.c_str(), "%127s %u", host, &port) != 2 || port == 0 || port > 65535) return reply(p, n, "err arguments");
    sockaddr_in a{};
    if (!resolve_ipv4(host, &a)) return reply(p, n, "err no such host");
    if (is_local_address(a)) {
      LOGW("plugin %s tried to reach the speaker itself (%s)", p.id.c_str(), host);
      return reply(p, n, "err forbidden address");                   // adb, the API, the plugins: never
    }
    a.sin_port = htons(uint16_t(port));
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    timeval tv{4, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (s < 0 || connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
      if (s >= 0) close(s);
      return reply(p, n, "err connect failed");
    }
    reply(p, n, "ok", s);
  } else if (method == "listen") {
    if (!need("network.listen")) return;
    const unsigned port = unsigned(atoi(args.c_str()));
    if (port < 1024 || port > 65535 || port == 5555 || port == 5037 || port == 6053 || port == 7000 || port == 49153 || port == 8765) return reply(p, n, "err forbidden port");
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
      if (s >= 0) close(s);
      return reply(p, n, "err cannot listen");
    }
    reply(p, n, "ok", s);
  } else if (method == "udp") {
    if (!need("network.listen")) return;
    unsigned port = 0;
    char group[40] = {0};
    const int got = sscanf(args.c_str(), "%u %39s", &port, group);
    if (got < 1 || port < 1024 || port > 65535 || port == 5555 || port == 6053 || port == 7000) return reply(p, n, "err forbidden port");
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bool ok = s >= 0 && bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0;
    if (ok && got == 2) {
      ip_mreq mr{};
      ok = inet_pton(AF_INET, group, &mr.imr_multiaddr) == 1 && (ntohl(mr.imr_multiaddr.s_addr) >> 28) == 0xE;   // 224.0.0.0/4 only
      mr.imr_interface.s_addr = htonl(INADDR_ANY);
      ok = ok && setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0;
    }
    if (!ok) {
      if (s >= 0) close(s);
      return reply(p, n, "err cannot open udp");
    }
    reply(p, n, "ok", s);
  } else if (method == "audio.open") {
    if (!need("audio.play")) return;
    audio_close(p);
    int pf[2];
    if (pipe(pf) != 0) return reply(p, n, "err pipe");
    p.aplay = fork();
    if (p.aplay == 0) {                                              // the player: reads the sound of the plugin from the pipe
      dup2(pf[0], 0);
      for (int fd = 3; fd < 1024; fd++) close(fd);
      const int nul = open("/dev/null", O_RDWR);
      dup2(nul, 1), dup2(nul, 2);
      execl("/usr/bin/aplay", "aplay", "-q", "-D", "speakers", "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2", static_cast<char *>(nullptr));
      _exit(1);
    }
    close(pf[0]);
    if (p.aplay < 0) {
      close(pf[1]);
      return reply(p, n, "err player");
    }
    if (audio_state) audio_state(true);
    reply(p, n, "ok", pf[1]);
  } else if (method == "audio.close") {
    if (!need("audio.play")) return;
    audio_close(p);
    reply(p, n, "ok");
  } else if (method == "http") {
    if (!need("network.connect")) return;
    http(p, n, args);
  } else if (method == "play") {
    if (!need("audio.play")) return;
    std::map<std::string, std::string> o;
    if (!read_object(args, o) || (o["url"].rfind("http://", 0) != 0 && o["url"].rfind("https://", 0) != 0)) return reply(p, n, "err arguments");
    if (play_url) play_url(p.id, o["url"], o["title"], o["cover"].rfind("http", 0) == 0 ? o["cover"] : std::string(), o["source"]);
    p.plays = true;
    LOGI("plugin %s plays %s", p.id.c_str(), o["url"].c_str());
    reply(p, n, "ok");
  } else if (method == "playfile") {                              // a WAV from the plugin's own folder (the sandbox cannot open it, the firmware can)
    if (!need("audio.play")) return;
    const std::string rel = args;
    bool safe = !rel.empty() && rel.size() < 120 && rel[0] != '/' && rel.find("..") == std::string::npos && rel.find('\n') == std::string::npos;
    const std::string path = dir(p.id) + "/" + rel;
    std::ifstream f(path, std::ios::binary);
    char hdr[12] = {0};
    if (safe && f) f.read(hdr, 12);
    struct stat st{};
    if (!safe || !f || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0 || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (64 << 20))
      return reply(p, n, "err нужен WAV внутри папки плагина (до 64 МБ)");
    if (play_url) play_url(p.id, "file://" + path, rel, std::string(), "");
    p.plays = true;
    LOGI("plugin %s plays file %s", p.id.c_str(), rel.c_str());
    reply(p, n, "ok");
  } else if (method == "stop") {
    if (!need("audio.play")) return;
    if (p.plays && stop_url) stop_url();
    p.plays = false;
    reply(p, n, "ok");
  } else if (method == "tool") {
    if (!need("assistant.tools")) return;
    std::map<std::string, std::string> o;
    if (!read_object(args, o) || o["name"].empty() || o["name"].size() > 40 || args.size() > 4096 || args.find('\t') != std::string::npos) return reply(p, n, "err arguments");
    if (!p.tools.count(o["name"]) && int(p.tools.size()) >= kMaxTools) return reply(p, n, "err too many commands");
    p.tools[o["name"]] = args;
    LOGI("plugin %s: command %s for the assistant", p.id.c_str(), o["name"].c_str());
    reply(p, n, "ok");
    if (state_changed) state_changed(p.id + "\ttools\t");
  } else if (method == "toolresult") {
    if (!need("assistant.tools")) return;
    std::map<std::string, std::string> o;
    if (!read_object(args, o) || o["id"].empty() || o["id"].size() > 40) return reply(p, n, "err arguments");
    const int64_t t = mono_ms();
    results_.erase(std::remove_if(results_.begin(), results_.end(), [t](const Result &r) { return t - r.at > kResultKeepMs; }), results_.end());
    results_.push_back({p.id, o["id"], json_quote(o["result"].substr(0, 12000)), t});   // a page of a plugin for the panel (panel_ui) takes a few KB
    reply(p, n, "ok");
    if (state_changed) state_changed(p.id + "\tresult\t");
    const std::string id = p.id;
    loop_.add_timer(int(kResultKeepMs) + 500, [this, id] { if (state_changed) state_changed(id + "\tresult\t"); });   // drops it from the status
  } else if (method == "publish") {
    if (!need("events.publish")) return;
    const size_t sp = args.find(' ');
    if (publish) publish(args.substr(0, std::min<size_t>(sp, 40)), sp == std::string::npos ? "" : args.substr(sp + 1, 160));
    reply(p, n, "ok");
  } else {
    reply(p, n, "err unknown method");
  }
}

// An HTTP(S) request of a plugin, made by the firmware in a thread of its own; the answer comes back on the loop.
void PluginHost::http(Plugin &p, const std::string &n, const std::string &args) {
  std::map<std::string, std::string> o;
  std::vector<std::string> headers;
  if (!read_object(args, o, &headers)) return reply(p, n, "err arguments");
  const std::string method = o["method"].empty() ? "GET" : o["method"], url = o["url"];
  if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE" && method != "HEAD") return reply(p, n, "err method");
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos || (url.compare(0, scheme, "http") != 0 && url.compare(0, scheme, "https") != 0)) return reply(p, n, "err url");
  std::string host = url.substr(scheme + 3);
  host = host.substr(0, host.find_first_of("/?#"));
  if (host.find('@') != std::string::npos) return reply(p, n, "err url");
  if (host.front() != '[') host = host.substr(0, host.find(':'));
  sockaddr_in a{};
  if (!resolve_ipv4(host, &a)) return reply(p, n, "err no such host");
  if (is_local_address(a)) {
    LOGW("plugin %s tried to reach the speaker itself over http (%s)", p.id.c_str(), host.c_str());
    return reply(p, n, "err forbidden address");
  }
  for (const std::string &h : headers)
    if (h.find('\r') != std::string::npos || h.find('\n') != std::string::npos) return reply(p, n, "err header");
  if (p.http_busy >= kMaxHttp) return reply(p, n, "err too many requests at once");
  p.http_busy++;
  const std::string id = p.id;
  const unsigned gen = p.gen;
  std::thread([this, id, gen, n, method, url, headers, body = o["body"]] {
    const HttpsResponse r = https_request(method, url, headers, body, kMaxBody);
    const std::string res = r.status ? "ok {\"status\":" + std::to_string(r.status) + ",\"type\":" + json_quote(r.content_type) + ",\"body\":" + json_quote(r.body) + "}"
                                     : "err " + (r.error.empty() ? std::string("no answer") : r.error.substr(0, 200));
    loop_.post([this, id, gen, n, res] {
      auto it = plugins_.find(id);
      if (it == plugins_.end() || it->second.gen != gen || it->second.proc.fd < 0) return;   // the plugin was stopped meanwhile
      it->second.http_busy--;
      reply(it->second, n, res);
    });
  }).detach();
}

// The voice assistant calls a command of a plugin: "! tool {"id","name","args"}" to it; the answer comes as "toolresult".
void PluginHost::call(const std::string &id, const std::string &call_id, const std::string &b64) {
  auto it = plugins_.find(id);
  const std::string req = b64_decode(b64);
  std::map<std::string, std::string> o;
  if (it == plugins_.end() || it->second.proc.fd < 0 || !read_object(req, o) || !it->second.tools.count(o["name"]) || call_id.size() > 40) {
    results_.push_back({id, call_id, json_quote("Ошибка: плагин сейчас не работает"), mono_ms()});
    if (state_changed) state_changed(id + "\tresult\t");
    return;
  }
  const size_t brace = req.find('{');
  const std::string line = "! tool {\"id\":" + json_quote(call_id) + "," + req.substr(brace + 1) + "\n";
  if (line.find('\n') != line.size() - 1) return;                 // one line only
  (void)!send(it->second.proc.fd, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}

bool PluginHost::transport(const std::string &dir) {
  const std::string line = "! transport " + dir + "\n";
  for (auto &kv : plugins_) {
    Plugin &p = kv.second;
    if (p.proc.fd < 0 || !p.plays) continue;
    (void)!send(p.proc.fd, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    return true;
  }
  return false;
}

void PluginHost::event(const std::string &name, const std::string &data) {
  const std::string line = "! " + name + " " + data + "\n";
  for (auto &kv : plugins_) {
    Plugin &p = kv.second;
    if (p.proc.fd < 0 || !allowed(p, "events.subscribe")) continue;
    if (std::find(p.subscribed.begin(), p.subscribed.end(), name) != p.subscribed.end())
      (void)!send(p.proc.fd, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  }
}

namespace {

bool put_file(const std::string &path, const std::string &data, mode_t mode) {
  const std::string tmp = path + ".part";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n <= 0) {
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    off += size_t(n);
  }
  close(fd);
  chmod(tmp.c_str(), mode);
  return rename(tmp.c_str(), path.c_str()) == 0;
}

// What the shell script of install() used to do with wget: the program, its sha-256, the files for the signature check, the markers.
void download_package(std::string d, std::string url, std::string sha, std::string dl, std::string id, bool js) {
  unlink((d + "/ready").c_str());
  unlink((d + "/error").c_str());
  mkdir((d + "/root").c_str(), 0755);
  auto fail = [&](const char *why) { put_file(d + "/error", std::string(why) + "\n", 0644); };
  const HttpsResponse r = https_request("GET", url, {}, "", 64u << 20);
  if (r.status != 200 || r.body.empty()) return fail("не скачался");
  if (sha256_hex(r.body) != sha) return fail("контрольная сумма не сошлась");
  static const char *const kTrust[] = {"lostos-signature.json", "manifest.json"}, *const kList[] = {"developers.json", "developers.json.sig"};
  const std::string trust = d + "/trust";
  for (const char *f : kTrust) unlink((trust + "/" + f).c_str());
  for (const char *f : kList) unlink((trust + "/" + f).c_str());
  rmdir(trust.c_str());
  if (!dl.empty()) {
    mkdir(trust.c_str(), 0755);
    for (const char *f : kTrust) {
      const HttpsResponse t = https_request("GET", dl + "/" + id + "/trust/" + f, {}, "", 1u << 20);
      if (t.status == 200) put_file(trust + "/" + f, t.body, 0644);
    }
    for (const char *f : kList) {
      const HttpsResponse t = https_request("GET", dl + "/" + f, {}, "", 1u << 20);
      if (t.status == 200) put_file(trust + "/" + f, t.body, 0644);
    }
  }
  chmod(d.c_str(), 0755);
  chmod((d + "/root").c_str(), 0755);
  if (js) {
    if (!put_file(d + "/main.js", r.body, 0755)) return fail("не записался");
  } else {
    unlink((d + "/main.js").c_str());
    if (!put_file(d + "/root/plugin", r.body, 0755)) return fail("не записался");
  }
  put_file(d + "/ready", "", 0644);
}

}  // namespace

void PluginHost::install(const std::string &id, const std::string &url, const std::string &sha, const std::string &perms, const std::string &dl, bool js, bool bin64, const std::string &args_b64) {
  auto web = [](const std::string &u) { return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0; };
  if (!valid_id(id) || !web(url) || sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos || (!dl.empty() && !web(dl))) {
    LOGW("plugin install refused: bad arguments");
    return;
  }
  Plugin p;
  p.id = id;
  p.name = id;
  p.js = js;
  p.bin64 = bin64;
  for (const std::string &g : split(perms, ','))
    for (const char *k : kKnownPermissions)
      if (g == k) p.asked.push_back(g);
  p.sha = sha;
  p.granted = allowed_for(p, p.asked);   // until the package has been looked at only the harmless ones
  unsigned top = 40000;
  for (const auto &kv : plugins_) top = std::max(top, kv.second.uid);
  p.uid = plugins_.count(id) ? plugins_[id].uid : top + 1;
  if (plugins_.count(id)) halt(plugins_[id]);
  mkdir(kRoot, 0755);
  mkdir(dir(id).c_str(), 0755);
  if (bin64) put_file(dir(id) + "/bin64", "", 0644);
  if (bin64) put_file(dir(id) + "/args", args_b64.empty() ? std::string() : b64_decode(args_b64), 0644);   // [launch] args of the package, one per line
  else unlink((dir(id) + "/bin64").c_str());
  plugins_[id] = p;
  save(p);
  // The program is fetched by a thread of ours (https_request: http:// and https://), checked against the sha-256 that the server gave, and put where the old shell script put it;
  // the poll below watches the same marker files "ready" and "error".
  std::thread(download_package, dir(id), url, sha, dl, id, js).detach();
  if (state_changed) state_changed(id + "\tdownloading\t");
  auto tries = std::make_shared<int>(0);
  auto poll = std::make_shared<std::function<void()>>();
  *poll = [this, id, tries, poll]() {
    const std::string d = dir(id);
    if (access((d + "/ready").c_str(), F_OK) == 0) {
      Plugin &q = plugins_[id];
      trust(q);
      q.enabled = true;
      save(q);
      run(q);
      *poll = nullptr;
      return;
    }
    if (access((d + "/error").c_str(), F_OK) == 0 || ++*tries > 120) {
      std::string why = "не установился";
      std::ifstream e(d + "/error");
      std::getline(e, why);
      plugins_[id].detail = why;
      if (state_changed) state_changed(id + "\terror\t" + why);
      *poll = nullptr;
      return;
    }
    loop_.add_timer(500, *poll);
  };
  loop_.add_timer(500, *poll);
}

void PluginHost::command(const std::string &line) {
  const std::vector<std::string> w = words(line);
  if (w.empty()) return;
  if (w.size() >= 2 && (w[0] == "enable" || w[0] == "disable") && system_.owns(w[1])) {
    system_.set_enabled(w[1], w[0] == "enable");   // no permissions to grant: the system plugins are the system
    if (state_changed) state_changed(w[1] + "\t" + (w[0] == "enable" ? "running" : "off") + "\t");
  } else if (w[0] == "install" && w.size() >= 4) {
    install(w[1], w[2], w[3], w.size() > 4 && w[4] != "-" ? w[4] : "", w.size() > 5 && w[5] != "-" ? w[5] : "", w.size() > 6 && w[6] == "js", w.size() > 6 && w[6] == "bin64", w.size() > 7 ? w[7] : std::string());

  } else if (w[0] == "call" && w.size() >= 4) {
    call(w[1], w[2], w[3]);
  } else if (w[0] == "remove" && w.size() >= 2 && valid_id(w[1])) {
    auto it = plugins_.find(w[1]);
    if (it != plugins_.end()) {
      halt(it->second);
      plugins_.erase(it);
    }
    const std::string d = dir(w[1]);
    if (fork() == 0) {
      execl("/bin/rm", "rm", "-rf", d.c_str(), static_cast<char *>(nullptr));
      _exit(0);
    }
    if (state_changed) state_changed(w[1] + "\tremoved\t");
  } else if ((w[0] == "enable" || w[0] == "disable") && w.size() >= 2 && plugins_.count(w[1])) {
    Plugin &p = plugins_[w[1]];
    if (w[0] == "enable") {
      p.asked = split(w.size() > 2 && w[2] != "-" ? w[2] : "", ',');
      p.granted = allowed_for(p, p.asked);   // the verdict about the package was made when it was downloaded (trusted, declared): the server can not widen it
      p.enabled = true;
      p.violations = p.crashes = 0;
      save(p);
      run(p);
    } else {
      p.enabled = false;
      halt(p);
      save(p);
      if (state_changed) state_changed(p.id + "\toff\t");
    }
  }
}

std::string PluginHost::status() const {
  std::string o;
  for (const auto &kv : plugins_) {
    const Plugin &p = kv.second;
    o += p.id + "\t" + (p.proc.pid > 0 ? "running" : p.enabled ? "restarting" : "off") + "\t" + p.detail + "\n";
    if (p.proc.pid > 0)
      for (const auto &t : p.tools) o += p.id + "\ttool\t" + t.second + "\n";
  }
  const int64_t t = mono_ms();
  for (const Result &r : results_)
    if (t - r.at <= kResultKeepMs) o += r.plugin + "\tresult\t" + r.id + "\t" + r.json + "\n";
  return o + system_.plugin_lines();
}


// ---------------------------------------------------------------- the local API of programs of Luna (127.0.0.1, a token per plugin)
void PluginHost::start_api() {
  if (api_fd_ >= 0) return;
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(fd, 8) != 0 || getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) != 0) {
    close(fd);
    return;
  }
  api_fd_ = fd;
  api_port_ = ntohs(a.sin_port);
  loop_.add_fd(fd, EPOLLIN, [this](uint32_t) { api_accept(); });
  LOGI("plugin API for programs on 127.0.0.1:%u", api_port_);
}

void PluginHost::api_accept() {
  for (;;) {
    const int c = accept4(api_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return;
    if (api_conns_.size() >= 16) {
      close(c);
      continue;
    }
    api_conns_[c];
    loop_.add_fd(c, EPOLLIN, [this, c](uint32_t) { api_read(c); });
  }
}

void PluginHost::api_close(int fd) {
  loop_.del_fd(fd);
  close(fd);
  api_conns_.erase(fd);
}

void PluginHost::api_read(int fd) {
  auto it = api_conns_.find(fd);
  if (it == api_conns_.end()) return;
  std::string &b = it->second;
  char buf[2048];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) {
      b.append(buf, size_t(n));
      if (b.size() > 16384) return api_close(fd);
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EINTR)) return api_close(fd);
    break;
  }
  const size_t he = b.find("\r\n\r\n");
  if (he == std::string::npos) return;
  std::string head = b.substr(0, he), lower = head;
  for (char &ch : lower) ch = char(tolower((unsigned char)ch));
  size_t clen = 0;
  const size_t cl = lower.find("content-length:");
  if (cl != std::string::npos) clen = size_t(strtoul(head.c_str() + cl + 15, nullptr, 10));
  if (clen > 12000) return api_close(fd);
  if (b.size() < he + 4 + clen) return;                             // the body has not come whole yet
  std::string token;
  for (const char *h : {"x-yam-token:", "x-luna-token:"}) {
    const size_t k = lower.find(h);
    if (k == std::string::npos) continue;
    size_t from = k + strlen(h), to = head.find("\r\n", from);
    token = head.substr(from, to == std::string::npos ? std::string::npos : to - from);
    while (!token.empty() && token.front() == ' ') token.erase(token.begin());
    while (!token.empty() && (token.back() == ' ' || token.back() == '\r')) token.pop_back();
  }
  const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
  const std::string method = sp1 == std::string::npos ? "" : head.substr(0, sp1);
  std::string path = sp2 == std::string::npos ? "" : head.substr(sp1 + 1, sp2 - sp1 - 1);
  if (const size_t q = path.find('?'); q != std::string::npos) path.erase(q);
  int code = 200;
  const std::string body = api_handle(method, path, token, b.substr(he + 4, clen), &code);
  const char *text = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Bad Request";
  const std::string out = "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  send(fd, out.data(), out.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  api_close(fd);
}

std::string PluginHost::api_handle(const std::string &method, const std::string &path, const std::string &token, const std::string &body, int *code) {
  Plugin *pp = nullptr;
  if (!token.empty())
    for (auto &kv : plugins_)
      if (kv.second.bin64 && kv.second.proc.pid > 0 && kv.second.token == token) pp = &kv.second;
  if (!pp) {
    *code = 401;
    return "{\"error\":\"token\"}";
  }
  // what the speaker is, in the names of Luna ("yandexmini3"), and the Lite 2 counts its digits from 0
  std::string model = "bergamot", info = device_info ? device_info() : std::string();
  if (const size_t m = info.find("model="); m != std::string::npos) model = info.substr(m + 6, info.find(' ', m) - m - 6);
  static const char *const kNames[][2] = {{"bergamot", "yandexmini3"}, {"plum", "yandexplum"}, {"cucumber", "yandexmidi"}, {"station-mini2", "yandexmini_2"},
                                          {"station-micro", "yandexmicro"}, {"tulip-d1", "yandexstation"}};
  std::string luna = model;
  for (const auto &n : kNames)
    if (model == n[0]) luna = n[1];
  const bool clock = info.find("clock=1") != std::string::npos;
  if (method == "GET" && path == "/device") {
    std::string f = "\"audio.volume.alsa\",\"input.evdev\"";
    if (info.find("ring=0") == std::string::npos) f += ",\"led.ring.rgb\"";
    if (clock) f += ",\"display.clock.segment.v1\"";
    return "{\"model\":\"" + luna + "\",\"architecture\":\"aarch64\",\"features\":[" + f + "]}";
  }
  if (method == "POST" && path == "/clock") {
    if (!allowed(*pp, "device.display")) {
      *code = 403;
      return "{\"error\":\"permission\"}";
    }
    if (!clock) {
      *code = 400;
      return "{\"error\":\"no clock\"}";
    }
    const size_t off = body.find("\"off\"");
    if (off != std::string::npos && body.find("true", off) != std::string::npos) {
      uint8_t zero[28] = {0};
      if (clock_segments) clock_segments(zero, 0);
      return "{}";
    }
    const size_t k = body.find("\"segments\"");
    if (k != std::string::npos) {
      const int base = luna.find("plum") != std::string::npos ? 0 : 1;
      uint8_t lv[28] = {0};
      size_t o = body.find('{', k), e = o == std::string::npos ? o : body.find('}', o);
      for (size_t i = o; e != std::string::npos && i < e;) {
        const size_t q1 = body.find('"', i);
        if (q1 == std::string::npos || q1 >= e) break;
        const size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= e) break;
        const std::string name = body.substr(q1 + 1, q2 - q1 - 1);
        const size_t colon = body.find(':', q2);
        if (colon == std::string::npos || colon >= e) break;
        const long v = strtol(body.c_str() + colon + 1, nullptr, 10);
        i = body.find(',', colon) == std::string::npos ? e : body.find(',', colon) + 1;
        if (name.size() == 2 && toupper((unsigned char)name[0]) >= 'A' && toupper((unsigned char)name[0]) <= 'G' && name[1] >= '0' && name[1] <= '9') {
          const int d = (name[1] - '0') - base;
          if (d >= 0 && d < 4) lv[d * 7 + (toupper((unsigned char)name[0]) - 'A')] = uint8_t(std::clamp<long>(v, 0, 255));
        }
      }
      if (clock_segments) clock_segments(lv, 600);                 // a program sends the next frame within a fraction of a second; when it stops the clock comes back
      return "{}";
    }
    *code = 400;
    return "{\"error\":\"segments or off\"}";
  }
  *code = 404;
  return "{\"error\":\"not here\"}";
}

}  // namespace cs
