#ifdef __ANDROID__
#include "core/android_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if __ANDROID_API__ < 24
namespace {
// One allocation per interface; ifa is first, so freeing the ifaddrs frees all of it.
struct Entry {
  ifaddrs ifa;
  sockaddr_in addr, mask;
  char name[IFNAMSIZ];
};
}  // namespace

extern "C" int getifaddrs(ifaddrs **out) {
  *out = nullptr;
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  char buf[8192];
  ifconf ic{};
  ic.ifc_len = sizeof buf;
  ic.ifc_buf = buf;
  if (ioctl(s, SIOCGIFCONF, &ic) < 0) {
    close(s);
    return -1;
  }
  ifaddrs *head = nullptr, **tail = &head;
  for (ifreq *r = ic.ifc_req; reinterpret_cast<char *>(r + 1) <= buf + ic.ifc_len; r++) {
    auto *e = static_cast<Entry *>(calloc(1, sizeof(Entry)));
    if (!e) break;
    memcpy(e->name, r->ifr_name, IFNAMSIZ);
    e->name[IFNAMSIZ - 1] = 0;
    memcpy(&e->addr, &r->ifr_addr, sizeof e->addr);
    e->ifa.ifa_name = e->name;
    e->ifa.ifa_addr = reinterpret_cast<sockaddr *>(&e->addr);
    ifreq q{};
    memcpy(q.ifr_name, r->ifr_name, IFNAMSIZ);
    if (ioctl(s, SIOCGIFFLAGS, &q) == 0) e->ifa.ifa_flags = static_cast<unsigned short>(q.ifr_flags);
    if (ioctl(s, SIOCGIFNETMASK, &q) == 0) {
      memcpy(&e->mask, &q.ifr_netmask, sizeof e->mask);
      e->ifa.ifa_netmask = reinterpret_cast<sockaddr *>(&e->mask);
    }
    *tail = &e->ifa;
    tail = &e->ifa.ifa_next;
  }
  close(s);
  *out = head;
  return 0;
}

extern "C" void freeifaddrs(ifaddrs *p) {
  while (p) {
    ifaddrs *next = p->ifa_next;
    free(p);
    p = next;
  }
}
#endif

#if __ANDROID_API__ < 28
extern "C" ssize_t getrandom(void *buf, size_t len, unsigned flags) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, static_cast<char *>(buf) + got, len - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += size_t(n);
  }
  close(fd);
  return got == len ? ssize_t(len) : -1;
}
#endif

#if __ANDROID_API__ < 33
extern "C" int backtrace(void **frames, int size) { return 0; }
#endif
#endif
