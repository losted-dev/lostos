package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// The daemon from GitHub Releases. The installer asks the repository for the newest stable release the same way the speaker does for itself (github.com redirects
// releases/latest to releases/tag/<tag>: no API, no token, no rate limit), takes lostos-<arch>.bin and checks it against the SHA256SUMS of that release.
// Whoever controls the repository controls the release, as with the self-update of the speaker: the protection is GitHub's own (two-step login, protected tags), not a signature.
// A package that was built from the same release has the same checksum: nothing is downloaded then. --offline (or no network) uses the package.
var (
	releaseRepo = "losted-dev/lostos"
	releaseBase = "" // the folder of the releases; "" = https://github.com/<repo>/releases (tests and forks point it elsewhere)
	tagRe       = regexp.MustCompile(`^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$`)
	fwChosen    = map[string]fwResult{}
)

type fwResult struct {
	path, note string
	err        error
}

func releasesURL() string {
	if releaseBase != "" {
		return strings.TrimRight(releaseBase, "/")
	}
	return "https://github.com/" + releaseRepo + "/releases"
}

// latestTag: the tag of the newest stable release ("" and an error when there is none).
func latestTag() (string, error) {
	c := http.Client{Timeout: 10 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	r, err := c.Get(releasesURL() + "/latest")
	if err != nil {
		return "", err
	}
	defer r.Body.Close()
	if r.StatusCode < 300 || r.StatusCode >= 400 {
		return "", fmt.Errorf("HTTP %d", r.StatusCode)
	}
	loc := r.Header.Get("Location")
	i := strings.LastIndex(loc, "/releases/tag/")
	if i < 0 {
		return "", errors.New("no release is published yet")
	}
	tag := loc[i+len("/releases/tag/"):]
	if !tagRe.MatchString(tag) {
		return "", fmt.Errorf("unexpected release tag %q", tag)
	}
	return tag, nil
}

func httpGetBig(url string, limit int64) ([]byte, error) {
	c := http.Client{Timeout: 3 * time.Minute}
	r, err := c.Get(url)
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d from %s", r.StatusCode, url)
	}
	b, err := io.ReadAll(io.LimitReader(r.Body, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("the file is larger than expected")
	}
	return b, nil
}

// parseSums reads a SHA256SUMS file: "<64 hex digits>  <name>" (or " *<name>").
func parseSums(b []byte) map[string]string {
	out := map[string]string{}
	for _, l := range strings.Split(string(b), "\n") {
		f := strings.Fields(strings.TrimSpace(l))
		if len(f) == 2 && len(f[0]) == 64 {
			out[strings.TrimPrefix(f[1], "*")] = strings.ToLower(f[0])
		}
	}
	return out
}

// fetchFirmware: lostos-<arch>.bin of the release <tag>, checked against its SHA256SUMS, kept in the cache folder. -> (path, the checksum)
func fetchFirmware(arch, tag string) (string, string, error) {
	if !tagRe.MatchString(tag) {
		return "", "", fmt.Errorf("%q is not a release tag (v1.2.3)", tag)
	}
	base := releasesURL() + "/download/" + tag
	asset := "lostos-" + arch + ".bin"
	sums, err := httpGetBig(base+"/SHA256SUMS", 1<<20)
	if err != nil {
		return "", "", fmt.Errorf("SHA256SUMS: %v", err)
	}
	want := parseSums(sums)[asset]
	if want == "" {
		return "", "", fmt.Errorf("the release %s has no %s", tag, asset)
	}
	return downloadChecked(base+"/"+asset, want, tag, asset)
}

func downloadChecked(url, want, tag, asset string) (string, string, error) {
	dir, _ := os.UserCacheDir()
	dir = filepath.Join(dir, "lostos-install", "releases", tag)
	if os.MkdirAll(dir, 0o755) != nil {
		dir = filepath.Join(os.TempDir(), "lostos-install-"+tag)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return "", "", err
		}
	}
	p := filepath.Join(dir, asset)
	if sha256File(p) == want { // downloaded before and still good
		return p, want, nil
	}
	b, err := httpGetBig(url, 16<<20)
	if err != nil {
		return "", "", err
	}
	sum := sha256.Sum256(b)
	if hex.EncodeToString(sum[:]) != want {
		return "", "", fmt.Errorf("%s does not match the SHA256SUMS of the release %s: nothing is installed from it", asset, tag)
	}
	if err := os.WriteFile(p+".part", b, 0o755); err != nil {
		return "", "", err
	}
	return p, want, os.Rename(p+".part", p)
}

// firmwareBinary decides which daemon goes on the station of this architecture: -> (path, a line for the person). With --version it is that release or an error;
// otherwise the newest release when it is reachable and different from the package, else the package.
func firmwareBinary(arch string) (string, string, error) {
	if r, ok := fwChosen[arch]; ok {
		return r.path, r.note, r.err
	}
	path, note, err := chooseFirmware(arch)
	fwChosen[arch] = fwResult{path, note, err}
	return path, note, err
}

func chooseFirmware(arch string) (string, string, error) {
	pkg := filepath.Join(root, "satellite", "build", arch, "lostos")
	havePkg := fileKB(pkg) > 0
	usePkg := func(why string) (string, string, error) {
		if !havePkg {
			return "", "", fmt.Errorf("no daemon for %s: the package has none%s", arch, why)
		}
		return pkg, "from the package" + why, nil
	}
	if opts.version != "" {
		p, sum, err := fetchFirmware(arch, opts.version)
		if err != nil {
			return "", "", fmt.Errorf("--version %s: %v", opts.version, err)
		}
		return p, fmt.Sprintf("%s from GitHub (SHA-256 %s…)", opts.version, sum[:12]), nil
	}
	if opts.offline {
		return usePkg("")
	}
	tag, err := latestTag()
	if err != nil {
		return usePkg(" (GitHub: " + err.Error() + ")")
	}
	base := releasesURL() + "/download/" + tag
	sums, err := httpGetBig(base+"/SHA256SUMS", 1<<20)
	if err != nil {
		return usePkg(" (release " + tag + ": " + err.Error() + ")")
	}
	want := parseSums(sums)["lostos-"+arch+".bin"]
	if want == "" {
		return usePkg(" (the release " + tag + " has no build for " + arch + ")")
	}
	if havePkg && sha256File(pkg) == want {
		return pkg, "the package is the release " + tag, nil
	}
	p, _, err := downloadChecked(base+"/lostos-"+arch+".bin", want, tag, "lostos-"+arch+".bin")
	if err != nil {
		return usePkg(" (" + tag + " not used: " + err.Error() + ")")
	}
	return p, fmt.Sprintf("%s from GitHub (SHA-256 %s…)", tag, want[:12]), nil
}
