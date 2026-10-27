// Sandbox for native plugins of the speaker.
//
// A plugin is a static ELF (armhf, built with musl, e.g. `zig cc -target arm-linux-musleabihf -static`) that runs as its own process:
//   * chroot into its own folder: it sees nothing but its binary (no /dev, /proc, /etc, other plugins, the firmware);
//   * its own unprivileged user id, no capabilities, no_new_privs, no supplementary groups;
//   * limits on memory (address space), CPU time, open files, processes, file size;
//   * a seccomp filter that kills it on any system call outside a short allow-list: it cannot open a file, open a socket, mount,
//     ptrace, create processes or threads. What is left is read/write on the descriptors it was given, memory, time, sleeping.
//   * one descriptor (fd 3, a unix socketpair) to the firmware: this is its whole world. Everything the plugin can do to the
//     speaker (LEDs, display, music, storage, network) goes through that descriptor as a request the firmware checks against the
//     permissions the user granted.
// The kernel of the speaker has seccomp filters and chroot, but no namespaces and no memory cgroup: the limits are rlimits.
#pragma once

#include <string>
#include <sys/types.h>

namespace cs {

struct SandboxLimits {
  size_t address_space = 48u << 20;   // bytes of virtual memory (a static program needs a few MB)
  unsigned cpu_seconds = 3600;        // CPU time in total; the plugin is killed with SIGXCPU when it is spent
  unsigned open_files = 32;
  unsigned file_size = 1u << 20;
};

struct SandboxProc {
  pid_t pid = -1;
  int fd = -1;                        // the firmware's end of the socketpair
};

// Starts `<root>/plugin` in the sandbox as user id `uid`. root must hold the program, readable and executable by uid.
bool sandbox_spawn(const std::string &root, unsigned uid, const SandboxLimits &limits, SandboxProc *out, std::string *error);

// "killed by a forbidden system call", "out of CPU time", ... for a wait status; empty when the exit was clean.
std::string sandbox_describe_exit(int wait_status);

}  // namespace cs
