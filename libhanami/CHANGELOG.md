# libhanami changelog

## 0.8.0 (unreleased)

- **0.x behavior/ABI change:** GRASP push/fetch requires one root NIP-11 HTTP
  GET (`Accept: application/nostr+json`) advertising `GRASP-01` before publishing
  state or transferring Git data. Missing support is `HANAMI_ERR_CONFIG`;
  transport/non-2xx HTTP/deadline failures are `HANAMI_ERR_NETWORK`; malformed
  JSON/GRASP field types, oversized documents or missing acceptance criteria
  are `HANAMI_ERR_NOSTR`. Failures never authorize publication or transfer.
  NULL-repository GRASP fetch performs capability preflight; offline callers
  should use `hanami_grasp_parse_clone_url` instead.
- Add owned server metadata to push results and `hanami_grasp_fetch_with_result`:
  supported GRASPs, acceptance criteria, optional curation and a GRASP-05 flag.
  Free with `hanami_grasp_metadata_free`, including before result reuse. Metadata
  remains available on capability/later refusals. GRASP-05 does not change
  announcement-listing policy. Plain NIP-34 and Blossom paths are unchanged.
- Declare nip11/curl dependencies; reuse the shared NIP-11 parser, now exposing
  typed GRASP extension fields. No package-boundary baseline additions.

## 0.7.1 (unreleased)

- Fix intermittent query TIMEOUT with an HTTP-503 relay (nostrc-jgl8o).
  A relay that loses its connection before EOSE/CLOSED is now unanswered at
  the reader's connection-loss notification, including failed upgrades.
  Queries consume EVENT/EOSE/CLOSED and state notifications on the caller
  thread, draining buffered events before finalizing a relay boundary.
- Preserve snapshot verdicts: an event wins; otherwise a refusal outranks
  unreachability; unanswered relays yield NETWORK; only all-EOSE absence yields
  NOT_FOUND. A still-open subscription without a boundary remains TIMEOUT.
- Keep publication transport failures classified as NETWORK even if the relay
  reconnects before the caller consumes the failure. libnostr reports synthetic
  disconnects with `ERR_WEBSOCKET_CLOSED`, distinct from the relay's `OK false`.
- The accompanying libnostr 2.1.1 correction distinguishes HTTP establishment
  from WebSocket upgrade and gates reconnect success/REQ replay on the latter.
  Neither component adds or changes public signatures or wire formats.

## 0.7.0 (unreleased)

`hanami_grasp_fetch` now refuses to update local remote-tracking refs unless
all Smart HTTP advertised ref tips exactly match an authenticated, EOSE-complete
kind-30618 from the clone owner or a maintainer named in the owner's NIP-34
announcement. Missing state, extra/missing refs and mismatched tips are errors.
The additive `hanami_grasp_fetch_with_signer` entrypoint permits a bounded
NIP-42 retry after the relay accepts AUTH with `OK true`; the existing
no-signer entrypoint returns an auth error when challenged.
This is a breaking 0.x behaviour correction (nostrc-0hh85), folded into the
unreleased 0.7.0 MINOR; callers must provide relay URLs. Private repositories
remain unsupported. The `snprintf` bounds correction (nostrc-bze2y) is a
PATCH-class build fix folded into the same unreleased version.

Public refdb write/rename/delete now require relay acceptance and roll back the
local cache on failure. Signer-backed contexts perform NIP-42 AUTH on the caller
thread and retry a refused publication/query once after AUTH OK true. No-signer,
rejection, CLOSED and missing-ACK paths fail visibly. Rapid state writes use
strictly advancing timestamps rather than same-second replaceable-event ties.
This 0.x MINOR behavior change (nostrc-3rxpv) removes cache-only success; callers
must handle publication errors and allow the signer to sign kind 22242.

The supporting libnostr 2.1.0 MINOR adds a current-connection challenge snapshot
accessor and sends kind-22242 events in AUTH envelopes with the existing OK
waiter. Other independently releasable components: no bump.

**Cold clone and fetch (nostrc-v2zyh):**

- New wire format: plaintext repositories publish an OID-keyed **manifest
  chain** on Blossom, referenced by the hanami extension tag
  `["hanami-manifest", <sha256>]` on kind-30618 (docs/libhanami-design.md
  §4.4b). Readers enforce a 64-segment depth limit; writers checkpoint after
  16 links. Row bounds match the sealed snapshot.
- **Behaviour change:** `hanami_clone` now produces a durable, self-contained
  repository. Verified objects are written to the local ODB; refs go under
  `origin` (URL = the `nostr://` URI), along with `hanami.endpoint`,
  `hanami.relays` and a `.git/hanami/index.sqlite` cache. Checkout failure is
  now an error. Blossom/Nostr backends are no longer attached to the returned
  repository. A 30618 without the manifest tag fails with
  `HANAMI_ERR_NOT_FOUND`.
- New public `hanami_fetch()`: incremental manifest import and object
  download into `refs/remotes/<remote>/*`.
- **Behaviour change:** the plaintext ODB `read` re-hashes every object
  against its Git OID and fails on mismatch. Previously only the Blossom wire
  hash was checked.
- **Behaviour change:** `hanami_index_get_by_blossom` returns
  `HANAMI_ERR_INDEX` when more than one OID maps to the hash (for example the
  empty blob and empty tree), instead of an arbitrary row.
- Storage: the SQLite index gains a `hanami_meta` table with
  `schema_version` 1. Unversioned databases are stamped on open, and newer
  versions are refused.
- **Push side:** `hanami_push_to_blossom` publishes objects, then a manifest
  segment, then the tagged kind-30618 through the relay-accepted publish.
  - Success requires the object uploads, the segment upload and relay OK
    true.
  - Segments are deltas of rows not yet published. A full checkpoint is
    written on the first push, after a tagless 30618, or once the chain has
    16 links.
  - Refs not covered by uploaded objects are refused (`HANAMI_ERR_INDEX`).
  - The cache gains `manifest_rows` and `manifest_depth`.
- New public `hanami_push_to_blossom_ex()`: the same push honouring
  `allow_insecure` from a config (loopback/http fixtures).
- **Behaviour changes** in `hanami_push_to_blossom`:
  - publishes HEAD;
  - no longer publishes `refs/remotes/*`;
  - fails on an unreadable local object (previously skipped).
- The `blossom://` transport push now uploads local objects and publishes
  through the same pipeline.
- `hanami_repo_open` (public mode) keeps its cache in
  `<gitdir>/hanami/index.sqlite`. It imports the owner's published chain at
  open, failing visibly on an invalid chain, and its refdb writes publish
  tagged states with segments for objects written through the Blossom ODB.
- Public repository backends now share ownership of the index, Blossom client
  and Nostr context, releasing them when the last backend is freed rather than
  leaking them on each `hanami_repo_open` (nostrc-7fhvv). Retained ODB/refdb
  handles may outlive the repository; freeing either backend first is safe.
- Empty-object reads and empty manifest encoding skip zero-length `memcpy`
  and `qsort` calls with NULL buffers, avoiding UBSan nonnull diagnostics
  without changing their results (nostrc-moymr).
- `hanami_publish_state` carries the signer's published manifest head
  forward.

**Multi-relay query completion (nostrc-sp8zk):**

- **Behaviour change:** one relay's CLOSED no longer aborts a multi-relay
  query (`hanami_nostr_fetch_repo`, `_fetch_state` and the internal
  kind-10063/30618 reads used by `hanami_clone`/`hanami_fetch`). It counts as
  that relay's failure; the query completes on the other relays' EOSE and
  returns the newest event they sent. With no event, a CLOSED is reported
  instead of `HANAMI_ERR_NOT_FOUND`, because the refusing relay may hold the
  event. A relay whose NIP-42 AUTH fails no longer aborts the query either.
- **Behaviour change:** a query CLOSED with `restricted:` now returns
  `HANAMI_ERR_AUTH`, matching publication. Previously it returned
  `HANAMI_ERR_NOSTR`.
- Without a signer, an AUTH challenge no longer stops the query early. The
  relay's eventual CLOSED is classified as above.
- `hanami_clone` documents its relay errors (`HANAMI_ERR_AUTH`/`_NOSTR`/
  `_NETWORK`/`_TIMEOUT`). The clone fixture covers CLOSED, `restricted:`,
  auth-required → AUTH → success, and one CLOSED relay beside an answering one
  on the kind-10063 and kind-30618 reads, through `hanami_clone` and
  `hanami_fetch`.

**Unreachable relays are unanswered, not excluded (nostrc-837ap):**

- **Behaviour change:** a configured relay that cannot be connected
  (refused, failed WebSocket upgrade, never registered) now counts as
  unanswered. Before, libnostr either skipped it, so absence was claimed
  without it, or queued a REQ it never answered (`HANAMI_ERR_TIMEOUT`).
  Queries first wait for each in-flight handshake's outcome, then await only
  established relays.
- With no event returned: `HANAMI_ERR_NOT_FOUND` only when every configured
  relay sent EOSE. Otherwise `HANAMI_ERR_AUTH` (auth-required:/restricted:
  CLOSED), else `HANAMI_ERR_NOSTR` (another CLOSED), else
  `HANAMI_ERR_NETWORK` (unreachable). An event from any reachable relay
  still wins.
- Effect on callers that treat NOT_FOUND as "nothing published yet":
  - `hanami_clone` fails visibly instead of producing an empty clone or
    `HANAMI_ERR_CONFIG`;
  - `hanami_fetch`, push base checks, GRASP push and `hanami_publish_state`
    fail with the relay error;
  - the refdb keeps its cache.

**NIP-65 outbox relays for clone (nostrc-nd9yl):**

- `hanami_clone` reads the owner's kind-10002 relay list (newest event across
  the discovery ∪ announcement relays) and adds up to 4 new write relays (`r`
  tags with no marker or `write`, ws/wss only) to the relay set. That set is
  used for the kind-10063 and kind-30618 reads and recorded in
  `hanami.relays` for `hanami_fetch`. An owner whose server list or state
  exists only on their outbox relays no longer fails with
  `HANAMI_ERR_CONFIG`. A missing or unreadable kind-10002 adds nothing. The
  configured `hanami.endpoint` still comes first.

## 0.6.0 (unreleased)

**Behaviour change (0.x MINOR):** `hanami_clone` no longer uses NIP-34 `clone`
URLs as the Blossom API endpoint (nostrc-g2g37). `clone` URLs are Git remotes
(e.g. GRASP Smart HTTP). The Blossom server now comes from the configured
endpoint or the owner's BUD-03 kind-10063 server list, and must pass
libblossom's capability probe. When no Blossom server is known, clone fails
with `HANAMI_ERR_CONFIG`; when no candidate is a Blossom server, it fails with
`HANAMI_ERR_BLOSSOM`. Announcements that put a Blossom URL in `clone` need a
kind-10063 list or an explicit `hanami.endpoint`. Clone discovery now honours
configured relays and records the selected server as `hanami.endpoint` in the
clone's config.

GRASP pushes now require kind-30618 acceptance from the GRASP root relay before
Smart HTTP transfer. Failed or unconfirmed operations attempt a strictly newer
corrective announcement; the result-bearing API distinguishes transfer and
recovery errors. Per-ref server rejection is no longer reported as success.
This is a 0.x MINOR behavior change (nostrc-fsuei); callers must handle errors
previously hidden by the post-push best-effort publication policy. Other
components: no bump (internal helpers, tests and documentation only).

## 0.5.1 (unreleased)

Fix the libgit2 error-reporting declarations needed to build the sealed ODB and
refdb backends with current libgit2 headers on macOS.

## 0.5.0 (unreleased)

The shared library now hides implementation symbols by default and exports
functions declared in its installed public headers through `HANAMI_API`.
Clients that linked undocumented symbols must migrate to the public API and
rebuild against the 0.5 ABI. No storage or wire format changes are intended.
