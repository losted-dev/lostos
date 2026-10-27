package main

import (
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestServerRejectsPlainHTTP(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"lostos":"server","version":"test","lite":true}`))
	}))
	defer srv.Close()
	if _, err := hello(srv.URL); err == nil {
		t.Fatal("an unencrypted station link was accepted")
	}
}

func TestServerTLSRequiresProvisionedCAAndBlocksDowngrade(t *testing.T) {
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Error("HTTPS client followed a downgrade")
	}))
	defer plain.Close()
	tlsServer := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/hello" {
			w.Write([]byte(`{"lostos":"server","version":"test","lite":true}`))
			return
		}
		http.Redirect(w, r, plain.URL, http.StatusFound)
	}))
	defer tlsServer.Close()
	if _, err := hello(tlsServer.URL); err == nil {
		t.Fatal("untrusted certificate accepted")
	}
	ca := filepath.Join(t.TempDir(), "ca.crt")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: tlsServer.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	localRoots.Store(tlsServer.URL, ca)
	defer localRoots.Delete(tlsServer.URL)
	info, err := hello(tlsServer.URL)
	if err != nil || !info.Secure || info.Plain {
		t.Fatalf("provisioned HTTPS failed: %+v %v", info, err)
	}
	client, err := serverClient(tlsServer.URL)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Get(tlsServer.URL + "/redirect"); err == nil {
		t.Fatal("HTTP downgrade accepted")
	}
	os.WriteFile(ca, []byte("invalid CA"), 0600)
	if _, err := hello(tlsServer.URL); err == nil {
		t.Fatal("invalid CA fell back to default trust")
	}
}
