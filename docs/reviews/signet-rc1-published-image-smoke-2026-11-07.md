# Signet published rc.1 image smoke — 2026-11-07

## Verdict and boundary

**PASS for the requested published-image startup-admission smoke.** Task: `nostrc-44q4r`, discovered from `nostrc-mk7v1.7`; live-verification notes appended to `nostrc-5xqun`. Both clean starts used exactly `wss://nos.lol` and `wss://relay.nostr.band`, without SIGHUP or relay-list changes. The earlier startup-admission failure did not recur for fresh requests.

**Limit:** band did not provide a usable daemon subscription in either startup window. This proves the two-relay **configuration**, duplicate subscriptions on the reachable relay, and operation despite the unavailable peer—not two healthy relays, band delivery, or NIP-42 rejection handling. Workstation clients used SSH egress through the Droplet, with TLS end-to-end to the original relay hostnames.

All times below are UTC on 2026-11-07. The Droplet existed from **11:01:31 to deletion acceptance at 11:17:55.535**, approximately **16m25s**, below the 60-minute deadline of 12:01:31. All four resource lists were confirmed empty by **11:18:49**. Only disposable identities and resources were used; no Spaces, production secrets, production keychains, daemon fixes, push, merge, master or tag changes.

| Check | Result | Executed evidence |
|---|---|---|
| Published image provenance and bundle | PASS | `gh attestation verify` exit 0; bundle checksum OK; successful release run 42363870599 |
| Provision, anonymous digest pull, preflight | PASS | cloud-init done/errors `[]`; normal registry pull; preflight exit 0 at 11:05:21 |
| Ready and signer listener boundary | PASS | `/ready=true`; signer only at `127.0.0.1:8080`; SSH remains public as intended |
| First-start fresh management | PASS | status exit 0 at 11:08:12; provision exit 0 at 11:08:25; audit allow |
| Pair → allowed sign → denied operation | PASS | pair 11:09:10; sign 11:09:34; authenticated policy denial 11:09:40; all exit 0 |
| Restart quarantine | PASS | immediate restart exit 1, 270s refusal at 11:09:56; automatic allowed start 11:14:32 |
| Paired sign and fresh management after restart | PASS | no re-pair; sign exit 0 at 11:16:58; status exit 0 at 11:16:59 |
| Revoke → denied | PASS | management revoke exit 0 at 11:17:16; authenticated refusal of previously allowed encryption at 11:17:19 |
| Teardown | PASS | final filtered Droplet/volume/firewall/SSH-key lists `[]`; local disposable material removed |

## Artifact and provenance

Exact runtime artifact, on the Droplet and for workstation `signetctl`:

```text
ghcr.io/chebizarro/nostrc-signet@sha256:6aff3456f7926cab6ab61d3335ba77ea06b549377da36bc74fb8a235295fe1ef
```

Commands, before provisioning:

```sh
gh attestation verify oci://ghcr.io/chebizarro/nostrc-signet@sha256:6aff3456f7926cab6ab61d3335ba77ea06b549377da36bc74fb8a235295fe1ef --repo chebizarro/nostrc
# Also captured --format json; both invocations exited 0.
gh run download 42363870599 -n signet-deployment-bundle -D "$D/artifact"
(cd "$D/artifact" && shasum -a 256 -c signet-deployment.tar.gz.sha256)
# signet-deployment.tar.gz: OK
```

The JSON verification receipt was written at **10:57:39.956**. Verified subject SHA-256 equals the requested digest. Certificate identity is `https://github.com/chebizarro/nostrc/.github/workflows/signet-release.yml@refs/tags/signet-v0.8.0-rc.1`; source is **`de2a408766773369407bbd5ef831ccc53f0f1727`**; runner environment is `github-hosted`; invocation is [release run 42363870599, attempt 1](https://github.com/chebizarro/nostrc/actions/runs/42363870599/attempts/1), queried as completed/success. Rekor timestamp: **10:31:32 UTC**.

The downloaded release manifest contained this default digest and the separately published passkeys digest. Host `RepoDigests` and OCI labels confirmed the default digest, source SHA and version `0.8.0-rc.1`. No locally built runtime was deployed. Passkeys was not exercised.

## Provisioning and command conventions

[Sanitized execution receipts](signet-rc1-published-image-smoke-2026-11-07-evidence.txt) retain the startup journal, SQLCipher audit rows, metrics, correlated replies, client results, negative receipts and teardown. These are selected actual outputs, not a reconstructed complete terminal transcript.

```sh
# Every shell invoking doctl; never printed or committed the token.
set +x
export DIGITALOCEAN_ACCESS_TOKEN="$(tr -d '[:space:]' < ~/.config/doctl/token)"

# H: ssh -i "$D/ssh" -o UserKnownHostsFile="$D/known_hosts" \
#      signet-admin@104.236.215.230
# C: published-image signetctl in workstation linux/amd64 container:
#      SIGNET_PROVISIONER_NSEC_FILE=/work/provisioner.key signetctl -c /work/operator.conf
# N: unmodified nip46_reference_client, workstation Linux ARM64 container:
#      SIGNET_LIVE_RELAYS=wss://nos.lol,wss://relay.nostr.band \
#      nip46_reference_client /work/client.ini <mode>
```

Created through `signet/deploy/digitalocean/create-droplet.sh --create`, with nyc3, `s-1vcpu-2gb`, Ubuntu 24.04, 20 GiB ext4, operator SSH CIDR `174.21.188.15/32`, and existing VPC `97f31fe2-0649-47a4-9e6d-a93e62c417fb`. Backups and IPv6 were not enabled. A private, invocation-local `doctl` adapter substituted the script's hardcoded `signet` tag / `signet-fw` name with `signet-smoke` / `signet-smoke-fw`, added the volume tag, and immediately recorded creation IDs in the bead. SSH keys use the task name; the firewall targets the task tag. No repository deployment tooling was edited.

Cloud-init finished at **11:05:06**, `errors: []`, `recoverable_errors: {}`. Docker 28.5.1, Compose v2.40.3. Volume mounted at `/srv/signet`, ext4; UUID `c9a638bb-ab25-4872-b24d-641f9262b055`. Generated independent random DB/backup keys and throwaway bunker, provisioner and client keys. Provisioner/client private keys remained on the workstation. Host secrets were installed 10001:10001, 0400 under root-owned 0700 `/opt/signet/secrets`; config was 10001:10001, 0640 in a 0750 directory. Default policy began deny-all.

Installed the checksum-verified CI bundle under `/opt/signet/releases/de2a408766773369407bbd5ef831ccc53f0f1727`, bound it with `bind-host.py --volume-name signet-smoke-data --config-revision nostrc-44q4r`, and selected it as `current`. The fresh-install registry route from the runbook was used:

```sh
sudo docker --config /opt/signet/secrets/docker compose --project-name signet \
  --env-file /opt/signet/config/deploy.env --env-file /opt/signet/current/release.env \
  -f /opt/signet/current/signet/docker-compose.yml pull signetd
sudo /opt/signet/current/signet/deploy/preflight.sh
sudo systemctl start signet-compose.service
```

Pull and preflight passed at **11:05:21**. This was first installation, not a `release.sh upgrade`/rollback exercise; no `--offline`, image archive import, registry login or digest bypass was used.

Direct workstation nos.lol HTTPS timed out (curl exit 28). The previously proven workaround was:

```sh
H -M -S "$D/tunnel.sock" -f -N -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:24443:nos.lol:443 -L 127.0.0.1:25443:relay.nostr.band:443
# Task-only Docker network aliases nos.lol and relay.nostr.band point to TCP
# socat forwarders to host.docker.internal:24443 and :25443 respectively.
```

Tunneled nos.lol returned HTTPS 200 at **11:03:09**; band timed out. No TLS termination/substitution or alternate relay was introduced. Reference-client source and relevant libnostr/libgo/NIP-46 sources in this worktree had zero diff against the release tag. The reference client was built with CMake in the existing local Linux ARM64 CI image; it is **client-side local-container evidence**, not a locally substituted daemon or native macOS proof.

## First startup: the decisive admission check

Daemon startup: **11:05:53.077**. Band's unavailable NIP-11 document delayed management initialization; subscriptions were installed at **11:08:08.866**, then the main loop started. The journal records:

```text
11:08:08 [simplepool] subscribe fire failed on wss://relay.nostr.band ... connection is NULL
11:08:08 [signetd] subscribed to canonical management kinds on relay pool
11:08:08 [simplepool] subscribe fire failed on wss://relay.nostr.band ... connection is NULL
11:08:08 [RELAY_POOL] Subscription 1 received 0 events before EOSE
11:08:08 [RELAY_POOL] Subscription 3 received 0 events before EOSE
```

These are daemon subscription receipts, not a packet capture of raw REQ frames. They show the duplicate startup subscriptions on the reachable nos.lol path; band has failed subscription attempts, not a demonstrated EOSE.

`/ready` became true before **11:08:11**. `ss -lntp` showed signer PID 10645 at `127.0.0.1:8080`, not a wildcard. DNS loopback and public SSH were the other listeners.

```text
C status                         11:08:11–11:08:12, exit 0
{"jsonrpc":"2.0","result":{"db_open":true,"agents":0,"relays_connected":true},"id":"00065fadbe8fafb4"}
C provision smoke-agent          11:08:24–11:08:25, exit 0
agent_id: smoke-agent
pubkey: 35c593072ccb284bc8954427404d5429161f8989871ab0685178e7f18f86ef47
```

The unmodified CLI's verified/correlated reply path returned success; provision output intentionally withholds the bunker URI. SQLCipher query with `PRAGMA query_only=ON` (read back as `1`) returned:

```text
id|ts|UTC|operation|agent_id|detail
1|1794049705|2026-11-07 11:08:25|mgmt_provision_agent|smoke-agent|{"decision":"allow","reason":"provisioned"}
agent_count|1
```

There were **no history-drop rows on this fresh first start**. This successful mutation followed initial daemon startup by approximately 152 seconds and management EOSE by 17 seconds, without SIGHUP or relay edits.

Metrics immediately before management and after provision: uptime **138 → 155**, active agents **0 → 1**, writeback failures **0 → 0**, sign/auth/revoke counters still zero. `signet_relays_connected` reported **2** even while band failed; that metric is not treated as proof of two usable relay subscriptions. Metrics do not replace the audit admission evidence.

## Pair, sign and policy denial

Executed, in order:

```sh
C reissue-connect smoke-agent --out /work/agent-secret
C set-policy smoke-agent '<explicit allow_clients; connect/get_public_key/sign_event/nip44_encrypt; kind 1; deny nip44_decrypt>'
N pair
N sign
N deny
```

`reissue-connect` exit 0 at approximately **11:08:51**; `set-policy` exit 0 at **11:08:53**, reply ID `00065fadc0fd9528`. The policy allowed only client `23457e444ded9f3edf381b66f133a7d2e4fd131c27a64c40cde405e3a3ec64f6`. The protected reference-client INI contained the proper bunker transport public key, returned connect secret, persistent client transport key, expected agent public key and allowed kind 1. No URI or secret is included here.

| Command | UTC interval / exit | Verified result |
|---|---|---|
| `N pair` | 11:09:03–11:09:10 / 0 | connect ACK; encryption allowed; signature `fc68dbba0c2d078ff02029b6558e1c811259b9cef7f245375512c0a779a8241d` |
| `N sign` | 11:09:29–11:09:34 / 0 | fresh process, no connect RPC; signature `2724aad24e240d5b5718b69e8c266adf594fc5eb1c6e932bc935e3a9003618e1` |
| `N deny` | 11:09:35–11:09:40 / 0 | encryption allowed, then authenticated `policy.deny.method` for `nip44_decrypt` |

These modes were executed individually, not as the aggregate `test_production_roundtrip.sh` command. Band publication timeouts were not accepted as policy denials; authenticated replies came from the bunker through nos.lol.

## Second clean start and retained pairing

`H sudo systemctl restart signet-compose.service` requested at **11:09:54**. The container stopped at **11:09:55**. The immediate start **correctly failed** at **11:09:56**:

```text
RESTART_COMMAND_EXIT=1
signet deployment refused: restart refused: 270s remain in stop quarantine
```

Systemd's configured delayed retry ran at **11:14:31**; preflight/allowed start at **11:14:32**; daemon startup at **11:14:33.628**. No receipt, quarantine or supervisor bypass was used. Management subscriptions began at **11:16:49.058**. Subscriptions **1 and 3 each reported four events before EOSE** at **11:16:49**; band again failed subscription installation. Ready was observed at **11:16:52**.

```text
N sign                         11:16:52–11:16:58, exit 0
signed-event-id=28cbf85c13f45c6a5eb6899762d74cc5261d2355321c448cce2d40e9c259e324
C status                       11:16:58–11:16:59, exit 0
{"jsonrpc":"2.0","result":{"db_open":true,"agents":1,"relays_connected":true},"id":"00065fadddf6fac9"}
```

No pairing/connect RPC was issued by `N sign`. The protected client configuration SHA-256 was unchanged before/after restart: `d5c79b34f847a7f87d9ff41122df7ee07388a5f2246ae1b37333aa5feec2fd9a`. Daemon INI SHA-256 remained `9cef80185bde92b36712d025da9200319ad72b1a8fbfc6def55382508478d230` from initial manifest through final inspection; final relay line was `wss://nos.lol;wss://relay.nostr.band;`. There was no SIGHUP.

**Do not erase the historical drops:** audit IDs **4–7**, all timestamp **1794050209 / 11:16:49**, contain `mgmt_history_dropped`, deny / `before_live_boundary`, with outer ephemeral sender identities. They coincide with the four pre-EOSE backfill events, before the fresh status began at 11:16:58. That timing and successful fresh reply distinguish them from the original live-request regression; there is no claim of decrypted per-event attribution. No further history-drop row appeared after that catch-up window.

## Revocation and final receipts

```text
C revoke-client <client-public-key>   completed 11:17:16, exit 0
{"jsonrpc":"2.0","result":{"ok":true,"request_id":"00065faddeea2ca0","code":"client_revoked","message":"client binding revoked"},"id":"00065faddeea2ca0"}
N revoked                            completed 11:17:19, exit 0
revocation: authenticated refusal of previously allowed encryption
```

The authenticated error was `client has no active NIP-46 session`, not a timeout. Final audit row:

```text
8|1794050235|2026-11-07 11:17:15|mgmt_revoke_client|8468729aedfb515e6cdb9743a6e9abb8e013534494010907300450e466fbc856|{"decision":"allow","reason":"client_revoked"}
```

`signet-ops verify-audit` returned **integrity OK, 8 entries, exit 0**. Final readiness was true. The second process's metrics reported sign **1**, auth OK **2**, denied **1**, reply ACK accept **3**, active agents **1**, writeback failures **0**. Counters reset on restart; these are not cumulative first-plus-second-process totals. `signet_revoke_total` remained zero; audit/reply/client refusal, not that counter, establish client revocation.

## Negative receipts and limits

- A preparatory local shell wrapper had a quoting error before any daemon start; corrected invocation used a here-document. Early direct/native/ad-hoc reference-client compilation attempts failed on headers/link dependencies; the subsequent CMake reference-client build succeeded. No failed attempt is counted as a passing live check.
- Two unprivileged host metadata reads returned permission denied; later `sudo` reads obtained volume IDs/config hashes. No permissions were weakened.
- Initial SQLCipher CLI `-readonly` inspection failed because this packaged CLI lacks that flag. The corrected inspection set and read back `PRAGMA query_only=ON`, then executed SELECTs only; no migration or DML.
- An extra final preflight invocation while the daemon was **already running** returned exit 1: `previous daemon has no stop receipt on this boot; investigate before restart`. It was not an activation attempt and was not bypassed. Initial preflight and systemd's post-quarantine preflight passed; this separate negative receipt is retained.
- Initial post-delete volume listing was stale, so the first teardown assertion failed. One delayed **read-only** verification, not another deletion or retry-to-green test, confirmed all four lists empty.
- No raw REQ packet capture, two-healthy-relay/band delivery proof, AUTH rejection, native macOS client test, passkeys, reboot, backup/restore/upgrade, long-duration soak or production custody approval is claimed. These were not added as acceptance gates for this short smoke.

## Resource inventory, destruction and cost

IDs were recorded on creation in `nostrc-44q4r`:

| Resource | ID / name | Created / receipt UTC | Delete accepted UTC |
|---|---|---|---|
| Droplet | `616854303`, `signet-smoke` | 11:01:31 API creation | 11:17:55.535 |
| 20 GiB volume | `9411d446-da9a-11f1-8944-c234aeb68f7e`, `signet-smoke-data` | 11:01:26 API creation | 11:18:08.950 |
| Firewall | `da9b13e0-5c85-4114-a36a-09289be0a444`, `signet-smoke-fw` | 11:01:25.041 receipt | 11:18:09.329 |
| SSH key | `60691110`, `signet-smoke` | 11:01:22.678 receipt | 11:18:09.773 |

Each `doctl compute <type> delete <exact-id> --force` returned exit 0. Tag deletion returned exit 0 at **11:18:10.127**. No existing VPC or unrelated resource was deleted. Final proof, filtering full JSON inventories by exact IDs, task name prefix or task tag:

```text
11:18:48.177 doctl compute droplet list --output json | task-resource-filter []
11:18:48.621 doctl compute volume list --output json | task-resource-filter []
11:18:48.951 doctl compute firewall list --output json | task-resource-filter []
11:18:49.300 doctl compute ssh-key list --output json | task-resource-filter []
11:18:49.300 ALL_TASK_CLOUD_RESOURCES_ABSENT
DESTRUCTION_VERIFIED_EXIT=0
```

The exact filter is `id in task_ids OR name.startswith("signet-smoke") OR "signet-smoke" in tags`; unrelated account inventory was omitted, not removed. Task-only local containers/network/tunnel and the downloaded runtime reference were removed at **11:21:21**. Temporary build/bundle inputs, SSH private key and all throwaway application keys were removed by **11:21:53**, after a known-secret scan and sanitized receipt extraction; absence of the private directory and pointer was asserted. The user's DO token/default context, pre-existing CI images, shared Docker cache, VPC and other worktrees were preserved.

The live DO size API returned **$0.01786/hour**. Droplet duration to deletion acceptance: **0.273482 hours × $0.01786 = $0.004884**. Conservatively counting through absence confirmation gives less than **$0.0052**. A 20 GiB volume at DigitalOcean's [$0.10/GiB-month rate, accruing hourly](https://docs.digitalocean.com/products/volumes/details/pricing/), verified for this report, adds approximately **$0.0008** on a fractional-duration estimate using 720 hours/month. Duration-based total is approximately **$0.006**; invoice granularity/rounding is not established by this test. No Spaces, backups or snapshots were purchased.

## Delivery

Documentation/evidence only: **no independently releasable component version bump**; no shipped artifact changed. Repository hooks were installed. Staged whitespace checking and ten focused receipt/link/secret-pattern assertions passed. One Oracle review was attempted, but it received stale context rather than these files and explicitly reported an incomplete review; no independent review approval is claimed. RepoPrompt's diff-artifact export also refused passive inspection of uninitialized submodules; native Git status confirmed only the two owned additions. `bd dolt pull` reported `no remote`; no synchronization route was invented. Work remains local on `signet/rc1-published-image-smoke`, based on `d6ea3b79e`. The published-image live-verification gap for `nostrc-5xqun` is closed in its appended notes with the unavailable-band limit retained. This is not broader release qualification.
