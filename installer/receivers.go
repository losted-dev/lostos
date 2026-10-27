package main

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// A receiver is a system plugin of lostOS that brings programs of its own to the station: AirPlay 2 or Spotify Connect. The programs come in
// the release archive (receivers/<arch>/<id>/); the owner chooses in the installer which of them the station gets, one, both or none.
type receiver struct {
	ID, Short, Name, About string
	Main                   string // the program that shows the receiver is there
}

var receiverList = []receiver{
	{"ru.lostos.airplay", "airplay", "AirPlay 2", "Колонка появится в списке AirPlay на iPhone, iPad и Mac: музыка, громкость с телефона, название трека.", "shairport-sync-ap2"},
	{"ru.lostos.spotify", "spotify", "Spotify Connect", "Колонка в списке устройств Spotify на телефоне. Нужен аккаунт Spotify Premium.", "librespot"},
}

// receiverInfo is what the wizard shows for one receiver of a station.
type receiverInfo struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	About     string `json:"about"`
	Installed bool   `json:"installed"`
}

func receiverSource(arch, id string) string { return filepath.Join(root, "receivers", arch, id) }

// receiversFor: the receivers whose programs for this userland are in the archive.
func receiversFor(arch string) []receiver {
	var out []receiver
	for _, r := range receiverList {
		if fi, err := os.Stat(filepath.Join(receiverSource(arch, r.ID), r.Main)); err == nil && fi.Size() > 0 {
			out = append(out, r)
		}
	}
	return out
}

func receiverInfos(s, arch string) []receiverInfo {
	var infos []receiverInfo
	for _, r := range receiversFor(arch) {
		_, err := sh(s, "[ -x "+dataDir+"/plugins/"+r.ID+"/"+r.Main+" ]")
		infos = append(infos, receiverInfo{r.ID, r.Name, r.About, err == nil})
	}
	return infos
}

// parseReceivers: --receivers all | none | airplay,spotify -> the ids of the plugins.
func parseReceivers(v string) ([]string, error) {
	v = strings.ToLower(strings.TrimSpace(v))
	switch v {
	case "", "none":
		return nil, nil
	case "all":
		var ids []string
		for _, r := range receiverList {
			ids = append(ids, r.ID)
		}
		return ids, nil
	}
	var ids []string
	for _, w := range strings.Split(v, ",") {
		w = strings.TrimSpace(w)
		found := false
		for _, r := range receiverList {
			if w == r.Short || w == r.ID {
				ids, found = append(ids, r.ID), true
			}
		}
		if !found {
			return nil, fmt.Errorf("--receivers: %q is not airplay, spotify, all or none", w)
		}
	}
	return ids, nil
}

// selectReceivers decides what this station gets: what was asked (--receivers, the wizard), else everything with --yes, else the owner is asked.
func selectReceivers(st station) []string {
	avail := receiversFor(st.arch)
	var ids []string
	switch {
	case opts.receiversSet:
		for _, id := range opts.receivers {
			for _, r := range avail {
				if r.ID == id {
					ids = append(ids, id)
				}
			}
		}
	case opts.yes || opts.dryRun:
		for _, r := range avail {
			ids = append(ids, r.ID)
		}
	default:
		for _, r := range avail {
			ans := strings.ToLower(readLine(fmt.Sprintf("%s: install %s? %s [Y/n] ", st.serial, r.Name, "(its programs go to /data/lostos/plugins)")))
			if ans == "" || ans == "y" || ans == "yes" || ans == "д" || ans == "да" {
				ids = append(ids, r.ID)
			}
		}
	}
	if len(avail) == 0 {
		fmt.Fprintf(out, "%s: AirPlay and Spotify are not in this archive for %s yet\n", st.serial, st.arch)
	}
	return ids
}

func receiverFiles(arch, id string) []string {
	var files []string
	_ = filepath.WalkDir(receiverSource(arch, id), func(p string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() {
			rel, _ := filepath.Rel(receiverSource(arch, id), p)
			files = append(files, filepath.ToSlash(rel))
		}
		return nil
	})
	sort.Strings(files)
	return files
}

// receiversKB: the room the chosen receivers (and the equaliser and sounds that come with them) take on /data.
func receiversKB(arch string, ids []string) int {
	if len(ids) == 0 {
		return 0
	}
	kb := int(dirKB(filepath.Join(root, "receivers", arch, "audio")))
	for _, id := range ids {
		kb += int(dirKB(receiverSource(arch, id)))
	}
	return kb
}

func dirKB(dir string) int64 {
	var n int64
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() {
			if fi, e := d.Info(); e == nil {
				n += fi.Size()
			}
		}
		return nil
	})
	return n / 1024
}

// pushChanged copies one file when the station has another one (or none): to name.new, checked by its sha-256, then renamed over the old one
// (a running program keeps its old file until it restarts). Returns whether the file was replaced.
func pushChanged(s, local, remote string, mode string) (bool, bool) {
	want := sha256File(local)
	have, _ := sh(s, "[ -f "+remote+" ] && sha256sum "+remote)
	if want != "" && want == firstField(have) {
		return false, true
	}
	if push(s, local, remote+".new") != nil {
		return false, false
	}
	got, _ := sh(s, "sha256sum "+remote+".new")
	if want != firstField(got) {
		_ = shTo(out, s, "rm -f "+remote+".new")
		fmt.Fprintf(errw, "%s: %s was corrupted on the way (sha256 mismatch), not replaced\n", s, filepath.Base(local))
		return false, false
	}
	return true, shTo(out, s, "chmod "+mode+" "+remote+".new && mv "+remote+".new "+remote) == nil
}

// deployReceivers puts the chosen receivers on the station: the programs into /data/lostos/plugins/<id>/ and, with them, the equaliser and the
// short sounds of the AirPlay switch. lostos itself starts and watches them (they are its system plugins). A receiver that runs while its programs
// are replaced is stopped first, lostos starts it again after the restart that follows. The switch of the owner (enabled=0 in plugin.conf) stays.
func deployReceivers(s string, st station, ids []string) bool {
	if len(ids) == 0 {
		fmt.Fprintf(out, "== %s: no receivers chosen (AirPlay, Spotify can be added by running the installer again)\n", s)
		return true
	}
	fmt.Fprintf(out, "== %s: receivers\n", s)
	audio := filepath.Join(root, "receivers", st.arch, "audio")
	if shTo(out, s, "mkdir -p "+dataDir+"/alsa "+dataDir+"/sounds "+dataDir+"/plugins") != nil {
		return false
	}
	for _, f := range []struct{ name, dir, mode string }{
		{"libasound_module_pcm_lostoseq.so", "alsa", "644"}, {"air_on.wav", "sounds", "644"}, {"air_off.wav", "sounds", "644"},
	} {
		local := filepath.Join(audio, f.name)
		if _, err := os.Stat(local); err != nil {
			continue
		}
		if _, ok := pushChanged(s, local, dataDir+"/"+f.dir+"/"+f.name, f.mode); !ok {
			return false
		}
	}
	for _, id := range ids {
		dir := dataDir + "/plugins/" + id
		fmt.Fprintf(out, "%s: %s\n", s, id)
		files := receiverFiles(st.arch, id)
		changedAny := false
		stopped := false
		for _, rel := range files {
			local := filepath.Join(receiverSource(st.arch, id), filepath.FromSlash(rel))
			remote := dir + "/" + rel
			if strings.Contains(rel, "/") {
				if shTo(out, s, "mkdir -p "+dir+"/"+rel[:strings.LastIndex(rel, "/")]) != nil {
					return false
				}
			} else if shTo(out, s, "mkdir -p "+dir) != nil {
				return false
			}
			have, _ := sh(s, "[ -f "+remote+" ] && sha256sum "+remote)
			if !changedAny && firstField(have) != sha256File(local) { // the first file that differs: the receiver must not run on files that are replaced under it
				changedAny = true
				if !stopped {
					_ = shTo(out, s, "[ -x "+dataDir+"/lostos ] && "+dataDir+"/lostos --system stop "+id+" >/dev/null 2>&1; sleep 2; true")
					stopped = true
				}
			}
			if _, ok := pushChanged(s, local, remote, "755"); !ok {
				return false
			}
		}
		// plugin.conf only when there is none: what the owner switched stays
		if shTo(out, s, "[ -f "+dir+"/plugin.conf ] || printf 'kind=system\\nname="+receiverName(id)+"\\nenabled=1\\n' > "+dir+"/plugin.conf") != nil {
			return false
		}
	}
	return shTo(out, s, "sync") == nil
}

func receiverName(id string) string {
	for _, r := range receiverList {
		if r.ID == id {
			return r.Name
		}
	}
	return id
}
