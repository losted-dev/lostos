package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

func sum(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

// a repository of releases: /latest redirects to the tag, the files are under /download/<tag>/
type fakeReleases struct {
	srv       *httptest.Server
	tag       string
	files     map[string][]byte // "v1.2.3/lostos-armhf.bin" -> bytes
	sums      map[string]string // tag -> text of SHA256SUMS
	hits      int32
	noRelease bool
}

func newFake(t *testing.T) *fakeReleases {
	f := &fakeReleases{tag: "v1.2.3", files: map[string][]byte{}, sums: map[string]string{}}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&f.hits, 1)
		switch {
		case r.URL.Path == "/latest":
			if f.noRelease {
				http.Redirect(w, r, f.srv.URL+"/releases", http.StatusFound)
				return
			}
			http.Redirect(w, r, f.srv.URL+"/releases/tag/"+f.tag, http.StatusFound)
		case strings.HasPrefix(r.URL.Path, "/download/") && strings.HasSuffix(r.URL.Path, "/SHA256SUMS"):
			tag := strings.Split(r.URL.Path, "/")[2]
			if s, ok := f.sums[tag]; ok {
				fmt.Fprint(w, s)
				return
			}
			http.NotFound(w, r)
		case strings.HasPrefix(r.URL.Path, "/download/"):
			if b, ok := f.files[strings.TrimPrefix(r.URL.Path, "/download/")]; ok {
				w.Write(b)
				return
			}
			http.NotFound(w, r)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeReleases) publish(tag string, data []byte) {
	f.files[tag+"/lostos-armhf.bin"] = data
	f.sums[tag] = sum(data) + "  lostos-armhf.bin\n" + strings.Repeat("0", 64) + "  other.bin\n"
}

func setup(t *testing.T, f *fakeReleases, pkg []byte) {
	dir := t.TempDir()
	oldRoot, oldBase, oldOpts := root, releaseBase, opts
	t.Cleanup(func() { root, releaseBase, opts, fwChosen = oldRoot, oldBase, oldOpts, map[string]fwResult{} })
	root, releaseBase, opts, fwChosen = dir, f.srv.URL, options{}, map[string]fwResult{}
	os.Setenv("XDG_CACHE_HOME", dir)
	os.Setenv("LocalAppData", dir)
	os.Setenv("HOME", dir)
	if pkg != nil {
		os.MkdirAll(filepath.Join(dir, "satellite", "build", "armhf"), 0o755)
		os.WriteFile(filepath.Join(dir, "satellite", "build", "armhf", "lostos"), pkg, 0o755)
	}
}

func blob(seed string) []byte { return []byte(strings.Repeat(seed, 4096)) }

func TestParseSums(t *testing.T) {
	m := parseSums([]byte(strings.Repeat("a", 64) + "  lostos-armhf.bin\n" + strings.Repeat("b", 64) + " *lostos-aarch64.bin\nnonsense\n\n"))
	if m["lostos-armhf.bin"] != strings.Repeat("a", 64) || m["lostos-aarch64.bin"] != strings.Repeat("b", 64) || len(m) != 2 {
		t.Fatalf("%v", m)
	}
}

func TestLatestTag(t *testing.T) {
	f := newFake(t)
	setup(t, f, nil)
	if tag, err := latestTag(); err != nil || tag != "v1.2.3" {
		t.Fatalf("%q %v", tag, err)
	}
	f.noRelease = true
	if _, err := latestTag(); err == nil || !strings.Contains(err.Error(), "no release") {
		t.Fatalf("no release must be said: %v", err)
	}
	f.noRelease, f.tag = false, "v1.2.3/../../evil"
	if _, err := latestTag(); err == nil {
		t.Fatal("a strange tag is accepted")
	}
}

func TestNewerReleaseIsDownloadedAndChecked(t *testing.T) {
	f := newFake(t)
	pkg, rel := blob("old"), blob("new")
	f.publish("v1.2.3", rel)
	setup(t, f, pkg)
	p, note, err := firmwareBinary("armhf")
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(p); string(got) != string(rel) || !strings.Contains(note, "v1.2.3 from GitHub") {
		t.Fatalf("the release must be used: %s %q", p, note)
	}
	if strings.HasPrefix(p, filepath.Join(root, "satellite")) {
		t.Fatal("the package binary was used")
	}
	// the second ask is answered without the network
	hits := atomic.LoadInt32(&f.hits)
	if p2, _, _ := firmwareBinary("armhf"); p2 != p || atomic.LoadInt32(&f.hits) != hits {
		t.Fatal("the choice is made once per run")
	}
}

func TestPackageOfTheSameReleaseIsKept(t *testing.T) {
	f := newFake(t)
	same := blob("same")
	f.publish("v1.2.3", same)
	setup(t, f, same)
	p, note, err := firmwareBinary("armhf")
	if err != nil || p != filepath.Join(root, "satellite", "build", "armhf", "lostos") || !strings.Contains(note, "is the release v1.2.3") {
		t.Fatalf("%s %q %v", p, note, err)
	}
	if _, err := os.Stat(filepath.Join(os.Getenv("HOME"), "lostos-install")); err == nil {
		t.Log("cache folder exists (not an error)")
	}
}

func TestChecksumMismatchNeverInstalls(t *testing.T) {
	f := newFake(t)
	pkg := blob("old")
	f.publish("v1.2.3", blob("new"))
	f.files["v1.2.3/lostos-armhf.bin"] = blob("tampered") // what is served is not what SHA256SUMS says
	setup(t, f, pkg)
	p, note, err := firmwareBinary("armhf")
	if err != nil || !strings.HasPrefix(p, filepath.Join(root, "satellite")) || !strings.Contains(note, "not used") {
		t.Fatalf("by default the package stays and the person is told: %s %q %v", p, note, err)
	}
	fwChosen = map[string]fwResult{}
	opts.version = "v1.2.3"
	if _, _, err := firmwareBinary("armhf"); err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Fatalf("a pinned release that does not match is an error: %v", err)
	}
}

func TestPinnedVersion(t *testing.T) {
	f := newFake(t)
	f.publish("v1.0.0", blob("one"))
	f.publish("v1.2.3", blob("three"))
	setup(t, f, blob("old"))
	opts.version = "v1.0.0"
	p, note, err := firmwareBinary("armhf")
	if got, _ := os.ReadFile(p); err != nil || string(got) != string(blob("one")) || !strings.Contains(note, "v1.0.0") {
		t.Fatalf("%q %v", note, err)
	}
	fwChosen = map[string]fwResult{}
	opts.version = "v9.9.9"
	if _, _, err := firmwareBinary("armhf"); err == nil {
		t.Fatal("a release that does not exist must be an error")
	}
	if _, _, err := fetchFirmware("armhf", "../x"); err == nil {
		t.Fatal("a tag with a path in it is accepted")
	}
}

func TestOfflineAndNoNetworkUseThePackage(t *testing.T) {
	f := newFake(t)
	f.publish("v1.2.3", blob("new"))
	setup(t, f, blob("old"))
	opts.offline = true
	p, _, err := firmwareBinary("armhf")
	if err != nil || !strings.HasPrefix(p, filepath.Join(root, "satellite")) || atomic.LoadInt32(&f.hits) != 0 {
		t.Fatalf("--offline must not touch the network: %s %v hits=%d", p, err, f.hits)
	}
	fwChosen = map[string]fwResult{}
	opts.offline = false
	releaseBase = "http://127.0.0.1:9" // nobody listens
	p, note, err := firmwareBinary("armhf")
	if err != nil || !strings.HasPrefix(p, filepath.Join(root, "satellite")) || !strings.Contains(note, "GitHub:") {
		t.Fatalf("%s %q %v", p, note, err)
	}
	fwChosen = map[string]fwResult{}
	os.Remove(filepath.Join(root, "satellite", "build", "armhf", "lostos"))
	if _, _, err := firmwareBinary("armhf"); err == nil {
		t.Fatal("no package and no network must be an error")
	}
}
