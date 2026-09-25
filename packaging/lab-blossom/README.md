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
  any newly-enabled `rules.upload` entry.
- Force one direction for a specific push with the env var:
  `NOSTR_HOMED_BLOSSOM_PNG_SHIM=1 nostr-homed-provision push …`
  (or `=0` to force off).

## 6. systemd alternative (out of scope for this pass)

If your lab isn't running Docker, the same server can be run as a
systemd unit — `blossom-server` is a Node app that reads config from
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

- **A running blob store.** The `./blobs/` bind mount is a directory
  the operator creates; the container writes into it.
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
