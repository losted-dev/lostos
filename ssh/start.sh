#!/bin/sh
D=/data/dropbear
cd "$D" || exit 1
for t in dropbear dropbearkey scp dbclient; do
  [ -e "$D/$t" ] || ln -sf dropbearmulti "$D/$t"
done
for t in ed25519 ecdsa rsa; do
  [ -s "$D/host_$t" ] || "$D/dropbearkey" -t $t -f "$D/host_$t" >/dev/null 2>&1
done
if [ -s /run/dropbear.pid ]; then
  read -r old_pid < /run/dropbear.pid
  case "$old_pid" in ''|*[!0-9]*) exit 1;; esac
  if kill -0 "$old_pid" 2>/dev/null; then
  case "$(readlink "/proc/$old_pid/exe")" in
    "$D/dropbearmulti"|"$D/dropbearmulti (deleted)")
      kill "$old_pid" || exit 1
      tries=0
      while kill -0 "$old_pid" 2>/dev/null; do
        [ "$tries" -lt 5 ] || exit 1
        sleep 1
        tries=$((tries + 1))
      done
      ;;
    *) exit 1;;
  esac
  fi
fi
exec "$D/dropbear" -s -p 22 -P /run/dropbear.pid \
  -r "$D/host_ed25519" -r "$D/host_ecdsa" -r "$D/host_rsa"
