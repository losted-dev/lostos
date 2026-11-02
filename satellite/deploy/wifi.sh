#!/bin/sh
# wifi.sh - safe Wi-Fi management for the Station (wpa_supplicant + wpa_cli).
#   status                    machine-readable state (read by lostos every few seconds)
#   scan                      look around, results land in /tmp/wifi-scan (shown by "status")
#   add SSID PASS [now]       add a network. Without "now" it is only stored (the speaker joins it when it is in range
#                             and the others are not). With "now" the speaker switches to it at once, and goes back to
#                             the old network by itself if the new one does not give a working connection in 45 s.
#   forget SSID               remove a stored network (never the one in use)
# Nothing is saved to the config file before the connection is proven, so a power cut in the middle leaves the old
# config untouched; the stored networks are kept, new ones are only added.
D=/data/wifi
CONF=/bkupgrade/data/wpa_supplicant.conf
W="wpa_cli -i wlan0"
mkdir -p $D

say() { echo "$*" > $D/last; echo "$*"; }

gateway() { ip route 2>/dev/null | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -1; }

online() {   # associated, an address, and the gateway answers
  [ "$($W status 2>/dev/null | sed -n 's/^wpa_state=//p')" = "COMPLETED" ] || return 1
  gw=$(gateway); [ -n "$gw" ] || return 1
  ping -c 2 -W 2 "$gw" >/dev/null 2>&1 || return 1
  # a network is working only when the server of the speaker can be reached through it (a phone hotspot answers the gateway, but a server at home is out of reach)
  url=$(sed -n 's/^server=//p' /data/lostos/link.conf 2>/dev/null | head -1)
  [ -n "$url" ] || return 0
  srv=$(echo "$url" | cut -d/ -f3)
  p=443; echo "$url" | grep -q '^http:' && p=80
  h=$(echo "$srv" | cut -d: -f1)
  if echo "$srv" | grep -q ':'; then p=$(echo "$srv" | cut -d: -f2); fi
  command -v telnet >/dev/null 2>&1 || return 0
  tcp_ok "$h" "$p"
}

tcp_ok() {   # a TCP connection to the server opens within 6 s (the telnet of busybox says "Connected")
  (echo | telnet "$1" "$2" 2>&1 | grep -q Connected) & tp=$!
  n=0; while kill -0 $tp 2>/dev/null && [ $n -lt 6 ]; do sleep 1; n=$((n + 1)); done
  if kill -0 $tp 2>/dev/null; then kill $tp 2>/dev/null; return 1; fi
  wait $tp
}

status() {
  st=$($W status 2>/dev/null)
  ssid=$(echo "$st" | sed -n 's/^ssid=//p'); state=$(echo "$st" | sed -n 's/^wpa_state=//p')
  ip=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
  # the last message is shown for ten minutes only: after that (and after a reboot) it is a stale leftover
  last=""; [ -n "$(find $D/last -mmin -10 2>/dev/null)" ] && last=$(cat $D/last 2>/dev/null)
  printf 'cur\t%s\t%s\t%s\t%s\t%s\n' "$ssid" "$ip" "$state" "$(cat $D/pending 2>/dev/null)" "$last"
  $W list_networks 2>/dev/null | tail -n +2 | awk -F'\t' '{ printf "net\t%s\t%s\t%s\n", $1, $2, $4 }'
  [ -f /tmp/wifi-scan ] && head -n 25 /tmp/wifi-scan
}

scan() {
  $W scan >/dev/null 2>&1; sleep 4
  $W scan_results 2>/dev/null | tail -n +2 | awk -F'\t' '$5 != "" { printf "ap\t%s\t%s\t%s\n", $5, $3, ($4 ~ /WPA|WEP/ ? "locked" : "open") }' | sort -t"$(printf '\t')" -k3 -n -r | uniq > /tmp/wifi-scan
  say "scan: $(wc -l < /tmp/wifi-scan) networks"
}

bad() { say "error: $*"; exit 1; }

add() {
  ssid="$1"; pass="$2"; mode="$3"
  case "$pass" in *\"*|*\\*) bad "quotes and backslashes are not supported in the password" ;; esac
  [ -n "$ssid" ] && [ "$(printf %s "$ssid" | wc -c)" -le 32 ] || bad "the network name must be 1-32 characters"
  if [ -n "$pass" ]; then [ "$(printf %s "$pass" | wc -c)" -ge 8 ] && [ "$(printf %s "$pass" | wc -c)" -le 63 ] || bad "the password must be 8-63 characters"; fi
  if [ -f $D/pending ]; then
    # a leftover of an interrupted test (older than two minutes) does not block
    if [ -n "$(find $D/pending -mmin -2 2>/dev/null)" ]; then bad "a switch is already being tested, wait a minute"; fi
    rm -f $D/pending
  fi
  # an existing entry with this name is replaced by the new password
  $W list_networks | tail -n +2 | awk -F'\t' -v s="$ssid" '$2 == s && $4 !~ /CURRENT/ { print $1 }' | while read old; do $W remove_network "$old" >/dev/null; done
  cp $CONF "$D/backup-$(date +%s).conf" 2>/dev/null; ls -t $D/backup-*.conf 2>/dev/null | tail -n +6 | while read f; do rm -f "$f"; done
  prev=$($W status | sed -n 's/^id=//p')
  # which networks are enabled now (select_network switches all the others off for the duration of the test)
  $W list_networks | tail -n +2 | awk -F'\t' '$4 !~ /DISABLED/ { print $1 }' > $D/enabled-before
  id=$($W add_network | tail -1)
  case "$id" in ''|*[!0-9]*) bad "wpa_supplicant did not accept a new network" ;; esac
  ssid_hex=$(printf '%s' "$ssid" | od -An -tx1 | tr -d ' \n')
  $W set_network $id ssid $ssid_hex >/dev/null   # the name goes as hex (not in quotes): any characters, Cyrillic too
  if [ -n "$pass" ]; then
    # what the router offers (the flags of its beacon): WPA2 only, WPA2+WPA3 mixed, or WPA3 only
    flags=$($W scan_results 2>/dev/null | awk -F'\t' -v s="$ssid" '$5 == s { print $4 }' | head -1)
    ok() { [ "$($W "$@" 2>/dev/null | tail -1)" = "OK" ]; }
    if echo "$flags" | grep -q 'SAE' && ! echo "$flags" | grep -q 'WPA2-PSK'; then
      # WPA3 only; if this supplicant has no SAE, it falls back to the plain way below
      ok set_network $id sae_password "\"$pass\"" && ok set_network $id key_mgmt SAE && ok set_network $id ieee80211w 2 || sae_failed=1
    else sae_failed=1; fi
    if [ -n "$sae_failed" ]; then
      $W set_network $id psk "\"$pass\"" >/dev/null
      # WPA2 and WPA2/WPA3 mixed: the PMF ones too (some routers demand protected frames), then the plain one for old supplicants
      ok set_network $id key_mgmt "WPA-PSK WPA-PSK-SHA256" || $W set_network $id key_mgmt WPA-PSK >/dev/null
      $W set_network $id ieee80211w 1 >/dev/null 2>&1      # protected frames optional; an old supplicant ignores it
      $W set_network $id proto "WPA RSN" >/dev/null 2>&1; $W set_network $id pairwise "CCMP TKIP" >/dev/null 2>&1; $W set_network $id group "CCMP TKIP" >/dev/null 2>&1
    fi
  else $W set_network $id key_mgmt NONE >/dev/null; fi
  $W set_network $id scan_ssid 1 >/dev/null 2>&1                # also finds a hidden network and one that is quiet in the scan
  $W enable_network $id >/dev/null
  if [ "$mode" != "now" ]; then
    $W save_config >/dev/null
    say "added '$ssid': the speaker joins it when it is in range"
    return
  fi
  echo "testing '$ssid'" > $D/pending
  say "switching to '$ssid', going back in 45 s if it does not work"
  start-stop-daemon -S -b -x "$0" -- _watch "$id" "$prev" "$ssid"   # detached for real: survives the shell that called us
}

# The watchdog of "add ... now": a process of its own (setsid), so that it outlives whoever started it.
restore_enabled() {   # every stored network is enabled again (nobody disables one on purpose, and a leftover of an interrupted test must not stay off)
  $W list_networks | tail -n +2 | awk -F'\t' '{ print $1 }' | while read i; do $W enable_network "$i" >/dev/null; done
  tidy
}

tidy() {   # copies of the network in use (several entries with the same name) are removed, one is enough
  cur=$($W status | sed -n 's/^ssid=//p'); [ -n "$cur" ] || return 0
  $W list_networks | tail -n +2 | awk -F'\t' -v s="$cur" '$2 == s && $4 !~ /CURRENT/ { print $1 }' | while read old; do $W remove_network "$old" >/dev/null; done
  $W save_config >/dev/null
}

watch() {
  id="$1"; prev="$2"; ssid="$3"
  $W select_network $id >/dev/null
  n=0; joined=0; seen=""
  while [ $n -lt 9 ]; do
    sleep 5; n=$((n + 1))
    st=$($W status 2>/dev/null)
    seen=$(echo "$st" | sed -n 's/^wpa_state=//p')
    if [ "$(echo "$st" | sed -n 's/^ssid=//p')" = "$ssid" ] && [ "$seen" = "COMPLETED" ]; then
      joined=1
      if online; then
        restore_enabled
        $W save_config >/dev/null
        echo "ok: now on '$ssid'" > $D/last; rm -f $D/pending; exit 0
      fi
    fi
  done
  # why it did not work: joined but no gateway / never found / the handshake did not finish (usually a wrong password)
  if [ $joined = 1 ]; then why=nogw
  elif [ "$seen" = "SCANNING" ] || [ "$seen" = "INACTIVE" ]; then why=notfound
  else why=auth; fi
  $W remove_network $id >/dev/null
  if [ -n "$prev" ]; then $W select_network "$prev" >/dev/null; fi
  restore_enabled
  $W reconnect >/dev/null
  echo "failed: '$ssid' did not work [$why], went back to the previous network" > $D/last; rm -f $D/pending
}

forget() {
  ssid="$1"
  cur=$($W status | sed -n 's/^ssid=//p')
  [ "$ssid" != "$cur" ] || bad "that is the network in use"
  ids=$($W list_networks | tail -n +2 | awk -F'\t' -v s="$ssid" '$2 == s { print $1 }')
  [ -n "$ids" ] || bad "no such network"
  for i in $ids; do $W remove_network "$i" >/dev/null; done
  $W save_config >/dev/null
  say "forgot '$ssid'"
}

case "$1" in
  status) status ;;
  scan) scan ;;
  add) add "$2" "$3" "$4" ;;
  forget) forget "$2" ;;
  _watch) watch "$2" "$3" "$4" ;;
  *) echo "usage: wifi.sh status|scan|add SSID PASS [now]|forget SSID" ;;
esac