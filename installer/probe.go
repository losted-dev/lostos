package main

import (
	"fmt"
	"regexp"
)

// Devicetree codename -> name / userland ABI (build/<arch>/): all of it is in the catalog of models (catalog.go), which the package and the signed catalog of the lostOS
// repository extend. A model that is not in it, or whose support is only being prepared, is not installed: the daemon drives model-specific hardware (mic array + VQE
// preset, LEDs, clock, inputs).
func modelName(code string) string { return catalog[code].Name }

// userlandArch is the instruction set of the PROGRAMS of the station, which is not always the one of its processor: the Midi has a 64-bit kernel and
// a 32-bit userland (its firmware of October 2026: ld-linux-armhf, glibc 2.36). The loader of the userland tells; archOf is the fallback.
func userlandArch(s, code string) string {
	if _, err := sh(s, "[ -e /lib/ld-linux-aarch64.so.1 ] || [ -e /lib64/ld-linux-aarch64.so.1 ]"); err == nil {
		return "aarch64"
	}
	if _, err := sh(s, "[ -e /lib/ld-linux-armhf.so.3 ]"); err == nil {
		return "armhf"
	}
	return archOf(code)
}

// archOf: devicetree codename -> userland ABI (build/<arch>/); "" = not supported (not in the catalog, or the support is only being prepared).
func archOf(code string) string {
	if m, ok := catalog[code]; ok && m.Status != "preparing" {
		return m.Arch
	}
	return ""
}

// hasZigbee: the model has the Zigbee radio the daemon can run as a router (radio/), so the router
// firmware image is installed next to the daemon.
func hasZigbee(code string) bool { return catalog[code].Zigbee }

type station struct {
	serial, arch, code string
	zigbee             bool
	legacy             bool // no rootkeeper here: the autostart is our own init script /etc/init.d/S98lostos
}

// builtinModel: the daemon knows the model without a profile file.
func builtinModel(code string) bool { return catalog[code].Builtin }

// found: installable stations, in the order printed by probe.
var found []station

// probeResult is what the wizard shows for every device that answered (installable or not).
type probeResult struct {
	Serial      string `json:"serial"`
	Name        string `json:"name"`
	Code        string `json:"code"`
	State       string `json:"state"`
	Installable bool   `json:"installable"`
	Reason      string `json:"reason"`
	// the receivers (AirPlay, Spotify) that the archive can put on this station, and which of them it has already
	Receivers []receiverInfo `json:"receivers"`
}

var probed []probeResult

var codename = regexp.MustCompile(`yandex,([a-z0-9_]*)`)

// probe prints one status line for s and records it in found if it is installable. A device we
// connected to ourselves is disconnected again when it is not.
func probe(s string, ours bool) {
	rec := probeResult{Serial: s}
	skip := func(msg string) {
		rec.Reason = msg
		probed = append(probed, rec)
		fmt.Fprintf(out, "  %s: %s\n", s, msg)
		if ours {
			disconnect(s)
		}
	}
	compat, err := sh(s, `tr '\0' ' ' </proc/device-tree/compatible`)
	if err != nil {
		skip("adb not usable (unauthorized or offline)")
		return
	}
	m := codename.FindStringSubmatch(compat)
	if m == nil || m[1] == "" {
		skip("not a Yandex Station")
		return
	}
	code := m[1]
	rec.Code = code
	name := modelName(code)
	if name == "" {
		name = "unknown Yandex device"
	}
	rec.Name = name
	name += " (" + code + ")"
	if archOf(code) == "" {
		if catalog[code].Status == "preparing" {
			skip(name + " — support is being prepared (" + catalog[code].Note + "), skipped")
		} else {
			skip(name + " — not supported yet, skipped")
		}
		return
	}
	if uid, _ := sh(s, "id -u"); uid != "0" {
		skip(name + " — adb is not root, skipped")
		return
	}
	// legacy: no rootkeeper on this firmware, so the autostart is our own init script (/etc/init.d/S98lostos, independent of AirPlay or anything else).
	legacy := false
	if _, err := sh(s, "grep -q rootkeeper.sh /system/vendor/quasar/onboot.sh"); err != nil {
		// the script that starts the Yandex stack is S99quasar on some firmware and S97quasar on others (Midi)
		if _, err := sh(s, "[ -d /etc/init.d ] && ls /etc/init.d/S??quasar >/dev/null 2>&1"); err != nil {
			skip(name + " — firmware has neither " + rkPath + " nor an init.d to start from, skipped")
			return
		}
		legacy = true
	}
	// The daemon uses the speaker's own libraries (echo cancellation, wake word, playback): without them it cannot start.
	if _, err := sh(s, "for f in libyandex_evil libtflite libquasar_gstreamer; do [ -f /system/vendor/quasar/libs/$f.so ] || exit 1; done"); err != nil {
		skip(name + " — the firmware libraries of Yandex are not in /system/vendor/quasar/libs, skipped")
		return
	}
	var state string
	if _, err := sh(s, "pidof lostos >/dev/null"); err == nil {
		state = "satellite running"
	} else if _, err := sh(s, "[ -x "+dataDir+"/lostos ]"); err == nil {
		state = "satellite installed, not running"
	} else {
		state = "stock Yandex"
	}
	if legacy {
		state += " (autostart: /etc/init.d/S98lostos)"
	}
	if catalog[code].Status == "untested" {
		state += " [the profile is not tried on a real speaker yet]"
	}
	rec.State, rec.Installable = state, true
	arch := userlandArch(s, code)
	rec.Receivers = receiverInfos(s, arch)
	probed = append(probed, rec)
	found = append(found, station{s, arch, code, hasZigbee(code), legacy})
	fmt.Fprintf(out, "  [%d] %s: %s — %s\n", len(found), s, name, state)
}
