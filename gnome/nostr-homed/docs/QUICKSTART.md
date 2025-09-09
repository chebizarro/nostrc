# Quickstart: nostr-homed

This guide walks you through building and running a demo of nostr-homed.

Prereqs: fuse3, sqlite3, jansson, openssl, secp256k1, glib2, gio2, curl, nsync, pkg-config, cmake or meson.

## Build (CMake)

```
cmake -S . -B build -DENABLE_NOSTR_HOMED=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Demo data

- Publish demo profile (kind:30078 app.config:homed) and homedir.manifest (kind:30081 homedir.manifest:personal) to a test relay.
- Optionally publish homedir.secrets (kind:30079 homedir.secrets:personal) with NIP-44 encrypted refs.

## Fake fixtures (integration tests)

Use `tests/integ/fake_relay_fixture.py` and `tests/integ/fake_blob_server.py` to simulate a relay and a Blossom server.

```
python3 tests/integ/fake_relay_fixture.py &
python3 tests/integ/fake_blob_server.py &
./tests/integ/test_mount.sh
```

## Mount

```
sudo ./build/gnome/nostr-homed/nostrfs -o allow_other /home/demo
```

In production, `pam_nostr` calls `nostr-homectl` which starts a per-user FUSE mount via systemd.
