# nostr-homed (GNOME / Linux)

Nostr-backed roaming home directory and login stack, integrating NSS, PAM, and a FUSE3 filesystem. A per-user control service fetches manifests from Nostr relays, decrypts secrets via the existing signer D-Bus API, and mounts a POSIX home tree backed by Blossom/GRASP content-addressed storage.

Components:
- libnss_nostr — NSS module resolving users/groups from a local SQLite cache hydrated from Nostr events.
- pam_nostr — PAM module performing NIP-46-style auth via local D-Bus signer and bootstrapping the session mount.
- nostrfs — FUSE3 filesystem materializing home from homedir.manifest; write-back uploads to Blossom and republishes.
- nostr-homectl — CLI + D-Bus service managing cache, fetching events, decrypting secrets via signer, and launching mounts.
- systemd units — sandboxed services for the control daemon and per-user FUSE mount.

Defaults:
- RELAYS_DEFAULT: ["wss://relay.damus.io","wss://nostr.wine"]
- BLOSSOM_BASE_URL: "https://blossom.example.org"
- HOMED_NAMESPACE: "personal"
- SECRETS_NAMESPACE: "personal"
- UID_POLICY: "deterministic-hash"
- FUSE_IMPL: "fuse3"
- DBUS_SIGNER_BUS_NAME: "org.nostr.Signer"

D-Bus signer mapping
- This repository already includes D-Bus assets under `apps/gnostr-signer/` and `nips/nip55l/dbus/` using both `org.nostr.Signer` and `com.nostr.Signer` names.
- nostr-homed prefers `org.nostr.Signer` by default, but will also probe `com.nostr.Signer` for compatibility. See `src/common/nip46_client_dbus.c` for name probing.

Build
- CMake (top-level):
  ```sh
  cmake -S . -B build -DENABLE_NOSTR_HOMED=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```
- Meson (subdir):
  ```sh
  meson setup build-gnome gnome/nostr-homed
  meson compile -C build-gnome
  meson test -C build-gnome
  ```

Install
- NSS and PAM modules install into distro libdirs (`libnss_nostr.so.2`, `pam_nostr.so`).
- Systemd units install into `lib/systemd/system`.

Quickstart
- See `docs/QUICKSTART.md` for publishing sample events, running fake relay and blob servers, and mounting a demo home.

NSS/PAM enablement (overview)

Add `nss_nostr` to `nsswitch.conf` after files (cache-only; no network on hot path):

```
passwd:         files systemd nostr
group:          files systemd nostr
```

On Debian/Ubuntu, enable PAM session for `pam_nostr` (example using common-session):

```
# /etc/pam.d/common-session (append near the end)
session optional pam_nostr.so
```

OpenSSH example (append to `/etc/pam.d/sshd` after `@include common-session`):

```
session optional pam_nostr.so
```

Notes:
- `nostr-homectl warm-cache` provisions deterministic UID/GID mapping and persists `settings.manifest.<namespace>` and `settings.relays.<namespace>`.
- `OpenSession` is called by `pam_nostr` to start `nostrfs@user` via systemd and mount `/home/$USER`.

Groups

- Primary group is created during `WarmCache` provisioning with name equal to the username and `gid == uid`.
- NSS group lookups are resolved from the local cache only. Supplemental groups are not managed in v1.

Security
- See `docs/SECURITY.md` for threat model, secret handling (tmpfs), and systemd hardening.
