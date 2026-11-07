# Signet production security review (W2.B–D) — 2026-11-06

**Beads:** `nostrc-mk7v1.3` (W2), epic `nostrc-mk7v1`; findings `nostrc-z3b0s`, `nostrc-17ggo`, `nostrc-hbj7a`, `nostrc-d1g76`, `nostrc-nmefa`, `nostrc-k2d0c`, `nostrc-2h11s`, `nostrc-p9nyr`, `nostrc-6dx4o`, `nostrc-00zw1`, `nostrc-505og`, `nostrc-pr0jv`, `nostrc-65goc`, `nostrc-h8svi`, `nostrc-jw15k`, `nostrc-5efwz`, `nostrc-0nv36`, `nostrc-mnr6a`, `nostrc-zx913`, `nostrc-95xkd`, `nostrc-qe8yy`; linked existing `nostrc-a7p6i`, `nostrc-58bop`, `nostrc-nfnw`, `nostrc-1r5d.3`, `nostrc-1cx`, `nostrc-57dv3`.

**Branch:** `signet/rc1-security-review` (local, not pushed)

**Audited revision:** `master` at `62ceeb50f` (W1 runtime safety, W2.A relay, W3/W4 packaging, W5 docs merged)

**Authority:** [`signet/docs/THREAT_MODEL.md`](../../signet/docs/THREAT_MODEL.md) governs; matrix and severity rules from [plan §8.B–D](../plans/signet-production-deployment-2026-11-06.md#8-w2--relay-correctness-and-security-audit).

## Boundary and verdict

**Scope:** every row of the plan §8.C matrix against merged code — build/supply chain, listener exposure, bootstrap routes and TLS trust, NIP-46 and its replay cache, management (direct vs gift-wrap, replay, D2 write-back), relay pool and libnostr client TLS, storage/keys, backup/migration, logs/CLI/process, passkeys/CTAP-HID, provisioner/reload. **Out of scope:** the Go MCP wrapper (dropped), D-Bus TCP/unix, NIP-5L, ssh-agent (all refused by preflight, `signet/deploy/ops.py:231`), `signet-git-credential` (not built into the image, `signet/Dockerfile:27`).

**Evidence boundary:** source inspection of the merged tree, plus host-local execution on macOS arm64 (Apple Clang, Homebrew OpenSSL 3.6.5) of a Release build with SQLCipher ON, passkeys ON, test hooks OFF (`build-review/`). **No container image was built, no Droplet was used, and no live public relay was contacted.** Wherever a finding or blocker needs deployed-image or Droplet evidence, that is stated and left open. Line numbers are at `62ceeb50f`.

**Verdict: no Critical or High finding.** I found 6 Medium and 16 Low findings. One of the Lows (SR-22, threat-model drift) is fixed in this branch. Every finding that would otherwise rate High is either on the bootstrap surface, which ships disabled and is capped at Medium (SR-04, SR-05), or needs an authorized operator to step outside the supported route (SR-01, SR-03). None of the six Medium findings blocks the baseline profile under §8.D, but all six need owner acceptance of the recorded residual. The audit therefore does not add a code launch blocker. The rc.1 tag remains gated on the open operational and independent-review blockers in §Blocker disposition.

## Findings (ranked)

Severity is per plan §8.D. "Profile" is one of: baseline = signetd + ops on the Droplet; bootstrap = optional Caddy profile, disabled at launch; passkeys / ctap = optional image or profile. Verification legend: **R** = reproduced or exercised at runtime on the host; **S** = static source evidence only. Independent-review state is *pending W6 (`nostrc-mk7v1.7` / `nostrc-1r5d.3`)* for every row.

### SR-01 — Medium (C) — direct kind-25910 ingress executes secret-bearing mutations in plaintext · `nostrc-z3b0s`

- **Where:** `signet/src/signetd_main.c:562-571` (`signet_on_relay_event`) → `signet_mgmt_handler_handle_intent` (`signet/src/mgmt_protocol.c:~1212-1302`). This is the same dispatcher the gift-wrap path calls after unwrapping (`:1304-1323`).
- **Prerequisites / profile:** an authorized provisioner key holder sends a management request as a plain kind-25910 event instead of a NIP-17 gift wrap. Baseline profile.
- **Behavior:** `agent/adopt-existing` / `agent/restore-existing` (`agent_nsec`) and `credential/create|import|rotate` (`payload_b64`) are accepted and executed even though every relay could read the request content. The NIP-44 encryption at `mgmt_protocol.c:1284-1291` is applied after parsing, so it protects nothing on the wire. Replies go out gift-wrapped (`:892-900`) with no plaintext fallback.
- **Mitigation present:** the shipped `signetctl` always gift-wraps (`signet/src/signetctl_main.c:311,1525`). `THREAT_MODEL.md` §3.4 already says not to send secrets over direct ingress.
- **Reproducer:** static. A direct 25910 `agent/adopt-existing` from a provisioner key would reach the same handler as the wrapped path.
- **Remediation:** refuse secret-bearing methods, or all mutations, on direct ingress with a distinct error, and default that refusal on in encrypted-production mode. The server cannot un-leak a secret, but refusing turns silent success into a visible failure.
- **Launch:** not a blocker. Needs owner acceptance.

### SR-02 — Medium (I) — restart replay boundary relies on supported start paths; durable marker deferred · `nostrc-17ggo` (related `nostrc-a7p6i`)

- **Where:** RAM-only caches (`signet/src/replay_cache.c`; management `mgmt_protocol.c:640-660`; NIP-46 defaults `signet/src/signet_config.c:248-250`: 50,000 entries, 600 s TTL, 120 s skew). The 49 h resubscribe lookback is at `signetd_main.c:50-58`. The quarantine is in `signet/deploy/ops.py:155-185` (`wait_rule`), `signet/deploy/systemd/signet-compose.service:12-17` (`ExecStartPre=preflight`, `RestartSec=275s`), and the skew bound `[replay] skew_seconds ≤ 120` in `ops.py:218`.
- **Behavior:** supported starts are covered: systemd start/restart, `release.sh`/restore, and unclean reboot (uptime ≥ 270 s). A manual `docker compose up` / `docker start` bypasses the rule; the README covers `docker start` (`signet/deploy/README.md:313-315`) but not manual Compose. Across a clean reboot only wall time is compared (`ops.py:171-177`), so a forward clock jump shortens the real interval. Receipts are integrity-unprotected files on the volume.
- **Arithmetic:** a request accepted at most +120 s future-skewed before a stop is 150 s old at a stop+270 s restart, which is outside the 120 s window. The rule is sound as long as skew stays ≤ 120 s, which preflight enforces.
- **Audit decision (blocker 15):** a durable replay marker is **not required for rc.1**, because every supported start path enforces the quarantine. It is a post-rc hardening item. Residual: a request executed within ~120 s before an unsupported fast restart can execute again. RAM caches never give durable exactly-once.
- **`nostrc-a7p6i` interplay:** the fixed 10 s post-subscribe drop (`signetd_main.c:56-58,563,576`) is also an accidental restart-replay damper, because it drops backfill delivered in the first 10 s. Replacing it with the event-driven catch-up boundary must not widen replay; land it together with, or after, the durable marker.
- **Launch:** not a blocker (blocker 15 cleared with this disposition). Needs owner acceptance of the residual.

### SR-03 — Medium (I) — gift-wrap dedup is keyed by the outer wrap id; a re-wrapped rumor re-executes · `nostrc-hbj7a`

- **Where:** `signetd_main.c:580-582` passes the kind-1059 id. The cache is marked with it in `mgmt_protocol.c:~1862-1873`. The rumor time *is* used for the window (`:1319-1325`).
- **Prerequisites:** only the original authorized sender can re-wrap, because the seal is NIP-44-encrypted to the bunker. A relay cannot do it. Example: a client retry tool that re-wraps instead of resending the same wrap.
- **Behavior:** a new wrap of the same authenticated rumor inside the 120 s skew window executes the same mutation again. This is the §8.C "gift-wrap-ID dedup may not prevent rewrapping" case. It is listed in THREAT_MODEL §3.4, but no control prevents it.
- **Remediation:** additionally dedup on (seal author, rumor id) in the same refuse-when-full scoped cache.
- **Launch:** not a blocker. `signetctl` never auto re-signs or re-wraps a mutation (plan §8.A).

### SR-04 — Medium, capped (A) — bootstrap `/challenge` store is unbounded and never pruned · `nostrc-d1g76`

- **Where:** `signet/src/bootstrap_server.c:316-335` → `signet/src/nostr_auth.c:59-86`, which calls `g_hash_table_replace` with no cap. `signet_challenge_store_cleanup` (`nostr_auth.c:236`) has no production caller; the only reference is the store constructor at `signetd_main.c:958`.
- **Profile:** bootstrap only. **Not exploitable while disabled:** with `port = 0` no listener starts (`signetd_main.c:1007`), and preflight requires `port = 0` unless the profile is enabled (`ops.py:230`). Uncapped it would be High once enabled, because Caddy makes it remote and unauthenticated.
- **Reproducer (R):** `/tmp/sr-scripts/repro_challenge2.sh` ran the real `build-review/signet/signetd` with `SIGNET_REQUIRE_ENCRYPTED_DB=true`, a file-backed key, `[bootstrap] port = 19487`, and the default `require_tls`. 4,000 `GET /challenge?agent_id=<6000 bytes><i>` requests with `X-Forwarded-Proto: https` from loopback all returned **200**. RSS went from **23,696 KiB to 49,424 KiB** and was still **49,632 KiB** after 65 s idle, more than 2× the 30 s TTL.
- **Remediation:** global and per-agent caps, a bounded `agent_id`, challenges issued only for provisioned agents, and periodic cleanup on the main loop.
- **Disposition:** the profile stays disabled. Plan §7.D already conditions enabling it on "challenge/resource bounds".

### SR-05 — Medium, capped (C/I) — `/bootstrap` hands out the live connect secret on bearer token + pubkey equality · `nostrc-nmefa`

- **Where:** `bootstrap_server.c:156-309`. `bootstrap_pubkey` is compared for equality by `signet_store_verify_bootstrap_token`; no signature proves possession. The response embeds the agent's standing `connect_secret` (`:261-271`).
- **Profile:** bootstrap only (disabled). Token insertion has no production caller. This is the §8.C "bootstrap pubkey equality is not possession" case.
- **Positives:** token consumption is atomic and single-use (`:287-298`); the body cap and the trusted-peer check run before accumulation (`:443-465`).
- **Remediation:** a proof-of-possession signature over a server challenge, a fresh single-use connect secret per handoff, and documented bearer semantics.
- **Disposition:** the profile stays disabled.

### SR-06 — Medium (I) — virtual CTAP-HID has no per-caller authorization · `nostrc-k2d0c`

- **Where:** `signetd_main.c:832-839` binds the UHID device to `cfg.identity`. `signet/src/fido_ctaphid.c:307,353` call `signet_fido_make_credential` / `get_assertion` with `dev->agent_id` and no caller identity or policy check. UP is always asserted (`signet/src/fido.c:327-330,489-491`).
- **Profile:** ctap only. It is experimental and is not a Droplet launch surface (plan §4).
- **Behavior:** any local process that can open the kernel hidraw node mints and uses credentials as that agent.
- **Remediation:** document the hidraw ACL requirement, require an explicit owner group, and gate operations on policy. Keep the profile unsupported until then.
- **Disposition:** off at launch. Preflight ties `virtual_ctap` to the profile (`ops.py:233-239`).

### SR-07 — Low (I/A) — NIP-46 replay cache evicts at capacity and marks before authentication · `nostrc-2h11s`

- **Where:** `signet/src/nip46_server.c:491-495` marks before decrypt or binding. `replay_cache.c:233-241` evicts the oldest entry; `refuse_when_full` is only set for management (`mgmt_protocol.c:655`).
- **Behavior (S):** flooding 50,000 validly signed events from fresh keys evicts a legitimate id, which a relay can then redeliver within 120 s.
- **Impact:** replies stay NIP-44 encrypted to the original client. Duplicate execution only costs audit rows, rate budget and passkey side effects. Preflight forbids `max_entries = 0` (`ops.py:220`), which would otherwise make the NIP-46 cache NULL and skip replay checks entirely (`replay_cache.c:89-92`, `nip46_server.c:493`).
- **Remediation:** per-sender scope plus refuse-when-full, or mark after binding resolution.

### SR-08 — Low (C/I metadata) — plaintext `ws://` relays accepted in encrypted-production mode · `nostrc-p9nyr`

`libnostr/src/connection.c:1074-1080` accepts `ws://`. Neither `signet_config_validate` (`signet_config.c:840-845`) nor preflight requires `wss://`. Event signatures and NIP-44 still hold, but AUTH events and metadata travel in clear. The firewall's optional outbound port 80 means a `ws://host:80` relay would work. Reject non-`wss` relay URLs when encryption is required.

### SR-09 — Low (I) — NIP-46 connect consumes the secret and binds before the deny-list refusal · `nostrc-6dx4o`

`nip46_server.c:566-591` consumes and binds in one transaction, then refuses deny-listed agents at `:658-672`. The spent pairing persists and becomes live if the deny entry is lifted. Check the deny list before consume-and-bind.

### SR-10 — Low (C) — passkey PSK file read without protected-file checks · `nostrc-00zw1`

`signet_config.c:198-224` uses `g_file_get_contents`: it follows symlinks, checks neither owner nor mode, and reads without a bound. It also accepts the value-bearing INI `sync_key` and `SIGNET_PASSKEYS_SYNC_KEY`. Preflight refuses a value in the INI (`ops.py:216`). Use `signet_secret_input_file()` (`signet/src/secret_input.c:15-52`), as the bunker key already does.

### SR-11 — Low — passkeys/CTAP configured-but-unavailable only warn · `nostrc-505og`

`signetd_main.c:823-825,840-842,847-852` only warn when FIDO service init fails, CTAP start fails, or passkeys are enabled on the default build. Plan §10.H says "never pretend active". Preflight blocks the image/profile mismatch at deploy time (`ops.py:233-239`); the daemon itself does not. Make these fatal when configured.

### SR-12 — Low (I) — daemon-writable `/opt/signet/config` holds root-consumed `deploy.env` and `Caddyfile` · `nostrc-pr0jv`

`/opt/signet/config` is mounted rw for D2 (`signet/docker-compose.yml:31-35`), and preflight *requires* `deploy.env` to be owned by 10001 (`ops.py:203`). Root `ops.py` passes it to Compose (`:116-118`), and root Caddy loads `Caddyfile` from the same directory (`signet/deploy/compose.bootstrap.yml:18-21`). `env_file()` allowlists keys and rejects `$`, quotes and whitespace (`ops.py:74-84`), which neutralizes `deploy.env` injection. The Caddyfile is unvalidated. Exploiting either requires daemon code execution first. Mount only the INI (or a dedicated rw subdirectory) into signetd.

### SR-13 — Low (A) — MHD servers have no connection timeout or limits · `nostrc-65goc`

`health_server.c:372-379` and `bootstrap_server.c:566-574` set no `MHD_OPTION_CONNECTION_TIMEOUT`, `CONNECTION_LIMIT` or `PER_IP_CONNECTION_LIMIT`. Under host networking any local process can hold idle connections and starve `/health` / `/ready`. That is an operator-visibility failure, not an outage: Compose `restart: "no"`.

### SR-14 — Low, capped (I) — bootstrap `/auth` issues a lease for an unknown `agent_id`; responses not JSON-escaped · `nostrc-h8svi`

`nostr_auth.c:209-219` skips the pubkey↔agent check when the agent is unknown, so any provisioned agent key can obtain a lease labelled with an arbitrary id (`bootstrap_server.c:397-416`). Responses are assembled with `%s` (`:102,300-302,407-411`). Bootstrap profile only.

### SR-15 — Low (C) — legacy migration leaves `.plaintext-backup` without forced `0600` and never removes it · `nostrc-jw15k`

`signet/src/store.c:248-252`: `link()` inherits the DB mode, and `signet_copy_file` uses `fopen("wb")` (`:139-143`). Migration also runs under `SIGNET_REQUIRE_ENCRYPTED_DB=true` (`store.c:612-652`). The launch uses a fresh DB inside a `0700` `/data`, so this only matters for upgrades of legacy plaintext stores.

### SR-16 — Low (I) — `signetctl list-agents --verify` reports ok when the stored pubkey is NULL · `nostrc-5efwz`

`signetctl_main.c:1321-1341` compares only when `meta.pubkey` is non-NULL, and `store.c:2694` maps an empty pubkey to NULL. Detected mismatches and decrypt failures do return nonzero; the probe traced the non-zero exit to `signetctl_main.c:1364,1435`, which I did not open myself.

### SR-17 — Low (C/I) — audit log opened without `O_NOFOLLOW`; existing file mode not tightened · `nostrc-0nv36`

`signet/src/audit_logger.c:78-79`. JSONL serialization itself is safe (JsonBuilder/JsonGenerator; the probe cited `audit_logger.c:320-371`, which I did not read myself). This is defence in depth under `0700` `/data`.

### SR-18 — Low (C) — secret-bearing JSON parse trees and NIP-46 params freed without wipe · `nostrc-mnr6a`

Locations: `mgmt_protocol.c:171,210-227,1061,1126-1128` (request parse), `:425-432,848-853` (reply parse), and the `bunker_uri` copies at `:1962-1986,2075-2121`. On the NIP-46 side, `nips/nip46/src/core/nip46_msg.c:102-125` frees duplicated params with `free()`. The primary buffers are wiped. Same-UID memory access is out of scope, so this narrows freed-heap residue only.

### SR-19 — Low (I) — `signet_relay_pool_handle_event_json` fails open on allocation failure · `nostrc-zx913`

`signet/src/relay_pool.c:1309-1318` dispatches without verification if `nostr_event_new()` returns NULL. `:1334-1339` leaves `ev.event_json` uninitialized, and the gift-wrap branch reads it (`signetd_main.c:575`). There is no production caller (header and `signet/tests/test_relay_pool_protocol.c` only). Live subscriptions verify in middleware first (`relay_pool.c:122-128`).

### SR-20 — Low (I) — release supply-chain pin gaps · `nostrc-95xkd`

- apt packages are unpinned (`signet/Dockerfile:10-18,57-61`). Package inputs are recorded with `dpkg-query` (`:30,66`), which plan §9.A accepts.
- Workflow actions are referenced by tag, not SHA (`.github/workflows/signet-release.yml`).
- `check-release.py` does not validate the Docker/Compose/containerd pins.
- `.dockerignore:42` re-includes `**/fixtures/**`, which pulls test private keys (e.g. `apps/grotto/tests/fixtures/profile-images-test.pem`) into the builder context. They do not reach the runtime stage: `signet/Dockerfile:45-80` copies only selected binaries and libs.

### SR-21 — Low (A/I) — off-host backup recorded without read-back verification · `nostrc-qe8yy`

`ops.py:255-285` records `off_host: true` once `aws s3 cp` exits 0, but `signet/deploy/README.md:253-255` requires a verified upload.

### SR-22 — Low (doc) — threat model still stated a ≥150 s stop rule · fixed in this branch

THREAT_MODEL §3.4 and §5 said ≥150 s; the plan (`62ceeb50f`) and deployment (`ops.py`, `RestartSec=275s`, `README.md:4`) use 270 s. Corrected in the separate THREAT_MODEL commit, together with the residuals from SR-01/02/03/04/06/07. No bead: the fix is in this branch.

## §8.C matrix coverage

| Focus | What was checked | Result / evidence |
|---|---|---|
| Build/supply chain | SQLCipher ON, test hooks OFF, explicit passkeys arg per variant (`Dockerfile:9,23-24`); no `setcap`; `USER 10001:10001`; separate runtime stage; digest-pinned `versions.env:3-4`; `ldd` gate (`Dockerfile:76-79`); `.dockerignore` coverage | Cleared, except SR-20. **Image not built here**, so `ldd` output, image config and in-image flags are not observed (§Runtime capability manifest). |
| Listener exposure | `http_listen.c` numeric parser (no DNS, no wildcard fallback, bracketed v6); `MHD_OPTION_SOCK_ADDR` in both servers; daemon binds `127.0.0.1` (`signetd_main.c:1009,1142`); enabled-listener failure is fatal | **R**: lsof showed only `127.0.0.1:18080` and `127.0.0.1:19487`; `192.168.40.126:18080/19487` connect refused (curl exit 7). SR-13 open. Droplet `ss -lntp` and off-host test not run. |
| Bootstrap | trusted-peer + single `https` header before body (`bootstrap_server.c:87-97,443`); duplicate header rejected; 8 KiB cap; atomic token consume | **R**: no header → 403, duplicate → 403, forged single header from loopback → 200 (accepted on-box limitation, THREAT_MODEL A11). SR-04 (R), SR-05, SR-14 open; profile disabled. |
| NIP-46 | outer signature in middleware before dispatch (`relay_pool.c:117-128`); 64 KiB plaintext cap; binding pinning; deny before policy; writer fence in store transaction; reply NIP-44 to requester; same-signed-event failover, never re-sign | Cleared, except SR-07, SR-09. CTest `client_bindings`, `relay_pool_wire`, `relay_pool_protocol` passed. |
| Management | direct vs wrapped; seal author == rumor pubkey (`nips/nip17/src/nip17.c:503-520`); provisioner/self-service/delivery-grant authz; refuse-when-full scoped caches; no plaintext reply fallback; audit redaction | SR-01, SR-02, SR-03 open. CTest `mgmt_replay` passed. |
| Relay pool | generation-scoped OK dispatcher; owned GLib sources; readiness = current required subscription past EOSE, CLOSED/disconnect reset (`relay_pool.c:294-300,673-686,1721-1737`); TLS | Cleared, except SR-19. CTest `relay_pool_ok_lifetime`, `relay_pool_wire`, `relay_pool_protocol` passed. TLS: see blocker 5. |
| Storage/keys | encrypted-required refusal before services (`signetd_main.c:698-740`); fail-closed key-store construction; prepare-before-commit mutations; `%q`/`%Q` SQL; secure allocation | **R**: `daemon_startup` 10/10 cases passed (incl. empty/unavailable/ambiguous/wrong key, unwritable state, listener unwind). `key_store` passed. SR-15 open. |
| Backup/migration | `O_EXCL|O_NOFOLLOW` 0600 targets, verification, `.pre-restore`, exclusive-lock restore, online backup under a concurrent writer | **R**: `test_online_backup_consistency: PASS (concurrent writes observed)`; wrong keys, tamper, refusals all PASS. SR-15, SR-21 open. Fresh-host restore drill not run. |
| Logs/CLI/process | stdin-only BYO secrets (`signetctl_main.c:936-939`); bunker URI redaction; `--out` 0600 `O_EXCL`; audit JSON serialization; Compose `cap_drop: ALL`, `no-new-privileges`, `read_only`, `core: 0`, memlock unlimited; systemd `LimitCORE=0`, `UMask=0077` | Cleared, except SR-16, SR-17, SR-18. `/proc/<pid>/status` (Cap*, NoNewPrivs, VmLck) and limits **not observed** (no Linux container). `mlockall` stays warn-only (§must-not-paper-over). |
| Passkeys | build/runtime gating; NIP-46/D-Bus policy and ownership (`fido.c:418-431,864-875`); UP always; UV default-deny; PSK secretbox export/import | SR-06, SR-10, SR-11 open. Counter is always 0 and RP ID is caller-selected (by design for an exportable software authenticator; noted, no bead). Programmatic python-fido2 interop and Linux HID not run here. |
| Provisioner/reload | D2 desired state, mtime tombstone rule, write-back via temp + fsync + rename (`signet_config.c:688-808`), adoption marks only after write-back (`config_reload.c:296-329`), env override warns (`signet_config.c:657-662`) | **R**: `config_reload` passed incl. "signed D2 grant/revoke/reload/restart/empty" and "adoption write failure and retry". |

**Protocol-smells:** not rerun. W2.A recorded a scoped `nostr-protocol-smells` A–J pass (comment on `nostrc-mk7v1.3`, 2026-11-07). The only residual it found is the fixed 10 s accept delay (`nostrc-a7p6i`), dispositioned under SR-02.

## Must not be papered over

- **RAM-only replay caches do not give durable exactly-once.** True; SR-02 records the residual, and the 270 s rule bounds only supported restarts.
- **Gift-wrap-id dedup does not prevent re-wrapping.** True; SR-03.
- **A new event id can repeat a logical mutation, and JSON-RPC ids only correlate replies.** True; THREAT_MODEL §3.4.
- **A lost reply does not prove a failed mutation.** True; THREAT_MODEL §3.4 lost-replies rule. NIP-46 replies fail over by republishing the same signed event (`relay_pool.c:1212-1247`), and ACK outcomes are exported in `/metrics` (`signet_nip46_reply_ack_total`).
- **Bootstrap pubkey equality is not possession.** True; SR-05.
- **`sodium_malloc` + warn-only `mlockall` do not justify "never swapped".** `mlockall` failure only warns (`signetd_main.c:635-645`), and `CAP_IPC_LOCK` is dropped in Compose; whether unlimited memlock alone locks pages is unverified until `/proc/<pid>/status` VmLck is read on the Droplet.
- **An audit hash chain without an independently retained checkpoint does not resist whole-history replacement.** True; THREAT_MODEL §3.3. `backup.sh` uploads `audit.jsonl` and the encrypted DB, but no separate chain-head checkpoint.

## Exposure matrix (launch layout, host networking)

| Listener | Bind | Default | Off-host reachable | Evidence |
|---|---|---|---|---|
| health `/health` `/ready` `/metrics` | `127.0.0.1:<health_port>` | on | no | R (host lsof + LAN refusal); Droplet `ss` pending |
| bootstrap `/bootstrap` `/challenge` `/auth` | `127.0.0.1:9487` | off (port 0, preflight-enforced) | only via Caddy 443 when the profile is enabled | R (host) |
| Caddy 443 | host | off (profile) | yes when enabled | S (`Caddyfile`: admin off, redirects off, 3 routes, header overwrite, 8 KB) |
| D-Bus TCP 47472 / unix, NIP-5L, ssh-agent | — | off | — | refused by preflight `ops.py:231` |
| Outbound relays | `wss://` (SR-08: `ws://` not refused) | ≥1 required | n/a | S |

## Runtime capability manifest (what was and was not observed)

| Property | Observed? |
|---|---|
| Host Release build SQLCipher ON / passkeys ON / test hooks OFF | yes (`build-review/`) |
| Image build flags, `ldd`, labels, embedded files | **no** (no `docker build` run) |
| In-image TLS verification (bookworm OpenSSL/lws) | **no** |
| `/proc/<pid>/status` Cap*/NoNewPrivs/VmLck, limits | **no** |
| Droplet `ss -lntp`, off-host 8080/9487 failure | **no** |

## Verification (what ran)

| Check | Result |
|---|---|
| `cmake -S . -B build-review -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIGNET_ENABLE=ON -DBUILD_TESTING=ON -DSIGNET_USE_SQLCIPHER=ON -DSIGNET_ENABLE_PASSKEYS=ON -DSIGNET_ENABLE_TEST_HOOKS=OFF` + `ninja signet/signetd signet/signetctl signet/tests/all` | PASS (after `git -c protocol.file.allow=always submodule update --init third_party/nostrdb`) |
| `ctest --test-dir build-review/signet/tests --output-on-failure` | 42 passed, 1 failed, 2 disabled (`live_dbus_interop_manual`, `production_roundtrip`: manual/live). The failure was `deployment_contract`: host Python lacked `jsonschema` (environment, not product). |
| `PATH=/tmp/sr-venv/bin:$PATH bash signet/tests/test_deployment_smoke.sh --static` (venv with `jsonschema`) | PASS, 9/9 |
| `ctest -R '^(backup_restore\|config_reload\|daemon_startup)$' -V` | PASS; per-case output quoted in the matrix |
| `ctest --test-dir build-review -R '^test_connection_tls_no_resumption$'` | PASS: ClientHello TLS ≥ 1.2, ECDHE AEAD only, provider groups preserved; untrusted CA **and** wrong hostname refused; no resumption |
| `/tmp/sr-scripts/repro_listen_challenge.sh` (real signetd) | lsof shows only loopback binds; LAN connects refused; header matrix 403/200/403 |
| `/tmp/sr-scripts/repro_challenge2.sh` (real signetd, N=4000) | 4000×200; RSS 23,696 → 49,424 → 49,632 KiB (SR-04) |

Six read-only explore probes produced the first-pass source reading. Every load-bearing file:line used above was re-read in the worktree, except the few the text marks as probe-cited and not re-read (SR-16's exit-code lines, SR-17's serializer lines, SR-18's line list). Not run: Meson suites, ASan, Docker image builds, Droplet checks, live relays, python-fido2 interop, Linux UHID. `nostrc-nfnw` (ASan, 31 pre-existing failures) was not rerun. Its listed failures are libnostr/client-side; W2.A's focused ASan relay-pool tests passed. Not a Signet launch blocker. `nostrc-57dv3` (Meson FIDO test dependency) is test-infrastructure only. `nostrc-58bop`: `signet/CMakeLists.txt` links neither nip55l nor nip49, so it is not on the deployed Signet path and not a blocker.

## Blocker disposition (plan §12.4)

| # | Blocker | Disposition | Evidence pointer |
|---|---|---|---|
| 1 | Health/bootstrap bind to all interfaces | **Cleared (code + host)**; Droplet `ss`/off-host confirmation remains a W4 step | `http_listen.c`, `health_server.c:372-379`, `bootstrap_server.c:566-574`, `signetd_main.c:1009,1142`; repro lsof + LAN refusal; CTest `http_listen` |
| 2 | Missing config/key yields ephemeral operation | **Cleared (host)** | `signetd_main.c:667-740`, `signet_config.c:617-674`; `daemon_startup` 10/10 |
| 3 | Provisioner authority drift | **Cleared (code + test)**; rw `/etc/signet` and no env override are enforced by Compose/preflight (`docker-compose.yml:27-35`, `ops.py:210`) | `config_reload.c:245-330`; `config_reload` signed D2 + adoption-retry PASS |
| 4 | AUTH/publish OK collision; callback UAF | **Cleared (host)**: `nostrc-izf9`, `nostrc-8str` closed | CTest `relay_pool_ok_lifetime`, `relay_pool_wire`, `relay_pool_protocol` PASS |
| 5 | Ineffective outbound TLS hardening/verification | **Open: image evidence missing.** Host proof: hardening applies; untrusted CA and wrong hostname refused | `test_connection_tls_no_resumption` PASS on macOS/OpenSSL 3.6.5. Needs the same test (or an equivalent probe) in the bookworm image; the Docker test stage runs only Signet Meson suites (`Dockerfile:32-43`) |
| 6 | Unresolved critical/high NIP-46/management findings | **No critical/high found by this review; open pending W6** independent review and owner sign-off (`nostrc-1r5d.3`, `nostrc-mk7v1.7`). Medium SR-01/02/03 need owner acceptance | this document |
| 7 | No proven launch client enrollment/reconnect | **Open** (W4, `nostrc-1cx`); not run here | — |
| 8 | SQLCipher custody review / deployed-image proof | **Open**: host SQLCipher CTest green (`key_store`, `backup_restore`, `store*`); independent review and in-image encrypted restart/restore not done | CTest log |
| 9 | No trustworthy recovery procedure | **Open** (fresh-host restore drill, W4) | `backup_restore` round trip is not a drill |
| 10 | Volume absence creates fresh state | **Open (Droplet demonstration)**; code present: exact mountpoint, UUID, DO device, volume id, before any state path (`ops.py:145-153,198`) | `deployment_contract --static` 9/9 |
| 11 | Idle core saturation | **Open**; not examined at runtime | — |
| 12 | Version/tag mismatch | **Open until tag time.** Signet sources agree on 0.8.0 (`signetd_main.c:107`, `signetctl_main.c:44`, `meson.build:17`, `CMakeLists.txt:10`, `VERSION_MANIFEST.md:55`); libnostr still declares 2.1.2 after the `connection.c` TLS change (bump owned by the sibling branch) | — |
| 13 | Key ceremony not performed | **Open** (operator-present, W4) | — |
| 14 | Online backup consistency unproven | **Cleared at test level (host)**: concurrent-writer online backup restores to a consistent, verified store. Droplet drill still W4 | `test_online_backup_consistency: PASS (concurrent writes observed)` |
| 15 | Replay window across restart not bounded | **Cleared with disposition**: ≥270 s rule enforced on all supported start paths, skew ≤120 s enforced; durable marker not required for rc.1 (SR-02 residual, `nostrc-17ggo`) | `ops.py:155-185,218`, `signet-compose.service:12-17` |

## Version decision

This review and the THREAT_MODEL edit change documentation only: **no component bump**. The already-declared Signet **0.8.0** MINOR (`VERSION_MANIFEST.md:55`) covers the merged W1–W5 interfaces. libnostr needs its own PATCH assessment for `connection.c` (`nostrc-86xt`); that is outside this branch. Each finding's remediation gets its version assessment in its bead.

## Merge recommendation

**Merge** the review and THREAT_MODEL commits (documentation only). **Do not tag `signet-v0.8.0-rc.1` yet.** Blockers 5–13 need deployed-image, Droplet, operator or W6 independent-review evidence. Before W6, the owner should accept or schedule the six Medium findings (SR-01–SR-06). SR-04 and SR-05 are disposed by keeping the bootstrap profile disabled; SR-06 by keeping ctap unsupported.
