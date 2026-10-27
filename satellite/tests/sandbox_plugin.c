/* The test plugin for tests/sandbox_test.cpp: does what the first line it reads from fd 3 tells it to, friendly or hostile.
 * Build: zig cc -target arm-linux-musleabihf -static -Os tests/sandbox_plugin.c -o plugin */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void say(const char *s) { (void)!write(3, s, strlen(s)); }

int main(void) {
  char cmd[64] = {0};
  if (read(3, cmd, sizeof cmd - 1) <= 0) return 1;
  if (!strncmp(cmd, "hello", 5)) {
    char b[64];
    snprintf(b, sizeof b, "hello uid=%d", (int)getuid());
    say(b);
  } else if (!strncmp(cmd, "open", 4)) {
    int fd = open("/dev/fb0", O_RDWR);          /* must be killed by seccomp before it returns */
    say(fd < 0 ? "open refused" : "OPENED (bad)");
  } else if (!strncmp(cmd, "socket", 6)) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    say(s < 0 ? "socket refused" : "SOCKET (bad)");
  } else if (!strncmp(cmd, "fork", 4)) {
    pid_t p = fork();
    say(p < 0 ? "fork refused" : "FORKED (bad)");
  } else if (!strncmp(cmd, "mem", 3)) {
    volatile char *big = malloc(300u << 20);     /* more than the address-space limit (used afterwards: the compiler may not drop it) */
    if (big) big[12345] = 1;
    say(big ? "ALLOCATED (bad)" : "mem refused");
  } else if (!strncmp(cmd, "mmap", 4)) {
    size_t sizes[] = {8u << 20, 40u << 20, 60u << 20, 300u << 20};
    char b[160] = "";
    for (int i = 0; i < 4; i++) {
      void *m = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      char t[40];
      snprintf(t, sizeof t, "%uMB:%s ", (unsigned)(sizes[i] >> 20), m == MAP_FAILED ? "no" : "yes");
      strcat(b, t);
    }
    say(b);
  } else if (!strncmp(cmd, "limits", 6)) {
    struct rlimit as, cpu;
    getrlimit(RLIMIT_AS, &as);
    getrlimit(RLIMIT_CPU, &cpu);
    char b[96];
    snprintf(b, sizeof b, "as=%lu cpu=%lu", (unsigned long)as.rlim_cur, (unsigned long)cpu.rlim_cur);
    say(b);
  } else if (!strncmp(cmd, "spin", 4)) {
    volatile unsigned long x = 0;
    for (;;) x++;
  }
  return 0;
}
