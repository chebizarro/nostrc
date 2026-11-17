# fipsd packaging

Packaging for the nostrc C port of the FIPS daemon, mirroring upstream
fips's `packaging/` where the daemon's shape is the same and nostrc's own
release tooling where it is not. Outputs go to `build-deb/`, `build-rpm/`,
`build-pkg/` and `build-dist/` at the repository root.

```sh
make -C fipsd/packaging deb            # Debian/Ubuntu .deb on this host
make -C fipsd/packaging deb-container  # the same in the pinned ubuntu:24.04 image (docker/podman)
make -C fipsd/packaging rpm            # Fedora .rpm on a host with rpmbuild
make -C fipsd/packaging rpm-container  # the same in the pinned Fedora image
make -C fipsd/packaging pkg            # macOS .pkg installer
make -C fipsd/packaging archive        # fipsd-<version>.tar.gz release source archive
make -C fipsd/packaging version        # the declared version, and this tree's Debian version
make -C fipsd/packaging check          # shellcheck over the packaging shell, plutil over the plist
```

## One payload, declared once

Every format stages the daemon with `cmake --install <build>/fipsd`: the
install rules at the end of `fipsd/CMakeLists.txt` are the list of what a
package ships, and the recipes (`debian/rules`, `rpm/fipsd.spec`,
`arch/PKGBUILD`, `macos/build-pkg.sh`) only name files. The payload:

| Installed | From | Notes |
| --------- | ---- | ----- |
| `/usr/bin/fips-gateway`, `/usr/lib/systemd/system/fips-gateway.service` | `src/gateway/`, `systemd/fips-gateway.service.in` | Linux only; separate Debian/RPM `fips-gateway`, bundled in Arch `fipsd` as upstream does. Opt-in, never started or enabled by installation. |
| `/usr/bin/fipsd` (+ `fipsctl` when its target exists) | `fipsd/`, `src/fipsctl/` | `fipsctl` is installed by a deferred `if(TARGET fipsctl)`. |
| `/usr/lib/<triplet>/libfipsnative.so.0`, `.so`, `.a`, headers, `.pc` | `src/native/client.c`, `include/` | Debian: `libfipsnative0` + `libfipsnative-dev`; RPM: `libfipsnative` + `-devel`. |
| `/usr/lib/<triplet>/fipsd/libnostr-json.so.2` | `libjson/` | Private copy with an RPATH, as the desktop apps ship it: `libjson/` builds shared only, and the daemon must not depend on or collide with the login stack's public `libnostr-json2`. |
| `/usr/share/fips/fips.yaml.example` | `common/fips.yaml` | Seeded to `/etc/fips/fips.yaml` (0600) on first install, never overwritten. Not under `/usr/share/doc`, which minimal installs path-exclude. |
| `/etc/fips/hosts`, `/etc/fips/fips.nft`, `/etc/fips/fips.d/` | `common/` | Conffiles (dpkg automatic, `%config(noreplace)`, `backup=`). `fips.nft` is carried unchanged from upstream; macOS drops it. |
| `/usr/lib/systemd/system/{fipsd,fips-dns,fips-firewall}.service` | `systemd/*.service.in` | Generated with the install prefix; see [systemd/README.md](systemd/README.md). |
| `/usr/lib/tmpfiles.d/fipsd.conf`, `/usr/lib/sysusers.d/fipsd.conf` | `common/fips.tmpfiles`, `common/fips.sysusers` | `/run/fips` 0750 root:fips; the `fips` group. |
| `/usr/lib/fips/fips-dns-{setup,teardown}` | `common/` (a sibling task's) | Installed when present; `fips-dns.service` is skipped when not. |

The `fips` group gates the control socket (`/run/fips/control.sock`, 0770)
and the native-API socket; the daemon sets their group and mode itself
(`fipsd/src/privilege.c`) and runs as root for the TUN device. Every package
creates the group (`groupadd --system` in the maintainer scripts, plus the
sysusers fragment) and the Debian purge removes it with `/etc/fips`.

## Versions

`fipsd/packaging/common/version.sh` prints the one version every recipe
reads: `declare_component_version(FIPSD M m p)` in `fipsd/CMakeLists.txt`,
the source `VERSION_MANIFEST.md` names as authoritative and the value
`scripts/tag_release.py fipsd --cache <build>/CMakeCache.txt` turns into the
`fipsd-vM.m.p` tag. `scripts/test-release-packaging.py`
(`FipsdPackagingVersionTests`) checks that `debian/changelog`, `rpm/fipsd.spec`,
`arch/PKGBUILD` and the manifest row agree with it, so a bump that misses one
cannot ship.

`version.sh --deb` is the Debian version for the current tree: `M.m.p-1` at
the `fipsd-vM.m.p` tag, otherwise `M.m.p~git<date>.<sha>[.dirty]-1`, which
dpkg orders before the release the tree is heading for. The workflow and
`build-deb-container.sh` append `~ubuntu24.04`: one deb per distribution
release, as the Groundhog release names them. An exported tree (no `.git`)
still embeds its revision in `fipsd -V` through the `FIPSD_GIT_REV` CMake
cache variable, which `build-deb.sh` sets from the tree it exported.

Releases: `scripts/make-desktop-source-archive.sh <version> dist fipsd` writes
`fipsd-<version>.tar.gz` with the submodules (the RPM spec's `Source0` and the
PKGBUILD's `source=` name that asset under the `fipsd-v<version>` release),
and `.github/workflows/fips-packaging.yml` builds, installs and smoke-tests
every format on a `fipsd-v*` tag or by dispatch.

## Debian / Ubuntu

`debian/` is a debhelper 13 recipe for four binary packages: `fipsd`,
`fips-gateway`, `libfipsnative0`, `libfipsnative-dev`. The opt-in gateway
package depends on the same-version `fipsd`, `iproute2` and the libnftables
runtime derived by `dpkg-shlibdeps` (Ubuntu: `libnftables1`).
`build-deb.sh` copies the tree to a scratch directory with this directory as `debian/` (the shape of
`scripts/build-desktop-deb.sh`), runs `dpkg-buildpackage -b -us -uc`, then
`check-deb-depends.sh`, `check-glibc-floor.sh` and `check-deb-revision.sh`
over the result. `rules` configures with `common/cmake-args.sh` (the daemon
and libnostr with its GLib layer, no GObject/GTK wrappers, no tests), builds `fipsd/all` only and installs
from `fipsd/` only.

Install behaviour: `fipsd.service` is enabled and started on install and
restarted on upgrade (debhelper's default); `fips-dns.service` is enabled but
not started inside apt, since `fips-dns-setup` rewrites the host resolver;
`fips-firewall.service` is neither. `remove` keeps `/etc/fips` (config and
identity keys) and drops the `.fips` DNS routing the helpers may have
written; `purge` removes `/etc/fips`, `/run/fips` and the group.

```sh
sudo apt install ./build-deb/fipsd_*.deb ./build-deb/libfipsnative0_*.deb
sudo fipsd/packaging/testing/smoke-systemd.sh     # start, query the socket, stop
sudo apt purge fipsd libfipsnative0
```

Install the gateway with `sudo apt install ./build-deb/fips-gateway_*.deb`
alongside `fipsd`. Configure the shared `/etc/fips/fips.yaml` using
[the gateway deployment example](../docs/fips-gateway.md#deploy-deploy-gatewaymd),
enable IPv6 forwarding/proxy NDP, then
`sudo systemctl enable --now fips-gateway.service`. The gateway service stays
disabled until requested. Stop it before removing `fipsd`;
`sudo apt purge fips-gateway fipsd` also removes the gateway's tmpfs pool state.

## RPM

The spec also produces a separate `fips-gateway` subpackage, requiring the
same-version `fipsd`, `nftables` (libnftables runtime) and `iproute`.
Install both with `dnf install ./fipsd-*.aarch64.rpm ./fips-gateway-*.aarch64.rpm`
(use your native architecture), configure as above and explicitly enable the
service. Erasing the subpackage stops/disables the unit and drops its runtime
pool state; as with `fipsd`, RPM retains operator configuration.

`rpm/fipsd.spec` builds from the source archive on Fedora 42+, with nsync
bundled from `third_party/` (Fedora has no package; `--with system_nsync`
uses one). Same files, same group, same seeding and the same two units
enabled; `%postun` on the last erase removes `/run/fips` and the DNS routing
but keeps `/etc/fips`, since rpm has no purge. `fipsctl` and the DNS helpers
are listed through `--define "_fipsd_has_fipsctl 1"` /
`"_fipsd_has_dns_helpers 1"`, which `build-rpm.sh` sets from the tree.
Named `fipsd`: Fedora's `fips` is an unrelated FITS viewer owning
`/usr/bin/fips`, which the daemon does not touch.

## Arch Linux

`arch/PKGBUILD` builds from the release archive through CMake; the `fips`
group and `/run/fips` come from the sysusers/tmpfiles fragments through
pacman's hooks, and `/etc/fips/fips.yaml` ships seeded as a `backup=` file.
See [arch/README.md](arch/README.md).

## macOS

`macos/build-pkg.sh` builds a Release tree with prefix `/usr/local`, stages
it with `cmake --install`, adds `Library/LaunchDaemons/com.fips.fipsd.plist`,
`/etc/resolver/fips` (`::1` port 5354, the shipped `dns` settings) and the
install scripts, and writes an unsigned `.pkg` with `pkgbuild`. `postinstall`
seeds `/usr/local/etc/fips/fips.yaml` and `hosts` from the `.default` copies,
creates the `fips` group with `dscl`, adds the console user to it, and
bootstraps the job in the system domain. `macos/uninstall.sh` reverses it,
keeping config, keys and logs. A local `sudo installer -pkg ... -target /`
needs no signature; a distributed package wants `productsign`.

## Smoke tests

`testing/` holds what the workflow runs between install and uninstall:
`smoke-systemd.sh` (root, systemd as PID 1: start the unit, `fipsctl status`
or `control-query.py show_status`, check the socket's group and mode, stop),
`smoke-direct.sh` (no systemd, for containers: the packaged daemon from a
throwaway config with the TUN off), `smoke-launchd.sh` (macOS: the installed
system job as root, or a built binary in the user's launchd domain), and
`control-query.py`, the one-line JSON request the control socket speaks.
`sudo timeout 90 python3 testing/smoke-gateway.py` starts/stops the installed
gateway unit with the deployment example adapted to loopback and high DNS ports,
queries both control commands and checks nft table/route/socket teardown. It
restores the original config and sysctls. Run only on disposable runners.
`--direct` runs the same check in a fresh Fedora container with NET_ADMIN and
`/dev/net/tun`, without systemd. This proves packaged startup, not mesh forwarding;
the opt-in gateway netns suite covers forwarding.

## Checks

| Script | Catches |
| ------ | ------- |
| `check-deb-depends.sh <deb>...` | A `Depends` that differs from what `dpkg-shlibdeps` derives from the package's ELF objects, and a `Recommends` that drifted from `debian/control`. |
| `check-glibc-floor.sh <deb|binary>...` | A binary needing a newer glibc than `build-floor.env` declares for the release it is built for. |
| `check-deb-revision.sh <deb> <head> <state>` | A packaged `fipsd -V` naming another commit or dirty flag than the tree. |

All three refuse to report a pass they did not establish (exit 2).
