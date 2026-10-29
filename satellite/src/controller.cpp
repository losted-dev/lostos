#include "controller.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <vector>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <cerrno>
#include <fcntl.h>
#include <dirent.h>
#include <deque>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "esphome/mdns.h"
#include "update/updater.h"

namespace cs {
namespace {

constexpr const char *kModels = "/system/vendor/quasar/spotter_models/activation/";
constexpr const char *kCommandModel = "/system/vendor/quasar/spotter_models/command";
// After the wake word a stock command ("Алиса, громче") is handled locally if it is heard
// within this window; the HA pipeline is then cancelled.
constexpr int64_t kCommandAfterWakeMs = 2500;
constexpr const char *kSounds = "/system/vendor/quasar/sounds/";
const std::string kSndWake = std::string(kSounds) + "common/spotter_start.mp3";
const std::string kSndCancel = std::string(kSounds) + "common/spotter_cancel.mp3";
const std::string kSndTimer = std::string(kSounds) + "common/timer.wav";
constexpr float kDuckGain = 0.15f;
// AirPlay / Spotify music while a dialog is on: the louder the speaker, the deeper the dip (the assistant has to stay
// intelligible over loud music): 0.35 at volume <= 10%, falling linearly to 0.12 (-18 dB) at volume >= 50%.
float airplay_duck_gain(float volume) { return std::min(0.35f, std::max(0.12f, 0.35f - 0.575f * (volume - 0.1f))); }
constexpr int64_t kTimerRingMaxMs = 15 * 60 * 1000;
// Longest wait for the next pipeline event before the dialog is given up locally. HA's voice
// command segmenter ends speech capture after at most 15 s of audio. Between INTENT_START and
// INTENT_END nothing arrives (ESPHome forwards INTENT_PROGRESS only once, when TTS streaming
// starts), and conversation agents (LLMs with tool calls) may think for a minute or more.
constexpr int64_t kListenTimeoutMs = 30'000;
constexpr int64_t kProcessTimeoutMs = 120'000;
constexpr uint16_t kSendspinPort = 8928;
// deCONZ-protocol Zigbee coordinator (ZHA socket:// / Zigbee2MQTT tcp://); 6638 is the port
// network Zigbee coordinators conventionally use.
constexpr uint16_t kZigbeePort = 6638;
// Updates: releases of this repository on GitHub. The first check waits for the network after
// boot; an automatic install waits until the speaker is idle (it restarts the daemon).
constexpr const char *kUpdateRepo = "losted-dev/lostos";
constexpr int64_t kUpdateFirstCheckMs = 60'000;
constexpr int64_t kUpdateCheckPeriodMs = 6 * 3600'000LL;
constexpr int64_t kUpdateIdleRetryMs = 10 * 60'000;

std::string read_line(const char *path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

// Index in Clock::face_names(); -1 for "None" / "".
int clock_face_index(const std::string &name) {
  const auto &names = Clock::face_names();
  for (size_t i = 0; i < names.size(); i++)
    if (names[i] == name) return int(i);
  return -1;
}

}  // namespace

// ---------------- Settings ----------------

bool Settings::load(const std::string &path) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    float fv = strtof(v.c_str(), nullptr);
    if (k == "volume2") {
      volume = fv;
      has_volume = true;
    } else if (k == "volume") volume_legacy = fv;
    else if (k == "volume_curve") legacy_on_stock = v == "2";
    else if (k == "muted") muted = v == "1";
    else if (k == "mic_muted") mic_muted = v == "1";
    else if (k == "clock_on") clock_on = v == "1";
    else if (k == "clock_brightness") clock_brightness = fv;
    else if (k == "clock_scroll_ms") clock_scroll_ms = std::clamp(int(fv), 100, 2000);
    else if (k == "light_on") light.on = v == "1";
    else if (k == "light_brightness") light.brightness = fv;
    else if (k == "light_r") light.r = fv;
    else if (k == "light_g") light.g = fv;
    else if (k == "light_b") light.b = fv;
    else if (k == "light_effect") light.effect = v;
    else if (k == "wake_word") wake_word = v;
    else if (k == "tz") tz = v;
    else if (k == "auto_update") auto_update = v == "1";
    else if (k == "nearest_answers") nearest_answers = v == "1";
    else if (k == "update_attempt") update_attempt = v;
    else if (k == "zigbee_role") zigbee_role = ZigbeeService::parse_mode(v);
    else if (k == "zigbee_flash_failed") zigbee_flash_failed = uint32_t(strtoul(v.c_str(), nullptr, 16));
    else if (k == "music_viz") music_viz = v;
    else if (k == "clock_eq") clock_eq = v;
    else if (k == "clock_eq_style") clock_eq_style = v;
    else if (k == "led_speed") led_speed = std::isfinite(fv) ? std::clamp(fv, 0.25f, 4.f) : 1.f;
    else if (k == "voice_indication") voice_indication = v == "1";
    else if (k == "volume_sound") volume_sound = v == "1";
    else if (k == "lyrics") lyrics = v == "1";
    else if (k == "cover_lookup") cover_lookup = v == "1";
    else if (k == "alice_button") alice_button = v == "1";
    else if (k == "led_case_color") led_case_color = v;
  }
  if (clock_eq != "Off" && clock_eq != "Always") clock_eq = "On track change";
  if (clock_eq_style != "Bars") clock_eq_style = "Wave";
  const auto &styles = Leds::music_styles();
  if (std::find(styles.begin(), styles.end(), music_viz) == styles.end()) music_viz = "All styles";
  return true;
}

void Settings::save(const std::string &path) const {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      LOGW("cannot write %s", tmp.c_str());
      return;
    }
    f << "volume2=" << volume << "\nvolume=" << volume_legacy << "\nmuted=" << muted << "\nmic_muted=" << mic_muted
      << "\nclock_on=" << clock_on << "\nclock_brightness=" << clock_brightness
      << "\nclock_scroll_ms=" << clock_scroll_ms << "\nlight_on=" << light.on
      << "\nlight_brightness=" << light.brightness << "\nlight_r=" << light.r << "\nlight_g=" << light.g
      << "\nlight_b=" << light.b << "\nlight_effect=" << light.effect << "\nwake_word=" << wake_word
      << "\ntz=" << tz << "\nauto_update=" << auto_update << "\nupdate_attempt=" << update_attempt
      << "\nzigbee_role=" << ZigbeeService::role(zigbee_role).name << "\nzigbee_flash_failed=" << std::hex
      << zigbee_flash_failed << std::dec << "\nnearest_answers=" << nearest_answers << "\nmusic_viz=" << music_viz << "\nclock_eq=" << clock_eq << "\nclock_eq_style=" << clock_eq_style
      << "\nled_speed=" << led_speed << "\nvoice_indication=" << voice_indication << "\nvolume_sound=" << volume_sound << "\nlyrics=" << lyrics << "\ncover_lookup=" << cover_lookup << "\nalice_button=" << alice_button
      << "\nled_case_color=" << led_case_color << "\n";
  }
  rename(tmp.c_str(), path.c_str());
}

// ---------------- Controller ----------------

Controller::Controller(Loop &loop, const Model &model, SystemPlugins &sysplugins, const std::string &state_path)
    : loop_(loop),
      model_(model),
      state_path_(state_path),
      mic_(loop, model),
      player_(loop),
      mixer_(model),
      leds_(loop, model),
      clock_(loop, model),
      input_(loop, model),
      light_(loop),
      arbiter_(loop),
      system_(sysplugins),
      airplay_(loop, sysplugins),
      bt_(loop),
      dlna_(loop),
      updater_(loop, kUpdateRepo, LOSTOS_VERSION) {}

Controller::~Controller() = default;

std::string Controller::model_dir(const std::string &id) const { return kModels + id; }

bool Controller::start() {
  st_.load(state_path_);
  if (!st_.tz.empty()) {  // clock shows local time before HA connects
    setenv("TZ", st_.tz.c_str(), 1);
    tzset();
  }

  DeviceIdentity id;
  std::string mac = read_line("/sys/class/net/wlan0/address");
  for (auto &c : mac) c = toupper(c);
  std::string suffix;
  for (char c : mac)
    if (c != ':') suffix += tolower(c);
  suffix = suffix.size() >= 6 ? suffix.substr(suffix.size() - 6) : suffix;
  id.name = model_.host_prefix + suffix;
  id.friendly_name = model_.friendly_name;
  id.mac = mac;
  id.model = model_.model_name;
  id.board = model_.codename;
  id.manufacturer = "Yandex";
  id.project_version = LOSTOS_VERSION;

  ApiConfig cfg;
  // The wake words this station has a stock model for (spotter_models/activation/<id>). Alice and
  // Yandex are on every station; Yasmina ships on some (seen on the Mini 3). Home Assistant lists
  // the available ones, the user picks one, and the capture thread loads that model.
  static const WakeWordInfo kWakeWords[] = {
      {"alisa", "Алиса", {"ru"}}, {"yandex", "Яндекс", {"ru"}}, {"yasmina", "Yasmina", {"ar"}}};
  cfg.wake_words.clear();
  for (const auto &w : kWakeWords)
    if (access(model_dir(w.id).c_str(), F_OK) == 0) cfg.wake_words.push_back(w);
  if (cfg.wake_words.empty()) cfg.wake_words.push_back({"alisa", "Алиса", {"ru"}});
  if (std::none_of(cfg.wake_words.begin(), cfg.wake_words.end(), [&](const WakeWordInfo &w) { return w.id == st_.wake_word; }))
    st_.wake_word = cfg.wake_words.front().id;   // the saved word has no model on this station: fall back to the first available
  cfg.active_wake_words = {st_.wake_word};
  cfg.light_effects = Leds::effect_names();
  cfg.music_styles = Leds::music_styles();
  cfg.case_colors = {"None"};
  const auto case_colors = Leds::case_colors();
  cfg.case_colors.insert(cfg.case_colors.end(), case_colors.begin(), case_colors.end());
  cfg.clock_faces = {"None"};
  cfg.clock_faces.insert(cfg.clock_faces.end(), Clock::face_names().begin(), Clock::face_names().end());
  const auto slash = state_path_.rfind('/');
  cfg.key_path = (slash == std::string::npos ? std::string(".") : state_path_.substr(0, slash)) + "/api_key";
  if (!model_.zigbee_tty.empty())
    for (const auto &r : ZigbeeService::kRoles) cfg.zigbee_modes.push_back(r.label);
  api_ = std::make_unique<ApiServer>(loop_, id, cfg, *this);
  arbiter_ok_ = arbiter_.start(id.name);
  // Through the server: stations of one household that share no network still choose the one that hears the owner best
  arbiter_.set_relay([this](const WakeArbiter::Claim &c) { link_.send_wake(c.station, c.wake, c.busy, c.level_db); });
  link_.on_wake = [this](const std::string &station, const std::string &wake, bool busy, float level) {
    loop_.post([this, station, wake, busy, level] { arbiter_.on_remote(WakeArbiter::Claim{station, wake, busy, level}); });
  };
  link_.on_peers = [this](int n) { loop_.post([this, n] { arbiter_.set_remote_peers(n); }); };

  // Hardware first: UI feedback should work even if HA is not connected.
  if (!leds_.open()) LOGW("LEDs unavailable");
  if (!clock_.open()) LOGW("clock unavailable");
  clock_.set_enabled(st_.clock_on);
  clock_.set_brightness(st_.clock_brightness);
  clock_.set_scroll_ms(st_.clock_scroll_ms);
  clock_.set_frame_callback([this](const std::string &f) { if (api_) api_->publish_text_sensor(TextSensorId::DISPLAY, f); });   // the live mirror of the display in the panel
  leds_.set_speed(st_.led_speed);
  leds_.set_voice_indication(st_.voice_indication);
  leds_.set_music_style(st_.music_viz);
  leds_.set_user_light(st_.light.on,
                       Rgb{uint8_t(st_.light.r * 255), uint8_t(st_.light.g * 255), uint8_t(st_.light.b * 255)},
                       st_.light.brightness, st_.light.effect, 0);

  mixer_.set_soft_output([this](float gain) { player_.set_master_gain(gain); });
  if (!mixer_.open()) LOGW("mixer unavailable");
  if (st_.has_volume) {
    // volume2= is authoritative
  } else if (st_.volume_legacy >= 0) {  // written by an earlier build: keep the speaker as loud
    st_.volume = st_.legacy_on_stock ? std::min(1.0f, st_.volume_legacy) : mixer_.from_legacy(st_.volume_legacy);
    LOGI("volume %.3f from an earlier build -> %.3f", st_.volume_legacy, st_.volume);
    persist();
  } else {
    // No volume saved: keep the one the speaker already had, but never start loud: on the Lite 2
    // the softvol control is at 0 dB (100 %) after boot until the Yandex stack sets it. 0.3 is
    // stock level 6 of 20 (Alice's "3").
    st_.volume = std::min(mixer_.volume(), 0.3f);
  }
  mixer_.set_volume(st_.volume);
  mixer_.set_muted(st_.muted);
  if (!st_.led_case_color.empty()) {  // after the volume: persist() writes it too
    const bool listed =
        std::find(case_colors.begin(), case_colors.end(), st_.led_case_color) != case_colors.end();
    if (!listed) LOGW("case colour %s not in this firmware, none used", st_.led_case_color.c_str());
    if (!listed || !leds_.set_case_color(st_.led_case_color)) {  // Leds logs an unreadable file
      st_.led_case_color.clear();
      persist();
    }
  }
  if (!st_.update_attempt.empty()) {
    if (st_.update_attempt == LOSTOS_VERSION) {
      LOGI("updated to %s", LOSTOS_VERSION);
      st_.update_attempt.clear();
      persist();
    } else {
      LOGW("update to %s did not stay installed (rolled back?); automatic updates skip it",
           st_.update_attempt.c_str());
    }
  }
  publish_update();
  loop_.add_timer(kUpdateFirstCheckMs, [this] { check_update(); }, kUpdateCheckPeriodMs);
  if (!player_.init(model_.audio_sink)) LOGE("player init failed");
  // ANNOUNCING is reported only for HA announcements (on_announce / media announce), not for
  // earcons, timer alarms or pipeline TTS that share the ANNOUNCE playback channel.
  player_.on_state([this](Channel, PlayState) { publish_media(); });

  if (input_.open()) {
    hw_mic_muted_ = input_.hw_muted();
    input_.on_mute([this](bool m) {
      {  // two flips of the mic switch: AirPlay on / off; four flips: Bluetooth on / off (decided when the flipping stops)
        const int64_t now = Loop::now_ms();
        if (now >= mic_flip_block_until_ms_) {
          mic_flips_++;
          if (mic_flip_timer_) loop_.cancel_timer(mic_flip_timer_);
          mic_flip_timer_ = loop_.add_timer(1300, [this] {
            mic_flip_timer_ = 0;
            const int n = mic_flips_;
            mic_flips_ = 0;
            if (n == 4) {
              mic_flip_block_until_ms_ = Loop::now_ms() + 2500;
              LOGI("mic switch flipped four times: Bluetooth -> %s", bt_.enabled() ? "off" : "on");
              bluetooth_set(!bt_.enabled(), false);
            } else if (n == 2) {
              bool running = false;
              if (DIR *dir = opendir("/proc")) {
                while (dirent *e = readdir(dir)) {
                  if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                  std::ifstream f(std::string("/proc/") + e->d_name + "/comm");
                  std::string name;
                  if (f && std::getline(f, name) && name.rfind("shairport-sync", 0) == 0) running = true;
                }
                closedir(dir);
              }
              LOGI("mic switch flipped twice: AirPlay -> %s", running ? "off" : "on");
              if (!airplay_.power(!running)) {   // the owner switched AirPlay off in the panel: the mic switch does not bring it back
                LOGI("AirPlay is switched off by the owner, the mic switch does nothing");
              } else {
                air_disabled_ = running;   // the user turned AirPlay off (the sender is not let back in)
                mic_flip_block_until_ms_ = Loop::now_ms() + 4000;   // the toggle takes a few seconds: more flips meanwhile are ignored
                player_.play(Channel::ANNOUNCE, {running ? "file:///data/lostos/sounds/air_off.wav" : "file:///data/lostos/sounds/air_on.wav"});
              }
            }
          });
        }
      }
      if (m) cancel_arbitration();
      hw_mic_muted_ = m;
      LOGI("hardware mic mute: %d", m);
      api_->publish_binary_sensor(BinarySensorId::HW_MIC_MUTE, m);
      if (m && phase_ == Phase::LISTENING) {
        finish_interaction();
      }
      update_detection();
      update_ui();
    });
    input_.on_button([this](Button b, Press p) { button(b, p); });
  } else {
    LOGW("input devices unavailable");
  }

  // AirPlay 2 (shairport-sync): its sender's volume slider drives our volume (with the usual ring / display
  // feedback), its state keeps the stock "pause / louder / next" command spotter listening, and swipes over the
  // touch pads skip tracks in the sender's player.
  airplay_.on_volume = [this](float v) {   // AirPlay's slider, or Spotify's (the volume_changed event of librespot)
    if (airplay_.spotify_playing() || airplay_.spotify_known()) {
      if (std::fabs(v - spotify_vol_) < 0.012f) return;   // the echo of what we have just sent to Spotify
      spotify_vol_ = v;                                   // it comes from the app: it already has it
    }
    set_volume(v, true);
  };
  airplay_.on_cover = [this] {
    ++air_cover_ver_;
    publish_now();
  };
  airplay_.on_track = [this](const std::string &s) {
    api_->publish_text_sensor(TextSensorId::AIRPLAY_NOW, s);
    if (s.empty()) return;
    if (!last_air_track_.empty() && s != last_air_track_ && Loop::now_ms() - skip_ms_ > 4000) track_change(1);
    last_air_track_ = s;
    publish_now();
  };
  airplay_.on_spotify_now = [this](const std::string &s) {
    const size_t tab = s.find('\t');   // "Artist - Title<TAB>cover": the text for people, the picture for the voice server apart
    api_->publish_text_sensor(TextSensorId::SPOTIFY_NOW, s.substr(0, tab));
    spotify_now_ = s;
    spotify_pos_ms_ = 0;   // a new track starts at the beginning (a "pos" line follows when it is not so)
    publish_spotify_extra();
    publish_now();
  };
  airplay_.on_progress = [this](uint32_t pos, uint32_t dur) { publish_playback("airplay", pos, dur); };
  bt_.on_position = [this](uint32_t pos, uint32_t dur) { publish_playback("bluetooth", pos, dur); };
  airplay_.on_spotify_loading = [this] { hub_.report(MediaSource::SPOTIFY, true, Loop::now_ms()); };   // the phone picked this speaker in Spotify
  airplay_.on_spotify_pos = [this](uint32_t ms) {
    spotify_pos_ms_ = ms;
    publish_spotify_extra();
    publish_media_state();
  };
  airplay_.on_spotify_stop = [this] {
    hub_.report(MediaSource::SPOTIFY, false, Loop::now_ms());
    spotify_stopped_ms_ = Loop::now_ms();
    last_music_ms_ = Loop::now_ms();
    last_target_ = MusicTarget::SPOTIFY;   // it was playing until now (also when lostos restarted meanwhile): it can be resumed
    if (st_.clock_eq != "Off") clock_.show_pause();
    publish_media();
  };
  airplay_.on_spotify_track = [this] {
    track_change(Loop::now_ms() - skip_ms_ < 5000 ? skip_dir_ : 1);
  };
  airplay_.on_spotify_start = [this] {
    spotify_vol_ = -1.f;
    push_volume_to_sources();                 // the app's slider shows the speaker's volume from the first second
    last_target_ = MusicTarget::SPOTIFY;  // Spotify takes over: the AirPlay sender is dropped (AirPlay itself stays on)
    if (st_.clock_eq == "On track change") clock_.show_eq(5000);
    hub_.report(MediaSource::SPOTIFY, true, Loop::now_ms());   // Spotify takes over: whoever played before is paused
    last_music_ms_ = Loop::now_ms();
    publish_media();
  };
  airplay_.on_state = [this] {
    const bool s = airplay_.session();
    if (!s) air_cover_ver_ = 0;   // the sender left: its picture goes with it
    prev_air_session_ = s;
    const bool ap = airplay_.playing();
    if (ap) last_target_ = MusicTarget::AIRPLAY;
    if (ap != prev_air_playing_) hub_.report(MediaSource::AIRPLAY, ap, Loop::now_ms());   // AirPlay really starts to play: the others step aside
    if (ap != prev_air_playing_ && st_.clock_eq != "Off") {
      if (ap && st_.clock_eq == "On track change") clock_.show_eq(5000);
      else if (!ap) clock_.show_pause();
    }
    prev_air_playing_ = ap;
    last_music_ms_ = Loop::now_ms();
    publish_media();
    update_music();
    update_commands();
    update_ui();
  };
  input_.on_swipe([this](bool forward) {  // like turning a page: from "+" to "-" (leftwards) = the NEXT track
    if (tap_timer_) {   // the finger crossed the centre pad on its way: that touch is not a tap
      loop_.cancel_timer(tap_timer_);
      tap_timer_ = 0;
    }
    if (music_target() == MusicTarget::NONE) return LOGI("swipe ignored: nothing to skip in");
    transport(forward ? Transport::PREV : Transport::NEXT);
  });
  airplay_.start();
  // native plugins of the speaker: what they may do is only what these functions do (and only with the user's permission)
  phost_.led = [this](int r, int g, int b, int sec) {
    char hex[16];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
    on_led_show("", hex, sec);
  };
  phost_.display = [this](const std::string &text, int sec) { clock_.show_temporary_text(text, sec); };
  phost_.media = [this](const std::string &cmd) { media_command(cmd); };
  phost_.volume = [this] { return st_.volume; };
  phost_.button_claim = [this](bool claimed) { button_claimed_ = claimed; LOGI("centre button %s by a plugin", claimed ? "claimed" : "released"); };
  phost_.publish = [this](const std::string &topic, const std::string &text) { api_->fire_event(EventId::BUTTON, "plugin " + topic + " " + text); };
  phost_.audio_state = [this](bool playing) { hub_.report(MediaSource::PLUGIN, playing, Loop::now_ms()); };
  phost_.play_url = [this](const std::string &url, const std::string &) {   // as media from Home Assistant: the music channel, Sendspin steps aside
    if (sendspin_) sendspin_->yield_to_local();
    player_.play(Channel::MUSIC, {url});
  };
  phost_.stop_url = [this] { player_.stop(Channel::MUSIC); };
  phost_.state_changed = [this](const std::string &) { publish_plugins(); };
  phost_.start();
  link_.on_press = [this](const std::string &code, const std::string &wav) { loop_.post([this, code, wav] { press_begin(code, wav); }); };
  link_.on_press_end = [this] { loop_.post([this] { press_end(); }); };
  link_.on_say = [this](const std::string &path) { loop_.post([this, path] { say_after_boot(path, 0); }); };
  link_.start(model_.friendly_name);
  hub_.send_pause = [this](MediaSource src) {   // the one way to ask a source to stand aside
    switch (src) {
      case MediaSource::AIRPLAY:
        airplay_.send("pause");
        loop_.add_timer(700, [this] { if (hub_.active() != MediaSource::AIRPLAY) drop_airplay_session(); });
        break;
      case MediaSource::SPOTIFY:
        if (!spotify_ctl("pause")) api_->fire_event(EventId::BUTTON, "spotify_pause");
        break;
      case MediaSource::BLUETOOTH: bt_.command("pause"); break;
      case MediaSource::PLUGIN: phost_.audio_stop(); break;
      default: break;
    }
  };
  hub_.on_assumed = [this](MediaSource src) {   // a source that stays silent after the pause is not playing
    LOGI("media hub: source %d took for paused", int(src));
    if (src == MediaSource::AIRPLAY) airplay_.assume_paused();
    else if (src == MediaSource::SPOTIFY) airplay_.spotify_assume_paused();
    else if (src == MediaSource::BLUETOOTH) bt_.assume_paused();
    else if (src == MediaSource::PLUGIN) phost_.audio_stop();
  };
  if (airplay_.spotify_playing()) hub_.report(MediaSource::SPOTIFY, true, Loop::now_ms());   // pulsar restarted while Spotify played
  loop_.add_timer(400, [this] { hub_.tick(Loop::now_ms()); }, 400);
  loop_.add_timer(2000, [this] { publish_sysinfo(); }, 5000);
  bt_.on_volume = [this](float v) {   // the phone's slider moves the speaker's volume
    if (Loop::now_ms() - bt_connected_ms_ < 4000) return;          // right after connecting the phone only reports its own level
    if (std::fabs(v - bt_vol_) < 0.012f) return;                   // the echo of what we have just sent to the phone
    bt_vol_ = v;
    set_volume(v, true);
  };
  bt_.on_state = [this] {
    if (bt_.connected() != prev_bt_connected_) {
      prev_bt_connected_ = bt_.connected();
      if (prev_bt_connected_) {   // a phone has connected: it gets the speaker's volume, not the other way round
        bt_connected_ms_ = Loop::now_ms();
        bt_vol_ = -1.f;
        loop_.add_timer(1500, [this] { push_volume_to_sources(); });
      }
    }
    const bool p = bt_.playing();
    if (p != prev_bt_playing_) {
      prev_bt_playing_ = p;
      last_music_ms_ = Loop::now_ms();
      hub_.report(MediaSource::BLUETOOTH, p, Loop::now_ms());
      if (p) {
        last_target_ = MusicTarget::BT;
        if (st_.clock_eq == "On track change") clock_.show_eq(5000);
      } else if (st_.clock_eq != "Off") {
        clock_.show_pause();
      }
    }
    update_commands();
    update_ui();
    publish_media();
    publish_bt();
  };
  bt_.on_now = [this](const std::string &s) {
    if (!s.empty() && !last_bt_track_.empty() && s != last_bt_track_ && Loop::now_ms() - skip_ms_ > 4000) track_change(1);
    last_bt_track_ = s;
    publish_bt();
    publish_now();
  };
  bt_.start();
  loop_.add_timer(3000, [this] { publish_bt(); }, 10000);
  alarms_load();
  loop_.add_timer(3000, [this] { alarm_led(); });
  loop_.add_timer(1000, [this] { alarm_tick(); }, 1000);
  loop_.add_timer(5000, [this] { publish_wifi(); }, 6000);
  loop_.add_timer(300, [this] { boot_wait(); });

  if (light_.open()) {
    light_.on_lux([this](float lux) {
      lux_ready_ = true;
      clock_.set_ambient_lux(lux);
      leds_.set_ambient_lux(lux);
      int64_t now = Loop::now_ms();
      float diff = std::abs(lux - last_lux_published_);
      if (last_lux_published_ < 0 || diff > std::max(1.0f, last_lux_published_ * 0.1f) || now - last_lux_ms_ > 60000) {
        api_->publish_sensor(SensorId::ILLUMINANCE, lux);
        last_lux_published_ = lux;
        last_lux_ms_ = now;
      }
    });
  }

  mic_.on_wake([this](const std::string &phrase, float level_db) { on_wake(phrase, level_db); });
  mic_.on_command([this](const std::string &word) { on_command(word); });
  mic_.on_audio([this](const int16_t *pcm, size_t n) {
    if (phase_ == Phase::LISTENING || (phase_ == Phase::SPEAKING && barge_enabled_ && !announcing_))
      api_->voice_send_audio(pcm, n);
    else if (arbitrating_)  // the request goes on with what was said right after the wake word
      arb_audio_.insert(arb_audio_.end(), pcm, pcm + n);
  });
  mic_.on_music([this](const MusicFrame &f) {
    leds_.music_frame(f, false);
    clock_.music_frame(f);
  });
  if (!mic_.start(model_dir(st_.wake_word), kCommandModel)) LOGE("microphone front-end failed to start");
  update_detection();

  start_sendspin(id);
  apply_gains();  // a saved mute
  if (!api_->start()) {
    LOGE("ESPHome API server failed to start");
    return false;
  }
  if (sendspin_ && api_->mdns())
    api_->mdns()->add_service("_sendspin._tcp", id.name, kSendspinPort,
                              {"path=/sendspin", "name=" + id.friendly_name});
  start_zigbee(id);
  start_dlna(id);
  update_ui();
  return true;
}

// The speaker as a DLNA media player (Home Assistant finds it by itself): what plays with its cover, and the controls.
void Controller::start_dlna(const DeviceIdentity &id) {
  std::string mac;
  for (char c : id.mac)
    if (isxdigit(static_cast<unsigned char>(c))) mac.push_back(char(tolower(static_cast<unsigned char>(c))));
  mac.resize(12, '0');
  dlna_.get_now = [this] { return now_info(); };
  dlna_.on_action = [this](const std::string &a) {
    if (a == "play") transport(Transport::PLAY);
    else if (a == "pause" || a == "stop") transport(Transport::PAUSE);
    else if (a == "next") transport(Transport::NEXT);
    else if (a == "previous") transport(Transport::PREV);
  };
  dlna_.get_volume = [this] { return st_.volume; };
  dlna_.set_volume = [this](float v) { set_volume(v, true); };
  dlna_.get_mute = [this] { return st_.muted; };
  dlna_.set_mute = [this](bool m) { set_muted(m); };
  if (!dlna_.start(id.friendly_name, "5c0a3d00-7c3e-4a8e-9b2f-" + mac, 49153)) LOGW("DLNA renderer did not start");
}

void Controller::persist() {
  st_.volume_legacy = mixer_.to_legacy(st_.volume);
  st_.save(state_path_);
}

// ---------------- HA connection ----------------

void Controller::on_client_connected(const std::string &client_info) {
  LOGI("Home Assistant connected: %s", client_info.c_str());
  error_shown_ = false;
  publish_media();
  api_->publish_switch(SwitchId::MIC_MUTE, st_.mic_muted);
  api_->publish_switch(SwitchId::CLOCK, st_.clock_on);
  api_->publish_switch(SwitchId::AUTO_UPDATE, st_.auto_update);
  api_->publish_switch(SwitchId::NEAREST_ANSWERS, st_.nearest_answers);
  api_->publish_switch(SwitchId::VOICE_INDICATION, st_.voice_indication);
  api_->publish_switch(SwitchId::VOLUME_SOUND, st_.volume_sound);
  api_->publish_switch(SwitchId::LYRICS, st_.lyrics);
  api_->publish_switch(SwitchId::COVER_LOOKUP, st_.cover_lookup);
  api_->publish_switch(SwitchId::ALICE_BUTTON, st_.alice_button);
  eq_publish();
  publish_update();
  api_->publish_binary_sensor(BinarySensorId::HW_MIC_MUTE, hw_mic_muted_);
  api_->publish_number(NumberId::CLOCK_BRIGHTNESS, st_.clock_brightness * 100.f);
  api_->publish_number(NumberId::CLOCK_SCROLL, float(st_.clock_scroll_ms));
  api_->publish_text(TextId::CLOCK_TEXT, clock_text_);
  api_->publish_select(SelectId::CLOCK_FACE, clock_face_);
  api_->publish_number(NumberId::LED_SPEED, st_.led_speed);
  api_->publish_select(SelectId::MUSIC_VIZ, st_.music_viz);
  api_->publish_select(SelectId::CLOCK_EQ, st_.clock_eq);
  api_->publish_select(SelectId::CLOCK_EQ_STYLE, st_.clock_eq_style);
  clock_.set_eq_style(st_.clock_eq_style == "Bars");
  api_->publish_select(SelectId::CASE_COLOR, st_.led_case_color.empty() ? "None" : st_.led_case_color);
  if (zigbee_) {
    api_->publish_select(SelectId::ZIGBEE_MODE, ZigbeeService::role(st_.zigbee_role).label);
    api_->publish_text_sensor(TextSensorId::ZIGBEE, zigbee_->status());
  }
  api_->publish_light(st_.light);
  if (last_lux_published_ >= 0) api_->publish_sensor(SensorId::ILLUMINANCE, last_lux_published_);
}

void Controller::on_client_disconnected() {
  LOGW("Home Assistant disconnected");
  voice_link_lost();
}

void Controller::on_voice_subscribed(bool subscribed) {
  LOGI("voice assistant %s", subscribed ? "subscribed" : "unsubscribed");
  if (!subscribed) {
    server_last_ms_ = 0;  // whoever served the voice is gone (Home Assistant takes over, it has no speech-to-text): the
                          // assistant server announces itself again right after it subscribes
    voice_link_lost();
  }
}

// No more events come for the current run, and a stop would reach whichever client subscribes
// next. A dialog still listening or thinking ends now. An answer already playing is fetched over
// HTTP independently of the API link, so it plays out (still ducked) and its end closes the
// dialog; a stream that dies with HA ends it through the player instead.
void Controller::voice_link_lost() {
  awaiting_run_start_ = run_active_ = stale_run_end_ = false;
  continue_conversation_ = false;
  if (phase_ == Phase::LISTENING || phase_ == Phase::PROCESSING) finish_interaction();
}

void Controller::on_time(uint32_t epoch, const std::string &tz) {
  if (!tz.empty()) {
    setenv("TZ", tz.c_str(), 1);
    tzset();
    if (tz != st_.tz) {
      st_.tz = tz;
      persist();
    }
  }
  {  // right away: the system clock follows the server, the nonsense a fresh boot starts with is gone at once
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (epoch > 1700000000u && std::labs(long(tv.tv_sec) - long(epoch)) > 2) {
      tv.tv_sec = epoch;
      tv.tv_usec = 0;
      if (settimeofday(&tv, nullptr) == 0) LOGI("system time set from the server");
    }
  }
  time_ready_ = true;
  LOGI("time from HA: epoch=%u tz='%s' (local clock now follows TZ)", epoch, tz.c_str());
}

// ---------------- voice pipeline ----------------

void Controller::update_detection() { mic_.set_detection(!st_.mic_muted && !hw_mic_muted_); }

void Controller::on_wake(const std::string &phrase, float level_db) {
  if (st_.mic_muted || hw_mic_muted_) return;
  LOGI("wake word '%s', level %.1f dB", phrase.c_str(), level_db);
  pre_wake_ = mic_.recent(16000 * 5);
  if (timer_ringing_) {  // the wake word works as a stop word while ringing
    // A ringing station takes the turn so that its neighbours do not start a dialog.
    cancel_arbitration();
    if (arbitration_on()) arbiter_.claim(st_.wake_word, level_db, true, nullptr);
    return stop_timer_ring();
  }
  if (phase_ == Phase::LISTENING || phase_ == Phase::PROCESSING) {
    // This station is in a dialog already and keeps the turn: its neighbours that heard the same "Алиса" must not start a dialog of their own.
    if (!arbitrating_ && arbitration_on()) arbiter_.claim(st_.wake_word, level_db, true, nullptr);
    return;
  }
  if (arbitrating_) return;
  // Without the HA pipeline this station could win and stay silent: it does not take part.
  if (!arbitration_on() || !api_->voice_subscribed()) return answer_wake(phrase, {});
  arbitrating_ = true;
  commands_blocked_until_ms_ = 0;  // a station that lost the previous wake word may win this one
  arb_phrase_ = phrase;
  arb_level_ = level_db;
  arb_audio_.clear();
  arb_command_.clear();
  mic_.set_streaming(true);
  update_commands();
  update_ui();
  arbiter_.claim(st_.wake_word, level_db, phase_ == Phase::SPEAKING || announcing_,
                 [this](bool won, const WakeArbiter::Claim &best, size_t peers) { arbitration_done(won, best, peers); });
}

void Controller::answer_wake(const std::string &phrase, std::vector<int16_t> preroll) {
  if (timer_ringing_) return stop_timer_ring();
  if (phase_ == Phase::IDLE && access("/tmp/lostos-boot-done", F_OK) == 0 && !server_alive()) return local_wake();
  if (phase_ == Phase::LISTENING || phase_ == Phase::PROCESSING) return;
  if (phase_ == Phase::SPEAKING || announcing_) interrupt_speech();  // barge-in: cut the answer short
  start_pipeline(phrase, true, std::move(preroll));
}

void Controller::arbitration_done(bool won, const WakeArbiter::Claim &best, size_t peers) {
  arbitrating_ = false;
  std::vector<int16_t> audio = std::move(arb_audio_);
  std::string command = std::move(arb_command_);
  arb_audio_.clear();
  arb_command_.clear();
  if (!won) {
    mic_.set_streaming(false);
    LOGI("wake word: %s answers (%s%.1f dB, ours %.1f dB)", best.station.c_str(), best.busy ? "busy, " : "",
         best.level_db, arb_level_);
    // The command spotter of a playing station would still act on "Алиса, громче".
    commands_blocked_until_ms_ = Loop::now_ms() + kCommandAfterWakeMs;
    update_commands();
    update_ui();
    return;
  }
  LOGI("wake word: this station answers (%zu other station(s) heard it)", peers);
  if (!command.empty()) {  // "Алиса, громче": handled locally, as right after the wake word
    mic_.set_streaming(false);
    update_commands();
    update_ui();
    return on_command(command);
  }
  // Capture keeps streaming into the new dialog: switching it off here could drop the chunk
  // between the preroll and live audio.
  answer_wake(arb_phrase_, std::move(audio));
  if (phase_ != Phase::LISTENING) mic_.set_streaming(false);  // no dialog started (e.g. HA gone)
}

void Controller::cancel_arbitration() {
  if (!arbitrating_) return;
  arbiter_.cancel();
  arbitrating_ = false;
  arb_audio_.clear();
  arb_command_.clear();
  if (phase_ != Phase::LISTENING) mic_.set_streaming(false);
}

void Controller::start_pipeline(const std::string &phrase, bool earcon, std::vector<int16_t> preroll) {
  cancel_arbitration();  // the button, a continued conversation or HA's start_conversation win
  if (!api_->voice_subscribed()) {
    LOGW("wake ignored: Home Assistant voice assistant not subscribed");
    leds_.set_ui_state(UiState::ERROR);
    loop_.add_timer(1500, [this] { update_ui(); });
    return;
  }
  LOGI("starting voice pipeline (wake='%s')", phrase.c_str());
  commands_blocked_until_ms_ = 0;
  phase_ = Phase::LISTENING;
  dialog_++;
  stale_run_end_ = run_active_;
  awaiting_run_start_ = true;
  stop_sent_ = false;
  listen_started_ms_ = Loop::now_ms();
  loop_.add_timer(kCommandAfterWakeMs + 100, [this] { update_commands(); });
  tts_url_.clear();
  tts_played_ = false;
  continue_conversation_ = false;
  update_duck();
  if (earcon) player_.play(Channel::ANNOUNCE, {kSndWake});
  const uint64_t dialog = dialog_;
  api_->voice_request_start(phrase);
  if (dialog != dialog_) return;  // sending failed and closed the voice client, which ended the dialog
  // Exactly 5 s of context come first (what was said up to the wake word; silence for a button or a continued
  // conversation): the assistant on the server splits the stream there and judges whom the speech was meant for.
  {
    std::vector<int16_t> ctx = pre_wake_.size() == 16000 * 5 ? std::move(pre_wake_) : std::vector<int16_t>(16000 * 5, 0);
    pre_wake_.clear();
    for (size_t i = 0; i < ctx.size() && dialog == dialog_; i += 512)
      api_->voice_send_audio(ctx.data() + i, std::min<size_t>(512, ctx.size() - i));
    if (dialog != dialog_) return;
  }
  // HA drops audio queued before the start and queues what follows it on the same connection.
  for (size_t i = 0; i < preroll.size() && dialog == dialog_; i += 512)
    api_->voice_send_audio(preroll.data() + i, std::min<size_t>(512, preroll.size() - i));
  if (dialog != dialog_) return;  // the audio backlog overflowed and aborted the dialog
  mic_.set_streaming(true);
  arm_watchdog();
  update_ui();
}

void Controller::stop_streaming() {
  // The microphone stays open while she thinks and speaks (with barge-in on): a shout like "игнорируй, не отвечай" cancels
  // what she was about to do; a wake word heard meanwhile keeps its audio.
  mic_.set_streaming(arbitrating_ || barge_enabled_);
  if (phase_ == Phase::LISTENING) phase_ = Phase::PROCESSING;
}

void Controller::on_voice_event(VaEvent type, const std::map<std::string, std::string> &data) {
  auto get = [&](const char *k) -> std::string {
    auto it = data.find(k);
    return it == data.end() ? std::string() : it->second;
  };
  // Numbers: VaEvent (1 run-start, 2 run-end, 3/4 stt, 5/6 intent, 7/8 tts, 11/12 vad, 0 error).
  if (type != VaEvent::INTENT_PROGRESS) LOGI("voice event %u", (unsigned)type);
  if (type == VaEvent::RUN_START) {
    run_active_ = true;
    awaiting_run_start_ = false;
  } else if (awaiting_run_start_) {
    if (type != VaEvent::RUN_END || stale_run_end_) {
      if (type == VaEvent::RUN_END) stale_run_end_ = run_active_ = false;
      LOGD("voice event %u: from the replaced run, ignored", (unsigned)type);
      return;
    }
    // A RUN_END with no run to replace: HA ended our request without running it (it intercepts
    // the wake word while an automation waits for one).
    awaiting_run_start_ = false;
  } else if (type == VaEvent::RUN_END) {
    run_active_ = false;
  }
  if (phase_ == Phase::IDLE) {  // the run of a dialog that already ended; it is being stopped
    LOGD("voice event %u: dialog already ended, ignored", (unsigned)type);
    return;
  }
  switch (type) {
    case VaEvent::RUN_START:
      tts_url_ = get("url");
      tts_played_ = false;
      continue_conversation_ = false;
      break;
    case VaEvent::STT_VAD_END:
    case VaEvent::STT_END:
      stop_streaming();
      update_ui();
      break;
    case VaEvent::INTENT_START:
      if (phase_ == Phase::LISTENING) stop_streaming();
      update_ui();
      break;
    case VaEvent::INTENT_PROGRESS:
      if (get("tts_start_streaming") == "1") play_tts();
      break;
    case VaEvent::INTENT_END:
      continue_conversation_ = get("continue_conversation") == "1";
      break;
    case VaEvent::TTS_END:
      if (!get("url").empty()) tts_url_ = get("url");
      play_tts();
      break;
    case VaEvent::ERROR:
      // HA accepts one wake word phrase per 2 s across satellites: another one answers.
      if (get("code") == "duplicate_wake_up_detected") {
        LOGI("pipeline: another satellite answers this wake word (HA duplicate wake-up)");
        stop_streaming();
        break;
      }
      LOGW("pipeline error: %s %s", get("code").c_str(), get("message").c_str());
      stop_streaming();
      error_shown_ = true;
      leds_.set_ui_state(UiState::ERROR);
      if (get("code") != "stt-no-text-recognized") play_closing_earcon(kSounds + model_.error_sound);
      break;
    case VaEvent::RUN_END:
      stop_streaming();
      if (!tts_played_) finish_interaction();
      break;
    default:
      break;
  }
  arm_watchdog();
}

void Controller::play_tts() {
  if (tts_played_ || tts_url_.empty()) return;
  tts_played_ = true;
  phase_ = Phase::SPEAKING;
  if (barge_enabled_) mic_.set_streaming(true);   // somebody may talk over the answer
  update_ui();
  player_.play(Channel::ANNOUNCE, {tts_url_}, [this, dialog = dialog_](bool ok) {
    if (dialog == dialog_ && phase_ == Phase::SPEAKING) tts_finished(ok);
    resume_ring();
  });
}

// A cut-short or failed answer does not reopen the microphone, nor does a ringing timer.
void Controller::tts_finished(bool ok) {
  if (ok && continue_conversation_ && !timer_ringing_ && !st_.mic_muted && !hw_mic_muted_) {
    continue_conversation_ = false;
    start_pipeline("", false);
    return;
  }
  finish_interaction();
}

void Controller::finish_interaction() {
  // The dialog may end before its run does (answer cut short, cancel, timeout): stop the run so
  // HA does not go on generating; whatever it still sends is dropped while idle.
  if ((run_active_ || awaiting_run_start_) && !stop_sent_) {
    api_->voice_request_stop();
    stop_sent_ = true;
  }
  awaiting_run_start_ = false;
  mic_.set_streaming(arbitrating_);
  phase_ = Phase::IDLE;
  dialog_++;
  tts_played_ = false;
  tts_url_.clear();
  continue_conversation_ = false;
  update_duck();
  arm_watchdog();
  if (error_shown_) {
    error_shown_ = false;
    loop_.add_timer(1200, [this] { update_ui(); });
  } else {
    update_ui();
  }
}

// The dialog failed without a pipeline run to end it: error ring, then idle.
void Controller::abort_dialog(const char *why) {
  LOGW("voice pipeline: %s", why);
  error_shown_ = true;
  leds_.set_ui_state(UiState::ERROR);
  finish_interaction();
}

void Controller::on_voice_start_failed() {
  if (phase_ != Phase::LISTENING && phase_ != Phase::PROCESSING) return;
  awaiting_run_start_ = false;  // no run follows
  abort_dialog("Home Assistant refused to start it");
}

void Controller::on_voice_audio_overflow() {
  if (phase_ != Phase::LISTENING && phase_ != Phase::PROCESSING) return;
  stop_sent_ = true;  // by the server itself
  abort_dialog("network too slow for the microphone audio");
}

// Without it a run HA never ends (lost connection state, a hung agent) would leave the ring
// listening, the microphone streaming and the wake word ignored.
void Controller::arm_watchdog() {
  if (watchdog_) loop_.cancel_timer(watchdog_);
  watchdog_ = 0;
  if (phase_ != Phase::LISTENING && phase_ != Phase::PROCESSING) return;
  watchdog_ = loop_.add_timer(phase_ == Phase::LISTENING ? kListenTimeoutMs : kProcessTimeoutMs, [this] {
    watchdog_ = 0;
    abort_dialog("no progress from Home Assistant, giving up");
  });
}

// Cuts the TTS answer or HA announcement short. An interrupted announcement ends here (its
// completion callback then only reports to HA); the dialog of an interrupted answer is ended or
// replaced by the caller, otherwise by the TTS completion callback.
void Controller::interrupt_speech() {
  player_.stop(Channel::ANNOUNCE);
  if (announcing_) {
    announcing_ = false;
    announce_seq_++;
  }
}

// Error and cancel sounds outlive the dialog they close; music stays ducked until they end.
void Controller::play_closing_earcon(const std::string &path) {
  closing_earcon_ = true;
  const uint64_t seq = ++earcon_seq_;
  player_.play(Channel::ANNOUNCE, {path}, [this, seq](bool) {
    if (seq != earcon_seq_) return;
    closing_earcon_ = false;
    update_duck();
  });
}

// Announcements do not take over the dialog phase, and their end leaves any dialog alone. HA
// cancels its own running pipeline before it announces (assist_satellite async_internal_announce),
// so a dialog still active here is either ending (its RUN_END or the watchdog follows) or was
// started by HA after the announcement.
void Controller::on_announce(const AnnounceRequest &req) {
  LOGI("announce: '%s'%s", req.text.c_str(), phase_ == Phase::IDLE ? "" : " (during a dialog)");
  std::vector<std::string> urls;
  if (!req.preannounce_media_id.empty()) urls.push_back(req.preannounce_media_id);
  urls.push_back(req.media_id);
  announcing_ = true;
  const bool conv = req.start_conversation;
  const uint64_t seq = ++announce_seq_;
  update_duck();
  update_ui();
  publish_media();
  player_.play(Channel::ANNOUNCE, urls, [this, conv, seq](bool ok) {
    api_->voice_announce_finished(ok);  // exactly once per announcement, also when cut short
    if (seq == announce_seq_) {
      announcing_ = false;
      if (ok && conv && phase_ == Phase::IDLE && !timer_ringing_ && !st_.mic_muted && !hw_mic_muted_)
        start_pipeline("", false);
      end_announce();
    }
    resume_ring();
  });
}

void Controller::end_announce() {
  update_duck();
  update_ui();
  publish_media();
}

// ---------------- timers ----------------

void Controller::on_timer_event(const TimerInfo &t) {
  LOGI("timer %u '%s' (%s) left=%u active=%d", (unsigned)t.event, t.name.c_str(), t.id.c_str(), t.seconds_left,
       t.is_active);
  switch (t.event) {
    case TimerEvent::STARTED:
      if (!timers_.count(t.id)) leds_.show_timer_started();
      timers_[t.id] = HaTimer{t.is_active, Loop::now_ms() + int64_t(t.seconds_left) * 1000, t.seconds_left};
      break;
    case TimerEvent::UPDATED:
      timers_[t.id] = HaTimer{t.is_active, Loop::now_ms() + int64_t(t.seconds_left) * 1000, t.seconds_left};
      break;
    case TimerEvent::CANCELLED:
      timers_.erase(t.id);
      break;
    case TimerEvent::FINISHED:
      timers_.erase(t.id);
      if (t.name.rfind("alarm:", 0) == 0) {
        ring_label_ = t.name.substr(6);
        ring_tries_ = 0;
      }
      start_ring(t.name.rfind("alarm:", 0) == 0);
      break;
  }
  show_timers();
}

// The clock shows the running timer that ends first, else the paused one with the least time
// left. The clock counts a running one down by itself, so it is set only when the choice or the
// timer's data changes, and looked at again when the shown one runs out.
void Controller::show_timers() {
  const int64_t now = Loop::now_ms();
  const std::string *best_id = nullptr;
  const HaTimer *best = nullptr;
  int64_t best_left = 0;
  for (const auto &[id, tm] : timers_) {
    const int64_t left = tm.active ? tm.end_ms - now : int64_t(tm.seconds_left) * 1000;
    if (left <= 0) continue;  // ran out locally; HA's FINISHED follows
    if (!best || (tm.active && !best->active) || (tm.active == best->active && left < best_left)) {
      best_id = &id;
      best = &tm;
      best_left = left;
    }
  }
  if (timer_tick_) loop_.cancel_timer(timer_tick_);
  timer_tick_ = 0;
  if (!best) {
    if (!shown_timer_.empty()) clock_.set_timer(0, true);
    shown_timer_.clear();
    return;
  }
  const bool same = *best_id == shown_timer_ && best->active == shown_.active &&
                    (best->active ? best->end_ms == shown_.end_ms : best->seconds_left == shown_.seconds_left);
  if (!same) {
    clock_.set_timer(best_left, best->active);
    shown_timer_ = *best_id;
    shown_ = *best;
  }
  if (best->active) timer_tick_ = loop_.add_timer(best_left, [this] {
    timer_tick_ = 0;
    show_timers();
  });
}

void Controller::start_ring(bool alarm) {
  if (timer_ringing_) return;
  timer_ringing_ = true;
  timer_ring_started_ms_ = Loop::now_ms();
  ring_alarm_ = alarm;
  ring_max_ms_ = alarm ? 5 * 60 * 1000 : 60 * 1000;
  if (alarm) {  // wakes up gently: from the quietest to the loudest in three minutes
    ring_saved_volume_ = st_.volume;
    mixer_.set_volume(0.02f);
    if (ring_ramp_tick_) loop_.cancel_timer(ring_ramp_tick_);
    ring_ramp_tick_ = loop_.add_timer(1000, [this] { ring_ramp(); }, 1000);
  }
  clock_.set_ringing(true);
  update_duck();
  update_ui();
  ring_timer();
}

// ---------------- alarms (kept on the speaker) ----------------

namespace {
constexpr const char *kAlarmFile = "/data/lostos/alarms.txt";
}

// One line per alarm: id TAB snooze TAB tries TAB epochs,comma,separated TAB label [TAB repeat-after minutes]
void Controller::alarms_load() {
  alarms_.clear();
  std::ifstream f(kAlarmFile);
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> p;
    size_t s = 0, e;
    while (p.size() < 5 && (e = line.find('\t', s)) != std::string::npos) {
      p.push_back(line.substr(s, e - s));
      s = e + 1;
    }
    p.push_back(line.substr(s));
    if (p.size() != 5 && p.size() != 6) continue;
    Alarm a;
    a.id = p[0];
    a.snooze = p[1] == "1";
    a.reminder = p[1] == "2";
    a.tries = atoi(p[2].c_str());
    a.label = p[4];
    if (p.size() == 6) a.snooze_min = std::max(0, std::min(60, atoi(p[5].c_str())));
    std::stringstream ss(p[3]);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      const long long v = strtoll(tok.c_str(), nullptr, 10);
      if (v > 0) a.at.push_back(v);
    }
    if (!a.at.empty()) alarms_.push_back(std::move(a));
  }
  LOGI("alarms: %zu stored", alarms_.size());
}

void Controller::alarms_save() const {
  const std::string tmp = std::string(kAlarmFile) + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    for (const auto &a : alarms_) {
      std::string csv;
      for (int64_t v : a.at) csv += (csv.empty() ? "" : ",") + std::to_string((long long)v);
      fprintf(f, "%s\t%d\t%d\t%s\t%s\t%d\n", a.id.c_str(), a.reminder ? 2 : a.snooze ? 1 : 0, a.tries, csv.c_str(), a.label.c_str(), a.snooze_min);
    }
    fclose(f);
    rename(tmp.c_str(), kAlarmFile);
  }
}

void Controller::alarm_led() { clock_.set_alarm_led(!alarms_.empty()); }

namespace {
constexpr const char *kRemDir = "/data/lostos/reminders/";

bool safe_id(const std::string &id) {
  return !id.empty() && id.size() <= 32 &&
         std::all_of(id.begin(), id.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) != 0; });
}

std::deque<int64_t> parse_epochs(const std::string &csv) {
  std::vector<int64_t> v;
  std::stringstream ss(csv);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    const long long x = strtoll(tok.c_str(), nullptr, 10);
    if (x > 0) v.push_back(x);
  }
  std::sort(v.begin(), v.end());
  return std::deque<int64_t>(v.begin(), v.end());
}
}  // namespace

// From the assistant server: "alarm_set <id> <epochs> <label>" | "reminder_set <id> <epochs> <url> <label>" |
// "alarm_del <id>" | "alarm_clear" | "alarm_gc" (files of reminders that are gone).
void Controller::alarm_command(const std::string &cmd) {
  const bool is_set = cmd.rfind("alarm_set ", 0) == 0, is_rem = cmd.rfind("reminder_set ", 0) == 0;
  if (cmd == "alarm_clear") {
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const Alarm &a) { return !a.snooze; }), alarms_.end());
  } else if (cmd == "alarm_gc") {
    if (DIR *d = opendir(kRemDir)) {
      while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
        const std::string id = name.substr(0, name.size() - 4);
        if (std::none_of(alarms_.begin(), alarms_.end(), [&](const Alarm &a) { return a.reminder && a.id == id; }))
          unlink((std::string(kRemDir) + name).c_str());
      }
      closedir(d);
    }
    return;
  } else if (cmd.rfind("alarm_del ", 0) == 0) {
    const std::string id = cmd.substr(10);
    if (!safe_id(id)) return;
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [&](const Alarm &a) { return !a.snooze && a.id == id; }), alarms_.end());
    unlink((std::string(kRemDir) + id + ".wav").c_str());
  } else if (is_set || is_rem) {
    size_t pos = cmd.find(' ') + 1;
    auto word = [&]() {
      const size_t e = cmd.find(' ', pos);
      std::string w = cmd.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      pos = e == std::string::npos ? cmd.size() : e + 1;
      return w;
    };
    Alarm a;
    a.id = word();
    const std::string csv = word();
    std::string url;
    if (is_rem) url = word();
    if (is_set && cmd.compare(pos, 2, "~z") == 0) {      // "~z<minutes>": how long until a ring nobody stopped comes back (0 = never)
      const std::string z = word();
      a.snooze_min = std::max(0, std::min(60, atoi(z.c_str() + 2)));
    }
    a.label = pos < cmd.size() ? cmd.substr(pos) : "";
    for (char &c : a.label)
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (!safe_id(a.id)) return;
    a.reminder = is_rem;
    a.at = parse_epochs(csv);
    if (is_rem && !a.at.empty()) {
      if (url.rfind("http://", 0) != 0 || url.size() > 300) return;
      alarm_fetch(a.id, url);
    }
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [&](const Alarm &x) { return !x.snooze && x.id == a.id; }), alarms_.end());
    if (!a.at.empty()) alarms_.push_back(std::move(a));
  } else {
    return;
  }
  LOGI("alarms: '%.40s' -> %zu stored", cmd.c_str(), alarms_.size());
  alarms_save();
  alarm_led();
}

// The phrase of a reminder is fetched from the server once, when the reminder is set (a child process, so that nothing waits).
void Controller::alarm_fetch(const std::string &id, const std::string &url) {
  const std::string file = std::string(kRemDir) + id + ".wav";
  if (access(file.c_str(), R_OK) == 0) return;
  mkdir(kRemDir, 0755);
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    if (fork() == 0) {
      setsid();
      for (int fd = 3; fd < 1024; fd++) close(fd);
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
      execl("/bin/sh", "sh", "-c", "wget -q -T 15 -O \"$1.part\" \"$2\" && mv \"$1.part\" \"$1\" || rm -f \"$1.part\"", "sh", file.c_str(),
            url.c_str(), static_cast<char *>(nullptr));
    }
    _exit(0);
  }
  waitpid(pid, nullptr, 0);
}

// A button of the speaker (input.cpp), or the Alice button pressed by SIGUSR1 (press_action_button).
void Controller::button(Button b, Press p) {
  if (b == Button::ACTION) LOGI("button: action %s, press_waiting=%d", p == Press::LONG ? "long" : "short", int(press_waiting_));
  phost_.event("button", b == Button::ACTION ? (p == Press::LONG ? "action_long" : "action") : b == Button::VOLUME_UP ? "volume_up" : "volume_down");
  if (b == Button::ACTION && press_waiting_ && p != Press::LONG) {   // the owner is connecting this station: this press is the proof that the speaker is in his hands
    press_waiting_ = false;
    clock_.clear_code();
    link_.press_pressed();
    player_.play(Channel::ANNOUNCE, {kSndWake});
    return;
  }
  if (b == Button::ACTION) {
    api_->fire_event(EventId::BUTTON, p == Press::LONG ? "action_long" : "action");
    if (p == Press::LONG) return;  // exposed to HA automations only
    if (sendspin_pairing_prompt_ && sendspin_) return sendspin_->confirm_pairing_window();
    if (timer_ringing_) return stop_timer_ring();
    if (phase_ == Phase::LISTENING || phase_ == Phase::PROCESSING) {
      interrupt_speech();  // an announcement playing over the dialog goes with it
      play_closing_earcon(kSndCancel);
      return finish_interaction();
    }
    if (phase_ == Phase::SPEAKING || announcing_) {
      interrupt_speech();
      return finish_interaction();
    }
    if (phase_ == Phase::IDLE) {  // a second tap within a moment starts Spotify; a single one acts after the wait
      if (button_claimed_) {   // a plugin took the button: it gets the event above and decides what a tap does ...
        if (!music_mode()) return;
        LOGI("centre button: a plugin holds it, but music is playing: the tap works as pause / play");   // ... except while music plays: it is always pause / play
      }
      if (tap_timer_) {
        loop_.cancel_timer(tap_timer_);
        tap_timer_ = 0;
        last_music_ms_ = Loop::now_ms();
        LOGI("double tap: Spotify");
        api_->fire_event(EventId::BUTTON, spotify_play_event(true));
        return;
      }
      tap_timer_ = loop_.add_timer(350, [this] {
        tap_timer_ = 0;
        centre_single_tap();
      });
      return;
    }
    if (st_.alice_button) start_pipeline("", true);
  } else {
    api_->fire_event(EventId::BUTTON, b == Button::VOLUME_UP ? "volume_up" : "volume_down");
    set_volume(st_.volume + (b == Button::VOLUME_UP ? 0.05f : -0.05f), true);
  }
}

// SIGUSR1 to the daemon (root on the speaker only): a short press of the Alice button, through the same path as the real one.
// Used where nobody stands at the speaker, e.g. to confirm the station on the server.
void Controller::press_action_button() {
  LOGI("button: Alice button pressed by SIGUSR1");
  button(Button::ACTION, Press::SHORT);
}

// The owner confirmed this station on his server: show the first letters of its id on the clock, say the line, wait for the Alice button.
void Controller::press_begin(const std::string &code, const std::string &wav) {
  press_waiting_ = true;
  press_wav_ = wav;
  if (!code.empty()) clock_.show_code(code);
  if (!wav.empty()) play_reminder(wav, false);
  if (press_timer_) loop_.cancel_timer(press_timer_);
  press_timer_ = loop_.add_timer(22000, [this] { if (press_waiting_ && !announcing_ && !press_wav_.empty()) play_reminder(press_wav_, false); }, 22000);   // the line again while it waits
}

void Controller::press_end() {
  if (press_timer_) loop_.cancel_timer(press_timer_);
  press_timer_ = 0;
  if (press_waiting_) clock_.clear_code();
  press_waiting_ = false;
  if (!press_wav_.empty()) unlink(press_wav_.c_str());
  press_wav_.clear();
}

void Controller::play_reminder(const std::string &path, bool delete_after) {
  announcing_ = true;
  const uint64_t seq = ++announce_seq_;
  update_duck();
  update_ui();
  player_.play(Channel::ANNOUNCE, {"file://" + path}, [this, seq, path, delete_after](bool) {
    if (delete_after) unlink(path.c_str());   // said once: the phrase is not kept
    if (seq == announce_seq_) {
      announcing_ = false;
      end_announce();
    }
    resume_ring();
  });
}

// Once a second: an alarm that is due rings by itself and a reminder says its phrase (no server needed); a timer whose
// end the server did not announce within three seconds rings too.
void Controller::alarm_tick() {
  const int64_t now_ms = Loop::now_ms();
  bool ring_timer_now = false;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (it->second.active && now_ms > it->second.end_ms + 3000) {
      LOGW("timer %s ran out and the server did not say so: it rings on its own", it->first.c_str());
      it = timers_.erase(it);
      ring_timer_now = true;
    } else {
      ++it;
    }
  }
  if (ring_timer_now) {
    start_ring(false);
    show_timers();
  }
  if (!time_ready_ || alarms_.empty()) return;
  const int64_t now = time(nullptr);
  struct Say {
    std::string id;
    bool last;
  };
  std::vector<Say> say;
  bool changed = false, fire = false;
  std::string label;
  int tries = 0, snooze_min = 5;
  for (auto &a : alarms_) {
    while (!a.at.empty() && a.at.front() <= now) {
      const int64_t late = now - a.at.front();
      a.at.pop_front();
      changed = true;
      if (late > 600) continue;  // the speaker was off at that time: too late to wake or remind anybody
      if (a.reminder) {
        say.push_back({a.id, a.at.empty()});
      } else if (!fire && !timer_ringing_) {
        fire = true;
        label = a.label;
        tries = a.tries;
        snooze_min = a.snooze_min;
      }
    }
  }
  if (changed) {
    // a reminder that has no moments left: its phrase goes after it was said (below), or now when it was not due
    std::vector<std::string> gone;
    for (const auto &a : alarms_)
      if (a.at.empty() && a.reminder && std::none_of(say.begin(), say.end(), [&](const Say &s) { return s.id == a.id; })) gone.push_back(a.id);
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const Alarm &a) { return a.at.empty(); }), alarms_.end());
    for (const auto &id : gone) unlink((std::string(kRemDir) + id + ".wav").c_str());
    alarms_save();
    alarm_led();
  }
  for (const auto &s : say) {
    const std::string path = std::string(kRemDir) + s.id + ".wav";
    if (access(path.c_str(), R_OK) == 0) {
      LOGI("reminder %s: the prepared phrase is played", s.id.c_str());
      play_reminder(path, s.last);
    } else {
      LOGW("reminder %s: no phrase on the speaker, a plain signal instead", s.id.c_str());
      player_.play(Channel::ANNOUNCE, {kSndTimer});
    }
  }
  if (fire) {
    LOGI("alarm '%s' rings (try %d)", label.c_str(), tries + 1);
    ring_label_ = label;
    ring_tries_ = tries;
    ring_snooze_min_ = snooze_min;
    start_ring(true);
  }
}

// The ring gives the ANNOUNCE channel to speech (TTS answer, HA announcement) and resumes after it.
void Controller::ring_timer() {
  if (timer_ring_tick_) loop_.cancel_timer(timer_ring_tick_);
  timer_ring_tick_ = 0;
  if (!timer_ringing_) return;
  if (Loop::now_ms() - timer_ring_started_ms_ > ring_max_ms_) return stop_timer_ring(true);
  if (announcing_ || phase_ == Phase::SPEAKING) {
    ring_yielded_ = true;
    return;
  }
  player_.play(Channel::ANNOUNCE, {kSndTimer}, [this](bool) {
    if (timer_ringing_ && !timer_ring_tick_) timer_ring_tick_ = loop_.add_timer(300, [this] { ring_timer(); });
  });
}

void Controller::resume_ring() {
  if (!ring_yielded_) return;
  ring_yielded_ = false;
  ring_timer();
}

void Controller::ring_ramp() {
  if (!timer_ringing_ || !ring_alarm_) return;
  const double k = std::min(1.0, double(Loop::now_ms() - timer_ring_started_ms_) / 180000.0);
  mixer_.set_volume(float(0.02 + 0.98 * k));
}

void Controller::stop_timer_ring(bool timeout) {
  const bool was_alarm = timer_ringing_ && ring_alarm_;
  if (ring_ramp_tick_) loop_.cancel_timer(ring_ramp_tick_);
  ring_ramp_tick_ = 0;
  if (was_alarm) {
    mixer_.set_volume(ring_saved_volume_);   // the volume the user had before the alarm
    api_->fire_event(EventId::BUTTON, timeout ? "ring_timeout" : "ring_stopped");
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const Alarm &a) { return a.snooze; }), alarms_.end());
    if (timeout && ring_tries_ < 6 && ring_snooze_min_ > 0) {  // nobody switched it off: it rings again after the alarm's own pause
      Alarm s;
      s.id = "snooze";
      s.label = ring_label_;
      s.snooze = true;
      s.snooze_min = ring_snooze_min_;
      s.tries = ring_tries_ + 1;
      s.at.push_back(int64_t(time(nullptr)) + ring_snooze_min_ * 60);
      alarms_.push_back(std::move(s));
      LOGI("alarm: not switched off, rings again in %d minutes (try %d)", ring_snooze_min_, ring_tries_ + 2);
    }
    alarms_save();
    alarm_led();
  }
  ring_alarm_ = false;
  clock_.set_ringing(false);
  timer_ringing_ = false;
  ring_yielded_ = false;
  if (timer_ring_tick_) loop_.cancel_timer(timer_ring_tick_);
  timer_ring_tick_ = 0;
  interrupt_speech();  // the ring, or the speech it yielded to
  update_duck();
  update_ui();
}

// ---------------- media ----------------

// Music (HA media channel and Sendspin) is ducked while a dialog, HA announcement, timer ring or
// closing earcon is on.
void Controller::update_duck() {
  const bool want = phase_ != Phase::IDLE || announcing_ || timer_ringing_ || closing_earcon_;
  if (want == ducked_) return;
  ducked_ = want;
  apply_gains();
}

// Mute: softvol raw 0 is the bottom of its dB range (-60 dB on the Midi), still audible, so mute
// also zeroes the software gains of both playback channels and Sendspin.
void Controller::apply_gains() {
  const float music = st_.muted ? 0.0f : ducked_ ? kDuckGain : 1.0f;
  player_.set_channel_gain(Channel::MUSIC, music);
  player_.set_channel_gain(Channel::ANNOUNCE, st_.muted ? 0.0f : 1.0f);
  if (sendspin_) sendspin_->set_duck_gain(music);
  airplay_.set_duck(st_.muted ? 1.0f : ducked_ ? airplay_duck_gain(st_.volume) : 1.0f);
}

void Controller::start_sendspin(const DeviceIdentity &id) {
  if (access("/data/lostos/no-sendspin", F_OK) == 0) {  // Music Assistant is not used here: no server on the Sendspin port
    LOGI("sendspin: disabled (/data/lostos/no-sendspin)");
    return;
  }
  SendspinConfig cfg;
  cfg.name = id.friendly_name;
  cfg.product_name = id.model;
  cfg.manufacturer = id.manufacturer;
  cfg.software_version = id.project_version;
  for (char c : id.mac) cfg.mac += char(tolower(c));
  auto slash = state_path_.rfind('/');
  cfg.state_dir = (slash == std::string::npos ? std::string(".") : state_path_.substr(0, slash)) + "/sendspin";
  cfg.port = kSendspinPort;

  SendspinService::Hooks h;
  h.on_stream = [this](bool active) {
    // Music Assistant took over the speaker: the HA media channel yields to it.
    if (active && player_.state(Channel::MUSIC) != PlayState::IDLE) player_.stop(Channel::MUSIC);
    publish_media();
  };
  h.on_volume = [this](float v) { set_volume(v, false); };
  h.on_mute = [this](bool m) { set_muted(m); };
  h.on_pairing_code = [this](const std::string &code) {
    if (code.empty()) clock_.clear_code();
    else clock_.show_code(code);
  };
  h.on_pairing_window = [this](bool open) {
    sendspin_pairing_prompt_ = open;
    if (open) clock_.show_code("----");
    else clock_.clear_code();
  };
  h.on_music = [this](const MusicFrame &f) {
      leds_.music_frame(f, true);
      clock_.music_frame(f);
    };
  sendspin_ = std::make_unique<SendspinService>(loop_, std::move(cfg), std::move(h));
  if (!sendspin_->start(st_.volume, st_.muted)) {
    LOGE("Sendspin player unavailable");
    sendspin_.reset();
  }
}

void Controller::start_zigbee(const DeviceIdentity &id) {
  if (model_.zigbee_tty.empty()) return;
  const auto slash = state_path_.rfind('/');
  const std::string dir = slash == std::string::npos ? std::string(".") : state_path_.substr(0, slash);
  ZigbeeService::Config cfg;
  cfg.lines = {model_.zigbee_tty, model_.zigbee_reset, model_.zigbee_boot};
  cfg.coordinator_image = dir + "/zigbee-coordinator.ybl";
  cfg.router_image = dir + "/zigbee-router.ybl";
  updater_.add_extra_asset("zigbee-coordinator.ybl", cfg.coordinator_image);
  updater_.add_extra_asset("zigbee-router.ybl", cfg.router_image);
  cfg.deconz_port = kZigbeePort;
  cfg.failed_version = st_.zigbee_flash_failed;
  cfg.outcome = [this](uint32_t failed) {
    if (failed == st_.zigbee_flash_failed) return;
    st_.zigbee_flash_failed = failed;
    persist();
  };
  const auto mode = st_.zigbee_role;
  zigbee_ = std::make_unique<ZigbeeService>(loop_, std::move(cfg), mode, [this] {
    const std::string s = zigbee_->status();
    if (s == zigbee_status_) return;
    zigbee_status_ = s;
    LOGI("zigbee: %s", s.c_str());
    api_->publish_text_sensor(TextSensorId::ZIGBEE, s);
  });
  zigbee_->start();
  // ZHA discovers the coordinator (radio_type selects zigpy-deconz, serial_number is its unique id).
  if (mode == ZigbeeService::Mode::COORDINATOR && api_->mdns())
    api_->mdns()->add_service("_zigbee-coordinator._tcp", id.name, kZigbeePort,
                              {"radio_type=deconz", "serial_number=" + id.name, "name=" + id.friendly_name});
}

void Controller::set_muted(bool m) {
  st_.muted = m;
  mixer_.set_muted(m);
  apply_gains();
  if (sendspin_) sendspin_->report_volume(st_.volume, st_.muted);
  publish_media();
  persist();
}

void Controller::set_volume(float v, bool feedback) {
  st_.volume = std::min(1.0f, std::max(0.0f, v));
  mixer_.set_volume(st_.volume);
  if (ducked_) apply_gains();  // the duck depth follows the volume
  if (st_.muted && feedback) {
    st_.muted = false;
    mixer_.set_muted(false);
    apply_gains();
  }
  phost_.event("volume", std::to_string(st_.volume));
  if (feedback) {
    leds_.show_volume(st_.volume);
    clock_.flash_number(int(st_.volume * 100 + 0.5f));
    const int64_t now = Loop::now_ms();
    if (st_.volume_sound && !announcing_ && phase_ == Phase::IDLE && now - last_click_ms_ > 250) {   // a click on every step (not in the middle of a dialog)
      last_click_ms_ = now;
      // the stock click of the Station; Station 1 has volume_change.wav instead
      static const std::string click = access((std::string(kSounds) + "common/volume_sound.mp3").c_str(), R_OK) == 0
                                           ? std::string(kSounds) + "common/volume_sound.mp3"
                                           : std::string(kSounds) + "common/volume_change.wav";
      player_.play(Channel::ANNOUNCE, {click});
    }
  }
  if (sendspin_) sendspin_->report_volume(st_.volume, st_.muted);
  push_volume_to_sources();
  publish_media();
  persist();
}

void Controller::push_volume_to_sources() {
  if (airplay_.spotify_known() && std::fabs(st_.volume - spotify_vol_) >= 0.012f) {
    spotify_vol_ = st_.volume;
    const std::string c = "vol " + std::to_string(int(st_.volume * 100.f + 0.5f));
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd >= 0) {
      sockaddr_un a{};
      a.sun_family = AF_UNIX;
      strncpy(a.sun_path, "/tmp/librespot-ctl.sock", sizeof a.sun_path - 1);
      sendto(fd, c.data(), c.size(), 0, reinterpret_cast<sockaddr *>(&a), sizeof a);
      close(fd);
    }
  }
  if (bt_.connected() && std::fabs(st_.volume - bt_vol_) >= 0.012f) {
    bt_vol_ = st_.volume;
    bt_.set_volume(st_.volume);
  }
}

// The "Spotify cover" sensor carries everything the voice server needs about the track apart from its name:
// "cover url <TAB> track uri <TAB> length in ms <TAB> position in ms" (the server counts the progress on from the position).
void Controller::publish_spotify_extra() {
  const size_t tab = spotify_now_.find('\t');
  const std::string rest = tab == std::string::npos ? std::string() : spotify_now_.substr(tab + 1);
  api_->publish_text_sensor(TextSensorId::SPOTIFY_COVER, rest.empty() ? rest : rest + '\t' + std::to_string(spotify_pos_ms_));
}

// Where the track of AirPlay / Bluetooth is: "source <TAB> position ms <TAB> length ms" at the moment of the event
// (the voice server counts the time on while the music plays).
void Controller::publish_playback(const char *source, uint32_t pos, uint32_t dur) {
  pb_src_ = source;
  pb_pos_ = pos;
  pb_dur_ = dur;
  publish_media_state();
  api_->publish_text_sensor(TextSensorId::PLAYBACK, std::string(source) + '\t' + std::to_string(pos) + '\t' + std::to_string(dur));
}

// Another source took the speaker: the AirPlay sender is cut off, so its volume slider and buttons no longer reach us.
// AirPlay is switched off for a moment and on again (the speaker stays visible to the phones).
void Controller::drop_airplay_session() {
  if (air_disabled_ || !airplay_.session()) return;
  LOGI("airplay: session dropped, another source plays");
  airplay_.power(false);
  loop_.add_timer(3000, [this] { if (!air_disabled_) airplay_.power(true); });
}

void Controller::publish_media() {
  update_commands();  // command spotter follows playback state
  MediaState s = MediaState::IDLE;
  if (announcing_) {
    s = MediaState::ANNOUNCING;
  } else {
    switch (player_.state(Channel::MUSIC)) {
      case PlayState::PLAYING:
      case PlayState::LOADING:
        s = MediaState::PLAYING;
        break;
      case PlayState::PAUSED:
        s = sendspin_ && sendspin_->streaming() ? MediaState::PLAYING : MediaState::PAUSED;
        break;
      default:
        s = sendspin_ && sendspin_->streaming() ? MediaState::PLAYING : MediaState::IDLE;
    }
  }
  // What AirPlay, Spotify and Bluetooth play shows in Home Assistant's media player too (the state; titles are in the text sensors).
  if (!announcing_) {
    if (ext_playing()) s = MediaState::PLAYING;
    else if (s == MediaState::IDLE && ext_paused()) s = MediaState::PAUSED;
  }
  api_->publish_media(s, st_.volume, st_.muted);
  publish_now();
  update_music();
}

// The music that counts (playing, or paused and resumable): its source, artist, title and cover; the source is empty when none.
DlnaRenderer::Now Controller::now_info() const {
  DlnaRenderer::Now n;
  if (!(ext_playing() || ext_paused())) return n;
  std::string line;
  switch (music_target()) {
    case MusicTarget::SPOTIFY: {
      n.source = "Spotify";
      const size_t tab = spotify_now_.find('\t');
      line = spotify_now_.substr(0, tab);
      if (tab != std::string::npos) n.cover = spotify_now_.substr(tab + 1);
      if (const size_t t2 = n.cover.find('\t'); t2 != std::string::npos) n.cover.resize(t2);   // the rest is the track uri and its length
      if (const size_t k = n.cover.find("ab67616d00004851"); k != std::string::npos) n.cover.replace(k + 8, 8, "00001e02");   // 64 px -> 300 px
      break;
    }
    case MusicTarget::AIRPLAY:
      n.source = "AirPlay";
      line = last_air_track_;
      if (air_cover_ver_) n.cover = "file:///tmp/airplay-cover.img#" + std::to_string(air_cover_ver_);
      break;
    case MusicTarget::BT: n.source = "Bluetooth"; line = bt_.now(); break;
    default: return n;
  }
  n.title = line;
  const size_t dash = line.find(" - ");
  if (dash != std::string::npos) {
    n.artist = line.substr(0, dash);
    n.title = line.substr(dash + 3);
  }
  for (std::string *f : {&n.artist, &n.title})
    if (f->size() > 90) f->resize(90);
  for (std::string *f : {&n.artist, &n.title, &n.cover})
    for (char &c : *f)
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  n.playing = ext_playing();
  n.paused = !n.playing;
  return n;
}

static std::string json_string(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += char(c);
    } else if (c < 0x20) {
      o += ' ';   // line breaks and tabs do not belong in a title
    } else {
      o += char(c);
    }
  }
  return o + "\"";
}

void Controller::media_command(const std::string &cmd) {
  if (cmd == "play") transport(Transport::PLAY);
  else if (cmd == "pause") transport(Transport::PAUSE);
  else if (cmd == "playpause") transport(Transport::PLAYPAUSE);
  else if (cmd == "next") transport(Transport::NEXT);
  else if (cmd == "previous") transport(Transport::PREV);
  else LOGW("media: unknown command '%s'", cmd.c_str());
}

void Controller::publish_sysinfo() {
  api_->publish_text_sensor(TextSensorId::SYSINFO, sysinfo_.line({speaker_name(model_), model_.friendly_name, model_.codename, LOSTOS_VERSION}));
}

// The owner gave the speaker a name (or took it back): AirPlay and Spotify announce themselves under it, so they start again with the new name.
void Controller::rename_speaker(const std::string &raw) {
  std::string name = raw;
  while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
  while (!name.empty() && (name.back() == ' ' || name.back() == '\n' || name.back() == '\r')) name.pop_back();
  bool ok = name.size() <= 100;   // a name of up to 40 letters (UTF-8), no control characters
  for (unsigned char c : name) ok = ok && c >= 0x20 && c != 0x7f;
  if (!ok || !set_speaker_name(name)) {
    LOGW("name: not accepted: %s", name.c_str());
    return;
  }
  LOGI("name: the speaker is called '%s' now", speaker_name(model_).c_str());
  for (const char *id : {"ru.lostos.airplay", "ru.lostos.spotify"}) system_.set_running(id, false);
  loop_.add_timer(2500, [this] {
    for (const char *id : {"ru.lostos.airplay", "ru.lostos.spotify"}) system_.set_running(id, true);   // (a receiver that the owner switched off stays off)
  });
  publish_sysinfo();
}

void Controller::publish_plugins() { api_->publish_text_sensor(TextSensorId::PLUGINS, phost_.status()); }

// The equalizer lives in /data/lostos/eq.conf: the ALSA plugin of the speaker (alsa/lostoseq.c) reads that file, lostos writes it.
// The sensor "eq_state" repeats it as text "enabled;preamp;g0,...,g9" for the panels.
static const char *kEqFile = "/data/lostos/eq.conf";

void Controller::eq_set(const std::string &args) {
  int on = 0;
  float pre = 0;
  char gains[160] = {0};
  if (sscanf(args.c_str(), "%d %f %159s", &on, &pre, gains) != 3) return LOGW("eq: bad command '%s'", args.c_str());
  // ten numbers, each within +-15 dB: the plugin and the panels trust this file
  float g[10];
  const char *p = gains;
  for (int i = 0; i < 10; i++) {
    char *end = nullptr;
    g[i] = strtof(p, &end);
    if (end == p) return LOGW("eq: ten gains are needed");
    g[i] = std::max(-15.f, std::min(15.f, g[i]));
    p = *end == ',' ? end + 1 : end;
  }
  pre = std::max(-15.f, std::min(15.f, pre));
  std::string text = "enabled=" + std::to_string(on ? 1 : 0) + "\npreamp=" + std::to_string(pre) + "\ngains=";
  for (int i = 0; i < 10; i++) text += (i ? "," : "") + std::to_string(g[i]);
  text += "\n";
  const std::string tmp = std::string(kEqFile) + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fputs(text.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), kEqFile);
  }
  eq_publish();
}

void Controller::eq_publish() {
  int on = 0;
  float pre = 0;
  std::string gains = "0,0,0,0,0,0,0,0,0,0";
  if (FILE *f = fopen(kEqFile, "r")) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "enabled=", 8)) on = atoi(line + 8);
      else if (!strncmp(line, "preamp=", 7)) pre = strtof(line + 7, nullptr);
      else if (!strncmp(line, "gains=", 6)) {
        gains = line + 6;
        while (!gains.empty() && (gains.back() == '\n' || gains.back() == '\r')) gains.pop_back();
      }
    }
    fclose(f);
  }
  api_->publish_text_sensor(TextSensorId::EQ, std::to_string(on) + ";" + std::to_string(pre) + ";" + gains);
}

// The picture of the music in one text: {"source","playing","title","artist","cover","uri","pos","dur","air_cover"}.
// pos is where the track was at the moment of this message (the reader counts on while it plays).
void Controller::publish_media_state() {
  const DlnaRenderer::Now n = now_info();
  const char *src = n.source == "Spotify" ? "spotify" : n.source == "AirPlay" ? "airplay" : n.source == "Bluetooth" ? "bluetooth" : "";
  std::string uri;
  uint32_t pos = 0, dur = 0;
  if (n.source == "Spotify") {
    pos = spotify_pos_ms_;
    size_t f1 = spotify_now_.find('\t'), f2 = f1 == std::string::npos ? f1 : spotify_now_.find('\t', f1 + 1);
    size_t f3 = f2 == std::string::npos ? f2 : spotify_now_.find('\t', f2 + 1);
    if (f2 != std::string::npos) uri = spotify_now_.substr(f2 + 1, f3 == std::string::npos ? std::string::npos : f3 - f2 - 1);
    if (f3 != std::string::npos) dur = uint32_t(strtoul(spotify_now_.c_str() + f3 + 1, nullptr, 10));
  } else if (!n.source.empty() && pb_src_ == (n.source == "AirPlay" ? "airplay" : "bluetooth")) {
    pos = pb_pos_;
    dur = pb_dur_;
  }
  std::string j = std::string("{\"source\":\"") + src + "\",\"playing\":" + (n.playing ? "true" : "false") + ",\"title\":" + json_string(n.title) +
                  ",\"artist\":" + json_string(n.artist) + ",\"cover\":" + json_string(n.source == "Spotify" ? n.cover : std::string()) +
                  ",\"uri\":" + json_string(uri) + ",\"pos\":" + std::to_string(pos) + ",\"dur\":" + std::to_string(dur) +
                  ",\"air_cover\":" + std::to_string(air_cover_ver_) + "}";
  if (j == last_media_json_) return;
  last_media_json_ = j;
  phost_.event("media", std::string(src[0] ? src : "none") + " " + (n.playing ? "1" : "0") + " " + n.title);   // to the plugins that follow the music
  api_->publish_text_sensor(TextSensorId::MEDIA, j);
}

// The text sensor in Home Assistant, readable: "Artist - Title · Source" (+ "· пауза"), "Ничего не играет" when none.
void Controller::publish_now() {
  dlna_.changed();
  const DlnaRenderer::Now n = now_info();
  std::string out = "Ничего не играет";
  if (!n.source.empty()) {
    out = (n.artist.empty() ? n.title : n.artist + " — " + n.title) + " · " + n.source + (n.paused ? " · пауза" : "");
  }
  publish_media_state();
  if (out == last_now_) return;
  last_now_ = out;
  api_->publish_text_sensor(TextSensorId::NOW_PLAYING, out);
}

bool Controller::ext_playing() const { return airplay_.playing() || airplay_.spotify_playing() || bt_.playing(); }

// A source that is paused but can be resumed (a connected sender, the last Spotify track, a connected phone).
bool Controller::ext_paused() const {
  if (ext_playing()) return false;
  if (last_target_ == MusicTarget::AIRPLAY && airplay_.session()) return true;
  if (airplay_.spotify_known() &&
      (last_target_ == MusicTarget::NONE || (last_target_ == MusicTarget::SPOTIFY && Loop::now_ms() - last_music_ms_ < 30 * 60 * 1000)))
    return true;   // (NONE: lostos restarted, Spotify is what the speaker knows about)
  if (last_target_ == MusicTarget::BT && bt_.connected()) return true;
  return false;
}

void Controller::update_music() {
  const PlayState ps = player_.state(Channel::MUSIC);
  const bool media =
      ps == PlayState::PLAYING || ps == PlayState::LOADING || (sendspin_ && sendspin_->streaming()) ||
      airplay_.playing() || airplay_.spotify_playing() || bt_.playing();  // AirPlay, Spotify and Bluetooth get the same ring / display animation
  leds_.set_media_playing(media);
  // The speaker-reference analysis only runs while its result can be shown.
  mic_.set_music_analysis(media && (st_.music_viz != "Off" || st_.clock_eq != "Off") && !st_.light.on);
  clock_.set_music_playing(media && st_.clock_eq == "Always");
  clock_.set_eq_always(st_.clock_eq == "Always");
}

void Controller::on_media_command(const MediaCommandRequest &cmd) {
  if (cmd.has_media_url) {
    if (cmd.has_announcement && cmd.announcement) {
      // Media-player announcements report completion through state only.
      announcing_ = true;
      const uint64_t seq = ++announce_seq_;
      update_duck();
      update_ui();
      player_.play(Channel::ANNOUNCE, {cmd.media_url}, [this, seq](bool) {
        if (seq == announce_seq_) {
          announcing_ = false;
          end_announce();
        }
        resume_ring();
      });
    } else {
      if (sendspin_) sendspin_->yield_to_local();  // HA media takes the speaker from Sendspin
      player_.play(Channel::MUSIC, {cmd.media_url});
    }
  }
  if (cmd.has_volume) set_volume(cmd.volume, true);   // the panel / Home Assistant slider shows on the display too
  if (cmd.has_command) {
    const bool ss_on = sendspin_ && sendspin_->streaming();
    // Play / pause from Home Assistant act on AirPlay, Spotify or Bluetooth when one of them is the music (not HA's own media).
    bool routed = false;
    if (!(player_.state(Channel::MUSIC) == PlayState::PLAYING || ss_on) && (ext_playing() || ext_paused())) {
      switch (cmd.command) {
        case MediaCommand::PLAY: transport(Transport::PLAY); routed = true; break;
        case MediaCommand::PAUSE:
        case MediaCommand::STOP: transport(Transport::PAUSE); routed = true; break;
        case MediaCommand::TOGGLE: transport(Transport::PLAYPAUSE); routed = true; break;
        default: break;
      }
    }
    if (!routed) switch (cmd.command) {
      case MediaCommand::PLAY:
        if (ss_on && player_.state(Channel::MUSIC) == PlayState::PAUSED) sendspin_->yield_to_local();
        player_.resume(Channel::MUSIC);
        break;
      case MediaCommand::PAUSE:
        // While Sendspin plays, HA's pause/stop leave the Sendspin group (the speaker stops).
        if (ss_on && player_.state(Channel::MUSIC) != PlayState::PLAYING) sendspin_->yield_to_local();
        player_.pause(Channel::MUSIC);
        break;
      case MediaCommand::TOGGLE:
        if (player_.state(Channel::MUSIC) == PlayState::PLAYING)
          player_.pause(Channel::MUSIC);
        else if (ss_on)
          sendspin_->yield_to_local();
        else
          player_.resume(Channel::MUSIC);
        break;
      case MediaCommand::STOP:
        player_.stop(Channel::MUSIC);
        if (ss_on) sendspin_->yield_to_local();
        break;
      case MediaCommand::MUTE:
      case MediaCommand::UNMUTE:
        set_muted(cmd.command == MediaCommand::MUTE);
        break;
      case MediaCommand::VOLUME_UP:
        set_volume(st_.volume + 0.05f, false);
        break;
      case MediaCommand::VOLUME_DOWN:
        set_volume(st_.volume - 0.05f, false);
        break;
      default:
        LOGD("media command %u ignored", (unsigned)cmd.command);
    }
  }
  publish_media();
}

// ---------------- entities ----------------

void Controller::on_restart() {
  // A restart cuts the upload short (the shutdown watchdog does not wait ~25 s); the radio would
  // be written again on the next start, but stays without Zigbee meanwhile.
  if (zigbee_ && zigbee_->flashing()) {
    LOGW("restart requested from Home Assistant: refused while the Zigbee radio is being written");
    return;
  }
  LOGW("restart requested from Home Assistant");
  loop_.stop();
}

void Controller::on_switch(SwitchId id, bool state) {
  if (id == SwitchId::MIC_MUTE) {
    st_.mic_muted = state;
    if (state) cancel_arbitration();
    if (state && phase_ == Phase::LISTENING) {
      finish_interaction();
    }
    update_detection();
    update_ui();
  } else if (id == SwitchId::CLOCK) {
    st_.clock_on = state;
    clock_.set_enabled(state);
  } else if (id == SwitchId::AUTO_UPDATE) {
    st_.auto_update = state;
  } else if (id == SwitchId::NEAREST_ANSWERS) {
    st_.nearest_answers = state;
  } else if (id == SwitchId::VOICE_INDICATION) {
    st_.voice_indication = state;
    leds_.set_voice_indication(state);
  } else if (id == SwitchId::VOLUME_SOUND) {
    st_.volume_sound = state;
  } else if (id == SwitchId::LYRICS) {
    st_.lyrics = state;
  } else if (id == SwitchId::COVER_LOOKUP) {
    st_.cover_lookup = state;
  } else if (id == SwitchId::ALICE_BUTTON) {
    st_.alice_button = state;
  }
  api_->publish_switch(id, state);
  persist();
  if (id == SwitchId::AUTO_UPDATE) maybe_auto_update();
}

// ---------------- updates ----------------

void Controller::publish_update() {
  UpdateState u;
  u.current_version = LOSTOS_VERSION;
  u.title = "lostos";
  // latest == current hides the offer: nothing newer, or not checked yet.
  bool offer = !release_.version.empty() && release_newer(release_.version, LOSTOS_VERSION);
  u.latest_version = offer ? release_.version : u.current_version;
  u.release_url = release_.url;
  u.in_progress = u.has_progress = update_installing_;
  u.progress = update_progress_;
  api_->publish_update(u);
}

void Controller::check_update() {
  if (access("/data/lostos/allow-update", F_OK) != 0) return;   // updates only when this file exists: nothing is offered or installed
  updater_.check([this](const std::string &err, const Release &r) {
    if (!err.empty()) {
      LOGI("update check: %s", err.c_str());
      return;
    }
    if (r.version != release_.version) LOGI("latest release %s (running %s)", r.version.c_str(), LOSTOS_VERSION);
    release_ = r;
    publish_update();
    maybe_auto_update();
  });
}

// Restarting cuts off whatever plays: automatic installs wait for silence.
bool Controller::idle_for_update() const {
  return phase_ == Phase::IDLE && !announcing_ && !timer_ringing_ &&
         player_.state(Channel::MUSIC) != PlayState::PLAYING && !(sendspin_ && sendspin_->streaming()) &&
         !(zigbee_ && zigbee_->flashing());
}

void Controller::maybe_auto_update() {
  if (update_retry_) {
    loop_.cancel_timer(update_retry_);
    update_retry_ = 0;
  }
  bool current_known = false;
  if (!st_.auto_update || release_.version.empty() || update_installing_ ||
      !release_newer(release_.version, LOSTOS_VERSION, &current_known) || !current_known ||
      release_.version == st_.update_attempt)
    return;
  if (!idle_for_update()) {
    update_retry_ = loop_.add_timer(kUpdateIdleRetryMs, [this] {
      update_retry_ = 0;
      maybe_auto_update();
    });
    return;
  }
  install_update("automatic");
}

void Controller::install_update(const char *why) {
  if (updater_.busy() || update_installing_) return;
  if (zigbee_ && zigbee_->flashing()) {  // the install ends with a restart
    LOGW("update: not while the Zigbee radio is being written");
    return;
  }
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) {
    LOGE("update: cannot find the running binary");
    return;
  }
  // A binary replaced under the running daemon (installer, manual mv) reads "<path> (deleted)":
  // the install location is still <path>.
  std::string binary(exe, size_t(n));
  const std::string deleted = " (deleted)";
  if (binary.size() > deleted.size() && binary.ends_with(deleted)) binary.resize(binary.size() - deleted.size());
  const Release r = release_;
  LOGW("installing release %s (%s)", r.version.c_str(), why);
  st_.update_attempt = r.version;
  persist();
  update_installing_ = true;
  update_progress_ = 0;
  publish_update();
  updater_.install(
      r, binary,
      [this](float p) {
        update_progress_ = p;
        publish_update();
      },
      [this, r](const std::string &err, bool fatal) {
        if (err.empty()) {
          LOGW("installed %s, restarting", r.version.c_str());
          update_progress_ = 100;
          publish_update();
          loop_.add_timer(500, [this] { loop_.stop(); });  // exit 0: supervise.sh starts the new binary
          return;
        }
        LOGE("update to %s failed: %s", r.version.c_str(), err.c_str());
        update_installing_ = false;
        if (!fatal) {  // a network failure: the next check tries again
          st_.update_attempt.clear();
          persist();
        }
        publish_update();
      });
}

void Controller::on_update(UpdateCommand cmd) {
  if (access("/data/lostos/allow-update", F_OK) != 0) {
    LOGI("update request ignored: updates are disabled (create /data/lostos/allow-update to allow them)");
    return;
  }
  if (cmd == UpdateCommand::CHECK) return check_update();
  if (cmd != UpdateCommand::INSTALL) return;
  if (release_.version.empty() || release_.version == LOSTOS_VERSION) {
    LOGW("update: no other release to install");
    return;
  }
  install_update("requested from Home Assistant");
}

void Controller::on_number(NumberId id, float value) {
  if (id == NumberId::CLOCK_BRIGHTNESS) {
    st_.clock_brightness = std::min(100.f, std::max(0.f, value)) / 100.f;
    clock_.set_brightness(st_.clock_brightness);
    api_->publish_number(id, st_.clock_brightness * 100.f);
    persist();
  } else if (id == NumberId::CLOCK_SCROLL) {
    st_.clock_scroll_ms = std::clamp(int(std::lround(value / 50.f)) * 50, 100, 2000);
    clock_.set_scroll_ms(st_.clock_scroll_ms);
    api_->publish_number(id, float(st_.clock_scroll_ms));
    persist();
  } else if (id == NumberId::LED_SPEED) {
    st_.led_speed = std::clamp(value, 0.25f, 4.f);
    leds_.set_speed(st_.led_speed);
    api_->publish_number(id, st_.led_speed);
    persist();
  }
}

void Controller::on_text(TextId id, const std::string &value) {
  clock_text_ = value;
  clock_.set_text(value);
  api_->publish_text(id, value);
  // Text and face replace each other on the display; Clock::set_text already dropped the face.
  if (!value.empty() && clock_face_ != "None") {
    clock_face_ = "None";
    api_->publish_select(SelectId::CLOCK_FACE, clock_face_);
  }
}

void Controller::on_select(SelectId id, const std::string &option) {
  if (id == SelectId::ZIGBEE_MODE) {
    if (!zigbee_) return;
    auto role = st_.zigbee_role;
    for (const auto &r : ZigbeeService::kRoles)
      if (option == r.label) role = r.mode;
    if (role == st_.zigbee_role) return api_->publish_select(id, ZigbeeService::role(role).label);
    // The role starts with the daemon, which writes the radio firmware it needs; the
    // coordinator's mDNS advertisement is only made in that role.
    LOGW("zigbee: role %s chosen, restarting", ZigbeeService::role(role).name);
    st_.zigbee_role = role;
    st_.zigbee_flash_failed = 0;
    persist();
    api_->publish_select(id, ZigbeeService::role(role).label);
    loop_.stop();
    return;
  }
  if (id == SelectId::CLOCK_EQ_STYLE) {
    st_.clock_eq_style = option;
    clock_.set_eq_style(option == "Bars");
    api_->publish_select(id, option);
    persist();
    return;
  }
  if (id == SelectId::CLOCK_EQ) {
    st_.clock_eq = option;
    api_->publish_select(id, option);
    persist();
    update_music();
    return;
  }
  if (id == SelectId::MUSIC_VIZ) {
    st_.music_viz = option;
    leds_.set_music_style(option);
    api_->publish_select(id, option);
    persist();
    update_music();
    return;
  }
  if (id == SelectId::CASE_COLOR) {
    const std::string stem = option == "None" ? "" : option;
    st_.led_case_color = leds_.set_case_color(stem) ? stem : "";
    api_->publish_select(id, st_.led_case_color.empty() ? "None" : st_.led_case_color);
    persist();
    return;
  }
  clock_face_ = option;
  clock_.set_face(clock_face_index(option));
  api_->publish_select(id, option);
  if (option != "None" && !clock_text_.empty()) {
    clock_text_.clear();
    api_->publish_text(TextId::CLOCK_TEXT, clock_text_);
  }
}

void Controller::on_button(ButtonId id) {
  if (!zigbee_) return;
  if (zigbee_->mode() != ZigbeeService::Mode::ROUTER) {
    LOGW("zigbee: join/leave only apply to the router role");
    return;
  }
  if (id == ButtonId::ZIGBEE_JOIN) zigbee_->join();
  else zigbee_->leave();
}

// Wi-Fi management: /data/lostos/wifi.sh does the work (it keeps the old network until the new one is proven).
// The password never reaches the log: only the first word of the command is logged.
static void run_wifi(const std::string &value) {
  std::vector<std::string> a;
  size_t p = 0;
  for (;;) {
    const size_t t = value.find('\t', p);
    a.push_back(value.substr(p, t == std::string::npos ? std::string::npos : t - p));
    if (t == std::string::npos) break;
    p = t + 1;
  }
  if (a.empty() || a.size() > 5) return;
  LOGI("wifi: %s", a[0].c_str());
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {  // double fork: the script may start a watchdog and must not leave a zombie of lostos
    if (fork() == 0) {
      setsid();
      for (int fd = 3; fd < 1024; fd++) close(fd);
      int nul = open("/dev/null", O_RDWR);
      if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
      }
      std::vector<char *> argv;
      static const char *kWifi = "/data/lostos/wifi.sh";
      argv.push_back(const_cast<char *>(kWifi));
      for (auto &s : a) argv.push_back(const_cast<char *>(s.c_str()));
      argv.push_back(nullptr);
      execv(kWifi, argv.data());
    }
    _exit(0);
  }
  waitpid(pid, nullptr, 0);
}

void Controller::publish_wifi() {
  FILE *f = popen("/data/lostos/wifi.sh status 2>/dev/null", "r");
  if (!f) return;
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof buf, f) && out.size() < 3500) out += buf;
  pclose(f);
  if (out != last_wifi_) {
    last_wifi_ = out;
    api_->publish_text_sensor(TextSensorId::WIFI, out);
  }
}

void Controller::on_action(ActionId id, const std::string &value, int32_t seconds) {
  if (id == ActionId::ASSISTANT) {
    if (value == "bluetooth on" || value == "bluetooth off") {
      bluetooth_set(value == "bluetooth on", false);
      return;
    }
    if (value == "hello") {
      server_last_ms_ = Loop::now_ms();  // our assistant server says it is there (Home Assistant also subscribes to the voice pipeline, so that is no proof)
      server_hello_ = true;
      return;
    }
    if (value.rfind("eq ", 0) == 0) {
      eq_set(value.substr(3));
      return;
    }
    if (value.rfind("media ", 0) == 0) {   // the one media command of the speaker: play | pause | playpause | next | previous (whatever is the music)
      media_command(value.substr(6));
      return;
    }
    if (value.rfind("name ", 0) == 0 || value == "name") {   // the name of the speaker for AirPlay and Spotify (empty: the name of the model again)
      rename_speaker(value.size() > 5 ? value.substr(5) : "");
      return;
    }
    if (value.rfind("plugin ", 0) == 0) {   // install | remove | enable | disable a native plugin (docs/plugins.md)
      phost_.command(value.substr(7));
      return;
    }
    if (value.rfind("spotify_seek ", 0) == 0) {   // the panel's progress bar: jump to a position (ms) in the Spotify track
      spotify_ctl(("seek " + value.substr(13)).c_str());
      return;
    }
    if (value.rfind("jingle ", 0) == 0) {  // right when Home Assistant has done it: the stock smart-home jingle (the answer follows)
      player_.play(Channel::ANNOUNCE, {"file:///data/lostos/sounds/" + value.substr(7) + ".wav"});
      return;
    }
    if (value == "alarm on" || value == "alarm off") return;   // the speaker knows its own alarms now
    if (value.rfind("alarm_", 0) == 0 || value.rfind("reminder_", 0) == 0) {
      alarm_command(value);
      return;
    }
    if (value == "barge on" || value == "barge off") {
      barge_enabled_ = value == "barge on";
      LOGI("barge-in %s", barge_enabled_ ? "on" : "off");
      if (!barge_enabled_ && phase_ == Phase::SPEAKING) mic_.set_streaming(arbitrating_);
    } else if (value == "interrupt" && phase_ == Phase::SPEAKING) {  // somebody talks over her: she stops and listens
      LOGI("barge-in: interrupted by the server");
      interrupt_speech();
      start_pipeline("", false, mic_.recent(16000 * 2));
    }
    return;
  }
  if (id == ActionId::WIFI) {
    run_wifi(value);
    loop_.add_timer(800, [this] { publish_wifi(); });
    return;
  }
  if (id == ActionId::CLOCK_ANIM) {
    clock_.show_temporary_anim(value, seconds);
    return;
  }
  if (id == ActionId::AIRPLAY) {  // value: on | off | play | pause | playpause | next | previous
    LOGI("airplay: action '%s'", value.c_str());
    // The confirmation chime is part of the spoken answer (the voice server prepends it): a separate sound on the
    // same channel would be cut off by the answer, and the firmware's .opus jingles break this player.
    if (value == "on" || value == "off") {
      air_disabled_ = value == "off";
      airplay_.power(value == "on");
    }
    else if (value == "play") transport(Transport::PLAY);   // Home Assistant's buttons act on whatever is the music
    else if (value == "pause") transport(Transport::PAUSE);   // (AirPlay, Bluetooth, Spotify through the server)
    else if (value == "playpause") transport(Transport::PLAYPAUSE);
    else if (value == "next") transport(Transport::NEXT);
    else if (value == "previous") transport(Transport::PREV);
    return;
  }
  const bool text = id == ActionId::CLOCK_SHOW_TEXT;
  LOGI("clock: temporary %s '%s' for %d s", text ? "text" : "face", value.c_str(), int(seconds));
  if (text) clock_.show_temporary_text(value, seconds);
  else clock_.show_temporary_face(clock_face_index(value), seconds);
}

void Controller::on_led_show(const std::string &effect, const std::string &color, int32_t seconds) {
  if (effect.empty() && color.empty()) return leds_.clear_temporary();
  Rgb rgb{255, 255, 255};
  if (!color.empty()) {
    const std::string hex = color[0] == '#' ? color.substr(1) : color;
    if (hex.size() != 6 || !std::all_of(hex.begin(), hex.end(), [](char c) { return isxdigit((unsigned char)c); })) {
      LOGW("led_show: bad colour '%s'", color.c_str());
      return;
    }
    const unsigned long v = strtoul(hex.c_str(), nullptr, 16);
    rgb = Rgb{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  }
  LOGI("leds: show '%s' #%02x%02x%02x, %d s requested", effect.c_str(), rgb.r, rgb.g, rgb.b, int(seconds));
  leds_.show_temporary(effect, rgb, seconds);
}

void Controller::on_light(const LightState &l, uint32_t transition_ms) {
  st_.light = l;
  leds_.set_user_light(l.on, Rgb{uint8_t(l.r * 255), uint8_t(l.g * 255), uint8_t(l.b * 255)}, l.brightness,
                       l.effect == "None" ? "" : l.effect, transition_ms);
  persist();
  update_music();
}

void Controller::on_active_wake_words(const std::vector<std::string> &ids) {
  if (ids.empty()) return;
  if (ids[0] == st_.wake_word) return;
  st_.wake_word = ids[0];
  LOGI("wake word -> %s", st_.wake_word.c_str());
  mic_.set_model(model_dir(st_.wake_word));
  api_->set_active_wake_words({st_.wake_word});
  persist();
}

// Stock command spotter ("стоп", "хватит", "пауза", "продолжи", "громче", "тише", "дальше", "назад"):
// active without the wake word while something plays, and right after the wake word.
void Controller::update_commands() {
  bool playing = player_.state(Channel::MUSIC) == PlayState::PLAYING || (sendspin_ && sendspin_->streaming()) ||
                 announcing_ || timer_ringing_ || phase_ == Phase::SPEAKING || airplay_.playing() || airplay_.spotify_playing() || bt_.playing();
  // Right after a pause the commands stay on for two minutes, so "продолжи" / "играй" resume the music without "Алиса".
  const bool resumable = ext_paused() && Loop::now_ms() - last_music_ms_ < 120000;
  playing = playing || resumable;
  bool after_wake = (phase_ == Phase::LISTENING && Loop::now_ms() - listen_started_ms_ < kCommandAfterWakeMs) ||
                    arbitrating_ || Loop::now_ms() < local_listen_until_ms_;
  mic_.set_commands(!st_.mic_muted && !hw_mic_muted_ && (playing || after_wake));
}

// While music plays (and for a minute after it was last touched) the centre button is its stop / play key;
// otherwise it calls the assistant.
bool Controller::music_mode() const {
  return airplay_.playing() || airplay_.spotify_playing() || bt_.playing() || Loop::now_ms() - last_music_ms_ < 60000;
}

Controller::MusicTarget Controller::music_target() const {
  if (airplay_.playing()) return MusicTarget::AIRPLAY;
  if (airplay_.spotify_playing()) return MusicTarget::SPOTIFY;
  if (bt_.playing()) return MusicTarget::BT;
  if (last_target_ == MusicTarget::BT && bt_.connected()) return MusicTarget::BT;
  if (last_target_ == MusicTarget::AIRPLAY && airplay_.session()) return MusicTarget::AIRPLAY;
  if (last_target_ == MusicTarget::SPOTIFY) return MusicTarget::SPOTIFY;
  if (airplay_.spotify_known()) return MusicTarget::SPOTIFY;   // paused for good: "play" resumes Spotify
  return airplay_.session() ? MusicTarget::AIRPLAY : MusicTarget::NONE;
}

// The one module that moves the music. Forward / next is always: the next track and a sweep to the right on the display.
// AirPlay: next / previous go to the sender as they are (they used to be crossed; the current senders answer them straight)
// Spotify: events the assistant on the PC turns into Web API calls.
void Controller::transport(Transport t) {
  const MusicTarget tg = music_target();
  LOGI("transport %d -> %s", int(t), tg == MusicTarget::AIRPLAY ? "AirPlay" : tg == MusicTarget::SPOTIFY ? "Spotify" : tg == MusicTarget::BT ? "Bluetooth" : "none");
  const int64_t now = Loop::now_ms();
  last_music_ms_ = now;
  const bool skip = t == Transport::NEXT || t == Transport::PREV;
  if (skip) {
    skip_dir_ = t == Transport::NEXT ? 1 : -1;
    skip_ms_ = now;
  }
  if (tg == MusicTarget::AIRPLAY) {
    switch (t) {
      case Transport::PLAYPAUSE: airplay_.send("playpause"); break;
      case Transport::PAUSE: airplay_.send("pause"); break;
      case Transport::PLAY: airplay_.send("play"); break;
      case Transport::NEXT: airplay_.send("next"); break;
      case Transport::PREV: airplay_.send("previous"); break;
    }
    if (skip) track_change(skip_dir_);
  } else if (tg == MusicTarget::SPOTIFY) {
    // Spotify is controlled right in librespot on the speaker (a control socket), the PC is not needed. The assistant
    // server (Web API) is only the fallback, and starts fresh music after a long pause.
    const bool resume_old = std::string(spotify_play_event(false)) == "cmd_play" || !server_alive();
    switch (t) {
      case Transport::PLAYPAUSE:
        if (airplay_.spotify_playing()) {
          if (!spotify_ctl("pause")) api_->fire_event(EventId::BUTTON, "cmd_pause");
        } else if (!(resume_old && airplay_.spotify_known() && spotify_resume())) {
          api_->fire_event(EventId::BUTTON, spotify_play_event(false));
        }
        break;
      case Transport::PAUSE:
        if (!spotify_ctl("pause")) api_->fire_event(EventId::BUTTON, "cmd_pause");
        break;
      case Transport::PLAY:
        if (!(resume_old && airplay_.spotify_known() && spotify_resume())) api_->fire_event(EventId::BUTTON, spotify_play_event(false));
        break;
      case Transport::NEXT:
        if (!spotify_ctl("next")) api_->fire_event(EventId::BUTTON, "cmd_next");
        break;
      case Transport::PREV:
        if (!spotify_ctl("prev")) api_->fire_event(EventId::BUTTON, "cmd_previous");
        break;
    }
  } else if (tg == MusicTarget::BT) {
    switch (t) {
      case Transport::PLAYPAUSE: bt_.command("playpause"); break;
      case Transport::PAUSE: bt_.command("pause"); break;
      case Transport::PLAY: bt_.command("play"); break;
      case Transport::NEXT: bt_.command("next"); break;
      case Transport::PREV: bt_.command("previous"); break;
    }
    if (skip) track_change(skip_dir_);
  } else {  // HA media / Sendspin
    switch (t) {
      case Transport::PLAYPAUSE:
      case Transport::PAUSE:
        if (player_.state(Channel::MUSIC) == PlayState::PLAYING || player_.state(Channel::MUSIC) == PlayState::LOADING)
          player_.pause(Channel::MUSIC);
        else if (t == Transport::PLAYPAUSE && player_.state(Channel::MUSIC) == PlayState::PAUSED)
          player_.resume(Channel::MUSIC);
        if (sendspin_ && sendspin_->streaming()) sendspin_->transport(SendspinService::Transport::PAUSE);
        break;
      case Transport::PLAY:
        if (player_.state(Channel::MUSIC) == PlayState::PAUSED) player_.resume(Channel::MUSIC);
        else if (sendspin_) sendspin_->transport(SendspinService::Transport::PLAY);
        break;
      case Transport::NEXT:
      case Transport::PREV:
        if (sendspin_) sendspin_->transport(t == Transport::NEXT ? SendspinService::Transport::NEXT
                                                                 : SendspinService::Transport::PREVIOUS);
        break;
    }
  }
}

void Controller::centre_single_tap() {
  if (phase_ != Phase::IDLE) return;
  if (!music_mode()) {
    if (st_.alice_button) start_pipeline("", true);        // the button is switched off for the assistant: a tap with no music does nothing
    return;
  }
  transport(Transport::PLAYPAUSE);
}

const char *Controller::spotify_play_event(bool double_tap) const {
  const bool fresh = spotify_stopped_ms_ && Loop::now_ms() - spotify_stopped_ms_ > 10 * 60 * 1000;
  if (double_tap) return fresh ? "cmd_spotify_fresh" : "cmd_spotify";
  return fresh ? "cmd_play_fresh" : "cmd_play";
}

// The loudness of the stock boot sound, one byte per 40 ms (the clock dances to it).
static const char kBootEnvHex[] = "00000019333943545e5466667fa7afe3ba987d665657cba98a71635f5a565865616972726e6e706e6f6e7776c9d1ab8c93a2aba9c29f8a98a2a4a7acaca8a7a5a6a3a3a4a1a19e999abaddb5957a7a7c79d2ac8d746c6c69625e5c5453524e656d61534a3f3e4677685f6266595149464647675c52646e62574f4c4b50756d636a6c60597b8596a7b9c2c9cbc1ad93bfc9a58ed1cca7a49fa4a5c3deb6a09fa3afb3b2b4b2aeadafb2b4b2b1b0a7aeb5b5b5b8b6bcb5afa7a5a7a5a1a5a89f999b9b9a9da4a19a9c999285817c7c6e6a666b6d615153473d3d322a2a261f1a15110e0b0907060504030202";

// The display stays blank after power-up until the light sensor has been read (so that the brightness does not jump) and
// the boot show can begin.
void Controller::boot_wait() {
  if (boot_wait_since_ms_ == 0) boot_wait_since_ms_ = Loop::now_ms();
  if (!lux_ready_ && Loop::now_ms() - boot_wait_since_ms_ < 4000) {
    loop_.add_timer(250, [this] { boot_wait(); });
    return;
  }
  // Power-up: the show starts when the assistant server has answered (everything works), or after 25 s without it,
  // with a "something is wrong" sound at the end.
  const bool fresh_boot = access("/tmp/lostos-boot-done", F_OK) != 0;
  if (fresh_boot && !server_hello_ && Loop::now_ms() - boot_wait_since_ms_ < 25000) {
    loop_.add_timer(250, [this] { boot_wait(); });
    return;
  }
  boot_show(server_hello_);
  release_clock();
}

// The clock stays dark until the boot show is over and the real time is known (after power-up it shows nonsense for a
// few seconds); a minute is the longest it waits for the time.
void Controller::release_clock() {
  const int64_t now = Loop::now_ms();
  if (now < boot_end_ms_ - 450 || (!time_ready_ && now - boot_wait_since_ms_ < 60000)) {
    loop_.add_timer(300, [this] { release_clock(); });
    return;
  }
  { time_t tt = time(nullptr); struct tm tm_; localtime_r(&tt, &tm_); LOGI("clock released at %02d:%02d (TZ=%s)", tm_.tm_hour, tm_.tm_min, getenv("TZ") ? getenv("TZ") : "-"); }
  clock_.set_hold(false);
}

// A line from the server ("connected") is said after the boot show, not over it: while the show has not begun or is still playing it waits (at most 40 s).
void Controller::say_after_boot(const std::string &path, int tries) {
  // the show is still to come only where there is a boot sound to play it with (boot_show gives up without it and leaves no marker)
  const bool show_pending = access("/tmp/lostos-boot-done", F_OK) != 0 && access("/system/vendor/quasar/sounds/common/boot.wav", R_OK) == 0;
  if ((show_pending || Loop::now_ms() < boot_end_ms_) && tries < 80) {
    loop_.add_timer(500, [this, path, tries] { say_after_boot(path, tries + 1); });
    return;
  }
  if (!announcing_) play_reminder(path, true);
}

void Controller::boot_show(bool server_ok) {
  if (access("/tmp/lostos-boot-done", F_OK) == 0) return;   // once per power-up (/tmp is cleared by a reboot)
  const char *snd = "/system/vendor/quasar/sounds/common/boot.wav";
  if (access(snd, R_OK) != 0) return;
  if (FILE *f = fopen("/tmp/lostos-boot-done", "w")) fclose(f);
  std::vector<uint8_t> env;
  for (size_t i = 0; i + 1 < sizeof kBootEnvHex; i += 2)
    env.push_back(uint8_t(strtoul(std::string(kBootEnvHex + i, 2).c_str(), nullptr, 16)));
  LOGI("boot chime");
  player_.play(Channel::ANNOUNCE, {std::string("file://") + snd});
  const int64_t len = clock_.show_boot(env);
  leds_.show_boot(env, len + 3500);
  boot_end_ms_ = Loop::now_ms() + len + 3500;
  if (!server_ok) {  // nothing answered: the show still plays, and a failure tone says that something is wrong
    LOGW("boot: the assistant server did not answer");
    probe_net();
    loop_.add_timer(len - 400, [this] {
      when_net_known([this] { player_.play(Channel::ANNOUNCE, {no_server_sound()}); });
    });
  }
  (void)len;   // the show itself ends with the eyes and the fade to the clock
}

// A command for the Spotify player on the speaker: "play" | "pause" | "playpause" | "next" | "prev" (librespot's control socket).
bool Controller::spotify_ctl(const char *cmd) {
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, "/tmp/librespot-ctl.sock", sizeof a.sun_path - 1);
  const ssize_t n = sendto(fd, cmd, strlen(cmd), 0, reinterpret_cast<sockaddr *>(&a), sizeof a);
  close(fd);
  LOGI("spotify control: %s %s", cmd, n >= 0 ? "sent" : "unavailable (the assistant server takes it)");
  return n >= 0;
}

// "Play" for a paused Spotify: librespot is asked first (no server needed). After a long pause Spotify has dropped the session of the speaker
// and librespot takes the command but plays nothing, so a few seconds later we look: still silent means the server resumes it through the Web API.
bool Controller::spotify_resume() {
  if (!spotify_ctl("play")) return false;
  loop_.add_timer(3000, [this] {
    if (airplay_.spotify_playing() || airplay_.playing() || bt_.playing()) return;
    LOGI("spotify: librespot took 'play' but nothing started, the server resumes it (Web API)");
    api_->fire_event(EventId::BUTTON, "cmd_play");
  });
  return true;
}

// "Turn Bluetooth on / off": the stock voice answer when it was said (even without a server), the jingle for the button.
void Controller::bluetooth_set(bool on, bool voice) {
  if (!bt_.installed()) return;
  bt_.set_enabled(on);
  const char *snd = voice ? (on ? "file:///system/vendor/quasar/sounds/ru-RU/bluetooth_turning_on.mp3"
                                : "file:///system/vendor/quasar/sounds/ru-RU/bluetooth_turning_off.mp3")
                          : (on ? "file:///data/lostos/sounds/bt_on_jingle.wav" : "file:///data/lostos/sounds/bt_off_jingle.wav");
  player_.play(Channel::ANNOUNCE, {snd});
  publish_bt();
}

void Controller::publish_bt() {
  const std::string s = std::string(bt_.enabled() ? "on" : "off") + "\t" + (bt_.connected() ? "1" : "0") + "\t" +
                        (bt_.playing() ? "1" : "0") + "\t" + (bt_.playing() ? bt_.now() : std::string());
  api_->publish_text_sensor(TextSensorId::BLUETOOTH, s);
}

// A wake word while the assistant server does not answer (no network, the PC is off): the local commands still work
// ("turn Bluetooth on", "pause", "louder"), anything else gets the "no internet" answer.
void Controller::local_wake() {
  LOGW("wake word, but the assistant server does not answer: local commands only");
  local_cmd_done_ = false;
  local_listen_until_ms_ = Loop::now_ms() + 4500;
  probe_net();
  player_.play(Channel::ANNOUNCE, {kSndWake});
  leds_.set_ui_state(UiState::LISTENING);
  update_commands();
  loop_.add_timer(4600, [this] {
    update_commands();
    update_ui();
    if (!local_cmd_done_)
      when_net_known([this] {
        if (!local_cmd_done_) play_closing_earcon(no_server_sound());
      });
  });
}

void Controller::probe_net() {
  net_probe_ = -1;
  net_waiter_ = nullptr;
  probe_internet([this](bool ok) {
    net_probe_ = ok ? 1 : 0;
    LOGI("internet: %s", ok ? "reachable" : "not reachable");
    if (net_waiter_) {
      auto w = std::move(net_waiter_);
      net_waiter_ = nullptr;
      w();
    }
  });
}

void Controller::when_net_known(std::function<void()> fn) {
  if (net_probe_ != -1) return fn();
  net_waiter_ = std::move(fn);
}

std::string Controller::no_server_sound() const {
  return net_probe_ == 0 ? kSounds + std::string("ru-RU/no_internet.mp3")
                         : std::string("file:///data/lostos/sounds/backend_unavailable.wav");
}

// A ping of two public addresses in a child process; the answer comes from the event loop.
void Controller::probe_internet(std::function<void(bool)> cb) {
  int p[2];
  if (pipe(p) != 0) return cb(true);
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return cb(true);
  }
  if (pid == 0) {
    dup2(p[1], 1);
    for (int f = 3; f < 1024; f++) close(f);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) dup2(nul, 0), dup2(nul, 2);
    execl("/bin/sh", "sh", "-c",
          "ping -c1 -W2 77.88.8.8 >/dev/null 2>&1 || ping -c1 -W2 1.1.1.1 >/dev/null 2>&1; [ $? -eq 0 ] && echo 1 || echo 0",
          static_cast<char *>(nullptr));
    _exit(1);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
  fcntl(p[0], F_SETFD, FD_CLOEXEC);
  struct Probe {
    int fd;
    pid_t pid;
    std::string out;
    std::function<void(bool)> cb;
  };
  auto pr = std::make_shared<Probe>(Probe{p[0], pid, "", std::move(cb)});
  loop_.add_fd(pr->fd, EPOLLIN, [this, pr](uint32_t) {
    char tmp[64];
    for (;;) {
      ssize_t n = ::read(pr->fd, tmp, sizeof tmp);
      if (n > 0) {
        pr->out.append(tmp, size_t(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;   // the rest comes later
      break;                                                       // end of the answer
    }
    loop_.del_fd(pr->fd);
    close(pr->fd);
    waitpid(pr->pid, nullptr, 0);
    pr->cb(pr->out.find('1') != std::string::npos);
  });
}

void Controller::track_change(int dir) {  if (st_.clock_eq == "Off") return;
  clock_.show_track_change(dir, 5);
}

void Controller::music_pause() { transport(Transport::PAUSE); }
void Controller::music_resume() { transport(Transport::PLAY); }

void Controller::on_command(const std::string &word) {
  if (st_.mic_muted || hw_mic_muted_) return;
  LOGI("spotter phrase '%s'", word.c_str());
  enum { NONE, STOP, PAUSE, RESUME, LOUDER, QUIETER, NEXT, PREV, BT_ON, BT_OFF } c = NONE;
  if (word == "стоп" || word == "хватит") c = STOP;
  else if (word == "пауза") c = PAUSE;
  else if (word == "продолжи" || word == "играй") c = RESUME;
  else if (word == "громче") c = LOUDER;
  else if (word == "тише") c = QUIETER;
  else if (word == "дальше" || word == "следующий") c = NEXT;
  else if (word == "назад") c = PREV;
  else if (word.find("блютуз") != std::string::npos)   // the stock phrases "включи блютуз" / "выключи блютуз"
    c = word.find("выключи") != std::string::npos ? BT_OFF : BT_ON;
  if (c == NONE) return;  // TV navigation, "включи свет" etc. are left to Home Assistant
  if (Loop::now_ms() < commands_blocked_until_ms_) {
    LOGI("voice command '%s' ignored: another station answers this wake word", word.c_str());
    return;
  }
  if (arbitrating_) {
    arb_command_ = word;
    LOGI("voice command '%s' held until the wake arbitration ends", word.c_str());
    return;
  }
  bool after_wake = phase_ == Phase::LISTENING && Loop::now_ms() - listen_started_ms_ < kCommandAfterWakeMs;
  if (after_wake && (c == BT_ON || c == BT_OFF) && server_alive()) {
    // The assistant server is here: it understands this phrase itself and answers in its own voice (the stock voice would
    // be a second voice in the speaker), then it switches Bluetooth and the jingle plays.
    LOGI("voice command '%s': the assistant server answers it", word.c_str());
    return;
  }
  LOGI("voice command '%s'%s", word.c_str(), after_wake ? " (after wake word, HA pipeline cancelled)" : "");
  if (after_wake) {
    finish_interaction();
  }
  if (Loop::now_ms() < local_listen_until_ms_) local_cmd_done_ = true;
  switch (c) {
    case BT_ON:   // the stock voice only when there is no assistant server: otherwise just the jingle
      bluetooth_set(true, !server_alive());
      break;
    case BT_OFF:
      bluetooth_set(false, !server_alive());
      break;
    case STOP:
      if (timer_ringing_) {
        stop_timer_ring();
      } else if (phase_ == Phase::SPEAKING) {
        interrupt_speech();
        finish_interaction();
      } else if (announcing_) {  // a dialog listening meanwhile goes on
        interrupt_speech();
        update_duck();
        update_ui();
      } else {
        music_pause();
      }
      break;
    case PAUSE:
      music_pause();
      break;
    case RESUME:
      music_resume();
      break;
    case LOUDER:
    case QUIETER:
      set_volume(st_.volume + (c == LOUDER ? 0.1f : -0.1f), true);
      break;
    case NEXT:
    case PREV:
      transport(c == NEXT ? Transport::NEXT : Transport::PREV);
      break;
    case NONE:
      break;
  }
  update_commands();
}

void Controller::update_ui() {
  leds_.set_muted(st_.mic_muted || hw_mic_muted_);
  update_commands();
  if (timer_ringing_) return leds_.set_ui_state(UiState::TIMER_RING);
  switch (phase_) {
    case Phase::LISTENING:
      return leds_.set_ui_state(UiState::LISTENING);
    case Phase::PROCESSING:
      return leds_.set_ui_state(UiState::THINKING);
    case Phase::SPEAKING:
      return leds_.set_ui_state(UiState::SPEAKING);
    case Phase::IDLE:
      break;
  }
  if (arbitrating_) return leds_.set_ui_state(UiState::LISTENING);
  if (announcing_) return leds_.set_ui_state(UiState::SPEAKING);
  leds_.set_ui_state(UiState::IDLE);
}

}  // namespace cs
