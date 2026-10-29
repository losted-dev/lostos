package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The catalog of speaker models. Which stations the installer knows is DATA, not code: satellite/models/catalog.json of the package (and, newer, the one that the lostOS repository
// publishes, signed by the team) says what a model is called, which userland it has, whether the daemon knows it without a profile, and in which state the support is. Without any
// catalog the six models below are known, as before.
//
//	status "verified"  tried on a real speaker
//	status "untested"  the profile is made from the dump of the firmware, not tried on the speaker: installed after a warning
//	status "preparing" the model is known, the support is not ready: not installed
//
// A catalog from the network is accepted only when catalog.json.sig is an Ed25519 signature of its bytes by a key of the team (teamKeys); it lists the SHA-256 of every profile file, so a
// profile that comes from the network is checked against the signed list as well.

// teamKeys are the public keys of the lostOS team (base64, the same keys that sign the plugin registry).
var teamKeys = []string{"Kn7QcpJmGVBEwX3DAhFlFQzmA8AcRVrPKKgOqtlth08="}

const (
	modelsTag         = "lostos-models-v1\n"
	defaultCatalogURL = "https://raw.githubusercontent.com/losted-dev/lostos/main/satellite/models"
)

type modelInfo struct {
	Code    string `json:"code"`
	Name    string `json:"name"`
	Arch    string `json:"arch"`
	Zigbee  bool   `json:"zigbee"`
	Builtin bool   `json:"builtin"`
	Status  string `json:"status"`
	Note    string `json:"note"`
	Profile string `json:"profile"` // file name in the catalog folder, e.g. station-micro.conf
	SHA256  string `json:"sha256"`  // of that file (the network copy is checked against it)
}

var catalog = map[string]modelInfo{
	"cucumber":      {Code: "cucumber", Name: "Yandex Station Midi", Arch: "aarch64", Zigbee: true, Builtin: true, Status: "verified"},
	"plum":          {Code: "plum", Name: "Yandex Station Lite 2", Arch: "armhf", Builtin: true, Status: "verified"},
	"bergamot":      {Code: "bergamot", Name: "Yandex Station Mini 3", Arch: "armhf", Builtin: true, Status: "verified"},
	"station-micro": {Code: "station-micro", Name: "Yandex Station Lite (Micro)", Arch: "armhf", Status: "untested"},
	"station-mini2": {Code: "station-mini2", Name: "Yandex Station Mini 2", Arch: "armhf", Status: "untested"},
	"station-midi":  {Code: "station-midi", Name: "Yandex Station 2", Arch: "armhf", Status: "preparing", Note: "the top light panel needs a LED type and a profile from a real speaker (the model has no screen and no clock)"},
}

var catalogBase string // where a profile of the network catalog is fetched from ("" = only the package)

func verifyBlob(tag string, raw []byte, sigB64 string) bool {
	return verifyWith(teamKeys, tag, raw, sigB64)
}

func parseCatalog(raw []byte) ([]modelInfo, error) {
	var doc struct {
		V      int         `json:"v"`
		Models []modelInfo `json:"models"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil || doc.V != 1 {
		return nil, errors.New("catalog: unknown format")
	}
	for _, m := range doc.Models {
		if m.Code == "" || strings.ContainsAny(m.Code, "/\\ .") || (m.Arch != "armhf" && m.Arch != "aarch64") {
			return nil, fmt.Errorf("catalog: bad entry %q", m.Code)
		}
		if m.Profile != "" && (strings.ContainsAny(m.Profile, "/\\") || !strings.HasSuffix(m.Profile, ".conf")) {
			return nil, fmt.Errorf("catalog: bad profile name %q", m.Profile)
		}
	}
	return doc.Models, nil
}

func mergeCatalog(list []modelInfo) {
	for _, m := range list {
		if m.Status == "" {
			m.Status = "untested"
		}
		catalog[m.Code] = m
	}
}

// loadCatalog reads the catalog of the package, then the newer one of the network (when allowed). Nothing here is fatal: the six built-in models stay.
func loadCatalog(root string, offline bool) {
	if raw, err := os.ReadFile(filepath.Join(root, "satellite", "models", "catalog.json")); err == nil {
		if list, err := parseCatalog(raw); err == nil {
			mergeCatalog(list)
		} else {
			fmt.Fprintln(errw, "catalog of the package:", err)
		}
	}
	if offline {
		return
	}
	base := catalogBase
	if base == "" {
		base = defaultCatalogURL
	}
	raw, err := httpGet(base+"/catalog.json", 256<<10)
	if err != nil {
		return // no network, no repository yet: the package catalog is enough
	}
	sig, err := httpGet(base+"/catalog.json.sig", 4<<10)
	if err != nil || !verifyBlob(modelsTag, raw, string(sig)) {
		fmt.Fprintln(errw, "catalog from the network: the signature does not match, ignored")
		return
	}
	list, err := parseCatalog(raw)
	if err != nil {
		fmt.Fprintln(errw, "catalog from the network:", err)
		return
	}
	mergeCatalog(list)
	catalogBase = base
}

func httpGet(url string, limit int64) ([]byte, error) {
	c := http.Client{Timeout: 4 * time.Second}
	r, err := c.Get(url)
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", r.StatusCode)
	}
	b, err := io.ReadAll(io.LimitReader(r.Body, limit+1))
	if err != nil || int64(len(b)) > limit {
		return nil, errors.New("too large")
	}
	return b, nil
}

// profileFile returns a path of the profile of the model: the package first, else the file of the network catalog (kept in cache after its SHA-256 matches the signed list).
func profileFile(code string) (string, error) {
	local := filepath.Join(root, "satellite", "models", code+".conf")
	if _, err := os.Stat(local); err == nil {
		return local, nil
	}
	m, ok := catalog[code]
	if !ok || m.Profile == "" || m.SHA256 == "" || catalogBase == "" {
		return "", fmt.Errorf("no model profile for %s in the package or in the signed catalog", code)
	}
	raw, err := httpGet(catalogBase+"/"+m.Profile, 128<<10)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(raw)
	if hex.EncodeToString(sum[:]) != strings.ToLower(m.SHA256) {
		return "", errors.New("the profile from the network does not match the checksum of the signed catalog")
	}
	dir, _ := os.UserCacheDir()
	dir = filepath.Join(dir, "lostos-install")
	if os.MkdirAll(dir, 0o755) != nil {
		dir = os.TempDir()
	}
	p := filepath.Join(dir, code+".conf")
	return p, os.WriteFile(p, raw, 0o644)
}
