# Groundhog and Grotto for Arch Linux (x86_64 and aarch64)

`PKGBUILD` here builds two packages, `groundhog` and `grotto`, from the
release source archive. Each Groundhog release (`groundhog-v*` tag,
`.github/workflows/groundhog-release.yml`) ships them for both
architectures:

| Architecture | Built on | Files |
|---|---|---|
| x86_64 | `archlinux:latest` | `groundhog-<ver>-1-x86_64.pkg.tar.zst`, `grotto-<ver>-1-x86_64.pkg.tar.zst` |
| aarch64 | Arch Linux ARM (`menci/archlinuxarm`, pinned by digest) on an arm64 runner | `groundhog-<ver>-1-aarch64.pkg.tar.zst`, `grotto-<ver>-1-aarch64.pkg.tar.zst` |

Both jobs run `scripts/ci/build-arch-package.sh`, which you can run by hand
in either kind of container:

```sh
scripts/make-desktop-source-archive.sh 0.0.0-local /tmp/gh/src
docker run --rm --platform linux/arm64 -v /tmp/gh:/w -v "$PWD/scripts/ci:/ci:ro" -w /w \
  menci/archlinuxarm:base-devel sh /ci/build-arch-package.sh src/groundhog-0.0.0-local.tar.gz dist
```

Install with `pacman -U groundhog-*.pkg.tar.zst grotto-*.pkg.tar.zst`
(`grotto` is only needed for local, Grotto-backed accounts).

## Arch Linux ARM on Android (Termux proot-distro, chroot)

The aarch64 packages install and run in an Arch Linux ARM root on Android,
e.g. `proot-distro install archlinux` in Termux, or a rooted chroot. Nothing
in either package needs systemd: there are no install scriptlets, and the
systemd user unit for `grotto-daemon` is shipped but optional. What such a
root lacks, and what to do instead:

- **No systemd user session, no session bus.** Start one yourself and run
  everything inside it:
  `dbus-run-session -- groundhog` (or `eval "$(dbus-launch --sh-syntax)"`
  once per shell). Groundhog reaches Grotto over that bus; the bus starts
  `grotto-daemon` on demand through its D-Bus service file, whose
  `SystemdService=` line is ignored when there is no systemd.
- **Secret Service.** Groundhog keeps its database key, and Grotto its keys,
  in a Secret Service provider. Install `gnome-keyring` and start it inside
  the same session, e.g.
  `dbus-run-session -- sh -c 'echo -n "$PASS" | gnome-keyring-daemon --unlock --components=secrets; groundhog'`.
- **Display.** Use a Termux:X11 or VNC display (`export DISPLAY=:0`), or
  Wayland where your setup provides one. Software rendering is usual:
  `GSK_RENDERER=cairo` (or `ngl` if the GPU path works) avoids blank windows
  under proot.
- **Audio and voice messages.** GStreamer plugins come from the packages'
  dependencies (`gst-plugins-base`, `gst-plugins-good`); add
  `gst-plugins-bad`/`gst-libav` for AAC. Sound needs a PulseAudio server
  reachable from the root (Termux `pulseaudio` with
  `PULSE_SERVER=127.0.0.1`).
- **pacman sandbox.** pacman 7 downloads as a Landlock-sandboxed user, which
  proot and some kernels cannot provide. If `pacman` fails with a Landlock or
  "switching to sandbox user" error, add `DisableSandbox` under `[options]`
  in `/etc/pacman.conf`.
