// lostos-install finds rooted Yandex Stations on the local network and installs or updates lostos.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

const (
	dataDir = "/data/lostos"
	rkPath  = "/data/quasar/rootkeeper.sh"
)

const usage = `lostos-install: finds rooted Yandex Stations on the local network and installs or updates lostos.

  lostos-install                      (no arguments, on a desktop) opens the wizard in the browser
  lostos-install --cli                the same steps in the terminal
  lostos-install [--list] [--scan] [--ssh] [--no-enable] [--yes] [--dry-run] [--uninstall [--purge]]
                 [--server https://host] [--forward name=192.168.x.y:port] [--receivers airplay,spotify|all|none] [ip[:port] ...]

Without addresses: finds Yandex Stations via mDNS or, with --scan, by scanning the /24
of the default-route interface for adb (port 5555); connects, identifies the model and offers
the supported, rooted stations found. Per station:
- copies the daemon + scripts to /data/lostos (scripts via .new + mv; when the binary
  differs from the installed one, the installed one is kept as .prev; an identical one is not
  copied and leaves .prev alone);
- --ssh also installs dropbear (static build from ssh/build.sh) to /data/dropbear;
- adds the boot hooks to /data/quasar/rootkeeper.sh (backup: rootkeeper.sh.bak-<timestamp>),
  placed before an endless ` + "`while`" + ` loop if the script has one, otherwise appended, and shows the
  diff against the backup;
- enables the satellite (stops the Yandex stack) unless --no-enable; an already running
  satellite is restarted with the new binary and, after ~10 s, checked (process alive, no crash
  report in the log); on failure the previous binary is put back (asked, or automatic with --yes).
In a repository checkout (satellite/Makefile present) the daemon is built first
(make; ssh/build.sh for --ssh). Nothing outside /data is modified. Re-running is safe.
adb: from PATH, else platform-tools/ next to the installer, else downloaded there
from dl.google.com (Android SDK Platform-Tools 37.0.1; Google's terms apply).
--list only shows what was found; --yes installs to every station found without asking.
--dry-run shows what would be done (model, space, files, boot hook) and changes nothing on the station.
--uninstall gives the station back to Yandex: stops lostos, removes its boot hook (backup kept; the rest of rootkeeper.sh is untouched),
keeps /data/lostos unless --purge; reboot afterwards.
--gui opens the wizard (it is the default without arguments on Windows, macOS and a Linux desktop), --no-browser only prints its address.
--server writes the server address into the station's /data/lostos/link.conf (the station then links itself to that lostOS server over HTTPS and
shows its id); --forward name=host:port lets that server reach one host of your home network through the station (Home Assistant: --forward ha=192.168.1.5:8123).
--receivers chooses which receivers go on the station: airplay (AirPlay 2), spotify (Spotify Connect), both (all) or none. Without it the installer asks for
each one (--yes: all that are in the archive for the model). They are system plugins of lostOS; run the installer again to add one later.
--doctor looks at the stations that are found (what is installed, running, linked, how much room is left, errors of the log) and changes nothing; with --fix the installation is repeated where
something is wrong with it. --catalog URL (default: the lostOS repository) is the folder of the signed catalog of speaker models: new models and their profiles come from it without a new
installer; --offline uses only what the package has (no downloads at all).
The daemon comes from GitHub Releases: the newest stable release of the lostOS repository (--repo owner/name for a fork) is used when it differs from the package, after its SHA256SUMS
match; --version v0.3.1 pins one release (an error if it can not be had), --offline keeps the package. A build of your own: use --offline.
--scan may reach other Android devices on the network, which can show an adb authorization prompt.
`

type options struct {
	ssh, noEnable, yes, list, scan, dryRun, uninstall, purge bool
	gui, cli, noBrowser                                      bool
	localServer                                              bool     // --local-server <speaker ip>: install and start the small server on this computer, and link the speaker to it
	receivers                                                []string // --receivers airplay,spotify: the system plugins to put on the station (ids)
	receiversSet                                             bool     // the owner said (also "none"): do not ask
	server                                                   string   // --server https://host: write it into the station's link.conf
	forwards                                                 []string // --forward ha=192.168.1.5:8123 (repeatable)
	targets                                                  []string
	catalog                                                  string // --catalog URL: the folder of the signed catalog of models (default: the repository of lostOS)
	offline                                                  bool   // --offline: only the catalog of the package
	version, repo                                            string // --version v0.3.1: that release of the daemon from GitHub; --repo owner/name: another repository (a fork)
	doctor, fix                                              bool   // --doctor: look at an installed station and tell what is wrong; --fix: put it right
}

var (
	out     io.Writer = os.Stdout // what the steps print: the terminal, or the log of the wizard
	errw    io.Writer = os.Stderr
	opts    options
	root    string // payload: satellite/, ssh/ (release archive or repository checkout)
	adbPath string
	stdin   = bufio.NewReader(os.Stdin)
)

func main() {
	code := run()
	pauseIfOwnConsole()
	os.Exit(code)
}

var errHelp = errors.New("help")

func parseArgs(args []string) (options, error) {
	var o options
	for i := 0; i < len(args); i++ {
		a := args[i]
		value := func(name string) (string, error) { // --name value  or  --name=value
			if strings.HasPrefix(a, name+"=") {
				return a[len(name)+1:], nil
			}
			if i+1 >= len(args) {
				return "", fmt.Errorf("%s needs a value", name)
			}
			i++
			return args[i], nil
		}
		switch {
		case a == "--gui":
			o.gui = true
		case a == "--offline":
			o.offline = true
		case a == "--doctor":
			o.doctor = true
		case a == "--fix":
			o.fix = true
		case a == "--catalog" || strings.HasPrefix(a, "--catalog="):
			v, err := value("--catalog")
			if err != nil {
				return o, err
			}
			o.catalog = strings.TrimSpace(v)
		case a == "--version" || strings.HasPrefix(a, "--version="):
			v, err := value("--version")
			if err != nil {
				return o, err
			}
			if o.version = strings.TrimSpace(v); !tagRe.MatchString(o.version) {
				return o, fmt.Errorf("--version needs a release tag like v0.3.1")
			}
		case a == "--repo" || strings.HasPrefix(a, "--repo="):
			v, err := value("--repo")
			if err != nil {
				return o, err
			}
			if o.repo = strings.TrimSpace(v); !regexp.MustCompile(`^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$`).MatchString(o.repo) {
				return o, fmt.Errorf("--repo needs owner/name")
			}
		case a == "--cli":
			o.cli = true
		case a == "--local-server":
			o.localServer = true
		case a == "--server-src" || strings.HasPrefix(a, "--server-src="):
			v, err := value("--server-src")
			if err != nil {
				return o, err
			}
			serverSrc = strings.TrimSpace(v)
		case a == "--server-repo" || strings.HasPrefix(a, "--server-repo="):
			v, err := value("--server-repo")
			if err != nil {
				return o, err
			}
			serverRepo = strings.TrimSpace(v)
		case a == "--no-browser":
			o.noBrowser = true
		case a == "--server" || strings.HasPrefix(a, "--server="):
			v, err := value("--server")
			if err != nil {
				return o, err
			}
			o.server = strings.TrimRight(strings.TrimSpace(v), "/")
		case a == "--receivers" || strings.HasPrefix(a, "--receivers="):
			v, err := value("--receivers")
			if err != nil {
				return o, err
			}
			if o.receivers, err = parseReceivers(v); err != nil {
				return o, err
			}
			o.receiversSet = true
		case a == "--forward" || strings.HasPrefix(a, "--forward="):
			v, err := value("--forward")
			if err != nil {
				return o, err
			}
			o.forwards = append(o.forwards, strings.TrimSpace(v))
		case a == "--ssh":
			o.ssh = true
		case a == "--no-enable":
			o.noEnable = true
		case a == "-y" || a == "--yes":
			o.yes = true
		case a == "--list":
			o.list = true
		case a == "--dry-run":
			o.dryRun = true
		case a == "--uninstall":
			o.uninstall = true
		case a == "--purge":
			o.purge = true
		case a == "--scan":
			o.scan = true
		case a == "-h" || a == "--help":
			return o, errHelp
		case strings.HasPrefix(a, "-"):
			return o, fmt.Errorf("unknown option %s", a)
		case strings.Contains(a, ":"):
			o.targets = append(o.targets, a)
		default:
			o.targets = append(o.targets, a+":5555")
		}
	}
	return o, nil
}

// findRoot locates the payload: next to the executable (release archive), else the working
// directory or one of its parents (repository checkout, `go -C installer run .`).
func findRoot() (string, error) {
	has := func(d string) bool {
		_, err := os.Stat(filepath.Join(d, "satellite", "deploy", "boot.sh"))
		return err == nil
	}
	if exe, err := os.Executable(); err == nil {
		if exe, err := filepath.EvalSymlinks(exe); err == nil && has(filepath.Dir(exe)) {
			return filepath.Dir(exe), nil
		}
	}
	if d, err := os.Getwd(); err == nil {
		for {
			if has(d) {
				return d, nil
			}
			p := filepath.Dir(d)
			if p == d {
				break
			}
			d = p
		}
	}
	return "", errors.New("payload not found: run lostos-install from the unpacked release archive or inside the repository")
}

// readLine prints prompt on stderr and returns the trimmed answer ("" on EOF).
func readLine(prompt string) string {
	fmt.Fprint(os.Stderr, prompt)
	l, err := stdin.ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return ""
	}
	return strings.TrimSpace(l)
}

func run() int {
	var err error
	if opts, err = parseArgs(os.Args[1:]); err != nil {
		if err == errHelp {
			fmt.Print(usage)
			return 0
		}
		fmt.Fprintln(errw, err)
		return 1
	}
	if root, err = findRoot(); err != nil {
		fmt.Fprintln(errw, err)
		return 1
	}
	if opts.catalog != "" {
		catalogBase = strings.TrimRight(opts.catalog, "/")
	}
	if opts.repo != "" {
		releaseRepo = opts.repo
	}
	loadCatalog(root, opts.offline)
	if opts.localServer { // the server of this computer, then the speakers get its address as --server
		if len(opts.targets) == 0 {
			fmt.Fprintln(errw, "--local-server needs the address of a speaker (192.168.x.y): it tells which address of this computer the speaker sees")
			return 1
		}
		u, setup, ok := setupLocalServer(opts.targets[0])
		if !ok {
			return 1
		}
		opts.server = u
		if setup != "" {
			fmt.Fprintf(out, "первый вход в панель (один раз, 24 часа): %s\n", setup)
		}
	}
	if opts.server != "" && !opts.list && !opts.uninstall && !opts.doctor && !opts.dryRun && !confirmServer(opts.server) {
		fmt.Fprintln(errw, "отменено: сервер не подтверждён")
		return 1
	}
	if opts.gui || (len(os.Args) == 1 && !opts.cli && desktopAvailable()) {
		return runGUI()
	}
	if !findAdb() || !startServer() {
		return 1
	}
	if !opts.list && !opts.uninstall && !opts.dryRun && (!opts.doctor || opts.fix) && !build() {
		return 1
	}

	fmt.Fprintln(out, "== looking for stations")
	seen := map[string]bool{}
	targets := opts.targets
	if len(targets) == 0 {
		// Already connected network devices first, then mDNS (or the port scan).
		for _, s := range networkDevices() {
			seen[s] = true
			probe(s, false)
		}
		if opts.scan {
			targets = scan()
		} else {
			targets = mdns()
		}
	}
	for _, s := range targets {
		if seen[s] {
			continue
		}
		seen[s] = true
		ours := false
		if !isConnected(s) {
			connect(s)
			ours = true
		}
		probe(s, ours)
	}
	if len(found) == 0 {
		fmt.Fprintln(out, "no supported rooted station found")
		return 1
	}
	if opts.list {
		return 0
	}
	if opts.doctor {
		return runDoctor(found)
	}

	var chosen []station
	if opts.yes {
		chosen = found
	} else {
		ans := readLine("install to which? (numbers separated by spaces, a = all, Enter = cancel): ")
		if ans == "a" {
			chosen = found
		} else {
			for _, n := range strings.Fields(ans) {
				i, err := strconv.Atoi(n)
				if err != nil || n[0] < '0' || n[0] > '9' || i < 1 || i > len(found) {
					fmt.Fprintf(errw, "bad choice: %s\n", n)
					return 1
				}
				chosen = append(chosen, found[i-1])
			}
		}
	}
	if len(chosen) == 0 {
		fmt.Fprintln(out, "nothing to do")
		return 0
	}

	failed := 0
	for _, st := range chosen {
		var ok bool
		if opts.uninstall {
			ok = uninstallOne(st)
		} else {
			linkFollows = opts.server != ""
			ok = installOne(st)
			if ok && opts.server != "" && !opts.dryRun {
				ok = configureLink(st.serial, opts.server, opts.forwards)
			}
		}
		if !ok {
			failed = 1
		}
	}
	return failed
}
