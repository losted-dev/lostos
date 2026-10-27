package main

import (
	"archive/zip"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestMergeEnvKeepsOwnerLines(t *testing.T) {
	old := "# mine\nLOSTOS_TG_TOKEN=abc\nLOSTOS_PUBLIC_URL=http://old\nTTS_PORT=1\n"
	got := mergeEnv(old, map[string]string{"LOSTOS_PUBLIC_URL": "http://192.168.1.2:8765", "LOSTOS_LITE": "1"})
	for _, want := range []string{"# mine\n", "LOSTOS_TG_TOKEN=abc\n", "LOSTOS_PUBLIC_URL=http://192.168.1.2:8765\n", "LOSTOS_LITE=1\n", "TTS_PORT=1\n"} {
		if !strings.Contains(got, want) {
			t.Errorf("missing %q in\n%s", want, got)
		}
	}
	if strings.Contains(got, "http://old") {
		t.Errorf("the old value stayed:\n%s", got)
	}
	if mergeEnv(got, map[string]string{"LOSTOS_LITE": "1"}) != got {
		t.Errorf("a second run changes the file")
	}
}

func makeZip(t *testing.T, files map[string]string) string {
	p := filepath.Join(t.TempDir(), "s.zip")
	f, _ := os.Create(p)
	zw := zip.NewWriter(f)
	for n, c := range files {
		w, _ := zw.Create(n)
		w.Write([]byte(c))
	}
	zw.Close()
	f.Close()
	return p
}

func TestUnzipServerTakesTheServerFolder(t *testing.T) {
	z := makeZip(t, map[string]string{
		"lostos-server-main/README.md":              "x",
		"lostos-server-main/server/voice_server.py": "print(1)",
		"lostos-server-main/server/web/app.js":      "js",
		"lostos-server-main/docs/a.md":              "d",
	})
	app := t.TempDir()
	if err := unzipServer(z, app); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"voice_server.py", "web/app.js"} {
		if _, err := os.Stat(filepath.Join(app, filepath.FromSlash(f))); err != nil {
			t.Errorf("%s was not extracted", f)
		}
	}
	if _, err := os.Stat(filepath.Join(app, "README.md")); err == nil {
		t.Errorf("a file outside server/ was extracted")
	}
}

func TestUnzipServerRefusesAnEscapingPath(t *testing.T) {
	z := makeZip(t, map[string]string{"r/server/voice_server.py": "x", "r/server/../../evil.txt": "x"})
	if err := unzipServer(z, t.TempDir()); err == nil {
		t.Errorf("a path that leaves the folder was accepted")
	}
}

func TestUnzipServerWithoutServerFails(t *testing.T) {
	if err := unzipServer(makeZip(t, map[string]string{"a/b.txt": "x"}), t.TempDir()); err == nil {
		t.Errorf("an archive without the server was accepted")
	}
}

// LOSTOS_E2E_LOCAL=<the server folder>: a real run (venv, pip, start) with a temporary home; needs python3 and the network for pip
func TestSetupLocalServerEndToEnd(t *testing.T) {
	src := os.Getenv("LOSTOS_E2E_LOCAL")
	if src == "" {
		t.Skip("set LOSTOS_E2E_LOCAL to the server folder to run it")
	}
	home := os.Getenv("LOSTOS_E2E_HOME") // keep the folder to read the log of the server
	if home == "" {
		home = t.TempDir()
	}
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	t.Setenv("LOCALAPPDATA", home)
	serverSrc, localPort = src, 18765
	defer func() { serverSrc, localPort = "", 8765 }()
	url, _, ok := setupLocalServer("127.0.0.1:5555")
	if !ok {
		t.Fatal("setup failed")
	}
	if !helloOK("http://127.0.0.1:18765") {
		t.Fatalf("the server does not answer (%s)", url)
	}
}
