# lab-blossom — permissive Blossom recipe for the porthome lab CI

> **Status**: recipe only. Deliverable of `nostrc-4f9v`.
> Nothing here is running by default — the operator stands this up
> manually on a lab / CI host that has a public IPv4/IPv6 address.

## 1. Why this exists

The community Blossom set that portable-home v1 depends on is fragile:

| Server                     | Random-bytes upload | PNG-shim upload | Verdict                           |
| -------------------------- | ------------------- | --------------- | --------------------------------- |
| `blossom.sharegap.net`     | 201                 | 201             | baseline anchor of the default set |
| `blossom.primal.net`       | 415                 | 201 (via shim)  | shim required, works              |
| `blossom.band`             | 415                 | 500 (decoder)   | out of scope — full PNG decoder    |

Evidence: `docs/reviews/porthome-hy3e-shim-live-probe-2026-09-25.md`.
For a `min_replication=2` posture we need a second permissive replica
that the operator controls. The community set gives us one (sharegap);
primal is second-best but not always reachable. This recipe adds a
third option under the operator's own DNS.

## 2. What the recipe delivers

- `docker-compose.yml` — hzrd149's reference [`blossom-server`][ref]
  behind an nginx TLS terminator. Sizes to a 4 MiB porthome chunk with
  headroom.
- `blossom-server.config.yaml` — deliberately permissive server config:
  no content sniffing, no thumbnailing, no rewriting, no allow-list on
  pubkeys, no size cap under 8 MiB.
- `nginx.conf` — HTTPS on port 8443 (mapped from container 443), reverse
  proxies to the blossom-server container with `proxy_request_buffering
  off` so BUD-02's body-hash check still sees the exact bytes the pusher
  sent.

The **default** remains the permissive two-machine demo: public host binding on
port 8443, native kind-24242 upload auth, no pubkey allowlist, and public reads.
The 8 MiB cap covers 4 MiB chunks plus ~14 KiB AEAD overhead and the 41-byte shim.

The original `ghcr.io/hzrd149/blossom-server:v1.0.0` tag returns `manifest unknown`.
This recipe therefore uses digest-pinned **6.4.1** with its actual `/app/config.yml`
schema. `upload.maxSize: 8388608` replaces the legacy `MAX_FILE_SIZE` environment
setting; both permissive and private modes use this same enforced cap. SQLite
state lives in `./data/`, while the operator-visible `./blobs/` store is retained.
This is not an automatic migration of an old upstream index.

[ref]: https://github.com/hzrd149/blossom-server

## 3. SSRF pre-check — MUST run on a public IP

`nostr-home-fetch` (the unprivileged porthome fetcher) refuses to
resolve a Blossom URL to a private-range IP (RFC 1918 / RFC 4193 /
loopback / link-local). This is the SSRF guard from `nostrc-ww50`; it
is NOT optional in the pusher's environment. Therefore this recipe
**MUST** be exposed on a public address before the porthome pusher
can talk to it.

Options in decreasing order of "recommended":

1. **Real public IP + real DNS + letsencrypt certificate.** The
   `nginx.conf` in this recipe expects certificates at
   `/etc/letsencrypt/live/<host>/`. Run `certbot certonly --standalone
   -d blossom.lab.example.com` on the host once, then `docker compose
   up -d`. Rotate on the standard 60-day cycle.

2. **Public IP with self-signed cert.** Generate a long-lived self-
   signed pair and bind-mount it in place of the letsencrypt files.
   The pusher's libcurl link uses the system trust store, so operators
   must add the cert to that store (or set `CURL_CA_BUNDLE` in the
   pusher environment). Suitable for a two-machine demo where both
   ends are under the same operator.

3. **VPN or reverse-tunnel giving the lab host a public-looking IP.**
   Terraform / Tailscale / plain wireguard all work; the SSRF pre-check
   only inspects DNS resolution outcome, not routing. If your VPN
   assigns 100.x.y.z addresses (Tailscale's CGNAT range) the fetcher
   still resolves it as "public" — accepted.

There is **no lab-only CLI flag** to bypass the SSRF guard on the
fetcher, and there should not be. If you find yourself wanting one,
your setup is wrong — put the lab Blossom behind a public IP or use
the community set with the shim.

## 4. TLS notes

The upstream `blossom-server` speaks only HTTP. nginx does the TLS.
That split matters because BUD-02's authorization event carries the
sha256 of the **request body**; if TLS were terminated *inside*
blossom-server itself and the terminator re-buffered the body, some
edge cases (gzip pre-processing, HTTP/2 upgrade) could mangle bytes on
the wire. The nginx config sets `proxy_request_buffering off` so the
body is streamed verbatim.

Self-signed cert quickstart:

```
cd packaging/lab-blossom
openssl req -x509 -newkey rsa:4096 -keyout privkey.pem \
    -out fullchain.pem -days 30 -nodes \
    -subj "/CN=blossom.lab.example.com"
# Then in docker-compose.yml, replace the letsencrypt bind mount with:
#   - ./privkey.pem:/etc/letsencrypt/live/blossom.lab.example.com/privkey.pem:ro
#   - ./fullchain.pem:/etc/letsencrypt/live/blossom.lab.example.com/fullchain.pem:ro
```

Do NOT commit the resulting `privkey.pem` or `fullchain.pem`. A
`.gitignore` inside `packaging/lab-blossom/` blocks the common names.

## 5. Pointing the pusher at the lab server

Once TLS is live and the DNS name resolves publicly:

```
nostr-homed-provision push \
    --home-dir /tmp/home \
    --blossom https://blossom.lab.example.com:8443 \
    --blossom https://blossom.sharegap.net \
    --relay wss://relay.sharegap.net
```

Notes on the auto-shim decision (nostrc-si30):

- On first push, the auto-decide helper runs a capability probe against
  each server. Sharegap will return `raw_random_ok=YES`; a well-
  configured lab-blossom will also return `raw_random_ok=YES` (it does
  not sniff bodies at all). The shim stays OFF and the wire is
  minimal.
- If the auto-decide ever flips to ON against this lab server, the
  server config has drifted — check `blossom-server.config.yaml` for
  any newly-enabled `storage.rules` restriction or `upload.requirePubkeyInRule`.
- Force one direction for a specific push with the env var:
  `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 nostr-homed-provision push …`
  (or `=0` to force off).

## 6. systemd alternative (out of scope for this pass)

If your lab isn't running Docker, the same server can be run as a
systemd unit — `blossom-server` 6.4.1 is a Deno app that reads config from
disk and writes blobs into a directory. A `blossom-lab.service` +
`blossom-lab.socket` unit would work. This recipe picks docker-compose
because:

- The upstream image is the tested one.
- Bring-up is 1 shell command; teardown is `docker compose down`.
- No systemd version tax on lab hosts that already run docker for
  the rest of the CI pipeline.

If the lab is systemd-only, translate this recipe by hand: the
container's exposed port + volumes are the whole surface area, and
none of the config depends on docker-specific behaviour.

## 7. What this recipe deliberately DOES NOT include

- **A running blob store.** The `./blobs/` and `./data/` bind mounts are
  directories the operator supplies; the container writes into them.
- **Any real certificates or keys.** `.gitignore` in this directory
  blocks `privkey.pem`, `fullchain.pem`, and `*.key` / `*.crt` by
  convention.
- **DNS zone stubs.** The operator supplies the DNS name in their own
  zone.
- **A CI pipeline that stands this up automatically.** That is filed
  as a follow-up under nostrc-4f9v's parent chain.

## 8. Verifying the recipe

The Compose file and configs are lint-clean:

```
docker compose config           # validates docker-compose.yml
nginx -t -c <(cat nginx.conf)   # validates the nginx snippet
```

Neither command actually starts anything — both are static checks. Add
them to your local pre-flight before touching a real host.

## 9. Opt-in private vault service (VSP-01)

The default above is deliberately **not private**. VSP-01 is an explicit layer:

```sh
cd packaging/lab-blossom
export VSP01_ALLOWLIST=/absolute/path/to/vault-members.txt
export VSP01_CERT_DIR=/absolute/path/to/tls
# TLS directory contains fullchain.pem and privkey.pem.
docker compose -f docker-compose.yml -f docker-compose.vsp01.yml up -d
```

Requires Docker Compose >=2.24.4 (`!override`). The override adds `vsp-auth`,
replaces nginx's configuration with `nginx.vsp01.conf`, **replaces** the public
port mapping with `127.0.0.1:${VSP01_PORT:-8443}:443`, and uses a project-scoped
named volume rather than the permissive demo's bind mounts. Do not apply it to
an already-running permissive project: stop that project first, or use another
`-p` project name and port. Changing modes is not a blob-store migration.

The allowlist is service configuration, not a Nostr event: at most 256 lowercase
64-character hex pubkeys, one per line, with blank lines and `#` comments allowed.
Set `VSP01_ALLOWLIST` to an operator-managed file. Missing/invalid files refuse
startup, and the checked-in empty default denies everyone. Restart `vsp-auth`
to reload membership. Configure relayd with the same membership file,
`auth = "optional"` and its exact public `relay_url`; see
[`apps/relayd/README.md`](../../apps/relayd/README.md#auth-and-private-vault-service-vsp-01).

The private ingress uses upstream's locked BUD-11 verifier for signed kind-24242
auth (signature, expiry, action and server scope), then checks membership and
hash scope **before** any store lookup. Missing, invalid and nonmember credentials
get **401 + `WWW-Authenticate: Nostr`**, for both existing and absent blobs,
including GET/HEAD and uploads. Root JSON and `/.well-known/blossom.json` advertise
`"private": true`. Private responses use `Cache-Control: no-store`, removing
upstream public immutable caching. Byte-exact `application/octet-stream` uploads
are accepted without sniffing/rewriting. BUD-04 mirror remains upstream-validated.

**Never publish the store/auth ports directly or attach untrusted containers to
the private Compose network.** nginx is the security boundary. Membership grants
transport access, not decryption; private read clients must supply get-scoped
auth. The existing fetcher's SSRF/HTTPS guards still apply; this lab does not
bypass them or prove a full porthome client integration. This is an operator lab,
not an audited production service or hosted/release acceptance.

### Disposable §13 h conformance

```sh
packaging/lab-blossom/run_vsp01_lab.sh /absolute/path/to/nostrc-relayd
```

Requires a relayd build with nostrdb storage, Docker Compose, openssl, curl and
Python 3. The runner always selects **both** Compose files, with a unique project,
loopback TLS port, temporary cert/store and public test-vector member (`sk=1`,
never a deployment identity). It starts the real relay daemon and Blossom stack.
On Docker Desktop it reaches the native relay through `host.docker.internal`;
on Linux an isolated host may require `VSP01_RELAY_BIND=0.0.0.0` to reach the host
gateway (temporarily exposing the test relay on the host network).

It verifies member upload/HEAD/GET byte round trips, nonmember existing/missing
GET/HEAD401, upload401 and Nostr challenges, private/NIP-11 markers, no-store,
malformed/stale/tampered/wrong-kind/wrong-relay/wrong-challenge AUTH rejection,
unauthenticated `auth-required:` and nonmember `restricted:` REQ/COUNT/EVENT,
and OK→immediate stored EVENT/EOSE round trips for 30078/445/1059. The authenticated
member differs from the outer event author, as required for gift wraps. Checks
observe protocol frames, not sleep-based backfill completion; relayd still has
its inherited stored-query-only behavior, not live fan-out.

EXIT/INT/TERM cleanup removes only runner-owned containers, volume, network,
relay process and temporary files. Failures stay failures and print logs.
For a manually started private project, `docker compose -f docker-compose.yml
-f docker-compose.vsp01.yml down` retains data; adding **`--volumes` destroys it**.
The permissive default keeps its host bind-mounted data even after `down`.
