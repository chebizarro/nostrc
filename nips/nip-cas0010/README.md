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
is embedded anywhere in this module. `nostr_otel_local_signer_*` is a
convenience local-key signer for **tests and local development only**; the tests
generate ephemeral keys with `nostr_key_generate_private()`.

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
validates the tags, applies the admission policy (`DROP` or `FLAG`), decodes the
body and invokes the handler. It touches no network, so a relay subscription
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
