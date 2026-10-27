/* Hostile plugins for the end-to-end test on the speaker (ref/build-test-plugins.sh builds one package per case):
 *   CASE_PERM  asks for led without the permission four times  -> "err permission", switched off after the third
 *   CASE_SEGV  crashes at once                                 -> restarted twice, then switched off
 *   CASE_SYS   opens a file                                    -> killed by seccomp (forbidden system call), restarted twice, then off */
#include <fcntl.h>
#include <signal.h>

#include "../sdk/lostos_plugin.h"

int main(void) {
  char res[64];
  lp_call("log", "test plugin started", res, sizeof res);
#if defined(CASE_PERM)
  for (int i = 0; i < 4; i++) {
    int r = lp_call("led", "255 0 0 1", res, sizeof res);
    lp_call("log", r == 0 ? "led OK (bad)" : res, NULL, 0);
    usleep(300000);
  }
  for (;;) pause();
#elif defined(CASE_SEGV)
  raise(SIGSEGV);
#elif defined(CASE_SYS)
  int fd = open("/dev/fb0", O_RDWR);
  lp_call("log", fd < 0 ? "open refused" : "OPENED (bad)", NULL, 0);
#endif
  return 0;
}
