# Signet rc.1 independent review (W6) — 2026-11-06

**Beads:** `nostrc-mk7v1.7` (W6), epic `nostrc-mk7v1`. This review discharges `nostrc-1r5d.3` for signet scope. New findings: `nostrc-kc35u` (W6-01), `nostrc-b82i4` (W6-02), `nostrc-mu8zv` (W6-03), `nostrc-bo0if` (W6-04). Comments added to existing beads: `nostrc-6dx4o` (W6-05 extends SR-09) and `nostrc-2h11s` (I-2). Linked: `nostrc-95xkd`, `nostrc-z3b0s`, `nostrc-17ggo`, `nostrc-hbj7a`, `nostrc-d1g76`, `nostrc-nmefa`, `nostrc-k2d0c`, `nostrc-1cx`, `nostrc-86xt`, `nostrc-a7p6i`.

**Branch:** `signet/rc1-w6-independent-review` (local, not pushed)

**Reviewed revision:** `master` at `d57f32e9f`. This includes W1 runtime safety, W2.A relay fixes, W3/W4 packaging and deploy tooling, W5 docs/0.8.0, the integration verification with libnostr 2.1.3 (`f313f355c`), and the W2.B–D security review.

**Reviewer independence:** I wrote none of this code or the earlier review documents. Every claim below comes from my own reads of the tree at `d57f32e9f` and my own test runs. Where I agree with an earlier document, it is because I checked the claim, not because the document made it.

**Authority:** [`signet/docs/THREAT_MODEL.md`](../../signet/docs/THREAT_MODEL.md); severity per [plan §8.D](../plans/signet-production-deployment-2026-11-06.md#8d-severity-and-sequencing); blocker list per plan §12.4.

## Boundary and verdict

**Scope:**
- (1) NIP-46 core hardening and custody across `nip46_server.c`, `mgmt_protocol.c`, `relay_pool.c`, `key_store.c`, `store.c` and `replay_cache.c`, plus the daemon wiring in `signetd_main.c`.
- (2) Every §12.4 candidate blocker.
- (3) The six open Medium findings from the security review.
- (4) Version sources and tag rules.
- (5) Release-route and other gaps that none of the plan, the security review or the plan critique raised.

**Out of scope:** Go MCP (dropped), D-Bus, NIP-5L, ssh-agent. I did not edit any code, build files or docs other than this file.

**Evidence boundary:** host-local only, on macOS arm64 (Apple Clang; Homebrew SQLCipher 3.53.3, libcbor 0.14.0, OpenSSL 3.x). Blocker 5's in-image TLS check is in progress on a sibling branch, so I did not build the image. I did not use a Droplet or a container, and I contacted no live public relay. I made read-only GitHub API calls to resolve upstream action tags.

**Verdict:**
- **NIP-46 core and custody (`nostrc-1r5d.3`): no Critical or High.** The merged code enforces pairing-before-policy, deny precedence, the writer fence, bounded replay, generation-scoped OK/AUTH and encrypted custody, as the plan and security review describe (§Question 1). `nostrc-1r5d.3` can be closed for signet scope.
- **There is one new High, on the release route (W6-01, `nostrc-kc35u`), and it blocks the tag.** `signet-release.yml` pins `aquasecurity/trivy-action@0.33.1`. That un-prefixed tag was deleted upstream after the CVE-2026-33634 tag hijack, so the publishing workflow cannot run as written. Re-pinning it to any mutable tag would leave third-party code able to swap the image before it is pushed and attested. The fix is small.
- **Four new Lows** (W6-02 to W6-05), none of them blockers.
- **I concur with all six Mediums as Medium, and none is a tag blocker** (§Question 3).

## Findings (ranked)

Legend: **R** = exercised at runtime by me; **S** = static source evidence read by me.

### W6-01 — High (I), release route — `signet-release.yml` pins a deleted, hijacked trivy-action tag, and third-party actions run with write access to the image they publish · `nostrc-kc35u` · **tag blocker**

- **Where:** `.github/workflows/signet-release.yml:89,96` (`aquasecurity/trivy-action@0.33.1`) and `:80` (`anchore/sbom-action@v0`), in the `package` job.
- **Upstream facts (checked 2026-11-06 via `gh api`):**
  - `repos/aquasecurity/trivy-action/git/ref/tags/0.33.1` returns **404**.
  - `tags/v0.33.1` → `b6643a29…`.
  - `tags/v0.35.0` = `tags/0.35.0` → `57a97c7e7821a5776cebc9bb87c984fa69cba8f1`.
  - Upstream deleted the un-prefixed tags after the 2026-03-19 hijack (CVE-2026-33634: trivy-action 0.0.1–0.34.2 force-pushed to credential-stealing commits; only `v`-prefixed tags were restored to the legitimate commits).
  - 0.33.1 is also inside GHSA-9p44-j4g5-cfx5 (script injection, `>= 0.31.0, <= 0.34.0`, patched `>= 0.34.1`).
- **Effect today:** GitHub cannot resolve the action, so the `package` job fails at setup. Then `publish` and `bundle` never run. Pushing `signet-v0.8.0-rc.1` would leave a tag with no published image. The `workflow_dispatch` `publish_candidate` route is blocked the same way, and that is the route the owner would use to get a commit-addressed image for the Droplet steps.
- **Latent effect (the reason this is High rather than a CI nit):**
  - In the `package` job, `evidence/image.tar`, `image.tar.sha256` and `image.json` are written at `:74–79`.
  - The SBOM and trivy actions then run in the same job, with filesystem access to `evidence/`.
  - `publish` (`:153–171`) checks only that those files agree with each other (`sha256sum --check`, then `docker load`, then an image-ID compare against `image.json`). It then pushes and creates a build-provenance attestation.
  - So a compromised or re-pointed third-party action can replace the published image under a valid attestation. This is the integrity seam the §8.C "Build/supply chain" row is meant to close. The security review's SR-20 (`nostrc-95xkd`, Low) noted tag refs in general, but missed both this hijacked pin and the evidence-rewrite path.
- **History:** the workflow has never run on GitHub (`gh run list` reports it is not on the default branch), so no past secret exposure needs rotating.
- **Remediation (before tagging):**
  - Pin every third-party action to a reviewed full commit SHA. For trivy-action that means v0.35.0 or later, verified against the upstream advisory.
  - Seal the image identity before any third-party code runs. For example: emit the digest or ID as a step output straight from `build-image.sh`'s metadata, before the scanners run, and have `publish` compare against that output. Alternatively, run the scanners in a separate job on a read-only copy of the artifact.
  - Version impact: CI-only change, no component bump.

### W6-02 — Low (I, operator docs) — `≥150 s` stop rule still taught where deployment enforces 270 s · `nostrc-b82i4`

- **Where:** `signet/README.md:206,435,571`; `signet/INTEGRATION.md:197,609`; `signet/docs/WRITER_EPOCH_CUTOVER.md:102`; plan §10.H row "Restart < 150 s after stop".
- **Context:** SR-22 corrected only THREAT_MODEL. The deployment side is already at 270 s: `ops.py:156` (`wait_rule`, 270 s), `signet-compose.service:12–17` (`ExecStartPre=preflight.sh`, `RestartSec=275s`) and `signet/deploy/README.md:4`.
- **Why 150 s is wrong, not merely conservative:** a request accepted with `created_at` = stop + 120 s (the maximum future skew) is 30 s old at a restart 150 s after the stop. It is therefore still inside the 120 s window, and the RAM cache is empty, so it executes again.
- **Exposure:** supported start paths enforce 270 s, so only an operator following the README by hand is exposed.
- **Recommendation:** fix in the pre-tag commit. Docs only, no bump. Not a blocker.

### W6-03 — Low (A), baseline — unauthenticated senders get bunker-signed replies and audit rows with no global budget · `nostrc-mu8zv`

- **NIP-46:**
  - Any validly signed kind-24133 event `#p`-tagged to the bunker, from any key with no binding, gets the full reply treatment. That is a NIP-44 decrypt and encrypt, a signed reply publish and a flushed `audit.jsonl` row (`nip46_server.c:910–964`, `signet_audit_nip46` `:111–195`).
  - No per-sender or global budget applies on this path. The management refusal audit does have one (`mgmt_protocol.c:752–791`).
  - The rows are small: the parser caps `id` at 256 and `method` at 128 bytes (`nips/nip46/src/core/nip46_msg.c:7–10`). So this is CPU, relay-traffic and disk-growth amplification bounded by the relays' own rate limits, not a practical disk fill.
- **Management:**
  - A non-provisioner's direct kind-25910 request gets a gift-wrapped `not_provisioner` reply (`mgmt_protocol.c:1259–1268`). That contradicts the "silently drop … do not confirm the bunker exists" design at `:1681–1687`, which the gift-wrap path relies on.
  - Requester buckets are per sender (`:701–745`), so fresh keys bypass them.
- **Remediation:** add global budgets with suppressed-row counting, mirroring `signet_mgmt_refusal_audit`.
- **Severity:** Low. It does not block rc.1.

### W6-04 — Low (release hygiene) — `check-release.py` ignores `signet/CMakeLists.txt` · `nostrc-bo0if`

- `VERSION_MANIFEST.md:55` lists `signet/CMakeLists.txt` (`project VERSION`) as authoritative, but `signet/deploy/check-release.py:15–30` compares only Meson, `SIGNET_VERSION`, `SIGNETCTL_VERSION` and the manifest row.
- All five agree today (§Question 4), so rc.1 is unaffected. A future drift would pass the release gate.

### W6-05 — Low (I) — `connect` spends the secret and binds before the replay and policy verdicts · comment on `nostrc-6dx4o` (extends SR-09)

- **Where:** `nip46_server.c:573–579` runs the consume-and-bind. The replay result is computed at `:494–496` but enforced only at `:709–720`, and the policy is evaluated at `:681–739`.
- **Behavior:** a connect with a valid unconsumed secret that is TOO_OLD or IN_FUTURE, or whose method the policy denies, still spends the secret and persists the binding while the client receives an error. SR-09 identified this only for the deny-list case.
- **Why it stays Low:** custody is still gated, because `allow` requires `rr == OK`, a binding, no deny entry and a policy allow. The connect events involved are signed by the secret holder, and NIP-46 kind 24133 is ephemeral.
- **Fix:** together with SR-09, consume-and-bind only after the replay, deny and policy checks pass.

### Informational (no bead)

- **I-1. The `since` advance takes `created_at` from any signed sender.** `relay_pool.c:141–150` records the maximum `created_at` of any validly signed inbound event. `signet_relay_pool_update_since_from_latest` (`:1698–1719`) turns that into `filter_since`. A future-dated event from anyone would therefore push `since` into the future. Today the 49 h floor overwrites the value immediately (`signetd_main.c:423–430` → `relay_pool.c:1048`), and the only gap is a `signet_relay_pool_start()` failure, which happens only while stopping. So this is latent dead logic. Remove it when `nostrc-a7p6i` replaces the fixed accept delay.
- **I-2. `SIGNET_REPLAY_FULL` is not handled in NIP-46's early-error ladder** (`nip46_server.c:709–739`) and would fall through to allow. It is unreachable today, because the NIP-46 cache is unscoped and does not set `refuse_when_full`. SR-07's proposed fix would make it reachable. Recorded on `nostrc-2h11s`.
- **I-3. Kinds 24133 and 25910 are both in the ephemeral range (20000–29999).** Compliant relays do not store them, so cross-restart replay is in practice a kind-1059 gift-wrap concern, or a malicious or non-compliant relay concern. This supports keeping SR-02 at Medium.
- **I-4. Corrections to the security review's "not run" list:**
  - The python-fido2 external verifier does run on host: CTest `fido_interop_external` printed "PASS verify_external (python-fido2 independently decoded and verified signet's attestation object + ES256 assertion)".
  - The SQLCipher+passkeys Meson suite now ran (§Verification).

## Question 1 — NIP-46 core hardening and custody (`nostrc-1r5d.3`)

| Property | Finding | Evidence |
|---|---|---|
| Outer signature boundary | Every inbound event is Schnorr-verified before dispatch. Invalid events are dropped and logged by id only. | S `relay_pool.c:117–129`; R `relay_pool_protocol`, `relay_pool_wire` |
| Plaintext bounds | NIP-44 v2 decrypt; plaintext ≤ 64 KiB, NUL-free over the full span before C-string parsing; parser caps id 256 / method 128 / 64 params / 64 KiB per param | S `nip46_server.c:503–526`, `nip46_msg.c:7–10,…` |
| **Pairing-before-policy** | The policy identity is the agent the client is bound to, never the daemon default. Non-connect methods with no binding are refused `not_connected` before policy. Connect resolves either through an atomic consume-and-bind (one store transaction) or through the hash-pinned own-stale-secret reconnect. | S `nip46_server.c:554–645` (policy at `:681`); R `client_bindings`: `test_pair_once_reconnect_freely`, `test_reconnect_foreign_secret_refused`, `test_spent_secret_reuse_rejected`, `test_unbound_client_rejected` PASS. W6-05 caveat on connect side-effect ordering. |
| **Deny precedence** | A deny-listed agent identity is refused before policy evaluation on every path, including reconnect and requests. | S `nip46_server.c:658–672`; R `test_suspended_agent_binding_refused` PASS. SR-09/W6-05: the connect side effect happens first. |
| Custody TOCTOU | The binding is re-validated **inside** the custody transaction: `signet_bound_custody_callback` re-reads the client binding and secret hash under `BEGIN IMMEDIATE` before the key callback runs. Lock order is `ks->mu` → SQLite connection mutex. | S `key_store.c:999–1014,1056–1084`, `store.c:3000–3017` |
| **Writer fence** | Fenced identity: sign only when the lease is not revoked, an owner exists and `client == owner` (case-insensitive 64-hex). Fence marker without a lease row → deny. NIP-04 (`legacy_only` → NULL client) is denied once fenced. Writer client keys are single-use, so a former owner cannot be reinstated. | S `store.c:3067–3178`, `key_store.c:1104`; R `test_writer_fence_persists_and_fails_closed`, `test_writer_keys_are_single_use`, `test_writer_transfer_serializes_with_sign`, `test_fenced_nip46_signing_contract`, `test_fenced_nip46_nip44_contract`, `test_pre_history_writer_db_fails_closed` PASS |
| **Replay bounds** | Dedup is checked before the time window. Missing, overlong or zero-timestamp ids fail closed. TTL is raised to ≥ 2×skew. Management uses three class caches (16,384 ids each), refuse-when-full, 512 per non-provisioner sender, and is never skew 0. Production preflight enforces skew 1..120 and `max_entries > 0`, and the daemon reads the same INI keys. | S `replay_cache.c:89–275`, `mgmt_protocol.c:644–676,1852–1908`, `ops.py:218–221`, `signet_config.c:369–374`; R `replay_cache`, `mgmt_replay` ("All management replay tests passed"), `test_replayed_reissue_mints_once` PASS. Residuals: SR-02, SR-03, SR-07, I-2. |
| **OK/AUTH generation scoping** | Watches are keyed by `generation:event_id`. One wire OK settles every watcher of that event exactly once. A watch from an earlier generation is finished as `transport-aborted`, never accepted. AUTH acceptance triggers a resubscribe only for the current generation. Disconnect aborts all watches and clears readiness. A deadline timeout is a distinct `timeout:` status. Relay reasons are prefixed `relay-rejected:`. | S `relay_pool.c:166–311,400–540,573–592`; R `relay_pool_ok_lifetime`: "PASS (overlap, once, generation, abort, queued teardown)" |
| Readiness | Ready means: a connected relay whose **current required** subscription is not closed and has reached EOSE, with the revision checked. The post-AUTH resubscribe reissues the stored `#p` + `since` filter. | S `relay_pool.c:331–351,420–445,939–970,1721–1739` |
| Reply publication | The signed reply is enqueued and the same bytes are retained for failover, never re-signed. ACK outcomes go to `/metrics`. | S `nip46_server.c:948–964`, `relay_pool.c:~1212–1247` |
| **Encrypted custody** | Encrypted-required mode refuses before any service starts if the key or path is empty, or if there is no persistent store. SQLCipher is detected via `cipher_version`, then a keyed read rejects a wrong key, and `REQUIRE_ENCRYPTED_DB` refuses plain SQLite. The DEK is BLAKE2b-derived; agent nsecs are sealed with `crypto_secretbox_easy` using random nonces. | S `signetd_main.c:696–740`, `store.c:602–725,290–340,1451–1460`; R `daemon_startup` (wrong key, empty key/path, ambiguous key, unavailable key file, unwritable/unavailable state dir, missing/malformed config, listener unwind: all PASS), `key_store`, `store`, `backup_restore` (`test_envelope_tamper_rejected`, `test_file_tamper_rejected`, `test_wrong_keys_fail_closed`) PASS |

Conclusion: I found no Critical or High defect on these paths.

## Question 2 — §12.4 blocker disposition (my own)

| # | Blocker | My disposition | Evidence pointer |
|---|---|---|---|
| 1 | Health/bootstrap bind to all interfaces | **Cleared (code + host test).** Off-host confirmation is an owner Droplet step. | `http_listen.c`; `MHD_OPTION_SOCK_ADDR` `health_server.c:378`, `bootstrap_server.c:572`; daemon binds `127.0.0.1` `signetd_main.c:1009,1142`; R CTest `http_listen`, `daemon_startup` "enabled listener failure and bootstrap unwind" PASS |
| 2 | Missing config/key → ephemeral operation | **Cleared** | `signetd_main.c:686–740`; Compose sets `SIGNET_REQUIRE_ENCRYPTED_DB=true` (`docker-compose.yml:28`); R `daemon_startup` 10 cases PASS |
| 3 | Provisioner authority drift (D2) | **Cleared** | Compose mounts `/opt/signet/config` rw (`docker-compose.yml:33`); no provisioner env; preflight `test -w` as UID 10001 (`ops.py:211`); R `config_reload`: "signed D2 grant/revoke/reload/restart/empty: PASS", "adoption write failure and retry: PASS" (read-only dir logs CRITICAL, marker not set) |
| 4 | AUTH/publish OK collision; callback UAF | **Cleared** | §Question 1 OK/AUTH row; R `relay_pool_ok_lifetime`, `relay_pool_wire`, `relay_pool_protocol` PASS |
| 5 | Outbound TLS hardening/verification | **In progress** (sibling branch is adding the libnostr TLS test to the Docker `test` stage; I did not re-check the image). Host proof re-run by me: PASS. Note that the runtime image uses bookworm's `libwebsockets17`/`libssl3` (`signet/Dockerfile:57–60`), so the host result does not transfer. | R `test_connection_tls_no_resumption`: "ClientHello TLS >=1.2, ECDHE AEAD only, provider groups preserved"; "untrusted CA and wrong hostname both refused"; "no … session resumed" |
| 6 | Unresolved critical/high NIP-46/management findings | **Cleared**: this independent review finds none on NIP-46/management. The six Mediums need owner disposition but are not blockers (§Question 3). | This document; `nostrc-1r5d.3` can close for signet scope |
| 7 | No proven launch enrollment/reconnect | **Open: owner Droplet step** (`nostrc-1cx`). The harness exists: `signet/tests/integration/{nip46_reference_client.c,test_production_roundtrip.sh}`, registered DISABLED/`live;manual` (`signet/tests/CMakeLists.txt:322–338`), plus `homed_broker_login_nip46_live`. It needs a candidate image, which W6-01 currently blocks. | — |
| 8 | SQLCipher custody review / deployed-image proof | **Review half cleared** by this document (SQLCipher CTest + Meson green, custody path read). **In-image encrypted restart + restore: open, owner Droplet step.** I did not re-review nostrig `signet-sqlcipher-key-store-test-20261015` / `cb25ace7` as a diff; its `key_store` test passes under SQLCipher in both CTest and Meson. | R CTest `key_store`, `store`, `store_lifecycle`, `db_migration`, `backup_restore`; Meson SQLCipher+passkeys 42/42 |
| 9 | No trustworthy recovery procedure | **Open: owner Droplet step** (fresh-host restore drill, plan §10.F) | `backup_restore` round-trip is not a drill |
| 10 | Volume absence creates fresh state | **Open: owner Droplet step.** Code is present: mountpoint, UUID, fstype, DO device and volume id checked before any state path (`ops.py:145–153`, called first in `preflight` `:198`). | R `deployment_contract` PASS (with `jsonschema` in a throwaway venv; it fails on a bare host Python for lack of the module, which is an environment issue) |
| 11 | Idle core saturation | **Open: owner Droplet step** (observe idle CPU on the 1-vCPU Droplet). Not examined. | — |
| 12 | Version/tag mismatch | **Cleared at source.** Re-check at tag time is mechanical (`check-release.py --tag`). Publication is blocked by W6-01. | §Question 4 |
| 13 | Key ceremony not performed | **Open: owner Droplet step** (operator-present, plan §10.C) | — |
| 14 | Online backup consistency | **Cleared at test level.** The drill rides with #9. | R `test_online_backup_consistency: PASS (concurrent writes observed)` |
| 15 | Replay window across restart | **Cleared with disposition.** 270 s quarantine enforced on every supported start path; skew bounded 1..120; durable marker deferred (concur with SR-02). Fix the W6-02 docs before tag (recommended). | `ops.py:156–185,218`, `signet-compose.service:12–17`; I-3 |

Owner Droplet steps 7, 8 (in-image), 9, 10, 11 and 13 all need an image. Plan §16 puts W4 before W6. My position is that 7, 9, 10 and 13 should be demonstrated on a commit-addressed candidate image before the rc tag, and 11 observed in the same session. That makes W6-01 a prerequisite for them too, unless the owner builds the image on the Droplet.

## Question 3 — position on the six Medium findings

| Finding | Severity | Proposed disposition | Tag blocker? | Notes |
|---|---|---|---|---|
| SR-01 `nostrc-z3b0s`: direct 25910 executes secret-bearing mutations in plaintext | **Concur Medium (C)** | **Concur:** accept for rc.1 with the residual recorded in THREAT_MODEL §3.4. | **No** | Confirmed: `handle_intent` NIP-44-encrypts the already-plaintext params locally (`mgmt_protocol.c:1280–1294`), so the encryption protects nothing on the wire. Exploitation needs an authorized provisioner to bypass `signetctl`, which always wraps. The fix is cheap (refuse `agent_nsec`/`payload_b64` methods on direct ingress in encrypted mode). I recommend it as a **promotion criterion for `signet-v0.8.0`**, not for rc.1. |
| SR-02 `nostrc-17ggo`: restart replay relies on supported start paths; durable marker deferred | **Concur Medium (I)** | **Concur:** no durable marker for rc.1. | **No** | Arithmetic checked: a +120 s request is 150 s old at stop+270 s. I-3 narrows the practical surface to 1059 gift-wraps and malicious relays. Precondition I would attach: fix the W6-02 docs before tagging, because the README still teaches an unsafe 150 s. |
| SR-03 `nostrc-hbj7a`: wrap-id dedup; a re-wrapped rumor re-executes | **Concur Medium (I)** | **Concur:** accept for rc.1. | **No** | Only the original authorized sender can re-wrap, and the rumor `created_at` is skew-checked (`:1319–1325`), so the exposure is ≤ 240 s per rumor. (seal author, rumor id) dedup is a reasonable promotion-time fix. |
| SR-04 `nostrc-d1g76`: `/challenge` store unbounded | **Concur Medium, capped (A)** | **Concur:** profile stays disabled. | **No** | Not reachable at launch: no listener unless `bootstrap_port > 0` (`signetd_main.c:1007`), and preflight requires `port = 0` without the profile (`ops.py:230`). It must be fixed before the bootstrap profile is ever enabled, at which point it is High. |
| SR-05 `nostrc-nmefa`: `/bootstrap` releases the live connect secret on bearer + pubkey equality | **Concur Medium, capped (C/I)** | **Concur:** profile stays disabled. | **No** | Same gating as SR-04. Pubkey equality is not possession; this needs PoP + fresh secret before enablement. |
| SR-06 `nostrc-k2d0c`: virtual CTAP-HID has no per-caller authz | **Concur Medium (I)** | **Concur:** CTAP stays unsupported. | **No** | Preflight ties `virtual_ctap` to the `ctap` profile (`ops.py:239`), and the profile is not a Droplet launch surface. The daemon only warns (SR-11), so the disposition depends on preflight being the only start path, which it is on the Droplet. |

None of the six Mediums should block the tag. All six still need recorded owner acceptance (plan §8.D Medium treatment). SR-01 and SR-03 are good candidates for the `signet-v0.8.0` promotion criteria.

## Question 4 — version and tag

| Source | Value | Evidence |
|---|---|---|
| `VERSION_MANIFEST.md` signet row | 0.8.0, Latest release "Unreleased", Release tag "—" | `VERSION_MANIFEST.md:55` |
| `signet/meson.build` | `version: '0.8.0'` | `:17` |
| `signet/CMakeLists.txt` | `project(signet VERSION 0.8.0 LANGUAGES C)` | `:10` |
| `SIGNET_VERSION` | `"0.8.0"` | `signet/src/signetd_main.c:107` |
| `SIGNETCTL_VERSION` | `"0.8.0"` | `signet/src/signetctl_main.c:44` |
| libnostr | `declare_component_version(NOSTR 2 1 3)`; manifest 2.1.3 Unreleased | `libnostr/CMakeLists.txt:8`, `VERSION_MANIFEST.md:22` |

- **Tag rule check:** `signet-v0.8.0-rc.1` matches `AGENTS.md`'s `<component>-v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]`. The authoritative sources and Declared version carry the base 0.8.0, and the release columns stay unchanged until publication, as the prerelease rule requires.
- **`check-release.py` run (R):**
  - `--tag signet-v0.8.0-rc.1` → `{"version": "0.8.0", "image_tag": "0.8.0-rc.1"}`.
  - `signet-v0.8.1-rc.1` → "tag disagrees with source base version".
  - Caveat: W6-04 (the script does not read CMake).
- **Existing tags:** no `signet-v*` or `libnostr-v*` tags exist locally.
- **libnostr:** 2.1.3 ships inside the signet image, built from source. A libnostr tag is not a prerequisite for the signet rc.

## Question 5 — what the plan, review and critique all missed

1. **W6-01: the release route is unrunnable and, once re-pinned, sits in the image's trust path.** This alone changes the recommendation from "tag after the owner Droplet steps" to "fix the workflow first". It also blocks the candidate images those Droplet steps need.
2. **W6-02:** operator docs still teach an unsafe 150 s stop.
3. W6-03 and W6-05, and the latent I-2 fail-open that SR-07's own remediation would introduce, refine existing findings but do not change the recommendation.

## Verification (what I ran)

| Check | Result |
|---|---|
| `git -c protocol.file.allow=always submodule update --init third_party/nostrdb` | OK |
| `cmake -S . -B build-w6 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIGNET_ENABLE=ON -DBUILD_TESTING=ON -DSIGNET_USE_SQLCIPHER=ON -DSIGNET_ENABLE_PASSKEYS=ON -DSIGNET_ENABLE_TEST_HOOKS=OFF` | OK. sqlcipher 3.53.3 and libcbor 0.14.0 found; cache shows SQLCipher ON, passkeys ON, test hooks OFF. |
| `ninja -C build-w6 signet/all signet/tests/all tests/test_connection_tls_no_resumption` | OK (239 steps; only duplicate-library linker warnings) |
| `ctest --test-dir build-w6/signet/tests --output-on-failure -j6` | 42 passed, 1 failed, 2 disabled (`live_dbus_interop_manual`, `production_roundtrip`: manual/live, not run). The failure was `deployment_contract`, because the host Python has no `jsonschema`. |
| `PATH=<throwaway venv with jsonschema>:$PATH ctest … -R '^deployment_contract$'` | PASS |
| `ctest … -V` on `key_store`, `backup_restore`, `config_reload`, `client_bindings`, `mgmt_replay`, `daemon_startup`, `relay_pool_ok_lifetime` | All PASS. Per-case lines quoted in §Question 1/2. |
| `ctest --test-dir build-w6/signet/tests -R '^fido_interop_external$' -V` | PASS: python-fido2 independently verified the attestation and the ES256 assertion |
| `ctest --test-dir build-w6 -R '^test_connection_tls_no_resumption$' -V` | PASS (TLS ≥ 1.2 / AEAD; untrusted CA **and** IP/hostname mismatch refused; no resumption) |
| `meson setup build-w6-meson-sqlcipher signet -Dnostrc_build_dir=../build-w6 -Dsignet_use_sqlcipher=true -Dsignet_enable_passkeys=true -Dsignet_enable_tests=true` + `meson test` | **42 OK, 0 fail, 0 skipped.** sqlcipher found, passkeys enabled. |
| `python3 -I signet/deploy/check-release.py --tag …` (three tags) | As in §Question 4 |
| `gh api repos/aquasecurity/trivy-action/git/ref/tags/{0.33.1,v0.33.1,0.35.0,v0.35.0}`; advisory GHSA-9p44 | 404 / `b6643a29` / `57a97c7e` / `57a97c7e`; GHSA range `>=0.31.0 <=0.34.0` |
| `gh run list --workflow signet-release.yml` | 404: never run on GitHub |

**Not run:** the Meson plain-SQLite matrix, ASan, any Docker image build (blocker 5 is in progress on the sibling branch), the Droplet steps, live relays, the `production_roundtrip` and `homed_broker_login_nip46_live` live tests, and Linux UHID.

**Not re-queried:** nostrig. `nostrig task list --json` returned nothing parseable within the time box, so the plan §12.2 snapshot stands unrefreshed. `nostr-protocol-smells` was not rerun: this review changes no protocol code, and W2.A's scoped pass is recorded on `nostrc-mk7v1.3`.

## Version decision

This review is documentation only: **no component bump.** Signet stays at 0.8.0 (MINOR, already declared) and libnostr at 2.1.3 (PATCH, already declared). The W6-01 fix is CI-only and needs no bump. Each Low's remediation gets its own assessment in its bead.

## Merge and tag recommendation

- **Merge** this review (documentation only).
- **Close `nostrc-1r5d.3`** for signet scope. No Critical or High exists on the NIP-46, management or custody paths.
- **Do not tag `signet-v0.8.0-rc.1` yet.** Tag when all of the following hold:
  1. **W6-01 (`nostrc-kc35u`) is fixed:** third-party actions are SHA-pinned and the image identity is sealed before any third-party step. It now blocks `nostrc-mk7v1.7`.
  2. Blocker 5's in-image TLS test lands and passes in the Docker `test` stage.
  3. The owner records acceptance of SR-01 to SR-06 (I recommend accepting all six as Medium, non-blocking, with SR-01 and SR-03 as promotion criteria).
  4. The owner Droplet steps 7, 8 (in-image restart/restore), 9, 10 and 13 are demonstrated on a commit-addressed candidate image, with 11 observed in the same session. This is per plan §16 order; the owner may choose to sequence it differently, but should record that choice.
  5. Recommended, not required: the W6-02 doc fix lands in the same pre-tag commit.
- **Promotion criteria for `signet-v0.8.0`:** keep the plan's proposed criteria (14-day soak, no new Critical/High, one scheduled backup + restore drill), and add SR-01 and SR-03 remediation.
