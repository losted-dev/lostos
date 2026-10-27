#!/bin/sh
# lostOS station probe: what a speaker is made of (for a model profile and for "will lostOS fit here").
# Run on the speaker (BusyBox sh):   adb shell sh < probe-station.sh > probe.txt      or   sh /tmp/probe-station.sh > /tmp/probe.txt
# Nothing here reads passwords, tokens or Wi-Fi settings; MAC addresses and serial numbers are masked in the output.
# Send probe.txt to whoever makes the profile (docs/models.md).

mask() { sed -E 's/([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g; s/(serial|Serial|SN|sn)[^ ]*[ =:]+[^ ]+/\1 ***/g'; }
sec() { echo; echo "===== $1"; }

sec "model"
cat /proc/device-tree/model 2>/dev/null | tr -d '\000'; echo
cat /proc/device-tree/compatible 2>/dev/null | tr '\000' '\n'
uname -a | mask
grep -i 'hardware\|model name\|processor' /proc/cpuinfo | sort | uniq -c
cat /etc/os-release 2>/dev/null | head -4
cat /system/vendor/quasar/VERSION 2>/dev/null

sec "memory (the numbers that decide whether lostOS fits)"
free -m 2>/dev/null || head -5 /proc/meminfo
echo "-- biggest processes (RSS, kB)"
ps -o rss,vsz,args 2>/dev/null | sort -n -r | head -15 | cut -c1-110

sec "storage (where lostOS can live: a writable partition with 30+ MB free)"
df -h 2>/dev/null | grep -v tmpfs | grep -v devtmpfs
echo "-- mounts (read-only or writable)"
grep -v 'tmpfs\|proc\|sysfs\|cgroup\|devpts\|debugfs' /proc/mounts | cut -c1-110
echo "-- partitions"
cat /proc/mtd 2>/dev/null
ubinfo -a 2>/dev/null | grep -i 'volume\|name\|size' | head -30

sec "audio"
cat /proc/asound/cards 2>/dev/null
arecord -l 2>/dev/null
aplay -l 2>/dev/null
cat /etc/asound.conf 2>/dev/null | head -60

sec "leds, display, clock"
ls /sys/class/leds 2>/dev/null | tr '\n' ' '; echo
for f in /sys/class/graphics/fb*/name; do [ -f "$f" ] && echo "$f: $(cat $f)"; done 2>/dev/null

sec "buttons and switches"
cat /proc/bus/input/devices 2>/dev/null | grep -i 'name\|handlers'

sec "radios and sensors"
ls /dev/ttyS* /dev/ttyAML* 2>/dev/null | tr '\n' ' '; echo
ls /sys/bus/iio/devices 2>/dev/null | tr '\n' ' '; echo
ls /sys/class/net 2>/dev/null | tr '\n' ' '; echo
ls /sys/class/bluetooth 2>/dev/null | tr '\n' ' '; echo

sec "what is already installed"
ls /data/lostos 2>/dev/null | head -30
ls /bkupgrade 2>/dev/null | head -10
du -sk /data/lostos /data/shairport 2>/dev/null
echo; echo "===== end"
