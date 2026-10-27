# Making a release

1. Make sure `main` is what you want to ship (the tests of `installer/` pass).
2. Tag it and push the tag: `git tag v0.4.0 && git push origin v0.4.0` (a tag with a dash, `v0.4.0-rc1`, becomes a pre-release).
3. The workflow *Release* builds `lostos-armhf.bin` and `lostos-aarch64.bin`, writes `SHA256SUMS` and the installers, and creates a **draft** release. Read it, then publish.
4. From that moment `lostos-install` (it asks `releases/latest`) and every speaker with automatic update switched on take it. `lostos-install --version v0.3.1` pins an older release, `--offline` uses only the package.

Speakers and the installer check a binary against the `SHA256SUMS` of the same release; there is no separate signature for the firmware. Keep two-step login on for everybody who can push, and protect the tags `v*`
(Settings, Rules, Rulesets) so that a release can not be replaced afterwards. Plugins are different: they are signed, see the repository `lostos-plugins`.
