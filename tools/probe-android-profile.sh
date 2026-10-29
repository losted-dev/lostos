#!/system/bin/sh
# lostOS probe for ANDROID speakers, part 2 (the first Yandex Station "tulip-d1"): the details a port needs, read with root where the plain adb shell is not allowed.
# Run after probe-android.sh:  adb push probe-android-profile.sh /data/local/tmp/     then     adb shell "sh /data/local/tmp/probe-android-profile.sh" > android-profile.txt
# It only READS (it runs nothing of the Yandex programs). Lines with keys, tokens, passwords, serial numbers, MAC addresses and network names are dropped.

HIDE='key|token|secret|password|passwd|serial|mac|imei|ssid|bssid|uuid|ethaddr'
Q=/system/vendor/quasar
sec() { echo; echo "===== $1"; }
# runs a command line as root when su exists, else as the shell user
run() { if [ "$(id -u)" = 0 ]; then sh -c "$1" 2>&1; elif [ -x /system/xbin/su ]; then su -c "$1" 2>&1; else sh -c "$1" 2>&1; fi; }

sec "root"
id; run id

sec "mixers of the sound cards (tinymix, every card)"
for c in 0 1 2 3 4; do echo "-- card $c"; run "tinymix -D $c 2>&1 | head -90"; done

sec "pcm devices (info)"
for f in /proc/asound/card*/pcm*/info; do echo "-- $f"; run "cat $f 2>/dev/null | head -12"; done

sec "audio policy"
run "cat /system/etc/audio_policy.conf 2>/dev/null | head -90"
echo "-- audio flinger (the start of its dump)"
run "dumpsys media.audio_flinger 2>/dev/null | head -50"

sec "the Yandex stack: audio and LED settings of quasar.cfg"
run "grep -n -B3 -A14 '\"micChannels\"' $Q/quasar.cfg 2>/dev/null | grep -v -i -E '$HIDE'"
run "grep -n -i -E 'vqe|preset|spkChannels|mainChannel|sampleRate|deviceName|inRate|outRate|platformToAlsaMap|ledconfig|led_util|i2c|gpio|button|mute' $Q/quasar.cfg 2>/dev/null | grep -v -i -E '$HIDE' | head -70"

sec "the Yandex stack: files"
run "ls -l $Q/libs 2>/dev/null | head -60"
run "ls -l $Q/ledpatterns 2>/dev/null | head -40"
run "ls -l $Q/led_util $Q/maind $Q/quasar_launcher2 2>/dev/null"
echo "-- bitness of the programs (the 5th byte of the ELF header: 01 = 32 bit, 02 = 64 bit)"
for f in $Q/maind $Q/led_util $Q/libs/libyandex_evil.so $Q/libs/libtflite.so $Q/libs/libquasar_gstreamer.so; do [ -e $f ] && echo "$f: $(run "od -An -tx1 -N 5 $f 2>/dev/null")"; done
echo "-- play_pattern.sh"
run "head -30 $Q/play_pattern.sh 2>/dev/null"
echo "-- onboot.sh"
run "head -60 $Q/onboot.sh 2>/dev/null | grep -v -i -E '$HIDE'"
echo "-- launch.json"
run "head -80 $Q/launch.json 2>/dev/null | grep -v -i -E '$HIDE'"

sec "gpio (the buttons and switches of this speaker are probably here)"
for g in /sys/class/gpio/gpio[0-9]*; do [ -d "$g" ] && echo "$(basename $g): direction=$(run "cat $g/direction") value=$(run "cat $g/value") edge=$(run "cat $g/edge") active_low=$(run "cat $g/active_low")"; done
echo "-- who uses them"
run "cat /sys/kernel/debug/gpio 2>/dev/null | head -40"

sec "i2c and the chips on it (LED driver, amplifier, codec)"
for i in /sys/class/i2c-dev/i2c-*; do echo "$(basename $i): $(cat $i/name 2>/dev/null)"; done
ls /sys/bus/i2c/devices 2>/dev/null
for d in /sys/bus/i2c/devices/*; do [ -e $d/name ] && echo "$(basename $d): $(cat $d/name 2>/dev/null)"; done
echo "-- kernel modules of audio and LEDs"
run "lsmod 2>/dev/null | head -40"

sec "end"
