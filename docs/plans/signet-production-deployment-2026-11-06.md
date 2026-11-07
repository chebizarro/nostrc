# Signet first production deployment: Plan

Date: 2026-11-06 · Component: `signet/` (signetd, signetctl) · Declared version: 0.7.0, unreleased (`VERSION_MANIFEST.md:55`) · Target: **`signet-v0.8.0-rc.1`** · Tracking epic: **`nostrc-mk7v1`** (children `.1`–`.7` = W0–W6)

## 1. Goal and scope

Prepare Signet for its first production deployment on **one DigitalOcean Droplet (Ubuntu 24.04) running Docker Compose**, delivering:

1. **Packaging for rapid deployment** — versioned, digest-pinned images; a single supported Compose baseline; cloud-init/doctl provisioning; backup, restore, upgrade and rollback procedures.
2. **A full security audit** — a dated review under `docs/reviews/`, one bead per finding, governed by `signet/docs/THREAT_MODEL.md`; critical/high findings sequenced as launch blockers; NIP-46/relay-pool paths (epic `nostrc-ot2c`) in scope.
3. **Documentation updates** — reconcile README/INTEGRATION/examples/threat model with observed code.
4. **A frank implemented-vs-promised assessment** — per-feature table with doc ref → code ref → status → launch action.
5. **A consolidated summary of outstanding beads and nostrig issues** — classified by deployment relevance, ending in an explicit blocker list so the owner picks the release cut.

### 1.1 Owner decisions (2026-11-06; amended 2026-11-07)

| Topic | Decision |
|---|---|
| GHCR visibility (2026-11-07) | Public — repo is public, image carries no secrets; Droplet pulls need no credential. |
| Hosting | Single Droplet + Docker Compose now; App Platform and DOKS recorded as later paths only (App Platform is unsuitable as built: no persistent volumes, no `cap_add`/devices, gVisor). |
| Surfaces in scope | NIP-46 + management over outbound relay WebSockets; HTTP health/metrics **loopback-only, scraped on-box**; HTTP bootstrap/enrollment server (packaged, disabled); passkeys / virtual CTAP-HID (programmatic acceptance). |
| Surfaces out of scope | **Go MCP wrapper (`signet-mcp`) — dropped from this release (second round)**; D-Bus TCP (47472), NIP-5L and ssh-agent unix sockets, D-Bus system bus, public health exposure, `signet-git-credential` in the container. |
| Audit shape | Report under `docs/reviews/` + bead per finding; `THREAT_MODEL.md` governs and README claims are reconciled to it; critical/high findings are launch blockers; relay-pool/NIP-46 (`nostrc-ot2c`) included. |
| Release cut (second round) | **`signet-v0.8.0-rc.1`**: all four version sources bump to 0.8.0 in one commit; the rc tag is published only after blockers 1–15 (§12.4) are cleared or explicitly dispositioned. Promotion to `signet-v0.8.0` follows a soak. |
| Nostrig discount rule (second round) | Nostrig items whose acceptance or confirmation requires the local cascadia deployment stack (bahia-*, fp-*, platform-*, stage01-*, hiveci-*, fleet-*, openclaw-*, interlocutor-*, groups-relay-*, cascadia-go-*, max-loom-*) are **discounted** — not rc.1 gates. Signet- and libnostr-owned items remain. |
| Enrollment proof client (second round) | The in-repo nostr-homed live test `homed_broker_login_nip46_live` (label `live;manual`, `NH_NIP46_LIVE=1`) plus a scripted reference client under `signet/tests/integration/`. OpenClaw is discounted. |
| Provisioner authority (second round) | D2 kept as specified in §7.E (owner confirmed). |
| Independent review (second round) | A separate design/review agent run produces the independent NIP-46/custody review (house format under `docs/reviews/`) **before** tagging rc.1; owner signs off. Closes `nostrc-1r5d.3` for signet scope. |
| HTTP `/auth` sessions (second round) | No consumer this release → bootstrap profile ships disabled; its audit findings cap at Medium unless exploitable while disabled. |
| Passkey PSK (second round) | Single-instance fleet PSK; no cross-instance sync at launch. |

### 1.2 Evidence boundary

This plan is based on source inspection, tracker queries (`bd`, `nostrig task list`) and DigitalOcean documentation checked on 2026-11-06. **No builds, tests, cloud operations or audits were executed.** Every command in §14 is planned, not run. Tracker snapshots and DO facts must be refreshed at implementation and release time.

## 2. Summary of approach

Use the existing daemon, CLI, SQLCipher store, policy engine and relay transports; make **targeted** security and packaging corrections rather than redesigning Signet. The launch baseline is persistent encrypted NIP-46 signing, gift-wrapped management, operator-assisted enrollment, and genuinely loopback-only observability. HTTP enrollment and passkeys are **audited and honestly labelled**, not misrepresented as production capabilities; the Go MCP wrapper is dropped from this release and recorded as a stub. The work is five streams (W1 runtime safety, W2 relay fixes + audit, W3 images/Compose/release, W4 Droplet provisioning + operations, W5 docs/assessment/tracker reconciliation) preceded by a baseline step (W0) and closed by an independent review and the rc.1 tag (W6). Not prerequisites for the baseline: replacing MCP, a new enrollment protocol, historical writer-epoch migration, repository-wide sanitizer repair.

## 3. Current state

### 3.1 Components, ownership, constraints

| Component | Responsibility | Constraint that shapes the plan |
|---|---|---|
| `SignetConfig` / `signet/src/signet_config.c` | defaults → GKeyFile INI → environment (`:3–6,598–615`); owns bunker identity material | An explicitly supplied missing/malformed file falls back silently to defaults + env |
| `signet/src/signetd_main.c` | Wires stores, policies, replay caches, relay pool, handlers, optional transports, HTTP servers | Several startup failures log and continue (bootstrap listener `:956–991`, health `:1105–1113`, `mlockall` `:600–639`, `PR_SET_DUMPABLE` `:590–598`) |
| `SignetKeyStore` / `key_store.c` | Owns `SignetStore`, `sodium_malloc` hot cache, ephemeral bindings, mutex | Lock order: key-store mutex → SQLite mutex/transaction; empty master key → cache-only mode (`:247–275,575–605`) |
| `SignetStore` / `store.c` | SQLite/SQLCipher connection, DEK derivation (`:287–338,680–700`), envelope encryption (`:1448–1460`), migration, backup/restore (`:1095–1286`) | SQLCipher passphrase and envelope DEK both derive from the same master string; stored formats must stay byte-compatible |
| `SignetRelayPool` / `relay_pool.c` | Outbound relays, subscriptions, NIP-42 AUTH (`:248–415`), publish | Callbacks on worker threads; deferred GLib callbacks hold raw pointers (`nostrc-8str`); AUTH and publish share one OK slot (`nostrc-izf9`) |
| `SignetNip46Server` / `nip46_server.c` | NIP-44 v2 decrypt (`:478–529`), binding + deny + policy (`:566–692`), custody op, encrypted reply (`:921–950`) | Response publish passes no ACK callback — enqueue success ≠ relay acceptance |
| `SignetMgmtHandler` / `mgmt_protocol.c` | Provisioner/self-service/delivery-grant authz (`:1007–1039,1603–1858`), replay (`:641–675,1824–1907`), mutations, gift-wrapped replies | Direct kind-25910 content is readable on the wire (`:1212–1302`); gift-wrap 1059 is confidential (`:1304–1323`) |
| `SignetHealthServer` / `health_server.c` | MHD daemon, mutex-protected snapshot, `/health` `/ready` `/metrics` (`:128–267`) | **Bind bug:** parses only the port, no `MHD_OPTION_SOCK_ADDR` (`:342–362`) → binds all interfaces despite `"127.0.0.1:%d"` (`signetd_main.c:1105–1108`) |
| `SignetBootstrapServer` / `bootstrap_server.c` | `POST /bootstrap`, `GET /challenge`, `POST /auth` (`:130–147,305–407,435–476`); borrows stores/challenges/fleet/audit | Same bind bug (`:536–559`); TLS = first `X-Forwarded-Proto: https` with no proxy allowlist (`:63–89,497–510`) |
| `signetctl` / `signetctl_main.c` | Signs management JSON-RPC with provisioner key, NIP-17 gift-wraps, validates correlated replies; local store commands | Needs bunker **public** key + provisioner nsec file (`:128–131`); `adopt-existing --sec <nsec>` accepts argv (`:64–68,801–848`); `reissue-connect --show-secret` prints secret (`:356–413`) |
| `signet/Dockerfile`, `signet/docker-compose.yml` | Build + run with persistent `/data`, Compose secret for bunker nsec, hardening | Config instructions name a non-existent file; health port published; passkeys OFF; `setcap cap_ipc_lock`; no `.dockerignore`; unpinned base |

### 3.2 Deployable surfaces (observed defaults)

| Surface | Default | Config key / env | Evidence |
|---|---|---|---|
| NIP-46 + mgmt over outbound relay WS | always on; ≥1 relay required | `[nostr] relays` / `SIGNET_RELAYS` | `signetd_main.c:881–917`, `signet_config.c:786–790` |
| HTTP health | on, port 8080 | `[server] health_port` / `SIGNET_HEALTH_PORT` (0 disables) | `signet_config.c:230–231` |
| HTTP bootstrap | off (port 0) | `[bootstrap] port` / `SIGNET_BOOTSTRAP_PORT` → `0.0.0.0:<port>` | `signet_config.c:225–228,376–378`, `signetd_main.c:967–973` |
| D-Bus unix / TCP | off / off (0.0.0.0:47472) | `[dbus] unix_enabled`, `tcp_enabled`, `tcp_port` | `signet_config.c:380–388` |
| NIP-5L / ssh-agent sockets | off | `[nip5l]`, `[ssh_agent]` (`/run/signet/*.sock`) | `signet_config.c:390–400` |
| Virtual CTAP-HID | build-gated, runtime off | `[passkeys] virtual_ctap`; needs `/dev/uhid` | `signet.conf.example:103–118`, `fido_ctaphid.c:607–656` |
| `signet-git-credential`, `signet-mcp` | separate stdio executables | — | `git_credential_main.c:3–24`, `sdks/go/cmd/signet-mcp/main.go:16–37` |

### 3.3 End-to-end paths

- **Signing:** client kind 24133 → relay-pool signature verify (`relay_pool.c:100–135`) → NIP-44 decrypt → parse → client-binding + deny list → `policy_engine` (`policy_store.c:735–774`; kind gating via `allow_kinds`, default deny — kind 1 is not a hardcoded exception, `GRANT_LOGIN_KIND.md` documents a policy example) → transaction-bound custody op → encrypted signed reply → `signet_relay_pool_publish_event_json_ack(..., NULL, NULL)`.
- **Management:** `signetctl` → JSON-RPC → NIP-17 gift wrap → relay → unwrap, seal author = sender → provisioner/self-reissue/`credential.deliver` authz → replay mark (3 caches, 16,384 total, 512 per non-provisioner scope, fail-closed) → store mutation → encrypted correlated reply. Direct kind-25910 ingress bypasses transport confidentiality; `THREAT_MODEL.md`'s "plaintext management mutations are rejected" wording is too broad.
- **HTTP:** daemon builds listen string → server copies → `start()` extracts port only → MHD without `MHD_OPTION_SOCK_ADDR`.
- **Persistence:** master string → SQLCipher `PRAGMA key` + BLAKE2b-derived DEK → agent nsecs (`crypto_secretbox_easy`) and credential payloads (per-agent BLAKE2b key, XSalsa20-Poly1305, `store_secrets.c:17–26,123–145`). `connect_secret` is a plaintext TEXT column protected only by SQLCipher (`store.c:417–426,1475–1500`). `SIGNET_REQUIRE_ENCRYPTED_DB=true` (`store.c:712–723`) is enforced only when a store is opened; an empty `SIGNET_DB_KEY` never opens one.
- **Enrollment (live path):** `signetctl provision <agent> --deliver <pubkey>` → `agent/provision` → NIP-17 DM `signet/bunker-handoff` with bunker URI (`signetctl_main.c:700–718`, `mgmt_protocol.c:1944–1988`, `bootstrap_delivery.c:25–76`); client `connect` with one-time secret, atomically consumed, pubkey bound (`nip46_server.c:554–619`). HTTP token insertion and receive/ACK helpers have **no production callers** (`bootstrap_delivery.c:82–155`, `store_tokens.c:17`); `signet_store_get_active_session_by_token` is used only by `tests/test_bootstrap_contract.c:102–127`.
- **Provisioner authority (D1/D2, fp-56t; `signet/include/signet/config_reload.h:3–38`):** first-boot seed from config/env when the DB has never carried provisioner policy (`signetd_main.c:733–746`). `[nostr] provisioner_pubkeys` is **live-reloadable**: SIGHUP runs D2 reconciliation (`config_reload.c:124–203`, `store.c:2574–2614`) where **config is desired state** — keys listed in config are granted, keys not listed are revoked and tombstoned (`store.c:2598–2614`), and a listed key with a tombstone is refused **only while the config file's mtime ≤ the revocation time** (`store.c:2574`; a config edited after the revocation re-grants it). The effective set is written back into the INI (`config_reload.c:189–203`); an env-sourced list (`SIGNET_PROVISIONER_PUBKEYS`) cannot be written back and is treated as older than any revocation (`:176–180`). At startup `signet_config_reload_adopt_persisted()` (`signetd_main.c:1204–1211`) adopts the persisted set once, but sets its once-only marker (`SIGNET_PROVISIONER_ADOPTION_MARKER`) **before** attempting write-back (`config_reload.c:237–240` vs `:282–291`), so a failed write is never retried. Consequences for a read-only config mount: a runtime grant is revoked by the next SIGHUP (write-back failed, config lacks it), and a runtime-revoked seed key is re-granted after any later config edit bumps the mtime. README:258 ("legacy one-time seed") understates D2.

### 3.4 Build, CI, release, version (observed)

- Artifacts: `signetd`, `signetctl`, `signet-git-credential`, static `signet_core` (not installed), `signet.conf.example` (CMake installs to `${datadir}/nostrc`; Meson does not). Neither build installs `signet/dbus/*` nor a systemd unit (`signet/CMakeLists.txt:184–219`, `signet/meson.build:303–343`).
- Options default OFF in both builds: `SIGNET_USE_SQLCIPHER`, `SIGNET_ENABLE_PASSKEYS` (libcbor), `SIGNET_ENABLE_TEST_HOOKS` (`CMakeLists.txt:39–50,88–105`, `meson_options.txt:5–40`). CMake treats libmicrohttpd as optional and compiles `health_server_stub.c` (`CMakeLists.txt:159–169`); Meson requires it.
- `signet/Dockerfile:11–60,80–136`: `debian:bookworm-slim` builder + runtime (unpinned), `COPY .` of monorepo root, CMake Release SQLCipher ON / passkeys OFF, builds only `signetd` + `signetctl`, runs Meson tests (SQLite + SQLCipher) in builder, runtime `setcap cap_ipc_lock+ep signetd`, `USER signet`, `VOLUME /data`, `EXPOSE 8080`, `ENTRYPOINT ["signetd"]` `CMD ["-c","/data/signet.toml"]`. No `.dockerignore`, no `LABEL`, apt unpinned, pip `cmake>=3.30,<3.31`.
- `signet/docker-compose.yml:1–74`: named volume `signet-data:/data`; Compose `secrets:` file → `/run/secrets/signet_bunker_nsec`; `SIGNET_DB_KEY` as plain env; publishes `${SIGNET_HEALTH_PORT:-8080}:8080`; `cap_add: IPC_LOCK`, `no-new-privileges`, `read_only`, `/tmp` tmpfs 10M, memlock unlimited, 256M/1 CPU. No `cap_drop`, seccomp, `/dev/uhid`, config bind-mount or bootstrap port. Header says `cp signet.toml.example data/signet.toml` — file does not exist and `./data` is not mounted; absent config silently uses defaults + env.
- CI: `.github/workflows/signet-ci.yml` is `workflow_call`/manual, invoked by `ci-selective.yml:147–154`; matrix Debug × passkeys × SQLCipher + test-hooks + Release(passkeys+SQLCipher); CMake + CTest only — no Meson, Go or Docker build/publish. `release.yml:11–20` excludes `signet-v*`; `scripts/ci/configure-release.sh:33–40` disables Signet; `tests/selection/inventory.json:30957–30975` has `package_jobs: []`, `publisher: "none"`. Component-release pattern: `.github/workflows/groundhog-release.yml:17–28,411–435`. No `signet-v*` tags exist.
- Version sources that must agree: `VERSION_MANIFEST.md:55`, `signet/meson.build:16–18`, `signet/src/signetd_main.c:105` (`SIGNET_VERSION`), `signet/src/signetctl_main.c:40` (`SIGNETCTL_VERSION`); CMake `project(signet C)` has no VERSION (`CMakeLists.txt:10`); `signet/docs/signet-docs.xml:10` says "Signet 1.0". Tag convention `<component>-v<semver>` (`AGENTS.md:123–126`).
- Prior packaging art: `docs/designs/packaging-plan-debian-fedora.md:83–85,139,304–310,854` (nostr-signet deb/rpm row); `packaging/lab-blossom/docker-compose.yml:11–43` (compose + nginx TLS + pinned image); `apps/grotto/daemon/packaging/systemd/user/grotto-daemon.service:11–73` (hardened unit); bd memory `packaging-pre-phase-complete-merge-09b919cb-track-a`.

### 3.5 Storage, crypto, process hardening (observed)

- Secret-bearing assets: `SIGNET_DB_KEY`/`SIGNET_BACKUP_KEY` (env; not wipeable), agent nsecs (secretbox + SQLCipher), credential payloads (secretbox + SQLCipher), `connect_secret` (SQLCipher only), bunker nsec (`SIGNET_BUNKER_NSEC_FILE`; value-bearing env rejected, `signet_config.c:480–527`; hex stays in `SignetConfig`), passkey PSK (INI `sync_key`, key file or `SIGNET_PASSKEYS_SYNC_KEY`, `signet_config.c:200–220,442–445,586–589`).
- Master-key rotation = manual offline export/re-import (`THREAT_MODEL.md:96–104,225–237`). `signetctl backup-db` requires SQLCipher source + independent `SIGNET_BACKUP_KEY`; restore offline, preserves `.pre-restore` (`store.c:1095–1286`, `signetctl_main.c:1078–1133`). Plaintext→SQLCipher auto-migration on open leaves a `.plaintext-backup` (bd memory `signet-sqlcipher-migration`; `test_db_migration.c:77–126`).
- Hardening present/absent: `RLIMIT_MEMLOCK` raise + `mlockall` warn-only; `PR_SET_DUMPABLE=0` warn-only; no `RLIMIT_CORE`, `MADV_DONTDUMP`, seccomp or privilege drop; SIGHUP reloads relays/policy, listeners/paths/identity are restart-only (`config_reload.c:45–100,350–427`).
- Audit logger: generic JSON API, no field allowlist (`audit_logger.c:68–76,150–180,305–366`), file mode 0600, default stdout; DB hash-chained `audit_log` for credential ops (`store.c:584–597`). `/metrics` has static labels only, no auth (`health_server.c:128–194,248–267`).
- State paths are independent (no data-dir setting): `[store] db_path` default `/data/signet.db`; `[policy_defaults] policy_file` default empty (no identities loaded; updates write the file, `policy_store.c:471–487,1041–1068`); `[audit] path`; sockets under `/run/signet/`. Replay caches are RAM-only (`replay_cache.c:89–117`, `mgmt_protocol.c:641–659`).
- Deps: OpenSSL, libsodium, SQLite/SQLCipher, GLib/JSON-GLib/Jansson, libmicrohttpd, libwebsockets (via libnostr), secp256k1, libcurl, libcbor (passkeys). `libnostr/src/connection.c:925–945` sets `LCCSCF_USE_SSL` only; `nostrc-86xt` (P3 open): the SSL_CTX hardening callback never runs, client TLS runs on lws/OpenSSL defaults.
- Analysis tooling: root CMake ASan/UBSan/TSan options (`CMakeLists.txt:124–153`); root libFuzzer harnesses (`tests/CMakeLists.txt:202–379`, `ENABLE_FUZZING` off); no Signet-specific sanitizer/fuzz targets; `nostrc-nfnw` tracks 31 pre-existing ASan ctest failures incl. nip46.

### 3.6 Passkeys and MCP (observed; MCP is out of scope for rc.1 — facts retained for the assessment only)

- Programmatic passkeys **implemented**: NIP-46 `webauthn_make_credential` / `webauthn_get_assertion` policy-checked (`nip46_server.c:445–470,679–742,830–872`); D-Bus `MakeCredential`/`GetAssertion` (`dbus_common.c:421–459,633–644`); requires passkeys build + store + fleet PSK (`fido.c:50–53`, `signet_config.c:806–820`); headless UP always asserted, UV default-deny unless `allow_headless_uv` (`fido.c:248–250,327–330,489–491`).
- Virtual CTAP-HID **partial**: Linux `/dev/uhid` (`fido_ctaphid.c:607–656`); handles GetInfo/MakeCredential/GetAssertion; rejects clientPIN/selection/reset/GetNextAssertion (`:246–300,373–400`); `test_fido_ctaphid.c:47–122` uses in-memory frames; `live_dbus_interop_manual` CTest disabled (`tests/CMakeLists.txt:295–303`). `docs/plans/signet-passkeys-fido-2026-07-02.md:16,67–78,124–134`: Phase 4 "done" but Linux validation outstanding; attested identity, relay auto-sync, credential-ID scheme deferred.
- MCP **stub**: `tools/call` returns a fixed string (`main.go:27–37`); `signetclient` has an injected `Relay` interface with no concrete relay and a `MgmtClient` with no signing/wrapping/reply handling (`signetclient/client.go:17–128`, `contextvm.go:9–45`); Go module depends on `git.sharegap.net/cascadia/cascadia-go v0.2.1`; not built in CI. `docs/investigations/signet-mcp-server-2026-08-19.md:29–33,42–43,148–158` recommends a native C stdio sidecar over authenticated unix D-Bus for agent ops, gated ContextVM for admin ops, HTTP deferred.

### 3.7 Reuse and blocking seams

Reuse: SQLCipher verification, envelope encryption, backup/restore, policy enforcement, pairing transactions, deny-list, credential-access path, existing CMake + Meson suites, the existing weak cache-allocation seam for failure tests, the Groundhog release-workflow pattern.

Blocking seams: (1) HTTP bind bug; (2) silent config fallback and cache-only production operation; (3) relay AUTH/OK slot and callback-lifetime defects; (4) D2 write-back semantics vs a read-only config mount and the mark-before-write adoption bug (§3.3); (5) no published image route; (6) docs presenting partial/stub surfaces as complete; (7) RAM-only replay windows (120 s skew) crossing every restart against a 49-hour resubscribe lookback (`signetd_main.c:53`, `mgmt_protocol.c:641–656`).

## 4. Resolved launch decisions

| Decision | Specification | Consequence |
|---|---|---|
| Hosting | One `ubuntu-24-04-x64` Droplet, initially `s-1vcpu-2gb`, Docker Engine + Compose plugin, pinned | Linux/amd64 is the only supported target for this release |
| Network layout | `network_mode: host` for signetd (and optional Caddy) | Health genuinely binds host loopback and is scraped on-box; accepted loss of network-namespace isolation, compensated by strict bind + Cloud Firewall |
| Signing/management | Outbound TLS relay WebSockets; gift-wrapped (1059) management is the supported client path | No inbound signer or management port; direct 25910 ingress is an audit item |
| Enrollment | Operator-assisted `signetctl provision`; proof via the in-repo nostr-homed live test `homed_broker_login_nip46_live` (`NH_NIP46_LIVE=1`, `NH_NIP46_LIVE_CONFIG` with bunker URI + fresh client key) and a scripted reference client in `signet/tests/integration/` | No claim of automatic fleet enrollment; OpenClaw/cascadia consumers are discounted for rc.1 |
| Relay-delivered handoff | Keep implementation; advertise only after a real receiving consumer completes the round trip | A successful send is not delivery |
| HTTP bootstrap | Audited, packaged behind an optional Caddy TLS profile, **disabled at first launch** | No production token/session workflow is claimed |
| Passkeys | Optional `passkeys` image variant; programmatic WebAuthn acceptance only; runtime off unless configured and verified | Single-instance fleet PSK supplied as a mounted file; no cross-instance sync at launch |
| Virtual CTAP-HID | Experimental Linux-host profile (`compose.ctap.yml`), not a Droplet launch requirement | A UHID device on a Droplet does not present an authenticator to a browser elsewhere |
| MCP | **Dropped from this release** (owner). Not built, shipped, audited or listed as a surface; README/assessment record it as a stub | No Go CI job in rc.1; a native sidecar (per the 2026-08-19 investigation) is a separate future feature |
| Writer epochs | Fresh database, one daemon; no historical cutover | Fence behavior stays in audit scope; migrating an existing fleet uses `WRITER_EPOCH_CUTOVER.md` separately |
| Registry | GHCR public (owner-approved 2026-11-07), digest-pinned deployment; anonymous pulls | DOCR documented as alternative (Starter: 1 repo/500 MiB; manual GC) |
| TLS proxy | Caddy for the optional enrollment profile | nginx is an alternative, not a second maintained route |
| Provisioner authority | **Keep D2** (config is desired state, write-back converges INI and store; recorded decision fp-56t). `/etc/signet` is mounted **read-write** for UID 10001; production never sets `SIGNET_PROVISIONER_PUBKEYS` (env shadows the file and defeats write-back); fix adoption to mark only after a successful write-back | Runtime grants/revocations survive reloads because write-back succeeds; the persisted-only alternative (export W1.E) is rejected as it reverses a recorded design and silently ignores config edits (§7.E) |
| IPv6 | Droplet IPv6 **disabled** at first launch (no `--enable-ipv6`); listener tests cover `[::1]` bind correctness only | Firewall rules are IPv4; off-host v6 negative tests are out of scope until IPv6 is enabled |
| Droplet backups | `--enable-backups` **not** used; root disk is rebuildable from the release bundle + escrow and must hold no state | Avoids placing `/opt/signet/secrets` into provider-held backups in the same account as the Spaces backup objects |
| Supervision | systemd `signet-compose.service` is the only supervisor; Compose `restart: "no"` | Preflight (volume UUID, secrets, digest) runs on every boot and restart |
| Backup mode | **Online** daily backup from the `signet-ops` container, gated on a consistency test (§10.E); any deliberate daemon stop (upgrade, state restore) waits ≥270 s before restart | Removes a daily restart and the replay window it reopens |
| Version / cut | Full change set ⇒ `0.8.0` (MINOR: additive config/file-key interfaces + 0.x breaking behavior); tag **`signet-v0.8.0-rc.1`** | Manifest Declared version 0.8.0; release columns record the prerelease only once published |
| Independent review gate | Design/review agent produces `docs/reviews/signet-rc1-independent-review-<date>.md` after W2; owner sign-off precedes the rc tag | Closes `nostrc-1r5d.3` for signet scope; blockers 6 and 8 depend on it |

## 5. Execution index

| ID | Goal | Done when | Key files | Depends on | Size |
|---|---|---|---|---|---|
| W0 (`nostrc-mk7v1.1`) | Establish owned work and evidence baseline | Bead claimed; hooks installed; revision recorded; tracker exports refreshed; this plan extended in place | `docs/plans/signet-production-deployment-2026-11-06.md`, beads | — | S |
| W1 (`nostrc-mk7v1.2`) | Fail-closed startup, correct listeners, file-backed secrets, truthful CLI | Real entrypoint tests show each specified refusal and no unintended listener; §7 acceptance per item | `signet_config.c/.h`, `signetd_main.c`, `health_server.c`, `bootstrap_server.c`, new `http_listen.c`, `secret_input.c`, `config_reload.c`, `key_store.c`, `signetctl_main.c` | W0 | L |
| W2 (`nostrc-mk7v1.3`; owning beads `nostrc-izf9`, `-8str`, `-hj1v`, `-86xt`) | Relay correctness fixes + full security review | Owning beads (`nostrc-izf9`, `-8str`, `-hj1v`, `-86xt`) fixed with tests; `docs/reviews/signet-production-security-review-2026-11-06.md` with one bead per finding and blocker disposition; protocol-smells review run | `relay_pool.c/.h`, `nip46_server.c`, `signetctl_main.c`, `libnostr/src/connection.c`, `THREAT_MODEL.md` | W1 (for audit targets) | XL |
| W3 (`nostrc-mk7v1.4`) | Two image variants, Compose baseline, release workflow | Clean-image build of both variants; digest-pinned fresh-host `compose up` succeeds; `signet-release.yml` publishes with provenance | `signet/Dockerfile`, `.dockerignore`, `signet/docker-compose.yml`, `signet/deploy/*`, `.github/workflows/signet-release.yml`, `signet-ci.yml` | W1 (config path, bind), W2.A | L |
| W4 (`nostrc-mk7v1.5`; fulfils `nostrc-1cx`) | Droplet provisioning + operations runbook | Disposable Droplet: volume gating, key ceremony, loopback exposure, real client enrollment, restart durability, backup/restore/upgrade/rollback demonstrated | `signet/deploy/digitalocean/*`, `signet/deploy/{preflight,backup,release}.sh`, `signet/deploy/systemd/*`, `signet/deploy/README.md` | W3 | L |
| W5 (`nostrc-mk7v1.6`) | Reconciled docs, assessment table, tracker classification, version bump to 0.8.0 | No unsupported production promise remains; every deployment-relevant tracker item has a disposition; all four version sources + CMake say 0.8.0 | `signet/README.md`, `INTEGRATION.md`, `THREAT_MODEL.md`, examples, `VERSION_MANIFEST.md`, `docs/TESTING.md`, `signet/meson.build`, `signetd_main.c`, `signetctl_main.c`, `CMakeLists.txt` | W1–W4 outcomes | M |
| W6 (`nostrc-mk7v1.7`; closes `nostrc-1r5d.3` for signet) | Independent review + `signet-v0.8.0-rc.1` tag/publish | Review artifact under `docs/reviews/` with no open critical/high; owner sign-off; tag/manifest/source agree; `signet-release.yml` publishes digests + provenance | `docs/reviews/signet-rc1-independent-review-<date>.md`, `VERSION_MANIFEST.md` | W1–W5 | M |

**Soak (optional, before rc.1):** a pinned-HEAD soak on the disposable Droplet after W1–W3 may run to observe idle CPU (`libnostr-idle-writable-busy-poll-20260817`) and relay behavior; it does not substitute for any W6 gate.

## 6. W0 — Baseline

1. `bd onboard && bd prime`; `bd update nostrc-mk7v1.1 --claim` (epic `nostrc-mk7v1`); `scripts/install-hooks.sh`.
2. Record the exact source revision and the `doctl`, Docker, Compose versions used.
3. Inspect owners before editing: `signet/include/signet/relay_pool.h` (callback typedefs), `tests/selection/inventory.json` generator (header/owning doc), `scripts/ci/configure-release.sh`.
4. Refresh tracker exports (`bd list --status=open|in_progress|blocked --limit 0 --json --readonly`; `nostrig task list --json --timeout 30s`) and reconcile §12 against them; record unavailable synchronization explicitly rather than inventing it.

## 7. W1 — Runtime safety and interface changes

### 7.A Strict configuration and required persistence

Keep `int signet_config_load(const char *path, SignetConfig *out_cfg)`; change behavior:

- `path == NULL`/empty: defaults + environment (unchanged).
- Explicit readable valid INI: defaults → file → environment.
- Explicit missing, unreadable or malformed path: **return failure**; never continue with defaults. Errors name the path and failure class, never file contents. Apply to daemon and all `signetctl` callers.

Production Compose always supplies `-c /etc/signet/signet.conf`, `SIGNET_REQUIRE_ENCRYPTED_DB=true`, a nonempty DB path and a mounted master-key file. When encrypted operation is required, `signetd_main.c` must: (1) resolve the master key; (2) reject empty key or DB path **before** starting relays or HTTP; (3) require key-store creation with a non-null persistent store; (4) require existing SQLCipher verification (`store.c:704–723`) to pass; (5) on failure exit nonzero without exposing any service. Preserve explicit development cache-only mode when encryption is not required; do not remove `SignetKeyStore`'s ephemeral mode globally.

Tests (new `signet/tests/test_daemon_startup.sh`, executing the real binary): explicit missing/invalid config; empty key; unavailable key file; wrong key; plain-SQLite build with encrypted mode required; unwritable state directory. Each asserts nonzero exit and no HTTP listener (`ss -lnt`).

### 7.B File-backed master and backup keys

Add `SIGNET_DB_KEY_FILE` and `SIGNET_BACKUP_KEY_FILE`; keep value-bearing `SIGNET_DB_KEY`/`SIGNET_BACKUP_KEY` for compatibility but remove them from production examples. New private utility `signet/src/secret_input.c/.h` (no global state): resolves env value **or** protected file into a caller-owned `sodium_malloc` buffer released with `sodium_free()` on every path. Contract: **nonempty** value and nonempty file path both set → fail as ambiguous (an empty `SIGNET_DB_KEY=` from Compose interpolation plus a file is fine); neither → report absent (caller decides); open once `O_CLOEXEC|O_NOFOLLOW` and inspect the descriptor; require regular file owned by effective UID with no group/world bits; max 4 KiB; strip one trailing LF (optionally CRLF) and nothing else; reject embedded NUL, embedded line endings, empty result; never log values; do not duplicate the store's master-material validation or derivation. Use it for master/backup resolution in the daemon and every local CLI path including `restore-existing --sec-from-store` (`signetctl_main.c:806–853`). Keep bunker/provisioner key decoding separate but apply the same protected-file checks when consolidating. **Compatibility:** no stored-format change; a file containing the same legacy master string opens the same database.

### 7.C Correct HTTP address binding

New private parser `signet/src/http_listen.c/.h` shared by both HTTP servers: output `sockaddr_storage` + length + family; accept numeric IPv4 `a.b.c.d:port` and bracketed IPv6 `[::1]:port`; reject hostnames, malformed brackets, empty host/port, non-decimal ports, out-of-range ports; port 0 allowed at the server API for ephemeral-port tests (daemon config 0 still means disabled); no DNS, no wildcard fallback. Both servers retain the parsed address for their lifetime and pass it via `MHD_OPTION_SOCK_ADDR` (plus `MHD_USE_IPv6` when needed). Preserve `signet_health_server_start()`/`signet_bootstrap_server_start()` `→ int` and idempotent start/stop; add synchronous bound-port accessors (0 when not running) reading the real MHD socket. Daemon addresses: health `127.0.0.1:<health_port>`; bootstrap changes from `0.0.0.0` to `127.0.0.1:<bootstrap_port>`. Enabled-listener construction or startup failure becomes **fatal** with one orderly unwind path; `health_server_stub.c` must report unavailable (nonzero) when a nonzero health port is requested. Snapshot synchronization unchanged (`hs->mu`).

Shutdown/unwind order for both servers: **stop the MHD listener before freeing any borrowed dependency** (key store, store, challenge store, fleet registry, audit logger) — on the startup-failure unwind and on normal shutdown.

Acceptance (new `signet/tests/test_http_listen.c`): real socket tests prove loopback binding; connections to the host's non-loopback address fail; explicit wildcard binds only when requested; `[::1]` proves correct binding or an explicit capability failure, never an IPv4 fallback; no public-health configuration is introduced. Release note: because the default `health_port` is 8080 (`signet_config.c:230–231`), a CMake build without libmicrohttpd (`health_server_stub.c`) now fails to start unless `health_port = 0` is set.

### 7.D Bootstrap TLS trust

Move TLS policy into `SignetConfig`/`SignetBootstrapServerConfig` instead of reading env in the constructor:

| Setting | Type / default | Contract |
|---|---|---|
| `[bootstrap] require_tls` | bool, `true` | existing `SIGNET_BOOTSTRAP_REQUIRE_TLS` override retained |
| `[bootstrap] trusted_proxy_cidrs` | owned list; default loopback v4+v6 only | new CSV env `SIGNET_BOOTSTRAP_TRUSTED_PROXY_CIDRS`; invalid CIDR fails validation |
| server trusted masks | immutable, server-owned parsed masks | — |

Acceptance sequence: (1) obtain TCP peer from MHD; (2) peer must match a trusted proxy; (3) exactly one `X-Forwarded-Proto` whose trimmed value is `https` (case-insensitive); (4) reject duplicate/comma-joined/missing/other; (5) never trust an untrusted peer's forwarding headers; (6) reject before body accumulation where MHD permits; (7) error text must not instruct callers to spoof. Caddy **overwrites** the header. Keep the 8 KiB body cap and `no-store`. Restart-only: extend `config_reload.c` so SIGHUP never partially applies these. Under host networking the loopback trusted-proxy default separates **on-box from off-box only**: any local process can reach `127.0.0.1:9487` with a forged header — record this in `THREAT_MODEL.md` as an accepted limitation of the single-host layout. The profile stays disabled until a named session-token consumer exists and the HTTP audit findings (challenge/resource bounds, bearer semantics, pubkey-equality-not-possession) are resolved.

### 7.E Provisioner authority (keep D2; fix adoption; make write-back a deployment invariant)

Observed behavior is in §3.3. The export proposed persisted-only authority; that is **rejected**: it reverses the recorded D2 decision (fp-56t, `config_reload.h:24–38`) without owner direction, and would make a config-file "revoke" a silent no-op. The code-aligned design keeps D2 and removes the conditions under which its guarantees fail:

- `/etc/signet` is a **read-write** directory owned by `10001:10001` (`0750`), so `signet_config_write_provisioners()` can replace the INI (temp + rename). Secrets stay elsewhere (§10.A). `preflight.sh` verifies writability.
- Production never sets `SIGNET_PROVISIONER_PUBKEYS`; the seed lives in `[nostr] provisioner_pubkeys` in the INI (remove it from `docker-compose.yml:28`). Config validation **warns** when the env override is present together with a config file in encrypted-production mode.
- Fix `signet_config_reload_adopt_persisted()` (`config_reload.c:232–291`): mark the adoption only after a successful write-back (or no-op equality), so a failed write is retried on the next start; a write-back failure at startup or on SIGHUP logs critical, increments a `/metrics` counter, and sets a `provisioner_writeback_failed` flag visible in `/ready` detail (readiness stays true — authorization is still correct — but the drift is observable).
- Keep runtime `config/grant-provisioner` / `config/revoke-provisioner` committing to the store before visibility (`mgmt_protocol.c:790`, `signetd_main.c:922` remain). Keep `signet_mgmt_handler_set_config_path()`.
- README:258 and `signet.conf.example` document D2 accurately: config is desired state; runtime changes are written back; edit the config list to change authority **or** use `signetctl grant/revoke-provisioner` — never both without reloading.
- Extend `signet/tests/test_config_reload.c` with: (a) runtime grant → SIGHUP with the pre-grant config → grant persists (write-back succeeded); (b) runtime revoke of seed key → later unrelated config edit (mtime bump) → SIGHUP → key stays revoked because write-back removed it; (c) read-only config dir → write-back failure is reported and the adoption marker is **not** set; (d) env override present → warning and documented env-is-older semantics.

Acceptance: (a)–(d) plus restart and intentionally empty persisted set, verified through **signed management requests**, not store helpers alone.

### 7.F Cache/store mutation consistency

`key_store.c` prepares replacement cache storage before durable mutation in `signet_key_store_restore_agent()`, but provision/adopt/rotate allocate afterward. Extend the restore pattern to all three: (1) generate/validate material; (2) prepare cache entry + insertion key before durable mutation; (3) hold the key-store mutex across the durable mutation — custody callbacks must **not re-enter the key store** (lock order: key-store mutex → SQLite mutex/transaction); (4) perform the durable mutation; (5) swap only on success; (6) wipe/dispose on failure, preserving prior authority and output buffers. For persistent construction, fail the whole key store if listing, decrypting, validating or preparing any required agent key fails — no silently partial fleet. Preserve custody callbacks and lock order; do not replace transaction-bound signing with cache signing. Acceptance: allocation-failure tests via the existing seam (extend `signet/tests/test_key_store.c`) prove failed provision/adopt/rotate leaves durable identities and cache unchanged; corrupt startup records cause visible failure.

### 7.G CLI secret handling and exit codes

- `adopt-existing --sec -` becomes the only supported secret input; reject inline values before any network activity (matches `restore-existing`'s stdin restriction). Wire `agent/adopt-existing` unchanged.
- Processing order: argument validation → config validation → secret loading → operation, with one cleanup path.
- Wipe adopted/restored keys, encoded payloads, request plaintext and secret-bearing replies on all exits.
- Local introspection failures return nonzero (broken audit chain, failed `list-agents --verify`).
- Validate agent secret is exactly 32 bytes before deriving the pubkey.
- Correct help text that frames reissue as routine restart recovery; change the `adopt-existing my-agent --sec nsec1...` usage example (`signetctl_main.c:136`) to `--sec -`; treat bunker URIs as secrets.

Acceptance: extend `signet/tests/test_signetctl_credentials.sh` to invoke the real CLI and assert exit codes, redaction, file mode 0600, refusal of inline secrets and no network activity on invalid arguments.

## 8. W2 — Relay correctness and security audit

### 8.A Relay corrections (land under owning beads)

**One OK dispatcher per relay** (`nostrc-izf9`): registry keyed by event ID scoped to a connection generation; pending record = purpose (AUTH|PUBLISH), callback/context, destructor, terminal status; register before send; route `OK true/false` by event ID without displacing another watcher; remove a terminal watch exactly once; a late OK from a previous generation cannot authenticate the current connection; teardown terminates outstanding operations with a distinguishable transport-aborted result. Keep publish signatures; local return = enqueue; callbacks = relay accept/reject; neither = recipient delivery.

**Deferred callback lifetime** (`nostrc-8str`): pool owns pending GLib source IDs and contexts; contexts hold lifetime refs + generation token; stop marks stopping, cancels sources, detaches callbacks, stops/joins workers, then frees; destroy notifications wipe copied secrets; never wait on a worker while holding a mutex its callback needs.

**Write-result ownership** (`nostrc-hj1v`): validate `nostr_relay_write()`'s real ownership contract in libnostr before changing; match its string/answer-channel lifecycle exactly.

**Subscription readiness**: readiness requires a connected relay whose *current required* subscription has completed catch-up (not "any subscription has EOSE", `relay_pool.c:1262–1281`); old/closed/pre-AUTH subscriptions do not count; reconnect resets readiness; AUTH success reissues the stored complete filter **including recipient (`#p` = bunker pubkey) and time scope (`since = now − 49 h`, `signetd_main.c:53,65–79,412–428`)**; `CLOSED` records its reason and clears readiness; never use a settle delay.

**NIP-46 reply publication contract** (`nip46_server.c:921–950` currently passes no ACK callback): log the per-relay ACK result without secrets; add a `/metrics` counter for reply accept/reject/timeout; on rejection or no ACK, republish only the **already-signed** reply event and only to other connected relays; never re-sign or re-execute the request; `/ready` is independent of reply ACKs.

**Replay window across restarts** (`mgmt_protocol.c:472,641–656,1854–1889`; `nip46_server.c:490–494`): both caches are RAM-only with a 120 s skew; every restart re-reads up to 49 h of relay history. A request processed in the ~120 s before a stop and redelivered after a restart shorter than ~120 s executes again. Deployment rule (§10.E/G): any deliberate stop keeps the daemon down ≥270 s (2× the 120 s skew plus margin, so future-dated requests inside the skew are also covered; raised from 150 s during W3 review) before restart, recorded in the threat model; the audit (§8.C) decides whether a durable replay marker is required beyond that rule.

**`signetctl` lifecycle**: replace connect/settle polling with a GLib-main-context state machine `CONNECTING → REPLY_SUBSCRIBING → REQUEST_PENDING → COMPLETE`, terminals: connection/subscription failure, relay rejection, valid correlated error reply, deadline, cancellation. Publish after the reply subscription's catch-up boundary and keep it open until terminal. A deadline bounds the operation, not backfill. Never auto re-sign a timed-out mutation. Validate `signet/include/signet/relay_pool.h` typedefs first; add only the current-subscription completion notification.

**TLS** (`nostrc-86xt`): decide which hardening settings are still wanted (TLS1.2 min, AEAD ciphers, groups — a fixed groups list would drop OpenSSL 3.5's X25519MLKEM768 default) and apply them where they take effect (`lws_context_creation_info` client options or handling the reason before the conn guard); add a ClientHello fixture test like `tests/test_connection_tls_no_resumption.c`; verify hostname/CA verification is active in the deployed image. libnostr changes get their own version assessment.

Run `nostr-protocol-smells` (or the `nostr-protocol-review` workflow) before merging any of the above.

### 8.B Audit authority and artifact

Create `docs/reviews/signet-production-security-review-2026-11-06.md` in the house review format (dated title, bead IDs, boundary/verdict, ranked findings, verification, version decision, merge recommendation). Expand `signet/docs/THREAT_MODEL.md` from store-centric scope to the selected deployed and optional surfaces, preserving its key hierarchy and adversary exclusions (live root/same-UID compromise stays excluded; dump protection stays warn-only unless changed). Add adversaries: malicious/compromised relay; authorized malicious client; unauthorized network sender; compromised delivery-grant requester; proxy-header spoofing and listener exposure; container/image/config downgrade and secret-handling mistakes; restart replay boundaries and lost-reply behavior; optional passkey trust boundaries (PSK file custody, headless UV). Distinguish host custody from at-rest encryption.

Each finding records: stable ID + bead; source revision and file/function; attack prerequisites and affected profiles; reproducer/test and observed result; severity and C/I/A impact; remediation or explicit disabled-surface disposition; verification evidence and independent-review state; launch-blocker classification. One independently actionable finding = one bead; existing beads are linked, not duplicated.

### 8.C Audit matrix

| Focus | Method and adversarial checks | Artifact |
|---|---|---|
| Build/supply chain | Confirm SQLCipher/passkeys/test-hook flags in the built image; `ldd`; image config; provenance; embedded secrets; `.dockerignore` coverage | Image finding records + runtime capability manifest |
| Listener exposure | Real bind tests; `ss -lntp` for v4/v6; off-host attempts to 8080/9487 | Socket evidence + exposure matrix |
| Bootstrap | Header spoofing, duplicates, wrong JSON types, empty/oversized/chunked bodies, token races, attempt/expiry, stale handoff, challenge bounds, shutdown ordering | Route tests (`test_http_routes.c`) + token/auth state findings |
| NIP-46 | Outer signature boundary, malformed plaintext, IDs/timestamps, pairing-before-policy, deny precedence, identity pinning, concurrency, writer-fence enforcement, reply publication outcome | Tests through real handlers + launch relay |
| Management | Direct vs wrapped ingress; sender auth; grant shape; self-service; malformed RPC IDs; plaintext ingress; replay across restart/rewrap; reply redaction | Confidentiality/replay findings + corrected guarantees |
| Relay pool | AUTH/publish overlap, rejection, reconnect, `CLOSED`, stale EOSE, cancellation, shutdown with queued idle work, TLS identity verification | Protocol tests + protocol-smells result |
| Storage/keys | Missing/wrong key, plaintext downgrade, corrupt record, SQLCipher verification, partial cache load, mutation allocation failure | Fault-injection tests + custody findings |
| Backup/migration | Wrong master/backup key, symlinks/aliases, sidecars (WAL/SHM), interrupted ops, `.plaintext-backup` leftovers, config/policy completeness | Restore drill + durability findings |
| Logs/CLI/process | Inline secrets, URI output, audit redaction call sites, exit statuses, swap/core claims vs `/proc/<pid>/status`, capabilities, file ownership | Process inspection + redaction tests |
| Passkeys | Build/runtime gating, authz, PSK export/import boundary, headless UP/UV, credential ownership, UHID isolation | Programmatic interop evidence; separate Linux HID evidence |
| Provisioner/reload | Stale config after revoke, initial empty policy, live grants, restart, persisted authority | Real management round-trip tests |

Must not be papered over: RAM-only replay caches do not give durable exactly-once; gift-wrap-ID dedup may not prevent rewrapping an authenticated inner request; a new event ID can repeat a logical mutation; a lost reply does not prove a failed mutation; bootstrap pubkey equality is not possession; `sodium_malloc` + warn-only `mlockall` do not justify "never swapped"; an audit hash chain without an independently retained checkpoint does not resist whole-history replacement.

### 8.D Severity and sequencing

| Severity | Treatment |
|---|---|
| Critical | Stop deployment of the affected profile; fix + independent verification first |
| High | Block that profile until fixed or the surface is genuinely removed from deployment |
| Medium | Record mitigation, residual impact, owner acceptance; block only if it prevents the specified operational outcome |
| Low | Track separately |

Disabling a vulnerable optional surface is a valid disposition; relabelling it "experimental" while leaving it enabled is not. Bootstrap/HTTP findings cap at **Medium** for rc.1 because the profile ships disabled with no consumer (owner decision) — unless a finding is exploitable while disabled (e.g. the listener still starts), in which case normal severity applies. Remediation designs for new findings belong in their beads.

## 9. W3 — Images, Compose, release packaging

### 9.A Image variants

| Variant | SQLCipher | Passkeys | Test hooks |
|---|---|---|---|
| default | ON | OFF | OFF |
| `passkeys` | ON | ON | OFF |

Both contain `signetd`, `signetctl`, CA roots, required shared libs, in-container health probe. Exclude `signet-mcp` and `signet-git-credential`.

Dockerfile changes: pin Debian builder/runtime digests and tool versions in `signet/deploy/versions.env` (release fails on placeholders/floating refs); add `libcbor` build/runtime deps only for `passkeys` (build arg `SIGNET_PASSKEYS=ON|OFF`); set all three security build options explicitly; keep the SQLite + SQLCipher Meson checks in a `test` stage the release workflow executes (enable passkey Meson tests for the variant); `ldd` both executables; **remove `setcap`** (conflicts with `cap_drop: ALL`/`no-new-privileges`); fixed UID/GID `10001:10001`; exec-form entrypoint; default config path `/etc/signet/signet.conf`; OCI labels (source, revision, version, licence, created); no secret-bearing env defaults; record package inputs and digests rather than claiming byte-reproducibility. Root `.dockerignore`: `.git`, local secret/config dirs, build trees, databases/backups, deployment runtime files — keep source, build metadata, test fixtures.

### 9.B Compose baseline (`signet/docker-compose.yml`)

`signetd` service: required digest-pinned `SIGNET_IMAGE` sourced from the per-release `/opt/signet/current/release.env` (no production `build:`; site settings live separately in `/opt/signet/config/deploy.env`); `network_mode: host`; no `ports:`; `user: "10001:10001"`; `cap_drop: [ALL]`, no `cap_add`; `no-new-privileges`; `read_only: true`; bounded `/tmp` tmpfs; memlock soft/hard unlimited; `ulimits.core: 0`; limits 512 MiB / 1 CPU; log rotation 3×10 MiB; **`restart: "no"`** (systemd is the only supervisor, §10.B — Docker's own restart policy would bypass preflight at boot); `stop_grace_period: 30s`; healthcheck `curl -sf http://127.0.0.1:${SIGNET_HEALTH_PORT}/health` with the port pinned in the production INI (the image `HEALTHCHECK` hardcodes 8080, `Dockerfile:121–122`); relay outage must not recreate a healthy process; readiness/operator checks on `/ready`. Verify on the Droplet that unlimited `RLIMIT_MEMLOCK` suffices without `CAP_IPC_LOCK` rather than keeping both.

| Host source | Container destination | Mode | Owner |
|---|---|---|---|
| `/opt/signet/config` | `/etc/signet` | **rw** directory (D2 write-back, §7.E) | `10001:10001` `0750` |
| `/srv/signet/state` | `/data` | rw | `10001:10001` `0700` |
| `/opt/signet/secrets/db.key` | `/run/secrets/signet_db_key` | ro file | `10001:10001` `0400` |
| `/opt/signet/secrets/bunker.nsec` | `/run/secrets/signet_bunker_nsec` | ro file | `10001:10001` `0400` |
| `/opt/signet/secrets/passkey-sync.key` (optional) | `/run/secrets/signet_passkey_sync_key` | ro file | `10001:10001` `0400` |
| `/opt/signet/secrets/backup.key` (ops only) | `/run/secrets/signet_backup_key` | ro file | `10001:10001` `0400` |
| `/srv/signet/backups` (ops only) | `/backups` | rw | `10001:10001` `0700` |

Bind mounts with host-path creation disabled (`create_host_path: false`); a missing source must not create empty state. State in `/data`: `signet.db` (+ WAL/SHM), `policies.conf`, and **required** `audit.jsonl` (`[audit] path=/data/audit.jsonl`; stdout audit would be silently discarded by log rotation; included in the backup bundle with a stated retention). Policies are mutable state because management persists them. Environment holds only secret **paths**, public identifiers, relay URLs, non-secret settings, and `SIGNET_REQUIRE_ENCRYPTED_DB=true`; it never holds `SIGNET_PROVISIONER_PUBKEYS` (§7.E).

### 9.C Optional Caddy profile (`signet/deploy/compose.bootstrap.yml`, `signet/deploy/Caddyfile`)

Profile off by default; Caddy alone accepts inbound 443; signet bootstrap stays `127.0.0.1:9487`; proxy only `/bootstrap`, `/challenge`, `/auth` — never `/health`, `/ready`, `/metrics`; overwrite `X-Forwarded-Proto: https`; body/header read deadlines and the 8 KiB limit; `admin off` (under host networking the default `localhost:2019` admin API would let any local process rewrite the proxy); disable the automatic port-80 redirect listener (`auto_https disable_redirects`) or list port 80 in the exposure matrix; certificate state in separately owned persistent dirs under `/srv/signet/caddy`; pinned official Caddy image, read-only root, `cap_drop: ALL` + `NET_BIND_SERVICE`, no Docker socket; state explicitly if Caddy runs as root for port 443. TLS-ALPN-01 on 443; if 443 is source-restricted, DNS-01 with a narrowly scoped DNS credential. Never open public enrollment solely to issue a certificate. **Caddy vs nginx:** Caddy avoids a separate renewal subsystem for one upstream; nginx (native ACME module) only if the operator already maintains it.

### 9.D Registry and CI

GHCR public (owner-approved 2026-11-07: repo is public, image carries no secrets; fits the Actions release route and avoids DOCR Starter's 1-repo/500 MiB limit); CI publication uses repository-scoped `GITHUB_TOKEN` `packages: write` permissions; Droplet pulls are anonymous and digest-pinned, with no package credential required; DOCR documented as alternative (`doctl registry login --read-only --expiry-seconds`, manual GC read-only window).

New `.github/workflows/signet-release.yml` (pattern: `groundhog-release.yml`): (1) trigger on `signet-v*` tags and manual candidate builds; (2) verify tag/base version across all four authoritative sources; (3) run `signet-ci.yml` + Docker test stage + Meson; (4) build both variants; (5) emit SBOM, vulnerability report, revision, provenance, digests; (6) publish only after checks pass; (7) manual HEAD images carry a commit identity, never a version; (8) maintain a vulnerability-exception list with finding refs and expiry. Extend `signet-ci.yml` with affected packaging checks (no Go job: the SDK/MCP is out of scope). Update `tests/selection/inventory.json` publisher/package metadata via its supported generator (identify it in W0).

## 10. W4 — DigitalOcean provisioning and operations

### 10.A Host layout and deployment manifest

| Path | Owner / purpose |
|---|---|
| `/opt/signet/releases/<commit>/` | root; immutable deployment bundle (compose files, scripts, `manifest.json`, `release.env` with image digests) |
| `/opt/signet/current` | root; symlink to selected release — switching it switches `release.env` and therefore the digests |
| `/opt/signet/config/` | `10001:10001` `0750`; non-secret INI (D2 write-back target) + `deploy.env` site settings (+ optional Caddyfile) |
| `/opt/signet/secrets/` | root `0700`; files `10001:10001` `0400` (`db.key`, `bunker.nsec`, `backup.key`, optional `passkey-sync.key`), mounted individually; registry pull credential and Spaces key (root-only, `0400`, single-bucket write-only policy — no delete — so a host compromise cannot erase backups) |
| `/srv/signet/` | required DO volume mountpoint |
| `/srv/signet/state/` | `10001:10001` `0700`; durable daemon state |
| `/srv/signet/backups/` | protected backup staging |
| `/srv/signet/caddy/` | optional TLS state |
| `/run/lock/signet-maintenance.lock` | serializes maintenance operations |

Non-secret deployment manifest (`/opt/signet/releases/<commit>/manifest.json`): manifest schema version; source commit + component version; default/passkeys image digests; Caddy digest; Docker/Compose package selections; expected volume ID + filesystem UUID; enabled profiles; config revision/hash; public bunker/provisioner identifiers; **store schema version before/after each upgrade** (drives §10.G image-rollback gating); last successful backup reference. Never secret values. `preflight.sh` and `release.sh` assert that the running image digest equals the manifest digest. Droplet backups are not used (§4) — the root disk holds no state and is rebuilt from the bundle plus escrow; Signet backups (§10.E) are mandatory.

### 10.B Cloud-init and doctl

`signet/deploy/digitalocean/cloud-init.yaml` responsibilities: operator SSH key, disable password/root login; switch apt sources to HTTPS (DO Ubuntu images default to an HTTP mirror — verify; otherwise allow outbound TCP 80 to the mirror); install pinned Docker/Compose and `do-agent`; create directories per §10.A; mount the identified volume via `fs_setup`/`mounts` on `/dev/disk/by-id/…` (format only a verified blank device); fail visibly if storage is absent or has the wrong identity; install `signet-compose.service` (`RequiresMountsFor=/srv/signet`, `ExecStartPre=preflight.sh`, `ExecStart=docker compose up`, `Restart=on-failure` — the **only** supervisor) and `signet-backup.timer`; **never** receive keys, registry, Spaces or DNS credentials in user-data; do not start Signet before custody material is installed and validated.

`signet/deploy/digitalocean/create-droplet.sh` validates inputs and runs (flags verified 2026-11-06; re-check against installed `doctl`):

```bash
# 1. firewall first (tag-scoped), so cloud-init runs with rules attached
doctl compute firewall create --name signet-fw --tag-names signet \
  --inbound-rules "protocol:tcp,ports:22,address:$OPERATOR_CIDR" \
  --outbound-rules "protocol:tcp,ports:443,address:0.0.0.0/0 protocol:tcp,ports:80,address:0.0.0.0/0 protocol:udp,ports:53,address:0.0.0.0/0 protocol:tcp,ports:53,address:0.0.0.0/0 protocol:udp,ports:123,address:0.0.0.0/0"
# 2. volume, 3. droplet (no --enable-backups, no --enable-ipv6 — see §4)
doctl compute volume create signet-data --region "$DO_REGION" --size 20GiB --fs-type ext4
doctl compute droplet create "$SIGNET_HOST" --image ubuntu-24-04-x64 --size s-1vcpu-2gb \
  --region "$DO_REGION" --ssh-keys "$DO_SSH_KEY_ID" --vpc-uuid "$DO_VPC_ID" \
  --volumes "$DO_VOLUME_ID" --tag-names signet --enable-monitoring \
  --user-data-file signet/deploy/digitalocean/cloud-init.yaml
```

Firewall: inbound 22 from operator CIDR only; no baseline application port; optional 443 only when the enrollment profile is enabled; no inbound 8080/9487/47472; outbound 443 (relays, registry, Spaces), 80 only if apt stays on HTTP (drop after switching to HTTPS mirrors), DNS over UDP+TCP 53, NTP 123; if a configured relay uses a non-443 port, add it explicitly. IPv4 only until IPv6 is enabled (§4). Reserved IP only if a stable inbound TLS address is needed (not required for outbound-only NIP-46). Monitoring: `doctl monitoring alert create` for CPU/memory/disk; resource alerts do not prove signer health — `/ready` does.

### 10.C First-boot key ceremony (operator-present; relates to nostrig `fp-6`)

1. Generate master key (≥32 bytes, hex) and bunker nsec with trusted local tooling. 2. Keep the provisioner nsec on the operator workstation; never mount it into signetd. 3. Record and independently verify bunker and provisioner pubkeys. 4. Transfer master/bunker files over SSH to temporary protected paths. 5. Install under `/opt/signet/secrets` as `10001:10001` `0400`. 6. Escrow recovery copies of master and bunker keys off-host under operator custody. 7. Generate an independent backup key; escrow separately from backup objects. 8. Install explicit relay URLs (e.g. `wss://nos.lol`, `wss://relay.nostr.band`; never `relay.damus.io`), the initial provisioner seed, empty default-deny policy state, DB path. 9. Record expected volume identity and digests in the manifest. 10. `systemctl start signet-compose.service` (runs `preflight.sh`, `docker compose pull`, `up -d`). No automatic key generation when a file is absent. After the first seed, grants/revocations go through signed management requests only.

### 10.D Enrollment flow (launch path)

1. Operator `signetctl -c operator.conf` with relay URLs, bunker **public** key, protected provisioner key file (`SIGNET_PROVISIONER_NSEC_FILE`). 2. `signetctl provision launch-agent`. 3. Treat the returned bunker URI as a secret. 4. Write a one-time login bundle for the in-repo nostr-homed live test (`NH_NIP46_LIVE=1`, `NH_NIP46_LIVE_CONFIG` → bunker URI + fresh client key; see bd memory `c7-closed-live-nip-46-external-signer-login`) and the reference client config under `signet/tests/integration/`. 5. Where a client needs a connect-secret file: `signetctl reissue-connect launch-agent --out /secure/launch-agent-connect-secret` (`umask 077`); never via argv or logs. 6. Each client connects with its own persistent keypair. 7. Verify pairing, a policy-permitted signature (`allow_kinds` incl. the login kind), a denied operation, autonomous reconnect after daemon and client restarts — with the reference client, which is scripted for restart cases. 8. Verify revocation with the previously authorized client (`signetctl list-clients`, `revoke-client`). `/auth` sessions are not part of this path. For single-writer identities use `agent/writer-acquire` per `WRITER_EPOCH_CUTOVER.md` and demonstrate owner success + other-client denial; a fresh-store test is not historical cutover proof.

### 10.E Backup (online daily SQLCipher export, consistency-gated)

`signet-ops` Compose service in profile `ops`: same digest; `signetctl` entrypoint; `user: "10001:10001"`; mounts `/data` rw, `/etc/signet` ro, `/run/secrets/signet_db_key` ro, `/run/secrets/signet_backup_key` ro, `/backups` rw (`/srv/signet/backups`); **no** bunker/provisioner key; `network_mode: none`; no restart policy or healthcheck. **All local store commands** (`backup-db`, `restore-db`, `verify-audit`, `list-agents --verify`) run through this service as UID 10001 — host-root `signetctl` would fail §7.B's ownership check. This also resolves nostrig `signet-file-backed-admin-config-20260830` for on-box admin: file-backed `SIGNET_DB_KEY_FILE` plus a durable bunker **pubkey** in `/etc/signet/signet.conf`, never the private key.

**Mode:** `signet_store_backup()` (`store.c:1095–1164`) checkpoints the WAL, ATTACHes the target and runs `sqlcipher_export` on an open connection; only `restore-db` requires the daemon stopped (`signetctl_main.c:116–122`; `store.c:1317–1343` takes an exclusive lock). An **online** backup from `signet-ops` (a second process, own connection, WAL snapshot isolation) therefore avoids a daily restart and the replay window it reopens (§8.A). Gate: a new consistency test (`signet/tests/test_backup_restore.c` extension) runs a concurrent writer (provision/rotate/credential loop) against a daemon-held store while `backup-db` executes from a second connection, then restores and verifies row counts, audit-chain integrity and agent-key/public-key consistency; `PRAGMA wal_checkpoint(TRUNCATE)` returning `SQLITE_BUSY` is tolerated (its result is already ignored, `store.c:1144–1146`). If the test cannot be made to pass, fall back to offline backup with the ≥270 s stop rule.

`signet/deploy/backup.sh` (via `signet-backup.timer`): (1) acquire `/run/lock/signet-maintenance.lock`; (2) record release/config identity; (3) `docker compose --profile ops run --rm signet-ops backup-db` to a unique staging path under `/backups`; (4) require success + encrypted-file verification; (5) archive non-secret config (`signet.conf`, `deploy.env`), `policies.conf`, `manifest.json`, `audit.jsonl`; (6) upload backup + bundle to Spaces (`aws s3 --endpoint-url https://<region>.digitaloceanspaces.com` or `s3cmd`, write-only key; lifecycle expiry 30 days); (7) record checksum/reference and result in the manifest; (8) release lock. Never upload master/backup/bunker/registry/DNS secrets with backups. Targets: **RPO 24 h, RTO 60 min**, confirmed by the restore drill, not asserted. On failure: preserve the previous backup, alert; an upload failure is not an off-host backup. Any deliberate daemon stop elsewhere (upgrade, state restore) waits **≥270 s** before restart (§8.A replay rule).

### 10.F Restore and lost-host recovery

1. Provision a clean host + new/approved volume (§10.B). 2. Recover the pinned release bundle, master key and backup key from separate custody. 3. Keep Signet stopped. 4. Via `signet-ops`: `signetctl restore-db` with the master active at backup time. 5. Restore the policy/config bundle. 6. Restore the bunker identity separately. 7. Via `signet-ops`: `signetctl verify-audit` and `list-agents --verify`. 8. Start the pinned image (≥270 s after any prior stop on the same store). 9. Verify bunker identity, existing client reconnect, signing, revocation state. Reuse `.pre-restore`; automation moves prior recovery artifacts into a named protected directory before a deliberate subsequent restore. A DO volume snapshot is a recovery aid, not an application-consistent backup; never copy a live DB ignoring WAL.

### 10.G Upgrade and rollback (`signet/deploy/release.sh`)

Upgrade: install bundle under `releases/<commit>` → pull + inspect candidate by digest (from its `release.env`) → preflight + config compatibility → lock → stop → pre-upgrade backup + bundle; record store schema version in the manifest → switch `current` symlink → wait ≥270 s since stop → start → check `/health`, `/ready`, a management call, an existing client → record post-upgrade schema version → unlock. Never run two daemon versions against one store. **Image rollback** (switch `current` back) only when the manifest shows the store schema version unchanged by the upgrade (the store migrates on open, `store.c:~385–392`); `release.sh` refuses otherwise. **State rollback**: stop candidate, preserve its DB/config separately, restore pre-upgrade backup + bundle, start previous image — loses intervening grants/revocations/leases/bindings; requires operator confirmation and a post-rollback authorization review; never automatic on a brief readiness failure.

### 10.H Required failure behavior

| Failure | Required result |
|---|---|
| Missing master/bunker file | preflight and daemon fail; no key generation |
| Missing explicit config | nonzero exit; no defaults-only start |
| Volume missing | preflight fails before Compose; no root-disk state dir created |
| Wrong volume | UUID mismatch fails even if mountpoint exists (enforced on every boot because systemd runs preflight; Compose has no restart policy) |
| Provisioner write-back fails | critical log + metric + `/ready` detail flag; authorization unchanged; preflight refuses a non-writable config dir |
| Restart < 270 s after stop | `release.sh`/`restore`/`preflight.sh` refuse; manual `docker compose up` bypass documented as a replay risk (SR-02, `nostrc-17ggo`) |
| Wrong DB key | store refuses; no second database created |
| SQLCipher unavailable | required encrypted mode refuses startup |
| Relay AUTH rejected | reason logged without secrets; readiness false; bounded reconnect, no busy-spin |
| No relay connected | readiness false; health degraded, not liveness-failed |
| Enabled HTTP listener fails | daemon exits nonzero and unwinds |
| Registry unavailable | running digest continues; failed pull replaces nothing |
| Disk full | previous backup preserved; store write failures surfaced; no false success |
| Stop > 30 s | forced termination recorded; integrity/readiness checks on restart |
| `/dev/uhid` absent | CTAP profile preflight fails; baseline unaffected |
| Passkeys enabled on default image | config/startup fails; never pretend active |

## 11. W5 — Documentation and implemented-vs-promised assessment

### 11.A Assessment table (promised → observed → status → launch action)

| Promise (doc) | Observed (code) | Status | Launch action |
|---|---|---|---|
| Persistent NIP-46 pairing (`README.md:9–34`) | durable bindings + identity pinning (`nip46_server.c:554–619`, `test_client_bindings.c:625–680`) | Implemented | Verify restart/revocation in production |
| Signing via hot cache (README architecture `:86–116`) | persistent custody uses writer transactions | Implemented; description stale | Fix diagrams/comments |
| SQLCipher persistence (`README.md:146–148,480`) | optional build; verified when store opens (`store.c:704–723`) | Implemented, conditional | Require in image and runtime |
| Deployment keys required (`README.md:182`) | empty `SIGNET_DB_KEY` → cache-only (`key_store.c:247–275`) | Partial guarantee | §7.A fail-closed |
| Health endpoint (`README.md:449–471`) | `/health` `/ready` `/metrics`; bind bug | Partial | §7.C fix; replace sample |
| Encrypted management (`README.md:53`) | gift-wrap confidential; direct 25910 readable (`mgmt_protocol.c:1212–1302`) | Conditional | Document boundary; gift-wrap is the production route |
| Management at-most-once (`README.md:53`, INTEGRATION) | RAM caches keyed by delivered event ID (`mgmt_protocol.c:641–675`) | Limited (process lifetime) | Document restart/retry/rewrap boundary; audit |
| Provisioner env "legacy one-time seed" (`README.md:258`) | seed on first boot **and** D2 reload reconciliation + write-back (`config_reload.c:124–203`); adoption marks before writing (`:237–291`) | Conflicting / partially broken under read-only config | §7.E keep D2, rw config dir, no env override, adoption fix; update docs + tests |
| NIP-17 bootstrap tokens (`README.md:55–61`) | provision delivers bunker URI; token lifecycle has no callers | Partial | Separate URI handoff from HTTP token workflow |
| HTTP auth session in enrollment (`INTEGRATION.md:531–542`) | routes issue leases; no deployed consumer | Partial/deferred | Remove from launch procedure |
| Keys "never swapped" (`README.md:80,479`) | `mlockall` warn-only (`signetd_main.c:600–639`) | Overstated | Match threat model; add core limit |
| Core dumps disabled (`README.md:81,482`) | `PR_SET_DUMPABLE` warn-only; no `RLIMIT_CORE` | Conditional | `ulimits.core: 0`; qualify claim |
| Keys never leave process (`README.md:84,478`) | true in normal operation; imports/backups/operator custody are distinct | Needs qualification | State precise guarantee |
| Every operation hash-chained (`README.md:73`) | DB chain for credential ops (`store.c:584–597`); generic logger not chained (README:74 admits) | Partially verified | Audit call sites; narrow wording |
| Test counts "23 Meson / 18 CMake" (`README.md:474`) | brittle | Stale-prone | Replace with commands |
| Passkey API (`INTEGRATION.md:48–95`) | policy-checked service + NIP-46 methods behind build/config/PSK | Implemented, gated | `passkeys` image + real interop |
| Virtual CTAP-HID (`signet-passkeys-fido-2026-07-02.md:16`) | Linux adapter, unsupported commands, no live-host proof | Partial/experimental | Defer production acceptance |
| Go MCP (`sdks/go/cmd/signet-mcp`) | `tools/call` fixed response (`main.go:27–37`); `signetclient` has no concrete relay | Stub | **Dropped from rc.1** (owner); README states it is not shipped; separate future feature |
| Writer fence (`README.md:13`, `WRITER_EPOCH_CUTOVER.md`) | durable enforcement; rollout not performed (`:95–108`) | Implemented; rollout separate | Fresh deploy ≠ cutover proof |
| Backup/restore (`THREAT_MODEL.md:225–237`) | independent envelope + master-bound payloads (`store.c:1095–1286`) | Implemented | Exercise container restore drill |
| Docker quick start (`README.md:184–189`, compose header) | wrong filename/mount; public health; no release image | Incomplete | Replace with §9/§10 procedure |
| Login grant (`GRANT_LOGIN_KIND.md:3–11`) | live proof passed 2026-09-21 (bd memory `c7-closed-…`) | Implemented; doc stale | Link evidence without claiming rerun |

### 11.B Documentation corrections

- `signet/README.md`: launch-supported vs optional/deferred surfaces; GKeyFile INI names consistently; correct `[dbus] unix_enabled/tcp_enabled/tcp_port` and `socket_path` (`:224–238`); bootstrap default `port = 0`; add all eight `SIGNET_PASSKEYS_*` vars and the new `*_FILE` key inputs to the env table (`:240–265`); D2 semantics at `:258` (config is desired state, write-back, mtime rule, env override caveat); `/health` vs `/ready`; replace test-count claims (`:474`) with commands; stdin for BYO keys; add `restore-existing`, `writer-acquire`, `set-policy`; qualify memory/audit/encryption/replay/custody claims to match `THREAT_MODEL.md`; link the assessment table.
- `signet/INTEGRATION.md`: explicit launch enrollment path; remove HTTP-session sequencing from it; separate reconnect from forced re-pairing; fix `dbus_unix/dbus_tcp/dbus_tcp_port` (`:33–36`); timeout ambiguity and no blind mutation retries; direct vs gift-wrapped confidentiality.
- Examples: `signet.conf.example:71` policy format is GKeyFile INI not JSON; bootstrap default; new canonical `signet/policies.conf.example` with `[identity.<name>]` groups, old `policies.toml.example` kept only as a clearly deprecated reference this release; `.env.example` holds public values and file paths only; document ownership/mounts.
- `signet/docs/THREAT_MODEL.md` stays the authority; README cannot exceed it without evidence.
- Historical docs: `GRANT_LOGIN_KIND.md` links the 2026-09-21 live-proof record; `WRITER_EPOCH_CUTOVER.md` distinguishes fresh deploy from unfinished historical rollout; `signet-docs.xml` drops "Signet 1.0".
- `docs/TESTING.md`: document the focused Signet packaging/live-test entry points; `VERSION_MANIFEST.md`: declared version per §15.2, release columns only after publication.

## 12. Outstanding beads and nostrig issues (snapshot 2026-11-06)

Classification is a deployment assessment, not a change to tracker priority. Refresh in W0: for each deployment-relevant item record source tracker, exact ID, status **date**, affected profile, dependency link and disposition; join by actual scope/dependencies (`bd show`, `nostrig task show`), not title keywords. Snapshot date for every row below: 2026-11-06.

### 12.1 Beads — deployment-relevant (of 51 outstanding title matches)

| ID | Title (short) | P | Status | Disposition |
|---|---|---|---|---|
| nostrc-4j37h | Fence Signet agent-key signing with per-identity writer epoch | P0 | in_progress | Fence correctness in audit; historical cutover blocks migrating an existing fleet, not a fresh DB |
| nostrc-ot2c | [C] NIP-46 protocol hardening epic | P0 | open | Track children; epic status is not proof |
| nostrc-ot2c.1 | Audit NIP-46 callers / freeze compatibility contract | P0 | in_progress | Candidate blocker until landed evidence reconciled |
| nostrc-ot2c.3 | Deadline/cancellation ownership, callback/GLib lifetimes | P0 | in_progress | Candidate blocker (overlaps §8.A lifetimes) |
| nostrc-ot2c.4 | Event-driven subscription-before-publish lifecycle | P0 | in_progress | Candidate blocker (overlaps §8.A readiness/CLI state machine) |
| nostrc-ot2c.5 | Signed response admission / active-request correlation | P0 | in_progress | Candidate blocker |
| nostrc-ot2c.6 | Pre-paired provider with wrapped secrets / broker verifier | P0 | open | Desktop provider integration — not a signer blocker |
| nostrc-ot2c.7 | Real signer pre-login approval / fault compatibility | P0 | open | Desktop login; live proof recorded 2026-09-21 — reconcile and close or rescope |
| nostrc-ot2c.2 | (closed) | — | closed | Reuse evidence for the chosen revision |
| nostrc-1r5d.3 | Independently review C NIP-46 core hardening | P0 | open | **Launch-review blocker** (independent review of W2) |
| nostrc-1r5d.2 | Independently review broker/proof/vault/PAM/desktop UX | P0 | open | Desktop; not a signer blocker |
| nostrc-1r5d.1/.4/.5 | (titles not in snapshot) | — | open | Resolve scope in W0 |
| nostrc-nzvi | Signer hardening Milestone A (SignEvent, GetRelays, fuzz) | P1 | in_progress | Link applicable requirements; avoid duplicate umbrella acceptance |
| nostrc-1cx | Signet SIG live relay verification ready | P2 | open | **Blocker** until selected image/relay/client round trip is demonstrated (§10.D) |
| nostrc-izf9 | AUTH + publish-ack share one OK slot | P2 | open | **Blocker** — §8.A fix + tests |
| nostrc-8str | AUTH idle callbacks retain raw pointers | P2 | open | **Blocker** — §8.A fix + tests |
| nostrc-hj1v | relay_pool strdup'd write + undrained answer channel leak | P3 | open | Fix or prove non-impact with real ownership evidence |
| nostrc-86xt | libnostr SSL_CTX hardening never runs | P3 | open | Audit dependency; **blocker** if peer verification is ineffective |
| nostrc-58bop | Plain `memset` secret wipes (nip55l/NIP-49) | P2 | open | Classify affected buffers; blocker only if a deployed Signet path is affected |
| nostrc-nfnw | 31 pre-existing ASan ctest failures | P3 | open | Isolate failures intersecting deployed Signet/libnostr paths; do not repair all |
| nostrc-rb0e (+16 children) | Samba/domain desktop lab epic | P0 | open/blocked | Not a deployment blocker absent a shared-code dependency |

Not deployment-related (desktop/Grotto/Groundhog/gnostr/test-infra; listed for completeness): nostrc-1gqwr, -3c7n, -k4p14.16, -mevv, -otxhq, -svsj, -u6v55 (+.31, .7.1), -z1fb, -28b7a, -299r0, -2ig3s, -4hv51, -9z5w, -ecrx, -gyogu, -klrq8, -qp24.10.17, -qp24.16, -qp24.3, -qp24.52, -qp24.61, -sq3j, -t6g7, -9ik4, -fdg3, -nbtu, -qgsn, -qp24.51, -sei9, -xdg9, -41wr, -iheg, -tfgp.

### 12.2 Nostrig (`fleet-planning`) — 38 outstanding signet matches

**Discount rule (owner, second round):** items whose acceptance or confirmation requires the local cascadia deployment stack are discounted and are **not** rc.1 gates. After the rule, the only nostrig items still in scope are `signet-sqlcipher-key-store-test-20261015` (review + deployed-image proof), `signet-file-backed-admin-config-20260830` (resolved by §10.E), `signet-zero-restart-full-config-20260911` (deferred) and `libnostr-idle-writable-busy-poll-20260817` (verifiable on the Droplet). Every other row below is retained for context only.

| ID | Title (short) | P | Status | Disposition |
|---|---|---|---|---|
| signet-sqlcipher-key-store-test-20261015 | Fix Signet SQLCipher key_store release test | P0 | in_progress | Fix `cb25ace7` merged, 31/31 SQLCipher pass; **independent review + deployed-image verification required** |
| signet-zero-restart-full-config-20260911 | Complete zero-restart full runtime configuration | P0 | open | Defer; documented restart-only settings acceptable |
| signet-file-backed-admin-config-20260830 | Durable file-backed signetctl admin config | P1 | open | **Blocker for on-box admin**; solved by §10.E ops service (public bunker key + file-backed keys) |
| fp-6 | Genesis bootstrap: interim-nsec → Signet | P0 | open | **Discounted** (cascadia stack); the operator-present key ceremony itself remains a plan step (§10.C) |
| bahia-openclaw-signet-enrollment-20260819 | Automate secure Signet/NIP-46 enrollment | P0 | blocked (sidecar lineage) | **Discounted**; enrollment proof uses in-repo clients (§10.D) |
| bahia-openclaw-complete-agent-provisioning / -readiness-ux / -rollout-conformance / -transaction-reconciliation-20260819 | OpenClaw provisioning program | P0 | blocked | Downstream consumer program; not signer blockers |
| bahia-identity-compromise-recovery-workflow-20261015 | Identity-compromise containment/recovery | P0 | in_progress | Informs §10.F/§8.B revocation evidence; not a signer code blocker |
| bahia-service-key-leak-rotation-20261010 | Rotate leaked Bahia service key | P0 | in_progress | Consumer-side; verify no Signet-custodied key involved |
| platform-zero-restart-runtime-config-20260823 | Platform zero-restart config | P0 | blocked | Defer |
| stage01-external-secret-delivery-canary-20260929 | Stage-01 secret delivery canary | P0 | blocked | Downstream; not a blocker |
| fp-cfg.8 | Env secret burn-down → mounted files + Signet identities | P1 | in_progress | §7.B addresses Signet's own inputs; broader burn-down separate |
| fp-cfg | Env-config anti-pattern remediation epic | P1 | blocked | Separate |
| libnostr-idle-writable-busy-poll-20260817 | Idle WebSocket writable busy-poll burns a core | P1 | in_progress | **Candidate operational blocker** on a 1-vCPU host; verify on Droplet, fix if present |
| hiveci-terminal-release-attestor-reclassify-or-build-20260918 | Build bridge/Signet attestor or remove gate | P1 | open | **Discounted**; rc.1 uses the repository provenance route (§9.D) |
| signet-… via bahia-alertmanager-signet-nip29-incidents-20260928, review-…, bahia-prometheus-nip98-scrape-bridge-20260928, review-…, bahia-managed-instance-watchdog-recovery-20260829, review-…, bahia-signed-environment-units-20260904, bahia-agent-runtimes-infrastructure-20260810, bahia-hermes-agent-runtime-adapter-20260906, cascadia-go-public-module-20260818, fleet-otel-observability-nostr-transport-20260914, fleet-p1-p2-infrastructure-execution-program-20260911, max-loom-worker-hive-ci-20260810, openclaw-nostr-agent-profile-tool-20260820 | Downstream consumers / fleet programs | P1 | various | Classify individually; not signer blockers because they mention Signet |
| groups-relay-croissant-secret-posture-20260922, interlocutor-nostr-io-20260826, interlocutor-nostr-mcp-shim-20260825 | Relay/operator tooling | P2 | various | Post-launch |
| bahia-nip46-publish-feedback-20260927, review-…, interlocutor-hardening-docs-20260826 | Admission feedback / docs | P3 | various | Pre-launch hardening (consumer side) / post-launch |
| fp-53 | Sovereign catalog signed via Signet | P4 | blocked | Post-launch |

Closed since 2026-08-08 (25) provide regression context, not permission to skip checks: incl. `fp-signet-secret-store-completion-20260803` (2026-09-29), `signet-contextvm-credential-delivery-20260929`, `signet-delivery-grant-deny-precedence-20260929`, `signet-sqlcipher-provisioner-revoke-regression-20260911`, `bahia-resilient-signet-startup-20260818`, `loom-worker-hive-ci-nsec-env-leak-20260918`.

### 12.3 Durable notes (`bd memories signet`)

`c7-closed-live-nip-46-external-signer-login` (live login proof passed 2026-09-21 on bunker.sharegap.net), `nip-46-live-homed-provider-nip46-wired-to` (provider config shape), `packaging-pre-phase-complete-merge-09b919cb-track-a`, `signet-passkeys-phase-0-feasibility-spike-is-go`, `signet-sqlcipher-migration`, `signetctl-contextvm-migration`.

### 12.4 Candidate release blockers (owner decides the cut against these)

| # | Candidate blocker | Required evidence / disposition | Work |
|---|---|---|---|
| 1 | Health/bootstrap bind to all interfaces | fixed `MHD_OPTION_SOCK_ADDR` + on-host `ss` + off-host failure | §7.C |
| 2 | Missing config/key yields unintended (ephemeral) operation | entrypoint refusal tests; no `REQUIRE_ENCRYPTED_DB` bypass | §7.A/B |
| 3 | Provisioner authority drift (D2 write-back impossible under read-only config / env override; adoption marks before writing) | rw config dir, env override removed, adoption fix, `test_config_reload.c` cases (a)–(d) through signed requests | §7.E |
| 4 | AUTH/publish OK collision; callback use-after-free | `nostrc-izf9`, `-8str` fixed with concurrent/teardown tests | §8.A |
| 5 | Ineffective outbound TLS hardening/verification | `nostrc-86xt` reviewed; negative TLS tests; verification confirmed in image | §8.A |
| 6 | Unresolved critical/high NIP-46 or management findings | fix + independent review (`nostrc-1r5d.3`) or remove the surface | §8.B–D |
| 6a | Six Medium findings (SR-01…SR-06) | SR-01 fixed (`4090fc20a`); SR-02…SR-06 accepted for rc.1 with conditions recorded in the security review "Owner dispositions (2026-11-07)" and on beads `nostrc-17ggo`, `-hbj7a`, `-d1g76`, `-nmefa`, `-k2d0c` | §8.D |
| 7 | No proven launch client enrollment/reconnect | `homed_broker_login_nip46_live` + `signet/tests/integration/` reference client against the rc image and launch relays (`nostrc-1cx`), not mocked handlers | §10.D |
| 8 | SQLCipher custody review / deployed-image proof | `signet-sqlcipher-key-store-test-20261015` independently reviewed; encrypted restart + restore verified | §8.C, §10.F |
| 9 | No trustworthy recovery procedure | successful fresh-host restore drill | §10.F |
| 10 | Volume absence creates fresh empty state | mount-identity preflight failure demonstrated | §10.B/H |
| 11 | Idle core saturation (`libnostr-idle-writable-busy-poll-20260817`) | observed on Droplet; fixed if present | §14 manual |
| 12 | Version/tag mismatch | tag equals implemented component version across all sources | §15.2 |
| 13 | Key ceremony not performed | operator-present ceremony evidence (§10.C; nostrig `fp-6` itself is discounted) | §10.C |
| 14 | Online backup consistency unproven | concurrent-writer backup/restore test passes, or offline mode with ≥270 s rule adopted | §10.E |
| 15 | Replay window across restart not bounded | ≥270 s stop rule in `release.sh`/runbook + audit decision on durable marker | §8.A, §8.C |

**Status at `signet-v0.8.0-rc.1` (2026-11-07):** see the addendum in `docs/reviews/signet-rc1-independent-review-2026-11-06.md` and the drill report `docs/reviews/signet-rc1-droplet-drill-2026-11-07.md`; all 15 rows are cleared or carry a recorded disposition. The production key ceremony (§10.C) remains the operator's launch step.

**Published 2026-11-07:** `signet-v0.8.0-rc.1` at `de2a40876` via `Signet release` run 42363870599 (all jobs green). Images (public GHCR, owner decision 2026-11-07): `ghcr.io/chebizarro/nostrc-signet@sha256:6aff3456f7926cab6ab61d3335ba77ea06b549377da36bc74fb8a235295fe1ef` (default, tag `0.8.0-rc.1`) and `ghcr.io/chebizarro/nostrc-signet@sha256:d3896a746fafd3ad734139d2800e23803195e84c685da3df4a42a54e03527732` (passkeys, tag `0.8.0-rc.1-passkeys`); SLSA provenance verified with `gh attestation verify`, SBOM attestation present. The image ships 29 unfixed bookworm HIGH/CRITICAL CVEs under documented exceptions expiring 2026-12-07 (`signet/deploy/vulnerability-exceptions.json`); remediation and the SR-02/SR-03 conditions gate promotion to `signet-v0.8.0`.

**Not baseline blockers:** replacing MCP, activating HTTP sessions, virtual CTAP-HID on the Droplet, universal zero-restart config, historical writer cutover for a fresh DB, unrelated desktop/sanitizer failures.

**Release cut (decided): `signet-v0.8.0-rc.1`.** Blockers 1–15 must each be cleared or carry an explicit disposition in the W6 review before the tag; the rc label communicates maturity but does not waive critical/high findings. Promotion to `signet-v0.8.0` follows a soak on the production Droplet with no new critical/high findings.

## 13. File-by-file impact

Paths marked *new* do not exist; inspect existing files before editing.

| File | Change | Depends on |
|---|---|---|
| `docs/plans/signet-production-deployment-2026-11-06.md` | extend in place with evidence links and final decision | all |
| `docs/reviews/signet-production-security-review-2026-11-06.md` *new* | audit artifact | W2 |
| `signet/src/signet_config.c`, `include/signet/signet_config.h` | strict explicit-file load; bootstrap TLS/proxy fields; protected key reads | W1 |
| `signet/src/config_reload.c`, `include/signet/config_reload.h` | restart-only TLS settings; adoption marks only after successful write-back; write-back failure flag/metric; header documents the rw-config and no-env invariants | §7.D/E |
| `signet/src/secret_input.c/.h` *new* | protected secret resolver | daemon/CLI |
| `signet/src/http_listen.c/.h` *new* | numeric socket-address parser | HTTP servers |
| `signet/src/health_server.c`, `health_server_stub.c`, `include/signet/health_server.h` | honor address; bound-port accessor; stub reports unavailable | parser |
| `signet/src/bootstrap_server.c`, `include/signet/bootstrap_server.h` | honor address; trusted-peer TLS; immutable policy; accessor | config/parser |
| `signet/src/signetd_main.c` | required-persistence checks; loopback bootstrap; fatal listener failures; stop-listener-before-free unwind; env-override warning; `/ready` write-back flag | W1 |
| `signet/src/key_store.c` | prepare-before-commit; fail partial load; fix stale comments | store APIs |
| `signet/src/signetctl_main.c` | file keys; stdin-only BYO; cleanup; exit codes; event-driven relay lifecycle | secret util, relay |
| `signet/src/relay_pool.c`, `include/signet/relay_pool.h` | OK dispatcher; generation-scoped readiness; safe lifetimes; minimal API addition | beads |
| `signet/src/nip46_server.c` | reply ACK logging/metric; signed-reply republish to other relays; never re-sign | relay ACK (§8.A) |
| `signet/src/mgmt_protocol.c` | changes only from audit findings (provisioner write-back path unchanged) | §8 |
| `libnostr/src/connection.c` | TLS hardening where it takes effect (`nostrc-86xt`) | separate version assessment |
| `signet/CMakeLists.txt`, `signet/meson.build` | register utilities/tests; CMake project VERSION; Meson installs config example; parity | W1 |
| `signet/Dockerfile`, `.dockerignore` *new* | §9.A | runtime fixes |
| `signet/docker-compose.yml` | §9.B baseline + `ops` profile; drop `SIGNET_PROVISIONER_PUBKEYS`, `ports:`, `cap_add`, `build:` fallback | docker |
| `signet/deploy/compose.bootstrap.yml`, `Caddyfile` *new* | §9.C | TLS fix |
| `signet/deploy/compose.ctap.yml` *new* | explicit `/dev/uhid` mapping + group; no privileged | passkeys image |
| `signet/deploy/versions.env` *new* | reviewed build/runtime pins | release |
| `signet/deploy/release.env` *new* (per bundle) | `SIGNET_IMAGE`, Caddy digest for that release | §9.B, §10.G |
| `signet/deploy/README.md` *new* | provisioning/custody/ops/recovery runbook | W3/W4 |
| `signet/deploy/digitalocean/cloud-init.yaml`, `create-droplet.sh` *new* | §10.B | pins |
| `signet/deploy/preflight.sh`, `backup.sh`, `release.sh` *new* | §10.B/E/G | layout |
| `signet/deploy/systemd/signet-compose.service`, `signet-backup.service`, `.timer` *new* | mount-gated start; daily backup | scripts |
| `.github/workflows/signet-ci.yml` | packaging checks | tests |
| `.github/workflows/signet-release.yml` *new* | §9.D | CI/versioning |
| `tests/selection/inventory.json` (+ generator) | publisher/package metadata | workflow names |
| `signet/tests/test_http_listen.c`, `test_http_routes.c`, `test_secret_input.c`, `test_daemon_startup.sh`, `test_deployment_smoke.sh`, `integration/test_production_roundtrip.sh`, `integration/nip46_reference_client.*` *new* | §14; reference client scripts pair/sign/reconnect/revoke cases | APIs |
| `docs/reviews/signet-rc1-independent-review-<date>.md` *new* | W6 independent review artifact | W2 |
| `signet/tests/test_key_store.c`, `test_config_reload.c`, `test_backup_restore.c`, `test_signetctl_credentials.sh`, `test_relay_pool_protocol.c`, `test_relay_pool_ok_lifetime.c` | extend owning tests (cache failure; D2 cases (a)–(d); concurrent-writer backup consistency; CLI; relay) | changes |
| `signet/tests/CMakeLists.txt`, `signet/tests/meson.build` | register tests with capabilities visible; keep parity (`check_test_parity.sh`) | tests |
| `signet/README.md`, `INTEGRATION.md`, `docs/THREAT_MODEL.md`, `signet.conf.example`, `policies.conf.example` *new*, `policies.toml.example`, `.env.example`, `docs/GRANT_LOGIN_KIND.md`, `docs/WRITER_EPOCH_CUTOVER.md`, `docs/signet-docs.xml` | §11 | final behavior |
| `VERSION_MANIFEST.md`, `docs/TESTING.md` | version decision; test entry points | §15.2 |

No baseline change to `store.c` schema, the Go MCP code, or the unsupported local transports; audit findings may add scoped changes.

## 14. Verification (planned, not executed)

```bash
# setup / affected selection (builds/tests stay outside the static-only push hook)
bd onboard && bd prime && scripts/install-hooks.sh
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"

# CMake production configuration
cmake -S . -B build-signet-prod -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIGNET_ENABLE=ON \
  -DBUILD_TESTING=ON -DSIGNET_USE_SQLCIPHER=ON -DSIGNET_ENABLE_PASSKEYS=ON -DSIGNET_ENABLE_TEST_HOOKS=OFF
cmake --build build-signet-prod && ctest --test-dir build-signet-prod/signet/tests --output-on-failure
# keep passkeys-off / plain-SQLite matrix entries as compatibility checks (a skipped SQLCipher test is not evidence)

# Meson compatibility
meson setup build-signet-meson-sqlite signet -Dnostrc_build_dir=../build-signet-prod -Dsignet_enable_tests=true
meson test -C build-signet-meson-sqlite --print-errorlogs
meson setup build-signet-meson-sqlcipher signet -Dnostrc_build_dir=../build-signet-prod \
  -Dsignet_use_sqlcipher=true -Dsignet_enable_passkeys=true -Dsignet_enable_tests=true
meson test -C build-signet-meson-sqlcipher --print-errorlogs

# images
docker build --target test --build-arg SIGNET_PASSKEYS=OFF -f signet/Dockerfile .
docker build --build-arg SIGNET_PASSKEYS=OFF -t signet:test -f signet/Dockerfile .
docker build --build-arg SIGNET_PASSKEYS=ON  -t signet:test-passkeys -f signet/Dockerfile .

# deployment (on the Droplet)
docker compose --env-file /opt/signet/config/deploy.env -f /opt/signet/current/signet/docker-compose.yml config --quiet
docker compose --env-file /opt/signet/config/deploy.env -f /opt/signet/current/signet/docker-compose.yml up -d
curl --fail http://127.0.0.1:8080/health && curl --fail http://127.0.0.1:8080/ready && curl --fail http://127.0.0.1:8080/metrics
sudo ss -lntp
```

Manual host checks: from another host, fail to reach 8080/9487 over IPv4 (and IPv6 once enabled, §4); `/proc/<pid>/status` (Uid, Cap*, NoNewPrivs, VmLck); `/proc/<pid>/limits` (memlock, core); `docker inspect` shows no master/provisioner secret in env; observe idle CPU long enough to distinguish transient activity from the reported busy loop; restart container and host, confirm client pairing survives; unmount the throwaway volume and confirm preflight refuses without creating a replacement DB.

| Mandatory behavioral test | Passing condition |
|---|---|
| HTTP address | socket bound to the requested numeric address |
| TLS proxy | trusted peer + single `https` passes; spoofed/untrusted/duplicate fail |
| Startup | config/key/store/listener failures exit nonzero before operation |
| Provisioner authority | D2 cases (a)–(d) of §7.E: runtime grant survives SIGHUP; runtime revoke survives a later config edit; read-only dir reports failure without marking adoption; env override warns |
| Backup consistency | concurrent-writer online backup restores to a consistent, verified store (§10.E gate) |
| Cache failure | no durable mutation followed by failed cache preparation |
| Relay OK overlap | AUTH and publish each receive their own outcome |
| Relay teardown | queued AUTH/resubscribe and late OK cannot touch freed objects |
| Readiness | only current required subscriptions satisfy readiness |
| CLI | inline secrets rejected; audit/key verification failure returns nonzero |
| Enrollment | `homed_broker_login_nip46_live` passes (connect→ack, sign_event, broker verify ok, tampered→invalid_proof) and the reference client pairs, signs, reconnects after daemon/client restart, is denied after revocation |
| Backup/restore | fresh-container restore retains identity, bindings, policies, credentials, audit verification |
| Wrong recovery keys | restore refuses without replacing the original |
| Passkeys | external verifier (python-fido2 per `tests/phase0/README.md`) validates artifacts from the `passkeys` image |
| CTAP (optional) | Linux `/dev/uhid` + libfido2 passes; otherwise profile stays unsupported |

Live tests must fail visibly when prerequisites (relay, Linux, verifier) are absent — never convert a missing capability into success.

## 15. Risks, compatibility, version

### 15.1 Behavioral changes requiring release notes

Explicit invalid config paths fail; encrypted production mode cannot fall into ephemeral operation; bootstrap binds loopback; untrusted/ambiguous forwarding headers no longer satisfy TLS; inline BYO CLI secrets rejected; provisioner adoption no longer marks before a successful write-back and warns on an env override; a build without libmicrohttpd fails to start unless `health_port = 0`; local verification failures return nonzero. No DB schema or key-derivation change; existing stores and legacy master strings stay readable.

### 15.2 Version decision

Per `AGENTS.md` component versioning, the additive file-key/config interfaces plus 0.x breaking behavior make the full change set a **MINOR bump to 0.8.0**, updated in one commit across `VERSION_MANIFEST.md`, `signet/meson.build`, `SIGNET_VERSION`, `SIGNETCTL_VERSION`, and a newly authoritative CMake project version. Do not update "Latest release"/"Release tag" before publication. **Decided:** the release is the prerelease `signet-v0.8.0-rc.1`; authoritative sources and Declared version carry `0.8.0`, release columns record the full prerelease version once the tag is published (`AGENTS.md` prerelease rule). libnostr changes (`nostrc-86xt`, busy-poll) get an independent assessment against their manifest row.

### 15.3 Operational risks

Host networking is a deliberate simplicity tradeoff (strict bind + firewall mandatory); a public bootstrap endpoint adds an incompletely integrated workflow (keep disabled); backups do not replace key escrow; `.plaintext-backup` migration artifacts must be inventoried and protected; state rollback can restore revoked authority; a fixed digest does not guarantee relay availability or dependency freshness; tracker/DO facts are dated snapshots.

## 16. Implementation order

1. **W0** baseline (bead, hooks, revision, owners, tracker refresh).
2. **W1.A/B/G** strict config + file keys + CLI (atomic: interfaces, callers, tests, version sources).
3. **W1.C/D** HTTP binding + trusted proxy (real/stub parity, restart-only reload, unwind, socket/route tests); bootstrap stays disabled.
4. **W1.E/F** D2 adoption fix + env-override guard + cache consistency — precedes the Compose config layout (rw config dir, no env provisioner list).
5. **W2.A** relay fixes under owning beads; CLI state machine; run `nostr-protocol-smells`.
6. **W3** build and qualify both image variants, `.dockerignore`, pins, Compose baseline + `ops`/`bootstrap`/`ctap` profiles, image tests.
7. **W2.B–D** security review of every selected surface; one bead per finding; severity disposition.
8. **W4** disposable Droplet: volume gating, key ceremony, loopback exposure, real enrollment, restart durability, relay AUTH failure, policy/revocation.
9. **W4.E–G** backup, restore, upgrade, rollback drills with throwaway state and escrowed test keys; record digests/commands/outcomes.
10. **W5** docs reconciliation, assessment table finalization, tracker dispositions.
11. **W6** independent review (design/review agent, house format) of W1–W5 outcomes with every §12.4 blocker dispositioned; owner sign-off.
12. **Publish `signet-v0.8.0-rc.1`**: verify manifest/source/tag agreement (0.8.0), run `signet-release.yml`, publish digests and provenance, update release columns for the published prerelease only.

## 17. Open questions

All material questions were resolved by the owner on 2026-11-06 (§1.1 second-round rows): enrollment proof client (nostr-homed live test + reference client), D2 kept, independent review by a separate review agent before rc.1, no `/auth` consumer, single-instance PSK, external attestation discounted, cut = `signet-v0.8.0-rc.1`, MCP dropped.

Remaining (non-blocking, resolve during W0/W4): DO region and the `OPERATOR_CIDR` for the firewall; soak duration and promotion criteria from rc.1 to `signet-v0.8.0` (proposed: 14 days with no new critical/high findings and one successful scheduled backup + restore drill).

## 18. References

- Signet docs: `signet/README.md`, `signet/INTEGRATION.md`, `signet/docs/THREAT_MODEL.md`, `signet/docs/WRITER_EPOCH_CUTOVER.md`, `signet/docs/GRANT_LOGIN_KIND.md`, `signet/docs/plans/fp-signet-secret-store-completion.md`, `signet/tests/phase0/README.md`
- Prior plans/reviews: `docs/plans/signet-passkeys-fido-2026-07-02.md`, `docs/reviews/signet-passkeys-fido-feasibility-critique-2026-07-02.md`, `docs/investigations/signet-mcp-server-2026-08-19.md`, `docs/designs/packaging-plan-debian-fedora.md`, `docs/plans/versioning-release-tagging-2026-08-10.md`, `packaging/lab-blossom/docker-compose.yml`, `.github/workflows/groundhog-release.yml`
- Repo rules: `AGENTS.md` (scope, beads, testing, versioning, banned relays, Nostr behavior, landing), `VERSION_MANIFEST.md`, `docs/TESTING.md`, `signet/include/signet/config_reload.h` (D1/D2)
- Plan critique applied: `docs/reviews/signet-production-deployment-plan-critique-2026-11-06.md`
- DigitalOcean (checked 2026-11-06): https://docs.digitalocean.com/reference/doctl/reference/compute/droplet/create/ · https://docs.digitalocean.com/reference/doctl/reference/compute/volume/create/ · https://docs.digitalocean.com/reference/doctl/reference/compute/firewall/create/ · https://docs.digitalocean.com/products/volumes/details/features/ · https://docs.digitalocean.com/products/backups/details/features/ · https://docs.digitalocean.com/products/container-registry/details/pricing/ · https://docs.digitalocean.com/products/spaces/how-to/configure-lifecycle-rules/ · https://docs.digitalocean.com/products/app-platform/details/limits/ · https://docs.digitalocean.com/reference/doctl/reference/monitoring/alert/create/
- Containers/TLS: https://caddyserver.com/docs/automatic-https · https://nginx.org/en/docs/http/ngx_http_acme_module.html · https://docs.docker.com/reference/compose-file/services/#devices · https://docs.docker.com/compose/how-tos/use-secrets/ · https://docs.kernel.org/hid/uhid.html · https://github.com/warmcat/libwebsockets/blob/main/test-apps/test-client.c
