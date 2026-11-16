#!/usr/bin/env bash
# Build a macOS .pkg installer for fipsd.
#
#   fipsd/packaging/macos/build-pkg.sh [--output-dir DIR] [--build-dir DIR]
#                                     [--version V] [--no-build]
# Output: build-pkg/fipsd-<version>-macos-<arch>.pkg (unsigned)
#
# Configures and builds the daemon (Release, prefix /usr/local), stages it
# with `cmake --install <build>/fipsd` -- the same payload the Linux packages
# ship, minus the nftables file, which is Linux-only -- and adds the launchd
# daemon plist, a /etc/resolver/fips entry for the .fips domain, and the
# install scripts that seed the config, create the fips group and load the
# job. The payload is what fipsd/CMakeLists.txt declares, so a `make install`
# to /usr/local and this package put the same files in the same places.
#
# Prerequisites: Xcode command-line tools (pkgbuild), and the build
# dependencies .github/workflows/fips-ci.yml installs with brew. The package
# is not signed or notarized: `sudo installer -pkg ... -target /` installs it
# locally (Gatekeeper gates downloaded packages, not local ones); signing is
# `productsign --sign "Developer ID Installer: ..."` over the result.
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
here=$(cd "$(dirname "$0")" && pwd)
out=$root/build-pkg
build=$root/build-pkg/build
version=""
do_build=1
while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir) out=${2:?missing value for --output-dir}; shift 2 ;;
        --build-dir) build=${2:?missing value for --build-dir}; shift 2 ;;
        --version) version=${2:?missing value for --version}; shift 2 ;;
        --no-build) do_build=0; shift ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "build-pkg: unknown option $1" >&2; exit 2 ;;
    esac
done
[ "$(uname -s)" = Darwin ] || { echo "build-pkg: macOS only" >&2; exit 2; }
command -v pkgbuild >/dev/null 2>&1 || { echo "build-pkg: pkgbuild (Xcode command-line tools) is required" >&2; exit 2; }
[ -n "$version" ] || version=$(sh "$root/fipsd/packaging/common/version.sh" "$root")
arch=$(uname -m)
mkdir -p "$out"
out=$(cd "$out" && pwd)

rev=""
if git -C "$root" rev-parse --verify --quiet HEAD >/dev/null 2>&1; then
    rev=$(git -C "$root" rev-parse --short=10 HEAD)
    git -C "$root" diff --quiet HEAD -- fipsd || rev="$rev-dirty"
fi

if [ "$do_build" -eq 1 ]; then
    # Homebrew's OpenSSL and curl are keg-only; fips-ci.yml sets the same path.
    if command -v brew >/dev/null 2>&1 && [ -z "${PKG_CONFIG_PATH:-}" ]; then
        PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig"
        export PKG_CONFIG_PATH
    fi
    # shellcheck disable=SC2046
    cmake -S "$root" -B "$build" -G Ninja \
        $(sh "$root/fipsd/packaging/common/cmake-args.sh") \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DGO_WARNINGS_AS_ERRORS=OFF \
        ${rev:+-DFIPSD_GIT_REV=$rev}
    cmake --build "$build" --target fipsd/all
fi
[ -x "$build/fipsd/fipsd" ] || { echo "build-pkg: $build/fipsd/fipsd is missing (run without --no-build)" >&2; exit 1; }

staging=$(mktemp -d)
scripts=$(mktemp -d)
trap 'rm -rf "$staging" "$scripts"' EXIT

# The payload (binaries, the example config, hosts, the client library).
DESTDIR=$staging cmake --install "$build/fipsd" >/dev/null
confdir=$staging/usr/local/etc/fips
# nftables is Linux-only; the baseline has no meaning here.
rm -f "$confdir/fips.nft"
rmdir "$confdir/fips.d" 2>/dev/null || true
# Operator-editable files ship as .default; postinstall seeds the live copies
# only when absent, so an upgrade never overwrites them.
mv "$confdir/hosts" "$confdir/hosts.default"
cp "$staging/usr/local/share/fips/fips.yaml.example" "$confdir/fips.yaml.default"
for bin in "$staging"/usr/local/bin/*; do strip "$bin"; done
"$staging/usr/local/bin/fipsd" -V
# On a Mac that tracks file provenance, every file this script writes carries
# com.apple.provenance, which cannot be cleared (xattr -c, cp -X and ditto
# --noextattr all keep it) and which pkgbuild archives as an AppleDouble
# ._ entry beside each file, printing "write: Permission denied" for the
# ones it cannot rewrite. The entries only re-apply that attribute at
# install time; the payload itself is what pkgutil --payload-files lists.

mkdir -p "$staging/usr/local/var/log/fips" "$staging/Library/LaunchDaemons" "$staging/etc/resolver"
cp "$here/com.fips.fipsd.plist" "$staging/Library/LaunchDaemons/"
plutil -lint "$staging/Library/LaunchDaemons/com.fips.fipsd.plist"
# DNS resolver for the .fips domain. Must match the daemon's dns.bind_addr
# and dns.port (::1, 5354 in the shipped config).
printf 'nameserver ::1\nport 5354\n' > "$staging/etc/resolver/fips"

# Stop a launchd job and wait until its process has exited: `launchctl
# bootout` returns while the job is still shutting down, and replacing the
# binary or starting the new one in that window has left the old daemon's
# UDP socket bound. Printed into both install scripts.
stop_job_fn=$(cat <<'STOPJOB'
stop_job() {
    JOB_PID="$(launchctl print "$1" 2>/dev/null | awk '$1 == "pid" { print $3; exit }')"
    shift
    launchctl bootout "$@" 2>/dev/null || true
    [ -n "$JOB_PID" ] || return 0
    JOB_WAIT=0
    while kill -0 "$JOB_PID" 2>/dev/null; do
        JOB_WAIT=$((JOB_WAIT + 1))
        [ "$JOB_WAIT" -eq 60 ] && { kill -9 "$JOB_PID" 2>/dev/null || true; }
        [ "$JOB_WAIT" -ge 70 ] && break
        sleep 0.5
    done
    return 0
}
STOPJOB
)

{
printf '#!/bin/sh\n\n%s\n\n' "$stop_job_fn"
cat <<'PREINSTALL'
# Stop the service before an upgrade; postinstall starts the new one.
stop_job system/com.fips.fipsd system /Library/LaunchDaemons/com.fips.fipsd.plist
exit 0
PREINSTALL
} > "$scripts/preinstall"

{
printf '#!/bin/sh\nset -e\n\n%s\n\n' "$stop_job_fn"
cat <<'POSTINSTALL'
LOG=/var/log/fipsd-install.log
log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" | tee -a "$LOG"; logger -t fipsd-install "$*"; }
log "postinstall started"

CONFDIR=/usr/local/etc/fips
# Seed the live config and hosts only if absent (preserved on upgrade).
if [ ! -f "$CONFDIR/fips.yaml" ]; then
    cp "$CONFDIR/fips.yaml.default" "$CONFDIR/fips.yaml"
    chmod 600 "$CONFDIR/fips.yaml"
    log "installed default config"
fi
[ -f "$CONFDIR/hosts" ] || cp "$CONFDIR/hosts.default" "$CONFDIR/hosts"

# The daemon writes its log here as root; keep the directory root-owned so
# nobody else can swap the log for a link elsewhere.
LOGDIR=/usr/local/var/log/fips
mkdir -p "$LOGDIR"
chown root:wheel "$LOGDIR"
chmod 755 "$LOGDIR"

# The fips group gates the control socket (/var/run/fips/control.sock,
# 0770). The console user is added so fipsctl works without sudo; a new
# login session is needed for the membership to take effect.
if ! dscl . -read /Groups/fips >/dev/null 2>&1; then
    gid=999
    while dscl . -search /Groups PrimaryGroupID "$gid" 2>/dev/null | grep -q .; do gid=$((gid - 1)); done
    dscl . -create /Groups/fips RecordName fips
    dscl . -create /Groups/fips PrimaryGroupID "$gid"
    log "created group fips ($gid)"
fi
REAL_USER="$(stat -f '%Su' /dev/console 2>/dev/null || true)"
if [ -n "$REAL_USER" ] && [ "$REAL_USER" != root ]; then
    if ! dscl . -read /Groups/fips GroupMembership 2>/dev/null | grep -qw "$REAL_USER"; then
        dscl . -append /Groups/fips GroupMembership "$REAL_USER"
        log "added $REAL_USER to group fips"
    fi
fi

# Flush DNS caches so the new /etc/resolver/fips is read.
dscacheutil -flushcache
killall -HUP mDNSResponder 2>/dev/null || true

stop_job system/com.fips.fipsd system /Library/LaunchDaemons/com.fips.fipsd.plist
launchctl bootstrap system /Library/LaunchDaemons/com.fips.fipsd.plist
log "launchd job loaded"
log "postinstall complete"
exit 0
POSTINSTALL
} > "$scripts/postinstall"
chmod +x "$scripts/preinstall" "$scripts/postinstall"

pkg="$out/fipsd-$version-macos-$arch.pkg"
pkgbuild \
    --root "$staging" \
    --scripts "$scripts" \
    --identifier com.fips.fipsd.pkg \
    --version "$version" \
    --ownership recommended \
    "$pkg"
echo "Package built: $pkg"
echo "Install with: sudo installer -pkg $pkg -target /"
echo "Remove with:  sudo fipsd/packaging/macos/uninstall.sh"
