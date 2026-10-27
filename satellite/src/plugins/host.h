// Native plugins of the speaker: installing, starting in the sandbox (plugins/sandbox.h) and answering what they ask.
//
// On the speaker a plugin lives in /data/lostos/plugins/<id>/:  root/plugin (the static program, the only thing its sandbox sees),
// plugin.conf (name, permissions the user granted, state) and data.txt (the plugin's little key-value store, kept by the firmware).
//
// The plugin talks to the firmware over file descriptor 3, one text line per message:
//     plugin -> firmware   "<n> <method> <arguments>"          firmware -> plugin   "<n> ok <result>"  or  "<n> err <reason>"
//     firmware -> plugin   "! <event> <data>"                   (only after "subscribe <event>")
// Methods and the permission each one needs:
//     log <text>                         -
//     subscribe <button|media|volume>    events.subscribe
//     led <r> <g> <b> <seconds>          device.led       (the ring shows this colour for a while)
//     display <seconds> <text>           device.display   (the clock shows this text for a while)
//     media <play|pause|playpause|next|previous>   media.control
//     volume                              device.read     (result: 0..1)
//     get <key> / set <key> <value>      storage          (up to 32 keys, 256 bytes each)
//     publish <topic> <text>             events.publish   (fired as an event of the speaker for Home Assistant and the server)
//     resolve <name>                     network.connect  (result: an IPv4 address)
//     connect <host> <port>              network.connect  (the firmware opens the TCP connection and hands the descriptor over)
//     listen <port>                      network.listen   (a listening TCP socket on a port from 1024 up, handed over)
//     udp <port> [<multicast group>]     network.listen   (a UDP socket, optionally in a multicast group such as 224.0.0.251, handed over)
//     audio.open                         audio.play       (a pipe for 48 kHz stereo S16LE sound, played as a music source of the speaker)
//     audio.close                        audio.play
//     button claim | button release      assistant.intercept (while claimed, a tap of the centre button does nothing by itself: the plugin
//                                         gets the "button" events and decides; stopping a timer and cancelling a dialog still work)
// A plugin cannot create a socket itself (the sandbox forbids it): it gets only sockets the firmware opened for it, and the firmware
// refuses addresses that belong to the speaker itself (loopback, its own addresses: adb, the API, the other plugins) and the speaker's own ports.
// A call without the permission is answered with "err permission" and counted; three of them stop the plugin.
// A plugin that is killed by the sandbox (a forbidden system call, out of memory or CPU time) is restarted twice, then switched off.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/loop.h"
#include "plugins/sandbox.h"
#include "plugins/system.h"

namespace cs {

class PluginHost {
 public:
  PluginHost(Loop &loop, SystemPlugins &sysplugins) : loop_(loop), system_(sysplugins) { state_changed = [](const std::string &) {}; }

  // What the plugins may do to the speaker: the controller fills these in.
  std::function<void(int r, int g, int b, int seconds)> led;
  std::function<void(const std::string &text, int seconds)> display;
  std::function<void(const std::string &cmd)> media;
  std::function<float()> volume;
  std::function<void(const std::string &topic, const std::string &text)> publish;
  std::function<void(bool claimed)> button_claim;                  // a plugin took / gave back the centre button (the Alice button)
  std::function<void(bool playing)> audio_state;                  // a plugin started / stopped playing sound (a source of the media hub)
  std::function<void(const std::string &line)> state_changed;   // a one-line status for the panel ("id\tstate\tdetail")

  void start();                                                    // starts the plugins that are switched on
  // "install <id> <url> <sha256> <permissions> [<download base>]", "remove <id>", "enable <id> <permissions>", "disable <id>", "list": from the voice server
  void command(const std::string &line);
  void event(const std::string &name, const std::string &data);    // to the plugins that subscribed
  void audio_stop();                                               // another source took the speaker: the sound of the plugins stops
  std::string status() const;                                      // "id<TAB>state<TAB>detail" per line

 private:
  struct Plugin {
    std::string id, name;
    std::vector<std::string> granted;
    bool enabled = false;
    SandboxProc proc;
    std::string detail, inbuf;
    std::vector<std::string> subscribed;
    int violations = 0, crashes = 0;
    unsigned uid = 0;
    std::vector<std::string> asked;                               // what the server asked to grant (the last time); granted is what the speaker allowed (see trust.h)
    std::string sha;                                              // SHA-256 of the program that was downloaded
    bool trusted = false;                                         // the speaker verified the signature of the package itself
    std::string who;                                              // the publisher named in it
    std::vector<std::string> declared;                            // the device permissions that its signed manifest declares
    std::map<std::string, std::string> kv;
    bool claims_button = false;                                   // this plugin holds the centre button
    pid_t aplay = -1;                                             // the player of the sound this plugin sends
  };
  Loop &loop_;
  SystemPlugins &system_;   // the plugins of the system itself (AirPlay, Spotify): not in the sandbox, listed and switched with the others
  std::map<std::string, Plugin> plugins_;

  static std::string dir(const std::string &id);
  bool load(const std::string &id);
  void save(const Plugin &p) const;
  void run(Plugin &p);
  void on_readable(const std::string &id);
  void reply(Plugin &p, const std::string &n, const std::string &res, int fd = -1);   // fd: a descriptor to hand over with the answer
  void audio_close(Plugin &p);
  void release_button(Plugin &p);                                   // the claim ends with the plugin
  void handle(Plugin &p, const std::string &line);
  void stopped(Plugin &p);
  void halt(Plugin &p);
  bool allowed(const Plugin &p, const std::string &perm) const;
  void install(const std::string &id, const std::string &url, const std::string &sha, const std::string &perms, const std::string &dl);
  void trust(Plugin &p);                                           // checks the signature of the package here (trust.h) and sets what the plugin is granted
  std::vector<std::string> allowed_for(const Plugin &p, const std::vector<std::string> &asked) const;   // what the speaker grants of what the server asks
};

}  // namespace cs
