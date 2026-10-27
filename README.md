# lostOS

The firmware of lostOS: a replacement of the voice stack of a rooted Yandex Station that keeps the speaker's own hardware (microphones, light, display, buttons) and lets it work with the lostOS server
(or Home Assistant as an ESPHome device). It also plays AirPlay 2 and Spotify Connect, has an equaliser, Bluetooth, and a sandbox for plugins that run on the speaker.

Not in this repository: the receivers (shairport-sync, librespot and their libraries) are large binaries with their own licenses: they come with the releases, not with the source.

## Models

| model | state |
|---|---|
| Station Mini 3 (`bergamot`) | works |
| Station Lite 2 (`plum`) | works (built in) |
| Station Midi (`cucumber`) | built in, not tried on a real speaker |
| Station Lite "Micro" (`station-micro`), Mini 2 (`station-mini2`) | profiles are made from firmware dumps, not tried |
| Station 2 (`station-midi`) | in preparation (an LED matrix on top, a pixel display in front) |

The list of models and their profiles is data (`satellite/models/catalog.json`, signed, and one `.conf` per model), so a new speaker is added by a file, not by changing the code: see `docs/models.md`.

## Installer

`installer/` is `lostos-install` (Go): it finds rooted Stations in your network, shows what it would do (`--dry-run`), installs, updates, checks (`--doctor`, `--fix`), takes everything back (`--uninstall`).
It needs root `adb` on the speaker; it does not give you root. Releases contain it built for Windows, macOS and Linux.

## Build

`satellite/Makefile` builds the daemon with zig for ARM (see `satellite/third_party/fetch.sh` for what it downloads).

License: MIT (the code of the pulsar project, which the first version is built on, is MIT as well).
