package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"
)

// adbExe is the name of the adb program on this system.
func adbExe() string {
	if runtime.GOOS == "windows" {
		return "adb.exe"
	}
	return "adb"
}

// findAdbQuiet sets adbPath to the adb that is there already (from PATH, else platform-tools/ next to the payload) and downloads nothing.
func findAdbQuiet() bool {
	if p, err := exec.LookPath("adb"); err == nil {
		adbPath = p
		return true
	}
	local := filepath.Join(root, "platform-tools", adbExe())
	if _, err := os.Stat(local); err == nil {
		adbPath = local
		return true
	}
	return false
}

// findAdb sets adbPath: adb from PATH, else platform-tools/ next to the payload, else downloaded there.
func findAdb() bool {
	if findAdbQuiet() {
		return true
	}
	dest := filepath.Join(root, "platform-tools")
	local := filepath.Join(dest, adbExe())
	pkg, ok := ptPackages[runtime.GOOS]
	if !ok || (runtime.GOOS == "linux" && runtime.GOARCH != "amd64") {
		fmt.Fprintln(errw, "adb not found (Android platform-tools): install it, e.g. sudo apt install adb")
		return false
	}
	url := ptBase + pkg.file
	fmt.Fprintf(errw, "adb not found: downloading Android SDK Platform-Tools 37.0.1 from %s (terms: https://developer.android.com/studio/terms)\n", url)
	if err := fetchPlatformTools(url, pkg.size, pkg.sha1, dest); err != nil {
		fmt.Fprintf(errw, "platform-tools: %v; install adb yourself (Android platform-tools)\n", err)
		return false
	}
	adbPath = local
	return true
}

// startServer starts the adb server with our stderr, not a pipe: a server spawned by a captured
// adb call would inherit the pipe and keep it open (blocking that call on Windows).
func startServer() bool {
	cmd := exec.Command(adbPath, "start-server")
	cmd.Stdout, cmd.Stderr = os.Stderr, os.Stderr
	if err := cmd.Run(); err != nil {
		fmt.Fprintln(errw, "adb start-server failed")
		return false
	}
	return true
}

func adbCmd(args ...string) *exec.Cmd { return exec.Command(adbPath, args...) }

func push(s, local, remote string) error {
	c := adbCmd("-s", s, "push", local, remote)
	c.Stderr = errw // the log of the wizard shows what adb says, as the terminal does
	return c.Run()
}

func pull(s, remote, local string) error {
	c := adbCmd("-s", s, "pull", remote, local)
	c.Stderr = errw
	return c.Run()
}

// connect: adb connect, then waits until the device is really there. Right after the adb server was started (the first run on a computer) a speaker stays "offline" for a second or two,
// and the first command used to fail at once with "unauthorized or offline". -> the state that was seen last: "device", "offline", "unauthorized" or "" (not found).
func connect(s string) string {
	_ = adbCmd("connect", s).Run()
	state := ""
	for i := 0; i < 16; i++ {
		state = adbState(s)
		if state == "device" || state == "unauthorized" {
			return state
		}
		if i == 6 && state == "" { // still not found after 3 s: connect once more
			_ = adbCmd("connect", s).Run()
		}
		time.Sleep(500 * time.Millisecond)
	}
	return state
}

// adbState is what adb says about the device: "device", "offline", "unauthorized" or "" (anything else, for example not found).
func adbState(s string) string {
	b, _ := adbCmd("-s", s, "get-state").CombinedOutput()
	t := strings.ToLower(strings.TrimSpace(string(b)))
	switch {
	case t == "device":
		return "device"
	case strings.Contains(t, "unauthorized"):
		return "unauthorized"
	case strings.Contains(t, "offline"):
		return "offline"
	}
	return ""
}
func disconnect(s string) { _ = adbCmd("disconnect", s).Run() }

var networkSerial = regexp.MustCompile(`:[0-9]+$`)

// devices returns the serials adb lists in state `device`.
func devices() []string {
	b, _ := adbCmd("devices").Output()
	var r []string
	for i, l := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		if f := strings.Fields(l); i > 0 && len(f) >= 2 && f[1] == "device" {
			r = append(r, f[0])
		}
	}
	return r
}

func networkDevices() []string {
	var r []string
	for _, s := range devices() {
		if networkSerial.MatchString(s) {
			r = append(r, s)
		}
	}
	return r
}

func isConnected(s string) bool {
	for _, d := range devices() {
		if d == s {
			return true
		}
	}
	return false
}

var errNoStatus = errors.New("no exit status")

// shRaw runs cmd on the device and returns its output (newlines as sent) and an error for adb
// failures or a non-zero remote status. adb shell on these firmwares always exits 0 and adds \r:
// the remote status is carried in the output.
func shRaw(serial, cmd string) (string, error) {
	b, err := adbCmd("-s", serial, "shell", cmd+"\necho __rc=$?").Output()
	if err != nil {
		return "", err
	}
	o := strings.TrimRight(strings.ReplaceAll(string(b), "\r", ""), "\n")
	i := strings.LastIndex(o, "__rc=")
	if i < 0 {
		return "", errNoStatus
	}
	out, rc := o[:i], o[i+len("__rc="):]
	if rc != "0" {
		return out, fmt.Errorf("exit %s", rc)
	}
	return out, nil
}

// sh is shRaw with trailing newlines removed (for captured output).
func sh(serial, cmd string) (string, error) {
	out, err := shRaw(serial, cmd)
	return strings.TrimRight(out, "\n"), err
}

// shTo runs cmd and copies its output to w.
func shTo(w io.Writer, serial, cmd string) error {
	out, err := shRaw(serial, cmd)
	io.WriteString(w, out)
	return err
}

// firstField is the text up to the first space (sha256sum output → digest).
func firstField(s string) string {
	f, _, _ := strings.Cut(s, " ")
	return f
}
