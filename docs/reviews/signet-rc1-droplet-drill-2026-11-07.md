# Signet rc.1 disposable Droplet drills — 2026-11-07

## Boundary and verdict

**W4: `nostrc-mk7v1.5`. Procedure evidence, not release-image approval.** Related beads: `nostrc-1cx` (live enrollment), `nostrc-5xqun` (live management admission), `nostrc-onloi` (candidate test-stage failure), `nostrc-24y5k` (Spaces retention assumptions, Low/P3).

All identities, database/backup keys, provisioners, client keys and SSH credentials were throwaway. This was **not the production key ceremony**. No production secrets, production resources, daemon fixes, push or merge were used. Only the assigned branch `signet/rc1-w4-droplet-drills` was changed. Times below are UTC on 2026-11-07.

Candidate source: `7f2aeaecde6dc271d5a8d2754a708c95177652ba`, Signet 0.8.0. Default linux/amd64 runtime image:

```text
signet-drill@sha256:98b163ac2650f1834f53245bb5827b4aa2dbe9e5b195d3d27e47334a7ebb0ad3
```

The candidate **test stage did not pass**: `mgmt_ingress_wire` failed in both SQLite and SQLCipher runs (38 passed / 1 failed in each). `nostrc-onloi` belongs to a separate fixing agent. The owner explicitly authorized using the built runtime stage for disposable procedure drills. TLS regression and SQLCipher concurrent-writer `backup_restore` passed, but neither overrides the failed stage. No GHCR artifact or release tag was published.

The offline bundle was default-only. Its unused passkeys manifest slot referenced the same digest to satisfy the bundle schema; **no passkeys build/profile evidence or publishable two-variant release bundle is claimed**. Docker 28's containerd image store preserved the actual RepoDigest through `docker save | ssh ... docker load`; the classic store did not. The preflight digest check was never bypassed.

## Evidence and command conventions

The companion [sanitized execution receipts](signet-rc1-droplet-drill-2026-11-07-evidence.txt) retain selected command outputs, timestamps, signed-event IDs, failures and cleanup receipts (trailing whitespace normalized). Private raw material was temporary and is not committed. The following command prefixes describe the actual execution, with secret values omitted:

```sh
# Every shell using doctl; no named context was created.
set +x
export DIGITALOCEAN_ACCESS_TOKEN="$(tr -d '[:space:]' < ~/.config/doctl/token)"
# H / R: ssh -i <throwaway-key> -o UserKnownHostsFile=<drill-known-hosts>
#        signet-admin@159.203.99.21 / signet-admin@104.131.179.166
# C: workstation candidate signetctl, protected provisioner file, public operator INI
# N: unmodified nip46_reference_client in workstation linux/amd64 container
# O: host ops prefix below (noninteractive stdin closed for scripted invocations)
sudo docker --config /opt/signet/secrets/docker compose --project-name signet \
  --env-file /opt/signet/config/deploy.env --env-file /opt/signet/current/release.env \
  -f /opt/signet/current/signet/docker-compose.yml --profile ops run --rm -T signet-ops
```

Direct workstation TCP/443 to the approved public relays failed independently in curl/nak. Client execution therefore used workstation-local SSH TCP forwarding through the primary Droplet, with local Docker aliases for the **original TLS hostnames**, to `nos.lol:443` and `relay.nostr.band:443`. Provisioner/client secrets stayed on the workstation; TLS remained end-to-end to the public relay. This proves the workstation client over an SSH egress path, **not direct workstation internet reachability**. No alternate public relay was substituted.

## A–C: image, provisioning, custody and host boundary

Commands: `build-image.sh` for default linux/amd64; `make-bundle.py`; DO firewall creation, volume creation, then Droplet creation with rendered cloud-init; `bind-host.py`; `preflight.sh`; `systemctl start signet-compose.service`; `curl /health`, `/ready`, `/metrics`; `ss -lntp`; `/proc/<pid>/status`, `/proc/<pid>/limits`; `docker inspect` environment.

- Firewall created **06:09:02**, first volume **06:09:06**, primary Droplet **06:09:11**. nyc3, `s-1vcpu-2gb`, `ubuntu-24-04-x64`, 20 GiB ext4. No backups or IPv6. Inbound SSH only from workstation /32; no public signer port. Outbound 443, DNS TCP/UDP 53 and NTP UDP 123.
- Original cloud-init failed: Ubuntu 24.04 has no apt `awscli` candidate. Corrected host initialization ran on primary; a second fresh Droplet's corrected cloud-init completed `done`, `errors: []`.
- With the genuine manifest but volume detached, preflight at **06:16:12** exited 1 (`findmnt` failure), with `NO_ROOT_STATE` and `NO_ROOT_BACKUPS`. Wrong filesystem UUID at **06:20:06** exited 1 (`wrong volume UUID/type`). Correct attachment/UUID and digest passed at **06:22:25**. No root-disk fallback was used.
- `/opt/signet/secrets`: root:root 0700; secret files UID/GID 10001:10001, 0400. Bunker and provisioner public keys were independently derived and compared. The provisioner secret was never installed on either host.
- `/health`, `/ready`, `/metrics` served on loopback. Initial readiness took about **137 seconds**, including failing band NIP-11 discovery. `ss` showed `127.0.0.1:8080`, not wildcard. Workstation probes of public 8080 and 9487 timed out at **06:22:59**.
- Native daemon UID 10001; all capability sets zero; `NoNewPrivs: 1`; `VmLck` approximately 629280–662136 KiB; memlock unlimited; core size 0; `mlockall` succeeded. Docker environment contained file-path selectors, `SIGNET_REQUIRE_ENCRYPTED_DB` and PATH, not secret values.

## D: enrollment diagnosis and live client results

The bounded diagnosis started **07:07:42**. Initial signed status/provision calls timed out despite `/ready=true` and relay EOSE. Before retrying mutations, local encrypted-store inspection showed zero agents. A workstation-only diagnostic CLI build added ACK/subscription metadata and saved encrypted outgoing envelopes; the daemon remained the exact candidate image. This diagnostic build is not release-image evidence.

### `nostrc-5xqun`: confirmed rejection point, limited root-cause attribution

The nos.lol query used kinds `[24133,25910,1059]`, `#p=b8cf710a0293897a136cc7ce43b22a9d91313decdd3cd662f9d266896ce56a39`, and `since=query_time-49h`, matching the daemon's management filter. It returned the requests then EOSE. Candidate libnostr decrypted the stored gift-wraps using the drill bunker key, printing only timestamps, method and public identities:

| Request | Relay ACK | Outer `created_at` | Authenticated rumor time | Store audit |
|---|---|---:|---:|---|
| status `0529f59d7de0348fc40b60a69f8f660b1a4604327ca81b7d1f5b5c25487208a7` | accepted=1 | 1793897853 | 1794035605 | at 1794035606: `mgmt_history_dropped`, `before_live_boundary` |
| second-provisioner provision `cac7c6c8064818dfc52e7184844c545a18f8db1fa28115e212baeb4da3c52969` | accepted=1 | 1793914443 | 1794035862 | at 1794035864: same refusal |

Daemon EOSE was logged at **07:12:00 / 1794035520**, before these fresh requests. Audit rows identify the **outer ephemeral sender**, not the authenticated provisioner. For kind 1059, this locates the refusal at `mgmt_protocol.c:1385–1393`, where `live_since <= 0` rejects before decrypting. The later rumor-time check (`1354–1377`) would record the authenticated sender. Thus **fresh live requests were classified as history because the subscription boundary was unavailable/zero**. The narrower theory that NIP-59's randomized outer timestamp was compared directly with the EOSE boundary is **not supported**: the code explicitly compares the inner rumor time.

`relay_pool.c:714–733` requires the delivering callback subscription to equal the current required subscription, be connected/eosed/not closed, with ready/auth state valid. Which condition was false was not instrumented in the daemon. Do not claim a proven internal pointer/race cause or equivalence with `nostrc-onloi`.

The second provisioner was genuinely authorized using disposable public INI configuration plus SIGHUP: **07:17:37**, `granted=1`, `2 authorized`. Its fresh provision still received ACK true and was history-dropped. A band-only CLI attempt failed relay connection; an independent TLS query timed out during handshake. No AUTH rejection, CLOSED reason or OK false was observed; network failure is not AUTH evidence. Before/after metrics showed zero active agents/sign/auth/revoke operations during the failures; the metrics did not expose the admission-drop count. The SQLCipher hash-chain audit, queried with `PRAGMA query_only=ON`, supplied the decisive refusal evidence.

### Successful single-relay workaround, not a daemon fix

At **07:18:51**, the disposable daemon's relay list was changed to **nos.lol only** through supported INI/SIGHUP. Reload logged `2 -> 1 relays`; EOSE arrived **07:18:52**. Fresh provision `9310df01b6b318476fecdfda017ecd3331d96be212e4dddad08238e9019b7a90` received ACK true and exit 0; store audit at **1794035938** recorded `mgmt_provision_agent / allow / provisioned`, and agents=1.

Later, the freshly started **nos.lol-only restore host** also timed out on signed `list-clients`: its audit at **1794037245** again recorded `mgmt_history_dropped / before_live_boundary` for the outer ephemeral sender, while NIP-46 signing worked. Therefore the defect is **not limited to two relays**. Changing/re-arming the relay set temporarily restored management; simply configuring one relay is not a durable fix. The exact internal subscription-state fault remains unproven. `nostrc-5xqun` stays open. No daemon code was patched and the deployment default relay list was not silently rewritten as a general fix.

Then the **unmodified** CLI/reference client ran:

```sh
C reissue-connect drill-nos-only --out /work/agent-secret
C set-policy drill-nos-only '<explicit client allowlist; connect/get_public_key/sign_event/nip44_encrypt; kind 1; deny nip44_decrypt>'
N /work/client.ini pair
N /work/client.ini deny
N /work/client.ini sign
# Fresh secret and second persistent transport key:
N /work/client2.ini pair
C revoke-client <second-client-public-key>
N /work/client2.ini revoked
```

Pair/sign, authenticated `policy.deny.method`, fresh-process reconnect, second-client pair and authenticated revocation refusal all passed. Representative signed IDs:

- pair: `55ff88ccec0fc52cc227c174c9801919dcc1edc4845bef645e93f4bd25a69d7f`
- fresh client process: `ef5cb82afebfdc15cd79067130380f5fc31127c7c09f4378f8e23a6b5e149fa5`
- second client before revocation: `ac83055a14217c73dcfceb6cf5aa6e128eafabe92186550d3dcad983dadff4e8`

The reference client's `pair`, `deny`, `sign`, `revoked` modes were invoked individually; **the aggregate `test_production_roundtrip.sh` was not run as one command**. Initial local wrapper attempts failed before execution due to a root-only build-directory path and then an incorrectly assembled URI; these are not passing client tests. Corrected execution copied the 0600 input into the disposable workstation container under its executing UID.

`NH_NIP46_LIVE=1 NH_NIP46_LIVE_CONFIG=/work/homed.toml ctest --test-dir /src/build-docker/gnome/nostr-homed -R '^homed_broker_login_nip46_live$' --output-on-failure`: **1/1 passed, 5.39 seconds**. This is workstation Linux-container execution against the live host, not a native macOS test. An earlier invocation failed because its generated config was absent; that receipt is retained. A local configparser exception exposed an already-consumed throwaway connect token in session output; it is redacted from committed receipts and all drill material is destroyed at teardown. No master/provisioner/DO token was exposed.

`C writer-acquire drill-nos-only <first-client-public-key>` returned **epoch 1**. Owner sign succeeded (`5fea39ddd7bea76eb51b6dede684d0f2ab726842f4daa3d5fc4c51d52a13b63a`). A third client explicitly allowed by policy connected successfully, but NIP-44 returned authenticated `crypto operation denied or failed`; list-clients showed that third binding active and the separately revoked second binding inactive. The reference `pair` command consequently exited 1 at its expected-allowed encryption step: this is a **negative writer-fence observation**, not a passing pair suite. Fresh-store owner fencing is not historical writer cutover evidence.

### Paired restart and full reboot

`systemctl restart signet-compose.service` stopped the primary at **07:23:43**. Its immediate preflight refused with 270 seconds remaining; systemd's delayed retry restarted the container. At **07:29:15**, the original persistent client signed without `connect_rpc` or a fresh secret: `d86df56d7edab17fc0f2956c6dc17b1447db418dfc51878951405b6d29448341`.

`systemctl reboot` then changed boot ID from `7a0e75d3-a213-4d3f-9424-1cc8eb717b65` to `dfe81dee-b3a4-47f8-b9d8-c1efaf4c5944`. At **07:29:40** preflight refused with **250 seconds remaining**. The daemon started at **07:34:18**, and readiness was observed at **07:34:39**. After re-establishing the workstation SSH relay tunnel, the existing client signed again: `dfc83987db8d857da5b08c7667275f8e40fd870c5599878f01acbd1f94735070`. Both are single-relay, encrypted candidate-image durability evidence; neither clears the startup management-admission defect.

## E–F: online backup and fresh-host restore

`systemctl start signet-backup.service` ran **06:24:57–06:25:05**, with the daemon running: `Result=success`, `ExecMainStatus=0`. It invoked the real `signet-ops` online encrypted backup and uploaded three objects under:

```text
s3://signet-drill-20261107/signet/20261107T062457Z-819f1098/
```

The manifest recorded the successful off-host reference. Workstation download verified object hashes against checksums and manifest. The selected SQLCipher concurrent-writer `backup_restore` test passed in 4.31 seconds; live backup alone is not a simultaneous-writer stress proof. Configuration/policy administration was kept outside the sampled backup window.

The second host used a fresh 20 GiB volume and its own UUID-bound manifest. Restore commands were `O restore-db /backups/from-spaces/store.db --confirm`, `O verify-audit`, `O list-agents --verify`. At **06:36:40–42**, audit integrity OK, 0 entries; 0 agents / 0 failed verification. This initial empty-store restore was only procedure mechanics; it was not presented as paired-client recovery. The restore daemon stayed stopped while the primary was active.

### Populated restore, with existing and revoked clients

After the paired reboot, a second online service backup at **07:35:01** produced:

```text
s3://signet-drill-20261107/signet/20261107T073501Z-51727768/
store.db SHA256 f6435e0ebda710432288bb57d2bba60e224e026aa5c1f1d64c015c4a1ab35a57
config.tar.gz SHA256 055154c7b69889962ae582fe50895363d1f1cdae4dc44fc2e5238e2349527607
```

Workstation S3 download hashes matched both `checksums.json` and the host manifest (`off_host: true`). The primary was stopped before restore; at **07:36:15** it had no signer container or listener (unit retained failed status, tracked separately). The restore host had never run the signer before this populated restore. Its earlier `.pre-restore` artifact was preserved in a protected directory, and only allowlisted config/policy/audit files were extracted; the primary's volume identity manifest was **not** installed.

`O restore-db /backups/populated/store.db --confirm`, `O verify-audit`, `O list-agents --verify` passed: **55 audit entries; 1 agent, 0 failed verification**, same agent public key. A new stopped quarantine was established at **07:35:24**; systemd start at **07:39:55** followed 270 seconds down, readiness at **07:40:10**. The old workstation client, with no fresh connect secret or pairing call, signed:

```text
a46d2f04a2918f1774eb789abeb3572ac6ee0d32cd1574dcfb3e4d924bc49faa
```

The previously revoked client received authenticated refusal. A query-only SQLCipher check showed the original owner binding active, second binding still revoked at **1794036128**, and third binding active. This distinguishes retained revocation state from merely denying a non-owner under the writer fence. Signed management `list-clients` itself timed out after this startup, with a matching history-drop audit row; that failure is retained, not converted into a management pass. NIP-46 recovery and encrypted custody passed; management recovery remains affected by `nostrc-5xqun`.

Spaces access used the owner's explicit disposable exception: workstation-only full-access setup/cleanup key and a bucket-scoped read/write/delete host key. Versioning and 30-day lifecycle were enabled. **PutObject-only isolation and hostile-host retention are unproven.** DigitalOcean's [access documentation](https://docs.digitalocean.com/products/spaces/how-to/manage-access/) specifies Read/Write/Delete for limited write grants and disallows combining limited keys with bucket policies. Low bead `nostrc-24y5k` requests correcting the runbook/plan: protected version retention or a separate copy under a different credential is required; ordinary versioning alone is not immutable against version deletion. The page's statement that keys cannot be managed by CLI is stale relative to the actual successful doctl commands; the permission description is the relevant citation.

## G: upgrade and rollback

Schema-identical alternate source `4fbac4ea9c04088a8015cff8b2f173587300d8a8` (candidate's parent, documentation/tracker-only source difference), default image:

```text
signet-drill@sha256:4ef581e0b574c54c5401e080a0cd50911a3000fac472713854c5ca69888033de
```

- Ordinary upgrade attempted a registry pull for the unpublished image and refused **before stopping**, preserving active candidate/current symlink.
- First explicit offline upgrade **06:40:15–06:46:49** failed HTTP 503 at the old 120-second readiness budget. Later readiness did **not** retroactively pass it. Missing post-upgrade schema inspection correctly refused image rollback; unconfirmed state rollback refused.
- Confirmed state rollback using `--confirm RESTORE-REVOKED-AUTHORITY` passed **06:50:06–06:56:56**, restoring the pre-upgrade backup and candidate, audit OK (3 entries), agents=0. This recovery preceded the next successful upgrade: a documented sequencing deviation, not a replay of the planned order. It did not prove revocation safety across intentional state rewind.
- Corrected `release.sh upgrade <alternate-bundle> --offline` passed **06:58:19–07:05:12**. Before/after schema version 33 and DDL SHA256 `52a50d7b752a6ab61f449de825b4c6c79d0555334f3ae6738fb3220acd4d9a28` were equal.
- Image rollback started **07:05:12**; candidate current symlink, active service and readiness were observed by **07:13:24**. The session interruption lost its final command-exit receipt: do not claim a captured exit 0. Subsequent client proofs ran on the rolled-back candidate. Both release operations enforce the 270-second stopped interval. These original upgrade/state-rollback runs preceded enrollment.

### Populated-state upgrade after recovery

To replace the interrupted rollback receipt with a complete client-bearing exercise, the alternate image and an independently volume-bound bundle were installed on the restore host. The three corrected deployment helpers were installed alongside immutable candidate bundles. With the primary signer stopped, `release.sh upgrade <alternate> --offline` ran **07:41:15–07:45:57**, returned successfully, and the original client signed `b88c1b238896388fac39e4b454b91aa15b22cea59f28bfd8fe9d15c9f9b4eed3`. The revoked client again received authenticated refusal. This verifies host upgrade plus existing NIP-46 state, not repaired management admission.

The subsequent `release.sh rollback-image <candidate>` also returned successfully. At **07:50:40**, the original client signed `aee9a50b0dc7e914fd489e5e92029336f7da7d99896d311d51ac4e276b0d83a0`, and the revoked client was still denied. The recorded sequence exited **0**. Final inspection showed the candidate current symlink and exact candidate image digest, with readiness true. Both transitions used the unchanged 270-second quarantine. This complete populated upgrade/image-rollback receipt supersedes the earlier missing exit receipt, without relabeling that interrupted run.

## H: failure checks and idle CPU

- Missing master-key file and missing config each caused preflight refusal; originals restored.
- Native candidate startup with missing key, missing config and wrong DB key on the stopped restore host exited 1 at **06:37:22–24**. Database hashes stayed unchanged; wrong-key fixture removed. No plaintext fallback.
- Restart attempted at **06:41:26** refused with **200 seconds remaining** in quarantine. Later explicit systemd restart also refused its immediate start at 270 seconds remaining and used the systemd retry path.
- No relay AUTH rejection occurred, so that conditional failure-table case is untested.
- Idle/low-traffic observation: primary PID 12605, stable process start time, HZ=100, **06:24:30.165800–06:39:30.387325**, **900.221523 seconds**, user+system tick delta **568**. CPU = `568 / 100 / 900.221523 * 100 = 0.630956%` of one core. `top -b` endpoints showed 0.0%. No core-saturating busy loop observed. First ~15 seconds overlapped final startup; read-only status attempts and online backup overlapped, but no successful signing. No DO agent graph was captured; this is not a long-term load test.

## Tooling fixes and focused checks

| Commit | Scoped fix | Verification |
|---|---|---|
| `bf2d9d2a4` | Ubuntu 24.04 AWS CLI in root-owned venv, pinned 1.46.3 | render/shell checks; 13 deployment contract tests; actual host init and fresh-host cloud-init |
| `88ca40aea` | explicit upgrade-only `--offline`, still checks actual pinned RepoDigests before stop | 14 contract tests; invalid-action refusal; live unpublished-image refusal and offline upgrade |
| `23978065f` | bounded readiness budget 180 seconds instead of 120; quarantine stays 270 | 14 contract tests; native failed receipt preserved; corrected live upgrade |

Each tooling delta received one Oracle review; no findings. No daemon changes. `check-release.py --tag signet-v0.8.0-rc.1` passed. These are corrections within the existing unreleased Signet 0.8.0 MINOR line, so no additional bump; documentation/evidence itself requires no version bump. Earlier report-only commits `3dd6a11c5`, `eb4ffdc64` are superseded by this review. Tracker synchronization: configured `bd dolt pull` had no remote; no remote was invented and no push/merge performed.

Additional Low/P3 follow-up `nostrc-udqwi`: an explicit systemd stop can leave `ActiveState=failed` after competing Compose stop/removal reports `No such container`. The signer was actually stopped (no container or listener), and quarantine/restart still worked. This cosmetic/operational failure-state issue was recorded, not made a new acceptance gate or mixed into the three reviewed fixes.

## Blocker disposition (W4 evidence, not W6 release clearance)

| Blocker | Disposition | Evidence / remaining boundary |
|---|---|---|
| 7 — launch enrollment/reconnect | **Partial; release blocker remains** | Real reference-client and homed success after relay-set reconfiguration; client/container/host restart and revoke proved. Startup signed-management history-drop recurs, including nos.lol-only restore (`nostrc-5xqun`). Candidate test stage also failed. |
| 8 — encrypted deployed-image custody | **Runtime drill passed; release review still external** | Native SQLCipher fail-closed checks, paired encrypted container/host restart and populated restore with active/revoked bindings. Independent custody review and a passing release image are not supplied by this drill. |
| 9 — recovery | **NIP-46 recovery drill passed, management limit retained** | Separate host/volume, verified Spaces objects, audit and key verification, existing client sign and revoked-client denial. Signed management after startup remains broken. |
| 10 — mount identity | **Drill passed** | Missing volume and wrong UUID refuse without root-disk state; correct mount/digest passes. |
| 11 — idle core saturation | **Not reproduced** | 900.221523 seconds, 0.630956% one-core CPU; no successful signing load, some startup/status/backup overlap. Not a soak or proof of all relay states. |
| 13 — key ceremony | **Throwaway procedure only** | Protected files and independent public-key checks; production operator ceremony/escrow is still required. |
| 14 — online backup consistency | **Selected consistency test + live procedure passed** | SQLCipher concurrent-writer test passed; running-daemon online export/upload and populated recovery passed. Full candidate test stage failed elsewhere; PutObject-only/hostile-host retention remains unproven (`nostrc-24y5k`). |

## Remaining limits

- `nostrc-5xqun` is not fixed. Confirmed failure point: fresh management rejected with zero/unavailable source-subscription live boundary. Exact internal state/race cause is unproven; do not attribute it directly to gift-wrap backdating or claim the one-relay configuration fixes it.
- `nostrc-onloi` prevents release-image acceptance. These host receipts do not override a failed test stage, and must be repeated as needed on the eventual reviewed/published release artifact.
- Direct workstation relay egress, band relay operation, relay AUTH rejection, passkeys/CTAP, HTTP bootstrap, production key custody, protected long-term backup retention and long-duration soak are outside the passing evidence.
- Initial state rollback was explicitly authorized on an empty-agent snapshot. It is not proof that a populated state rewind preserves later revocations; the confirmation warns that it does not.
- Original image-rollback exit status was lost at interruption; its observed postconditions are kept separate from the later fully recorded populated upgrade/rollback sequence.
- `nostrc-udqwi` tracks the manual-stop failed-state symptom separately. No daemon source was changed; no new broad qualification gate was added.

## Resource inventory and cost basis

Every creation ID was recorded in `nostrc-mk7v1.5` immediately after creation. Names/tags used the `signet-drill` namespace. Spaces keys and SSH keys have names rather than resource tags.

| Resource | ID / name | Created UTC |
|---|---|---|
| Primary Droplet | `616800461`, `signet-drill-primary` | 06:09:11 |
| Restore Droplet | `616803351`, `signet-drill-restore` | 06:25:44 |
| Primary 20 GiB volume | `bd0ea39e-da71-11f1-af9f-dafd37c47a91` | 06:09:06 |
| Restore 20 GiB volume | `0ba40963-da74-11f1-b80e-d67dcaaf751c` | 06:25:37 |
| Firewall | `4fc68123-21fb-4b43-b6f6-405b36d44b8e` | 06:09:02 |
| Throwaway SSH key | `60686858` | before first Droplet |
| Workstation Spaces key | `DO00B7W9GGXU2W46KWJC` | 06:09:30 |
| Host scoped Spaces key | `DO801VXADJHDKC9XTB28` | 06:09:54 |
| Bucket | `signet-drill-20261107`, nyc3 | approximately 06:09 |
| Snapshots | none created | — |

The existing VPC `97f31fe2-0649-47a4-9e6d-a93e62c417fb` was reused and must not be deleted. No named doctl context was created after the owner's authentication correction; the user's token file and default context are preserved.

Price basis: DO size API returned **$0.01786/hour per Droplet**. [Volumes](https://docs.digitalocean.com/products/volumes/details/pricing/) cost $0.10/GiB-month and accrue hourly (20 GiB ≈ $0.0028/hour using a 720-hour month). [Spaces](https://docs.digitalocean.com/products/spaces/details/pricing/) has a $5/month base, prorated hourly if all account buckets are removed (≈ $0.00694/hour on that same approximation). Existing subscription/allotment and billing rounding affect the actual invoice; this is an incremental estimate, not an invoice assertion.

Measured lifetime estimate through deletion:

- Droplets: `(1.714 + 1.438) hours × $0.01786 ≈ $0.0563`.
- Volumes: `(1.724 + 1.449) hours × $0.00278 ≈ $0.0088`.
- Spaces: approximately `1.706 hours × $0.00694 ≈ $0.0118`, if incremental prorated subscription applies.
- Total approximately **$0.08 duration-based; budget approximately $0.10 allowing hourly rounding**. No snapshots, paid backups, new VPC or substantial billable transfer were used.

## Destruction proof

At **07:50:51**, all **6 object versions/delete markers** were deleted and bucket deletion returned successfully. The immediate bucket list was stale, so that failed assertion is retained. One bounded verification retry at **07:51:51** returned an empty drill bucket list and **HEAD 404**. Both Spaces keys were then deleted. Both Droplet deletions were accepted at **07:52:01**; volumes, firewall and SSH key were deleted by **07:52:33**; the tag was deleted at **07:52:37**.

These are actual final command outputs, filtered from full JSON responses by the exact task IDs, `signet-drill` name prefix or tag. Unrelated account inventory is omitted, not deleted:

```text
doctl compute droplet list --output json | task-resource-filter
[]
doctl compute volume list --output json | task-resource-filter
[]
doctl compute firewall list --output json | task-resource-filter
[]
doctl compute ssh-key list --output json | task-resource-filter
[]
doctl spaces keys list --output json | task-resource-filter
[]
doctl compute snapshot list --output json | task-resource-filter
[]
BUCKET_LIST_SIGNET_DRILL []
HEAD_DELETED_BUCKET_HTTP 404
TAG_DELETED signet-drill
NO_SIGNET_DRILL_AUTH_CONTEXT
```

The evidence includes the exact filter and deletion driver. No cloud drill resources remain. Local cleanup completed at **07:54:00**: this drill's SSH tunnel, two proxy containers, Docker network, seven `signet-drill` image tags, temporary clones/build inputs, SSH private key, all throwaway application keys and Spaces credentials were removed. Final task container/image/network lists were empty; `/tmp/signet-drill-20261107.p5S0Ke` and `/tmp/signet-w4-current-path` absence was asserted. A known-secret scan passed before the private material was removed. Shared Docker build cache, unrelated `/tmp` files, other agents' worktrees, the existing VPC and the user's DO token/default context are preserved. No push or merge.

**Handoff:** the disposable execution and teardown are complete; `nostrc-mk7v1.5` retains a blocked disposition for remaining acceptance, specifically `nostrc-5xqun` startup management and `nostrc-onloi` release-image qualification. Passing NIP-46 custody/recovery receipts do not erase those failures.
