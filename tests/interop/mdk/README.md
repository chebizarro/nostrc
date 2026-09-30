# MDK interop harness

Third-party Marmot interop for the encrypted-groups release gate
(nostrc-7gx7, nostrc-77pa; privacy charter section 7.9). One side is
Groundhog's real `GhMlsService` (libmarmot). The other side is
[MDK](https://github.com/marmot-protocol/mdk) v0.8.0, the Rust library White
Noise 0.8 builds on. Both talk through the test's own local relays only.
Results and root causes are in
`docs/analysis/marmot-mdk-interop-2026-09-30.md`.

## Pins

| What | Pin |
| --- | --- |
| MDK | `v0.8.0` = `575ae29d25d58494135058d9e46affec7174e79a` (mdk-core, mdk-memory-storage, mdk-storage-traits) |
| OpenMLS | `04c50d7fb12d52f4f9aee26de5f5234f3df29fa8` (what MDK v0.8.0 locks) |
| Rust | `rust:1.93-slim-bookworm` (build stage only) |
| Everything else | `driver/Cargo.lock`, built with `--locked` |

MDK 0.9.0 and later is a different engine (cgka-engine, the adopted
profile), which libmarmot's group engine does not speak yet
(nostrc-qp24.5.1).

## Run it

Docker is required; no Rust toolchain is needed on the host.

```sh
cmake -S . -B _build -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_MDK_INTEROP=ON
cmake --build _build --target test-groundhog-mdk-interop
ctest --test-dir _build -R '^groundhog-mdk-interop' -V
```

- `groundhog-mdk-interop-image` (a CTest fixture) runs
  `docker build -t nostrc-mdk-interop:0.8.0 tests/interop/mdk/driver`.
- `groundhog-mdk-interop` then runs the six cases of
  `gnome/groundhog/tests/mls/test_mdk_interop.c`.
- The container reaches the test's relays on the host's 127.0.0.1:
  - Linux: `--network host`.
  - macOS (Docker Desktop): `MDK_DRIVER_DIAL_HOST=host.docker.internal`.
    Only the dialled host changes; the URLs, and so the NIP-42 `relay` tag,
    stay `127.0.0.1`.
- Without `BUILD_MDK_INTEROP` the test is not built. Without `GH_MDK_DRIVER`
  set, every case skips.

Knobs:

- `MDK_DRIVER_LOG=debug` (or any `tracing` EnvFilter): MDK's own diagnostics
  on stderr (default `warn`).
- `MDK_DRIVER_DEADLINE_S` (default 30): the bound on every relay wait. A hang
  becomes an error; nothing sleeps.
- `-DMDK_INTEROP_DRIVER="<command line>"`: another driver instead of the
  image, e.g. a native `cargo build --release` of `driver/`.

CI: `.github/workflows/marmot-mdk-interop.yml` runs on demand and nightly,
and keeps the verbose log as an artifact.

## Driver protocol

`driver/src/main.rs` reads one JSON object per line on stdin,
`{"id": N, "cmd": "...", ...}`. It writes exactly one answer per request, in
order: `{"id": N, "ok": true, ...}` or `{"id": N, "ok": false, "error": "..."}`.
Logs go to stderr.

Relays: the driver opens one WebSocket per operation, and answers NIP-42
AUTH:
- as the peer's account for its inbox (NIP-17 serves gift wraps only to
  their recipient);
- as a fresh key everywhere else.

| Command | Arguments | Answer |
| --- | --- | --- |
| `hello` | | MDK and OpenMLS pins |
| `peer_new` | `peer`, `secret` (hex) | `pubkey` |
| `publish_key_package` | `peer`, `relays` (tag), `to` (targets; `[]`: made, not sent) | `event` (JSON), `event_id` |
| `fetch_key_package` | `peer`, `author`, `from` | newest kind:30443 `event`; `mdk`: what `parse_key_package` makes of it (leaf/capability extensions, proposals) |
| `parse_key_package` | `peer`, `event` | same view for a given event |
| `create_group` | `peer`, `name`, `description`, `relays`, `admins`, `key_packages`, `welcome_relays`, optional `image` | group `state` and `welcomes` (gift-wrap ids per invitee) |
| `add_members` / `remove_members` | `peer`, `group`, `key_packages` + `welcome_relays` / `members` | state after the merged Commit |
| `update_group_data` | `peer`, `group`, `name`, `description`, `admins`, `image`, `publish` | state |
| `self_update` | `peer`, `group` | state |
| `send` | `peer`, `group`, `text`, `publish` | `event_id` (or the unpublished `event`) |
| `sync` | `peer`, `group` | every kind:445 of the group through `process_message` as a fixpoint: `results`, `failed` (MDK's error and `openmls`: OpenMLS's own verdict on a fresh copy), `state` |
| `fetch_welcomes` / `accept_welcome` | `peer`, `from` / `wrapper_id` | Welcomes found (refusals carry the rumor's kind and tags) / state |
| `state`, `messages` | `peer`, `group` | group view / stored messages |
| `export_secret` | `peer`, `group`, `label`, `context`, `length` | MLS-Exporter output (vector capture) |
| `group_extension` | `peer`, `group` | MDK's 0xF2EE bytes (vector capture) |
| `welcome_bytes` | `content` | whether OpenMLS parses a Welcome |

Commits follow MIP-03: published to the group relays first, merged only once
one relay accepted (else cleared). Welcomes follow MIP-02: one NIP-59 gift
wrap per invitee, sent after the Commit.

The vectors in `libmarmot/tests/test_interop.c` (`MDK_GDE_*`,
`MDK_445_*`) were captured with `group_extension`, `send` with
`"publish": false`, and `export_secret`.
