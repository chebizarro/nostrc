# libhanami changelog

## 0.7.0 (unreleased)

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
- The push path does not publish manifests yet; that lands separately.

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
