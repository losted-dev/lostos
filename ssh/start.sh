#!/bin/sh
# Starts dropbear SSH on port 22 (root, blank password allowed: -B), on IPv4 only: behind the home router that is the local
# network, while a public IPv6 address of the speaker would be reachable from the internet.
# Installed to /data/dropbear; called at boot from /data/quasar/rootkeeper.sh.
# Independent of adbd: never touches adb.
D=/data/dropbear
cd "$D" || exit 1
for t in dropbear dropbearkey scp dbclient; do
  [ -e "$D/$t" ] || ln -sf dropbearmulti "$D/$t"
done
for t in ed25519 ecdsa rsa; do
  [ -s "$D/host_$t" ] || "$D/dropbearkey" -t $t -f "$D/host_$t" >/dev/null 2>&1
done
pidof dropbear >/dev/null 2>&1 && exit 0
exec "$D/dropbear" -B -p 0.0.0.0:22 -P /run/dropbear.pid \
  -r "$D/host_ed25519" -r "$D/host_ecdsa" -r "$D/host_rsa"
