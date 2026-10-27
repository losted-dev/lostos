package main

import "testing"

func TestHomeURL(t *testing.T) {
	for addr, want := range map[string]bool{
		"http://192.168.1.5:8765": true, "http://10.0.0.2": true, "http://172.16.5.1:80": true, "http://localhost:8765": true, "http://127.0.0.1:8765": true, "http://nas.local:8765": true,
		"http://8.8.8.8": false, "http://example.org": false, "http://172.32.0.1": false, "https://192.168.1.5": false, "192.168.1.5": false, "http://192.168.1.5.evil.org": false,
	} {
		if got := homeURL(addr); got != want {
			t.Errorf("homeURL(%q) = %v, want %v", addr, got, want)
		}
	}
}
