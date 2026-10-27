package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

func listFixture(t *testing.T, doc string, signWith ed25519.PrivateKey, listPub ed25519.PublicKey) *httptest.Server {
	sig := base64.StdEncoding.EncodeToString(ed25519.Sign(signWith, append([]byte(serversTag), []byte(doc)...)))
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/servers.json":
			fmt.Fprint(w, doc)
		case "/servers.json.sig":
			fmt.Fprint(w, sig)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(srv.Close)
	oldKeys, oldBase := listKeys, plugapiBase
	listKeys, plugapiBase = []string{base64.StdEncoding.EncodeToString(listPub)}, srv.URL
	serversOnce, serversVal = sync.Once{}, knownServers{}
	dir := t.TempDir()
	os.Setenv("XDG_CACHE_HOME", dir)
	os.Setenv("LocalAppData", dir)
	os.Setenv("HOME", dir)
	t.Cleanup(func() {
		listKeys, plugapiBase = oldKeys, oldBase
		serversOnce, serversVal = sync.Once{}, knownServers{}
	})
	return srv
}

func doc(expires time.Time, hosts ...string) string {
	var items []string
	for _, h := range hosts {
		items = append(items, fmt.Sprintf(`{"h": %q, "name": "Official"}`, hostHash(h)))
	}
	return fmt.Sprintf(`{"v": 1, "serial": 1, "expires": %q, "servers": [%s]}`, expires.UTC().Format(time.RFC3339), strings.Join(items, ","))
}

func TestHostIsWrittenAnyWay(t *testing.T) {
	want := hostHash("os.example.org")
	for _, a := range []string{"os.example.org", "https://OS.Example.org/x", "HTTPS://os.example.org:8443/panel", "os.example.org.", "  os.example.org:1 "} {
		if hostHash(a) != want {
			t.Errorf("%q hashes differently", a)
		}
	}
	// the hash that server/devlist.py makes for the same host (the two tools must agree)
	if got := hostHash("os.example.org"); got != "8b5c6b1a8897326b6681f7307a2caa7f3e5499cee4dedeeac250052947fd8bc6" {
		t.Fatalf("the hash of a host differs from the one of the server tool: %s", got)
	}
}

func TestKnownAndUnknownServer(t *testing.T) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	listFixture(t, doc(time.Now().Add(time.Hour), "os.example.org"), priv, pub)
	if v := lookupServer("https://os.example.org:443"); !v.Known || v.Name != "Official" || v.State != "ok" || v.Banned {
		t.Fatalf("a listed server is not known: %+v", v)
	}
	if v := lookupServer("https://evil.example.net"); v.Known || v.Banned || v.State != "ok" {
		t.Fatalf("a stranger is known: %+v", v)
	}
}

func TestListFromAStrangerOrExpiredOrMissingSaysNothing(t *testing.T) {
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	_, other, _ := ed25519.GenerateKey(rand.Reader)
	listFixture(t, doc(time.Now().Add(time.Hour), "os.example.org"), other, pub) // signed by a key that is not trusted
	if v := lookupServer("os.example.org"); v.Known || v.State != "unavailable" {
		t.Fatalf("a list signed by a stranger is believed: %+v", v)
	}
	pub2, priv2, _ := ed25519.GenerateKey(rand.Reader)
	listFixture(t, doc(time.Now().Add(-time.Hour), "os.example.org"), priv2, pub2)
	if v := lookupServer("os.example.org"); v.Known || v.State != "expired" {
		t.Fatalf("an expired list is believed: %+v", v)
	}
	listFixture(t, "{}", priv2, pub2)
	plugapiBase = "http://127.0.0.1:9" // nobody listens, and nothing is cached
	serversOnce, serversVal = sync.Once{}, knownServers{}
	if v := lookupServer("os.example.org"); v.Known || v.State != "unavailable" {
		t.Fatalf("no list at all: %+v", v)
	}
}

func TestTheLastGoodListIsKeptWhenThereIsNoNetwork(t *testing.T) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	listFixture(t, doc(time.Now().Add(time.Hour), "os.example.org"), priv, pub)
	if v := lookupServer("os.example.org"); !v.Known {
		t.Fatal("not known with the network")
	}
	plugapiBase = "http://127.0.0.1:9"
	serversOnce, serversVal = sync.Once{}, knownServers{}
	if v := lookupServer("os.example.org"); !v.Known || v.Name != "Official" || v.State != "ok" {
		t.Fatalf("the saved list is not used: %+v", v)
	}
}
