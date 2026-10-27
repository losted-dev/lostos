package main

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

func TestServerRefusesPublicHTTP(t *testing.T) {
	for _, address := range []string{"http://example.com", "http://8.8.8.8", "http://192.168.1.5.evil.org"} {
		if _, err := serverClient(address); err == nil {
			t.Fatalf("public HTTP accepted: %s", address)
		}
	}
}

func TestServerRefusesRedirectDowngrade(t *testing.T) {
	client, err := serverClient("https://example.com")
	if err != nil {
		t.Fatal(err)
	}
	request, _ := http.NewRequest("GET", "http://example.com/api/v1/hello", nil)
	if err := client.CheckRedirect(request, []*http.Request{request}); err == nil {
		t.Fatal("HTTPS downgrade accepted")
	}
}
