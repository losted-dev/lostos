package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"testing"
	"time"
)

func TestBannedServerIsRefused(t *testing.T) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	d := fmt.Sprintf(`{"v": 1, "serial": 1, "expires": %q, "servers": [{"h": %q, "name": "Both"}, {"h": %q, "name": "Fine"}], "banned": [{"h": %q, "why": "steals keys"}, {"h": %q, "why": "wins over the list"}]}`,
		time.Now().Add(time.Hour).UTC().Format(time.RFC3339), hostHash("both.example.org"), hostHash("fine.example.org"), hostHash("evil.example.net"), hostHash("both.example.org"))
	listFixture(t, d, priv, pub)
	if v := lookupServer("https://EVIL.example.net:8443/x"); !v.Banned || v.Why != "steals keys" || v.Known {
		t.Fatalf("a banned server is not refused: %+v", v)
	}
	if v := lookupServer("both.example.org"); !v.Banned {
		t.Fatalf("a ban must win over the list of known servers: %+v", v)
	}
	if v := lookupServer("fine.example.org"); v.Banned || !v.Known {
		t.Fatalf("a good server is refused: %+v", v)
	}
	opts.yes = true
	defer func() { opts.yes = false }()
	if confirmServer("evil.example.net") {
		t.Fatal("--yes must not let a banned server through")
	}
}
