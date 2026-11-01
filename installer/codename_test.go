package main

import "testing"

func TestCodenameWithHyphen(t *testing.T) {
	for in, want := range map[string]string{
		"yandex,station-micro amlogic, axg":   "station-micro",
		"yandex,bergamot yandex,bergamot-dvt": "bergamot",
		"yandex,station-mini2":                "station-mini2",
	} {
		if m := codename.FindStringSubmatch(in); m == nil || m[1] != want {
			t.Errorf("%q: got %v, want %q", in, m, want)
		}
	}
}
