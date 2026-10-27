#include "plugins/system.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <limits.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"

extern char **environ;

namespace cs {

namespace {

constexpr int kTickMs = 1000;
constexpr int kStopGraceMs = 2000;     // after SIGTERM, then SIGKILL
constexpr int kBackoffMaxMs = 60000;
constexpr int64_t kStableMs = 60000;   // a program that ran this long did not "crash in a loop"
constexpr const char *kLogDir = "/tmp/lostos-system";

std::string read_link(const std::string &path) {
  char buf[512];
  ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::string read_cmdline(pid_t pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::replace(s.begin(), s.end(), '\0', ' ');
  return s;
}

// A program of ours that runs already (started by an earlier lostos, or by hand): the one with the same executable.
pid_t find_running(const SysProc &p) {
  // /data is a mount of another path (/bkupgrade/data): /proc/<pid>/exe shows the real one
  char real[PATH_MAX];
  const std::string want = realpath(p.exe.c_str(), real) ? std::string(real) : p.exe;
  pid_t found = 0;
  DIR *d = opendir("/proc");
  if (!d) return 0;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const pid_t pid = pid_t(atoi(e->d_name));
    if (pid == getpid()) continue;
    if (read_link("/proc/" + std::string(e->d_name) + "/exe") != want) continue;
    if (!p.match.empty() && read_cmdline(pid).find(p.match) == std::string::npos) continue;
    found = pid;
    break;
  }
  closedir(d);
  return found;
}

bool alive(pid_t pid) { return pid > 0 && kill(pid, 0) == 0; }

// Starts the program detached from lostos: its own session, no inherited descriptors, the signals unblocked
// (lostos blocks SIGTERM for its signalfd and the mask would be inherited).
pid_t spawn(const SysProc &p, const std::string &dir) {
  std::vector<std::string> envs;
  for (char **e = environ; e && *e; e++) {
    bool replaced = false;
    for (const auto &kv : p.env)
      if (!strncmp(*e, kv.first.c_str(), kv.first.size()) && (*e)[kv.first.size()] == '=') replaced = true;
    if (!replaced) envs.push_back(*e);
  }
  for (const auto &kv : p.env) envs.push_back(kv.first + "=" + kv.second);
  std::vector<char *> envp, argv;
  for (auto &s : envs) envp.push_back(const_cast<char *>(s.c_str()));
  envp.push_back(nullptr);
  argv.push_back(const_cast<char *>(p.exe.c_str()));
  for (auto &a : p.args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  mkdir(kLogDir, 0755);
  const std::string log = std::string(kLogDir) + "/" + p.name + ".log";

  const pid_t pid = fork();
  if (pid != 0) return pid;
  setsid();
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  for (int fd = 3; fd < 1024; fd++) close(fd);
  const int nul = open("/dev/null", O_RDONLY);
  const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (nul >= 0) dup2(nul, 0);
  if (out >= 0) {
    dup2(out, 1);
    dup2(out, 2);
  }
  if (!dir.empty() && chdir(dir.c_str())) {}
  execve(p.exe.c_str(), argv.data(), envp.data());
  _exit(127);
}

}  // namespace

SystemPlugins::SystemPlugins(Loop &loop, const Model &model) : loop_(loop), model_(model) {
  add_audio_core(*this, mods_);
  add_airplay_plugin(*this, mods_);
  add_spotify_plugin(*this, mods_);
}

SystemPlugins::~SystemPlugins() {
  if (sock_ >= 0) {
    loop_.del_fd(sock_);
    close(sock_);
    unlink(kSystemSocket);
  }
}

SysPlugin *SystemPlugins::find(const std::string &id) {
  for (auto &m : mods_)
    if (m.id == id) return &m;
  return nullptr;
}
const SysPlugin *SystemPlugins::find(const std::string &id) const { return const_cast<SystemPlugins *>(this)->find(id); }

bool SystemPlugins::installed(const std::string &id) const {
  const SysPlugin *m = find(id);
  return m && m->usable && m->usable();
}

SysLevel SystemPlugins::level(const std::string &id) const {
  const SysPlugin *m = find(id);
  return m ? m->level : SysLevel::OFF;
}

// "enabled=0" in plugin.conf of the plugin switches it off for good (no line: on).
bool SystemPlugins::enabled(const std::string &id) const {
  const SysPlugin *m = find(id);
  if (!m) return false;
  if (m->core) return true;
  std::ifstream f(m->dir + "/plugin.conf");
  for (std::string line; std::getline(f, line);)
    if (line == "enabled=0") return false;
  return true;
}

void SystemPlugins::begin(SysPlugin &m, SysLevel level) {
  if (m.level != SysLevel::OFF) {  // already running: only the level changes
    m.level = level;
    return;
  }
  if (!m.usable || !m.usable()) return;
  if (m.prepare && !m.prepare()) {
    LOGW("plugin %s: cannot prepare, not started", m.id.c_str());
    return;
  }
  m.procs = m.plan ? m.plan() : std::vector<SysProc>();
  m.begin_ms = Loop::now_ms();
  for (auto &p : m.procs) p.due_ms = m.begin_ms + p.delay_ms;
  m.level = level;
  LOGI("plugin %s: started (%zu programs)", m.id.c_str(), m.procs.size());
}

void SystemPlugins::stop_proc(SysProc &p) {
  if (p.pid <= 0) return;
  const pid_t pid = p.pid;
  kill(pid, SIGTERM);
  loop_.add_timer(kStopGraceMs, [this, pid] {
    if (alive(pid)) kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, WNOHANG);
  });
  p.pid = 0;
  p.child = false;
}

void SystemPlugins::halt(SysPlugin &m, bool keep_helpers) {
  if (m.level == SysLevel::OFF) return;
  for (auto &p : m.procs) {
    if (keep_helpers && !p.primary) continue;
    stop_proc(p);
  }
  if (keep_helpers) {
    m.level = SysLevel::HELPERS;
  } else {
    m.level = SysLevel::OFF;
    m.procs.clear();
  }
  LOGI("plugin %s: %s", m.id.c_str(), keep_helpers ? "primary programs stopped" : "stopped");
  if (!keep_helpers && on_stopped) on_stopped(m.id);
}

bool SystemPlugins::set_running(const std::string &id, bool on, bool keep_helpers) {
  SysPlugin *m = find(id);
  if (!m || !m->usable || !m->usable()) return false;
  if (on && !enabled(id)) return false;   // switched off by the owner: only the owner's switch starts it again
  if (on) {
    if (m->level == SysLevel::HELPERS) {  // the primary programs come back, the helpers never stopped
      m->level = SysLevel::ALL;
      for (auto &p : m->procs)
        if (p.primary) p.due_ms = Loop::now_ms(), p.fails = 0;
    } else {
      begin(*m, SysLevel::ALL);
    }
  } else {
    halt(*m, keep_helpers);
  }
  return true;
}

bool SystemPlugins::set_enabled(const std::string &id, bool on) {
  SysPlugin *m = find(id);
  if (!m || m->core) return false;
  const std::string path = m->dir + "/plugin.conf";
  std::string conf;
  {
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);)
      if (line.rfind("enabled=", 0) != 0) conf += line + "\n";
  }
  conf += std::string("enabled=") + (on ? "1" : "0") + "\n";
  std::ofstream(path, std::ios::trunc) << conf;
  return set_running(id, on);
}

void SystemPlugins::start_audio() {
  if (SysPlugin *m = find("audio")) begin(*m, SysLevel::ALL);
  tick();
  if (!started_) {
    started_ = true;
    loop_.add_timer(kTickMs, [this] { tick(); }, kTickMs);
  }
}

void SystemPlugins::start() {
  for (auto &m : mods_)
    if (!m.core && enabled(m.id)) begin(m, SysLevel::ALL);
  tick();
  serve();
}

// Every second: a program that has gone is started again (with a growing pause when it keeps going),
// one that has not started yet because of its delay is started when the time comes.
void SystemPlugins::tick() {
  const int64_t now = Loop::now_ms();
  for (auto &m : mods_) {
    if (m.level == SysLevel::OFF) continue;
    for (auto &p : m.procs) {
      if (p.primary && m.level == SysLevel::HELPERS) continue;
      if (p.pid > 0) {
        bool gone;
        if (p.child) {
          int st = 0;
          gone = waitpid(p.pid, &st, WNOHANG) == p.pid;
        } else {
          gone = !alive(p.pid);
        }
        if (!gone) continue;
        LOGW("plugin %s: %s (pid %d) has gone", m.id.c_str(), p.name.c_str(), int(p.pid));
        if (now - p.started_ms >= kStableMs) p.fails = 0;
        p.pid = 0;
        p.child = false;
        p.fails++;
        const int back = std::min(kBackoffMaxMs, 1000 << std::min(p.fails, 6));
        p.due_ms = now + back;
        continue;
      }
      if (now < p.due_ms) continue;
      if (const pid_t run = find_running(p)) {
        p.pid = run;
        p.child = false;
        p.started_ms = now;
        LOGI("plugin %s: %s already runs (pid %d), taken over", m.id.c_str(), p.name.c_str(), int(run));
        continue;
      }
      if (access(p.exe.c_str(), X_OK) != 0) {
        p.due_ms = now + kBackoffMaxMs;
        continue;
      }
      p.pid = spawn(p, m.dir);
      p.child = p.pid > 0;
      p.started_ms = now;
      LOGI("plugin %s: %s started (pid %d)", m.id.c_str(), p.name.c_str(), int(p.pid));
    }
  }
}

// The lines of the plugin list ("id<TAB>state<TAB>detail"), the same as for the plugins in the sandbox.
std::string SystemPlugins::plugin_lines() const {
  std::string o;
  for (const auto &m : mods_) {
    if (m.core || !m.usable || !m.usable()) continue;
    const char *st = m.level == SysLevel::ALL ? "running" : m.level == SysLevel::HELPERS ? "paused" : "off";
    o += m.id + "\t" + st + "\t" + m.title + "\n";
  }
  return o;
}

std::string SystemPlugins::status() const {
  std::ostringstream o;
  for (const auto &m : mods_) {
    const char *lv = m.level == SysLevel::ALL ? "on" : m.level == SysLevel::HELPERS ? "paused" : "off";
    o << m.id << " (" << m.title << "): " << (m.usable && m.usable() ? lv : "not installed");
    if (!m.core && m.usable && m.usable() && !enabled(m.id)) o << ", switched off by the owner";
    o << "\n";
    for (const auto &p : m.procs) {
      o << "  " << p.name << ": ";
      if (p.pid > 0) o << "pid " << p.pid << (p.child ? "" : " (taken over)");
      else if (p.primary && m.level == SysLevel::HELPERS) o << "stopped";
      else o << "waits" << (p.fails ? ", " + std::to_string(p.fails) + " failures" : "");
      o << "\n";
    }
  }
  return o.str();
}

std::string SystemPlugins::command(const std::string &line) {
  std::istringstream in(line);
  std::string verb, id;
  in >> verb >> id;
  if (verb == "status") return status();
  if (!find(id)) return "unknown module '" + id + "'\n";
  if (verb == "start") return set_running(id, true) ? "started\n" : "cannot start (not installed?)\n";
  if (verb == "stop") return set_running(id, false) ? "stopped\n" : "cannot stop\n";
  if (verb == "restart") {
    set_running(id, false);
    loop_.add_timer(2500, [this, id] { set_running(id, true); });
    return "restarting\n";
  }
  if (verb == "enable") return set_enabled(id, true) ? "enabled\n" : "cannot enable\n";
  if (verb == "disable") return set_enabled(id, false) ? "disabled\n" : "cannot disable\n";
  return "commands: status | start|stop|restart|enable|disable <module>\n";
}

// ---- the control socket: "lostos --module status" and the like

void SystemPlugins::serve() {
  if (sock_ >= 0) return;
  sock_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (sock_ < 0) return;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, kSystemSocket, sizeof a.sun_path - 1);
  unlink(kSystemSocket);
  if (bind(sock_, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0 || listen(sock_, 4) < 0) {
    close(sock_);
    sock_ = -1;
    return;
  }
  chmod(kSystemSocket, 0600);
  loop_.add_fd(sock_, EPOLLIN, [this](uint32_t) {
    for (;;) {
      const int c = accept4(sock_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) return;
      timeval tv{0, 300000};   // a client that says nothing must not hold the loop
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      char buf[256];
      const ssize_t n = recv(c, buf, sizeof buf - 1, 0);
      if (n > 0) {
        std::string line(buf, size_t(n));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const std::string answer = command(line);
        if (write(c, answer.data(), answer.size()) < 0) {}
      }
      close(c);
    }
  });
}

int run_system_command(int argc, char **argv) {
  std::string line;
  for (int i = 0; i < argc; i++) line += (i ? " " : "") + std::string(argv[i]);
  if (line.empty()) line = "status";
  const int s = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, kSystemSocket, sizeof a.sun_path - 1);
  if (s < 0 || connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0) {
    fprintf(stderr, "lostos is not running\n");
    return 1;
  }
  if (write(s, line.data(), line.size()) < 0) return 1;
  char buf[2048];
  ssize_t n;
  while ((n = read(s, buf, sizeof buf)) > 0) fwrite(buf, 1, size_t(n), stdout);
  close(s);
  return 0;
}

}  // namespace cs
