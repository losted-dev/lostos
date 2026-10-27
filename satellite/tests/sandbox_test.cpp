// Runs on the speaker (root): starts the test plugin in the sandbox for each hostile or friendly command and prints what happened.
//   ./sandbox_test /tmp/sbx/root      (the folder holds the static program "plugin" built from tests/sandbox_plugin.c)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "plugins/sandbox.h"

using namespace cs;

static int failures = 0;

static std::string run(const std::string &root, const std::string &cmd, int *status, unsigned cpu = 3600) {
  SandboxLimits lim;
  lim.cpu_seconds = cpu;
  SandboxProc p;
  std::string err;
  if (!sandbox_spawn(root, 40001, lim, &p, &err)) return "spawn failed: " + err;
  (void)!write(p.fd, (cmd + "\n").c_str(), cmd.size() + 1);
  std::string reply;
  for (int i = 0; i < 40; i++) {                              // up to 4 s
    pollfd pf{p.fd, POLLIN, 0};
    if (poll(&pf, 1, 100) > 0) {
      char b[200];
      const ssize_t n = read(p.fd, b, sizeof b);
      if (n > 0) reply.append(b, size_t(n));
      if (n <= 0) break;
      if (!reply.empty()) break;
    }
    int st;
    if (waitpid(p.pid, &st, WNOHANG) == p.pid) {
      *status = st;
      close(p.fd);
      return reply;
    }
  }
  kill(p.pid, SIGKILL);
  waitpid(p.pid, status, 0);
  close(p.fd);
  return reply;
}

static const char *only = nullptr;   // run only the tests whose name contains this text

static void expect(const char *what, const std::string &root, const std::string &cmd, const char *reply_has, const char *died_of, unsigned cpu = 3600) {
  if (only && !strstr(what, only)) return;
  int st = 0;
  const std::string r = run(root, cmd, &st, cpu);
  std::string why;
  // the process may still be alive after a reply: reap it
  if (!WIFSIGNALED(st) && !WIFEXITED(st)) st = 0;
  why = sandbox_describe_exit(st);
  const bool ok = (reply_has && r.find(reply_has) != std::string::npos) || (died_of && why.find(died_of) != std::string::npos);
  printf("%s %-28s reply='%s' exit='%s'\n", ok ? "ok  " : "FAIL", what, r.c_str(), why.c_str());
  if (!ok) failures++;
}

int main(int argc, char **argv) {
  const std::string root = argc > 1 ? argv[1] : "/tmp/sbx/root";
  if (argc > 2) only = argv[2];
  expect("friendly: hello + uid", root, "hello", "uid=40001", nullptr);
  expect("limits as seen by the plugin", root, "limits", "as=50331648", nullptr);
  expect("mmap sizes", root, "mmap", "MB:", nullptr);
  expect("hostile: open /dev/fb0", root, "open", nullptr, "forbidden system call");
  expect("hostile: socket()", root, "socket", nullptr, "forbidden system call");
  expect("hostile: fork()", root, "fork", nullptr, "forbidden system call");
  expect("hostile: too much memory", root, "mem", "mem refused", nullptr);
  expect("hostile: CPU burner (1 s)", root, "spin", nullptr, "killed", 1);   // soft = hard limit: the kernel sends SIGKILL
  printf(failures ? "%d FAILED\n" : "all sandbox tests passed\n", failures);
  return failures ? 1 : 0;
}
