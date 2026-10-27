// Minimal mDNS (RFC 6762/6763) responder advertising DNS-SD service instances over IPv4 under one
// host name. Answers PTR/SRV/TXT/A/ANY queries for each service, its instance and "<host>.local",
// lists every service type in "_services._dns-sd._udp.local" enumeration, sends unsolicited
// announcements on start, on add_service()/set_txt(), when the address or link changes (rtnetlink,
// with a 30 s poll as fallback), and goodbyes on destruction.
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/loop.h"

namespace cs {

class MdnsResponder {
 public:
  // host: host label without ".local" (also used as the instance name of this first service).
  // service: e.g. "_esphomelib._tcp". txt: "key=value" strings.
  MdnsResponder(Loop &loop, std::string host, std::string service, uint16_t port, std::vector<std::string> txt);
  ~MdnsResponder();
  MdnsResponder(const MdnsResponder &) = delete;
  MdnsResponder &operator=(const MdnsResponder &) = delete;

  bool start();

  // Advertises another service on the same host (A record). instance: label without dots, e.g. the
  // host label. May be called before or after start(); after start() it is announced at once.
  void add_service(std::string service, std::string instance, uint16_t port, std::vector<std::string> txt);
  // Replaces the TXT records of a service added earlier (e.g. "_esphomelib._tcp") and announces it.
  void set_txt(const std::string &service, std::vector<std::string> txt);
  // IPv4 address being advertised (network byte order), 0 when there is none.
  uint32_t ipv4() const { return ip_; }

 private:
  struct Service {
    std::string type_fqdn;      // "_esphomelib._tcp.local"
    std::string instance_fqdn;  // "<instance>._esphomelib._tcp.local"
    uint16_t port;
    std::vector<std::string> txt;
  };
  // Record selection bitmask: bit 0 = host A record; service i owns the four bits from 1 + 4*i
  // (PTR, SRV, TXT, service-type enumeration PTR). Up to kMaxServices services.
  enum Kind : unsigned { K_PTR = 0, K_SRV = 1, K_TXT = 2, K_ENUM = 3 };
  static constexpr unsigned kMaxServices = 7;
  static constexpr uint32_t kA = 1;
  static uint32_t bit(size_t svc, Kind k) { return 1u << (1 + 4 * svc + k); }
  uint32_t all_records() const;

  bool refresh_iface();  // returns true if the interface/IP changed
  void join_group();
  // Re-reads the interface, re-asserts the multicast membership and, on change or when forced
  // (link came back), announces again.
  void check_network(bool force_announce);
  bool open_netlink();
  void on_netlink();
  void on_readable();
  void handle_query(const uint8_t *p, size_t n, const sockaddr_in &from);
  std::string build(uint16_t id, uint32_t answers, uint32_t additional, uint32_t ttl_cap, bool goodbye,
                    const std::string &question) const;
  void announce(bool goodbye);
  void schedule_announcements();
  void send_to(const std::string &pkt, const sockaddr_in &to);

  Loop &loop_;
  std::string host_fqdn_;  // "<host>.local"
  std::vector<Service> services_;
  int fd_ = -1;
  int nl_fd_ = -1;  // rtnetlink: address/link changes
  int ifindex_ = 0;
  uint32_t ip_ = 0;  // network byte order
  std::string ifname_;
  bool join_failed_ = false;
  std::vector<std::pair<int, unsigned>> link_flags_;  // ifindex -> last IFF_* flags seen
  uint64_t poll_timer_ = 0;
  uint64_t change_timer_ = 0;  // debounces a burst of netlink events
  bool change_forced_ = false;
  uint64_t announce_timers_[2] = {};
};

}  // namespace cs
