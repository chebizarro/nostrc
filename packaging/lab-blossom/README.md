# lab-blossom — private VSP-01 reference lab

Implements shared-vault composition §7 / D14. This is an operator-started lab,
not an audited production service or a deployed acceptance claim.

## What runs

- `blossom-lab`: hzrd149/blossom-server **6.4.1**, pinned by image digest.
  The old `v1.0.0` image reference is unavailable. The YAML now uses 6.4.1's
  actual schema. Uploads use the declared Content-Type; the wildcard retention
  rule accepts `application/octet-stream`, with an 8 MiB cap and no sniffing,
  optimization or rewriting. BUD-01 GET/HEAD, BUD-02 upload/delete/list and
  BUD-04 mirror are enabled. Media processing, reports and landing UI are off.
- `vsp-auth`: nginx `auth_request` endpoint, using the pinned upstream's real
  BUD-11 kind-24242 verifier (signature, expiry, action and server scope).
  It checks an operator allowlist before any store lookup, including GET/HEAD
  for missing hashes. It also checks optional GET hash scope and required
  upload/delete hash scope. Unauthenticated, invalid and nonmember requests
  uniformly get **401 + `WWW-Authenticate: Nostr`**, never existence-dependent
  403/404. BUD-04 body/hash validation remains with the upstream mirror handler.
- `nginx`: TLS ingress. Public JSON server-info at `/` and
  `/.well-known/blossom.json` advertises `"private": true`. The port-80 listener
  is only on the Compose network, for isolated conformance tooling. All responses
  use `Cache-Control: no-store`; upstream's public immutable caching is removed.

**Do not publish the upstream store or auth service ports.** nginx is the
security boundary; a direct store URL bypasses private reads. Do not attach
untrusted containers to this lab's Compose network.

## Operator setup

Create an allowlist outside the checkout: one lowercase 64-character hex pubkey
per line (up to 256), with blank lines and `#` comments allowed. Missing/invalid
files fail startup; empty files deny everyone. This is service configuration,
not a protocol event. Restart the services to reload changes. The checked-in
`allowlist.txt` deliberately grants nobody access.

```sh
cd packaging/lab-blossom
export VSP01_ALLOWLIST=/absolute/path/to/vault-members.txt
export VSP01_CERT_DIR=/absolute/path/to/tls
# TLS directory must contain fullchain.pem and privkey.pem.
docker compose config     # static only, not verification
docker compose up -d
```

The default published address is **127.0.0.1:8443**. Use a deliberate Compose
port override and a trusted certificate to expose a real lab hostname. No login
keychain changes are needed. Never commit private keys or blob data. Use the
same allowlisted member identities in relayd's `vault_allowlist` file; configure
relayd's exact public `relay_url` and `auth = "optional"` as described in
[`apps/relayd/README.md`](../../apps/relayd/README.md#auth-and-private-vault-service-vsp-01).
Membership permits transport access, not decryption. Existing porthome fetcher
SSRF and HTTPS guards still apply; this lab does not bypass them or prove a
full porthome client integration. Clients must supply get-scoped auth on reads.

Persistent blobs and SQLite state live in a project-scoped named volume.
`docker compose down` retains it; **`docker compose down --volumes` destroys it**.
Use a unique `-p` project name when running alongside other agents.

## §13 h conformance (disposable)

Build `nostrc-relayd` with nostrdb storage, then run:

```sh
packaging/lab-blossom/run_vsp01_lab.sh /absolute/path/to/nostrc-relayd
```

Requires Docker Compose v2, openssl, curl and Python 3. The runner creates a
unique Compose project, volume/network, self-signed certificate, temporary relay
store and a public test-vector member (`sk=1`, **never a deployment identity**).
It starts the actual relay daemon and pinned Blossom stack. On Docker Desktop,
containers use `host.docker.internal` to reach the native relay. On Linux, set
`VSP01_RELAY_BIND=0.0.0.0` if the host gateway cannot reach loopback; this exposes
the temporary test listener to the host network, so use an isolated lab host.

Checks include:
- Private server-info and NIP-11 auth/vault markers.
- Nonmember and unauthenticated GET/HEAD of existing **and** missing blobs → 401
  with a Nostr challenge; nonmember upload → 401.
- Member byte-exact octet-stream upload, HEAD and GET; scope violations denied;
  member GET of a truly absent object stays 404.
- Unauthenticated protected/mixed/broad/ids-only REQ and COUNT → `auth-required:`;
  valid AUTH of a nonmember → `restricted:` for reads and writes.
- Invalid AUTH kind, URL, challenge, timestamp and signature rejected, with
  correlated OK replies; malformed AUTH gets an explicit failure.
- Member publication and immediate stored-query round trip of 30078/445/1059,
  observing OK and EVENT/EOSE, not sleeps. Outer EVENT authors differ from the
  authenticated member, as required for NIP-59 gift wraps.

EXIT/INT/TERM cleanup tears down only the runner's project containers, volumes,
network, relay process and temporary files. Failed runs print service logs and
remain failures; readiness retries do not retry conformance assertions. This is
local integration evidence, not hosted/release qualification or live fan-out
(relayd currently ends stored queries at EOSE).
