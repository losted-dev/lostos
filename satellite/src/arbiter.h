// Wake word arbitration between lostos stations on one LAN: every station that hears the wake word
// broadcasts a claim with its level (UDP 6054) and waits a short window for its peers' claims; only
// the best claim answers. Yandex does the same in its cloud (TSpotterValidation / multiactivation in
// maind), which is unavailable here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/loop.h"

namespace cs {

class WakeArbiter {
 public:
  struct Claim {
    std::string station;    // DeviceIdentity::name of the sender
    std::string wake;       // wake word id: "alisa" / "yandex"
    bool busy = false;      // sender is answering / announcing / ringing a timer: keeps the turn
    float level_db = -100;  // AudioFrontend level, 0.1 dB steps
  };
  // won: this station answers. best: the winning claim (ours when won). peers: other stations
  // whose claim for the same wake word counted.
  using DoneCb = std::function<void(bool won, const Claim &best, size_t peers)>;

  explicit WakeArbiter(Loop &loop);
  ~WakeArbiter();
  WakeArbiter(const WakeArbiter &) = delete;
  WakeArbiter &operator=(const WakeArbiter &) = delete;

  bool start(std::string station);  // false: no socket, the station answers alone
  void claim(const std::string &wake, float level_db, bool busy, DoneCb done);  // done may be empty
  void cancel();  // drops the pending decision; done is not called
  // Stations that do not share a network (a guest Wi-Fi, a virtual machine behind NAT) cannot hear the UDP broadcast: their claims go through the server link.
  // relay sends our claim to the server (once, the link is reliable); on_remote takes a peer's claim that the server passed on. Loop thread only.
  void set_relay(std::function<void(const Claim &)> relay) { relay_ = std::move(relay); }
  void on_remote(const Claim &c);
  void set_remote_peers(int n) { remote_peers_ = n; }   // other stations of the household that are on line through the server: the claims then take longer to arrive

 private:
  struct Received {
    Claim claim;
    int64_t at_ms;
  };
  void send();
  void on_readable();
  void accept(const Claim &c);   // a peer's claim, from the broadcast or from the server
  void decide();

  Loop &loop_;
  int fd_ = -1;
  std::string station_;
  std::vector<Received> recent_;  // one entry per peer station: its latest claim
  Claim mine_;
  int64_t claim_ms_ = 0;
  int copies_left_ = 0;
  uint64_t resend_timer_ = 0;
  uint64_t decide_timer_ = 0;
  DoneCb done_;
  std::function<void(const Claim &)> relay_;
  int remote_peers_ = 0;
  int64_t window_ms() const;
};

}  // namespace cs
