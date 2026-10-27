#include "plugins/sandbox.h"

#include <fcntl.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <vector>

namespace cs {

namespace {

// The plugin is a 32-bit ARM program on a 64-bit kernel: its system calls arrive with the ARM EABI numbers.
// (The numbers are written out: the speaker's kernel headers differ from the ones of the build machine.)
constexpr unsigned kArchArm = 0x40000028;   // AUDIT_ARCH_ARM
const unsigned kAllowed[] = {
    1,    // exit
    3,    // read
    4,    // write
    6,    // close
    11,   // execve (only /plugin exists in the chroot; the filter is already in place when it runs)
    20,   // getpid
    45,   // brk
    54,   // ioctl (musl asks for the terminal of stdout)
    78,   // gettimeofday
    91,   // munmap
    125,  // mprotect
    162,  // nanosleep
    163,  // mremap
    168,  // poll
    173,  // rt_sigreturn
    174,  // rt_sigaction
    175,  // rt_sigprocmask
    186,  // sigaltstack
    191,  // ugetrlimit
    192,  // mmap2
    199,  // getuid32
    200,  // getgid32
    201,  // geteuid32
    202,  // getegid32
    220,  // madvise
    224,  // gettid
    240,  // futex
    248,  // exit_group
    256,  // set_tid_address
    263,  // clock_gettime
    264,  // clock_getres
    265,  // clock_nanosleep
    268,  // tgkill (abort())
    142,  // _newselect
    221,  // fcntl64
    251,  // epoll_ctl
    252,  // epoll_wait
    285,  // accept (on a listening socket the firmware handed over)
    286,  // getsockname
    287,  // getpeername
    289,  // send
    290,  // sendto
    291,  // recv
    292,  // recvfrom
    296,  // sendmsg
    293,  // shutdown
    294,  // setsockopt
    295,  // getsockopt
    297,  // recvmsg
    335,  // pselect6
    346,  // epoll_pwait
    356,  // eventfd2
    357,  // epoll_create1
    366,  // accept4
    336,  // ppoll
    369,  // prlimit64 (musl's getrlimit)
    338,  // set_robust_list
    384,  // getrandom
    983045,  // ARM_NR_set_tls (0xf0005): the thread pointer of a static musl program
};

bool install_filter() {
  std::vector<sock_filter> p;
  auto stmt = [&](unsigned short code, unsigned k) { p.push_back(BPF_STMT(code, k)); };
  stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
  p.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArchArm, 1, 0));
  stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL);                       // another architecture (a 64-bit program): never
  stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
  const size_t n = sizeof kAllowed / sizeof kAllowed[0];
  for (size_t i = 0; i < n; i++) {
    // jump over the rest of the list to the "allow" at the end
    p.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAllowed[i], uint8_t(n - i), 0));
  }
  stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL);                       // everything else
  stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  sock_fprog prog{static_cast<unsigned short>(p.size()), p.data()};
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}

void limit(int what, rlim_t v) {
  const rlimit r{v, v};
  setrlimit(what, &r);
}

}  // namespace

bool sandbox_spawn(const std::string &root, unsigned uid, const SandboxLimits &lim, SandboxProc *out, std::string *error) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
    if (error) *error = "socketpair";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    if (error) *error = "fork";
    return false;
  }
  if (pid == 0) {
    // ---- the child: lock everything, then become the plugin. On any failure it ends, it never runs half-locked.
    const int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0);
    dup2(nul, 1);
    dup2(nul, 2);
    dup2(sv[1], 3);                                            // the one way to the firmware
    for (int fd = 4; fd < 1024; fd++) close(fd);               // nothing else of the firmware stays open (sockets, devices, files)
    fcntl(3, F_SETFD, 0);
    if (chroot(root.c_str()) != 0 || chdir("/") != 0) _exit(120);
    limit(RLIMIT_AS, lim.address_space);
    limit(RLIMIT_CPU, lim.cpu_seconds);
    limit(RLIMIT_NOFILE, lim.open_files);
    limit(RLIMIT_FSIZE, lim.file_size);
    limit(RLIMIT_NPROC, 1);
    limit(RLIMIT_CORE, 0);
    if (setgroups(0, nullptr) != 0 || setresgid(uid, uid, uid) != 0 || setresuid(uid, uid, uid) != 0) _exit(121);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(122);
    if (!install_filter()) _exit(123);
    char *const argv[] = {const_cast<char *>("/plugin"), nullptr};
    char *const envp[] = {nullptr};
    execve("/plugin", argv, envp);
    _exit(124);
  }
  close(sv[1]);
  out->pid = pid;
  out->fd = sv[0];
  return true;
}

std::string sandbox_describe_exit(int st) {
  if (WIFSIGNALED(st)) {
    switch (WTERMSIG(st)) {
      case SIGSYS: return "a forbidden system call";
      case SIGXCPU: return "CPU time is spent";
      case SIGSEGV: return "memory error (maybe out of memory)";
      case SIGKILL: return "killed";
      default: return "signal " + std::to_string(WTERMSIG(st));
    }
  }
  if (WIFEXITED(st) && WEXITSTATUS(st) >= 120 && WEXITSTATUS(st) <= 124) return "the sandbox could not be built (step " + std::to_string(WEXITSTATUS(st) - 119) + ")";
  if (WIFEXITED(st) && WEXITSTATUS(st) != 0) return "exit code " + std::to_string(WEXITSTATUS(st));
  return "";
}

}  // namespace cs
