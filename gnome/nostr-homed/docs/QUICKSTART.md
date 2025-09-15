# Quickstart: nostr-homed

This guide walks you through building and running a demo of nostr-homed.

Prereqs: fuse3, sqlite3, jansson, openssl, secp256k1, glib2, gio2, curl, pkg-config, cmake or meson.

## Build (CMake)

```
cmake -S . -B build -DENABLE_NOSTR_HOMED=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Demo data

- Publish demo profile (kind 30078, tag `["d","app.config:homed"]`) and homedir.manifest (kind 30081, tag `["d","personal"]`) to a test relay.
- Optionally publish homedir.secrets (kind 30079, tag `["d","personal"]`) with NIP-44 encrypted refs.

## Fake fixtures (integration tests)

Use `tests/integ/fake_relay_fixture.py` and `tests/integ/fake_blob_server.py` to simulate a relay and a Blossom server.

```
python3 tests/integ/fake_relay_fixture.py &
python3 tests/integ/fake_blob_server.py &
./tests/integ/test_mount.sh
```

## End-to-end (recommended)

1) Configure username (namespace-aware)

You can set a username for the namespace by either:

- Persisting a setting (preferred):

  ```sh
  # This writes settings.username.personal into the cache DB
  sqlite3 /var/lib/nostr-homed/cache.db \
    "INSERT OR REPLACE INTO settings(key,value) VALUES('username.personal','demo')"
  ```

- Or exporting an environment variable at runtime:

  ```sh
  export HOMED_NAMESPACE=personal
  export HOMED_USERNAME=demo
  ```

2) Warm the cache (provisions UID/GID + manifest/relays + secrets tmpfs)

```sh
export RELAYS_DEFAULT="wss://relay.damus.io,wss://nostr.wine"
export BLOSSOM_BASE_URL="https://blossom.example.org"
./build/gnome/nostr-homectl warm-cache
```

This will:

- Fetch profile relays (30078) and persist `settings.relays.<namespace>`.
- Fetch manifest (30081) and persist `settings.manifest.<namespace>`.
- Decrypt secrets (30079) via the D-Bus signer to `/run/nostr-homed/secrets/secrets.json` (0600, tmpfs).
- Provision deterministic UID/GID for the user via `nh_cache_map_npub_to_uid()`.

3) Open a session (mounts nostrfs via systemd)

```sh
./build/gnome/nostr-homectl open-session demo
```

This ensures warm cache, creates `/home/demo`, starts `nostrfs@demo.service`, waits for the mount, and updates status.

4) Verify write-back

- Use the integration script if `/dev/fuse` is available:

  ```sh
  bash gnome/nostr-homed/tests/integration/run_nostrfs_writeback.sh
  ```

- Or manually:

  ```sh
  echo hello > /home/demo/new.txt
  chmod 600 /home/demo/new.txt
  sync; sleep 1
  # Check manifest JSON via sqlite3
  sqlite3 /var/lib/nostr-homed/cache.db "select value from settings where key='manifest.personal'" | sed -n 1p
  ```

The FUSE write-back path uploads to Blossom (best-effort in CI), promotes the temp file into the per-UID CAS, updates the manifest, and triggers a best-effort coalesced publish of kind 30081 with `['d','<namespace>']`.

5) Close the session

```sh
./build/gnome/nostr-homectl close-session demo
```

This stops the systemd unit and waits for unmount.

## Notes

- CAS quota can be configured via `NOSTRFS_CAS_MAX_BYTES` or `NOSTRFS_CAS_MAX_MB` (default 512MB). Eviction is mtime-based.
- In production, `pam_nostr` performs auth via the D-Bus signer and calls `OpenSession`; `nss_nostr` resolves users from the local cache.
