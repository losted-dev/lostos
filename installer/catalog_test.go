package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func signed(t *testing.T, raw []byte, priv ed25519.PrivateKey) string {
	return base64.StdEncoding.EncodeToString(ed25519.Sign(priv, append([]byte(modelsTag), raw...)))
}

func TestCatalogSignature(t *testing.T) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	old := teamKeys
	defer func() { teamKeys = old }()
	teamKeys = []string{base64.StdEncoding.EncodeToString(pub)}
	raw := []byte(`{"v":1,"models":[]}`)
	if !verifyBlob(modelsTag, raw, signed(t, raw, priv)) {
		t.Fatal("a good signature is refused")
	}
	if verifyBlob(modelsTag, append(raw, ' '), signed(t, raw, priv)) {
		t.Fatal("a changed catalog is accepted")
	}
	_, other, _ := ed25519.GenerateKey(rand.Reader)
	if verifyBlob(modelsTag, raw, signed(t, raw, other)) {
		t.Fatal("a signature by a stranger is accepted")
	}
	if verifyBlob(modelsTag, raw, "not base64 !!") {
		t.Fatal("garbage is accepted")
	}
	if verifyBlob("lostos-registry-v1\n", raw, signed(t, raw, priv)) {
		t.Fatal("the signature of another kind of file is accepted")
	}
}

func TestParseCatalogRefusesBadEntries(t *testing.T) {
	for _, bad := range []string{`{"v":2,"models":[]}`, `{"v":1,"models":[{"code":"../x","arch":"armhf"}]}`, `{"v":1,"models":[{"code":"ok","arch":"mips"}]}`,
		`{"v":1,"models":[{"code":"ok","arch":"armhf","profile":"../../etc/passwd"}]}`, `nonsense`} {
		if _, err := parseCatalog([]byte(bad)); err == nil {
			t.Errorf("accepted: %s", bad)
		}
	}
	if list, err := parseCatalog([]byte(`{"v":1,"models":[{"code":"x-new","name":"New","arch":"armhf","status":"untested","profile":"x-new.conf"}]}`)); err != nil || len(list) != 1 {
		t.Fatalf("a good catalog is refused: %v", err)
	}
}

func TestModelsFromTheCatalog(t *testing.T) {
	if archOf("bergamot") != "armhf" || archOf("cucumber") != "aarch64" || !hasZigbee("cucumber") || hasZigbee("bergamot") {
		t.Fatal("the built-in models changed")
	}
	if archOf("station-midi") != "" {
		t.Fatal("a model whose support is being prepared must not be installed")
	}
	if archOf("nonsense") != "" || modelName("nonsense") != "" {
		t.Fatal("an unknown model must not be installed")
	}
	mergeCatalog([]modelInfo{{Code: "station-midi", Name: "Yandex Station 2", Arch: "armhf", Status: "untested"}})
	if archOf("station-midi") != "armhf" {
		t.Fatal("the catalog does not enable a model")
	}
	catalog["station-midi"] = modelInfo{Code: "station-midi", Name: "Yandex Station 2", Arch: "armhf", Status: "preparing"}
}

func TestProfileFromTheNetworkIsChecked(t *testing.T) {
	profile := []byte("codename=station-x\nmodel_name=X\n")
	sum := sha256.Sum256(profile)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/station-x.conf" {
			w.Write(profile)
			return
		}
		http.NotFound(w, r)
	}))
	defer srv.Close()
	dir := t.TempDir()
	oldRoot, oldBase := root, catalogBase
	defer func() { root, catalogBase = oldRoot, oldBase; delete(catalog, "station-x") }()
	root, catalogBase = dir, srv.URL
	os.Setenv("XDG_CACHE_HOME", dir)
	os.Setenv("LocalAppData", dir)
	catalog["station-x"] = modelInfo{Code: "station-x", Arch: "armhf", Status: "untested", Profile: "station-x.conf", SHA256: hex.EncodeToString(sum[:])}
	p, err := profileFile("station-x")
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(p); string(got) != string(profile) {
		t.Fatal("wrong content")
	}
	catalog["station-x"] = modelInfo{Code: "station-x", Arch: "armhf", Status: "untested", Profile: "station-x.conf", SHA256: "00" + hex.EncodeToString(sum[:])[2:]}
	if _, err := profileFile("station-x"); err == nil {
		t.Fatal("a profile that differs from the signed checksum is accepted")
	}
	if err := os.MkdirAll(filepath.Join(dir, "satellite", "models"), 0o755); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(dir, "satellite", "models", "station-x.conf"), []byte("from the package"), 0o644)
	if p, err := profileFile("station-x"); err != nil || filepath.Base(filepath.Dir(p)) != "models" {
		t.Fatalf("the package comes first: %v %s", err, p)
	}
}
