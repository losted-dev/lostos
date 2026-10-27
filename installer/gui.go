package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strings"
	"sync"
	"time"
)

// The wizard: a small web server on 127.0.0.1 (random port) that the installer opens in the browser. It drives the very same steps as the
// terminal version (installOne, configureLink ...); their output goes to the log that the page shows. Every call needs the token that only
// the page we served knows, and the Host header must be our own address (another site, even one that guesses the port, cannot use it).

//go:embed web/*
var webFS embed.FS

type guiServer struct {
	token string
	addr  string

	mu       sync.Mutex // one thing at a time: a scan, an install ...
	busy     bool
	lines    []string
	done     bool
	ok       bool
	result   map[string]any
	lastSeen time.Time
	quit     chan struct{}
}

// logWriter turns what the steps print into lines of the log.
type logWriter struct{ g *guiServer }

func (w logWriter) Write(p []byte) (int, error) {
	w.g.mu.Lock()
	defer w.g.mu.Unlock()
	for _, l := range strings.Split(strings.TrimRight(string(p), "\r\n"), "\n") {
		if l = strings.TrimRight(l, "\r"); l != "" {
			w.g.lines = append(w.g.lines, l)
		}
	}
	return len(p), nil
}

func runGUI() int {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintln(errw, err)
		return 1
	}
	tok := make([]byte, 16)
	_, _ = rand.Read(tok)
	g := &guiServer{token: hex.EncodeToString(tok), addr: ln.Addr().String(), lastSeen: time.Now(), quit: make(chan struct{})}
	mux := http.NewServeMux()
	sub, _ := fs.Sub(webFS, "web")
	static := http.FileServer(http.FS(sub))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.Host != g.addr {
			http.Error(w, "bad host", http.StatusForbidden)
			return
		}
		if r.URL.Path != "/" {
			static.ServeHTTP(w, r)
			return
		}
		page, _ := fs.ReadFile(sub, "index.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'")
		_, _ = w.Write([]byte(strings.Replace(string(page), "__TOKEN__", g.token, 1)))
	})
	api := func(path string, h func(r *http.Request) (any, int)) {
		mux.HandleFunc("/api/"+path, func(w http.ResponseWriter, r *http.Request) {
			if r.Host != g.addr || r.Header.Get("X-Token") != g.token {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			g.mu.Lock()
			g.lastSeen = time.Now()
			g.mu.Unlock()
			v, code := h(r)
			w.Header().Set("Content-Type", "application/json; charset=utf-8")
			w.WriteHeader(code)
			_ = json.NewEncoder(w).Encode(v)
		})
	}
	body := func(r *http.Request, v any) bool {
		return json.NewDecoder(http.MaxBytesReader(nil, r.Body, 1<<16)).Decode(v) == nil
	}

	api("env", func(r *http.Request) (any, int) {
		_, ours := os.Stat(root + "/satellite/deploy/boot.sh")
		return map[string]any{"adb": adbReady(), "payload": ours == nil, "os": runtime.GOOS, "version": versionOf()}, 200
	})
	api("adb", func(r *http.Request) (any, int) { // finds adb, downloads Google's platform-tools when asked to
		return g.run(func() (bool, map[string]any) { return findAdb() && startServer(), nil }), 200
	})
	api("scan", func(r *http.Request) (any, int) {
		var req struct {
			IPs  []string `json:"ips"`
			Deep bool     `json:"deep"`
		}
		body(r, &req)
		return g.run(func() (bool, map[string]any) {
			found, probed = nil, nil
			seen := map[string]bool{}
			for _, s := range networkDevices() {
				seen[s] = true
				probe(s, false)
			}
			targets := req.IPs
			for i, t := range targets {
				if !strings.Contains(t, ":") {
					targets[i] = t + ":5555"
				}
			}
			if len(targets) == 0 {
				if req.Deep {
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
			return true, map[string]any{"stations": probed}
		}), 200
	})
	api("server", func(r *http.Request) (any, int) {
		var req struct {
			URL string `json:"url"`
		}
		body(r, &req)
		info, err := checkServer(req.URL)
		if err != nil {
			return map[string]any{"ok": false, "error": err.Error()}, 200
		}
		return map[string]any{"ok": true, "server": info}, 200
	})
	api("install", func(r *http.Request) (any, int) {
		var req struct {
			Serial   string   `json:"serial"`
			Type     string   `json:"type"` // url | local | speaker
			URL      string   `json:"url"`
			Forwards []string `json:"forwards"`
			// the receivers the owner ticked (ids of the system plugins); an empty list means none
			Receivers []string `json:"receivers"`
		}
		body(r, &req)
		var st *station
		for i := range found {
			if found[i].serial == req.Serial {
				st = &found[i]
			}
		}
		if st == nil {
			return map[string]any{"ok": false, "error": "станция не выбрана или её нет в списке: найдите колонки заново"}, 200
		}
		if req.Type == "speaker" {
			return map[string]any{"ok": false, "error": "сервер в колонке пока не готов"}, 200
		}
		return g.start(func() (bool, map[string]any) {
			opts = options{yes: true, receivers: req.Receivers, receiversSet: true}
			res := map[string]any{}
			if req.Type == "local" { // the server of this computer first: the speaker is told its address
				u, setup, ok := setupLocalServer(st.serial)
				if !ok {
					return false, nil
				}
				req.URL, res["setup"] = u, setup
			}
			linkFollows = req.Type == "url" || req.Type == "local"
			if !installOne(*st) {
				return false, nil
			}
			if req.Type == "url" || req.Type == "local" {
				if !configureLink(st.serial, req.URL, req.Forwards) {
					return false, nil
				}
				id, state := readLinkState(st.serial)
				res["id"], res["state"], res["server"] = id, state, req.URL
			}
			return true, res
		}), 200
	})
	api("job", func(r *http.Request) (any, int) {
		var req struct {
			From int `json:"from"`
		}
		body(r, &req)
		g.mu.Lock()
		defer g.mu.Unlock()
		from := req.From
		if from < 0 || from > len(g.lines) {
			from = 0
		}
		return map[string]any{"lines": append([]string{}, g.lines[from:]...), "next": len(g.lines), "busy": g.busy, "done": g.done, "ok": g.ok, "result": g.result}, 200
	})
	api("link", func(r *http.Request) (any, int) {
		var req struct {
			Serial string `json:"serial"`
		}
		body(r, &req)
		id, state := readLinkState(req.Serial)
		return map[string]any{"id": id, "state": state}, 200
	})
	api("open", func(r *http.Request) (any, int) {
		var req struct {
			URL string `json:"url"`
		}
		body(r, &req)
		if u, err := url.Parse(req.URL); err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "" {
			return map[string]any{"ok": false}, 400
		}
		openBrowser(req.URL)
		return map[string]any{"ok": true}, 200
	})
	api("ping", func(r *http.Request) (any, int) { return map[string]any{"ok": true}, 200 })
	api("quit", func(r *http.Request) (any, int) {
		go func() { time.Sleep(200 * time.Millisecond); close(g.quit) }()
		return map[string]any{"ok": true}, 200
	})

	srv := &http.Server{Handler: mux}
	go func() { _ = srv.Serve(ln) }()
	pageURL := "http://" + g.addr + "/"
	fmt.Fprintf(os.Stdout, "lostOS: мастер установки открыт по адресу %s\nЗакройте это окно или нажмите в мастере «Закрыть», когда закончите.\n", pageURL)
	if !opts.noBrowser {
		openBrowser(pageURL)
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)
	tick := time.NewTicker(10 * time.Second)
	for {
		select {
		case <-g.quit:
			return 0
		case <-sig:
			return 0
		case <-tick.C:
			g.mu.Lock()
			idle := time.Since(g.lastSeen) > 15*time.Minute && !g.busy
			g.mu.Unlock()
			if idle {
				return 0
			}
		}
	}
}

// run does a short job in the request (a scan, adb) and returns its outcome with the log; start begins a long one (an install) and returns at once.
func (g *guiServer) run(fn func() (bool, map[string]any)) map[string]any {
	if !g.begin() {
		return map[string]any{"ok": false, "error": "уже идёт другая операция"}
	}
	ok, res := fn()
	g.finish(ok, res)
	g.mu.Lock()
	defer g.mu.Unlock()
	res2 := map[string]any{"ok": ok, "log": append([]string(nil), g.lines...)}
	for k, v := range res {
		res2[k] = v
	}
	return res2
}

func (g *guiServer) start(fn func() (bool, map[string]any)) map[string]any {
	if !g.begin() {
		return map[string]any{"ok": false, "error": "уже идёт другая операция"}
	}
	go func() {
		ok, res := fn()
		g.finish(ok, res)
	}()
	return map[string]any{"ok": true}
}

func (g *guiServer) begin() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.busy {
		return false
	}
	g.busy, g.done, g.ok, g.result, g.lines = true, false, false, nil, nil
	out, errw = logWriter{g}, logWriter{g}
	return true
}

func (g *guiServer) finish(ok bool, res map[string]any) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.busy, g.done, g.ok, g.result = false, true, ok, res
	out, errw = os.Stdout, os.Stderr
}

func adbReady() bool {
	if adbPath != "" {
		return true
	}
	return findAdbQuiet()
}

func versionOf() string {
	b, err := os.ReadFile(root + "/VERSION")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func openBrowser(u string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", u)
	case "darwin":
		cmd = exec.Command("open", u)
	default:
		cmd = exec.Command("xdg-open", u)
	}
	_ = cmd.Start()
}
