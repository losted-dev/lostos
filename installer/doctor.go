package main

import (
	"fmt"
	"strings"
)

// doctor looks at a station that already runs lostOS (or should): what is installed, whether it runs, whether it starts by itself, whether it is linked to a server, how much room is left.
// It changes nothing. --fix after it runs the normal installation for the stations in which something is wrong (it is safe to repeat), then looks again.
type finding struct {
	ok   bool
	text string
	fix  bool // the installation puts it right
}

func doctor(st station) []finding {
	s := st.serial
	var out []finding
	add := func(ok bool, fix bool, format string, a ...any) {
		out = append(out, finding{ok, fmt.Sprintf(format, a...), fix})
	}
	run := func(cmd string) (string, bool) { v, err := sh(s, cmd); return strings.TrimSpace(v), err == nil }

	m := catalog[st.code]
	add(true, false, "model: %s (%s)%s", m.Name, st.code, map[string]string{"untested": ", the profile is not tried on a real speaker", "verified": ""}[m.Status])
	if _, ok := run("[ -x " + dataDir + "/lostos ]"); !ok {
		add(false, true, "lostos is not installed on the station (no %s/lostos)", dataDir)
		return out
	}
	sum, _ := run("sha256sum " + dataDir + "/lostos")
	add(true, false, "daemon: %s/lostos, SHA-256 %s", dataDir, truncate(firstField(sum), 12))
	if _, ok := run("pidof lostos"); ok {
		add(true, false, "lostos is running")
	} else {
		add(false, true, "lostos is not running")
	}
	if st.legacy {
		if _, ok := run("[ -x /etc/init.d/S98lostos ]"); ok {
			add(true, false, "autostart: /etc/init.d/S98lostos")
		} else {
			add(false, true, "autostart: /etc/init.d/S98lostos is missing, lostos will not start after a reboot")
		}
	} else if _, ok := run("grep -q lostos " + rkPath); ok {
		add(true, false, "autostart: the hook in %s", rkPath)
	} else {
		add(false, true, "autostart: the hook is not in %s, lostos will not start after a reboot", rkPath)
	}
	if !builtinModel(st.code) {
		if _, ok := run("[ -s " + dataDir + "/models/" + st.code + ".conf ]"); ok {
			add(true, false, "the model profile %s.conf is on the station", st.code)
		} else {
			add(false, true, "the model profile %s.conf is not on the station: the daemon does not know this model", st.code)
		}
	}
	if conf, ok := run("cat " + dataDir + "/link.conf"); ok && strings.Contains(conf, "server=") {
		var url string
		for _, l := range strings.Split(conf, "\n") {
			if strings.HasPrefix(l, "server=") {
				url = strings.TrimSpace(strings.TrimPrefix(l, "server="))
			}
		}
		if !strings.HasPrefix(url, "https://") {
			add(false, false, "link.conf: the server must be an https:// address, now %q (the station refuses anything else)", url)
		} else if info, err := checkServer(url); err != nil {
			add(false, false, "the server %s does not answer from this computer: %v", url, err)
		} else {
			add(true, false, "linked to %s (a lostOS server %s)", url, info.Version)
			if id, state := readLinkState(s); id != "" {
				add(true, false, "station id %s, %s", id, state)
			}
		}
	} else {
		add(true, false, "not linked to a server (no server= in link.conf): it works as an ESPHome device for Home Assistant")
	}
	if free, ok := run("df -k " + dataDir + " | tail -1 | awk '{print $4}'"); ok {
		kb := 0
		fmt.Sscanf(free, "%d", &kb)
		add(kb > 8*1024, false, "free room in /data: %d MB%s", kb/1024, map[bool]string{true: "", false: " (less than 8 MB: an update may not fit)"}[kb > 8*1024])
	}
	if errs, ok := run("tail -n 200 /tmp/lostos.log 2>/dev/null | grep -ciE ' E |crash|segfault|abort'"); ok && errs != "0" && errs != "" {
		add(false, false, "the log has %s lines with errors: adb shell tail -n 80 /tmp/lostos.log", errs)
	}
	return out
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// runDoctor prints what the doctor sees for every station found and, with --fix, installs again where something that an installation can put right is wrong.
func runDoctor(stations []station) int {
	bad := 0
	for _, st := range stations {
		fmt.Fprintf(out, "== %s: %s\n", st.serial, modelName(st.code))
		list := doctor(st)
		needFix := false
		for _, f := range list {
			mark := "  ok   "
			if !f.ok {
				mark = "  FAIL "
				bad++
				needFix = needFix || f.fix
			}
			fmt.Fprintln(out, mark+f.text)
		}
		if needFix && opts.fix {
			fmt.Fprintf(out, "== %s: putting it right (the installation, safe to repeat)\n", st.serial)
			if !installOne(st) {
				return 1
			}
			for _, f := range doctor(st) {
				if !f.ok {
					fmt.Fprintln(out, "  FAIL "+f.text)
				}
			}
		} else if needFix {
			fmt.Fprintln(out, "  run again with --fix to put it right")
		}
	}
	if bad == 0 {
		fmt.Fprintln(out, "everything is in order")
		return 0
	}
	return 1
}
