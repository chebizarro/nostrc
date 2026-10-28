#!/usr/bin/env bash
# Retry transport/index failures without weakening signed metadata/hash checks.
set -euo pipefail

apt_get() {
  local limit=12m
  [[ " $* " != *' update '* ]] || limit=2m
  # Retain the real download summary even when a caller uses -qq or >/dev/null.
  timeout --signal=TERM --kill-after=30s "$limit" /usr/bin/apt-get \
    -o Acquire::Retries=2 -o Acquire::http::Timeout=20 \
    -o Acquire::https::Timeout=20 -o APT::Update::Error-Mode=any \
    -o Acquire::http::No-Cache=true -o Acquire::https::No-Cache=true \
    "$@" -o quiet=1 >&2
}

alternate_mirrors() {
  echo '::warning::apt failed; trying alternate distribution mirrors' >&2
  # Never change third-party repositories, suites, components or signing keys.
  for source in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
    [[ -f "$source" ]] || continue
    sed -i -E \
      -e 's@(https?://)([a-z]+\.)?(archive|security)\.ubuntu\.com/ubuntu(/|[[:space:]]|$)@\1mirrors.edge.kernel.org/ubuntu\4@g' \
      -e 's@(https?://)ports\.ubuntu\.com/ubuntu-ports(/|[[:space:]]|$)@\1mirrors.mit.edu/ubuntu-ports\2@g' \
      -e 's@(https?://)deb\.debian\.org/debian(/|[[:space:]]|$)@\1ftp.us.debian.org/debian\2@g' \
      "$source"
  done
}

verify_archives() {
  local cache=/var/cache/nostrc-apt/archives work archive
  compgen -G "$cache/*.deb" >/dev/null || return 0
  work="$(mktemp -d)"
  # Apt can trust an existing archive by filename. Validate restored bytes
  # against the signed indexes ourselves before allowing any install to use it.
  if ! /usr/bin/apt-cache dumpavail | awk -v cache="$cache" '
    function emit(v) { print hash "  " cache "/" package "_" v "_" arch ".deb" }
    function record( v) {
      if (package && version && arch && hash) {
        emit(version)
        if (version ~ /:/) {
          v=version; gsub(/:/, "%3a", v); emit(v)
          v=version; gsub(/:/, "%3A", v); emit(v)
        }
      }
      package=version=arch=hash=""
    }
    /^Package: / { package=$2 }
    /^Version: / { version=$2 }
    /^Architecture: / { arch=$2 }
    /^SHA256: / { hash=$2 }
    /^$/ { record() }
    END { record() }
  ' > "$work/expected"; then
    rm -rf -- "$work"
    return 1
  fi
  sha256sum "$cache"/*.deb > "$work/actual"
  awk 'FILENAME == ARGV[1] { allowed[$0]=1; next }
       !($0 in allowed) { print substr($0, index($0, "  ")+2) }' \
    "$work/expected" "$work/actual" > "$work/discard"
  while IFS= read -r archive; do
    echo "::warning::Discarding cached archive not verified by current signed indexes: $archive" >&2
    rm -f -- "$archive"
  done < "$work/discard"
  rm -rf -- "$work"
}

apt_update() {
  local attempts=4 delay=5 attempt rc
  # Ddebs can lag while publishing indexes. Give it two more attempts, but
  # never drop it or proceed with stale/partial metadata on the sanitizer lane.
  if grep -qs 'ddebs\.ubuntu\.com' /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; then
    attempts=6
  fi
  for ((attempt=1; attempt<=attempts; attempt++)); do
    echo "::notice::apt update attempt $attempt/$attempts" >&2
    if apt_get "$@"; then return 0; else rc=$?; fi
    # Preserve explicit interruption rather than retrying a canceled command.
    if [[ "$rc" == 130 || "$rc" == 143 ]]; then return "$rc"; fi
    if ((attempt == attempts)); then
      echo "::error::apt update failed after $attempts attempts; required indexes remain unavailable" >&2
      return "$rc"
    fi
    if ((attempt == 1)); then alternate_mirrors; fi
    echo "::warning::apt update failed (exit $rc); retrying in ${delay}s" >&2
    sleep "$delay"
    delay=$((delay * 2))
    if ((delay > 60)); then delay=60; fi
  done
}

if [[ " $* " == *' update '* ]]; then
  apt_update "$@"
  exit 0
fi
if [[ " $* " == *' install '* ]]; then verify_archives; fi
if apt_get "$@"; then exit 0; else rc=$?; fi
if [[ "$rc" == 130 || "$rc" == 143 ]]; then exit "$rc"; fi
alternate_mirrors
# Refresh install URIs after switching; all refreshes use the same retry policy.
apt_update update
if [[ " $* " == *' install '* ]]; then verify_archives; fi
apt_get "$@"
