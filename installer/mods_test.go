package main

import (
	"bytes"
	"strings"
	"testing"
)

// The mods that a real Station 2 carried before lostos (the probe of 2026-10-28): Spotify Connect of a mod, a night light, an arecord that holds the converter.
func TestMatchModsStation2(t *testing.T) {
	cmds := []string{
		"/data/opt/spotify-connect/spotiya --name Kitchen ",
		"/bin/sh /data/opt/night_light_poll.sh ",
		"arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 /dev/null ",
		"maind --mediad --platformd ",
		"/system/vendor/quasar/quasar_launcher launch.json ",
	}
	hits := matchMods(cmds, []string{"/data/opt/spotify-connect", "/data/opt/night_light_poll.sh"})
	got := map[string]bool{}
	for _, h := range hits {
		got[h.id] = len(h.running) > 0
	}
	for _, id := range []string{"spotiya", "night-light", "adc-keeper"} {
		if !got[id] {
			t.Errorf("%s is running and was not found: %v", id, got)
		}
	}
	if len(hits) != 3 {
		t.Errorf("only the three mods were expected, got %d: %v", len(hits), got)
	}
}

// The scan itself is a process on the speaker while it runs, and its command line holds every name it looks for: it must not find itself (found on a real Mini 3).
func TestScanDoesNotFindItself(t *testing.T) {
	raw := "P:-sh -c for d in /proc/[0-9]*; do c=$(tr x y); echo P:$c; done; for f in /data/opt/spotify-connect /data/opt/night_light_poll.sh; do echo F:$f; done\n" +
		"P:/data/lostos/lostos /data/lostos/state.conf\n" +
		"P:arecord -D hw:0,1 -f S16_LE /dev/null\n" +
		"F:/data/opt/night_light_poll.sh\n" +
		"H:/system/etc/rc.local:12:/data/opt/night_light_poll.sh &\n"
	cmds, files, hooks := parseScan(raw)
	hits := matchMods(cmds, files)
	got := map[string]bool{}
	for _, h := range hits {
		got[h.id] = true
	}
	if len(cmds) != 1 || !got["adc-keeper"] || !got["night-light"] || got["spotiya"] || len(hooks) != 1 {
		t.Errorf("only the arecord and the night light file are real: cmds %v, hits %v, hooks %v", cmds, got, hooks)
	}
}

func TestMatchModsNothing(t *testing.T) {
	hits := matchMods([]string{"maind --mediad", "/data/lostos/lostos --daemon", "dropbear -p 22"}, nil)
	if len(hits) != 0 {
		t.Errorf("a clean speaker must have no mods, got %v", hits)
	}
}

// A mod that is installed but not running is mentioned, and does not count as one that gets in the way.
func TestMatchModsInstalledOnly(t *testing.T) {
	hits := matchMods(nil, []string{"/data/opt/night_light_poll.sh"})
	if len(hits) != 1 || len(hits[0].running) != 0 || len(hits[0].present) != 1 {
		t.Fatalf("expected one installed, not running mod: %v", hits)
	}
	var b bytes.Buffer
	save := out
	out = &b
	defer func() { out = save }()
	if n := reportMods(hits, ""); n != 0 {
		t.Errorf("a mod that is not running does not block: %d", n)
	}
	if !strings.Contains(b.String(), "installed") {
		t.Errorf("the report must say it is installed: %q", b.String())
	}
}

func TestInfoModDoesNotBlock(t *testing.T) {
	hits := matchMods([]string{"/data/opt/bin/alicebridge --serve"}, nil)
	var b bytes.Buffer
	save := out
	out = &b
	defer func() { out = save }()
	if len(hits) != 1 || reportMods(hits, "") != 0 {
		t.Errorf("AliceBridge is mentioned, not blocking: %v", hits)
	}
}

func TestReportShowsHooksAndReasons(t *testing.T) {
	hits := matchMods([]string{"/bin/sh /data/opt/night_light_poll.sh"}, nil)
	var b bytes.Buffer
	save := out
	out = &b
	defer func() { out = save }()
	n := reportMods(hits, "/system/etc/rc.local:12:/data/opt/night_light_poll.sh &")
	s := b.String()
	if n != 1 || !strings.Contains(s, "/dev/fb0") || !strings.Contains(s, "rc.local:12") || !strings.Contains(s, "not touched") {
		t.Errorf("the report is incomplete (blocking %d): %s", n, s)
	}
}
