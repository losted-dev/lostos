package main

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// The servers that the installer knows. A speaker that is linked to a server lets it hear everything that is said after the wake word, control the speaker and (with the signed plugins
// only) run programs on it, so the installer says so when a person writes an address that is not on the list that the lostOS team keeps (plugapi.losted.dev/servers.json, signed;
// the addresses in it are hashes of the hosts, so that the list is not a catalogue of servers). The list only informs a person: the speaker never reads it.
var (
	listKeys    = []string{"0BfdLXVvjXbwewfoIz0PNDv0QDM+QkbXOMF5oGXHEMs=", "Kn7QcpJmGVBEwX3DAhFlFQzmA8AcRVrPKKgOqtlth08="} // plugapi's key (scope "developers") and the key of the team
	plugapiBase = "https://plugapi.losted.dev"
	serversOnce sync.Once
	serversVal  knownServers
)

const serversTag = "lostos-servers-v1\n"

type knownServers struct {
	names  map[string]string // sha256 of the host -> the name that the team gave the server
	banned map[string]string // sha256 of the host -> why the installer refuses it
	state  string            // "ok" | "unavailable" (no list could be read and checked) | "expired"
}

// What the list says about an address.
type verdict struct {
	Name   string // the name of a known server
	Known  bool
	Banned bool   // the installer refuses it
	Why    string // the reason, for a banned one
	State  string // "ok" | "unavailable" | "expired": whether the list could say anything
}

func verifyWith(keys []string, tag string, raw []byte, sigB64 string) bool {
	sig, err := base64.StdEncoding.DecodeString(strings.TrimSpace(sigB64))
	if err != nil || len(sig) != ed25519.SignatureSize {
		return false
	}
	for _, k := range keys {
		pub, err := base64.StdEncoding.DecodeString(k)
		if err == nil && len(pub) == ed25519.PublicKeySize && ed25519.Verify(ed25519.PublicKey(pub), append([]byte(tag), raw...), sig) {
			return true
		}
	}
	return false
}

// hostOf: the host of an address the way the list is made (server/devlist.py server_host): lower case, no scheme, port, path or dot at the end.
func hostOf(addr string) string {
	a := strings.TrimSpace(addr)
	if !strings.Contains(a, "://") {
		a = "https://" + a
	}
	u, err := url.Parse(a)
	if err != nil {
		return ""
	}
	return strings.TrimRight(strings.ToLower(u.Hostname()), ".")
}

func hostHash(addr string) string {
	s := sha256.Sum256([]byte(hostOf(addr)))
	return hex.EncodeToString(s[:])
}

func parseServers(raw []byte, now time.Time) (map[string]string, map[string]string, string) {
	var doc struct {
		V       int    `json:"v"`
		Expires string `json:"expires"`
		Servers []struct {
			H    string `json:"h"`
			Name string `json:"name"`
		} `json:"servers"`
		Banned []struct {
			H   string `json:"h"`
			Why string `json:"why"`
		} `json:"banned"`
	}
	if json.Unmarshal(raw, &doc) != nil || doc.V != 1 {
		return nil, nil, "unavailable"
	}
	if exp, err := time.Parse(time.RFC3339, doc.Expires); err != nil || now.After(exp) {
		return nil, nil, "expired"
	}
	m, b := map[string]string{}, map[string]string{}
	for _, s := range doc.Servers {
		m[strings.ToLower(s.H)] = s.Name
	}
	for _, s := range doc.Banned {
		b[strings.ToLower(s.H)] = s.Why
	}
	return m, b, "ok"
}

func loadServers() knownServers {
	serversOnce.Do(func() {
		dir, _ := os.UserCacheDir()
		dir = filepath.Join(dir, "lostos-install")
		_ = os.MkdirAll(dir, 0o755)
		cache, cacheSig := filepath.Join(dir, "servers.json"), filepath.Join(dir, "servers.json.sig")
		raw, err1 := httpGet(plugapiBase+"/servers.json", 64<<10)
		sig, err2 := httpGet(plugapiBase+"/servers.json.sig", 4096)
		if err1 == nil && err2 == nil && verifyWith(listKeys, serversTag, raw, string(sig)) {
			_ = os.WriteFile(cache, raw, 0o644)
			_ = os.WriteFile(cacheSig, sig, 0o644)
		} else if r, e1 := os.ReadFile(cache); e1 == nil { // the last list that was good (checked again: the file may have been edited)
			s, e2 := os.ReadFile(cacheSig)
			if e2 != nil || !verifyWith(listKeys, serversTag, r, string(s)) {
				serversVal = knownServers{state: "unavailable"}
				return
			}
			raw = r
		} else {
			serversVal = knownServers{state: "unavailable"}
			return
		}
		names, banned, state := parseServers(raw, time.Now())
		serversVal = knownServers{names: names, banned: banned, state: state}
	})
	return serversVal
}

// lookupServer: what the signed list says about the address: banned (refused with a reason), known (no warning), or neither (a warning; State says whether the list could speak at all).
func lookupServer(addr string) verdict {
	l := loadServers()
	h := hostHash(addr)
	if why, ok := l.banned[h]; ok {
		return verdict{Banned: true, Why: why, State: l.state}
	}
	if n, ok := l.names[h]; ok {
		return verdict{Name: n, Known: true, State: l.state}
	}
	return verdict{State: l.state}
}

// The words that a person reads when the server is not on the list (the wizard shows the same).
func unknownServerWarning(state string) string {
	head := "Этот сервер не входит в список проверенных lostOS."
	if state != "ok" {
		head = "Список проверенных серверов получить не удалось, поэтому этот адрес проверить нельзя."
	}
	return head + " Сервер слышит всё, что вы говорите колонке после слова активации, управляет ею и может запускать на ней программы (только подписанные). Указывайте свой сервер или такой, которому доверяете."
}

// confirmServer is the CLI side: a server that is not on the list is warned about, and the person agrees (--yes agrees for him, the warning is printed anyway).
func confirmServer(addr string) bool {
	v := lookupServer(addr)
	if v.Banned { // a hard stop: no --yes and no question
		fmt.Fprintf(errw, "ОТКАЗ: сервер %s в списке заблокированных lostOS: %s\n", hostOf(addr), v.Why)
		return false
	}
	if v.Known {
		fmt.Fprintf(out, "== сервер %s: известный lostOS (%s)\n", hostOf(addr), v.Name)
		return true
	}
	if homeURL(addr) { // a server of the owner inside his own network: nothing to warn about except that the link is not encrypted
		fmt.Fprintf(out, "== сервер %s: в домашней сети, связь без шифрования\n", hostOf(addr))
		return true
	}
	fmt.Fprintf(errw, "ВНИМАНИЕ: %s\n", unknownServerWarning(v.State))
	if opts.yes {
		return true
	}
	return strings.HasPrefix(strings.ToLower(readLine("Подключить колонку к "+hostOf(addr)+"? [y/N]: ")), "y")
}
