package main

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"time"
)

// serverInfo is what a lostOS server says about itself at /api/v1/hello.
type serverInfo struct {
	URL          string `json:"url"`
	Version      string `json:"version"`
	Lite         bool   `json:"lite"`
	Secure       bool   `json:"secure"` // a station can link itself to it: https, or http inside the home network (Plain)
	Plain        bool   `json:"plain"`  // http inside the home network: no encryption, anybody on that network can read the link
	Registration string `json:"registration"`
	Tagline      string `json:"tagline"` // the line under the name that the server gives itself: the wizard shows it
	Known        bool   `json:"known"`   // the server is on the signed list of the servers that the lostOS team knows (servers.go)
	KnownName    string `json:"known_name"`
	List         string `json:"list"`   // "ok" | "unavailable" | "expired": whether the list could say anything
	Banned       bool   `json:"banned"` // the server is on the list of the banned: the wizard refuses it
	BannedWhy    string `json:"banned_why"`
}

// checkServer asks the address (a bare host, host:port or a full URL) whether a lostOS server answers there, trying https first.
func checkServer(raw string) (serverInfo, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return serverInfo{}, errors.New("введите адрес сервера")
	}
	var bases []string
	if strings.Contains(raw, "://") {
		bases = []string{strings.TrimRight(raw, "/")}
	} else {
		h := strings.TrimRight(raw, "/")
		bases = []string{"https://" + h}
	}
	var last error
	for _, b := range bases {
		info, err := hello(b)
		if err == nil {
			v := lookupServer(info.URL)
			info.KnownName, info.Known, info.List, info.Banned, info.BannedWhy = v.Name, v.Known, v.State, v.Banned, v.Why
			return info, nil
		}
		last = err
	}
	return serverInfo{}, last
}

var localRoots sync.Map

func serverClient(base string) (*http.Client, error) {
	u, err := url.Parse(base)
	if err != nil || u.Scheme != "https" || u.Host == "" || u.User != nil {
		return nil, errors.New("сервер должен использовать HTTPS")
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if path, ok := localRoots.Load(base); ok {
		pem, err := os.ReadFile(path.(string))
		if err != nil {
			return nil, err
		}
		roots := x509.NewCertPool()
		if !roots.AppendCertsFromPEM(pem) {
			return nil, errors.New("неверный сертификат локального сервера")
		}
		transport.TLSClientConfig = &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12}
	}
	return &http.Client{Timeout: 6 * time.Second, Transport: transport,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= 10 || req.URL.Scheme != "https" || req.URL.Host != u.Host {
				return errors.New("небезопасное перенаправление сервера")
			}
			return nil
		}}, nil
}

func hello(base string) (serverInfo, error) {
	c, err := serverClient(base)
	if err != nil {
		return serverInfo{}, err
	}
	resp, err := c.Get(base + "/api/v1/hello")
	if err != nil {
		return serverInfo{}, fmt.Errorf("нет ответа от %s", base)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return serverInfo{}, fmt.Errorf("%s отвечает, но это не сервер lostOS (код %d)", base, resp.StatusCode)
	}
	var h struct {
		Lostos       string `json:"lostos"`
		Version      string `json:"version"`
		Lite         bool   `json:"lite"`
		Registration string `json:"registration"`
		Tagline      string `json:"tagline"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&h); err != nil || h.Lostos != "server" {
		return serverInfo{}, fmt.Errorf("%s отвечает, но это не сервер lostOS", base)
	}
	return serverInfo{URL: base, Version: h.Version, Lite: h.Lite, Secure: true, Plain: false, Registration: h.Registration, Tagline: strings.TrimSpace(h.Tagline)}, nil
}

// homeURL: an http:// address that points inside the home network (a private IPv4 address, localhost, a name .local). The firmware accepts plain http for these and for nothing else.
func homeURL(raw string) bool {
	if !strings.HasPrefix(strings.TrimSpace(raw), "http://") {
		return false
	}
	h := hostOf(raw)
	if h == "localhost" || strings.HasSuffix(h, ".local") {
		return true
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.To4() != nil && (ip.IsPrivate() || ip.IsLoopback())
}

var forwardRe = regexp.MustCompile(`^[A-Za-z0-9]{1,16}=\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d{1,5}$`)

// configureLink writes /data/lostos/link.conf on the station (server=… and the forwards the owner asked for), restarts lostos and
// waits until the station has a link id. The firmware only talks to https servers and only forwards private IPv4 addresses.
func configureLink(s, serverURL string, forwards []string) bool {
	serverURL = strings.TrimRight(strings.TrimSpace(serverURL), "/")
	if _, err := serverClient(serverURL); err != nil {
		fmt.Fprintf(errw, "%s: станция подключается к серверу только по HTTPS (адрес %q не подходит)\n", s, serverURL)
		return false
	}
	if len(forwards) == 0 { // nothing new asked: the forwards that the owner set up before stay
		if old, err := sh(s, "cat "+dataDir+"/link.conf 2>/dev/null"); err == nil {
			for _, l := range strings.Split(old, "\n") {
				if l = strings.TrimSpace(l); strings.HasPrefix(l, "forward=") {
					forwards = append(forwards, strings.TrimPrefix(l, "forward="))
				}
			}
		}
	}
	var b strings.Builder
	b.WriteString("server=" + serverURL + "\n")
	for _, f := range forwards {
		if f = strings.TrimSpace(f); f == "" {
			continue
		}
		if !forwardRe.MatchString(f) {
			fmt.Fprintf(errw, "%s: пробрасываемый адрес %q не похож на имя=192.168.x.y:порт\n", s, f)
			return false
		}
		b.WriteString("forward=" + f + "\n")
	}
	dir, err := os.MkdirTemp("", "lostos-link")
	if err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	defer os.RemoveAll(dir)
	tmp := filepath.Join(dir, "link.conf")
	if err := os.WriteFile(tmp, []byte(b.String()), 0o600); err != nil {
		fmt.Fprintln(errw, err)
		return false
	}
	fmt.Fprintf(out, "== %s: связь с сервером %s\n", s, serverURL)
	if ca, ok := localRoots.Load(serverURL); ok {
		if push(s, ca.(string), dataDir+"/link-ca.crt.new") != nil ||
			shTo(out, s, "mv "+dataDir+"/link-ca.crt.new "+dataDir+"/link-ca.crt && chmod 600 "+dataDir+"/link-ca.crt") != nil {
			return false
		}
	} else if shTo(out, s, "rm -f "+dataDir+"/link-ca.crt") != nil {
		return false
	}
	if push(s, tmp, dataDir+"/link.conf.new") != nil ||
		shTo(out, s, "mv "+dataDir+"/link.conf.new "+dataDir+"/link.conf && chmod 600 "+dataDir+"/link.conf && sync") != nil {
		return false
	}
	_ = shTo(out, s, "rm -f /tmp/lostos-boot-done; "+dataDir+"/ctl.sh restart") // the last start of the installation plays the boot show again (see installOne)
	for i := 0; i < 30; i++ {
		time.Sleep(time.Second)
		if id, st := readLinkState(s); id != "" && st != "" {
			fmt.Fprintf(out, "идентификатор станции: %s (%s)\n", id, st)
			return true
		}
	}
	fmt.Fprintf(errw, "%s: станция не назвала идентификатор за 30 секунд, посмотрите лог: adb shell tail -n 40 /tmp/lostos.log\n", s)
	return false
}

// readLinkState reads the station id and what the link did last: "connecting", "pending" (waits for the owner), "confirmed", "offline", "off".
func readLinkState(s string) (id, state string) {
	if _, err := sh(s, "[ -s "+dataDir+"/link.conf ]"); err != nil {
		return "", "off"
	}
	idRaw, _ := sh(s, "cat "+dataDir+"/link.id 2>/dev/null")
	id = strings.TrimSpace(idRaw)
	last, _ := sh(s, "grep 'link:' /tmp/lostos.log | tail -n 1")
	switch {
	case strings.Contains(last, "confirmed by the owner"):
		state = "confirmed"
	case strings.Contains(last, "waits for the owner"):
		state = "pending"
	case strings.Contains(last, "connection ended"):
		state = "offline"
	case id != "":
		state = "connecting"
	}
	return id, state
}

// desktopAvailable: a graphical desktop where a browser can open (Windows and macOS always, Linux with a display).
func desktopAvailable() bool {
	switch runtime.GOOS {
	case "windows", "darwin":
		return true
	}
	return os.Getenv("DISPLAY") != "" || os.Getenv("WAYLAND_DISPLAY") != ""
}
