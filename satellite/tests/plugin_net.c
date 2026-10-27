/* Test plugin for the brokered network and for the sound output (needs network.connect and audio.play):
 *   1. connect to the speaker itself (127.0.0.1 and its own address)  -> the firmware must refuse
 *   2. connect to the server and read the start of an HTTP answer      -> allowed (a handed-over socket)
 *   3. play a 440 Hz tone for 3 seconds through audio.open (a pipe)    -> audible, and the plugin becomes a source of the media hub
 *   4. try to create a socket itself                                   -> killed by the sandbox (last, ends the plugin) */
#include <math.h>
#include <string.h>
#include <sys/socket.h>

#include "../sdk/lostos_plugin.h"

#define SERVER "192.168.1.78"

int main(void) {
  char res[160];
  int fd = lp_call_fd("connect", "127.0.0.1 5555", res, sizeof res);
  lp_call("log", fd < 0 ? res : "CONNECTED TO ADB (bad)", NULL, 0);
  fd = lp_call_fd("connect", "192.168.1.18 5555", res, sizeof res);
  lp_call("log", fd < 0 ? res : "CONNECTED TO ITSELF (bad)", NULL, 0);

  fd = lp_call_fd("connect", SERVER " 8765", res, sizeof res);
  if (fd >= 0) {
    const char *req = "GET /api/v1/media HTTP/1.0\r\nHost: x\r\n\r\n";
    (void)!write(fd, req, strlen(req));
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    char msg[120];
    snprintf(msg, sizeof msg, "server answered: %.30s", n > 0 ? buf : "(nothing)");
    for (char *c = msg; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    lp_call("log", msg, NULL, 0);
    close(fd);
  } else {
    lp_call("log", res, NULL, 0);
  }

  int pcm = lp_call_fd("audio.open", "", res, sizeof res);
  if (pcm >= 0) {
    short frame[2 * 960];
    for (int block = 0; block < 150; block++) {              /* 150 x 20 ms = 3 s */
      for (int i = 0; i < 960; i++) {
        short v = (short)(6000.0 * sin(2.0 * M_PI * 440.0 * (block * 960 + i) / 48000.0));
        frame[2 * i] = frame[2 * i + 1] = v;
      }
      if (write(pcm, frame, sizeof frame) < 0) break;
    }
    close(pcm);
    lp_call("audio.close", "", res, sizeof res);
    lp_call("log", "tone played", NULL, 0);
  } else {
    lp_call("log", res, NULL, 0);
  }

  lp_call("log", "now a forbidden socket()", NULL, 0);
  socket(AF_INET, SOCK_STREAM, 0);                           /* the sandbox kills the plugin here */
  lp_call("log", "SOCKET CREATED (bad)", NULL, 0);
  return 0;
}
