#include "plugins/host.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
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

#include "core/log.h"
#include "plugins/trust.h"

namespace cs {

namespace {

constexpr const char *kRoot = "/data/lostos/plugins";
constexpr size_t kMaxKeys = 32, kMaxValue = 256, kMaxLine = 1024;

const char *const kKnownPermissions[] = {"device.led", "device.display", "media.control", "device.read", "events.subscribe", "events.publish", "storage",
                                         "network.connect", "network.listen", "audio.play", "assistant.intercept"};

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
  return permissions_to_grant(known, t);
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
  if (!sandbox_spawn(dir(p.id) + "/root", p.uid, lim, &p.proc, &err)) {
    p.detail = "не запустился: " + err;
    state_changed(p.id + "\terror\t" + p.detail);
    return;
  }
  p.detail = "работает";
  p.inbuf.clear();
  const std::string id = p.id;
  loop_.add_fd(p.proc.fd, EPOLLIN, [this, id](uint32_t) { on_readable(id); });
  LOGI("plugin %s started (pid %d, uid %u)", id.c_str(), int(p.proc.pid), p.uid);
  if (state_changed) state_changed(p.id + "\trunning\t");
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
  (void)!sendmsg(p.proc.fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
  if (fd >= 0) close(fd);                                          // the plugin has its own copy now
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
  audio_close(p);
  release_button(p);
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
  } else if (method == "media") {
    if (!need("media.control")) return;
    if (args != "play" && args != "pause" && args != "playpause" && args != "next" && args != "previous") return reply(p, n, "err arguments");
    if (media) media(args);
    reply(p, n, "ok");
  } else if (method == "volume") {
    if (!need("device.read")) return;
    reply(p, n, "ok " + std::to_string(volume ? volume() : 0.f));
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
  } else if (method == "publish") {
    if (!need("events.publish")) return;
    const size_t sp = args.find(' ');
    if (publish) publish(args.substr(0, std::min<size_t>(sp, 40)), sp == std::string::npos ? "" : args.substr(sp + 1, 160));
    reply(p, n, "ok");
  } else {
    reply(p, n, "err unknown method");
  }
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

void PluginHost::install(const std::string &id, const std::string &url, const std::string &sha, const std::string &perms, const std::string &dl) {
  if (!valid_id(id) || url.rfind("http://", 0) != 0 || sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos ||
      (!dl.empty() && dl.rfind("http://", 0) != 0)) {
    LOGW("plugin install refused: bad arguments");
    return;
  }
  Plugin p;
  p.id = id;
  p.name = id;
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
  plugins_[id] = p;
  save(p);
  pid_t pid = fork();
  if (pid == 0) {
    if (fork() == 0) {
      setsid();
      for (int fd = 3; fd < 1024; fd++) close(fd);
      const int nul = open("/dev/null", O_RDWR);
      dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
      const char *script =
          "d=$1; rm -f \"$d/ready\" \"$d/error\"; mkdir -p \"$d/root\" && wget -q -T 30 -O \"$d/root/plugin.tmp\" \"$2\" || { echo 'не скачался' > \"$d/error\"; exit 1; };"
          "got=$(sha256sum \"$d/root/plugin.tmp\" | cut -d' ' -f1); [ \"$got\" = \"$3\" ] || { rm -f \"$d/root/plugin.tmp\"; echo 'контрольная сумма не сошлась' > \"$d/error\"; exit 1; };"
          "rm -rf \"$d/trust\"; if [ -n \"$4\" ]; then mkdir -p \"$d/trust\"; "
          "for f in lostos-signature.json manifest.json; do wget -q -T 20 -O \"$d/trust/$f\" \"$4/$5/trust/$f\" || rm -f \"$d/trust/$f\"; done; "
          "for f in developers.json developers.json.sig; do wget -q -T 20 -O \"$d/trust/$f\" \"$4/$f\" || rm -f \"$d/trust/$f\"; done; fi; "
          "chmod 755 \"$d\" \"$d/root\" \"$d/root/plugin.tmp\"; mv \"$d/root/plugin.tmp\" \"$d/root/plugin\"; : > \"$d/ready\"";
      execl("/bin/sh", "sh", "-c", script, "sh", dir(id).c_str(), url.c_str(), sha.c_str(), dl.c_str(), id.c_str(), static_cast<char *>(nullptr));
    }
    _exit(0);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
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
    install(w[1], w[2], w[3], w.size() > 4 && w[4] != "-" ? w[4] : "", w.size() > 5 ? w[5] : "");
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
  }
  return o + system_.plugin_lines();
}

}  // namespace cs
