# nostr-signer-webext-host

Native-messaging host (`org.nostr.signer_bridge`) for the NIP-07 browser
extension in `browser-extension/nip07/` (bead `nostrc-jjyp`). It forwards
`window.nostr` calls to the desktop signer `org.nostr.Signer` on the session
bus, with the page origin as `app_id`. It holds no key material.

Install steps, security model, wire protocol and error codes are in
`browser-extension/nip07/README.md` in the source tree. This file is shipped
as `/usr/share/doc/nostr-signer-webext-host/README.md`.

## Layout

| file | role |
|------|------|
| `native_messaging.[ch]` | 4-byte LE length + JSON framing on explicit fds |
| `nm_policy.[ch]` | origin → app_id, unsigned-event validation/canonicalization, npub → hex, getRelays shape, NUL guard |
| `nm_errors.[ch]` | error vocabulary + `org.nostr.Signer` / bus error mapping |
| `nm_router.[ch]` | request parsing, id/in-flight limits, provider dispatch, reply envelopes |
| `nm_provider_nip07.c` | NIP-07 → `org.nostr.Signer` (async D-Bus) |
| `nm_provider_webln.c` | WebLN → `org.nostr.Wallet1`: **stub**, TODO `nostrc-yka8` |
| `main_native_host.c` | stdin reader thread, main loop, stdout isolation |
| `manifests/*/org.nostr.signer_bridge.json.in` | host manifests (configured with the libexec path + pinned extension ids) |
| `install.sh` | per-user manifest installer (Firefox, Flatpak Firefox, Chromium, Chrome) |

## Build and test

```sh
cmake -S apps/gnostr-signer/native-host -B build-nmh -G Ninja -DBUILD_TESTING=ON
cmake --build build-nmh && ctest --test-dir build-nmh
```

The end-to-end test (`test_nmh_e2e`) needs the `nostr-signer-daemon` target,
so it is only built from the top-level tree
(`-DENABLE_NOSTR_SIGNER_WEBEXT_HOST=ON -DBUILD_TESTING=ON`); it runs the real
daemon on a private `GTestDBus` bus with a throw-away key and drives the real
host binary through pipes.

Manual smoke test against a running signer:

```sh
printf '{"id":"1","method":"getPublicKey","origin":"https://snort.social"}' |
  python3 -c 'import sys,struct;d=sys.stdin.buffer.read();sys.stdout.buffer.write(struct.pack("<I",len(d))+d)' |
  /usr/libexec/nostr-signer-webext-host | tail -c +5; echo
```

Environment: `NOSTR_SIGNER_BRIDGE_IDENTITY` (signer identity selector),
`NOSTR_SIGNER_BRIDGE_CALL_TIMEOUT_MS` (30000),
`NOSTR_SIGNER_BRIDGE_APPROVAL_TIMEOUT_MS` (120000),
`NOSTR_SIGNER_BRIDGE_DEBUG=1`.
