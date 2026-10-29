// Glue between Home Assistant (ESPHome API), the microphone front-end, playback and hardware UI.
// Voice-satellite behaviour follows OHF linux-voice-assistant (satellite.py).
#pragma once

#include <deque>

#include <map>
#include <memory>
#include <string>

#include "airplay/airplay.h"
#include "bt/bluetooth.h"
#include "dlna/dlna.h"
#include "arbiter.h"
#include "audio/frontend.h"
#include "core/loop.h"
#include "esphome/api_server.h"
#include "hw/hw.h"
#include "hw/sysinfo.h"
#include "media/hub.h"
#include "link/link.h"
#include "plugins/host.h"
#include "plugins/system.h"
#include "media/player.h"
#include "model.h"
#include "sendspin/sendspin_service.h"
#include "update/updater.h"
#include "zigbee/service.h"

namespace cs {

struct Settings {
  // Volume on the stock curve (Model::volume_map), state.conf volume2=. volume= keeps the same raw
  // level on the curve of earlier builds, so a binary rolled back by supervise.sh plays as loud.
  float volume = 0.5f;
  bool has_volume = false;       // volume2= was present
  float volume_legacy = -1;      // volume=; < 0: absent
  bool legacy_on_stock = false;  // volume_curve=2: one interim build wrote volume= on the stock curve
  bool muted = false;          // media mute
  bool mic_muted = false;      // software microphone mute
  bool clock_on = true;
  float clock_brightness = 1.0f;
  int clock_scroll_ms = 400;   // clock text scroll step
  LightState light;            // user ring light
  std::string wake_word = "alisa";
  std::string tz = "MSK-3";    // POSIX TZ (default matches what the Yandex firmware showed); updated from HA
  bool auto_update = false;    // install new releases without asking
  bool nearest_answers = true; // wake arbitration with other lostos stations
  std::string clock_eq_style = "Wave";  // Wave | Bars
  std::string clock_eq = "On track change";  // Off | On track change | Always
  std::string music_viz = "All styles";  // one of Leds::music_styles()
  float led_speed = 1;             // 0.25..4
  bool alice_button = true;       // the centre button starts the assistant (off: it does not; the other actions of the button stay)
  bool cover_lookup = true;       // the panel may look the cover of an AirPlay / Bluetooth track up online (artist and title leave the house)
  bool lyrics = false;            // the panels show synced lyrics in the player (kept here so every panel agrees)
  bool volume_sound = true;       // a short click on every volume step
  bool voice_indication = true;    // listening / thinking / speaking shown on the ring
  std::string led_case_color;      // stem of Leds::case_colors(), "" = none
  // Release being installed. Still set after a restart into another version: that release was
  // rolled back by supervise.sh (crash loop), automatic updates skip it.
  std::string update_attempt;
  // zigbee_role=off | pulsar-coordinator | router (ZigbeeService::kRoles)
  ZigbeeService::Mode zigbee_role = ZigbeeService::Mode::OFF;
  // zigbee_flash_failed=<hex>: radio image version given up on (the radio did not take it); not
  // written again until the role changes or another image version is installed.
  uint32_t zigbee_flash_failed = 0;
  bool load(const std::string &path);
  void save(const std::string &path) const;
};

class Controller : public ApiHooks {
 public:
  Controller(Loop &loop, const Model &model, SystemPlugins &sysplugins, const std::string &state_path);
  ~Controller() override;
  bool start();
  void press_action_button();   // SIGUSR1: a short press of the Alice button (main.cpp)

  // ApiHooks
  void on_client_connected(const std::string &client_info) override;
  void on_client_disconnected() override;
  void on_voice_subscribed(bool subscribed) override;
  void on_voice_event(VaEvent type, const std::map<std::string, std::string> &data) override;
  void on_announce(const AnnounceRequest &req) override;
  void on_timer_event(const TimerInfo &t) override;
  void on_active_wake_words(const std::vector<std::string> &ids) override;
  void on_media_command(const MediaCommandRequest &cmd) override;
  void on_switch(SwitchId id, bool state) override;
  void on_button(ButtonId id) override;
  void on_light(const LightState &st, uint32_t transition_ms) override;
  void on_number(NumberId id, float value) override;
  void on_text(TextId id, const std::string &value) override;
  void on_select(SelectId id, const std::string &option) override;
  void on_action(ActionId id, const std::string &value, int32_t seconds) override;
  void on_led_show(const std::string &effect, const std::string &color, int32_t seconds) override;
  void on_time(uint32_t epoch_seconds, const std::string &posix_tz) override;
  void on_restart() override;
  void on_voice_start_failed() override;
  void on_voice_audio_overflow() override;
  void on_update(UpdateCommand cmd) override;

 private:
  enum class Phase { IDLE, LISTENING, PROCESSING, SPEAKING };

  void on_wake(const std::string &phrase, float level_db);
  void answer_wake(const std::string &phrase, std::vector<int16_t> preroll);
  void arbitration_done(bool won, const WakeArbiter::Claim &best, size_t peers);
  void cancel_arbitration();
  bool arbitration_on() const { return st_.nearest_answers && arbiter_ok_; }
  void on_command(const std::string &word);
  void update_commands();
  void music_pause();
  void music_resume();
  void start_pipeline(const std::string &phrase, bool earcon, std::vector<int16_t> preroll = {});
  void stop_streaming();
  void play_tts();
  void tts_finished(bool ok);
  void finish_interaction();
  void voice_link_lost();
  void abort_dialog(const char *why);
  void arm_watchdog();
  void interrupt_speech();
  void end_announce();
  void play_closing_earcon(const std::string &path);
  void stop_timer_ring(bool timeout = false);
  void ring_timer();
  void resume_ring();
  void show_timers();
  void update_ui();
  void update_detection();
  void update_duck();
  void apply_gains();
  void set_volume(float v, bool feedback);
  void push_volume_to_sources();   // the master volume to the Spotify app and the phone on Bluetooth (their sliders follow)
  float spotify_vol_ = -1.f, bt_vol_ = -1.f;
  int64_t last_click_ms_ = 0;
  int64_t bt_connected_ms_ = 0;
  bool prev_bt_connected_ = false;
  void set_muted(bool m);
  void start_sendspin(const DeviceIdentity &id);
  void start_zigbee(const DeviceIdentity &id);
  void publish_media();
  void update_music();
  void persist();
  void check_update();
  void install_update(const char *why);
  void maybe_auto_update();
  bool idle_for_update() const;
  void publish_update();
  std::string model_dir(const std::string &id) const;

  Loop &loop_;
  const Model &model_;
  std::string state_path_;
  Settings st_;
  std::unique_ptr<ApiServer> api_;
  AudioFrontend mic_;
  Player player_;
  Mixer mixer_;
  Leds leds_;
  Clock clock_;
  Input input_;
  LightSensor light_;
  WakeArbiter arbiter_;
  BtLink bt_;           // the Station's Bluetooth audio, one more source
  SystemPlugins &system_;   // the plugins that come with the system (AirPlay, Spotify): the programs of the receivers
  void publish_sysinfo();
  void rename_speaker(const std::string &name);
  SysInfo sysinfo_;   // the load, temperature, memory and room of the speaker for the system widgets of the panel
  AirPlayLink airplay_;  // AirPlay 2 receiver next to us: volume slider, state, transport, on/off
  DlnaRenderer dlna_;    // DLNA media player for Home Assistant: what plays, the cover, the controls

  Phase phase_ = Phase::IDLE;
  int64_t listen_started_ms_ = 0;  // start of the current LISTENING phase (commands window)
  bool hw_mic_muted_ = false;
  bool prev_air_session_ = false, prev_air_playing_ = false;
  int64_t spotify_stopped_ms_ = 0;   // when Spotify last went quiet; 10+ minutes later "play" starts fresh music
  const char *spotify_play_event(bool double_tap) const;
  int skip_dir_ = 1;           // direction of the last skip command (+1 next, -1 previous)
  int64_t skip_ms_ = 0;
  std::string last_air_track_;   // the previous AirPlay track text: a different one means the track changed by itself
  void track_change(int dir);  // display: bars sweep, then the equalizer
  int64_t last_music_ms_ = 0;     // last music state change / button use: the centre button stays a play/pause key for a minute
  int64_t mic_flip_block_until_ms_ = 0;
  int64_t mic_toggle_ms_ = 0;     // previous hardware mic switch flip (two quick flips toggle AirPlay)
  uint64_t tap_timer_ = 0;        // a single tap on the centre button waits briefly for a second one
  bool music_mode() const;
  bool spotify_ctl(const char *cmd);   // play / pause / playpause / next / prev for librespot on the speaker
  bool spotify_resume();               // play for a paused Spotify, with a check that it really started (else the server resumes it)
  DlnaRenderer::Now now_info() const;
  void start_dlna(const DeviceIdentity &id);
  void publish_now();         // the "Now playing" text sensor: source, artist, title, cover of the music that counts
  int air_cover_ver_ = 0;     // counts the cover pictures the AirPlay sender delivered
  uint32_t spotify_pos_ms_ = 0;   // position in the Spotify track at the last librespot event
  void publish_spotify_extra();
  void publish_playback(const char *source, uint32_t pos, uint32_t dur);
  void publish_media_state();
  void eq_set(const std::string &args);   // "eq <enabled 0/1> <preamp dB> <ten gains dB, comma separated>"
  void eq_publish();   // the whole picture of the music as one JSON text (the "media" API of the voice server)
  uint32_t pb_pos_ = 0, pb_dur_ = 0;
  std::string pb_src_, last_media_json_;
  std::string spotify_now_;   // librespot's "Artist - Title<TAB>cover url"
  std::string last_now_;
  bool ext_playing() const;   // AirPlay, Spotify or Bluetooth plays
  bool ext_paused() const;    // one of them is paused and can be resumed
  void centre_single_tap();
  void publish_wifi();
  void boot_show(bool server_ok);
  void say_after_boot(const std::string &path, int tries);
  void boot_wait();
  bool lux_ready_ = false;
  bool prev_bt_playing_ = false;
  std::string last_bt_track_;
  int64_t server_last_ms_ = 0;                   // the last greeting of the assistant server (it repeats every 10 s)
  bool server_alive() const { return server_hello_ && Loop::now_ms() - server_last_ms_ < 35000; }
  int64_t local_listen_until_ms_ = 0;            // wake word without a server: only the local commands listen until then
  bool local_cmd_done_ = false;
  void local_wake();
  // Is the internet really there? Asked early (a ping in a child process), the answer picks the failure sound:
  // "no internet" when the network is gone, "assistant unavailable" when only the assistant server is silent.
  void probe_internet(std::function<void(bool)> cb);
  void probe_net();
  void when_net_known(std::function<void()> fn);
  std::string no_server_sound() const;
  int net_probe_ = -1;                // -1 not known yet, 0 no internet, 1 internet
  std::function<void()> net_waiter_;
  int mic_flips_ = 0;
  uint64_t mic_flip_timer_ = 0;
  void bluetooth_set(bool on, bool voice);
  void publish_bt();
  bool server_hello_ = false;   // the assistant server on the PC has greeted us since power-up
  bool time_ready_ = false;     // the clock knows the real time (sent by Home Assistant / the assistant)
  int64_t boot_end_ms_ = 0;
  void release_clock();
  int64_t boot_wait_since_ms_ = 0;
  std::string last_wifi_;
  bool barge_enabled_ = false;   // the microphone stays open while she talks, the server may cut her short
  std::vector<int16_t> pre_wake_;   // 5 s of audio up to the wake word, sent in front of the dialog
  // ONE place that drives the music: what the buttons, the swipes and the voice commands do (see Controller::transport).
  enum class Transport { PLAYPAUSE, PAUSE, PLAY, NEXT, PREV };
  enum class MusicTarget { NONE, AIRPLAY, SPOTIFY, BT };
  MusicTarget music_target() const;
  MediaHub hub_;   // who plays now and who has to stand aside
  bool button_claimed_ = false;   // a plugin has taken the centre button (assistant.intercept)
  PluginHost phost_{loop_, system_};   // native plugins in the sandbox
  LinkClient link_;           // the outgoing connection to a lostOS server on the internet (link.conf)
  void media_command(const std::string &cmd);
  void publish_plugins();
  bool air_disabled_ = false;   // AirPlay was switched off by the user (then it is not brought back)
  void drop_airplay_session();
  void transport(Transport t);
  MusicTarget last_target_ = MusicTarget::NONE;   // who played last (an idle, connected phone does not count)
  bool error_shown_ = false;
  // Wake arbitration: the window between our claim and the decision. Microphone audio and a
  // command word heard in it are held until the decision.
  bool arbiter_ok_ = false;
  bool arbitrating_ = false;
  std::string arb_phrase_;
  float arb_level_ = -100;
  std::vector<int16_t> arb_audio_;
  std::string arb_command_;
  int64_t commands_blocked_until_ms_ = 0;  // another station answers the wake word just heard
  uint64_t watchdog_ = 0;  // loop timer ending a LISTENING/PROCESSING phase HA never advances
  // Pipeline events carry no run id. HA ends a run it replaces (barge-in: our new start cancels
  // the old run) with a RUN_END sent before the new run's RUN_START, so until RUN_START every
  // event belongs to the replaced run.
  bool awaiting_run_start_ = false;  // this dialog's start request has no RUN_START yet
  bool run_active_ = false;          // some run is between RUN_START and RUN_END
  bool stale_run_end_ = false;       // the RUN_END of a replaced run is still due
  bool stop_sent_ = false;           // voice_request_stop already sent for this dialog's run
  std::string tts_url_;
  bool tts_played_ = false;
  bool continue_conversation_ = false;
  bool announcing_ = false;  // an HA announcement (assist or media player) plays on ANNOUNCE
  bool ducked_ = false;
  bool closing_earcon_ = false;
  uint64_t earcon_seq_ = 0;
  // Completion callbacks act only for what is still current: dialog_ changes whenever a dialog
  // starts or ends, announce_seq_ whenever an HA announcement starts or is interrupted.
  uint64_t dialog_ = 0;
  uint64_t announce_seq_ = 0;
  // timers
  bool timer_ringing_ = false;
  bool ring_yielded_ = false;  // the ring gave the ANNOUNCE channel to speech; resume_ring() continues it
  uint64_t timer_ring_tick_ = 0;
  int64_t timer_ring_started_ms_ = 0;
  bool ring_alarm_ = false;          // an alarm (the server names it "alarm:..."): rising volume, 5 minutes; a timer rings 1 minute
  int64_t ring_max_ms_ = 60000;
  float ring_saved_volume_ = 0.f;
  // Alarms live on the speaker (the server only hands them over): they ring when the PC is off. The server sends the coming
  // moments of each alarm (a repeating one: the next weeks); a ring nobody stops comes back in five minutes, six times.
  struct Alarm {
    std::string id, label;
    std::deque<int64_t> at;   // epoch seconds, ascending
    bool snooze = false;
    bool reminder = false;   // not a ring: the phrase prepared by the server is played (the file keeps its id)
    int tries = 0;
    int snooze_min = 5;      // a ring nobody stopped comes back after this many minutes (0 = never)
  };
  std::vector<Alarm> alarms_;
  int ring_tries_ = 0;
  int ring_snooze_min_ = 5;
  std::string ring_label_;
  void alarms_load();
  void alarms_save() const;
  void alarm_command(const std::string &cmd);
  void alarm_led();
  void alarm_tick();
  void alarm_fetch(const std::string &id, const std::string &url);
  void play_reminder(const std::string &path, bool delete_after);
  void start_ring(bool alarm);
  uint64_t ring_ramp_tick_ = 0;
  void ring_ramp();
  struct HaTimer {
    bool active;
    int64_t end_ms;         // while active
    uint32_t seconds_left;  // while paused
  };
  std::map<std::string, HaTimer> timers_;  // HA timers by TimerInfo::id
  std::string shown_timer_;                // id on the clock, "" = none
  HaTimer shown_{};                        // its data when it was put there
  uint64_t timer_tick_ = 0;                // looks again when the shown running timer runs out
  // light sensor publishing
  float last_lux_published_ = -1;
  int64_t last_lux_ms_ = 0;
  // Clock text / face entity states; not persisted (the clock shows the time after a restart).
  std::string clock_text_;
  std::string clock_face_ = "None";
  // updates
  Updater updater_;
  Release release_;            // latest release found; version "" = none known
  std::string update_error_;
  bool update_installing_ = false;
  float update_progress_ = 0;
  uint64_t update_retry_ = 0;  // waits for idle before an automatic install
  // Zigbee radio (Midi): coordinator for ZHA or router in another network.
  std::unique_ptr<ZigbeeService> zigbee_;
  std::string zigbee_status_;  // last status line published
  // Sendspin (Music Assistant) player; declared last so it stops (and stops calling back into
  // the members above) first.
  bool sendspin_pairing_prompt_ = false;
  bool press_waiting_ = false;          // the server asks to press the Alice button to connect this station (link.cpp "press")
  uint64_t press_timer_ = 0;
  std::string press_wav_;
  void press_begin(const std::string &code, const std::string &wav);
  void button(Button b, Press p);
  void press_end();
  std::unique_ptr<SendspinService> sendspin_;
};

}  // namespace cs
