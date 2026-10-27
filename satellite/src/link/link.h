// Station link: the speaker keeps ONE outgoing WebSocket (over TLS) to a lostOS server on the internet, so a server that is not in the same
// network can work with it. The server asks the link to open a few ports of this speaker (the ESPHome API 6053, AirPlay state 7000, covers
// 49153) and the link carries those connections. The protocol is described in server/link.py.
//
// Config: /data/lostos/link.conf   one line  server=https://host[:port]   (absent or empty: the link is off, nothing leaves the speaker)
//         forward=ha=192.168.1.5:8123   (optional, any number): a host of the home network the server may reach through the speaker (private IPv4 only)
// Key:    /data/lostos/link.key    32 random bytes of an Ed25519 private key, made on the first start (mode 0600, never sent anywhere)
// Id:     /data/lostos/link.id     the station id (first 20 hex digits of SHA-256 of the public key), written for whoever sets it up
// A new station is "pending" on the server until its owner confirms it in the panel.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cs {

class LinkClient {
 public:
  ~LinkClient();
  void start(const std::string &station_name);   // starts the thread when /data/lostos/link.conf names a server
  void stop();
  std::string state() const;                      // "off", "connecting", "pending" (waits for the owner), "online"

  // The owner confirmed this station on the server: the speaker must show the code, say the line (a wav the server sent, "" = none) and wait for
  // its Alice button. Both callbacks run on the link thread (post to the loop yourself). on_press_end: the window is over (confirmed, timed out, the link fell).
  std::function<void(const std::string &code, const std::string &wav_path)> on_press;
  std::function<void()> on_press_end;
  std::function<void(const std::string &wav_path)> on_say;   // the server asks the speaker to say a line (a wav it sent); link thread
  void press_pressed() { press_hit_ = true; }     // any thread: the button was pressed

  // Wake word arbitration through the server (stations that share no network): our claim goes out, the claims of the household's other stations come in
  // (the server passes them on). on_wake and on_peers run on the link thread (post to the loop yourself); on_peers: how many other stations of the household are on line.
  void send_wake(const std::string &station, const std::string &wake, bool busy, float level);   // any thread; dropped while the link is down
  std::function<void(const std::string &station, const std::string &wake, bool busy, float level)> on_wake;
  std::function<void(int peers)> on_peers;

 private:
  void run();
  bool session();                                  // one connection; false = failed, true = it was up and ended
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> state_{0};
  std::atomic<bool> press_hit_{false};
  std::atomic<bool> fresh_{false};                 // the daemon has just started (the boot show has not played yet): the hello says so once, so that the server says "connected" after the show
  int wake_fd_ = -1;                               // eventfd: wakes the link thread when a wake claim waits (poll sleeps up to a second otherwise)
  std::mutex wake_mu_;
  std::vector<std::string> wake_out_;              // frames waiting to go out (json), the link thread sends them
};

}  // namespace cs
