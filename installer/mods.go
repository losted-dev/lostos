package main

// Mods of other people on the speaker that get in the way of lostos. A rooted Station often carries something of its owner's: a Spotify Connect of its own, a night light on the LED panel, a
// program that holds the microphone converter. The installer looks (one adb call, read only), names what it found and why it matters, and offers to stop the running ones until the next reboot
// (--stop-mods). It never edits or deletes anything of theirs: a mod belongs to the owner of the speaker, and the lines that start it are only shown, so that he can take them out himself.

import (
	"fmt"
	"strings"
)

type mod struct {
	id, title string
	procs     []string // a piece of the command line of a running process
	files     []string // a path that says the mod is installed
	why       string   // what it does to lostos
	stop      string   // stops it until the next reboot (empty: only to be mentioned)
	info      bool     // does not get in the way: said, but not a reason to stop or to ask
}

var knownMods = []mod{
	{id: "spotiya", title: "spotiya (a Spotify Connect receiver of a mod)", procs: []string{"spotiya", "spotify-connect", "librespot-mod"}, files: []string{"/data/opt/spotify-connect"},
		why:  "it plays through the same speaker and takes the name in Spotify: with lostos's own Spotify Connect there are two receivers of the same name, and the sound is taken by whichever started first",
		stop: "killall spotiya 2>/dev/null; pkill -f spotify-connect 2>/dev/null"},
	{id: "night-light", title: "night_light_poll.sh (a night light on the LED panel)", procs: []string{"night_light_poll"}, files: []string{"/data/opt/night_light_poll.sh"},
		why:  "it writes into the LED panel (/dev/fb0), which lostos owns: the light flickers between two programs and the colours of the states (listening, thinking) are overwritten",
		stop: "pkill -f night_light_poll 2>/dev/null"},
	{id: "adc-keeper", title: "arecord that holds the microphone converter (hw:0,1)", procs: []string{"arecord -D hw:0,1", "arecord -Dhw:0,1"},
		why:  "it keeps the converter open: lostos then cannot open the microphones, or hears silence",
		stop: "pkill -f 'arecord -D hw:0,1' 2>/dev/null; pkill -f 'arecord -Dhw:0,1' 2>/dev/null"},
	{id: "led-panel", title: "an effects panel for the LED strip (ya2-panel and the like)", procs: []string{"station-led-panel", "ya2-panel", "led-effects"}, files: []string{"/tmp/station-led-panel.lock"},
		why:  "it plays its own frames on the LED panel (/dev/fb0) and fights lostos for it",
		stop: "pkill -f station-led-panel 2>/dev/null; pkill -f ya2-panel 2>/dev/null; pkill -f led-effects 2>/dev/null"},
	{id: "luna", title: "Luna (another shell for the speaker)", procs: []string{"/luna", "luna-daemon", "luna.sh"}, files: []string{"/data/luna"},
		why:  "it drives the same microphones, lights and sound as lostos: only one of them can own them",
		stop: "pkill -f luna-daemon 2>/dev/null; pkill -f luna.sh 2>/dev/null"},
	{id: "shairport", title: "a shairport-sync or another AirPlay receiver of a mod", procs: []string{"shairport-sync", "airplay-mod"}, files: []string{"/data/opt/shairport-sync"},
		why:  "AirPlay of lostos (a system plugin) is the same kind of receiver: two of them take the same port and the same name",
		stop: "pkill -f 'shairport-sync' 2>/dev/null"},
	{id: "squeeze", title: "squeezelite, snapclient or another player of a mod", procs: []string{"squeezelite", "snapclient", "raspotify", "mpd "},
		why:  "it plays through the same sound card: two players at once give silence or a crackle",
		stop: "pkill squeezelite 2>/dev/null; pkill snapclient 2>/dev/null; pkill raspotify 2>/dev/null"},
	{id: "alicebridge", title: "AliceBridge (the bridge of the stock Alice to a smart home)", procs: []string{"alicebridge"}, files: []string{"/data/opt/bin/alicebridge-env.sh"}, info: true,
		why: "it lives on the stock Yandex stack, which lostos stops: it will do nothing while lostos runs, and it is left as it is (its lines in rc.local are not touched)"},
}

// modHit is one mod that was found on a speaker.
type modHit struct {
	mod
	running []string // the command lines that match
	present []string // the paths that exist
}

// matchMods compares what the speaker said (the command lines of its processes, the paths that exist) with the known mods.
func matchMods(cmdlines, present []string) []modHit {
	var out []modHit
	for _, m := range knownMods {
		h := modHit{mod: m}
		for _, c := range cmdlines {
			for _, p := range m.procs {
				if strings.Contains(c, p) {
					h.running = append(h.running, strings.TrimSpace(c))
					break
				}
			}
		}
		for _, f := range m.files {
			for _, p := range present {
				if p == f {
					h.present = append(h.present, f)
				}
			}
		}
		if len(h.running) > 0 || len(h.present) > 0 {
			out = append(out, h)
		}
	}
	return out
}

// detectMods asks the speaker (one read-only call). Our own processes (lostos, the install itself, adb) are never counted.
func detectMods(s string) ([]modHit, string) {
	var paths []string
	for _, m := range knownMods {
		paths = append(paths, m.files...)
	}
	script := `for d in /proc/[0-9]*; do c=$(tr '\0' ' ' < $d/cmdline 2>/dev/null); [ -n "$c" ] && echo "P:$c"; done; for f in ` + strings.Join(paths, " ") + `; do [ -e "$f" ] && echo "F:$f"; done; ` +
		`for f in /system/etc/rc.local /data/quasar/rootkeeper.sh /etc/init.d/S*; do [ -f "$f" ] && grep -s -n -E "/data/opt|spotiya|night_light|luna|alicebridge|arecord" "$f" | sed "s#^#H:$f:#"; done`
	raw, _ := sh(s, script)
	cmds, files, hooks := parseScan(raw)
	return matchMods(cmds, files), strings.Join(hooks, "\n")
}

// reportMods prints what was found. blocking: how many get in the way (not only mentioned).
func reportMods(hits []modHit, hooks string) (blocking int) {
	if len(hits) == 0 {
		return 0
	}
	fmt.Fprintln(out, "  mods of other people on this speaker:")
	for _, h := range hits {
		state := "installed"
		if len(h.running) > 0 {
			state = "RUNNING"
		}
		mark := "!"
		if h.info {
			mark = "i"
		} else if len(h.running) > 0 {
			blocking++
		}
		fmt.Fprintf(out, "   [%s] %s — %s\n       %s\n", mark, h.title, state, h.why)
		for _, c := range h.running {
			fmt.Fprintf(out, "       process: %s\n", truncate(c, 110))
		}
		for _, f := range h.present {
			fmt.Fprintf(out, "       found:   %s\n", f)
		}
	}
	if hooks != "" {
		fmt.Fprintln(out, "  lines that start such things at boot (shown, not touched; take them out yourself if you want the mod gone):")
		for _, l := range strings.Split(hooks, "\n") {
			fmt.Fprintf(out, "       %s\n", truncate(l, 140))
		}
	}
	return blocking
}

// checkMods runs before the files are copied. A running mod that is in the way is stopped until the next reboot when --stop-mods says so; otherwise the person is asked (--yes goes on:
// the wizard has no terminal, it prints and goes on). false: the person said no.
func checkMods(s string) bool {
	hits, hooks := detectMods(s)
	n := reportMods(hits, hooks)
	if n == 0 {
		return true
	}
	if opts.stopMods {
		for _, h := range hits {
			if len(h.running) > 0 && !h.info && h.stop != "" {
				fmt.Fprintf(out, "  stopping %s until the next reboot\n", h.id)
				_, _ = sh(s, h.stop)
			}
		}
		return true
	}
	if opts.yes || opts.dryRun {
		fmt.Fprintln(out, "  (nothing is stopped: --stop-mods stops the running ones until the next reboot)")
		return true
	}
	ans := strings.ToLower(readLine("  go on with these mods running? they may keep lostos from working [y/N] "))
	if ans != "y" && ans != "yes" {
		fmt.Fprintln(errw, "  stopped: run again with --stop-mods to stop them until the next reboot, or stop them yourself")
		return false
	}
	return true
}

// parseScan splits what the scan printed: P: the command lines of the processes, F: the paths that exist, H: the lines of the boot scripts that start such things.
func parseScan(raw string) (cmds, files, hooks []string) {
	for _, l := range strings.Split(raw, "\n") {
		switch {
		case strings.HasPrefix(l, "P:"):
			c := l[2:]
			if strings.Contains(c, "/data/lostos/") || strings.Contains(c, "lostos-install") || strings.HasPrefix(c, "tr ") || strings.Contains(c, "for d in /proc/") {
				continue // our own processes, and this very scan (its text holds the names that it looks for)
			}
			cmds = append(cmds, c)
		case strings.HasPrefix(l, "F:"):
			files = append(files, strings.TrimSpace(l[2:]))
		case strings.HasPrefix(l, "H:"):
			hooks = append(hooks, strings.TrimSpace(l[2:]))
		}
	}
	return
}
