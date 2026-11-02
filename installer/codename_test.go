package main

import "testing"

// The name of the board comes first and the family after it: the exact name that the catalog does not know must not hide the family (a real Mini 3 is "yandex,bergamot-evt1 yandex,bergamot").
func TestPickCode(t *testing.T) {
	for in, want := range map[string]string{
		"yandex,bergamot-evt1 yandex,bergamot amlogic, axg": "bergamot",
		"yandex,bergamot yandex,bergamot-dvt":               "bergamot",
		"yandex,bergamot-evt2":                              "bergamot",
		"yandex,station-micro amlogic, axg":                 "station-micro",
		"yandex,station-midi":                               "station-midi",
		"yandex,unknown-x yandex,station-mini2":             "station-mini2",
		"yandex,brandnew-model":                             "brandnew-model",
		"amlogic, axg":                                      "",
		"":                                                  "",
	} {
		if got := pickCode(in); got != want {
			t.Errorf("%q: got %q, want %q", in, got, want)
		}
	}
}

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
