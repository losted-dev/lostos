package main

import (
	"slices"
	"strings"
	"testing"
)

func TestPlanRootkeeper(t *testing.T) {
	const L = "while true; do\n  sleep 60\ndone\n"
	const S, H = hookSat, hookSSH
	cases := []struct {
		in    string
		ssh   bool
		out   string
		added []string
		write bool
	}{
		{"#!/bin/sh\necho start\n" + L, false, "#!/bin/sh\necho start\n" + S + "\n\n" + L, []string{S}, true},
		{"#!/bin/sh\necho start\n", false, "#!/bin/sh\necho start\n\n" + S + "\n", []string{S}, true},
		{"#!/bin/sh\n", true, "#!/bin/sh\n\n" + H + "\n" + S + "\n", []string{H, S}, true},
		{"#!/bin/sh\n" + S + "\n" + L, false, "#!/bin/sh\n" + S + "\n" + L, nil, false},
		{"#!/bin/sh\n" + S + "\n" + L, true, "#!/bin/sh\n" + S + "\n" + H + "\n\n" + L, []string{H}, true},
	}
	for i, c := range cases {
		out, added, write := planRootkeeper(c.in, c.ssh)
		if out != c.out || !slices.Equal(added, c.added) || write != c.write {
			t.Errorf("case %d: got write=%v added=%q\n%s\nwant write=%v added=%q\n%s", i+1, write, added, out, c.write, c.added, c.out)
		}
		if d := checkRootkeeper(c.in, out); d != "" {
			t.Errorf("case %d: checkRootkeeper: %s", i+1, d)
		}
	}
}

func TestCheckRootkeeperRejects(t *testing.T) {
	in := "#!/bin/sh\necho start\nwhile true; do\n  sleep 60\ndone\n"
	out, _, _ := planRootkeeper(in, false)
	if d := checkRootkeeper(in, strings.Replace(out, "sleep 60", "sleep 61", 1)); d == "" {
		t.Error("changed loop body accepted")
	}
	if d := checkRootkeeper(in, strings.Replace(out, "echo start\n", "", 1)); d == "" {
		t.Error("removed line accepted")
	}
}

func TestRemoveHooks(t *testing.T) {
	base := "#!/bin/sh\necho start\n\nwhile true; do sleep 60; done\n"
	withHook, added, write := planRootkeeper(base, false)
	if !write || len(added) != 1 {
		t.Fatalf("hook not planned: %v %v", added, write)
	}
	back, removed := removeHooks(withHook, false)
	if !removed || back != base {
		t.Fatalf("removal does not restore the original:\n%q\n%q", back, base)
	}
	if again, removed := removeHooks(back, false); removed || again != back {
		t.Fatal("a script without our hook must stay as it is")
	}
	// the hook of dropbear stays unless asked for
	both, _, _ := planRootkeeper(base, true)
	only, _ := removeHooks(both, false)
	if !strings.Contains(only, hookSSH) || strings.Contains(only, hookSat) {
		t.Fatalf("only the satellite hook must go:\n%s", only)
	}
}
