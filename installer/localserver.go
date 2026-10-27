package main

// The small lostOS server on THIS computer: the installer finds Python, takes the code of the server (the repository lostos-server, or a folder/zip given with --server-src),
// makes a venv, installs the requirements, writes .env, sets up the start at login, starts it and waits until it answers. The speaker then links itself to it over plain http
// inside the home network (the firmware accepts http only for a private address). Windows, Linux and macOS; nothing here needs administrator rights.

import (
	"archive/zip"
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

var (
	serverRepo = "losted-dev/lostos-server" // --server-repo owner/name
	serverSrc  string                       // --server-src: a folder (the `server` folder of the repository or its parent) or a .zip, instead of GitHub
)

var localPort = 8765 // the first port tried; the one an earlier install wrote into .env stays

// pickPort: the port of an earlier install (TTS_PORT in its .env), else the first free port from localPort up (something else may sit on 8765).
func pickPort(envText string) int {
	for _, l := range strings.Split(envText, "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(l), "="); ok && k == "TTS_PORT" {
			var n int
			if _, err := fmt.Sscan(v, &n); err == nil && n > 0 && n < 65536 {
				return n
			}
		}
	}
	for p := localPort; p < localPort+20; p++ {
		if ln, err := net.Listen("tcp", fmt.Sprintf("0.0.0.0:%d", p)); err == nil {
			ln.Close()
			return p
		}
	}
	return localPort
}

// localDir is where the server lives: code in app/, the venv, the data (accounts, keys of the vault) in data/. Updating replaces app/ and keeps data/ and .env.
func localDir() string {
	if runtime.GOOS == "windows" {
		if d := os.Getenv("LOCALAPPDATA"); d != "" {
			return filepath.Join(d, "lostos-server")
		}
	}
	h, _ := os.UserHomeDir()
	return filepath.Join(h, ".lostos-server")
}

// findPython returns the interpreter (3.11 or newer) and what to tell the owner when there is none.
func findPython() (string, error) {
	cands := [][]string{{"python3"}, {"python"}}
	if runtime.GOOS == "windows" {
		cands = [][]string{{"py", "-3"}, {"python"}, {"python3"}}
	}
	for _, c := range cands {
		args := append(append([]string(nil), c[1:]...), "-c", "import sys;print(sys.executable if sys.version_info>=(3,11) else '')")
		o, err := exec.Command(c[0], args...).Output()
		if p := strings.TrimSpace(string(o)); err == nil && p != "" {
			return p, nil
		}
	}
	return "", errors.New("не найден Python 3.11 или новее: поставьте его с python.org (на Windows отметьте «Add python.exe to PATH») и запустите установщик снова")
}

// lanIPFor is the address of this computer that the speaker sees (no packet is sent: a UDP "connect" only picks the route).
func lanIPFor(speaker string) string {
	host := speaker
	if h, _, err := net.SplitHostPort(speaker); err == nil {
		host = h
	}
	c, err := net.Dial("udp", net.JoinHostPort(host, "6053"))
	if err != nil {
		return ""
	}
	defer c.Close()
	if a, ok := c.LocalAddr().(*net.UDPAddr); ok {
		return a.IP.String()
	}
	return ""
}

func serverVenvPython(dir string) string {
	if runtime.GOOS == "windows" {
		return filepath.Join(dir, "venv", "Scripts", "python.exe")
	}
	return filepath.Join(dir, "venv", "bin", "python")
}

// ---- the code of the server

func fetchServerCode(app string) error {
	if serverSrc != "" {
		st, err := os.Stat(serverSrc)
		if err != nil {
			return err
		}
		if st.IsDir() {
			src := serverSrc
			if _, err := os.Stat(filepath.Join(src, "voice_server.py")); err != nil {
				src = filepath.Join(src, "server")
			}
			return copyTree(src, app)
		}
		return unzipServer(serverSrc, app)
	}
	url := "https://github.com/" + serverRepo + "/archive/refs/heads/main.zip"
	fmt.Fprintf(out, "скачиваю сервер: %s\n", url)
	c := &http.Client{Timeout: 5 * time.Minute}
	resp, err := c.Get(url)
	if err != nil {
		return fmt.Errorf("не получилось скачать сервер: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("не получилось скачать сервер: %s ответил %d (репозиторий должен быть публичным)", url, resp.StatusCode)
	}
	tmp, err := os.CreateTemp("", "lostos-server-*.zip")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	if _, err := io.Copy(tmp, io.LimitReader(resp.Body, 200<<20)); err != nil {
		tmp.Close()
		return err
	}
	tmp.Close()
	return unzipServer(tmp.Name(), app)
}

// unzipServer takes the files under <anything>/server/ of the archive of the repository (or the root of an archive that is the `server` folder itself) into app/.
func unzipServer(zipPath, app string) error {
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer zr.Close()
	prefix := ""
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, "/server/voice_server.py") || f.Name == "server/voice_server.py" {
			prefix = strings.TrimSuffix(f.Name, "voice_server.py")
			break
		}
		if f.Name == "voice_server.py" {
			break
		}
	}
	if prefix == "" {
		found := false
		for _, f := range zr.File {
			if f.Name == "voice_server.py" {
				found = true
			}
		}
		if !found {
			return errors.New("в архиве нет сервера (voice_server.py)")
		}
	}
	base, _ := filepath.Abs(app)
	var total int64
	for _, f := range zr.File {
		if !strings.HasPrefix(f.Name, prefix) || strings.HasSuffix(f.Name, "/") {
			continue
		}
		rel := strings.TrimPrefix(f.Name, prefix)
		dst := filepath.Join(base, filepath.FromSlash(rel))
		if dst != base && !strings.HasPrefix(dst, base+string(os.PathSeparator)) { // a path that leaves the folder
			return fmt.Errorf("недопустимый путь в архиве: %s", f.Name)
		}
		if total += int64(f.UncompressedSize64); total > 400<<20 {
			return errors.New("архив слишком большой")
		}
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		w, err := os.Create(dst)
		if err != nil {
			rc.Close()
			return err
		}
		_, err = io.Copy(w, rc)
		rc.Close()
		w.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

func copyTree(src, dst string) error {
	return filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		if info.IsDir() {
			if n := info.Name(); n == "__pycache__" || n == ".git" || n == "data" {
				return filepath.SkipDir
			}
			return os.MkdirAll(filepath.Join(dst, rel), 0o755)
		}
		if n := info.Name(); n == ".env" || strings.HasSuffix(n, ".pyc") || strings.HasPrefix(n, "tests_") {
			return nil
		}
		r, err := os.Open(p)
		if err != nil {
			return err
		}
		defer r.Close()
		w, err := os.Create(filepath.Join(dst, rel))
		if err != nil {
			return err
		}
		defer w.Close()
		_, err = io.Copy(w, r)
		return err
	})
}

// ---- .env: our keys are set, every other line of the owner stays

func mergeEnv(old string, set map[string]string) string {
	lines := []string{}
	done := map[string]bool{}
	sc := bufio.NewScanner(strings.NewReader(old))
	for sc.Scan() {
		l := strings.TrimRight(sc.Text(), "\r")
		if k, _, ok := strings.Cut(l, "="); ok && !strings.HasPrefix(strings.TrimSpace(l), "#") {
			if v, mine := set[strings.TrimSpace(k)]; mine {
				if !done[strings.TrimSpace(k)] {
					lines = append(lines, strings.TrimSpace(k)+"="+v)
					done[strings.TrimSpace(k)] = true
				}
				continue
			}
		}
		lines = append(lines, l)
	}
	keys := make([]string, 0, len(set))
	for k := range set {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if !done[k] {
			lines = append(lines, k+"="+set[k])
		}
	}
	return strings.TrimRight(strings.Join(lines, "\n"), "\n") + "\n"
}

// ---- start at login

func autostartAndStart(dir string) error {
	app := filepath.Join(dir, "app")
	py := serverVenvPython(dir)
	logf := filepath.Join(dir, "server.log")
	switch runtime.GOOS {
	case "windows":
		cmd := filepath.Join(dir, "run.cmd")
		vbs := filepath.Join(dir, "run.vbs")
		if err := os.WriteFile(cmd, []byte("@echo off\r\ncd /d \""+app+"\"\r\n\""+py+"\" -u voice_server.py >> \""+logf+"\" 2>&1\r\n"), 0o644); err != nil {
			return err
		}
		if err := os.WriteFile(vbs, []byte("CreateObject(\"WScript.Shell\").Run \"\"\""+cmd+"\"\"\", 0, False\r\n"), 0o644); err != nil {
			return err
		}
		if ad := os.Getenv("APPDATA"); ad != "" { // the Startup folder of the user: no administrator rights needed
			st := filepath.Join(ad, "Microsoft", "Windows", "Start Menu", "Programs", "Startup")
			if err := os.WriteFile(filepath.Join(st, "lostOS-server.vbs"), []byte("CreateObject(\"WScript.Shell\").Run \"\"\""+cmd+"\"\"\", 0, False\r\n"), 0o644); err != nil {
				fmt.Fprintf(errw, "автозапуск не настроен: %v\n", err)
			}
		}
		return exec.Command("wscript.exe", vbs).Start()
	case "darwin":
		h, _ := os.UserHomeDir()
		plist := filepath.Join(h, "Library", "LaunchAgents", "dev.lostos.server.plist")
		body := `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>dev.lostos.server</string>
<key>ProgramArguments</key><array><string>` + py + `</string><string>-u</string><string>voice_server.py</string></array>
<key>WorkingDirectory</key><string>` + app + `</string>
<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
<key>StandardOutPath</key><string>` + logf + `</string><key>StandardErrorPath</key><string>` + logf + `</string>
</dict></plist>
`
		if err := os.MkdirAll(filepath.Dir(plist), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(plist, []byte(body), 0o644); err != nil {
			return err
		}
		_ = exec.Command("launchctl", "unload", plist).Run()
		return exec.Command("launchctl", "load", "-w", plist).Run()
	default:
		h, _ := os.UserHomeDir()
		unit := filepath.Join(h, ".config", "systemd", "user", "lostos-server.service")
		body := "[Unit]\nDescription=lostOS server\nAfter=network-online.target\n\n[Service]\nWorkingDirectory=" + app + "\nExecStart=" + py + " -u voice_server.py\nRestart=on-failure\nRestartSec=5\n\n[Install]\nWantedBy=default.target\n"
		if err := os.MkdirAll(filepath.Dir(unit), 0o755); err == nil {
			if err = os.WriteFile(unit, []byte(body), 0o644); err == nil {
				if exec.Command("systemctl", "--user", "daemon-reload").Run() == nil && exec.Command("systemctl", "--user", "enable", "--now", "lostos-server.service").Run() == nil {
					return nil
				}
			}
		}
		fmt.Fprintln(errw, "systemd для пользователя недоступен: запускаю сервер сейчас, но после перезагрузки его надо запускать самим")
		c := exec.Command(py, "-u", "voice_server.py")
		c.Dir = app
		if f, err := os.OpenFile(logf, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			c.Stdout, c.Stderr = f, f
		}
		return c.Start()
	}
}

func helloOK(base string) bool {
	_, err := hello(base)
	return err == nil
}

// setupLocalServer does everything. speaker is the address of the speaker ("192.168.1.7:5555"): it tells which address of this computer the speaker can reach.
// -> the address the speaker will link to ("http://192.168.1.20:8765") and the one-time login link of the panel ("" when the owner already exists).
func setupLocalServer(speaker string) (string, string, bool) {
	ip := lanIPFor(speaker)
	if ip == "" {
		fmt.Fprintln(errw, "не удалось определить адрес этого компьютера в сети колонки")
		return "", "", false
	}
	dir := localDir()
	oldEnv, _ := os.ReadFile(filepath.Join(dir, "app", ".env"))
	port := pickPort(string(oldEnv))
	url := fmt.Sprintf("https://%s:%d", ip, port)
	fmt.Fprintf(out, "== сервер на этом компьютере (%s)\n", url)
	py, err := findPython()
	if err != nil {
		fmt.Fprintln(errw, err)
		return "", "", false
	}
	fmt.Fprintf(out, "Python: %s\n", py)
	app, data := filepath.Join(dir, "app"), filepath.Join(dir, "data")
	for _, d := range []string{app, data} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			fmt.Fprintln(errw, err)
			return "", "", false
		}
	}
	if err := fetchServerCode(app); err != nil {
		fmt.Fprintln(errw, "ошибка:", err)
		return "", "", false
	}
	vp := serverVenvPython(dir)
	if _, err := os.Stat(vp); err != nil {
		fmt.Fprintln(out, "создаю venv")
		if o, err := exec.Command(py, "-m", "venv", filepath.Join(dir, "venv")).CombinedOutput(); err != nil {
			fmt.Fprintf(errw, "ошибка venv: %v\n%s\n", err, o)
			return "", "", false
		}
	}
	fmt.Fprintln(out, "ставлю зависимости (первый раз это занимает пару минут)")
	pipArgs := []string{"-m", "pip", "install", "--disable-pip-version-check", "-q", "-r", filepath.Join(app, "requirements.txt")}
	if runtime.GOOS == "linux" { // the default torch wheel of Linux drags in gigabytes of CUDA: the CPU build is all the server needs
		pipArgs = append(pipArgs, "--extra-index-url", "https://download.pytorch.org/whl/cpu")
	}
	pip := exec.Command(vp, pipArgs...)
	pip.Stdout, pip.Stderr = out, errw
	if err := pip.Run(); err != nil {
		fmt.Fprintln(errw, "ошибка pip:", err)
		return "", "", false
	}
	cert := exec.Command(vp, "-c", "import certs,sys; assert callable(getattr(certs, 'local_context', None)), 'Update lostos-server for local TLS support'; certs.ensure({sys.argv[1]})", ip)
	cert.Dir = app
	cert.Env = append(os.Environ(), "LOSTOS_DATA="+data)
	if output, err := cert.CombinedOutput(); err != nil {
		fmt.Fprintf(errw, "не удалось создать сертификат: %v\n%s\n", err, output)
		return "", "", false
	}
	localRoots.Store(url, filepath.Join(data, "certs", "ca.crt"))
	envPath := filepath.Join(app, ".env")
	old, _ := os.ReadFile(envPath)
	env := mergeEnv(string(old), map[string]string{
		"LOSTOS_LITE": "1", "LOSTOS_AUTH": "1", "LOSTOS_LOCAL_TLS": "1", "LOSTOS_PUBLIC_URL": url, "TTS_PORT": fmt.Sprint(port),
		"LOSTOS_DATA": filepath.ToSlash(data), "LOSTOS_BIND": "0.0.0.0", "LOSTOS_NO_LOCAL_ENGINES": "1",
	})
	if err := os.WriteFile(envPath, []byte(env), 0o600); err != nil {
		fmt.Fprintln(errw, err)
		return "", "", false
	}
	fmt.Fprintf(out, "Для входа в панель добавьте локальный CA в доверенные сертификаты браузера: %s\n", filepath.Join(data, "certs", "ca.crt"))
	local := url
	if helloOK(local) {
		fmt.Fprintln(out, "сервер уже запущен: чтобы он взял новый код, перезапустите его (или перезагрузите компьютер)")
	} else {
		if err := autostartAndStart(dir); err != nil {
			fmt.Fprintln(errw, "не удалось запустить сервер:", err)
			return "", "", false
		}
		fmt.Fprintln(out, "жду, пока сервер ответит")
		up := false
		for i := 0; i < 60 && !up; i++ {
			time.Sleep(time.Second)
			up = helloOK(local)
		}
		if !up {
			fmt.Fprintf(errw, "сервер не ответил за минуту, смотрите %s\n", filepath.Join(dir, "server.log"))
			return "", "", false
		}
	}
	link := ""
	if b, err := os.ReadFile(filepath.Join(data, "setup-link.txt")); err == nil {
		link = strings.TrimSpace(string(b))
	}
	fmt.Fprintln(out, "сервер работает и запускается при входе в систему")
	if runtime.GOOS == "windows" {
		fmt.Fprintln(out, "если Windows спросит про сетевой доступ для Python, разрешите его для частных сетей: иначе колонка не достучится")
	}
	fmt.Fprintf(out, "совет: закрепите за компьютером постоянный адрес %s в роутере, иначе при смене адреса колонка потеряет сервер\n", ip)
	return url, link, true
}
