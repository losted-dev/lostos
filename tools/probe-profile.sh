#!/bin/sh
# lostOS station probe, part 2: the details a model profile needs (microphones, LEDs, buttons, volume curve, sounds).
# Run after probe-station.sh:  adb push probe-profile.sh /tmp/   then   adb shell "tr -d '\r' < /tmp/probe-profile.sh | sh" > profile.txt
# Reads only the hardware sections of the system config; no accounts, tokens or Wi-Fi data.

Q=/system/vendor/quasar
sec() { echo; echo "===== $1"; }

sec "audio input of the stock firmware (quasar.cfg)"
grep -n -B10 -A16 '"micChannels"' $Q/quasar.cfg 2>/dev/null
echo "-- channels and vqe"
grep -n '"micChannels"\|"spkChannels"\|"mainChannel"\|"preset"\|counterActivationPresetName\|"sampleRate"' $Q/quasar.cfg 2>/dev/null | head -12
echo "-- volume curve"
grep -n 'platformToAlsaMap' $Q/quasar.cfg 2>/dev/null

sec "LEDs (name, trigger, max brightness)"
for l in /sys/class/leds/*; do
  [ -d "$l" ] && echo "$(basename $l)  max=$(cat $l/max_brightness 2>/dev/null)  trigger=$(sed -n 's/.*\[\(.*\)\].*/\1/p' $l/trigger 2>/dev/null)"
done
echo "-- led ring framebuffer"
for f in /sys/class/graphics/fb*; do
  [ -d "$f" ] && echo "$(basename $f): name=$(cat $f/name 2>/dev/null) size=$(cat $f/virtual_size 2>/dev/null) bpp=$(cat $f/bits_per_pixel 2>/dev/null)"
done
echo "-- led configs of the stock firmware"
ls $Q/ledconfigs 2>/dev/null | head -20
ls $Q/ledpatterns 2>/dev/null | head -20

sec "input devices (buttons, touch pads, mute switch)"
cat /proc/bus/input/devices 2>/dev/null
for e in /sys/class/input/event*; do
  [ -d "$e" ] && echo "$(basename $e): $(cat $e/device/name 2>/dev/null)  key-caps=$(cat $e/device/capabilities/key 2>/dev/null)  sw-caps=$(cat $e/device/capabilities/sw 2>/dev/null)"
done

sec "sounds of the stock firmware"
ls $Q/sounds 2>/dev/null | head -12
ls $Q/sounds/ru-RU 2>/dev/null | head -20

sec "mixer and sound card"
amixer -c 0 scontrols 2>/dev/null | head -20
amixer -c 0 controls 2>/dev/null | head -30
cat /proc/asound/card0/pcm*/info 2>/dev/null | grep -i 'id\|name\|subdevices_count'

sec "zigbee, light sensor, hardware"
ls /sys/devices/platform 2>/dev/null | grep -i 'zigbee\|telink\|light\|sar\|aw9' | head
ls /sys/bus/iio/devices/iio:device0 2>/dev/null | head -12
cat /sys/bus/iio/devices/iio:device0/name 2>/dev/null
echo; echo "===== end"
