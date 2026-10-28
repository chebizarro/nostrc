#!/bin/bash
# h10m.7 follow-up probe: the proposed policy parameters (PAD_32; AES-256-HCTR2 filenames).
#   docker run --rm --privileged --name h10m7-fscrypt-params -v "$PWD":/probe:ro debian:trixie bash /probe/policy-params.sh
set -u
apt-get update -qq >/dev/null && apt-get install -y -qq e2fsprogs python3 >/dev/null 2>&1 || { echo "APT INSTALL FAILED"; exit 2; }
f(){ python3 /probe/fsc.py "$@"; }
uname -r
head -c64 /dev/urandom > /tmp/k; truncate -s 64M /tmp/e.img; mkfs.ext4 -q -O encrypt /tmp/e.img; mkdir -p /mnt/e; mount -o loop /tmp/e.img /mnt/e
ID=$(f add /mnt/e /tmp/k); echo "id=$ID"
for spec in "xts-cts-pad32 1 4 0x03" "xts-hctr2-pad32 1 10 0x03"; do
  set -- $spec; mkdir /mnt/e/$1; echo "## $1"; f set /mnt/e/$1 "$ID" $2 $3 $4 && f get /mnt/e/$1 && echo ok > /mnt/e/$1/t && cat /mnt/e/$1/t
done
umount /mnt/e
