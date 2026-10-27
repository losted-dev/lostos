#include "bt/bluetooth.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

#include "core/log.h"

namespace cs {
namespace {
constexpr const char *kScript = "/data/lostos/bt.sh";
constexpr const char *kOffMarker = "/data/lostos/bt-off";

// bt.sh <args> in a detached child (double fork, no inherited descriptors, no zombie).
void run_detached(const std::vector<std::string> &args) {
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    if (fork() == 0) {
      setsid();
      for (int fd = 3; fd < 1024; fd++) close(fd);
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
      }
      std::vector<char *> argv;
      argv.push_back(const_cast<char *>(kScript));
      for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
      argv.push_back(nullptr);
      execv(kScript, argv.data());
    }
    _exit(0);
  }
  waitpid(pid, nullptr, 0);
}
}  // namespace

struct BtLink::Impl {
  Loop &loop;
  BtLink &owner;
  pid_t child = -1;
  int fd = -1;
  std::string buf;
  uint64_t retry = 0;
  Impl(Loop &l, BtLink &o) : loop(l), owner(o) {}

  void spawn() {
    int p[2];
    if (pipe(p) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
      close(p[0]);
      close(p[1]);
      return;
    }
    if (pid == 0) {
      dup2(p[1], 1);
      for (int f = 3; f < 1024; f++) close(f);
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) dup2(nul, 0), dup2(nul, 2);
      setenv("BT_PARENT", std::to_string(getppid()).c_str(), 1);   // the watcher ends when lostos does
      execl(kScript, kScript, "watch", static_cast<char *>(nullptr));
      _exit(1);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    child = pid;
    fd = p[0];
    loop.add_fd(fd, EPOLLIN, [this](uint32_t) { readable(); });
  }

  void readable() {
    char tmp[512];
    for (;;) {
      ssize_t n = ::read(fd, tmp, sizeof tmp);
      if (n > 0) {
        buf.append(tmp, size_t(n));
        continue;
      }
      if (n == 0) {  // the watcher is gone: start it again in a while
        loop.del_fd(fd);
        close(fd);
        fd = -1;
        if (child > 0) waitpid(child, nullptr, WNOHANG);
        child = -1;
        retry = loop.add_timer(5000, [this] {
          retry = 0;
          spawn();
        });
      }
      break;
    }
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      handle(buf.substr(0, nl));
      buf.erase(0, nl + 1);
    }
    if (buf.size() > 2048) buf.clear();
  }

  void handle(const std::string &line) {
    if (line.rfind("state ", 0) == 0) {  // "state <connected> <player status>"
      const bool c = line.size() > 6 && line[6] == '1';
      const bool p = line.find(" playing") != std::string::npos;
      LOGI("bluetooth: %s", line.c_str());
      const bool changed = c != owner.connected_ || p != owner.playing_;
      owner.connected_ = c;
      owner.playing_ = p;
      if (changed && owner.on_state) owner.on_state();
    } else if (line.rfind("pos ", 0) == 0) {   // "pos <position ms> <length ms>" of the track the phone plays
      unsigned long p = 0, d = 0;
      if (sscanf(line.c_str() + 4, "%lu %lu", &p, &d) == 2 && owner.on_position) owner.on_position(uint32_t(p), uint32_t(d));
    } else if (line.rfind("vol ", 0) == 0) {   // "vol <0..127>": the phone's slider
      const float v = std::strtof(line.c_str() + 4, nullptr) / 127.f;
      if (owner.on_volume) owner.on_volume(v < 0.f ? 0.f : v > 1.f ? 1.f : v);
    } else if (line.rfind("now", 0) == 0) {
      owner.now_ = line.size() > 4 ? line.substr(4) : "";
      if (owner.on_now) owner.on_now(owner.now_);
    }
  }
};

BtLink::BtLink(Loop &loop) : impl_(new Impl(loop, *this)) {}
BtLink::~BtLink() { delete impl_; }

bool BtLink::installed() const { return access(kScript, X_OK) == 0; }

void BtLink::start() {
  if (!installed()) return;
  enabled_ = access(kOffMarker, F_OK) != 0;
  if (!enabled_) run_detached({"off"});   // the stock stack powers Bluetooth on at boot: keep it off if it was switched off
  impl_->spawn();
}

void BtLink::set_enabled(bool on) {
  if (!installed()) return;
  enabled_ = on;
  if (!on) {
    connected_ = playing_ = false;
    now_.clear();
  }
  run_detached({on ? "on" : "off"});
  LOGI("bluetooth: %s", on ? "on (visible for pairing)" : "off");
  if (on_state) on_state();
}

void BtLink::set_volume(float v) {
  if (!installed() || !connected_) return;
  run_detached({"setvol", std::to_string(int(v * 127.f + 0.5f))});
}

void BtLink::command(const std::string &cmd) {
  if (!installed()) return;
  run_detached({cmd});
}

}  // namespace cs