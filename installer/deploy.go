package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

// build builds missing outputs in a repository checkout; the release archive ships them and no
// sources.
func build() bool {
	sat := filepath.Join(root, "satellite")
	if _, err := os.Stat(filepath.Join(sat, "Makefile")); err != nil {
		return true
	}
	fmt.Fprintln(out, "== build")
	fail := func(err error) bool {
		fmt.Fprintf(errw, "build: %v (building from source needs Linux or WSL, see README)\n", err)
		return false
	}
	mk := exec.Command("make", "-C", sat, fmt.Sprintf("-j%d", runtime.NumCPU()))
	mk.Stderr = errw
	if err := mk.Run(); err != nil {
		return fail(err)
	}
	if opts.ssh {
		for _, a := range []string{"aarch64", "armhf"} {
			if st, err := os.Stat(filepath.Join(root, "ssh", "out", a, "dropbearmulti")); err != nil || st.Size() == 0 {
				b := exec.Command(filepath.Join(root, "ssh", "build.sh"))
				b.Stderr = errw
				if err := b.Run(); err != nil {
					return fail(err)
				}
				break
			}
		}
	}
	return true
}

func sha256File(p string) string {
	f, err := os.Open(p)
	if err != nil {
		return ""
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return ""
	}
	return hex.EncodeToString(h.Sum(nil))
}

// installOne deploys, (re)starts and checks one station; every step checks its own status.
// linkFollows: the caller will call configureLink right after installOne (see the boot show there).
var linkFollows bool

func installOne(st station) bool {
	s := st.serial
	bin, fwNote, err := firmwareBinary(st.arch)
	if err != nil {
		fmt.Fprintf(errw, "%s: %v\n", s, err)
		return false
	}
	fmt.Fprintf(out, "== %s: daemon %s\n", s, fwNote)
	if fi, err := os.Stat(bin); err != nil || fi.Size() == 0 {
		fmt.Fprintf(errw, "%s: %s missing\n", s, bin)
		return false
	}
	ts := time.Now().Format("20060102150405")
	chosen := selectReceivers(st)
	if !checkSpace(s, bin, receiversKB(st.arch, chosen)) {
		return false
	}
	if opts.dryRun {
		return dryRun(s, bin, st, chosen)
	}
	changed, supChanged, ok := deployFiles(s, bin, ts, st.arch, st.zigbee, st.legacy)
	if !ok {
		return false
	}
	if !deployReceivers(s, st, chosen) {
		return false
	}
	if opts.noEnable {
		return true
	}
	fmt.Fprintf(out, "== %s: start\n", s)
	// Marker in the log: the health check looks only at what happens after it.
	mark := fmt.Sprintf("=== install-marker %d %d", os.Getpid(), time.Now().Unix())
	_ = shTo(out, s, "echo '"+mark+"' >> /tmp/lostos.log")
	// The boot show (the chime, the ring, the clock) plays once per power-up: the marker says it has. The daemon that starts LAST in the installation plays it, once, so that the person sees and hears
	// that lostOS is there. When the link to a server follows (configureLink restarts the daemon once more) this start must stay silent: the marker is set; else it is taken away.
	if linkFollows {
		_ = shTo(out, s, "touch /tmp/lostos-boot-done")
	} else {
		_ = shTo(out, s, "rm -f /tmp/lostos-boot-done")
	}
	action := "restart"
	if supChanged {
		action = "reload"
	}
	_ = shTo(out, s, "if [ -f "+dataDir+"/enabled ] && { pidof supervise.sh || pidof lostos; } >/dev/null; then "+
		dataDir+"/ctl.sh "+action+"; else "+dataDir+"/ctl.sh enable; fi")
	fmt.Fprintln(out)
	if !healthCheck(s, changed, mark) {
		return false
	}
	_ = shTo(out, s, dataDir+"/ctl.sh status; "+dataDir+"/ctl.sh log 5")
	fmt.Fprintln(out)
	return true
}

// deployFiles copies the daemon, scripts, the radio's firmware images (zigbee), dropbear (--ssh)
// and boot hooks. changed: the binary was replaced; supChanged: supervise.sh differs from the
// installed one.
func deployFiles(s, bin, ts, arch string, zigbee, legacy bool) (changed, supChanged, ok bool) {
	code := stationCode(s)
	changed, supChanged = true, true
	sshOut := filepath.Join(root, "ssh", "out", arch)
	deploy := filepath.Join(root, "satellite", "deploy")
	fmt.Fprintf(out, "== %s: daemon (%s)\n", s, arch)
	if shTo(out, s, "mkdir -p "+dataDir) != nil {
		return
	}
	want := sha256File(bin)
	have, _ := sh(s, "[ -f "+dataDir+"/lostos ] && sha256sum "+dataDir+"/lostos")
	if want == firstField(have) {
		fmt.Fprintln(out, "binary identical to the installed one, not copied (.prev kept)")
		changed = false
	} else {
		if push(s, bin, dataDir+"/lostos.new") != nil {
			return
		}
		got, _ := sh(s, "sha256sum "+dataDir+"/lostos.new")
		if want != firstField(got) {
			_ = shTo(out, s, "rm -f "+dataDir+"/lostos.new")
			fmt.Fprintf(errw, "%s: push corrupted (sha256 mismatch), nothing changed\n", s)
			return
		}
	}
	// Scripts: a running shell reads its script incrementally, so replace them atomically (new inode).
	// A running supervisor keeps executing the old inode: when supervise.sh changed it is reloaded.
	supHave, _ := sh(s, "[ -f "+dataDir+"/supervise.sh ] && sha256sum "+dataDir+"/supervise.sh")
	if firstField(supHave) == scriptSHA(filepath.Join(deploy, "supervise.sh")) {
		supChanged = false
	}
	for _, f := range []string{"boot.sh", "supervise.sh", "ctl.sh"} {
		if pushScript(s, filepath.Join(deploy, f), dataDir+"/"+f+".new") != nil {
			return
		}
	}
	if shTo(out, s, "cd "+dataDir+" && chmod 755 boot.sh.new supervise.sh.new ctl.sh.new &&\n"+
		"     mv boot.sh.new boot.sh && mv supervise.sh.new supervise.sh && mv ctl.sh.new ctl.sh") != nil {
		return
	}
	if changed {
		if shTo(out, s, "cd "+dataDir+" && chmod 755 lostos.new &&\n"+
			"       { [ ! -f lostos ] || cp -p lostos lostos.prev; } &&\n"+
			"       mv lostos.new lostos && sync") != nil {
			return
		}
	} else {
		_ = shTo(out, s, "sync")
	}

	if zigbee && (!deployRadioImage(s, "zigbee-coordinator.ybl") || !deployRadioImage(s, "zigbee-router.ybl")) {
		return
	}

	if !deployProfile(s, code) {
		return
	}

	if opts.ssh {
		fmt.Fprintf(out, "== %s: dropbear\n", s)
		if shTo(out, s, "mkdir -p /data/dropbear") != nil ||
			push(s, filepath.Join(sshOut, "dropbearmulti"), "/data/dropbear/dropbearmulti.new") != nil ||
			push(s, filepath.Join(sshOut, "sftp-server"), "/data/dropbear/sftp-server.new") != nil ||
			push(s, filepath.Join(root, "ssh", "start.sh"), "/data/dropbear/start.sh") != nil {
			return
		}
		// Running binaries stay busy; mv replaces them atomically.
		if shTo(out, s, "cd /data/dropbear && chmod 755 dropbearmulti.new sftp-server.new start.sh &&\n"+
			"       mv dropbearmulti.new dropbearmulti && mv sftp-server.new sftp-server && sync") != nil {
			return
		}
	}

	fmt.Fprintf(out, "== %s: boot hooks\n", s)
	if legacy {
		if !bootInit(s) {
			return
		}
	} else if !bootHooks(s, ts) {
		return
	}

	if opts.ssh {
		if shTo(out, s, "/data/dropbear/start.sh </dev/null >/dev/null 2>&1") != nil {
			fmt.Fprintln(errw, "не удалось безопасно перезапустить SSH")
			return
		}
		fmt.Fprintln(out, "SSH принимает только ключи из authorized_keys пользователя root; настройте свой публичный ключ через ADB до первого входа")
	}
	return changed, supChanged, true
}

// deployRadioImage copies one of lostOS's radio firmware images, read by the daemon only when the
// Zigbee role that needs it is chosen. A checkout without radio/build.sh output keeps the installed
// one.
func deployRadioImage(s, name string) bool {
	img := filepath.Join(root, "radio", "out", name)
	if fi, err := os.Stat(img); err != nil || fi.Size() == 0 {
		fmt.Fprintln(out, "radio/out/"+name+" missing (radio/build.sh), not copied")
		return true
	}
	want := sha256File(img)
	have, _ := sh(s, "[ -f "+dataDir+"/"+name+" ] && sha256sum "+dataDir+"/"+name)
	if firstField(have) == want {
		return true
	}
	if push(s, img, dataDir+"/"+name+".new") != nil {
		return false
	}
	got, _ := sh(s, "sha256sum "+dataDir+"/"+name+".new")
	if firstField(got) != want {
		_ = shTo(out, s, "rm -f "+dataDir+"/"+name+".new")
		fmt.Fprintf(errw, "%s: push corrupted (sha256 mismatch), %s not replaced\n", s, name)
		return false
	}
	return shTo(out, s, "cd "+dataDir+" && mv "+name+".new "+name+" && sync") == nil
}

// bootHooks adds the hook lines to rootkeeper.sh (planRootkeeper), keeping a backup.
func bootHooks(s, ts string) bool {
	tmp, err := os.MkdirTemp("", "lostos-install")
	if err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	defer os.RemoveAll(tmp)
	rkFile := filepath.Join(tmp, "rk")
	rk := "#!/bin/sh\n"
	if shTo(out, s, "[ -f "+rkPath+" ]") == nil {
		if pull(s, rkPath, rkFile) != nil {
			return false
		}
		b, err := os.ReadFile(rkFile)
		if err != nil {
			fmt.Fprintln(errw, err)
			return false
		}
		rk = string(b)
	}
	newRK, added, write := planRootkeeper(rk, opts.ssh)
	if !write {
		fmt.Fprintln(out, "already present in "+rkPath)
		return true
	}
	// Nothing but our hook lines and blank separators may change.
	if bad := checkRootkeeper(rk, newRK); bad != "" {
		fmt.Fprintf(errw, "%s: unexpected change to %s, not written:\n%s\n", s, rkPath, bad)
		return false
	}
	newFile := filepath.Join(tmp, "rk.new")
	if err := os.WriteFile(newFile, []byte(newRK), 0o644); err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	if push(s, newFile, rkPath+".new") != nil {
		return false
	}
	// Checked by the shell that runs it at boot (busybox sh).
	if _, err := sh(s, "sh -n "+rkPath+".new"); err != nil {
		_, _ = sh(s, "rm -f "+rkPath+".new")
		fmt.Fprintf(errw, "%s: %s.new fails sh -n on the device, not written\n", s, rkPath)
		return false
	}
	bak := rkPath + ".bak-" + ts
	if shTo(out, s, "{ [ ! -f "+rkPath+" ] || cp -p "+rkPath+" "+bak+"; } && chmod 755 "+rkPath+".new && mv "+rkPath+".new "+rkPath+" && sync") != nil {
		return false
	}
	if got, _ := sh(s, "sha256sum "+rkPath); firstField(got) != sha256File(newFile) {
		fmt.Fprintf(errw, "%s: %s on the device differs from the checked file\n", s, rkPath)
		return false
	}
	if shTo(out, s, "[ -f "+bak+" ]") == nil {
		fmt.Fprintf(out, "%s changed, diff against %s on the device:\n", rkPath, bak)
		d, _ := shRaw(s, "diff "+bak+" "+rkPath)
		for _, l := range splitLines(d) {
			fmt.Fprintln(out, "  "+l)
		}
	} else {
		fmt.Fprintf(out, "created %s with:\n", rkPath)
		for _, l := range added {
			fmt.Fprintln(out, "  "+l)
		}
	}
	return true
}

// healthCheck runs after a (re)start, at ~10 s and again at ~30 s: the process is alive and, in
// the log after the install marker, there is exactly one `=== start` and no crash report, exit or
// supervisor message (a binary that crashes and is restarted in between would otherwise look
// healthy). On failure, offer to put the previous binary back.
func healthCheck(s string, changed bool, mark string) bool {
	var log string
	bad, unknown := false, false
	for round := 1; round <= 2; round++ {
		if round == 1 {
			time.Sleep(10 * time.Second)
		} else {
			time.Sleep(20 * time.Second)
		}
		// The log may have been copy-truncated by supervise.sh meanwhile: the older half is in .1.
		full, _ := sh(s, "cat /tmp/lostos.log.1 /tmp/lostos.log 2>/dev/null | tail -n 1500")
		log, _ = sh(s, "tail -n 600 /tmp/lostos.log")
		if !strings.Contains(log, mark) {
			if strings.Contains(full, mark) {
				log = full
			} else {
				log = ""
			}
		}
		if log == "" {
			log = "(install marker not found in the log, even in .1)"
			unknown = true
			break
		}
		// The old daemon's exit (and the restart) land after the marker: judge only what follows
		// the first `=== start` after it, which is the new daemon's.
		_, log, _ = strings.Cut(log, mark)
		i := strings.Index(log, "=== start")
		if i < 0 {
			bad = true
			log = "(no start after the install marker)" + log
			break
		}
		log = log[i:]
		n := 0
		for _, l := range strings.Split(log, "\n") {
			if strings.HasPrefix(l, "=== start") {
				n++
			}
		}
		if shTo(out, s, "pidof lostos >/dev/null") != nil || n != 1 ||
			strings.Contains(log, "=== CRASH") || strings.Contains(log, "=== exit") || strings.Contains(log, "=== supervise") {
			bad = true
			break
		}
	}
	if unknown {
		fmt.Fprintf(errw, "%s: health check UNKNOWN: %s; not rolling back automatically\n", s, log)
		return false
	}
	if !bad {
		fmt.Fprintf(out, "%s: health check ok\n", s)
		return true
	}
	fmt.Fprintf(errw, "%s: health check FAILED, log:\n", s)
	lines := strings.Split(log, "\n")
	if len(lines) > 15 {
		lines = lines[len(lines)-15:]
	}
	fmt.Fprintln(errw, strings.Join(lines, "\n"))
	if changed && shTo(out, s, "[ -f "+dataDir+"/lostos.prev ]") == nil {
		ans := "y"
		if !opts.yes {
			ans = readLine("roll back to the previous binary? [Y/n] ")
		}
		if !strings.HasPrefix(ans, "n") && !strings.HasPrefix(ans, "N") {
			_ = shTo(out, s, "cd "+dataDir+" && cp -p lostos.prev lostos.rb && mv lostos lostos.bad &&\n"+
				"        mv lostos.rb lostos && sync && ./ctl.sh restart")
			fmt.Fprintf(errw, "%s: rolled back (bad binary kept as %s/lostos.bad)\n", s, dataDir)
		}
	}
	return false
}

// checkSpace: the daemon needs a few MB on /data (twice the binary while it is replaced, one more copy is kept as .prev) and some room for plugins.
func checkSpace(s, bin string, extraKB int) bool {
	fi, err := os.Stat(bin)
	if err != nil {
		return false
	}
	need := int(fi.Size()/1024)*3 + 6*1024 + extraKB // KB: three copies of the binary + scripts, models, plugins + the receivers chosen
	df, err := sh(s, "df -k /data | tail -n 1 | awk '{print $4}'")
	free, _ := strconv.Atoi(strings.TrimSpace(df))
	if err != nil || free == 0 {
		fmt.Fprintf(out, "%s: could not read the free space of /data, continuing\n", s)
		return true
	}
	fmt.Fprintf(out, "%s: /data has %d MB free, lostos needs about %d MB\n", s, free/1024, need/1024+1)
	if free < need {
		fmt.Fprintf(errw, "%s: not enough space on /data\n", s)
		return false
	}
	return true
}

// stationCode returns the devicetree codename of the station (yandex,<code>), "" when unknown.
func stationCode(s string) string {
	compat, err := sh(s, `tr '\0' ' ' </proc/device-tree/compatible`)
	if err != nil {
		return ""
	}
	if m := codename.FindStringSubmatch(compat); m != nil {
		return m[1]
	}
	return ""
}

// deployProfile puts the model profile of the station (satellite/models/<code>.conf) on it when the daemon has no built-in one
// (the profile file wins over the built-in anyway; an identical file is not copied).
func deployProfile(s, code string) bool {
	if code == "" || builtinModel(code) {
		return true
	}
	file, err := profileFile(code)
	if err != nil {
		fmt.Fprintf(errw, "%s: %v, the daemon will not know this station\n", s, err)
		return false
	}
	fmt.Fprintf(out, "== %s: model profile %s\n", s, code)
	dst := dataDir + "/models/" + code + ".conf"
	have, _ := sh(s, "[ -f "+dst+" ] && sha256sum "+dst)
	if firstField(have) == sha256File(file) {
		return true
	}
	if shTo(out, s, "mkdir -p "+dataDir+"/models") != nil || push(s, file, dst) != nil {
		return false
	}
	return true
}

// dryRun prints the plan and touches nothing.
func dryRun(s, bin string, st station, receivers []string) bool {
	fmt.Fprintf(out, "== %s: dry run, nothing is changed\n", s)
	fmt.Fprintf(out, "  model:    %s (%s)\n", modelName(st.code), st.code)
	fmt.Fprintf(out, "  daemon:   %s (%d KB) -> %s/lostos, the old one kept as lostos.prev\n", bin, fileKB(bin), dataDir)
	fmt.Fprintf(out, "  scripts:  boot.sh supervise.sh ctl.sh -> %s\n", dataDir)
	if !builtinModel(st.code) {
		fmt.Fprintf(out, "  profile:  satellite/models/%s.conf -> %s/models/\n", st.code, dataDir)
	}
	if len(receivers) == 0 {
		fmt.Fprintln(out, "  receivers: none (AirPlay and Spotify are not put on the station)")
	}
	for _, id := range receivers {
		fmt.Fprintf(out, "  receiver: %s (%d files, %d KB) -> %s/plugins/%s/, with the equaliser and the AirPlay sounds\n", receiverName(id), len(receiverFiles(st.arch, id)), dirKB(receiverSource(st.arch, id)), dataDir, id)
	}
	have, _ := sh(s, "[ -f "+rkPath+" ] && cat "+rkPath)
	if st.legacy {
		fmt.Fprintln(out, "  boot:     /etc/init.d/S98lostos (the firmware has no rootkeeper; the root is made writable for a moment and read-only again)")
	} else if strings.Contains(have, "/data/lostos/boot.sh") {
		fmt.Fprintln(out, "  boot:     the hook is already in "+rkPath)
	} else {
		fmt.Fprintln(out, "  boot:     one line added to "+rkPath+" (backup first): "+hookSat)
	}
	if !opts.noEnable {
		fmt.Fprintln(out, "  then:     the Yandex stack is stopped and lostos starts; if it does not stay up, the previous binary comes back")
	}
	return true
}

func fileKB(p string) int64 {
	if fi, err := os.Stat(p); err == nil {
		return fi.Size() / 1024
	}
	return 0
}

// uninstallOne gives the station back to Yandex: ctl.sh disable, our hook out of rootkeeper.sh (backup kept), data kept unless --purge.
func uninstallOne(st station) bool {
	s := st.serial
	fmt.Fprintf(out, "== %s: uninstall\n", s)
	if opts.dryRun {
		fmt.Fprintln(out, "  would run ctl.sh disable, remove the boot hook from "+rkPath+(map[bool]string{true: " and delete " + dataDir, false: ""}[opts.purge]))
		return true
	}
	_ = shTo(out, s, "[ -x "+dataDir+"/ctl.sh ] && "+dataDir+"/ctl.sh disable")
	tmp, err := os.MkdirTemp("", "lostos-uninstall")
	if err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	defer os.RemoveAll(tmp)
	if shTo(out, s, "[ -f "+initPath+" ]") == nil {
		_ = shTo(out, s, "mount -o remount,rw / && rm -f "+initPath+" && sync; mount -o remount,ro /")
		fmt.Fprintln(out, initPath+" removed")
	}
	if shTo(out, s, "[ -f "+rkPath+" ]") == nil {
		rkFile := filepath.Join(tmp, "rk")
		if pull(s, rkPath, rkFile) != nil {
			return false
		}
		b, err := os.ReadFile(rkFile)
		if err != nil {
			fmt.Fprintln(errw, err)
			return false
		}
		cleaned, removed := removeHooks(string(b), opts.ssh)
		if removed {
			if bad := checkRootkeeper(cleaned, string(b)); bad != "" {
				fmt.Fprintf(errw, "%s: unexpected difference in %s, not written:\n%s\n", s, rkPath, bad)
				return false
			}
			newFile := filepath.Join(tmp, "rk.new")
			if err := os.WriteFile(newFile, []byte(cleaned), 0o644); err != nil {
				fmt.Fprintln(errw, err)
				return false
			}
			ts := time.Now().Format("20060102150405")
			if push(s, newFile, rkPath+".new") != nil {
				return false
			}
			if _, err := sh(s, "sh -n "+rkPath+".new"); err != nil {
				_, _ = sh(s, "rm -f "+rkPath+".new")
				fmt.Fprintf(errw, "%s: the new %s fails sh -n, not written\n", s, rkPath)
				return false
			}
			if shTo(out, s, "cp -p "+rkPath+" "+rkPath+".bak-"+ts+" && chmod 755 "+rkPath+".new && mv "+rkPath+".new "+rkPath+" && sync") != nil {
				return false
			}
			fmt.Fprintln(out, "boot hook removed from "+rkPath+" (backup "+rkPath+".bak-"+ts+")")
		} else {
			fmt.Fprintln(out, "no lostos hook in "+rkPath)
		}
	}
	if opts.purge {
		_ = shTo(out, s, "rm -rf "+dataDir)
		fmt.Fprintln(out, dataDir+" deleted")
	}
	fmt.Fprintln(out, "done: reboot the station to bring the Yandex stack back for good (adb -s "+s+" reboot)")
	return true
}

// initScript is the autostart for firmware without rootkeeper: it only starts the boot script of lostOS, which stops the Yandex stack and starts
// the supervisor when /data/lostos/enabled exists. It knows nothing about AirPlay or any other part: lostOS starts on its own.
const initScript = `#!/bin/sh
# lostOS autostart (made by lostos-install). Remove it with: lostos-install --uninstall
case "$1" in
  start) [ -x /data/lostos/boot.sh ] && /data/lostos/boot.sh </dev/null >/dev/null 2>&1 & ;;
esac
exit 0
`

const initPath = "/etc/init.d/S98lostos"

// bootInit puts initScript into /etc/init.d (the root file system is read-only: it is remounted for the copy and set back).
func bootInit(s string) bool {
	fmt.Fprintf(out, "== %s: autostart %s\n", s, initPath)
	if shTo(out, s, "grep -q lostos/boot.sh "+initPath) == nil {
		fmt.Fprintln(out, "already present")
		return true
	}
	dir, err := os.MkdirTemp("", "lostos-init")
	if err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	defer os.RemoveAll(dir)
	tmp := filepath.Join(dir, "S98lostos")
	if err := os.WriteFile(tmp, []byte(initScript), 0o755); err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	if push(s, tmp, dataDir+"/S98lostos.new") != nil {
		return false
	}
	if _, err := sh(s, "sh -n "+dataDir+"/S98lostos.new"); err != nil {
		fmt.Fprintf(errw, "%s: the init script fails sh -n on the device, not installed\n", s)
		return false
	}
	err = shTo(out, s, "mount -o remount,rw / && cp "+dataDir+"/S98lostos.new "+initPath+" && chmod 755 "+initPath+" && sync; r=$?; mount -o remount,ro /; [ $r -eq 0 ]")
	_, _ = sh(s, "rm -f "+dataDir+"/S98lostos.new")
	if err != nil || shTo(out, s, "grep -q lostos/boot.sh "+initPath) != nil {
		fmt.Fprintf(errw, "%s: could not write %s (the root file system stayed read-only?)\n", s, initPath)
		return false
	}
	fmt.Fprintf(out, "installed %s\n", initPath)
	return true
}

// A shell script of the payload is sent with Unix line endings: a checkout or an archive made on Windows may have turned them into CRLF,
// and busybox sh does not forgive that (an "exit 0" with a CR: Illegal number). This is how boot.sh once stayed silent on a speaker.
func unixScript(local string) ([]byte, error) {
	b, err := os.ReadFile(local)
	if err != nil {
		return nil, err
	}
	return bytes.ReplaceAll(b, []byte("\r\n"), []byte("\n")), nil
}

func scriptSHA(local string) string {
	b, err := unixScript(local)
	if err != nil {
		return ""
	}
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

func pushScript(s, local, remote string) error {
	b, err := unixScript(local)
	if err != nil {
		return err
	}
	dir, err := os.MkdirTemp("", "lostos-script")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	tmp := filepath.Join(dir, filepath.Base(local))
	if err := os.WriteFile(tmp, b, 0o755); err != nil {
		return err
	}
	return push(s, tmp, remote)
}
