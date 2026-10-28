#!/usr/bin/env bash
set -euo pipefail
# Keep package signatures enabled. Try the main server before the redirector;
# curl bounds both database and archive transfers and retries transient errors.
for repo in mingw msys; do
  if [[ "$repo" == mingw ]]; then suffix="mingw/\$repo"; else suffix="msys/\$arch"; fi
  printf 'Server = https://repo.msys2.org/%s/\nServer = https://mirror.msys2.org/%s/\n' \
    "$suffix" "$suffix" > "/etc/pacman.d/mirrorlist.$repo"
done
sed -i '/^[[:space:]]*XferCommand[[:space:]]*=/d' /etc/pacman.conf
# This belongs in [options], not at EOF inside a package repository section.
sed -i '/^\[options\]/a XferCommand = /usr/bin/curl --fail --location --retry 2 --retry-delay 2 --connect-timeout 15 --max-time 180 --speed-limit 1024 --speed-time 30 --output %o %u' /etc/pacman.conf
cat /etc/pacman.d/mirrorlist.{mingw,msys}
