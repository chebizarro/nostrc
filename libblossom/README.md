# libblossom 0.1.0

Layer-3 synchronous Blossom transport. Uses libcurl, OpenSSL, Jansson and
`nips/bud02`; no GLib, libgit2, homed, encryption or chunking code.

```cmake
find_package(nostrc REQUIRED COMPONENTS blossom)
target_link_libraries(my_client PRIVATE nostrc::blossom)
```

Installed pkg-config name: `blossom` (`pkg-config --static --libs blossom`
for static consumers). Build with `BUILD_LIBBLOSSOM=ON` and `ENABLE_BUD02=ON`.
For a GLib-free dependency closure, build libnostr with `NOSTR_WITH_GLIB=OFF`.

Public headers are under `<blossom/>`. `client.h` owns one endpoint;
`pool.h` supplies replication/failover over the same transport. Handles are
single-thread-owned. The process-local capability cache is mutex-protected.

## Addresses and shimming

PUT uses probe-selected representation, not an environment flag. A server that
rejects raw bodies but accepts the 41-byte PNG prefix selects that prefix. The
address includes the prefix. Use `blossom_client_address` before indexing/HEAD;
pass that address as PUT's optional expected hash. Already shimmed input is not
wrapped twice. A pool prepares one representation across its destination set.

GET verifies the **wire** hash and returns wire bytes. Sealed-data consumers can
use the pure `blossom_shim_detect/unwrap` functions before decrypting. Git/file
consumers can use `blossom_client_get_content`, which verifies before unwrapping.
A successful PUT descriptor is checked against the bytes actually sent.

Cache observations expire after 24 hours, including in long-lived handles.
Clock rollback expires observations. Shared multi-x auth requires a positive
batch probe; a batch 401 switches to per-blob authorization. Probes use a fresh
ephemeral identity. A server advertising `private: true` is not upload-probed
with that unallowlisted identity; upload flags remain unknown. Missing metadata
is not evidence of a public server. Advertised `max_size`/`maxSize` is an upper
bound, never permission to exceed the caller's local cap.

## Transport boundaries

HTTPS by default, no credentials/query/fragment in endpoint URLs, no redirects
or proxy bypass, and resolved socket addresses must be public unicast. HTTP and
private/loopback sockets require the explicit `allow_insecure` **local-test**
option. TLS verification remains enabled. Uploads are bounded before sending;
GET and metadata buffers are bounded during reception, including chunked bodies.
Default blob cap: 16 MiB. GET/HEAD/LIST use BUD-02 auth when a signer is supplied;
401/403 remain errors. Upload without a signer never falls back to anonymous.

BUD-04 mirror URLs must contain the origin's 64-hex content hash (an optional
file extension is accepted). Mirroring reuses `t=upload` auth bound to that hash.
The remote server remains responsible for its own origin-fetch SSRF checks.

## Verification

`test-blossom-*` includes the moved client/shim/probe tests and a bounded local
HTTP fixture exercising public client/pool entrypoints. These are not live-server
or release acceptance. `BLOSSOM_BUILD_LIVE_PROBE=ON` builds the opt-in public probe;
`blossom-transport-driver live URL` is a separate opt-in 1 KiB / 1,2,4,8 MiB sweep
with one attempt per size and 10-second spacing. Both create ephemeral probe
blobs and attempt cleanup; failed writes/deletes cannot establish remote absence.
