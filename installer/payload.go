package main

// The installer on its own: when the files it needs (satellite/deploy, the speaker profiles, the receivers) are not next to it, it takes them from the newest release on GitHub:
// lostos-payload.zip, checked against the SHA256SUMS of that release (the protection is that of the repository, as for the daemon). They are kept in the cache folder.
// The adb tool is downloaded by findAdb as before. --offline never downloads: then the files must be next to the installer.

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const payloadAsset = "lostos-payload.zip"

func hasPayload(dir string) bool {
	_, err := os.Stat(filepath.Join(dir, "satellite", "deploy", "boot.sh"))
	return err == nil
}

// fetchPayload -> the folder with the files (cached per release). tag "" = the newest release.
func fetchPayload(tag string) (string, error) {
	if tag == "" {
		t, err := latestTag()
		if err != nil {
			return "", fmt.Errorf("GitHub: %v", err)
		}
		tag = t
	}
	if !tagRe.MatchString(tag) {
		return "", fmt.Errorf("%q is not a release tag (v1.2.3)", tag)
	}
	cache, _ := os.UserCacheDir()
	if cache == "" {
		cache = os.TempDir()
	}
	dir := filepath.Join(cache, "lostos-install", "payload", tag)
	if _, err := os.Stat(filepath.Join(dir, ".ok")); err == nil && hasPayload(dir) {
		return dir, nil
	}
	base := releasesURL() + "/download/" + tag
	sums, err := httpGetBig(base+"/SHA256SUMS", 1<<20)
	if err != nil {
		return "", fmt.Errorf("SHA256SUMS of %s: %v", tag, err)
	}
	want := parseSums(sums)[payloadAsset]
	if want == "" {
		return "", fmt.Errorf("the release %s has no %s: use the archive of the release or an older version", tag, payloadAsset)
	}
	fmt.Fprintf(out, "== files of the installer from GitHub (%s)\n", tag)
	b, err := httpGetBig(base+"/"+payloadAsset, 64<<20)
	if err != nil {
		return "", err
	}
	if sum(b) != want {
		return "", fmt.Errorf("%s does not match the SHA256SUMS of the release %s: nothing is installed from it", payloadAsset, tag)
	}
	_ = os.RemoveAll(dir)
	if err := unzipPayload(b, dir); err != nil {
		_ = os.RemoveAll(dir)
		return "", err
	}
	if !hasPayload(dir) {
		return "", errors.New("the archive of the release has no satellite/deploy")
	}
	return dir, os.WriteFile(filepath.Join(dir, ".ok"), []byte(tag), 0o644)
}

// unzipPayload: every file goes under dir; a name that leaves it is refused. The files are made for the speaker (adb push keeps the mode), so scripts and programs are executable.
func unzipPayload(data []byte, dir string) error {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return err
	}
	base, _ := filepath.Abs(dir)
	var total int64
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, "/") {
			continue
		}
		dst := filepath.Join(base, filepath.FromSlash(f.Name))
		if dst != base && !strings.HasPrefix(dst, base+string(os.PathSeparator)) {
			return fmt.Errorf("a path that leaves the folder in the archive: %s", f.Name)
		}
		if total += int64(f.UncompressedSize64); total > 256<<20 {
			return errors.New("the archive is larger than expected")
		}
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		w, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
		if err != nil {
			rc.Close()
			return err
		}
		_, err = io.Copy(w, rc)
		rc.Close()
		w.Close()
		if err != nil {
			return err
		}
	}
	return nil
}
