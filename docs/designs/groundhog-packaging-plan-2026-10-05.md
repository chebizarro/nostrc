# Groundhog alpha packaging plan (W30)

**Owner decision (2026-10-05):** ship the alpha as Flatpak, AppImage, Debian/Ubuntu
`.deb`, Fedora RPM, Arch and Nix packages ("etc."). The signer is renamed
**Grotto** (`org.nostr.Grotto`, W31, nostrc-8otj) and gets its own UX pass
before it is promoted, but Groundhog cannot work without a signer, so every
Groundhog package has to bring one along. Tracking: nostrc-xxv6.

## Where we start

Groundhog today has CMake install rules (binary, desktop file, D-Bus service,
metainfo, GSettings schema, one 512px icon) and nothing else: it is absent from
every packaging recipe in the tree.

| Asset in the tree | Covers | Reusable for Groundhog |
|---|---|---|
| `apps/gnostr-signer/packaging/flatpak/org.gnostr.Signer.yml` (GNOME 47 runtime; modules libsecp256k1, libsodium, nsync, jansson, flatcc, libwebsockets) | signer | module list is the common base |
| `.github/workflows/gnostr-appimage.yml` (linuxdeploy + GTK plugin, Ubuntu 24.04) | gnostr | recipe shape |
| `debian/` unified source package (`BUILD_APPS=OFF`, headless stack) | libnostr, login stack, relayd | add binary packages |
| `packaging/rpm/nostr-login.spec` | headless stack | subpackages; nsync still unresolved on Fedora (nostrc-dd5y) |
| `packaging/archlinux/PKGBUILD` (`gnostr`, `gnostr-signer`) | gnostr, signer | split package |
| `flake.nix` (signer daemon only) | signer daemon | add outputs |
| `snap/*/snapcraft.yaml` | gnostr, signer, daemon | optional |
| `.github/workflows/release.yml` (component tags `<name>-v<version>`) | gnostr, signer | add a `groundhog` component |

Runtime requirements that constrain the targets: GTK >= 4.14, libadwaita >= 1.5,
libsoup 3, json-glib, libsecret, libsodium, sqlcipher, GStreamer >= 1.20 (voice;
Opus/Ogg plus AAC/MP4 decode from "bad"/libav), libxml2 (W29 link previews), and
the in-tree libraries (libnostr, nostr-gobject, libmarmot, nostrdb, nsync).
That means Ubuntu 24.04+, Debian 13+, Fedora 40+, current Arch and nixpkgs
unstable/24.11+.

## Sequencing

1. **Rename first (W31), package second.** Publishing `gnostr-signer` packages
   and renaming them a week later means transitional packages, Flatpak
   end-of-life redirects and AUR renames. The mechanical rename is days of
   work; package names are forever. Recipes below already use `grotto`.
2. **Groundhog-only groundwork can proceed now** (it does not depend on the
   signer's name): install-rule audit, icons at all hicolor sizes, metainfo to
   Flathub quality, release-workflow component, version tag.
3. Then the targets, in the order that gives users something soonest:
   Flatpak, AppImage, `.deb`, RPM, Arch, Nix.

## Groundwork (target-independent)

- **G1 install audit.** `cmake --install` into a staging prefix with
  `BUILD_TESTING=OFF` (a testing build refuses to install: it carries the
  test-only D-Bus interface) and compare against the hicolor/AppStream/desktop
  specs: scalable + symbolic app icon, 48/64/128/256px rasters, autostart file,
  `gschemas`, translations (none yet), licence file.
- **G2 metainfo.** `appstreamcli validate --strict` and Flathub's linter:
  screenshots (hosted, light and dark), `<content_rating>`, `<branding>`,
  `<launchable>`, release entry for 0.12.0 with a date, developer id
  (`org.nostr`), URLs. Screenshots come from the GUI tests' screenshot hooks.
- **G3 signer coupling.** Decide per target how Groundhog gets its signer:
  package dependency (`Depends: grotto`), bundled module (Flatpak, AppImage),
  or D-Bus activation of a separately installed app. Groundhog's onboarding
  must say what to install when no signer answers (it already has a "Waiting
  for Nostr Signer" page; the copy needs the distro-specific hint).
- **G4 release mechanics.** `groundhog-v0.12.0` tag; `release.yml` gains a
  `groundhog` component with one job per target, each uploading an artifact
  and running an install-and-launch smoke test (`groundhog --smoke`, which
  exists for exactly this) on a clean image.
- **G5 reproducible inputs.** Submodules (`third_party/nostrdb`, `nsync`) must
  be in the source archive `release.yml` builds, or vendored for Flatpak.

## Targets

### Flatpak (primary)
- Manifest `gnome/groundhog/packaging/flatpak/org.nostr.Groundhog.yml`,
  GNOME 47 runtime (48 when it ships libadwaita we need nothing newer from).
  Modules: libsecp256k1, libsodium, sqlcipher, nsync, jansson, libwebsockets,
  then the monorepo with `-DBUILD_GROUNDHOG=ON -DBUILD_TESTING=OFF`.
- Permissions: wayland + fallback-x11, ipc, network, pulseaudio (voice),
  `--talk-name=org.freedesktop.secrets`, `--talk-name=org.nostr.Signer`,
  notifications and the file-chooser/background portals. No home access:
  attachments go through the portal.
- Signer: **separate Flatpak** `org.nostr.Grotto` that owns
  `org.nostr.Signer`; Groundhog talks to it over the session bus. Bundling the
  signer inside Groundhog's sandbox would give two apps two key stores.
- Distribution: our own Flatpak remote first (GitHub Pages + `flat-manager`
  or a static OSTree repo from CI), Flathub submission once G2 passes and
  Grotto's UX pass is done. Flathub wants a tagged release and no network at
  build time.
- Test: `flatpak-builder --install` in the Ubuntu 24.04 VM and Fedora container,
  `flatpak run org.nostr.Groundhog --smoke`.

### AppImage
- Same recipe shape as `gnostr-appimage.yml`: install to `AppDir/usr`,
  `linuxdeploy` with the GTK plugin, bundle GStreamer plugins and the
  GSettings schema. Built on Ubuntu 24.04 for x86_64 and aarch64.
- Signer: bundle `grotto` and `grotto-daemon` in the same AppImage and start
  the daemon on demand (the AppImage's `AppRun` exports a private
  `XDG_DATA_DIRS` entry with the D-Bus service file), since an AppImage user
  has no package manager to pull a dependency.
- No auto-update in the alpha; `zsync` metadata later.

### Debian / Ubuntu
- Add binary packages `groundhog` and `grotto` (+ `grotto-daemon`) to the
  unified `debian/` source package; the build currently passes
  `BUILD_APPS=OFF`/`BUILD_NOSTR_GOBJECT=OFF`, so the GUI stack needs its own
  configure pass or the flags flipped with the extra Build-Depends
  (libgtk-4-dev, libadwaita-1-dev, libsoup-3.0-dev, libsecret-1-dev,
  libsqlcipher-dev, libgstreamer1.0-dev, blueprint-compiler, libxml2-dev).
- `groundhog` Depends on `grotto`, Recommends the GStreamer plugin packages.
- Distribution: an apt repository (signed, GitHub Pages or Cloudsmith) for
  Ubuntu 24.04/24.10 and Debian 13; a PPA is optional.
- Test: `dpkg-buildpackage` in clean Docker images, `lintian`, install +
  `groundhog --smoke` under Xvfb.

### Fedora RPM
- Subpackages `groundhog` and `grotto` in a GUI spec beside
  `nostr-login.spec` (or the same spec with a `%bcond gui`).
- Blocked on nsync for Fedora (nostrc-dd5y): vendor `third_party/nsync` as a
  bundled static library for the RPM build, declared with
  `Provides: bundled(nsync)`.
- Distribution: COPR for Fedora 40/41/42.

### Arch
- Extend the split PKGBUILD: `groundhog`, `grotto` (replaces/provides
  `gnostr-signer`). Publish `groundhog-git` and a tagged `groundhog` to the
  AUR.

### Nix
- `flake.nix` gains `packages.groundhog` and `packages.grotto` (wrapGAppsHook4,
  GStreamer plugin path), plus a NixOS module is out of scope for the alpha.

### Not in the alpha
- macOS `.dmg` (needs notarization and the Keychain path hardened:
  nostrc-2hmd) and Windows. Snap only if someone asks.

## What building it showed (2026-10-05)

Built and installed on Ubuntu 24.04 x86_64 (the `gnome-dev` VM). These
findings replace the assumptions in the target sections above where they
differ.

- **An app-only install was incomplete.** Groundhog, `grotto` and
  `grotto-daemon` load `libnostr-json.so.1` at run time; every other in-tree
  library is static. `cmake --install <build>/gnome/groundhog` left it behind.
  Fixed: each app installs a private copy in `<libdir>/<app>/` with a
  relative RUNPATH (`cmake/NostrcPrivateJsonLib.cmake`).
- **Debian: a separate recipe, not more packages in `debian/`.** The
  top-level recipe configures the headless flavour (`NOSTR_WITH_GLIB=OFF`,
  shared libraries); the desktop apps need the GLib flavour, linked
  statically. `packaging/debian-desktop/` builds `groundhog` and `grotto`
  (app + background service in one package) through
  `scripts/build-desktop-deb.sh`. Installed and exercised: all three bus names
  are activatable, a call to `org.nostr.Signer` starts `grotto-daemon`,
  `groundhog --smoke` passes.
- **Flatpak: GNOME 51, not 47.** Flathub now carries runtimes 49, 50 and 51;
  47 is past end of life. 51 has the GTK 4.24 / libadwaita 1.10 Groundhog is
  developed against. The SDK has no `tclsh` (sqlcipher needs it to build), and
  nsync, jansson and libwebsockets need small flags to configure with its
  CMake and compiler. The older Grotto manifest pinned commits that do not
  match their tags and was never built.
- **The signer cannot live in a sandbox or a moving mount yet
  (nostrc-mevv).** The daemon identifies callers through `/proc` and trusts
  its approval UI by absolute path and inode. Inside a Flatpak it cannot
  resolve host PIDs, and Flatpak will not export a service file for
  `org.nostr.Signer` from an app called `org.nostr.Grotto`; inside an AppImage
  the mount path changes on every run. So for the alpha: **Groundhog's
  Flatpak talks to a natively installed Grotto**, there is **no Grotto
  Flatpak**, and the **AppImage waits** for that design.
- **First run needs Grotto open by hand (nostrc-sh5h).** The daemon rejects
  any request that needs approval unless the Grotto app is already running,
  and the app started as a D-Bus service does not listen for requests. This
  is independent of packaging and is the main obstacle to a first run that
  just works.
- **Package metadata contact.** Debian, RPM and AUR recipes carry the
  maintainer the owner chose: Biz <chebizarro@protonmail.com>.

## Risks

- **The signer is the product's front door and is not ready.** Mitigation:
  rename now, ship it as "Grotto (preview)" alongside Groundhog, and gate
  Flathub submission on its UX pass.
- **Voice codecs.** AAC/MP4 decode from White Noise needs GStreamer "bad" or
  libav; Flatpak's runtime has `org.freedesktop.Platform.ffmpeg-full`, Fedora
  ships without patented codecs. Groundhog already degrades honestly when a
  plugin is missing; the package descriptions must say what to install.
- **No CI minutes are spent on packaging yet.** Each target adds a job; keep
  them on tags and manual dispatch, not on every push.
