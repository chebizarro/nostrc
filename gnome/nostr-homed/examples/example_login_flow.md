# Example: Login Flow with nostr-homed

This walkthrough shows how PAM + NSS + nostrfs cooperate to provide a roaming home directory backed by Nostr.

Prerequisites

- Build nostr-homed with CMake:
  - `cmake -S . -B build -DENABLE_NOSTR_HOMED=ON`
  - `cmake --build build -j`
- A local DBus signer implementing `org.nostr.Signer` running for the user.
- FUSE3 available (`/dev/fuse`).

Steps

1) Configure NSS/PAM (Debian/Ubuntu)

- NSS: add `nostr` to passwd/group in `/etc/nsswitch.conf` after `files`:
  - `passwd: files systemd nostr`
  - `group:  files systemd nostr`
- PAM common-session (example):
  - Append to `/etc/pam.d/common-session`:
    - `session optional pam_nostr.so`
- OpenSSH example (append to `/etc/pam.d/sshd` after `@include common-session`):
  - `session optional pam_nostr.so`

2) Configure cache policy

Create `/etc/nss_nostr.conf`:

```
db_path=/var/lib/nostr-homed/cache.db
uid_base=100000
uid_range=100000
```

3) Warm the cache

```
export HOMED_NAMESPACE=personal
export HOMED_USERNAME=demo
export RELAYS_DEFAULT="wss://relay.damus.io,wss://nostr.wine"
export BLOSSOM_BASE_URL="https://blossom.example.org"
./build/gnome/nostr-homectl warm-cache
```

This fetches relays (30078) and manifest (30081), decrypts secrets (30079) via the local signer to tmpfs (0600), and provisions UID/GID + primary group.

4) Open a session

```
./build/gnome/nostr-homectl open-session demo
```

This mounts `/home/demo` via `nostrfs@demo.service`.

5) Verify

```
ls -la /home/demo
cat /home/demo/README.txt 2>/dev/null || true
```

6) Write-back test

```
echo "hello" > /home/demo/hello.txt
chmod 600 /home/demo/hello.txt
sync; sleep 1
```

Check the manifest persisted under `settings.manifest.personal` in the cache DB.

7) Close the session

```
./build/gnome/nostr-homectl close-session demo
```

Notes

- Secrets are decrypted only into tmpfs with mode 0600.
- CAS quota can be tuned with `NOSTRFS_CAS_MAX_BYTES` or `NOSTRFS_CAS_MAX_MB`.
- See `docs/QUICKSTART.md` and `docs/SECURITY.md` for details.
