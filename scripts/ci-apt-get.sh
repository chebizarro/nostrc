#!/usr/bin/env bash
# Retry transport failures without weakening apt's signed metadata/hash checks.
set -euo pipefail

apt_get() {
  # Retain the real download summary even when a caller uses -qq or >/dev/null.
  timeout --signal=TERM --kill-after=30s 12m /usr/bin/apt-get \
    -o Acquire::Retries=2 -o Acquire::http::Timeout=20 \
    -o Acquire::https::Timeout=20 -o APT::Update::Error-Mode=any \
    "$@" -o quiet=1 >&2
}

if apt_get "$@"; then
  exit 0
fi

echo '::warning::apt failed; retrying with alternate distribution mirrors'
# Change only known distribution endpoints, never third-party repositories,
# suites, components, signing keys or package/version arguments.
for source in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [[ -f "$source" ]] || continue
  sed -i -E \
    -e 's@(https?://)([a-z]+\.)?(archive|security)\.ubuntu\.com/ubuntu(/|[[:space:]]|$)@\1mirrors.edge.kernel.org/ubuntu\4@g' \
    -e 's@(https?://)ports\.ubuntu\.com/ubuntu-ports(/|[[:space:]]|$)@\1mirrors.mit.edu/ubuntu-ports\2@g' \
    -e 's@(https?://)deb\.debian\.org/debian(/|[[:space:]]|$)@\1ftp.us.debian.org/debian\2@g' \
    "$source"
done
# Install URIs are derived from package indexes, so refresh after switching.
# A failed refresh is fatal, including apt's otherwise-successful partial update.
if [[ " $* " != *' update '* ]]; then
  apt_get update
fi
apt_get "$@"
