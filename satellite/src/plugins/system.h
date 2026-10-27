// System plugins of lostOS: the plugins that come with the system ("Встроенный пакет") and run outside the sandbox, because
// they are the receivers of the speaker itself: AirPlay 2 (ru.lostos.airplay), Spotify Connect (ru.lostos.spotify), Bluetooth follows.
// They are plugins like the others (a folder in /data/lostos/plugins/<id>/ with plugin.conf, a page and permissions in the panel,
// the same install / enable / disable / remove commands); the difference is "kind=system" in plugin.conf. What they bring along
// (the receiver programs and their libraries) sits in that folder, the code that configures and runs them is part of lostos
// (system_drivers.cpp). lostos starts their processes, watches them and starts them again when they die.
//
// "audio" is not a plugin but a part of the system itself (the equaliser over the mixer and the process that holds the mixer):
// it is run by the same supervisor and never shows in the list.
//
// The processes do not die with lostos: they are detached, and a lostos that starts (after an update, after a crash) finds the
// ones that run already and takes them over, so the music goes on.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/loop.h"
#include "model.h"

namespace cs {

constexpr const char *kPluginsDir = "/data/lostos/plugins";
constexpr const char *kSystemSocket = "/tmp/lostos-system.sock";

// One program of a module and how it is started.
struct SysProc {
  std::string name;                                    // short, also the name of its log file
  std::string exe;                                     // full path of the program
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> env;  // added to the environment of lostos
  std::string match;       // a copy that runs already is ours when its command line contains this (empty: the program is enough)
  int delay_ms = 0;        // started this long after the module (a daemon that needs another one first)
  bool primary = false;    // "power off" stops only the primary programs, the helpers stay (the speaker stays visible)

  pid_t pid = 0;
  bool child = false;      // started by us (and so reaped by us); otherwise found running
  int fails = 0;
  int64_t due_ms = 0, started_ms = 0;
};

enum class SysLevel { OFF, HELPERS, ALL };

struct SysPlugin {
  std::string id, title, dir;
  bool core = false;                                   // always on (the audio stack): no switch for the owner
  std::function<bool()> usable;                          // the programs of the module are there
  std::function<bool()> prepare;                         // writes its configuration; false: do not start
  std::function<std::vector<SysProc>()> plan;            // the programs to run

  SysLevel level = SysLevel::OFF;
  std::vector<SysProc> procs;
  int64_t begin_ms = 0;
};

class SystemPlugins {
 public:
  SystemPlugins(Loop &loop, const Model &model);
  ~SystemPlugins();  // the processes keep running

  // The audio stack goes first and before anything of lostos opens ALSA: the equaliser is a replacement of /etc/asound.conf,
  // which a process reads once, and the mixer must be held by the keeper before a player takes it.
  void start_audio();
  // The rest: every module that is installed and not switched off by the owner.
  void start();

  bool installed(const std::string &id) const;
  SysLevel level(const std::string &id) const;
  // Runtime on / off of a module; off with keep_helpers leaves what keeps the speaker visible (AirPlay: nqptp and avahi).
  bool set_running(const std::string &id, bool on, bool keep_helpers = false);
  // The switch of the owner, it also holds after a reboot.
  bool set_enabled(const std::string &id, bool on);
  bool enabled(const std::string &id) const;

  std::string status() const;         // for people: every program and its pid
  std::string plugin_lines() const;   // for the plugin list: "id<TAB>state<TAB>name" per installed plugin
  bool owns(const std::string &id) const { return find(id) != nullptr && !find(id)->core; }
  // "status" | "start <id>" | "stop <id>" | "restart <id>" | "enable <id>" | "disable <id>" -> the answer text.
  std::string command(const std::string &line);

  const Model &model() const { return model_; }

  // A plugin was stopped for good (the owner's switch, a command): whoever showed its state must forget it.
  std::function<void(const std::string &id)> on_stopped;

 private:
  SysPlugin *find(const std::string &id);
  const SysPlugin *find(const std::string &id) const;
  void begin(SysPlugin &m, SysLevel level);
  void halt(SysPlugin &m, bool keep_helpers);
  void tick();
  void serve();
  void stop_proc(SysProc &p);

  Loop &loop_;
  const Model &model_;
  std::vector<SysPlugin> mods_;
  int sock_ = -1;
  bool started_ = false;
};

// The name that AirPlay and Spotify show for this speaker: the owner's one, else the name of the model; "" as a new name brings the default back.
std::string speaker_name(const Model &model);
bool set_speaker_name(const std::string &name);

// librespot calls "lostos --spotify-event" for every playback event; the answer goes to lostos through /tmp/spotify-events.
int run_spotify_event();
// "lostos --module <command>": talks to the running lostos. Returns the exit status.
int run_system_command(int argc, char **argv);

// The drivers (drivers.cpp).
void add_audio_core(SystemPlugins &mods, std::vector<SysPlugin> &out);
void add_airplay_plugin(SystemPlugins &mods, std::vector<SysPlugin> &out);
void add_spotify_plugin(SystemPlugins &mods, std::vector<SysPlugin> &out);

}  // namespace cs
