# libhanami changelog

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

## 0.5.1 (unreleased)

Fix the libgit2 error-reporting declarations needed to build the sealed ODB and
refdb backends with current libgit2 headers on macOS.

## 0.5.0 (unreleased)

The shared library now hides implementation symbols by default and exports
functions declared in its installed public headers through `HANAMI_API`.
Clients that linked undocumented symbols must migrate to the public API and
rebuild against the 0.5 ABI. No storage or wire format changes are intended.
