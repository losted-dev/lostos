/* lostOS plugin SDK for the speaker (C, header only, no libraries).
 *
 * Build a plugin as a static armhf program:   zig cc -target arm-linux-musleabihf -static -Os main.c -o plugin
 * The firmware starts it in a sandbox: its own user, a folder that holds nothing but the program, no files, no sockets, no
 * processes, limits on memory and CPU. The only thing it has is descriptor 3, a line protocol to the firmware (satellite/src/plugins/host.h).
 *
 *   #include "lostos_plugin.h"
 *   int main(void) {
 *     lp_call("subscribe", "button", NULL, 0);                    // needs the permission events.subscribe
 *     for (;;) {
 *       char ev[128];
 *       if (lp_wait_event(ev, sizeof ev, 5000) > 0 && !strncmp(ev, "button action", 13))
 *         lp_call("led", "139 92 246 2", NULL, 0);                // needs device.led
 *     }
 *   }
 */
#ifndef LOSTOS_PLUGIN_H
#define LOSTOS_PLUGIN_H

#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LP_FD 3

static int lp_got_fd_ = -1;   /* the descriptor that came with the last answer */
static char lp_buf_[2048];
static size_t lp_len_ = 0;

/* Takes one line out of the buffer (without the newline) into out; returns its length or -1 when there is no complete line yet. */
static int lp_take_(char *out, size_t cap) {
  char *nl = memchr(lp_buf_, '\n', lp_len_);
  if (!nl) return -1;
  size_t n = (size_t)(nl - lp_buf_);
  size_t c = n < cap - 1 ? n : cap - 1;
  memcpy(out, lp_buf_, c);
  out[c] = 0;
  memmove(lp_buf_, nl + 1, lp_len_ - n - 1);
  lp_len_ -= n + 1;
  return (int)c;
}

/* Reads from the firmware for up to timeout_ms; returns 1 when something arrived, 0 on timeout, -1 when the firmware closed the line. */
static int lp_fill_(int timeout_ms) {
  struct pollfd p = {LP_FD, POLLIN, 0};
  int r = poll(&p, 1, timeout_ms);
  if (r <= 0) return 0;
  struct iovec iov = {lp_buf_ + lp_len_, sizeof lp_buf_ - lp_len_};
  union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } ctl;
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = ctl.b;
  m.msg_controllen = sizeof ctl.b;
  ssize_t n = recvmsg(LP_FD, &m, 0);
  if (n <= 0) return -1;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))   /* a socket or pipe the firmware handed over with its answer */
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&lp_got_fd_, CMSG_DATA(c), sizeof(int));
  lp_len_ += (size_t)n;
  return 1;
}

static int lp_pending_n_ = 0;   /* events that arrive while a call waits for its answer are kept here (one line) */
static char lp_pending_[256];

/* One request. Returns 0 on "ok" (the result text goes to result), -1 on "err ..." or a lost line. */
static int lp_call(const char *method, const char *args, char *result, size_t cap) {
  static int next = 1;
  char line[1100], in[512];
  int id = next++;
  snprintf(line, sizeof line, "%d %s %s\n", id, method, args ? args : "");
  if (write(LP_FD, line, strlen(line)) < 0) return -1;
  char want[16];
  snprintf(want, sizeof want, "%d ", id);
  for (int tries = 0; tries < 200; tries++) {
    int n;
    while ((n = lp_take_(in, sizeof in)) >= 0) {
      if (in[0] == '!') {                                   /* an event in the middle of a call: kept for lp_wait_event */
        strncpy(lp_pending_, in + 2, sizeof lp_pending_ - 1);
        lp_pending_n_ = 1;
        continue;
      }
      if (!strncmp(in, want, strlen(want))) {
        const char *rest = in + strlen(want);
        if (!strncmp(rest, "ok", 2)) {
          if (result && cap) snprintf(result, cap, "%s", rest[2] == ' ' ? rest + 3 : "");
          return 0;
        }
        if (result && cap) snprintf(result, cap, "%s", rest);
        return -1;
      }
    }
    if (lp_fill_(100) < 0) return -1;
  }
  return -1;
}

/* Like lp_call, for the methods that hand a descriptor over (connect, listen, udp, audio.open): returns the descriptor or -1 (the reason is in result). */
static int lp_call_fd(const char *method, const char *args, char *result, size_t cap) {
  lp_got_fd_ = -1;
  if (lp_call(method, args, result, cap) != 0) return -1;
  int fd = lp_got_fd_;
  lp_got_fd_ = -1;
  return fd;
}

/* Waits up to timeout_ms for an event line ("button action", "media spotify 1 Title", "volume 0.50"); returns its length, 0 on timeout, -1 on loss. */
static int lp_wait_event(char *out, size_t cap, int timeout_ms) {
  if (lp_pending_n_) {
    lp_pending_n_ = 0;
    snprintf(out, cap, "%s", lp_pending_);
    return (int)strlen(out);
  }
  char in[512];
  for (;;) {
    int n;
    while ((n = lp_take_(in, sizeof in)) >= 0) {
      if (in[0] == '!') {
        snprintf(out, cap, "%s", in + 2);
        return (int)strlen(out);
      }
    }
    int f = lp_fill_(timeout_ms);
    if (f <= 0) return f;
  }
}

#endif
