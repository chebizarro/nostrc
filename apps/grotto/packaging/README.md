# Grotto native packaging (Preview 0.1.0)

One `grotto` package contains GUI + daemon. Debian recommends
`gnome-shell-extension-nostr`; the optional indicator never authorizes signing.
Use GTK ≥4.14, libadwaita ≥1.5, GLib ≥2.80, zbar, libqrencode and GStreamer
core/video/appsink with base/good capture plugins. No hardware/multisig claim.
Standalone recipes disable unrelated libhanami configuration; it otherwise pulls
libgit2/curl/SQLite build dependencies into a signer-only package.

- **Debian/Ubuntu**: copy `debian/` to a complete monorepo source root's
  `debian/`, then `dpkg-buildpackage -us -uc -b`. CMake builds
  `apps/grotto/all` and installs only `build/apps/grotto`; BUILD_TESTING is off.
  GUI and daemon are not split. Breaks/Replaces handles the old 0.1.0-1 split.
- **RPM**: `rpm/grotto.spec`; use a complete local `grotto-0.1.0.tar.gz`
  with root `grotto-0.1.0/`, including checked-out submodules, not GitHub's
  incomplete source archive. Build with `rpmbuild -ba`. The dependency floor,
  not the historical Fedora 38/RHEL 9 claim, determines compatibility.
- **Arch**: `archlinux/PKGBUILD` consumes the same local complete source archive.
  Run `updpkgsums` before distributing it; SKIP is only for local development.
  The legacy desktop bundle recipe under `packaging/archlinux/groundhog` is maintained
  too; its Groundhog source release must be refreshed before it can contain G12.
  It still uses the shared bundle package version, not Grotto's independent runtime
  version. Do not override pkgver inside a split function: [Arch's split-package
  contract](https://man.archlinux.org/man/alpm-split-package.7.en) shares that metadata.
  Use the standalone recipe for independently versioned Grotto; transitioning old
  bundle installations needs an explicit release/version policy, not a silent downgrade
  (`nostrc-u6v55.15`; owner choice requested in G12). The Debian desktop-bundle
  recipe likewise retains its source-release package version.
- **Nix**: from the root, `nix build '.?submodules=1#grotto'`. The legacy
  `#grotto-daemon` output aliases this same GUI+daemon package. The nonfunctional
  0.0.0 TCP-only derivation is removed. The wrapped ELF path, not its shell wrapper,
  is compiled into the approver allowlist; verify attestation after installation.
  This store-path configuration is not permission to weaken caller checks.

RPM/Arch/Nix recipes require their own installed-platform validation; updating a
recipe does not mean those native package builds were run. Both CMake and the
alternate Meson GUI install canonical desktop/GUI service/metainfo/icons, schema,
autostart **template**, user unit and network examples. Meson still consumes
CMake-built libraries/daemon; it is not an independent daemon implementation.
`signer/desktop-contract` checks this contract. No package enables GUI login or
remote network access on install. The native host, when built, uses its own
manifest installer; Nix/desktop-bundle builds currently leave that host off.

Systemd examples install under `/usr/share/doc/grotto/examples`; use exactly one
of `bunker.conf.example` and `headless-bunker.conf.example` as a user drop-in.
The base unit is localhost-only. See [deployment](../DAEMON_DEPLOYMENT.md).

**Flatpak and AppImage are experimental and release-blocked.** Required gates:
host caller attestation; trusted approver identification; Secret-portal custody;
activation ownership; browser-host behavior. See [the gate details](flatpak/README.md).
AppImage bundling stays deferred (`nostrc-h3kh`); no release artifact is promised.
Never substitute the legacy AppImage desktop file for the native canonical file.

No Grotto release tag is claimed. Packaging/build corrections fold into the initial
unreleased 0.1.0. Preview removal is a separate owner decision after automated,
installed-session and accessibility evidence, not an effect of installing a package.
