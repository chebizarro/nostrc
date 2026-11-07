# Signet production deployment plan: focused critique

Date: 2026-11-06 · Reviews `docs/plans/signet-production-deployment-2026-11-06.md` (the "plan") against the context_builder export `prompt-exports/oracle-plan-2026-11-06-135055-deep-plan-signet-pro-000e.md` (the "export"; only its `## Generated Plan` section, line 76 onward, is treated as the baseline).

## Context and scope

The critique covers only: export content the plan dropped or weakened; under-specified seams and contradictions; claims the code disproves; gaps that both documents miss; and questions that would change the design. Owner decisions in plan §1.1 are taken as fixed. I spot-checked these seams in code with targeted reads: `config_reload.c`, `store.c` (provisioner reconcile, backup), `mgmt_protocol.c` (write-back, replay), `signetd_main.c` (subscriptions, seeding, adoption, listeners), `health_server.c`, `signetctl_main.c` (help text, adopt/restore), `nip46_server.c` (replay), and `signet/Dockerfile`. I ran no builds or tests.

Severity: **H** = a plan or launch claim is wrong, or a custody/authority invariant breaks in the planned layout. **M** = an under-specified seam that will produce wrong implementation or operations. **L** = a correctness detail or a dropped specific.

---

## Findings (ranked)

### H1. The plan's provisioner-authority correction overcorrects. In the planned read-only layout, revocations *can* be reversed

The plan (§3.3 "Provisioner authority", §7.E, §3.7 seam 4) replaces the export's claim "config reconciliation could reverse revocations" with "reload refuses to resurrect runtime-revoked keys … write-back fails (critical log, authorization still correct) under a read-only config mount". The code does not support that unconditionally:

- **The resurrection guard depends on mtime.** `store.c:2574` refuses only when `desired_source_mtime <= revoked_at`. A config file modified *after* the revocation is treated as a deliberate re-grant (`store.h:798–812`).
- **Stale config revokes runtime grants.** Reconcile revokes every authorized key the config does not list (`store.c:2598–2614`). The code says so itself: `mgmt_protocol.c:~2782` ("a grant that never reaches config would be revoked by the next" reload).
- **The startup adoption is missing from the plan.** `signet_config_reload_adopt_persisted()` runs unconditionally at startup (`signetd_main.c:1204–1211`). It sets its once-only marker *before* attempting write-back (`config_reload.c:237–240` vs `:282–291`). Under a read-only mount the write fails, the marker is still set, and convergence is never retried. The plan's §7.E and §13 do not mention this function or its `SIGNET_PROVISIONER_ADOPTION_MARKER`.

With `/etc/signet` read-only (§9.B), two concrete chains follow:

1. Runtime `config/grant-provisioner` → write-back fails → the next SIGHUP revokes the grant.
2. Runtime revoke of seed key K → write-back fails, so the config still lists K → the operator later edits any setting (for example relays), which bumps the mtime → SIGHUP → K is re-granted.

The plan's §5 fast path includes W3.B (read-only config) but **omits W1.E**, so both chains are live in the soak deployment.

**Correction:**
- Restate §3.3 and §7.E as: "the refusal applies only while the config mtime ≤ the tombstone; stale config revokes runtime grants; startup adoption marks before it writes".
- Add `signet_config_reload_adopt_persisted` and its marker to §7.E and §13 (`config_reload.c`, `signetd_main.c:1204–1211`).
- Either add W1.E to the fast path, or mount the config read-write for the soak and document D2 semantics there.
- Add chains 1 and 2 to the §14 "Provisioner authority" test.

### H2. `--enable-backups` puts the master key and bunker nsec into provider-held backups

§10.B `doctl compute droplet create … --enable-backups` (also in the export) backs up the root disk. Per §10.A, the root disk holds `/opt/signet/secrets/{db.key,bunker.nsec}`, plus Docker's registry credential and wherever the Spaces credential lands. Meanwhile §10.E ships the SQLCipher backups to Spaces in the same DO account. One provider-account compromise then yields both the encrypted DB and its master key. That contradicts §10.E ("never upload master/backup/bunker … secrets with backups") and §10.C step 6 (escrow under operator custody).

**Correction:** Drop `--enable-backups`. The root disk is rebuildable from the release bundle plus escrow and holds no state. If droplet backups are still wanted, move secrets to a location they do not cover, and record the residual risk in the threat model.

### H3. `restart: unless-stopped` bypasses the mount-gated preflight

§9.B sets `restart: unless-stopped`. §10.B says `signet-compose.service` is "mount-gated, runs `preflight.sh` before `compose up` … not a second supervisor". But dockerd restarts `unless-stopped` containers itself at boot, independent of that unit. So on reboot the volume-UUID, secret-file and digest checks never run. §10.H "Wrong volume" (a UUID mismatch fails even if the mountpoint exists) is then not enforced. A wrong volume that happens to contain `state/` starts the signer against the wrong store.

**Correction:** Make systemd the only supervisor. Use `restart: "no"` in Compose, and have the unit run preflight and then `docker compose up` with `Restart=on-failure`. Alternatively, keep Docker's restart policy and add a `docker.service` drop-in with `RequiresMountsFor=/srv/signet`, accepting and documenting that this checks the mount but not the UUID.

### H4. Daily offline backups turn the RAM-only replay window into a routine daily exposure

Both NIP-46 (24133) and management (1059/25910) resubscribe with `since = now − 49 h` (`signetd_main.c:53,65–79,371,428`). Replay protection is in-RAM only:
- Management: rumor `created_at` must fall within a 120 s skew, and cache TTL is 2×skew (`mgmt_protocol.c:472,641–656,1854–1889`).
- NIP-46: an outer-ID cache (`nip46_server.c:490–494`).

Any request processed in roughly the 120 s before a stop, and redelivered after a restart shorter than about 120 s, executes again. Plan §8.C lists "replay across restart" only as an audit topic, while §10.E schedules a stop/start every day and §10.G adds one per upgrade. Each restart also re-unwraps up to 49 h of gift-wrap backlog (two NIP-44 decrypts per wrap before the rumor skew check) on a 1-vCPU host.

Also, the offline requirement is a choice, not a code constraint. `backup-db` is documented as a "point-in-time" backup with no stop requirement; only `restore-db` says "offline; stop signetd first" (`signetctl_main.c:116–122`). `signet_store_backup()` checkpoints the WAL and runs `sqlcipher_export` from an open connection (`store.c:1095–1160`).

**Correction:**
- In `backup.sh` and `release.sh`, keep the signer stopped for at least skew plus margin (≥150 s) before starting it again, and record that rule in §10.E/§10.G and in the threat model.
- Alternatively, resolve Q2 and switch to an online backup, which removes the daily restart entirely.

### M1. The concrete firewall command contradicts the prose and will break provisioning

The §10.B command opens outbound TCP 443, UDP 53 (v4) and UDP 123 (v4) only. Problems:
- **apt over HTTP.** DO Ubuntu images default to an HTTP apt mirror (`mirrors.digitalocean.com`). Verify this, and if so, cloud-init package installs fail once the firewall attaches.
- **DNS gaps.** There is no TCP 53 fallback, and no v6 rules for DNS or NTP.
- **Ordering.** The firewall is created *after* the droplet, so cloud-init runs with no rules attached.
- **IPv6.** No `--enable-ipv6` is set, yet §8.C and §14 require off-host v6 negative tests.

**Correction:**
- Create the tag-scoped firewall before the droplet.
- Either switch apt sources to HTTPS or add TCP 80 for the mirror.
- Add TCP 53.
- Decide IPv6 explicitly: enable it with v6 rules, or drop the v6 test rows.

### M2. Where the image digest lives is contradictory, so symlink rollback does not roll back

§14 runs `docker compose --env-file /opt/signet/config/deploy.env`, and §9.B requires `SIGNET_IMAGE` there. That puts the digest in the mutable site-config directory. But §10.A puts digests in `releases/<commit>/manifest.json`, and §10.G performs upgrade and rollback by switching the `current` symlink. Switching the symlink leaves `deploy.env` unchanged, so the old digest keeps running.

**Correction:** Put `SIGNET_IMAGE` and the Caddy digest in a per-release env file inside the bundle (`/opt/signet/current/release.env`), and keep only site settings in `config/deploy.env`. `release.sh` and `preflight.sh` should assert that the running digest equals the manifest digest.

### M3. Backup and recovery custody seams are missing from the mount and host tables

- **Backup key.** The backup-key file has no host path or mount row (§9.B, §10.A list only db/bunker/passkey keys). §7.B's resolver requires owner = effective UID with mode 0400, so the file must be `10001:10001`.
- **Staging directory.** `/srv/signet/backups` must be mounted read-write into `signet-ops` and be writable by UID 10001. `backup-db` creates its target with exclusive 0600 (`store.c:1126–1135`).
- **Restore runner.** §10.F does not say whether restore and `verify-audit` run through `signet-ops`. Host-root `signetctl` would fail §7.B's ownership check.
- **Spaces credential.** Its location, scope (single bucket), rotation, and exclusion from H2's root-disk backups are unspecified. A host credential that can delete objects also lets an attacker erase all backups.

**Correction:** Add host-path and mount rows for the backup key and staging directory. State that all local store commands run via `signet-ops` as UID 10001. Specify where the Spaces key lives and its least-privilege scope.

### M4. Removing D2 makes config provisioner edits silent no-ops, and the reversal of a recorded decision has no recorded approval

D2 ("config is desired state") is a documented design decision (`config_reload.h:24–38`, commit `8292a560f`, fp-56t). It lists `[nostr] provisioner_pubkeys` as live-reloadable (`config_reload.h:11–13`). After §7.E removes it, an operator who "revokes" a compromised provisioner by editing the config gets no effect and no warning. Plan §1.1 does not record owner approval for reversing D2.

**Correction:**
- Move `provisioner_pubkeys` to the D1 restart-only/seed-only warning set (`reload_warn_restart_only`).
- On reload and at startup, log a warning whenever the config list differs from the persisted set.
- Update the `config_reload.h` header.
- Record the D2 reversal as an owner decision (see Q1).

### M5. Under host networking, Caddy's admin API and "loopback = trusted proxy" are host-wide trust

§9.C and §7.D cover neither point.
- **Caddy admin API.** Caddy's admin endpoint defaults to `localhost:2019`. With `network_mode: host`, any host process or host-network container can rewrite the proxy config, for example to proxy `/metrics` or to forward a client-supplied `X-Forwarded-Proto`.
- **Trusted-proxy default.** The default `trusted_proxy_cidrs` (loopback) means *any* local process can reach `127.0.0.1:9487` directly with `X-Forwarded-Proto: https`.

**Correction:**
- Set `admin off` in the Caddyfile, or use a unix-socket admin endpoint.
- Disable Caddy's port-80 redirect listener, or list it in the exposure matrix.
- Document in §7.D and the threat model that, under host networking, the trusted-peer check separates on-box from off-box only.

### M6. Export specifics that the plan dropped or weakened

| Export (line) | Plan | Correction |
|---|---|---|
| Bootstrap server: "Stop its listener before freeing any borrowed dependency" (§2.1, :106) | §3.1 lists the borrowing; §7.C's "one orderly unwind path" gives no order | Add the rule to §7.C for both the startup unwind and shutdown |
| Key store: "callbacks must not re-enter the key store" (§2.1, :100) | dropped | Add to §7.F: step 3 holds the mutex across the durable mutation |
| "AUTH success reissues the stored complete filter, **including recipient and time scope**" (§3.4.A, :412) | "reissues the stored complete filter" | Restore the wording, since the `since` floor drives H4 (`signetd_main.c:412–428`) |
| Ambiguity applies to "**Nonempty** value and nonempty file path together" (§3.3.B, :244) | "value and file both set" | Decide explicitly. Compose `.env` interpolation commonly yields `SIGNET_DB_KEY=`; the export's rule keeps that working |
| Tracker records: source tracker, exact ID, status date, affected profile, dependency, disposition; join by scope/dependencies, not title (§3.8, :926–927) | W0 step 4 "reconcile §12"; the §12 tables have no status-date or dependency columns | Restore the six fields and the join rule in W0 step 4 and §12 |
| "Keep builds/tests outside the static-only push hook" (§4.1, :1055) | dropped | Restore under §14 setup |
| "CI publication uses repository-scoped permissions" (§3.5.D, :614) | dropped | Restore in §9.D |

### M7. The fast path claims "the same custody and recovery checks" but excludes them

§5's fast path runs W4.A–E only, which skips W4.F (restore drill) and W4.G. It also skips W1.E (see H1) and W1.G (inline `--sec` rejection). Yet it claims "the same custody and recovery checks", and §12.4 blocker 9 requires a fresh-host restore drill.

**Correction:** Add W4.F, W1.E (or a read-write config, per H1) and W1.G to the fast path, or drop the "same checks" claim and list the waived blockers explicitly.

### M8. "Observable publication outcomes" for NIP-46 replies has no contract

§13's `nip46_server.c` row and §7/§8.A do not say what happens when a reply is rejected or not acknowledged: log, metric, retry to other relays, or nothing.

**Correction:** Specify the behaviour:
- Log the per-relay ACK result with no secrets, and add a counter to `/metrics`.
- Republish only the *already-signed* reply event, and only to other connected relays.
- Never re-sign or re-execute.
- Make `/ready` independent of reply ACKs.

### L1. Startup and probe details

- **Probe port.** The Dockerfile `HEALTHCHECK` hardcodes `127.0.0.1:8080` (`signet/Dockerfile:121–122`). §9.B should define the Compose healthcheck from the configured `health_port`, or pin that port in the production config.
- **Stub becomes fatal by default.** §7.C makes `health_server_stub.c` fatal for a nonzero port, but the default `health_port` is 8080 (`signet_config.c:230–231`). CMake builds without libmicrohttpd will therefore fail to start by default. Add this to §15.1 release notes.

### L2. Image rollback has no mechanism for "provably reads the current schema"

§10.G gates image rollback on proof that the old binary reads the current schema, but nothing produces that proof, and the store migrates columns on open (`store.c:~392`).

**Correction:** Record the store schema version in the manifest before and after each upgrade. `release.sh` should refuse an image-only rollback when the schema version changed.

### L3. Audit records roll off silently under the planned log settings

The audit logger defaults to stdout (plan §3.5). Combined with §9.B's 3×10 MiB Docker log rotation, audit events are discarded silently.

**Correction:** Make `[audit] path=/data/audit.jsonl` required in the production config (not "optional", §9.B), include it in the backup bundle, and state its retention.

### L4. CLI help example still teaches inline secrets

`signetctl_main.c:136` shows `adopt-existing my-agent --sec nsec1...`. §7.G fixes only the reissue help text.

**Correction:** Change that example to `--sec -`.

---

## Questions that change the design or the order of work

1. **D2 reversal (H1, M4).** Does the owner approve replacing "config is desired state" (fp-56t) with persisted-only authority? If not, the alternative is to keep D2 with a read-write `/etc/signet` and fix the mark-before-write adoption. That changes §7.E, §9.B and the fast path.
2. **Online backup (H4).** Is `signet_store_backup()` point-in-time consistent across tables while signetd writes, i.e. does `sqlcipher_export` run inside a single read transaction? If yes, an online backup removes the daily restart, the replay window and the downtime. If no, specify the stop-plus-skew rule.
3. **Droplet backups (H2).** Are DO droplet backups required for any reason the release bundle plus escrow cannot cover? The answer decides whether secrets move or the flag is dropped.
4. **IPv6 (M1).** Is IPv6 enabled on the Droplet? That determines the firewall rules, the v6 listener tests and §7.C's IPv6 acceptance scope.
5. **Independent reviewer (`nostrc-1r5d.3`).** Who performs it, and when? It gates blockers 6 and 8, but no owner or slot appears in §16.

## Recommendations (summary)

Before W1 implementation starts, fix H1 (restate the observed D2 behaviour, include startup adoption, align the fast path) and settle Q1. Fold H2, H3, M1, M2 and M3 into §9.B/§10 before writing `cloud-init.yaml` and `create-droplet.sh`. Resolve Q2 before writing `backup.sh`. Restore the dropped export specifics in M6 when §7 is next edited.
