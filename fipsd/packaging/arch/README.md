# Arch Linux packaging (fipsd)

`PKGBUILD` builds `fipsd` (the daemon, its units, the `fips` group through
`sysusers.d`, `/run/fips` through `tmpfiles.d`, the seeded `/etc/fips/fips.yaml`)
and libfipsnative from the release source archive. It mirrors the Debian and
RPM recipes: everything comes from `cmake --install build/fipsd`.

```sh
cd fipsd/packaging/arch
makepkg -s                     # needs the fipsd-<version>.tar.gz release asset
sudo pacman -U fipsd-*.pkg.tar.zst
```

To build from a checkout rather than a release, write the archive first and
point the PKGBUILD at it:

```sh
scripts/make-desktop-source-archive.sh "$(sh fipsd/packaging/common/version.sh)" fipsd/packaging/arch fipsd
cd fipsd/packaging/arch && makepkg -s   # source= matches the local file name, so nothing is downloaded
```

`.github/workflows/fips-packaging.yml` runs this in an `archlinux` container
(x86_64) with the same archive, installs the package and smoke-tests it.

Publishing to the AUR follows `packaging/archlinux/README.md` (the gnostr
package): `makepkg --printsrcinfo > .SRCINFO`, then push `PKGBUILD`,
`fipsd.install` and `.SRCINFO` to `ssh://aur@aur.archlinux.org/fipsd.git`.
