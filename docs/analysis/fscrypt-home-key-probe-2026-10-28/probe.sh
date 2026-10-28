#!/bin/bash
# h10m.7 fscrypt v2 probe. Runs inside a throwaway privileged container:
#   docker run --rm --privileged --name h10m7-fscrypt-probe \
#     -v "$PWD":/probe:ro debian:trixie bash /probe/probe.sh
# fscryptctl is not packaged for Debian/Ubuntu arm64, so `fscryptctl` below is a
# shell shim over fsc.py, which issues the raw <linux/fscrypt.h> ioctls.
set -u
step(){ echo; echo "### $*"; }
try(){ echo "\$ $*"; "$@"; echo "  -> rc=$?"; }
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null && apt-get install -y -qq e2fsprogs f2fs-tools btrfs-progs python3 util-linux >/dev/null 2>&1 || { echo "APT INSTALL FAILED"; exit 2; }
fscryptctl(){ python3 /probe/fsc.py "$@"; }
export -f fscryptctl 2>/dev/null
step "versions"; uname -r; echo "ioctl driver: /probe/fsc.py"; mkfs.ext4 -V 2>&1 | head -1
useradd -u 1000 -m -d /tmp/alice-skel alice

step "derive keys (python HKDF-SHA256, porthome-style salts)"
cat > /tmp/hkdf.py <<'PY'
import hmac, hashlib, sys
def hkdf(salt, ikm, info, L):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, t, i = b"", b"", 1
    while len(out) < L:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest(); out += t; i += 1
    return out[:L]
seed = bytes.fromhex(sys.argv[1]); dev = bytes.fromhex(sys.argv[2]); L = int(sys.argv[3])
home_key = hkdf(b"porthome/v1/home", seed, b"", 32)            # libporthome porthome_key_derive
fk = hkdf(b"porthome/local/fscrypt/v1", home_key,
          b"home\x00" + b"acct-uuid" + b"\x00" + dev + (1).to_bytes(4, "big"), L)
sys.stdout.buffer.write(fk)
PY
SEED=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')
DEVA=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')
DEVB=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')
python3 /tmp/hkdf.py $SEED $DEVA 64 > /tmp/kA; python3 /tmp/hkdf.py $SEED $DEVA 64 > /tmp/kA2; chmod 644 /tmp/kA2
python3 /tmp/hkdf.py $SEED $DEVB 64 > /tmp/kB; python3 /tmp/hkdf.py $SEED $DEVA 32 > /tmp/k32
head -c64 /dev/urandom > /tmp/kvault
wc -c /tmp/kA /tmp/k32 | head -2; cmp -s /tmp/kA /tmp/kA2 && [ -s /tmp/kA ] && echo "re-derivation deterministic: yes"; [ -s /tmp/kB ] && ! cmp -s /tmp/kA /tmp/kB && echo "device-diversified keys differ: yes"

step "ext4 loop image with -O encrypt"
truncate -s 256M /tmp/ext4.img; mkfs.ext4 -q -O encrypt /tmp/ext4.img; mkdir -p /mnt/e; mount -o loop /tmp/ext4.img /mnt/e
dumpe2fs -h /tmp/ext4.img 2>/dev/null | grep -i 'features'
mkdir -p /mnt/e/home && chmod 755 /mnt/e/home
ID=$(fscryptctl add /mnt/e /tmp/kA); echo "add_key (root, 64B) id=$ID"
IDB=$(fscryptctl add /mnt/e /tmp/kB); echo "add_key device-B id=$IDB (differs: $([ "$ID" != "$IDB" ] && echo yes))"
fscryptctl remove /mnt/e "$IDB"

step "stage dir: policy must be set while EMPTY (before skel copy)"
mkdir -m700 /mnt/e/home/.nostr-op1
try fscryptctl set /mnt/e/home/.nostr-op1 "$ID"
fscryptctl get /mnt/e/home/.nostr-op1
mkdir /mnt/e/home/nonempty && touch /mnt/e/home/nonempty/x
try fscryptctl set /mnt/e/home/nonempty "$ID"

step "materialise into stage (.part + rename), then renameat2 NOREPLACE into unencrypted /home"
mkdir -p /mnt/e/home/.nostr-op1/Documents /mnt/e/home/.nostr-op1/.config
echo "secret plaintext" > /mnt/e/home/.nostr-op1/Documents/note.txt.part && mv /mnt/e/home/.nostr-op1/Documents/note.txt.part /mnt/e/home/.nostr-op1/Documents/note.txt
python3 -c 'import ctypes,os;libc=ctypes.CDLL(None,use_errno=True);r=libc.renameat2(-100,b"/mnt/e/home/.nostr-op1",-100,b"/mnt/e/home/alice",1);print("renameat2(RENAME_NOREPLACE) rc=",r,os.strerror(ctypes.get_errno()) if r else "")'
chown -R 1000:1000 /mnt/e/home/alice; chmod 700 /mnt/e/home/alice
echo "uid1000 reads (fs-wide v2 keyring, root added key):"; setpriv --reuid=1000 --regid=1000 --clear-groups cat /mnt/e/home/alice/Documents/note.txt

step "nested policy inside encrypted home (per-subtree vault key)"
mkdir /mnt/e/home/alice/Team
VID=$(fscryptctl add /mnt/e /tmp/kvault)
try fscryptctl set /mnt/e/home/alice/Team "$VID"
echo "Team inherited policy:"; fscryptctl get /mnt/e/home/alice/Team | head -3

step "sibling subtrees with different policies (would be needed for per-vault keys): cross-policy rename/link"
mkdir /mnt/e/home/bob-root /mnt/e/home/bob-root-vault
fscryptctl set /mnt/e/home/bob-root "$ID"; fscryptctl set /mnt/e/home/bob-root-vault "$VID"
echo hi > /mnt/e/home/bob-root-vault/f
python3 -c 'import os
for op in ("rename","link"):
    try:
        getattr(os,op)("/mnt/e/home/bob-root-vault/f","/mnt/e/home/bob-root/f"); print(op,"ok")
    except OSError as e: print(op,"->",e.strerror)'
echo plain > /mnt/e/home/plainfile
python3 -c 'import os
try: os.rename("/mnt/e/home/plainfile","/mnt/e/home/alice/plainfile"); print("rename unencrypted->encrypted ok")
except OSError as e: print("rename unencrypted->encrypted ->",e.strerror)'

step "non-root add_key / remove_key claim semantics"
setpriv --reuid=1000 --regid=1000 --clear-groups python3 /probe/fsc.py add /mnt/e /tmp/kA2 && echo "uid1000 add_key ok (same id -> claim added)"
fscryptctl status /mnt/e "$ID"

step "lock at logout with a busy file"
setpriv --reuid=1000 --regid=1000 --clear-groups python3 -c 'import time; f=open("/mnt/e/home/alice/Documents/note.txt"); time.sleep(60)' & HOLDER=$!
sleep 1
try fscryptctl remove /mnt/e "$ID"
echo "(root removed only root's claim)"; fscryptctl status /mnt/e "$ID"
try fscryptctl remove /mnt/e "$ID" --all
fscryptctl status /mnt/e "$ID"
echo "while INCOMPLETELY_REMOVED: busy file + cached dirs readable? ->"; cat /mnt/e/home/alice/Documents/note.txt; ls /mnt/e/home/alice
kill $HOLDER; wait $HOLDER 2>/dev/null; echo "holder killed"
try fscryptctl remove /mnt/e "$ID" --all
fscryptctl status /mnt/e "$ID"
sync; echo 2 > /proc/sys/vm/drop_caches 2>/dev/null

step "locked view (what a thief / other user / root-without-key sees)"
ls -la /mnt/e/home/alice | head; ls -la /mnt/e/home/alice/* 2>&1 | head -6
try cat /mnt/e/home/alice/Documents/note.txt
try touch /mnt/e/home/alice/newfile
echo "metadata still visible: sizes/mtimes/modes/owner/tree shape above"
grep -c "secret plaintext" /tmp/ext4.img && echo "PLAINTEXT FOUND IN IMAGE" || echo "plaintext not found in raw image"

step "re-derive from same seed -> unlock (deterministic, no on-disk key material)"
python3 /tmp/hkdf.py $SEED $DEVA 64 > /tmp/kre; fscryptctl add /mnt/e /tmp/kre; cat /mnt/e/home/alice/Documents/note.txt
fscryptctl remove /mnt/e "$ID" --all >/dev/null

step "recovery: root can delete a locked home without the key"
cp -a /mnt/e/home/bob-root /mnt/e/home/zz 2>/dev/null; fscryptctl remove /mnt/e "$VID" --all >/dev/null
try rm -rf /mnt/e/home/bob-root-vault; ls /mnt/e/home

step "32-byte master key with AES-256-XTS v2 policy"
mkdir /mnt/e/k32; ID32=$(fscryptctl add /mnt/e /tmp/k32); echo "id32=$ID32"
try fscryptctl set /mnt/e/k32 "$ID32"; echo x > /mnt/e/k32/f && cat /mnt/e/k32/f
umount /mnt/e

step "f2fs with -O encrypt"
truncate -s 256M /tmp/f2fs.img; mkfs.f2fs -q -O encrypt /tmp/f2fs.img >/dev/null; mkdir -p /mnt/f; mount -o loop /tmp/f2fs.img /mnt/f
FID=$(fscryptctl add /mnt/f /tmp/kA); mkdir /mnt/f/h; try fscryptctl set /mnt/f/h "$FID"; echo ok > /mnt/f/h/t && cat /mnt/f/h/t; umount /mnt/f

step "enable encrypt on an EXISTING ext4 (tune2fs) - mounted and unmounted"
truncate -s 64M /tmp/t.img; mkfs.ext4 -q /tmp/t.img; mkdir -p /mnt/t; mount -o loop /tmp/t.img /mnt/t
try tune2fs -O encrypt $(findmnt -no SOURCE /mnt/t); mkdir /mnt/t/h; TID=$(fscryptctl add /mnt/t /tmp/kA); echo "after online tune2fs add_key -> $TID"; umount /mnt/t
try tune2fs -O encrypt /tmp/t.img; mount -o loop /tmp/t.img /mnt/t; TID=$(fscryptctl add /mnt/t /tmp/kA); echo "after remount add_key -> $TID"; try fscryptctl set /mnt/t/h "$TID"; umount /mnt/t

step "ext4 WITHOUT encrypt feature"
truncate -s 64M /tmp/ne.img; mkfs.ext4 -q /tmp/ne.img; mkdir -p /mnt/n; mount -o loop /tmp/ne.img /mnt/n; mkdir /mnt/n/h
NID=$(fscryptctl add /mnt/n /tmp/kA); echo "add_key -> $NID"; try fscryptctl set /mnt/n/h "$ID"; umount /mnt/n

step "btrfs"
truncate -s 256M /tmp/b.img; mkfs.btrfs -q /tmp/b.img >/dev/null; mkdir -p /mnt/b; mount -o loop /tmp/b.img /mnt/b; mkdir /mnt/b/h
try fscryptctl add /mnt/b /tmp/kA; try fscryptctl set /mnt/b/h "$ID"; umount /mnt/b
echo; echo "### done"
