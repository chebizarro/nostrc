# Signet: single-host deployment and recovery

This is the W3/W4 operator route for Linux/amd64 Ubuntu 24.04.
The enforced stop rule is **270 seconds**: a request accepted 120 seconds in the
future remains admissible for up to 240 seconds after execution. Quarantine covers
twice the maximum 120-second replay skew plus 30 seconds of margin.
Preflight bounds `[replay] skew_seconds` to
1..120 seconds; zero/unbounded or larger settings are refused. It depends on the
W1 strict config/file-key/loopback changes and W2 relay fixes. Packaging source
and local tests are **not** release acceptance. The owner must execute the live
checks below on a disposable Droplet before approving the release. No cloud
resource is created by CI. Bootstrap is disabled at first launch; CTAP is
experimental. MCP, git-credential, D-Bus and Unix socket transports are not shipped
as supported container surfaces.

## Release inputs and trust

`versions.env` selects Debian/Caddy by registry digest and CMake/Meson and host
Docker/Compose/containerd by version. `build-image.sh OFF|ON` reads these pins;
`--target test` executes the libnostr outbound TLS regression
(`test_connection_tls_no_resumption`: ClientHello settings, untrusted-CA and
wrong-hostname refusal) and BOTH SQLite and SQLCipher Meson suites. Runtime builds
use SQLCipher, disable test hooks and contain only the daemon, CLI, required
libraries, curl and a read-only SQLCipher inspection tool. libcbor is installed
only for the passkeys variant. Package inventories and both `ldd` reports are in
`/usr/share/signet/`. Apt inputs are recorded, not claimed byte-reproducible.

`signet-release.yml` checks all four authoritative version sources, calls Signet
CI, tests and scans both images before publishing either. Manual builds use
`sha-<commit>` identity, never a version tag. Tagged releases use `signet-v*`.
It pushes the **same image tar** that was scanned, then attaches SPDX SBOM and
build provenance and emits digests plus `signet-deployment-bundle`. An attestation
or second-variant publication failure is a failed/incomplete release, even if
an image has already reached GHCR. Never deploy a partially published run.
Vulnerability exceptions require `id`, `finding`, `reason`, and an ISO `expires`
date in `vulnerability-exceptions.json`; expired entries fail release checks.

`ghcr.io/chebizarro/nostrc-signet` is **public** (owner-approved 2026-11-07):
the repo is public and the image carries no secrets. Droplet pulls are anonymous;
no read-only package credential or registry login is required. Keep the digest pin.
Image integrity relies on digest pinning and provenance attestation, not visibility.
CI obtains an anonymous GHCR pull token and probes the candidate tag before push:
HTTP 200 accepts an existing public image; 401/403/404 allow push for an absent or
inaccessible image. After push and digest capture, the anonymous tag manifest
**must return HTTP 200**, and its SHA-256 digest must equal the captured push digest
before attestations run. Any other post-push status or digest mismatch fails.
A token-endpoint 401/403/404 makes the manifest probe run without a bearer token.
Transport errors, malformed successful token responses and unexpected token or
pre-push manifest HTTP responses fail closed. GitHub defaults new packages to
private; the owner must make a newly created package public for the post-push gate.
No extra secret is required. An optional `GHCR_READ_TOKEN` repository secret
(classic PAT, `read:packages`) only enriches the pre-push check with user-owned
package REST visibility metadata; an unavailable lookup warns. The repository
`GITHUB_TOKEN` is used for push, not that lookup.
See [GitHub's GHCR defaults and anonymous access documentation](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry).
Source/image provenance and release-bundle checksum verification are owner
requirements before install. The legacy `release.yml` /
`configure-release.sh` route intentionally still excludes Signet. The scoped
`update-inventory.py` regenerates its selector publisher/package metadata; it
never rewrites unrelated inventory fields.

## Host creation (operator only)

Install doctl, Python 3, PyYAML and optionally cloud-init schema validation on the
operator workstation. Manifest validation in `ops.py` and the CTest/Meson
`deployment_contract` test also require the Python 3 `jsonschema` module:
install `python3-jsonschema` on Debian/Ubuntu, or `python3 -m pip install
jsonschema` in a virtual environment (use its `bin` directory first on `PATH`
for tests). Missing `jsonschema` is an explicit failure, not a skip; importing
`ops.py` or requesting `--help` does not require it. The Linux CI image already
installs this package, and cloud-init installs it on the deployment host.
Authenticate doctl locally; no credential is accepted in
user-data. Inspect installed `doctl ... --help` and the commands before use:

```sh
export DO_REGION=nyc3 SIGNET_HOST=signet-rc
export DO_SSH_KEY_ID=123456 DO_VPC_ID=<approved-vpc-uuid>
export OPERATOR_CIDR=<operator-ipv4-cidr>
export OPERATOR_SSH_PUBLIC_KEY_FILE=$HOME/.ssh/id_ed25519.pub
bash signet/deploy/digitalocean/create-droplet.sh --create
```

The script creates a tag and tag-scoped firewall, then a 20 GiB ext4 volume, then
an `ubuntu-24-04-x64`, `s-1vcpu-2gb` Droplet. Inbound is operator SSH only. Outbound
is TCP 443, UDP/TCP 53 and UDP 123. Ubuntu/Docker apt repositories use HTTPS; port
80 is not opened. IPv6 and provider backups are **not enabled**. Resource IDs and
rendered public user-data are retained in a unique local receipt directory.
Failures do not automatically delete resources; use those IDs to reconcile them.
Do not blindly rerun creation against existing names.

`cloud-init.yaml` is a template. The renderer inserts the public SSH key, volume
ID, pinned package versions and the exact repository systemd units. On Linux with
`cloud-init` and PyYAML installed, run the same schema-only check as CI:

```sh
bash signet/deploy/digitalocean/validate-cloud-init.sh
```

This validates both the raw template and temporary user-data produced by the same
renderer used by `create-droplet.sh`, with a throwaway public SSH key and placeholder
volume inputs. The rendered check includes the real `write_files` set (systemd
units, volume ID and host-init script); it does not create resources or execute
host initialization. The raw template alone is schema-valid but is not deployable
user-data. When `cloud-init` is installed on the operator workstation,
`create-droplet.sh` also validates the actual rendered receipt before sending it
to DigitalOcean.

Volume formatting is performed by `doctl --fs-type ext4`, **not** by cloud-init: cloud-init
requires the named DO by-id device already to be ext4, records its UUID, and
mounts it via UUID without `nofail`. It never formats an unknown attached disk.
Only after the mount succeeds are `state` and `backups` created. This deliberately
avoids a second `fs_setup` that might destroy an existing recovery volume.
Inspect `cloud-init status --long`, `/etc/signet-volume-{id,uuid}`, `findmnt`,
`docker version`, `docker compose version`, and `dpkg-query -W do-agent` over SSH.
Ubuntu 24.04 has no `awscli` apt candidate. Cloud-init installs the pinned
`AWSCLI_VERSION` into `/opt/signet-tools/awscli` using `python3-venv` and exposes
its root-owned launcher at `/usr/local/bin/aws`; verify `aws --version` too.
`--enable-monitoring` installs the DO agent; its absence requires owner repair.
The Signet unit and backup timer remain disabled until custody is ready.

Configure DO resource alerts for CPU/memory/disk and an alert on
`signet-backup.service` failure / backup age >24 hours. Resource alerts are not
signer readiness. Do not enable the timer without monitoring and restore proof.

## Layout and first-boot custody ceremony

| Path | Required ownership/mode and purpose |
|---|---|
| `/opt/signet/releases/<40-char-commit>` | root, not group/world writable; installed release bundle |
| `/opt/signet/current` | root-controlled symlink to selected bundle |
| `/opt/signet/config` | 10001:10001, 0750, **rw directory** for D2 atomic INI replacement |
| `config/signet.conf`, `config/deploy.env` | 10001:10001, 0640; non-secret |
| `/opt/signet/secrets` | root, 0700; no provider root-disk backups |
| `db.key`, `bunker.nsec`, `backup.key`, optional `passkey-sync.key` | 10001:10001, 0400; individually mounted ro |
| registry / Spaces credentials | root, 0400; never mounted into daemon or ops |
| `/srv/signet` | required DO volume, checked by UUID, by-id device and provisioned volume ID |
| `state`, `backups` | 10001:10001, 0700 |
| `/run/lock/signet-maintenance.lock` | host maintenance serialization |

1. Generate the master key (at least 32 random bytes, hex) and bunker nsec on a
   trusted workstation. Generate an **independent** backup key. Keep the
   provisioner nsec on the operator workstation; it never goes into signetd.
2. Independently verify bunker/provisioner public keys. Escrow master and bunker
   recovery material off-host; escrow backup key separately from backup objects.
3. Transfer files over SSH to protected temporary paths; install with the modes
   above. Do not pass values in argv, user-data, env files, tickets or logs.
4. Recover and verify the CI deployment bundle; install it under
   `/opt/signet/releases/<commit>`, root-owned. Executable scripts need mode 0755.
   Never install a bundle containing symlinks or group/world-writable files.
5. Create the INI from the current `signet/signet.conf.example`, with these
   production values (public keys and relays are site-specific):

```ini
[server]
health_port=8080
[nostr]
relays=wss://nos.lol,wss://relay.nostr.band
bunker_pubkey=<64-hex-public-key>
provisioner_pubkeys=<initial-provisioner-public-key>
[store]
db_path=/data/signet.db
[policy_defaults]
policy_file=/data/policies.conf
[audit]
path=/data/audit.jsonl
[bootstrap]
port=0
[passkeys]
enabled=false
virtual_ctap=false
```

Create an empty default-deny `/srv/signet/state/policies.conf` as 10001:10001 0600.
Do not enable D-Bus, NIP-5L or ssh-agent. The daemon creates the encrypted database
and audit file; absence of a key must never generate replacement custody material.
`deploy.env` uses unquoted `KEY=value`, no expansion, and contains:

```dotenv
SIGNET_HEALTH_PORT=8080
SPACES_ENDPOINT=https://nyc3.digitaloceanspaces.com
SPACES_BUCKET=<approved-single-bucket>
SPACES_PREFIX=signet
```

6. Bind the release to this host and inspect the resulting **non-secret** manifest:

```sh
sudo python3 /opt/signet/releases/<commit>/signet/deploy/bind-host.py \
  /opt/signet/releases/<commit> --volume-name signet-data --config-revision <review-id>
sudo ln -s /opt/signet/releases/<commit> /opt/signet/current
```

`manifest.schema.json` describes source/component version, all image digests,
package selections, volume ID+UUID, profiles, config hash/revision, public IDs,
backup reference and before/after store-schema observations. `release.env` belongs
to the bundle, not site config. Only operational backup/upgrade fields in the
root-owned manifest change; executable bundle contents remain immutable. Config
hash is a captured revision, not a permanent equality assertion: D2 writes the INI.

7. Pull selected digests **before** activation using both env files:

```sh
sudo docker --config /opt/signet/secrets/docker compose --project-name signet \
  --env-file /opt/signet/config/deploy.env --env-file /opt/signet/current/release.env \
  -f /opt/signet/current/signet/docker-compose.yml pull signetd
sudo /opt/signet/current/signet/deploy/preflight.sh
sudo systemctl enable --now signet-compose.service
```

systemd is the **only** supervisor; Compose has `restart: "no"`, host networking,
no ports, no added daemon capabilities, read-only root, core limit zero, unlimited
memlock, bounded tmpfs, and rotated operational logs. Verify actual memlock works
without `CAP_IPC_LOCK` on the Droplet (`/proc/<pid>/status` and `limits`); a warning
is not proof of locked memory. `/health` is liveness; `/ready` includes relay
catch-up. Relay outages must not trigger process recreation.

D2 remains desired-state configuration. Runtime provisioner grants/revocations
are written back to the rw INI. Never set `SIGNET_PROVISIONER_PUBKEYS` in production;
never concurrently edit its INI list and issue signed grant/revoke requests.
Observe W1 write-back critical logs, metrics and `/ready` detail. This layout
accepts host-network isolation loss and same-host proxy-header impersonation.

## Enrollment and live acceptance

Use workstation `signetctl -c operator.conf` with a protected
`SIGNET_PROVISIONER_NSEC_FILE` and the bunker **public** identity. Provision a
throwaway launch agent, set its explicit policy, and retain its bunker URI only
in an owner-only file. The reference-client policy must allow `connect`,
`sign_event`, `nip44_encrypt` and the chosen signing kind, and deny
`nip44_decrypt`. Use separate identities for separate consumers. Do not blindly
retry a timed-out management mutation: inspect its resulting state first.

Build `nip46_reference_client` through CMake or Meson. Create a 0600 file owned
by the test-running user:

```ini
[client]
bunker_uri=<single-use-secret-bunker-URI>
secret_hex=<persistent-client-transport-key-hex>
agent_pubkey=<expected-agent-public-key>
allowed_kind=1
```

Set `SIGNET_LIVE_RELAYS` to the exact comma-separated URI relay list,
`SIGNET_LIVE_CONFIG` to that file and `SIGNET_HEALTH_URL=http://127.0.0.1:8080`.
Provide executable paths `SIGNET_DAEMON_STOP`, `SIGNET_DAEMON_START` (operator SSH
wrappers calling systemd) and `SIGNET_REVOKE_CLIENT` (a workstation wrapper issuing
an actual signed `signetctl revoke-client` for this client). Run:

```sh
signet/tests/integration/test_production_roundtrip.sh /path/to/nip46_reference_client
```

This is destructive to that test binding and restarts the selected daemon.
Missing relays/daemon/config fail, not skip. It pairs, verifies a real signature
and expected identity, checks an authenticated policy refusal, starts another
client process without re-pairing, stops the daemon, enforces 270 seconds down,
restarts, signs again, then demands authenticated denial of an operation proven
allowed before revocation. Timeout is never accepted as revocation proof. Capture
printed signed-event IDs as durable non-secret evidence. Transport/EOSE/AUTH/OK
handling stays in the shared NIP-46 client; this is not independent verification
of that library. Also run `homed_broker_login_nip46_live` with `NH_NIP46_LIVE=1` and
its protected `NH_NIP46_LIVE_CONFIG` as specified by that test. Writer-acquire
owner/other-client denial needs its own owner drill; fresh storage is not historical
writer-cutover proof. HTTP `/auth` is not this enrollment path.

CMake registers the live test disabled unless `SIGNET_DEPLOYMENT_LIVE_TESTS=ON`;
Meson registers it with `-Dsignet_deployment_live_tests=true`. Ordinary
`deployment_contract` is explicitly source/refusal-unit evidence, not live proof.

## Online backup, retention and recovery

Before enabling online daily backup, require the concurrent-writer
`backup_restore` test to pass in the selected SQLCipher image. It invokes the real
`signetctl backup-db` in a second process while a store connection provisions,
rotates credentials and appends audit records; restored key/pubkey consistency,
row-count boundaries and audit chain must hold. Plain-SQLite skips are not proof.
If this fails, **do not enable online backup**: owner must use an offline backup
and the same >=270-second stop rule, or have the underlying store issue fixed.

`backup.sh` locks maintenance, validates the running digest, exports from
`signet-ops` (UID 10001, network none, no bunker/provisioner key), verifies encrypted
SQLCipher integrity, and stages a unique backup. It archives only the INI/site
settings, policies, manifest and audit JSONL. Files are sampled separately from
the DB snapshot: avoid concurrent policy/provisioner administration during this
window; this is not a cross-file atomic snapshot. Protect staging as secret data.
Logs/config must never contain secret values. Secrets are not in the archive.

Install root-only `secrets/spaces.credentials` in AWS credentials-file format
with a single-bucket **PutObject-only**, no-delete credential. Set bucket lifecycle
expiry to 30 days using a separate owner credential. The host cannot issue DeleteObject, but PutObject can overwrite known object
names. This is **not immutable retention**: host compromise can destroy reachable
recovery objects by overwrite. Before relying on hostile-host recovery, arrange
independently controlled archival or storage-side immutable/versioned retention
that the upload credential cannot undo; prove that mechanism in the restore drill. Enable `signet-backup.timer` only after a successful manual
`backup.sh`, verified upload and fresh-host restore. Failed uploads leave local
staging and the previous successful reference intact; alert, do not report an
off-host backup. Monitor staging disk use and remove only confirmed-off-host
staging older than the chosen local retention (recommended seven days). Audit
JSONL is retained in 30-day backup bundles; manage on-host archival only while
preserving open-file semantics. Rotated Docker logs are not the audit record.
RPO 24 hours / RTO 60 minutes are **targets**, confirmed only by measured drills.

For a lost host: provision an approved replacement volume; recover the verified
bundle and independent master/bunker/backup keys from escrow; bind a new host
manifest. Keep the daemon stopped. Download the selected objects on an operator
machine using a separate read credential and verify checksums before transfer.
Run all local store commands via the ops service, never host-root signetctl:

```sh
# Include optional overlays if enabled. Both env files are mandatory.
sudo docker --config /opt/signet/secrets/docker compose --project-name signet \
  --env-file /opt/signet/config/deploy.env --env-file /opt/signet/current/release.env \
  -f /opt/signet/current/signet/docker-compose.yml --profile ops run --rm -T \
  signet-ops restore-db /backups/<backup>/store.db --confirm
# Repeat the same prefix for: verify-audit ; list-agents --verify
```

Restore `signet.conf`, `deploy.env`, `policies.conf`, `audit.jsonl` with their
recorded ownership from the allowlisted bundle (do not blindly extract untrusted
tar paths). Restore bunker identity separately; never restore the old host's
volume identity manifest. Preserve `.pre-restore`; before a subsequent deliberate
restore move earlier recovery artifacts to a named 0700 directory. Establish a
new stopped interval with `python3 .../ops.py quarantine`; wait >=270 seconds and
start through systemd. Verify old client signing, bindings, revocations, credentials,
audit chain and public bunker identity. A raw volume snapshot or live DB copy
without WAL is not an application-consistent backup.

## Upgrade and rollback

Install and bind the candidate bundle; do not change `current` by hand. Run
`release.sh upgrade /opt/signet/releases/<commit>`. It locks maintenance, pulls and
inspects the candidate first, validates config, stops the daemon, makes a protected
pre-upgrade backup, records the schema, switches the symlink, enforces the stopped
interval, starts and checks health/readiness/digest, then records the new schema.
A failed pull changes nothing. After start, readiness has a bounded 180-second
budget, separate from the 270-second stopped interval; live two-relay startup
has taken 137 seconds while an unavailable relay's NIP-11 lookup completed.
A readiness timeout remains a failed operation, even if the daemon becomes ready
later. A failed later step stays failed and preserves artifacts; there is no
automatic state rollback. Complete acceptance with signed
management and the existing reference client from the workstation.

For an explicitly approved unpublished-image drill, use `release.sh upgrade
<bundle> --offline` only after transferring verified image archives. This skips
registry access, not digest verification: preflight must find every selected
image by its manifest digest before the current daemon is stopped. The default
route still pulls. Docker 28's classic image store may lose `RepoDigests` on
`docker load`; use the supported containerd image store on a fresh drill host
(`{"features":{"containerd-snapshotter":true}}` in `/etc/docker/daemon.json`,
then restart Docker) and verify the exact `RepoDigests` after import. Do not
switch an existing production host's image store as part of an upgrade: that
hides its old containers/images. Never substitute a tag or config ID for a
missing repository digest.

`release.sh rollback-image <previous-bundle>` requires the recorded before/after
schema **and current schema** to match. The observation is SQLite's schema cookie
plus a hash of ordered DDL, not a claim of an application migration-version API.
Unknown/changed schema refuses image rollback. For explicit state rollback:

```sh
sudo /opt/signet/current/signet/deploy/release.sh rollback-state \
  /opt/signet/releases/<previous-commit> --confirm RESTORE-REVOKED-AUTHORITY
```

It preserves candidate DB/config, verifies backup checksums, restores through the
previous image's ops service, restores only allowlisted config/state files,
verifies audit/keys, and waits >=270 seconds before start. This **loses intervening
grants, revocations, writer leases and bindings**. Owner confirmation and a
post-rollback authorization review are mandatory. Never run two versions on one
store. Stop receipts are on the volume; crashes record a receipt in ExecStopPost,
and unclean reboots require at least 270 seconds since boot. Direct `docker start`
or editing receipts bypasses the rule and is unsupported.

## Optional profiles

* Passkeys: bind host manifest with `--profile passkeys`; this selects the passkeys
  digest and mounts `passkey-sync.key`. Set the runtime INI explicitly and verify
  python-fido2 artifacts before advertising it. PSK is single-instance; no sync.
* Bootstrap: `--profile bootstrap`, port 9487 in INI, site hostname in deploy.env,
  Caddyfile copied to config, separately owned `/srv/signet/caddy/{data,config}`.
  Before first activation, add `-f .../signet/deploy/compose.bootstrap.yml
  --profile bootstrap` to the pull command and pull **both signetd and caddy**.
  Upgrades automatically pull and inspect every enabled supervised image.
  Caddy alone binds public TCP 443 as root with only NET_BIND_SERVICE; admin API,
  HTTP redirect listener and HTTP/3 are disabled. Only three enrollment routes
  proxy to loopback; X-Forwarded-Proto is overwritten. TLS-ALPN-01 uses 443. If 443
  is source restricted, use reviewed DNS-01-capable Caddy with a narrowly scoped
  DNS secret and a newly pinned digest; never open enrollment just for issuance.
  No supported HTTP-session consumer exists for this launch: keep it disabled.
* CTAP: add `--profile ctap` **and** passkeys; configure virtual_ctap and the actual
  UHID group in deploy.env. Missing `/dev/uhid` fails preflight. No privileged mode.
  A Droplet UHID device is not an authenticator attached to a remote browser.

Caddy avoids a separate renewal daemon. nginx/DOCR are alternatives, not maintained
second deployment routes. DOCR read-only short-lived login and manual GC are owner
operations; never silently fall back from GHCR.

## Required owner failure drills

| Fault | Required result / evidence |
|---|---|
| Missing master/bunker/config | preflight and real entrypoint refuse; no generated identity/default startup |
| Missing/wrong volume | mount/UUID/by-id/ID refusal before state directory or DB creation |
| Config not writable | preflight refuses; W1 write-back failure logs/metric/readiness detail remain visible |
| Restart under 270 s | preflight refuses; receipts include failed/forced stops |
| Wrong DB key / SQLCipher absent | real daemon refuses; no replacement database |
| AUTH rejected / relays absent | reason without secrets, readiness false, no busy-spin/restart loop |
| Enabled listener fails | daemon exits and unwinds borrowed dependencies safely |
| Registry unavailable | cached running digest remains; candidate pull changes nothing |
| Disk full/upload failure | prior backup preserved, failure visible, no false off-host success |
| Stop exceeds 30 s | retain systemd/Compose kill evidence; verify integrity and readiness before use |
| UHID missing / wrong passkeys image | optional profile refuses; baseline unaffected |
| Off-host probes | IPv4 8080/9487/47472 unreachable; no IPv6 until separately enabled |
| Restart/restore | same bunker identity and client binding; revoked clients remain refused |
| Schema change | image rollback refused; state rollback only with explicit confirmation |

Capture `ss -lntp`, `/proc` UID/capabilities/NoNewPrivs/VmLck/limits, redacted image
inspection, version/digests, observed idle CPU, signed-event IDs and restore duration.
Do not record keys or bunker URIs. Check boot and container restart plus deliberate
volume removal on the disposable host. The owner performs these drills and the
independent review before tag approval; local builds cannot stand in for them.

References: [DO create flags](https://docs.digitalocean.com/reference/doctl/reference/compute/droplet/create/),
[Caddy global options](https://caddyserver.com/docs/caddyfile/options),
[deployment plan](../../docs/plans/signet-production-deployment-2026-11-06.md).
