# NIP-46 Examples

This directory contains canonical examples using only the public `nostr_nip46_*` APIs from `nips/nip46/include/nostr/nip46/`.

These examples are transportless demos: requests are encrypted with NIP-04 and passed directly into the bunker handler. No network or relay is used.

## Build

From the repository root:

```sh
cmake -S . -B build
cmake --build build -j8
```

## Run

- Client demo:

```sh
./build/nips/nip46/nostr-nip46-client
```

- Bunker demo:

```sh
./build/nips/nip46/nostr-nip46-bunker
```

Both programs will:

- Initialize the JSON provider (Jansson via `nostr_json`).
- Configure secrets using a `bunker://` URI (no network).
- Perform a NIP-46 `connect` with ACL granting `sign_event`.
- Request `sign_event` for a simple content event.
- Verify the returned signature with `nostr_event_check_signature()`.

## Notes

- Keys used are deterministic demo values for reproducibility.
- ACL enforcement is active in the bunker: `sign_event` is denied unless granted during `connect`.
- These examples are intended as a minimal reference. Integrating with real relays requires wiring a transport that carries the ciphertext to/from `nostr_nip46_bunker_handle_cipher()`.
