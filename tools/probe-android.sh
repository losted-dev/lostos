#!/system/bin/sh
# lostOS probe for ANDROID speakers (the first Yandex Station "tulip-d1", Allwinner R18, Android 6; also Max, JBL Link Music): what the speaker is made of, to see whether and how lostOS can run on it.
# Run it on the speaker with adb:   adb push probe-android.sh /data/local/tmp/     then     adb shell "sh /data/local/tmp/probe-android.sh" > android-probe.txt
# It only READS: nothing is installed, changed or switched off. Lines with serial numbers, MAC addresses, network names, keys and tokens are dropped.
# Send android-probe.txt to whoever makes the port (docs/platforms.md).

HIDE='serial|mac|imei|meid|ssid|bssid|token|password|passwd|secret|hostname|dhcp|wifi|ethaddr|androidboot.serialno|bt_addr|btaddr|uuid|key'
sec() { echo; echo "===== $1"; }
has() { command -v "$1" >/dev/null 2>&1; }

sec "who is asking"
id
getenforce 2>/dev/null
for s in /system/xbin/su /system/bin/su /sbin/su; do [ -e $s ] && echo "su: $s"; done

sec "model and system (getprop, filtered)"
getprop | grep -i -E 'ro\.product|ro\.build|ro\.hardware|ro\.board|ro\.bootloader|ro\.secure|ro\.adb|ro\.debuggable|ro\.zygote|ro\.config|persist\.sys\.usb|sys\.usb|ro\.boot\.hardware|ro\.boot\.mode|ro\.opengles|ro\.sf|audio|ro\.vendor|ro\.mediatek|ro\.allwinner|sys\.boot' | grep -v -i -E "$HIDE"
uname -a
cat /proc/version 2>/dev/null
grep -i -E 'hardware|model name|processor|features|cpu architecture|cpu part' /proc/cpuinfo 2>/dev/null | sort | uniq -c | head -20

sec "memory and storage (where something can live: a writable partition with 30+ MB free)"
head -5 /proc/meminfo
df 2>/dev/null
echo "-- mounts"
grep -E ' /(system|data|cache|vendor|storage|mnt|sdcard|oem)' /proc/mounts 2>/dev/null | cut -c1-140
echo "-- partitions"
cat /proc/partitions 2>/dev/null | head -40
ls /dev/block/platform/*/by-name 2>/dev/null | head -40

sec "audio: which sound stack is there (alsa-lib or tinyalsa or only AudioFlinger)"
cat /proc/asound/cards 2>/dev/null
cat /proc/asound/pcm 2>/dev/null
ls -l /dev/snd 2>/dev/null
echo "-- libraries"
ls /system/lib /system/lib64 /vendor/lib /vendor/lib64 2>/dev/null | grep -i -E 'asound|tinyalsa|alsa|audio|opensles|aaudio|speex|opus|webrtc|quasar|yandex|tflite|evil' | sort | uniq | head -60
echo "-- audio hal and policy"
ls /system/lib/hw /vendor/lib/hw /system/lib64/hw 2>/dev/null | grep -i audio | head
ls /system/etc /vendor/etc 2>/dev/null | grep -i -E 'audio|mixer|alsa' | head
for t in tinymix tinycap tinyplay aplay arecord amixer; do has $t && echo "tool: $t"; done
has tinymix && tinymix 2>/dev/null | head -80

sec "Yandex software"
pm list packages 2>/dev/null | grep -i -E 'yandex|quasar|alice|station' | head -30
ls /system/priv-app /system/app /vendor/app 2>/dev/null | grep -i -E 'yandex|quasar|alice|station' | head -30
ls /system/vendor/quasar /vendor/quasar /system/quasar 2>/dev/null | head -30
echo "-- processes"
ps 2>/dev/null | grep -i -E 'yandex|quasar|alice|maind|station' | head -20
echo "-- startup"
ls -l /init*.rc /system/etc/init 2>/dev/null | head -20
ls /system/etc/init.d /system/bin/install-recovery.sh 2>/dev/null | head

sec "buttons, microphones, LEDs, display"
cat /proc/bus/input/devices 2>/dev/null | grep -E '^(N:|H:|B: (EV|KEY|SW))' | head -60
echo "-- leds"
for l in /sys/class/leds/*; do [ -d "$l" ] && echo "$(basename $l) max=$(cat $l/max_brightness 2>/dev/null) trigger=$(cat $l/trigger 2>/dev/null | cut -c1-80)"; done 2>/dev/null | head -40
echo "-- framebuffers"
for f in /sys/class/graphics/fb*; do [ -d "$f" ] && echo "$(basename $f): name=$(cat $f/name 2>/dev/null) size=$(cat $f/virtual_size 2>/dev/null) bpp=$(cat $f/bits_per_pixel 2>/dev/null)"; done 2>/dev/null
ls /dev/graphics /dev/i2c-* /dev/spidev* /dev/ttyS* /dev/ttyAMA* 2>/dev/null | head -20
echo "-- gpio and sensors"
ls /sys/class/gpio 2>/dev/null | head -5
ls /sys/bus/iio/devices 2>/dev/null | head

sec "radios and network (names only)"
ls /sys/class/net 2>/dev/null
ls /sys/class/bluetooth 2>/dev/null
getprop | grep -i -E 'wifi\.interface|bluetooth|wlan' | grep -v -i -E 'ssid|key|pass|mac|addr' | head -10

sec "what is already installed (our files)"
ls /data/lostos /data/local/tmp /data/opt 2>/dev/null | head -20

sec "end"
