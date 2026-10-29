package main

// From sorried, losted-dev/lostos#1.

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestServerAcceptsHomeHTTP(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"lostos":"server","version":"test","lite":true}`))
	}))
	defer server.Close()
	info, err := hello(server.URL)
	if err != nil || !info.Secure || !info.Plain {
		t.Fatalf("home HTTP refused: %+v %v", info, err)
	}
}

func TestServerRefusesRedirectDowngrade(t *testing.T) {
	client, err := serverClient("https://example.com")
	if err != nil {
		t.Fatal(err)
	}
	for _, to := range []string{"http://example.com/api/v1/hello", "https://evil.example.org/api/v1/hello"} {
		request, _ := http.NewRequest("GET", to, nil)
		if err := client.CheckRedirect(request, []*http.Request{request}); err == nil {
			t.Fatalf("redirect accepted: %s", to)
		}
	}
}

func TestServerRefusesUserInfo(t *testing.T) {
	if _, err := serverClient("https://user:pass@example.com"); err == nil {
		t.Fatal("an address with a login accepted")
	}
}
