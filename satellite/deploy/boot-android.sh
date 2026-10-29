#!/system/bin/sh
# Boot hook of Station 1 (tulip-d1, Android 6). It has no rootkeeper: one line in /system/etc/rc.local, right before
# onboot.sh, starts this in the background (detached, so the `wait` at the end of rc.local does not wait for it).
# Same contract as boot.sh: if /data/lostos/enabled exists, stop the Yandex stack that onboot.sh starts and run lostos
# under supervise.sh. The other hooks of rc.local are never touched.
D=/data/lostos
[ -f "$D/enabled" ] || exit 0
# Never stop the Yandex stack when there is nothing to replace it with.
[ -x "$D/lostos" ] && [ -x "$D/supervise.sh" ] || exit 0
[ "$(getprop ro.product.device)" = tulip-d1 ] || exit 0

# Tools the scripts use that Android 6 lacks (killall, which `q stop` calls) or has in another form: busybox's.
BB=/system/bin/busybox-smp
mkdir -p "$D/bin"
for a in pidof killall setsid cmp wc tr grep sleep timeout cp mv ln; do
  [ -e "$D/bin/$a" ] || ln -s "$BB" "$D/bin/$a"
done
export PATH="$D/bin:/system/bin:/system/xbin" ANDROID_DATA=/data ANDROID_ROOT=/system

pidof lostos >/dev/null 2>&1 && exit 0
pidof supervise.sh >/dev/null 2>&1 && exit 0  # never two supervisors

# The root file system is rebuilt at every boot: /tmp (tmpfs: the log, sockets, marks) and /bin (#!/bin/sh of the scripts
# and of `q`) have to be made again.
if [ ! -d /tmp ] || [ ! -e /bin/sh ]; then
  mount -o remount,rw /
  [ -d /tmp ] || mkdir /tmp
  [ -e /bin ] || ln -s /system/bin /bin
  mount -o remount,ro /
fi
grep -q " /tmp tmpfs " /proc/mounts || mount -t tmpfs -o mode=1777,size=64m tmpfs /tmp

# onboot.sh launches quasar right after starting us; wait for it, then stop it (q stop: launcher, then maind).
i=0
while [ $i -lt 60 ] && ! pidof quasar_launcher2 >/dev/null 2>&1; do
  sleep 1
  i=$((i + 1))
done
sleep 3
/system/vendor/quasar/q stop >/dev/null 2>&1

exec setsid "$D/supervise.sh" </dev/null >/dev/null 2>&1
