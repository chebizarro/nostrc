# nip-cas0010 — OTLP-over-Nostr transport primitives

C producer/consumer primitives for **NIP-CAS-0010**: OpenTelemetry OTLP payloads
carried over Nostr as ephemeral event kinds.

| Kind | Signal | `domain` | `schema` |
|------|--------|----------|----------|
| 24900 | traces | `traces` | `cascadia.otel.traces.v1` |
| 24901 | metrics | `metrics` | `cascadia.otel.metrics.v1` |
| 24902 | logs | `logs` | `cascadia.otel.logs.v1` |

## Scope

This module is a **transport for opaque OTLP bytes**. It never parses or
serializes OTLP protobuf/JSON: the caller hands it already-encoded OTLP bytes and
the consumer hands the same bytes back, attributed to the signing pubkey. Any
OTLP structure awareness (building `TracesData`, splitting an oversized single
message) belongs to the caller.

## Redaction is the caller's responsibility

Telemetry events are **plaintext**: `content` is only base64, never encrypted, and
any client subscribed to kinds 24900–24902 on the relay can read it. Because this
module treats OTLP as opaque bytes it **cannot redact anything** — it has no view
of attributes or log bodies (NIP-CAS-0010 security review §4.4, finding G3).

**Redact before passing OTLP bytes to `nostr_otel_producer_build_event()` /
`nostr_otel_producer_publish()`.** Apply the same default-on policy the Go
(`otelnostr.RedactionPolicy`) and TypeScript (loom-worker `redact.ts`) SDKs use,
at the point where you still hold structured OTLP (before serialization):

- replace values of attributes whose key segment is secret-like (`password`,
  `token`, `api_key`, `authorization`, `cookie`, `nsec`, `private_key`, `bunker`,
  `mnemonic`, `credentials`, …) — resource, scope, span, span event/link, log
  record and metric data point attributes, and log body map entries;
- scrub embedded secrets from every string attribute, log body, span name and
  status message: `Authorization`/`Bearer`/`Basic` credentials, `nsec1…` keys,
  `bunker://`/`nostrconnect://` URIs, `cashuA…`/`cashuB…` tokens, JWTs,
  `scheme://user:pass@` userinfo, sensitive URL query parameters, secret-named
  environment assignments (`*_TOKEN=`, `*_PASSWORD=`) and hex private keys;
- cap string values (4 KiB);
- never put secrets or free text in the `service` tag or recipients — tags are
  plaintext too.

Error strings (e.g. from config parsing or a Signet/NIP-46 connect failure) can
echo bunker URIs or relay credentials; do not export them unredacted. The
collector distribution (`cascadia-go/collector/distribution`) applies a
`redaction` processor as defence in depth, but it does not see events published
directly to relays, so producer-side redaction is mandatory.

## Wire rules

- `event.content` is **always** base64 — including the `identity` compression.
- Tags: exactly one `domain`, exactly one `schema` (both matching the kind), at
  most one `enc`, optional `service`, optional `p` recipients.
- `enc` is `["enc", <format>, <compression>]`; a **missing `enc` defaults to
  `otlp-proto` + `identity`**.
- Duplicate, short or overlong `enc`, and duplicate `domain`/`schema`, are
  rejected — never "last one wins".
- Producers keep event content at or below ~64 KiB (`NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES`).

## Signing

Signing is a function pointer (`NostrOtelSignFn`), so the caller supplies the
identity — normally **Signet / NIP-46** (`signet/`, `nips/nip46`). No private key
is embedded anywhere in this module.

### The test-only local signer is not in the shipped library

`nostr_otel_local_signer_*` is a raw-private-key signer for **tests and local
development only**, and documentation is not a build boundary — so it is behind
a build guard:

- Source lives in `src/otel_local_signer.c`, wrapped in
  `#ifdef NOSTR_OTEL_ENABLE_TEST_SIGNER`; the prototypes in
  `include/nostr/nip_cas0010/otel.h` are guarded by the same macro.
- The CMake option `NOSTR_OTEL_ENABLE_TEST_SIGNER` **defaults to OFF**, so a
  default build of `nostr_nip_cas0010_core` contains **no**
  `nostr_otel_local_signer_*` symbol at all. Verify with:
  `nm build/.../nostr_nip_cas0010_core.dir/src/*.o | grep local_signer` (no output).
- The module's tests still exercise it: they compile
  `src/otel_local_signer.c` into each test binary as a test-local helper, with
  `NOSTR_OTEL_ENABLE_TEST_SIGNER` defined for that target only.
- `-DNOSTR_OTEL_ENABLE_TEST_SIGNER=ON` puts it back into the library (with a
  loud configure-time warning). Never ship such a build: the fleet's
  Signet-first policy forbids raw private keys in producer processes.
- `BUILD_NIP_CAS0010_EXAMPLES` also defaults to **OFF** because
  `examples/otel_nostr_publish.c` signs with this test signer (it generates an
  ephemeral key with `nostr_key_generate_private()`); when built, the example
  gets the same test-local helper rather than a library symbol.

## Batching

`nostr_otel_producer_publish()` concatenates the supplied payloads and flushes an
event whenever adding the next payload would exceed the cap. Concatenation is the
protobuf repeated-field merge of the OTLP `*Data` messages, so a flushed batch
stays one valid OTLP message. A *single* payload that cannot fit returns
`NOSTR_OTEL_ERR_BATCH_TOO_LARGE`: splitting it requires OTLP structure, which
this module deliberately does not have.

## Compression

`identity` is mandatory and always available. `zstd` is compiled in when libzstd
is found via pkg-config at configure time (`ENABLE_NIP_CAS0010_ZSTD`, default
ON); otherwise the module is identity-only and requesting zstd returns
`NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING`. Query it at run time with
`nostr_otel_zstd_available()`. Decompression is bounded by
`max_decoded_bytes`, and frames without a declared content size are refused.

## Consumer

`nostr_otel_consumer_process()` verifies the id+signature, checks the kind,
validates the tags, applies the admission policy (`DROP` or `FLAG` with a
required admit callback, or explicit `OPEN`), decodes the body and invokes the
handler. It touches no network, so a relay subscription
(`nostr_otel_consumer_subscribe()` over `NostrSimplePool`), a bridge, or a test
can all feed it events.

## Build & test

```sh
cmake -B _build -DCMAKE_BUILD_TYPE=Debug
cmake --build _build -j
ctest --test-dir _build -R otel --output-on-failure
```

`examples/otel_nostr_publish.c` publishes one payload to the relays given on the
command line and prints it back from its own subscription.
