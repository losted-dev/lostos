package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func payloadZip(files map[string]string) []byte {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for n, c := range files {
		w, _ := zw.Create(n)
		w.Write([]byte(c))
	}
	zw.Close()
	return buf.Bytes()
}

func publishPayload(f *fakeReleases, tag string, z []byte) {
	f.files[tag+"/"+payloadAsset] = z
	f.sums[tag] = sum(z) + "  " + payloadAsset + "\n"
}

func TestFetchPayloadFromTheRelease(t *testing.T) {
	f := newFake(t)
	setup(t, f, nil)
	publishPayload(f, "v1.2.3", payloadZip(map[string]string{"satellite/deploy/boot.sh": "#!/bin/sh", "receivers/armhf/x/y": "bin"}))
	dir, err := fetchPayload("")
	if err != nil {
		t.Fatal(err)
	}
	if !hasPayload(dir) {
		t.Fatal("the files are not there")
	}
	if _, err := os.Stat(filepath.Join(dir, "receivers", "armhf", "x", "y")); err != nil {
		t.Fatal("receivers are not there")
	}
	hits := f.hits
	if _, err := fetchPayload(""); err != nil || f.hits > hits+1 { // the second time only /latest is asked: the files are cached
		t.Fatalf("not cached: %v hits %d -> %d", err, hits, f.hits)
	}
}

func TestFetchPayloadRefusesAWrongChecksum(t *testing.T) {
	f := newFake(t)
	setup(t, f, nil)
	publishPayload(f, "v1.2.3", payloadZip(map[string]string{"satellite/deploy/boot.sh": "x"}))
	f.sums["v1.2.3"] = strings.Repeat("0", 64) + "  " + payloadAsset + "\n"
	if _, err := fetchPayload(""); err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Fatalf("a payload with a wrong checksum was accepted: %v", err)
	}
}

func TestFetchPayloadWhenTheReleaseHasNone(t *testing.T) {
	f := newFake(t)
	setup(t, f, nil)
	publishPayload(f, "v1.2.3", payloadZip(map[string]string{"satellite/deploy/boot.sh": "x"}))
	f.sums["v1.2.3"] = sum([]byte("x")) + "  lostos-armhf.bin\n"
	if _, err := fetchPayload(""); err == nil {
		t.Fatal("an error was expected")
	}
}

func TestUnzipPayloadRefusesAnEscapingName(t *testing.T) {
	if err := unzipPayload(payloadZip(map[string]string{"../evil.txt": "x"}), t.TempDir()); err == nil {
		t.Fatal("a name that leaves the folder was accepted")
	}
}
