# Nostr Linux and Samba Login: Shipping Plan

**Started:** 2026-09-19 · **Finalized:** 2026-09-20 · **Status:** implementation dispatched; no release certification

**Execution authority (2026-09-20):** detailed work and status now live in beads: A `nostrc-nxpb`, B `nostrc-zcll`, C `nostrc-ot2c`, D `nostrc-rb0e`, final independent-review epic E `nostrc-1r5d` (40 child tasks total at creation). E has one review per A–D plus a combined release review; dependencies target implementation children, not review-dependent epic closure. Implementation is isolated on branch `feat/nostr-linux-samba-login-20260920`. The existing main-checkout changes are excluded. Local Docker compilation is available; real GDM/AD/Windows acceptance infrastructure is not configured and remains a hard gate. Consult beads for live status; this document is the design contract, not a substitute tracker.

### Implementation checkpoint — 2026-09-20

The first implementation wave is partial, not a release candidate. B has delivered packet validation, cryptographic challenge verification, vault encryption, and a local signing provider; broker/PAM activation is still absent. Root independently reran protocol and vault tests with ASan/UBSan. The D acceptance harness passes six portable tests and reports unavailable infrastructure rather than success. A identity authority and C NIP-46 hardening are in progress; D owns shared build integration. Detailed residual acceptance criteria remain in the child beads.

Two explicit gates remain unresolved: approval of the proposed dedicated-password SMB contract (`nostrc-rb0e.2`), and provision of the disposable GDM/Samba/AD/Windows lab (`nostrc-rb0e.1`). Portable work does not waive either gate. No shipping epic is complete.

## 1. Goal and release decision

Ship this as a **focused architectural separation**, not a patch to the existing session-bus signing call: introduce a system-level authentication broker, a root-controlled identity authority, read-only NSS projection, and two explicitly bound signing providers; make `pam_nostr` authenticate before session creation; and provision ordinary local homes without FUSE or Blossom. Deliver Samba through two separate products: a **proposed Nostr-authorized credential bridge for managed standalone SMB shares**, and conventional **winbind/pam_winbind domain desktop login**. Neither is Nostr-to-Kerberos translation. All support claims remain gated on pinned-version, real pre-login and client-interoperability tests.

**Recommendation: do not ship the existing PAM stack as authentication.** Prioritize proof verification and collision-safe identity ownership as P0 blockers, with the full product delivered through the P0/P1 scheduling priorities in §6. P0 means a prerequisite/security or release gate; both P1 Samba deliverables remain mandatory. User-confirmed scope is both local-key and external-signer Nostr login, both SMB share access and Samba/AD desktop login, and ordinary local homes first. Roaming homes are excluded from initial acceptance.

**Original planning audit boundary:** source/document inspection and read-only beads queries against working tree HEAD `ecc5948f69cc876507f2b307a792ff8b3046e475`; no build, installed-system test, deployment, issue mutation, or implementation was performed. The checkout contains unrelated staged and unstaged changes. This plan and the bounded critique are workflow artifacts; do not infer implementation completion.

### Background and evidence map

References are repository-relative; abbreviated `src/`, `tests/`, and `docs/` paths inside homed sections mean `gnome/nostr-homed/`, unless explicitly identified as repository-wide. New paths are proposals, not existing artifacts.

| Verified seam | Evidence | Release consequence |
|---|---|---|
| Cache presence is authentication; signature length is the only proof check | `gnome/nostr-homed/src/pam/pam_nostr.c:54-139` | P0: cannot grant login until enrolled-key cryptographic proof succeeds. |
| Session-only signer and FUSE startup | `gnome/nostr-homed/src/pam/pam_nostr.c:156-240`; `gnome/nostr-homed/docs/SYSTEMD_TOPOLOGY.md:3-44` | Move proof before session creation; local homes cannot depend on this path. |
| Hash allocation and UID identity overwrite | `gnome/nostr-homed/src/common/nostr_cache.c:7-38,117-131` | Replace authority; preserve approved existing owners during migration. |
| NSS mutation and unsafe initgroups | `gnome/nostr-homed/src/nss/nss_nostr.c:84-164`; cache open at `gnome/nostr-homed/src/common/nostr_cache.c:22-28` | Read-only identity projection and correct NSS result semantics. |
| WarmCache frees relay list before secrets fetch and ignores provisioning results | `gnome/nostr-homed/src/ctl/nostr-homectl.c:304-345` | Do not reuse roaming orchestration as an auth service. |
| Strict signed-event validation is reusable | `libnostr/src/event.c:489-526,580-699` | Add account/challenge policy around existing crypto, not custom Schnorr. |
| Actual NIP-46 transport is separate from D-Bus | `nips/nip46/src/core/nip46_session.c:559-666,675-780,818-940,1250-1375` | Harden lifecycle and distinguish transport key from user key/token. |
| Async ownership mismatch | `nips/nip46/include/nostr/nip46/nip46_client.h:13-18`; `nips/nip46/src/core/nip46_session.c:1635-1642`; `nips/nip46/src/glib/nip46_client_g.c:16-80` | Named caller audit and lifetime tests before reuse. |
| Bare-signature signer interface | `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml:31-51` | Interface existence proves neither account binding nor external approval. |
| Mock fallback in real-signer test | `gnome/nostr-homed/tests/integration/run_e2e_real_signer_test.sh:39-52`; `gnome/nostr-homed/tests/integration/mock_signer.c:52-59` | Require genuine login tests; retain invalid fixture as a rejection test. |
| PAM optional, FUSE-coupled builds | `gnome/nostr-homed/CMakeLists.txt:73-100,145-190`; `gnome/nostr-homed/meson.build:12-27,63-73`; root `CMakeLists.txt:176-179` | Required host packages and explicit feature gates. |
| Version ambiguity | `gnome/nostr-homed/nostr-homed.pc.in:8-9`; `VERSION_MANIFEST.md:13-21` | Establish authoritative version sources before release. |

### Beads and prior-art reconciliation

The installed `bd search` excludes closed issues by default. The audit repeated title searches using `--status all --limit 0`, then scanned all **307** records returned by `bd list --all --limit 0 --json --readonly` across title, description, notes and design for homed/PAM/NSS/Samba/winbind terms. No task matching those subsystem terms was found. This is the current store, not proof that historical work never existed. `nostrc-diz6`, cited by topology documentation, cannot be retrieved; do not reuse its ID or infer completion.

| Existing bead | Current status | Relevant evidence and treatment |
|---|---|---|
| `nostrc-svsj` | in_progress, P1 | GNostr application publishing under NIP-46; notes await user confirmation. Reuse lessons about explicit pubkeys/result validation, not as Linux-login acceptance or an automatic blocker. |
| `nostrc-80qa` | closed, P1 | App handshake-token/client-key confusion. Regression requirement for independent token and transport key in the broker. |
| `nostrc-koso` | closed, P1 | App single-relay pairing failure. Preserve explicit relay configuration and late-connection tests. |
| `nostrc-br78` | closed, P1 | App response misclassification/re-entry and multi-relay listener race. Require request/purpose binding and one terminal completion. |
| `nostrc-bmue` | closed, P3 | App retry/session lifetime fix; closure reports 23 NIP-46 tests. Do not infer generic GLib wrapper lifetime correctness from the app fix. |

Existing design/quickstart/security/topology documents remain useful history but are not shipping evidence. `gnome/nostr-homed/docs/DESIGN.md:7-44` describes roaming homes and stronger verification than source implements. `gnome/nostr-homed/docs/SYSTEMD_TOPOLOGY.md:65-70` describes login without a mount, while present open-session errors may fail the PAM session depending on stack controls. Recent homed history includes `94c55d30` test hardening and `b17e2a6e` protocol remediation; neither certifies GDM or Samba behavior. No Samba references were found in the 60-file homed subtree, and no relevant current beads were found. This is a scoped finding, not an assertion that every repository file was audited.

Future execution should create one P1 parent bead and granular children from the execution index; split Phase 3 into protocol admission, request ownership/cancellation, and provider compatibility items, and Phase 7 into credential authority, delivery, and revocation items. Recheck the live tracker first to avoid duplicates. No beads were created or closed by this planning run.

## 2. Current-state analysis

### 2.1 Existing authentication and identity flow

The supplied implementation currently follows this path:

```text
PAM authenticate
  → nh_cache_open_configured()
  → username exists in users
  → PAM_SUCCESS

PAM open_session
  → same cache lookup
  → synthesize HOME/SHELL/XDG variables
  → connect to caller's session bus
  → SignEvent(unsigned event, empty identity, "pam_nostr")
  → accept any 128-character result
  → Homed1.OpenSession(username)
  → warm-cache / systemctl / FUSE mount
```

The security boundary is wrong in several independent ways:

| Source | Observed behavior | Consequence |
|---|---|---|
| `src/pam/pam_nostr.c:pam_sm_authenticate` | Cache presence returns success. | Identification is treated as authentication. |
| `pam_nip46_challenge` | Empty identity selector, no enrolled-key lookup, length-only signature check. | Neither key ownership nor account binding is established. |
| `pam_sm_open_session` | Authentication is deferred until session opening. | Applications using PAM authentication without opening a session can bypass the supposed proof. |
| `pam_sm_open_session` | Uses `g_bus_get_sync(G_BUS_TYPE_SESSION)`. | This does not establish availability of the target user's bus at GDM pre-login. |
| `pam_sm_open_session` | Constructs home and runtime paths instead of relying on the account/logind contracts. | PAM environment can diverge from account state; setting a runtime path does not create a session bus. |
| `src/common/nip46_client_dbus.c` | Probes session-bus names. | Despite its filename, this is not a NIP-46 external-signer client. |

The existing PAM exported signatures are standard Linux-PAM entry points and must remain unchanged.

### 2.2 Existing cache ownership and mutation

`nh_cache` owns a SQLite connection plus UID allocation parameters. `nh_cache_open()` opens read/write, enables WAL, and creates tables. NSS opens this same database for every lookup.

The relevant transformation is:

```text
signer-selected npub
  → SHA256 of the supplied textual representation
  → first 32 digest bits modulo UID range
  → users/groups writes
  → NSS passwd/group records
```

Important details:

- `nh_cache_map_npub_to_uid()` hashes the input string, not a normalized public-key byte sequence.
- `nh_warm_cache()` passes an npub string despite the helper parameter being named `npub_hex`.
- `nh_cache_upsert_user()` replaces account identity on UID conflict.
- User and primary-group provisioning are separate writes; their failures are ignored by `nh_warm_cache()`.
- Lookup APIs conflate missing records and database errors and silently truncate strings.
- NSS already uses caller-owned output buffers and per-call database connections. Those patterns should be retained.
- `_nss_nostr_initgroups_dyn()` currently appends a group without checking account membership, duplicates, or the supplied limit.
- `test_groups.c` expects reassignment that `nh_cache_ensure_primary_group()` explicitly rejects.

The existing `users` table is therefore **legacy data requiring administrator review**, not an enrollment authority.

### 2.3 Roaming-home coupling

`nostr-homectl` combines network fetches, signer selection, cache mutation, provisioning, decrypted secrets, mounting, status, and D-Bus dispatch.

Additional source findings relevant to separation:

- `nh_open_session()` invokes system `systemctl`, although installed units are user units.
- It records `"mounted"` and returns success after mount/start failures.
- `GetStatus` returns text although the XML describes JSON.
- `WarmCache` mutates process-global environment.
- The owned relay list is freed before the subsequent secrets fetch uses it.
- The manifest parser reads metadata under `meta`; `EVENT_SCHEMA.md` shows it flattened.

These defects must not be imported into the new broker. This plan removes roaming-home dependencies from authentication rather than attempting a broad roaming-filesystem repair.

### 2.4 Reusable cryptographic boundary

Reuse, unchanged:

- `nostr_event_deserialize_signed()`
- `nostr_event_compute_id()`
- `nostr_event_validate()`
- `nostr_event_sign_secure()`
- NIP-19 public-key decoding
- `nostr_secure_buf` and explicit secret wiping

`nostr_event_validate()` verifies the declared ID against the canonical NIP-01 hash and verifies the Schnorr signature. It does **not** establish authorization, challenge freshness, expected-account binding, or single use; the broker must add those checks.

`NostrEvent` is mutable and not thread-safe. Each challenge and returned event must have one owner and must not be mutated concurrently.

### 2.5 What NIP-46 provides—and what blocks direct reuse

The actual external path exists in `nips/nip46`, not the D-Bus adapter:

```text
parse bunker URI
  → set independent client transport secret
  → establish relay response subscriptions
  → connect RPC
  → get_public_key RPC
  → sign_event RPC
  → decrypt response and correlate request ID
```

Reusable elements include session ownership, encrypted envelope helpers, URI/message helpers, persistent connections, request correlation, transport selection, and cancellation entry points.

The source identifies the following integration hazards. Token handling, polling and ownership-contract mismatches are directly observable; race/exploit outcomes require the named reproductions before assigning a specific existing-library bug:

- `client_connect()` temporarily stores the URI authorization token in `s->secret`, the same field later used for the transport private key.
- `get_public_key()` is a local getter that can return a transport/client identity. Authentication requires `get_public_key_rpc()`.
- Startup includes connection-count polling; later RPC waits periodically scan relays for late publishing.
- Response subscription establishment and session registry registration can race.
- The callback takes a session pointer from the registry without retaining it.
- The callback does not itself validate the outer event signature or recipient tag. Whether upstream admission guarantees this is not established by the supplied files.
- Pending-request removal and channel ownership can race timeout/cancellation.
- Request IDs use a non-atomic global counter.
- Timeout accounting excludes substantial connection, pacing, and publish time.
- The RPC implementation accepts only string results while the message parser supports additional forms.
- Intermediate `auth_url` responses have no explicit lifecycle.
- Async callback ownership comments conflict with the implementation, which frees results after callbacks.
- GLib tasks retain raw session pointers and cancellation checks do not interrupt an active RPC.

These require a bounded NIP-46 lifecycle hardening effort before the provider is declared supported.

### 2.6 Existing signer constraints

The supplied NIP-55L implementation:

- Uses session-bus deployment.
- Resolves secrets from environment, user Secret Service, or Keychain.
- Has permissive identity fallbacks.
- May fill `created_at` during signing.
- Returns a bare signature over D-Bus.
- Uses caller-supplied `app_id` in approval policy.
- Does not establish a trusted pre-login approval channel in the supplied code.

The GNostr signer user unit also restricts network access to localhost. It cannot be assumed to provide remote relay access.

**Decision:** Leave existing signer services and their D-Bus wire contracts unchanged. Reuse their cryptographic building blocks, not their session-dependent storage, identity-selection policy, or approval service.

### 2.7 Existing coverage and packaging

Current tests prove neither desktop authentication nor Samba support:

- The mock returns a deliberately invalid constant signature.
- The “real signer” test falls back to that mock.
- Its relay fixtures contain zero signatures.
- It exercises FUSE persistence, not a GDM authentication stack.
- Scripts use sleeps, modify `/etc`, and sometimes report missing prerequisites as success.
- Both CMake and Meson silently omit PAM when unavailable; Meson also requires FUSE.
- The pkg-config file advertises a common library not installed by the supplied CMake rules.

The official Samba and MIT references in §10 support these architecture constraints:

- Encrypted SMB authentication does not become PAM authentication by loading `pam_nostr`.
- `obey pam restrictions` concerns account/session restrictions.
- Domain desktop login requires real domain credentials and trust machinery.
- PKINIT requires X.509 credentials and KDC configuration.

## 3. Design

### 3.1 Product boundary, target matrix, and feasibility gates

### Proposed first-release matrix

These are **candidate targets, not verified support**.

| Area | Proposed initial target |
|---|---|
| Desktop OS | Ubuntu 24.04 LTS, amd64, fully updated security packages |
| Login stack | Distribution GDM3, GNOME Shell 46 family, Linux-PAM, systemd-logind; Wayland greeter |
| Mandatory fallback | Distribution console login with a separate local break-glass administrator |
| Standalone SMB server | Dedicated Ubuntu 24.04 host, Samba 4.19 distribution family, `security = user`, `tdbsam`; no domain membership |
| SMB clients | Windows 11 24H2 Explorer and Ubuntu 24.04 GNOME Files/GVfs |
| Domain desktop | Same desktop distribution, winbind/pam_winbind; Samba AD DC laboratory domain |
| External signer candidates | Amber NIP-46 as primary compatibility candidate; independently operated bunker as secondary. Historical nsec.app flow is not a hosted-service availability promise. |
| Protocol profile | `bunker://`, NIP-44 v2 transport, `connect`, `get_public_key`, `sign_event`; explicit per-login approval |
| Homes | Local ext4 filesystem; no FUSE, network home, or automatic encrypted-home unlock |

Before implementation, freeze exact package builds, VM image hashes, client builds, signer versions or deployment identifiers, and NIP specification revisions in a new acceptance matrix.

Amber is the single named primary candidate; this is an accepted planning risk, not verified compatibility. The unnamed independently operated bunker is not claimed support. If Amber fails, stop external-login delivery and select/pin another real signer through a reviewed matrix amendment, rerunning the same gate; neither a mock nor implementing the repository's unfinished bunker silently satisfies the requirement. If a candidate fails its gate, mark it unsupported. Do not silently substitute a mock, older transport, or password-based Nostr route. At least one named external signer must pass before releasing the required external-signer feature.

### Required laboratory infrastructure

- Snapshot-capable desktop VM with actual GDM and console access.
- Dedicated standalone Samba VM.
- Separate Samba AD DC and joined desktop VM.
- Controlled relay with deterministic EVENT/OK/EOSE/CLOSED/AUTH and disconnect injection.
- Controllable NIP-46 signer using real cryptography, including different transport and user keys.
- Windows client VM.
- Network partition, packet loss, clock-change, disk-full, process-kill, and database corruption injection.
- Package repository snapshot containing every pinned dependency.

### Hard gates

1. **Pre-login UI:** prove standard GDM hidden PAM input, explicit provider choice and cancellation; private keys never enter PAM.
2. **IPC:** prove peer credentials, process lifetime binding, and cancellation under actual GDM workers.
3. **Local provider:** prove decrypt/sign/wipe with no home, session bus, or Secret Service.
4. **External provider:** prove the selected signer protocol profile, including denial and interruption.
5. **Samba administration:** prove the chosen Samba command/binding behavior and stable SID handling on pinned packages.
6. **Domain route:** prove actual PAM dispatch and credential-cache behavior.

A failed gate stops the affected deliverable. Passing local login does not waive external login; passing standalone SMB does not waive domain desktop login.

### 3.2 Component split and trust boundaries

Introduce these components under `gnome/nostr-homed`:

| Component | Role and ownership |
|---|---|
| `nostr-authd` | Root system daemon; owns authoritative account policy, authentication transactions, verification, and session receipts. No relay networking. |
| `nostr-auth-provider` | Sandboxed per-transaction worker executable; local-key or NIP-46 mode. Cannot enroll users or grant PAM success. |
| `nostr-authctl` | Root administration CLI; enrollment, disable, recovery, migration, inspection. |
| `nostr-auth-ui` | Optional post-login SMB prompt/display helper; same-UID broker connection. GDM uses standard PAM conversation. |
| `libnss_nostr.so.2` | Read-only account projection consumer. No crypto or network dependencies. |
| `pam_nostr.so` | Thin synchronous broker client using PAM-owned transaction data. |
| `nostr-smb-credentiald` | Separate standalone-server authority for dedicated SMB credentials. |
| `nostr-smb-access` | Desktop credential-acquisition client; authenticates through the local broker and delivers SMB credentials to the user. |

Keep `nostr-homectl`, Homed1, and `nostrfs` as separately packaged experimental roaming components. They are not authentication authorities or pre-login dependencies.

This separation is necessary because the existing user service cannot safely own system accounts, provide pre-login availability, and simultaneously consume user-selected roaming content.

### 3.3 IPC: credential-checked Unix sockets

### Decision

Use Unix `SOCK_SEQPACKET`, not system D-Bus, for the new broker.

System D-Bus would provide familiar activation and caller identity, but explicit socket lifetime, descriptor passing, and cancellation are particularly useful here. Do not move the general-purpose `org.nostr.Signer` interface onto the system bus.

### Endpoints

| Endpoint | Permissions | Callers |
|---|---|---|
| `/run/nostr-auth/auth.sock` | root:root `0600` | Privileged PAM hosts and administration CLI |
| `/run/nostr-auth/user.sock` | root:root `0666` | Authenticated local users requesting only their own SMB authorization |
| Worker socketpair | Inherited descriptors only | Broker and its specific worker |

World-connectable endpoints are not world-authorized. Enforce credentials before parsing operations.

### Framing and limits

Version-one messages contain:

```text
version, operation, request_id, transaction_id, payload
```

- One UTF-8 JSON object per packet.
- Maximum packet: 64 KiB; reject truncation and ancillary-data mismatches.
- Reject duplicate object keys, unsupported versions, unknown operations, and invalid field types.
- Secret input is a separate bounded byte packet, not a logged JSON field.
- Maximum passphrase input: 1,024 UTF-8 bytes.
- Maximum username: 32 ASCII characters.
- Maximum service name: 64 bytes.
- Maximum signed proof: 16 KiB.
- No production endpoint or authority-path overrides from environment variables.

Broker-generated transaction IDs and receipts use independent 256-bit random values.

### Caller binding

At connection acceptance:

1. Read `SO_PEERCRED`.
2. Obtain a pidfd for the peer and record process start identity.
3. Authorize endpoint and operation from kernel credentials.
4. For PAM requests, require UID 0 and an allowed PAM service.
5. Treat UID 0 as inside the host trust boundary; a caller-supplied service string is not an independent security credential.
6. Resolve username to the authoritative record; never accept caller-supplied UID or pubkey as authority.
7. Bind transaction to connection, pidfd, account UUID, UID, key generation, purpose, service, and broker boot instance.

A passed socket descriptor cannot transfer transaction ownership. Require per-message credentials and reject messages from another PID/UID. Peer death or disconnect cancels the transaction.

### Operations

Partial interface shapes:

| Operation | Inputs | Result |
|---|---|---|
| `BeginLogin` | Username, PAM service, TTY/remote-host context | Transaction ID, enrolled providers, deadline, sanitized prompt |
| `SelectProvider` | Transaction ID, enrolled provider ID | Progress or failure |
| `WaitResult` | Transaction ID | Verified receipt or typed failure |
| `Cancel` | Transaction ID | Idempotent acknowledgement |
| `CheckAccount` | Username, optional receipt | Current policy result |
| `OpenLocalSession` | Receipt | Connection-bound session reference |
| `CloseLocalSession` | Session reference | Idempotent acknowledgement |
| `BeginSmbProof` | Enrolled server ID, server challenge | Own-account transaction only |
| `SubmitUnlock` | Connection-owned transaction, secret byte packet | Acknowledgement only |
| Administrative operations | Explicit enrollment/recovery payloads | Durable result and audit operation ID |

`WaitResult` is a pending request completed by an event, not polling.

Public failure variants are:

`unknown_account`, `disabled`, `not_ready`, `denied`, `invalid_proof`, `expired`, `cancelled`, `rate_limited`, `provider_unavailable`, `network_unavailable`, `interaction_required`, `storage_error`, `protocol_error`, `internal_error`.

Unprivileged clients receive only their own account operations; no account inventory or pending-transaction listing.

**Default-deny operation/endpoint ACL:**

| Operations | Endpoint | Authorization |
|---|---|---|
| BeginLogin, CheckAccount, OpenLocalSession, CloseLocalSession | auth.sock | UID 0; allowed PAM service; target resolved by authority; session operations require same-connection receipt/reference |
| Enrollment, migration, configuration, enable/disable/retire/recovery | auth.sock | UID 0; explicit administrative operation; never reachable on user.sock |
| BeginSmbProof | user.sock | Kernel UID resolves to an active enrolled Nostr account; server/resource allowlisted; target identity is derived from peer UID, not request |
| SelectProvider, SubmitUnlock, WaitResult, Cancel | Either socket | Only for a transaction begun on that exact connection by that PID/UID, with matching purpose and current account generation; SubmitUnlock allowed only in local-provider WAITING_INPUT |
| Any unlisted pair or a worker-origin client operation | Neither | Reject before side effects |

Server challenges supplied by the user client are untrusted: enforce exact schema/purpose, enrolled server authority, resource allowlist, expected own-account pubkey, bounded expiry and delivery hash before signing. They cannot request arbitrary event signing, alternate account access or login receipts. Test every denied endpoint/operation pair and cross-connection transaction reuse.

### 3.4 Standard PAM input and pre-login UX

**Decision:** use GDM's existing PAM conversation, not a custom greeter patch. The user did not require keeping an unlock passphrase out of PAM; imposing that requirement would create an unnecessary GNOME Shell/GDM fork. Private Nostr keys must never enter PAM. The unlock passphrase necessarily passes transiently through the trusted GDM/PAM authentication process, then the credential-checked broker channel, and is wiped after delivery. Host root and the installed login stack are inside the threat boundary.

- Offer enrolled provider selection with `PAM_PROMPT_ECHO_ON` only when both are enabled: `Choose Nostr login method: local or remote`. Accept exactly ASCII `local` or `remote` after trimming ASCII surrounding whitespace; no empty/default choice. With one provider show its name and use it. Allow at most three invalid selections within the original 180-second transaction budget, then return PAM_MAXTRIES. Cancellation aborts immediately; never silently switch after failure.
- Request the local vault passphrase with `PAM_PROMPT_ECHO_OFF`. Keep it in private module-owned memory, not `PAM_AUTHTOK`; never propagate it into pam_unix/pam_winbind, environment, command arguments, clipboard or logs.
- Copy into protected broker input, explicitly wipe/free PAM response buffers and any temporary copies on every return path. Disable core dumps in the login host where distribution policy permits. Do not promise memory locking for copies inside GDM itself; document that boundary.
- External signing displays a short `PAM_TEXT_INFO` approval instruction. The paired phone/device handles approval; no QR image, arbitrary URL launch, user bus, or browser is required at the greeter.
- Verify actual GDM rendering, hidden input, cancel propagation and deadline behavior on the pinned distribution. A cancelled/hung conversation cannot produce a login receipt. If GDM does not support the required interaction safely, the release gate fails; do not replace real UI acceptance with a fake conversation.
- Break-glass local console login is mandatory. Nostr SSH, sudo, arbitrary PAM consumers and a separately supported Nostr console UI are outside the initial matrix; their auth entry points must nevertheless fail closed rather than reintroduce cache-only success.
- Post-login SMB acquisition uses a same-UID client on `user.sock`; it can present a hidden terminal or protected desktop input prompt and sends the unlock input on that same connection. There is no public attachment-capability UI endpoint.

The rejection of a custom greeter preserves provider selection, cancellation, secret redaction, pre-session availability and real-GDM tests while avoiding an unsupported patch-maintenance obligation. Enrollment still reads private keys only through a protected administrator interface, never a PAM prompt.

### 3.5 Challenge and proof contract

### Event choice

Use a **private, never-published kind-1 signed event** carrying a domain-separated authorization statement. This uses an existing generic event shape without misrepresenting the request as NIP-42 relay authentication or claiming an unassigned authentication kind.

Tradeoff: signers may display it as a note. Compatibility requires intelligible preview and assurance that `sign_event` does not publish it. A signer that auto-publishes these events is unsupported.

Define the application constant centrally; do not scatter numeric kind literals.

### Version-one challenge

The broker constructs:

- `pubkey`: enrolled lowercase x-only hex key.
- `created_at`: nonzero issuance timestamp.
- `kind`: application challenge kind.
- `tags`: fixed application identifier, purpose, and challenge nonce.
- `content`: compact JSON serialized by Jansson, not formatted string interpolation.

Content fields:

| Field | Meaning |
|---|---|
| `protocol` | `"org.nostr.auth/1"` |
| `purpose` | `"linux-login"`, `"enrollment"`, `"provider-enrollment"`, or `"smb-credential"` |
| `nonce` | 32 random bytes encoded as lowercase hex |
| `transaction_id` | Broker transaction identity |
| `authority_id` | Persistent random identifier of the issuing authority |
| `boot_id` | Issuer boot instance |
| `account_id` | Immutable account UUID |
| `username` | Canonical authority-selected name |
| `uid` | Authority-selected UID |
| `pubkey` | Expected enrolled user key |
| `key_generation` | Enrollment generation |
| `service` | Allowed PAM service or SMB operation |
| `issued_at`, `expires_at` | Unix seconds; 120-second validity |
| `context` | Escaped host/seat/TTY/remote-host display information |
| `resource` | Empty for login; server/share-policy identifiers for SMB |

For SMB, include the server-issued challenge and delivery-secret hash described below. The local broker must not replace server challenge fields.

Context strings are informational, not trusted authorization claims.

### Verification algorithm

1. Check transaction is pending and its monotonic deadline has not elapsed.
2. Strictly parse the complete signed event.
3. Call `nostr_event_validate()`.
4. Require the expected enrolled pubkey.
5. Require exact equality of kind, timestamp, ordered tags, and content bytes to the broker's immutable challenge.
6. Require canonical event ID equal to the expected challenge ID.
7. Re-read account status and key generation from the authority.
8. Atomically transition pending proof to verified, once.
9. Produce a receipt bound to the original PAM connection and target account.

A returned bare signature can be adapted only by attaching it to the exact immutable challenge and then performing the same validation. The new providers return complete events; no new D-Bus signature adapter is required.

### Freshness and replay

- Use monotonic time for local expiry; signed wall-clock timestamps are descriptive and aid remote verification.
- Challenge lifetime: 120 seconds.
- Whole authentication transaction: 180 seconds, including input and setup.
- Mint the local challenge before collecting the passphrase; mint the external challenge after connection setup. In both cases ensure sufficient whole-transaction time remains and enforce both deadlines.
- Receipts expire after 300 seconds and are not reusable on another connection.
- Duplicate valid responses have no additional effect.
- Late, cancelled, wrong-generation, wrong-purpose, and wrong-account responses never revive a transaction.
- Restart discards all pending challenges and receipts; persistence cannot resurrect authentication.
- Never publish proofs or write signed challenges to normal logs.

Verification is linear in bounded event size. Pending transactions use a hash table indexed by transaction ID.

### 3.6 Broker state, concurrency, and cancellation

### Transaction states

```text
NEW
 → POLICY_CHECKED
 → WAITING_INPUT / PREPARING_PROVIDER
 → WAITING_PROOF
 → VERIFYING
 → VERIFIED
 → SESSION_OPENED
 → CLOSED
```

Terminal failures from any pre-verification state:

`DENIED`, `EXPIRED`, `CANCELLED`, `FAILED`.

No failure state transitions back to success.

### Execution model

- One GLib main context owns transaction state, sockets, child watches, and timers.
- Authority writes are serialized.
- Expensive KDF and network operations run in provider processes.
- Workers own their event/session objects.
- Worker completion returns immutable data; the main context performs final state admission.
- Maximum eight simultaneous authentication transactions, maximum two scrypt workers.
- No unbounded queue: excess requests receive `rate_limited`.
- Provider subprocesses run without root and without access to authority files or homes.
- Local workers have no network access; external workers have network access but no authority-write capability.

Use separate systemd sandbox profiles for local and external worker modes. Pass only the transaction's required configuration through inherited descriptors.

### Cancellation contract

Triggers include:

- PAM conversation cancellation.
- PAM handle cleanup.
- Caller connection close or pidfd death.
- Greeter destruction.
- Broker deadline.
- Account disable/key-generation change.
- Broker shutdown.

On cancellation:

1. Mark terminal before requesting worker cancellation.
2. Close input and result channels.
3. Cancel NIP-46 work and stop subscriptions.
4. Wipe secret input and decrypted keys.
5. Allow two seconds for worker cleanup, then terminate the worker.
6. Reap the child; discard any subsequent message.

A KDF may not be safely interruptible in-process. Process termination provides a bounded cleanup boundary.

PAM cancellation should return within one second of broker cancellation acknowledgement; all worker resources must be gone within three seconds. No cancellation path retains an authentication receipt.

### 3.7 Provider interface

Use an internal C enum and opaque operation object, not a plugin ABI.

Provider cases:

- `LOCAL_ENCRYPTED_KEY`
- `NIP46_BUNKER`

Internal operations:

```text
prepare(enrollment_snapshot, deadline)
begin_proof(immutable_challenge)
submit_unlock(secret_bytes)          // local only
cancel()
destroy()
```

Asynchronous events:

- `ready`
- `unlock_required`
- `approval_pending`
- `signed_event`
- `denied`
- `unavailable`
- `interaction_required`
- `failed`

The provider never returns “login successful.” Only the broker verifier may do so.

One account has one enrolled Nostr identity and may have both provider types for that same identity. If both are present, the user chooses explicitly. Failure does not silently switch providers.

### 3.8 Encrypted local-key provider

### Storage decision

Use a small versioned encrypted vault in the private authority database:

- KDF: OpenSSL scrypt.
- Parameters: `N = 262144`, `r = 8`, `p = 1`.
- Salt: 32 random bytes.
- Derived key: 32 bytes.
- AEAD: AES-256-GCM.
- Nonce: 12 fresh random bytes.
- Authentication tag: 16 bytes.
- Plaintext: exactly one validated 32-byte secp256k1 secret.
- Associated data: version, provider UUID, account UUID, enrolled pubkey, and key generation, using a specified length-prefixed encoding.
- Reject unsupported parameters rather than accepting attacker-controlled KDF costs.

The fixed KDF uses approximately 256 MiB per active unlock. Gate it on the minimum supported machine; maximum two workers limits memory amplification.

Passphrase rules:

- Minimum 12 UTF-8 bytes at creation, maximum 1,024 bytes.
- No Unicode normalization or silent whitespace trimming.
- Confirmation required during enrollment.
- Wrong passphrase and corrupted ciphertext produce the same user-facing unlock failure.

### Unlock flow

```text
GDM hidden PAM conversation (or protected post-login SMB prompt)
 → credential-bound broker transaction
 → local sandbox worker
 → scrypt
 → AEAD decrypt
 → validate scalar and derive expected pubkey
 → sign exact challenge with nostr_event_sign_secure()
 → wipe passphrase, derived key, plaintext key
 → return complete signed event
 → broker verification
```

Use locked, non-dumpable memory for secrets. Validate that `nostr_secure_buf` provides the required locking on the target; if not, add a private provider-side locked allocation wrapper. Failure to establish required secret-memory protections fails the operation.

No decrypted-key cache across transactions. No environment-secret fallback. No user keyring dependency.

### Enrollment and recovery

`nostr-authctl enroll-local`:

- Reads a key from a protected input descriptor or hidden terminal prompt, never an argument.
- Reads and confirms the passphrase outside PAM.
- Derives and displays the public identity.
- Encrypts the key and stages enrollment.
- Requires successful decrypt/sign/verify before activation.

Supported recovery:

- Change passphrase by unlocking and re-encrypting with fresh salt and nonce.
- Restore an encrypted backup with its passphrase.
- Root-authorized re-enrollment of the same key.
- Root-authorized identity replacement with explicit acknowledgement and a new key generation.

There is no passphrase bypass and no recovery from a lost passphrase without a key backup or replacement identity. Preserve UID/home ownership during authorized key replacement; cancel outstanding transactions.

Local login works offline after enrollment. It does not automatically unlock GNOME Keyring or an encrypted home.

### 3.9 NIP-46 provider

### Enrollment contract

Initial support is **pre-paired bunker flow**, not first-login QR enrollment.

Persist:

- Remote signer transport pubkey.
- Expected user pubkey established by `get_public_key_rpc()`: never assume it equals, or differs from, the remote signer transport pubkey. Store the two roles separately even when their values match.
- One to three explicitly configured relay URLs.
- Independent client transport keypair.
- Optional connect authorization token.
- Explicit transport mode: NIP-44 v2 only.
- Enrollment and protocol-profile versions.

Generate the client key locally; never use the URI `secret` as its private key.

Pairing must perform:

1. Strict URI validation and relay policy validation.
2. `connect` with limited signing permissions.
3. `get_public_key_rpc()`.
4. Equality with the administrator-selected user key.
5. A verified enrollment proof.
6. Explicit external-client approval policy validation.

Request `sign_event:1` where supported. If a signer only supports unrestricted signing grants, that capability must be recorded and per-request approval must still be enforced. A signer that cannot demonstrate per-login approval is not supported for this product profile.

### Persisted secret protection

Encrypt transport secrets and connect tokens with AES-256-GCM using a machine wrapping key delivered through a systemd credential.

- Wrapping key material is root-controlled and excluded from ordinary database backups.
- Document that host-root compromise can obtain it.
- Restoring onto another host requires the wrapping credential or re-pairing.
- User private keys never enter this provider.

### Runtime sequence

```text
load enrolled pairing
 → launch external worker
 → configure client key and explicit NIP-44 mode
 → establish subscribed relay transport
 → connect RPC
 → get_public_key_rpc and compare
 → submit exact sign_event
 → await external approval
 → complete event validation in broker
 → stop worker/session
```

No session-bus signer is involved.

### Network and approval behavior

- TLS certificate verification required.
- Only enrolled `wss://` endpoints in production.
- No discovery from user profile events.
- No built-in public relay fallback.
- Loopback `ws://` allowed only in isolated test builds.
- Reject local/link-local destinations by default; an administrator can explicitly allow a private managed relay.
- Network unavailable: deny with `network_unavailable`; offer explicit retry or an independently enrolled local provider.
- Signer denial: `denied`.
- Browser `auth_url` interaction: recognize it as intermediate protocol state, but initial GDM support returns `interaction_required` rather than launching arbitrary URLs in the greeter. Pairing must establish a usable non-browser pre-login flow.
- Deadline expiry: cancel and discard later responses.
- Reboot: retain pairing only; create new transactions, nonces, and RPC IDs.

### Required NIP-46 hardening

Modify the existing implementation rather than adding another relay protocol stack.

1. Separate connect authorization token from transport secret.
2. Add an additive request-options API carrying an absolute monotonic deadline and cancellation handle; keep existing signatures as compatibility wrappers.
3. Generate cryptographically random request IDs.
4. Replace startup and late-publish polling with per-relay connection/subscription state callbacks.
5. Register callback/session ownership before transport starts.
6. Install the response subscription before publishing on each relay; restore it on reconnect.
7. Do not require EOSE before accepting live ephemeral responses.
8. Validate outer event ID/signature, expected author, recipient tag, and kind before decrypting.
9. Correlate decrypted responses only with active requests.
10. Give pending requests explicit reference ownership; terminal completion uses one atomic winner.
- Preserve pending state for intermediate protocol responses.
12. Handle OK rejection, CLOSED, AUTH, disconnect, and reconnect distinctly.
13. Reuse the same request/event identity for transport retries; do not create fresh approval requests automatically.
14. Include connection, pacing, publication, and response wait in the deadline.
15. Retain session references in GLib tasks; route cancellation to the request.
16. Resolve the callback ownership mismatch by honoring the published caller-owned result/error contract; audit every caller and terminal path for leaks/double frees. See §7 for compatibility requirements.

For relay AUTH, use only the client's transport identity under configured policy, never request a user signature implicitly.

Subscription restoration and frame callbacks depend on libnostr relay APIs whose definitions were not supplied. The feasibility gate must inspect those contracts. If APIs are missing, add narrowly scoped relay lifecycle hooks with exact ownership tests before integrating the provider; do not approximate them with sleeps.

### 3.10 Identity authority and read-only NSS projection

### Private authority

Path: `/var/lib/nostr-auth/private/authority.db`.

- Parent `/var/lib/nostr-auth` root:root `0755`; private subdirectory root:root `0700`.
- Database and journals `0600`.
- SQLite WAL, foreign keys enabled, durable transactions.
- Schema version via `PRAGMA user_version = 1`.
- Broker is the sole writer.

Core tables:

| Table | Fields |
|---|---|
| `metadata` | `key TEXT PRIMARY KEY`, `value TEXT NOT NULL`; authority UUID and current generation |
| `accounts` | `account_id TEXT PRIMARY KEY`, `username TEXT UNIQUE NOT NULL`, `uid INTEGER UNIQUE NOT NULL`, `gid INTEGER UNIQUE NOT NULL`, `home TEXT UNIQUE NOT NULL`, `shell TEXT NOT NULL`, `status TEXT NOT NULL`, `key_generation INTEGER NOT NULL DEFAULT 1`, `created_at INTEGER NOT NULL`, `updated_at INTEGER NOT NULL` |
| `identities` | `account_id TEXT UNIQUE REFERENCES accounts`, `pubkey_hex TEXT UNIQUE NOT NULL` |
| `providers` | `provider_id TEXT PRIMARY KEY`, `account_id TEXT REFERENCES accounts`, `type TEXT NOT NULL`, `enabled INTEGER NOT NULL DEFAULT 0`, `format_version INTEGER NOT NULL`, `public_config_json TEXT NOT NULL`, `secret_blob BLOB NOT NULL` |
| `operations` | `operation_id TEXT PRIMARY KEY`, `account_id TEXT`, `type TEXT NOT NULL`, `phase TEXT NOT NULL`, `details_json TEXT NOT NULL`, `updated_at INTEGER NOT NULL` |

`status` cases:

`enrolling`, `active`, `disabled`, `repair_required`, `retired`.

No implicit active default. No key/UID reassignment through upsert.

Public configuration contains no token or secret URI. Local vault blobs contain the specified salt/KDF/nonce/ciphertext/tag envelope; external blobs contain the encrypted pairing secrets.

### Names and UID partition

Initial policy:

- Nostr usernames: `n_` followed by 1–30 lowercase ASCII letters, digits, or underscores.
- Names are immutable after enrollment.
- Nostr UID/GID range: `200000–299999`.
- Domain default idmap range: `1000000–1099999`.
- Primary AD domain RID range: `1100000–1999999`.
- Standalone SMB service accounts on their separate server: `500000–599999`.
- Existing system/local allocations must not overlap these ranges.

Require fully qualified `DOMAIN\user` domain names; disable winbind default-domain name shortening.

Allocate the lowest unused Nostr UID/GID pair under a serialized enrollment transaction, after checking local and configured domain ownership. Persist it permanently. Retired IDs remain reserved.

This deliberately replaces probabilistic hashing: stable persisted ownership matters more than reproducible allocation on unrelated machines.

A site must reserve these ranges operationally; checking NSS cannot prevent an unrelated root administrator from creating a conflicting account later. A consistency check blocks activation and flags subsequent conflicts.

### NSS projection

Path: `/var/lib/nostr-auth/nss.db`.

- Root-owned directory `0755`, snapshot file `0644`.
- Contains only account/group fields and projection generation.
- No keys, provider details, authentication receipts, or secrets.
- Disabled accounts remain resolvable so existing files retain ownership names.
- Enrolling/repair-required accounts are not login-ready.

Publish projection as a complete new SQLite database:

1. Build temporary file in the same protected directory.
2. Validate schema and contents.
3. Close SQLite, fsync file.
4. Atomically rename.
5. Fsync directory.

Use rollback-journal mode for construction and leave no journal in the published snapshot. Never mutate an installed snapshot inode.

NSS opens with `SQLITE_OPEN_READONLY`; no schema creation, migration, WAL configuration, or network access. Keep one connection per lookup.

New private reader interface:

```text
identity_reader_open(path)
lookup_name / lookup_uid / lookup_group
 → FOUND | NOT_FOUND | UNAVAILABLE | TOO_SMALL
identity_reader_close()
```

Preserve standard `_nss_nostr_*` ABI and caller-buffer ownership.

Fix `initgroups` to:

- Return NOTFOUND for non-Nostr accounts.
- Avoid duplicates.
- Respect `limit`.
- Return `TRYAGAIN` with `ENOMEM`/`ERANGE` appropriately.
- Add only the enrolled primary group; supplemental groups remain administrator-managed.

Projection failure after an authority update leaves the previous snapshot intact. Authentication consults authority, so stale NSS cannot enable a disabled account. New enrollment remains unready until projection publication succeeds.

### Administration and migration

CLI commands:

- `enroll-local`
- `enroll-external`
- `add-provider`
- `disable`
- `enable`
- `retire`
- `replace-identity`
- `change-passphrase`
- `repair-home`
- `check`
- `inspect`
- `export-public`
- `migration-report`
- `import-reviewed`

Mutations require root and produce operation IDs. Secret values never appear in machine-readable output.

Legacy import:

1. Read legacy `cache.db` without modifying it.
2. Decode/normalize npubs.
3. Report malformed keys, duplicate identity/name/UID ownership, group conflicts, external NSS conflicts, invalid homes, and FUSE mount state.
4. Require explicit administrator acceptance per account.
5. Preserve approved existing UID/GID ownership; record approved legacy range exceptions.
6. Require fresh provider enrollment/proof.
7. Do not infer the rightful owner of an overwritten UID from the surviving row.
8. Do not activate accounts with mounted roaming homes.
9. Publish projection only after local-home readiness.

Authority and projection changes are monotonic-generation operations; retries use the operation ID and cannot create a second account.

### 3.11 Ordinary local homes

### Ownership and timing

Provision homes during enrollment, before activating an account. Authentication does not fetch a manifest or mount anything.

Default:

- Home: `/home/<canonical_username>`.
- Owner: enrolled UID/GID.
- Mode: `0700`.
- Shell: `/bin/bash`, validated against `/etc/shells`.
- Parent `/home` must be root-controlled and not writable by ordinary users.

### Provisioning algorithm

1. Reserve account and UID/GID as `enrolling`.
2. Open `/home` by descriptor.
3. Create a same-filesystem staging directory keyed by operation ID.
4. Copy approved `/etc/skel` content without following symlinks outside the source tree.
5. Apply ownership/modes using descriptor-relative operations.
6. Apply required security labels.
7. Fsync contents and directory.
8. Rename into place without replacing an existing path.
9. Verify ownership, mode, labels, and absence of unexpected mounts.
10. Publish NSS projection and mark account active.

If the target exists, accept it only when an administrator explicitly adopts it and validation succeeds. Never recursively chown an arbitrary existing home.

Ubuntu's AppArmor policy must permit only the broker's defined provisioning paths. SELinux deployments are outside the initial matrix; provide a documented labeling hook and fail activation if required labeling is unavailable.

### Crash recovery

`operations.phase` records:

`reserved`, `staged`, `installed`, `projected`, `complete`.

On restart, reconcile using operation ID and filesystem identity. Never delete an unexpected nonempty directory as automatic cleanup. Mark ambiguity `repair_required`.

`pam_sm_acct_mgmt` checks readiness. `pam_sm_open_session` revalidates the home and receipt; it does not perform key proof or create a home opportunistically.

Closing a session does not unmount or delete the local home.

### 3.12 PAM and domain routing

### Nostr PAM behavior

Keep all exported function signatures.

| Entry point | New responsibility |
|---|---|
| `pam_sm_authenticate` | Begin broker transaction, drive provider selection, hidden passphrase input and progress/cancel conversation, wait for verified proof, store receipt/connection in `pam_set_data`. |
| `pam_sm_setcred` | No cryptographic credential export; successful no-op for the Nostr branch. |
| `pam_sm_acct_mgmt` | Broker account-policy/readiness check; never accepts projection presence alone. |
| `pam_sm_open_session` | Validate receipt/current account/home and obtain session reference. No signer or Homed1 call. |
| `pam_sm_close_session` | Close reference best-effort; clear PAM-owned resources. |

Receipt cleanup runs on `pam_end`, including error paths. A new authenticate invocation on the same PAM handle cancels and replaces the previous attempt.

Do not set `XDG_RUNTIME_DIR`; let `pam_systemd` own it. Let the login application use NSS account fields for home and shell.

### Error mappings

| Broker/PAM condition | PAM result |
|---|---|
| Valid proof | `PAM_SUCCESS` |
| Outside the reserved Nostr namespace | `PAM_IGNORE` |
| Reserved Nostr name absent | `PAM_USER_UNKNOWN`, fatal within Nostr route |
| Denial, bad passphrase, invalid proof, replay | `PAM_AUTH_ERR` |
| Expiry or explicit cancellation | `PAM_AUTH_ERR` |
| Conversation failure | `PAM_CONV_ERR` |
| Rate limit | `PAM_MAXTRIES` |
| Provider/network/storage unavailable | `PAM_AUTHINFO_UNAVAIL` |
| Disabled/retired account | `PAM_PERM_DENIED` |
| Account not provisioned/readied | `PAM_PERM_DENIED` |
| Session receipt/home validation failure | `PAM_SESSION_ERR` |
| Internal programming/protocol failure | `PAM_SYSTEM_ERR` |

Operational messages distinguish unavailable signer from denial without exposing secrets. No failure in a reserved Nostr branch falls through to another authentication mechanism.

### PAM policy graph

Package a pinned Ubuntu profile implementing this exact graph:

```text
canonical name starts n_
  → Nostr auth only
  → Nostr account policy
  → standard limits/access policy
  → Nostr local-session validation
  → pam_systemd and normal desktop session

canonical name is configured DOMAIN\name
  → pam_winbind auth only
  → pam_winbind account policy
  → domain home creation
  → pam_winbind credentials/session + pam_systemd

all other names
  → existing local Unix policy
```

Preserve distribution security modules, failure accounting, and access restrictions. Generate and test the concrete PAM controls against the pinned installed stack; do not append a globally `sufficient` Nostr module or bypass common account restrictions.

The package installer must refuse unknown locally modified PAM layouts rather than guessing control-flow jumps. Installation and activation are separate operations.

### 3.13 Standalone SMB credential mediation

### Product contract

This is the **proposed smallest feasible bridge**, not a user-selected credential mechanism:

> A fresh Nostr proof authorizes a dedicated, random Samba password for a managed standalone server. Unmodified SMB clients authenticate using that username/password.

It is not passwordless SMB, does not authenticate SMB through PAM, and does not modify AD credentials.

### Server authority and mapping

Run `nostr-smb-credentiald` on a dedicated standalone Samba host.

It owns:

- HTTPS issuance API.
- Nostr pubkey → immutable SMB account UUID mapping.
- Stable Unix account, UID/GID, and Samba SID association.
- Allowed share-policy IDs.
- Credential generation, expiry, issuance journal, and revocation state.

The server's administrator independently authorizes the public key and share policy. A desktop broker cannot grant itself server ACLs.

SMB names are generated, not supplied by the user: `nsmb_` plus a collision-checked stable account-ID prefix. Unix accounts have locked passwords, `/usr/sbin/nologin`, and no desktop home.

Use one dedicated Samba account per enrolled server identity, not a new UID for each password.

### Issuance API

HTTPS only, server certificate validation mandatory. Server identity/URL and allowed share policy are root-enrolled on the desktop; arbitrary URLs from PAM/UI are forbidden.

Operations:

- `CreateChallenge(account_ref, share_policy, delivery_hash)`
- `SubmitProof(challenge_id, signed_event, delivery_secret)`
- `FetchCredential(operation_id, delivery_secret)`
- `AcknowledgeDelivery(operation_id, delivery_secret)`
- Administrative disable/rotate/revoke operations

The desktop client creates a random 256-bit delivery secret; only its hash appears in the signed challenge. The server binds the credential operation to that hash.

Server proof includes:

- Server authority UUID.
- Expected enrolled Nostr key.
- Immutable SMB mapping.
- Requested share-policy ID.
- Challenge ID, nonce, deadline.
- Delivery hash.
- Operation purpose.

The local broker independently checks server configuration and own-user binding, obtains a fresh provider signature, and returns it to `nostr-smb-access`. The server repeats full event and challenge verification; local PAM success is not an authorization token for SMB.

### Credential creation and Samba update

- Generate 32 random bytes; encode as a 43-character base64url password.
- Never derive credentials from nsec, npub, passphrase, or a previous password.
- Credential lifetime: 24 hours.
- Delivery retrieval window: five minutes.
- One current credential generation per account.
- Rotation is explicit and warns that other cached clients will need updating.

Use the pinned distribution's absolute-path Samba tools:

- `smbpasswd` noninteractive stdin for add/update/enable/disable.
- `pdbedit` or the pinned Samba Python passdb binding for account/SID inspection, selected and frozen in the Samba feasibility gate.
- Never edit `passdb.tdb` directly.
- No shell interpolation.
- Passwords enter child processes through pipes, not arguments or environment.

The gate must freeze one inspection mechanism and its exact version contract before implementation proceeds. Prefer the Python binding if its SID access is available and reliable on the pinned package; otherwise pin and fixture-test the CLI output parser.

Unix account creation uses the distribution account-management tool under a serialized lock. Reconciliation verifies that existing name, UID, and SID ownership match; it never takes over an existing account.

### Durable state and crash recovery

Private server database: `/var/lib/nostr-smb/credentials.db`, root-only.

Tables include:

- `accounts`: account ID, Nostr key, Unix/Samba name, UID/GID, SID, enabled state, share-policy ID.
- `credentials`: account ID, generation, expiry, encrypted password envelope, state.
- `issuances`: operation/challenge IDs, proof digest, delivery hash, expiry, state.
- `audit_operations`: operation metadata without secrets.

Issuance states:

```text
CHALLENGED
 → VERIFIED
 → PREPARED
 → ACCOUNT_DISABLED
 → PASSDB_UPDATED
 → ACTIVE
 → DELIVERED
 → ACKNOWLEDGED
```

Failures enter `RECONCILE_REQUIRED`; no credential is delivered until all account/passdb checks succeed.

Write the encrypted new credential and intended generation durably before changing Samba. Temporarily disable the account, update the password, verify mapping, then enable and commit active state. If a crash leaves ambiguity, keep the account disabled. With the v1 volatile delivery-key policy, loss of that key requires a fresh proof and a new generation; never restore an ambiguous generation to service. Reconciliation must idempotently disable any prepared or active-but-unacknowledged generation whose recovery key is gone.

A repeated fetch requires the delivery secret and returns the same generation during the five-minute window. Acknowledgement or expiry deletes the recoverable password envelope. The NT password hash necessarily remains in Samba passdb; document its offline-cracking exposure.

If delivery is lost after acknowledgement/expiry, require a new proof and rotation. Do not recover plaintext from Samba hashes.

### Storage and client delivery

Encrypt retained delivery passwords with AES-256-GCM under per-record volatile keys held in protected authority memory, with server/account/generation/expiry associated data. Do not back up these keys. A restart loses undelivered credentials and disables their generations, requiring fresh proof and reissuance. This chooses erasure and fail-closed recovery over persistent delivery escrow; see §7.

Initial client UX:

1. `nostr-smb-access` obtains proof and credential.
2. A protected window displays server, dedicated username, password, and expiry.
3. User enters them into Explorer or GNOME Files.
4. Optional clipboard copy is explicit; clear best-effort after 30 seconds and warn that clipboard managers may retain it.
5. No automatic persistent credential-manager entry in the first release.

An explicit export option may write a `0600` credential file, with a leakage warning. No automatic shell-command construction containing the password.

Standard-client caching is outside the broker's control. Rotation/expiry invalidates future authentication but does not erase saved passwords.

### ACL binding

Use stable Unix UID/GID and Samba SID mappings. Shares use administrator-defined allowlists/groups and filesystem ACLs. Nostr events cannot supply filesystem paths, Unix groups, or arbitrary ACL expressions.

Changing a Nostr key through server administration preserves or explicitly revokes the existing account mapping; it never silently transfers share ownership.

### Expiry and active sessions

**Disabling or rotating a password does not reliably terminate existing SMB sessions.**

Define two revocation modes:

- **Normal:** disable new authentication immediately; established sessions may continue.
- **Forced:** disable account and restart the dedicated Samba instance, disconnecting every client on that instance.

Forced mode is deliberately coarse and costly; it avoids pretending that password rotation revokes session keys. Warn about interrupted writes and require explicit administrative selection. Test durable-handle reconnection and multichannel behavior.

Run a dedicated `nostr-smbd.service`, not the host's unrelated Samba service. Bind its lifetime to the credential supervisor:

- Reconcile expired/disabled accounts before supervisor readiness.
- Start Samba only after reconciliation.
- Schedule expiry using deadlines; re-evaluate on wall-clock changes.
- Stop Samba if the supervisor fails or misses its watchdog.
- Target: new authentication blocked within five seconds of expiry while healthy; supervisor failure stops service within 30 seconds.

Normal expiry does not forcibly terminate already authenticated sessions. Documentation must state this prominently.

### Limits and audit

- One active issuance per account.
- Maximum three challenge starts per minute per account and client source.
- Five proof failures per 15 minutes per account.
- Explicit rotation no more than once per minute.
- Audit issuance, mapping changes, expiry, denial, rotation, and forced disconnect.
- Never audit passwords, transport keys, delivery secrets, or full proof content.

### 3.14 Samba/AD desktop login

This is an independent conventional domain route.

### Prerequisites

- Correct DNS and time synchronization.
- Domain join using authorized administration.
- Protected machine trust credentials.
- Reachable DC/KDC during join and initial authentication.
- Explicit domain allowlist.
- Stable idmap configuration deployed before users create files.

Use:

- NSS `winbind`.
- `pam_winbind`.
- Kerberos authentication where supported by the pinned configuration.
- `idmap_rid` for the primary domain.
- `tdb` for the default mapping domain.
- Fully qualified domain usernames.

Do not map AD users into the Nostr authority or derive their UID from npub.

### Home and credential policy

- Domain homes: `/home/<DOMAIN>/<user>`.
- Parent directory is root-controlled.
- Use distribution `pam_mkhomedir` for the domain route only, mode `0700`, after account authorization.
- Validate actual winbind naming/template behavior and labels in the pinned VM.
- Let pam_winbind manage its supported credential cache; verify `KRB5CCNAME`, cache ownership, and logout cleanup.
- No custom broker-generated TGTs.
- No Nostr provider fallback after domain authentication failure.

### Offline policy

Default: cached offline domain login **disabled**.

Optional administrator-enabled cached login requires:

- Prior successful online authentication.
- Documented inability to observe recent domain disable/group changes offline.
- Tests for password changes, account revocation, stale membership, and cache expiry behavior supported by the pinned Samba build.
- Explicit site acceptance of those risks.

Do not invent a revocation guarantee stronger than winbind provides. Domain route failure must not affect local or Nostr-local login.

### PKINIT comparison

A future Nostr-authorized PKINIT bridge would additionally require:

- A trusted certificate issuer and enrollment authority.
- X.509 subject/SAN mapping to AD principals.
- Client private-key storage or hardware token support.
- KDC PKINIT configuration and trusted CA anchors.
- Certificate validity, revocation, and renewal policy.
- Smartcard-style PAM/GDM integration.
- Proof that Samba/AD and selected clients accept the issued credentials.
- A defensible policy translating Nostr authorization into certificate issuance.

That is a separate identity-federation program. A Nostr signature alone is not a Kerberos credential and cannot mint a TGT.

### 3.15 Packaging, observability, and configuration

### Build/package split

Create separate build targets/packages:

1. Identity reader/NSS.
2. Authentication broker/PAM/local provider.
3. NIP-46 provider.
4. GDM PAM integration and post-login SMB UI.
5. Standalone SMB bridge.
6. Domain integration configuration/documentation.
7. Experimental roaming home.

For authentication-enabled builds:

- Linux-PAM is required; configuration fails if absent.
- No FUSE, curl/Blossom, roaming manifest, or user signer dependency.
- NSS links only its reader/config code and SQLite.
- Both CMake and Meson build the same supported functionality.
- Meson must not silently consume opportunistic libraries from a hard-coded CMake build directory.

### Configuration files

Root-owned, non-user-writable:

- `/etc/nostr-auth/auth.conf`
- `/etc/nostr-auth/servers.d/<server-id>.conf`
- `/etc/nostr-auth/nss.conf`
- `/etc/nostr-smb/credentiald.conf`
- Dedicated Samba configuration.
- Pinned PAM integration profile.

Configuration parsing is shared within each component, strict, bounded, and side-effect-free. Unknown security-relevant fields or invalid numeric values fail startup. `check-config` uses the same parser as runtime.

### Systemd policy

`nostr-authd`:

- System service and socket activation.
- No user-bus dependency.
- No external network.
- `NoNewPrivileges`, private temporary/devices, protected kernel interfaces, restricted address families, core dumps disabled.
- Explicit writable authority/runtime/home-provisioning paths.
- Home access justified only for provisioning and validation.

Provider workers receive stricter mode-specific policy.

Do not reuse the existing FUSE capabilities or its `CAP_SYS_ADMIN` configuration.

### Audit

Structured journal fields:

- Operation/transaction ID.
- Account UUID.
- Purpose and provider type.
- State transition.
- Result category.
- Duration.
- Authority generation.
- Peer UID/PID and configured PAM service.

Exclude key material, passphrases, delivery secrets, signed challenge content, and full bunker URIs.

Expose root-only health output for:

- Authority/projection generations.
- Pending transaction counts.
- Worker counts.
- Projection/reconciliation failures.
- Provider availability.
- Last successful consistency check.

Health is not authentication: a healthy relay connection never implies a successful proof.

## 4. File-by-file impact

Paths below are relative to the repository root. New paths are proposed explicitly; source contents not supplied are not treated as inspected.

### 4.1 Existing authentication and roaming files

| File | Change and reason | Dependency |
|---|---|---|
| `gnome/nostr-homed/src/pam/pam_nostr.c` | Replace cache authentication, signer call, environment synthesis, and Homed1 session calls with broker protocol/PAM-data lifecycle. | Broker protocol and PAM tests |
| `gnome/nostr-homed/include/pam_nostr.h` | Retain exported signatures; document new stage responsibilities. | PAM implementation |
| `gnome/nostr-homed/src/nss/nss_nostr.c` | Read new projection; preserve caller buffers/per-call connections; correct result and initgroups semantics. | Identity reader |
| `gnome/nostr-homed/include/nss_nostr.h` | Add missing initgroups declaration; no ABI change to existing functions. | NSS implementation |
| `gnome/nostr-homed/src/common/nostr_cache.c` | Make legacy upsert reject identity/name/GID/home takeover; retain legacy schema and hash helper. | Legacy regression tests |
| `gnome/nostr-homed/include/nostr_cache.h` | Document conflict refusal and legacy-only scope. | Cache correction |
| `gnome/nostr-homed/src/ctl/nostr-homectl.c` | Prevent WarmCache from writing the new identity authority. Preserve legacy-only behavior in the excluded roaming package; separately track its relay lifetime and false-success defects. Do not silently remove roaming provisioning as part of local-login delivery. | Authority split |
| `gnome/nostr-homed/config/nss_nostr.conf.sample` | Relabel as legacy roaming cache configuration; remove implication that new authentication uses it. | New configuration |
| `gnome/nostr-homed/dbus/org.nostr.Homed1.xml` | Document roaming-only service and the existing text/JSON discrepancy; a wire-contract repair belongs to a separate versioned roaming change. | Documentation/API review |

Do not rename Homed1 into an authentication API.

### 4.2 New broker, identity, and provider files

All paths in this table are under `gnome/nostr-homed/`.

| Proposed files | Responsibility |
|---|---|
| `include/nostr_auth_protocol.h`, `src/auth/auth_protocol.c` | Framing, operation/result variants, validation, limits; no policy decisions. |
| `include/nostr_auth_client.h`, `src/auth/auth_client.c` | Synchronous PAM/admin socket client, cancellation and receipt ownership. |
| `src/auth/nostr-authd.c` | Main loop, service startup, endpoint ownership, shutdown. |
| `src/auth/auth_peer.c` | Credential/pidfd binding and same-connection transaction ownership. |
| `src/auth/auth_transaction.c` | State machine, deadlines, rate limits, single completion. |
| `src/auth/auth_challenge.c` | Domain-separated challenge construction and strict verification. |
| `src/auth/auth_provider.h`, `src/auth/auth_provider.c` | Internal provider enum/events and worker supervision. |
| `src/auth/nostr-auth-provider.c` | Worker entry point and descriptor-only initialization. |
| `src/auth/provider_local.c` | Vault decrypt/sign/wipe. |
| `src/auth/provider_nip46.c` | Pairing setup, RPC lifecycle, typed provider results. |
| `src/auth/auth_vault.c` | Versioned scrypt/AES-GCM envelopes and machine-wrapped pairing storage. |
| `src/auth/auth_config.c` | Strict configuration parser shared with validation tooling. |
| `include/nostr_identity.h`, `src/identity/identity_store.c` | Private authority schema, transactions, generations, operations. |
| `src/identity/identity_reader.c` | Minimal read-only NSS projection access. |
| `src/identity/identity_projection.c` | Atomic snapshot generation. |
| `src/identity/identity_migrate.c` | Read-only legacy report and explicit reviewed import. |
| `src/identity/local_home.c` | Descriptor-relative provisioning and crash reconciliation. |
| `src/ctl/nostr-authctl.c` | Administration commands and protected secret input. |
| `src/ui/nostr-auth-ui.c` | Own-user post-login SMB prompts and credential display; no custom greeter. |
| `config/auth.conf.sample`, `config/nss.conf.sample` | New configuration contracts. |
| `systemd/nostr-authd.service`, `systemd/nostr-authd.socket` | System pre-login deployment. |
| `systemd/nostr-auth-provider-local@.service`, `systemd/nostr-auth-provider-nip46@.service` | Separate worker sandboxes. |
| `packaging/apparmor/usr.sbin.nostr-authd` | Broker confinement policy. |
| `packaging/pam/nostr-auth` | Pinned distribution PAM profile. |
| `packaging/gdm/README.md` | Tested distribution PAM conversation/configuration, version applicability and rollback; no greeter patch. |
| `docs/AUTH_PROTOCOL.md`, `docs/IDENTITY_STORE.md` | Versioned IPC, challenge, authority, and recovery specifications. |
| `docs/ACCEPTANCE_MATRIX.md` | Frozen versions, capability results, evidence links. |

The GDM gate identifies actual installed PAM configuration files and tests normal conversation behavior before activation. It does not authorize an upstream UI fork.

### 4.3 NIP-46 changes

| File | Change |
|---|---|
| `nips/nip46/src/core/nip46_session.c` | Fix secret-role separation, request ownership, deadlines, cancellation, response admission, lifecycle callbacks, and reconnect behavior. |
| `nips/nip46/src/core/nip46_msg.c` | Strict correlated response forms, intermediate-response handling, safe JSON escaping of IDs/errors. |
| `nips/nip46/src/core/nip46_uri.c` | Reject malformed escapes/embedded NULs, bound collections, propagate allocation errors, clean partial parses. |
| `nips/nip46/include/nostr/nip46/nip46_client.h` | Add request-options/cancellation API; correct callback ownership documentation; retain old entry points. |
| `nips/nip46/include/nostr/nip46/nip46_types.h` | Document synchronized state/lifetime contracts; add opaque request type if required by the additive API. |
| `nips/nip46/include/nostr/nip46/nip46_client_g.h` | Document real cancellation and session retention. |
| `nips/nip46/src/glib/nip46_client_g.c` | Retain sessions, use request cancellation, clear sensitive task data. |
| `nips/nip46/CMakeLists.txt` | Register mandatory lifecycle/admission tests. |
| New `nips/nip46/tests/test_request_lifecycle.c` | Completion/cancel/timeout races and ownership. |
| New `nips/nip46/tests/test_relay_lifecycle.c` | Subscription-before-publish, reconnect, OK/CLOSED/AUTH behavior. |
| New `nips/nip46/tests/test_signed_response_admission.c` | Outer event verification, recipient/author binding, duplicates. |
| New `nips/nip46/tests/test_glib_lifetime.c` | Task/session teardown and cancellation. |

Existing callback call sites must be enumerated before the ownership implementation correction lands. Reconcile each caller with the preserved caller-owned contract in that same commit; do not silently relabel an existing public API as borrowed.

No planned changes to `nip46_bunker_g.c` merely to fill its placeholder. A complete production bunker is not required for the client deliverable; the controlled test signer must nevertheless actually receive and answer relay traffic.

### 4.4 Samba deliverable files

All new paths below are under `gnome/nostr-homed/`.

| Proposed files | Responsibility |
|---|---|
| `src/smb/nostr-smb-credentiald.c` | HTTPS issuance, proof verification, expiry scheduling, supervisor readiness. |
| `src/smb/smb_store.c` | Versioned credentials/issuance journal and encrypted delivery envelopes. |
| `src/smb/smb_account.c` | Narrow Samba/Unix account provisioning and reconciliation. |
| `src/smb/smb_policy.c` | Key/account/share-policy authorization and rate limits. |
| `src/smb/nostr-smb-access.c` | User issuance client, broker request, secure delivery UI coordination. |
| `include/nostr_smb_protocol.h` | Versioned API fields and result categories. |
| `config/smb-credentiald.conf.sample` | Server authority and expiry policy. |
| `config/smb.conf.standalone.sample` | Dedicated standalone Samba configuration. |
| `config/servers.d/example.conf` | Desktop-enrolled server identity and policy. |
| `systemd/nostr-smb-credentiald.service` | Credential supervisor and watchdog. |
| `systemd/nostr-smbd.service` | Dedicated Samba instance bound to supervisor readiness/lifetime. |
| `docs/SAMBA_STANDALONE.md` | Credential mediation, caching, rotation, active-session semantics. |
| `docs/SAMBA_AD_LOGIN.md` | Join, PAM routing, idmap, domain homes, offline policy. |
| `config/smb.conf.winbind.sample` | Non-overlapping domain/idmap settings. |
| `packaging/pam/nostr-winbind-policy.md` | Exact policy graph and pinned distribution realization. |

The standalone authority and desktop broker share challenge verification code, not databases or administrative privileges.

### 4.5 Build and documentation changes

| File | Change |
|---|---|
| `gnome/nostr-homed/CMakeLists.txt` | Split targets/features/dependencies, require PAM for auth builds, install system units, register tests and true skip codes. |
| `gnome/nostr-homed/meson.build` | Match CMake features; remove mandatory FUSE for auth and implicit prebuilt-library discovery. |
| New `gnome/nostr-homed/meson.options` | Explicit auth/external/SMB/roaming feature switches. |
| `gnome/nostr-homed/tests/meson.build` | Match new unit/integration tests and feature requirements. |
| `gnome/nostr-homed/nostr-homed.pc.in` | Remove nonexistent library-link promise; generate accurate component version/metadata. |
| `gnome/nostr-homed/README.md` | Separate supported local-home auth from experimental roaming. |
| `docs/DESIGN.md` | Replace optimistic auth claims with broker/authority/provider architecture. |
| `docs/SECURITY.md` | Threat boundaries, passphrase path, root compromise, network and SMB limitations. |
| `docs/SYSTEMD_TOPOLOGY.md` | System broker plus optional post-login user services; remove stale issue claim. |
| `docs/QUICKSTART.md` | Safe VM enrollment, GDM activation, recovery, local homes; move roaming instructions into a clearly experimental section. |
| `docs/EVENT_SCHEMA.md` | Link new authentication schema; correct roaming metadata example documentation to match the parser without changing its wire format. |
| `VERSION_MANIFEST.md` | During implementation, register authoritative homed version sources and record applicable component decisions. |

Existing roaming systemd units remain excluded from auth package installation. Their comprehensive repair is not part of login acceptance.

**Explicitly unchanged during implementation unless a reviewed decision supersedes it:** this plan; existing signer XML/service assets; NIP-55L signing APIs; supplied libnostr event validation APIs; unrelated staged or untracked work.

### 4.6 Tests

Modify:

- `tests/unit/test_groups.c`: conflict refusal, not reassignment.
- `tests/unit/test_cache.c`: legacy UID takeover rejection.
- `tests/unit/test_uid_map.c`: label hashing legacy; no claim of collision-safe allocation.
- `tests/integration/mock_signer.c`: valid deterministic key/signing mode plus explicit invalid-proof modes; fake signature must fail auth.
- `run_e2e_real_signer_test.sh`: no mock fallback under a real-signer label; classify as roaming test.
- `run_mock_signer_test.sh`: retain its existing exit-77 prerequisite behavior and register that skip code in CTest; do not misidentify it as an exit-0 false pass.
- `run_systemd_opensession_test.sh`: replace prerequisite exit-0 paths at lines 10, 56 and 62 with exit 77; explicitly label experimental roaming, not login acceptance.
- Both build systems: register exit 77 as skip where applicable.

Add under `gnome/nostr-homed/tests/`:

- `unit/test_auth_challenge.c`
- `unit/test_auth_protocol.c`
- `unit/test_auth_vault.c`
- `unit/test_identity_store.c`
- `unit/test_identity_projection.c`
- `unit/test_identity_migration.c`
- `unit/test_local_home.c`
- `unit/test_smb_issuance.c`
- `integration/test_pam_broker.c`
- `integration/test_broker_peers.c`
- `integration/test_provider_local.c`
- `integration/test_provider_nip46.c`
- `integration/test_nss_readonly.c`
- `integration/test_smb_recovery.c`
- `acceptance/run_gdm_vm.sh`
- `acceptance/run_smb_clients.sh`
- `acceptance/run_winbind_vm.sh`

Tests use injected descriptors/configuration through test-only constructors or isolated VM filesystems. Never rewrite the developer host's `/etc`.

## 5. Risks and migration

### 5.1 Breaking changes and versions

This plan itself needs **no version bump**.

Implementation changes homed authentication behavior, account authority, configuration, and package contracts. Establish `nostr-homed` version **0.2.0** in both CMake and Meson, derived into pkg-config, and register it in `VERSION_MANIFEST.md`; document the incompatibility with the current 0.1.0 pkg-config declaration.

Do not alter release/tag columns without a release.

NIP-46 changes are additive APIs plus security fixes. Its supplied build has no authoritative version and the manifest does not list it; record that fact and validate repository-wide version ownership before merge. Do not invent a released version.

No libnostr or signer version bump is planned merely for consuming existing APIs.

### 5.2 Deployment sequence

1. Install new packages without activating PAM or changing NSS.
2. Keep break-glass local login tested and console accessible.
3. Back up legacy cache and account/home ownership records.
4. Generate migration report.
5. Enroll providers and provision/adopt local homes.
6. Validate authority/projection consistency.
7. Activate NSS in a snapshot VM, then canary host.
8. Activate pinned GDM/PAM integration.
9. Expand only after reboot, disable, cancellation, and recovery tests pass.

Never automatically copy a roaming filesystem into `/home` or unmount it during package installation.

### 5.3 Rollback

- First restore the previous distribution PAM/GDM configuration.
- Keep new NSS projection support installed while files still belong to Nostr UIDs.
- Disable the new authentication service; retain authority and home data.
- Do not downgrade to the old cache-presence PAM implementation as a login mechanism.
- Do not export newly authoritative accounts back into the legacy writable cache automatically.
- Preserve UID allocations; never recycle them on rollback.
- Schema readers reject unsupported future versions. Database rollback requires a compatible backup and an explicit offline recovery procedure.
- Samba rollback first disables issued accounts or stops the dedicated server; removing the issuance daemon must not leave unbounded valid credentials.

Preserve unrelated dirty work throughout; update this plan only through explicit reviewed revisions.

### 5.4 Principal risks

- **GDM input integration:** hidden input and cancellation must work with the distribution PAM stack; transient passphrase exposure inside trusted login processes is accepted and documented.
- **NIP-46 compatibility:** actual clients may require browser authorization or permit automatic signing.
- **Host root compromise:** root can replace policy, read broker input, or compromise provider execution; this is outside the login mechanism's protection.
- **Offline theft:** encrypted vault security depends on passphrase entropy and KDF cost.
- **Projection lag:** affects account visibility, but must never authorize login.
- **Samba password caching:** credentials remain reusable until disabled; active sessions require separate revocation.
- **Domain idmap changes:** can reassign filesystem ownership; treat range/domain changes as migrations, not routine configuration edits.
- **Greeter/package upgrades:** unsupported upstream versions must disable activation until the compatibility gate is rerun, not install untested PAM control flow.

## 6. Execution index and implementation order

Sizes are relative engineering packages, not calendar promises: S = a few focused days, M = roughly one engineer-week, L = several engineer-weeks including integration. Feasibility failures change estimates. Owner roles identify required expertise; implementation must assign named owners in beads.

| Item / owner | Goal | Done when | Key files | Dependencies | Size |
|---|---|---|---|---|---|
| P0 contracts / security + Linux | Freeze challenge, authority, PAM and lab contracts | Phase 0 reviews and pinned matrix recorded | proposed `docs/AUTH_PROTOCOL.md`, `IDENTITY_STORE.md`, `ACCEPTANCE_MATRIX.md` under homed | None | M |
| P0 pre-login IPC / Linux | Prove system broker and ordinary GDM prompts | Phase 1 peer/cancel/input gates pass | proposed `src/auth/auth_peer.c`, `auth_protocol.c`; existing `src/pam/pam_nostr.c` | Contracts | M |
| P0 proof + vault / security | Real bound proof, encrypted local keys | Phase 2 positive/negative/offline gates | proposed `auth_challenge.c`, `auth_vault.c`, `provider_local.c` | IPC, contracts | L |
| P0 external provider / Nostr | Correct transport and real device approval | Phase 3 admission/lifetime/client gates | `nips/nip46/src/core/nip46_session.c`, GLib wrapper; proposed `provider_nip46.c` | Proof contract; final gate needs verifier | L |
| P0 identity authority / Linux + storage | Stable ownership and read-only NSS | Phase 4 collision/migration/read-only gates | `src/nss/nss_nostr.c`; proposed `src/identity/*` | Contracts; activation needs providers | L |
| P0 local homes + PAM / Linux | Local sessions independent of roaming | Phase 5 crash/home/policy gates | `src/pam/pam_nostr.c`; proposed `local_home.c` | Proof, providers, identity | M |
| P0 GDM acceptance / QA | Prove real login, lock and recovery | Phase 6 VM matrix passes, no skipped required cases | proposed `tests/acceptance/run_gdm_vm.sh` | All desktop items | M |
| P1 standalone SMB / Samba + security | Nostr-authorized standard-client credentials | Phase 7 client/crash/expiry/revocation gates | proposed `src/smb/*`, dedicated Samba units/config | Proof/provider contract, independent server-authority spike | L |
| P1 domain desktop / Samba + Linux | Conventional Samba/AD login without cross-route fallback | Phase 8 join/idmap/GDM/TGT tests | proposed `docs/SAMBA_AD_LOGIN.md`, winbind sample/profile | Contracts; combined gate needs PAM routing | M |
| P0 release gate / release + independent reviewer | Native packages, safe activation/rollback, documented support | Phase 9 and every required pillar accepted | CMake/Meson, packages, version manifest, all acceptance suites | Every required item | L |

**Critical path:** contracts → pre-login/proof → identity + homes → actual GDM acceptance → combined release. External-client compatibility and Samba feasibility start immediately after contracts; domain integration and SMB server work can proceed independently, but neither is optional for full-goal completion. Do not wait until the desktop is finished to discover incompatible external-signer or Samba assumptions.

**Immediate priority:** reproduce the cache-only authentication and UID-overwrite cases in isolated tests; prevent installing the old PAM module in the candidate package. Implement new components behind opt-in build/package switches, and never enable a fail-only prototype on a real workstation.

### Additional required work packages

| Item / owner | Goal | Done when | Key files | Dependencies | Size |
|---|---|---|---|---|---|
| P0 Samba/domain feasibility / Samba + Linux | Resolve administration/SID, credential delivery and real domain dispatch before implementation | Phase 0b passes or affected pillar explicitly stops | Proposed standalone/winbind configs, adapter contract and acceptance matrix | Phase 0 | M |
| P0 roaming safety follow-up / homed maintainer | Preserve confirmed memory-safety defect outside login scope | Separate bead specifies moving relay cleanup after final secrets use and ASan regression; false mount success receives a separate child | `gnome/nostr-homed/src/ctl/nostr-homectl.c:309-314`, `nh_open_session` | None; excluded roaming package is not on local-home critical path | S |

The roaming follow-up is a mandatory tracker handoff when execution begins, not a claim that this plan repaired it. Do not distribute the experimental roaming package as production-ready while those defects remain.

### Detailed phase acceptance

Each phase is a proposed future tracker work package; split independent root causes into child beads as specified in the execution index. No issue IDs are assigned and no historical completion is implied.

## Phase 0 — Freeze contracts and acceptance environment

**Priority:** P0  
**Dependencies:** None.

Create versioned protocol/store/matrix documents that implement this plan; record any evidence-based deviations explicitly.

Freeze:

- Threat model and root trust boundary.
- Challenge schema.
- Socket protocol and PAM conversation/secret-cleanup contract.
- Provider profile and secret ownership.
- Database schema/migration policy.
- PAM routing graph.
- Exact platform versions.

**Exit evidence:** reproducible VM manifests, package hashes, protocol/schema review, and a trace demonstrating where the greeter secret travels.

## Phase 0b — Samba and domain feasibility gates

**Priority:** P0 prerequisite. **Dependencies:** Phase 0 contracts. **Owner:** Samba/Linux integration engineer.

Run before Phase 7/8 implementation, in disposable VMs. Prove standalone `tdbsam` account create/update/disable/re-enable and stable SID/UID inspection on the pinned package. Select one adapter: prefer the packaged Samba Python passdb binding if it exposes the required stable SID operations; otherwise freeze locale-independent `pdbedit` output fixtures and exact command versions. Record each exit status, stdin/secret handling and crash/timeout behavior. Verify generated account names cannot take over an existing Unix or Samba account.

Freeze the server authority HTTPS deployment, TLS certificate provisioning, narrow privileged account-mutation boundary and authenticated credential-delivery contract. Prove password rotation does not imply active-session revocation, then test dedicated-instance stop/restart, durable handles and watchdog failure. Stop this pillar if the dedicated-instance isolation cannot be guaranteed.

On the separate domain VM, prove join/trust, winbind NSS/idmap ranges, actual GDM PAM name routing, credential cache creation/cleanup and no cross-route fallback. Capture whether the selected pam_winbind configuration falls back from Kerberos to samlogon; do not claim Kerberos-only policy without demonstrating enforcement.

**Done when:** exact adapter and deployment contracts, package hashes, successful SID stability and domain login traces, negative tests, and accepted SMB password-mediated UX are recorded in the acceptance matrix. Phase 7 depends on the standalone results; Phase 8 depends on the domain results. A failed gate blocks its mandatory pillar rather than silently narrowing the release.

## Phase 1 — Prove pre-login broker and trusted input

**Priority:** P0  
**Dependencies:** Phase 0.

Implement the smallest broker/UI/PAM skeleton that can only fail authentication until proof verification exists.

Tests:

- Real GDM worker connects before user bus/home exists.
- Wrong UID, wrong PID, transferred descriptor, cross-transaction secret submission, and wrong target are rejected.
- Greeter cancellation, worker death, and broker restart invalidate requests.
- Private-key canary never enters PAM. Verify wiping in project-owned PAM response/copy buffers, broker and worker buffers on every completion/failure/cancel path. Passphrase canaries must not appear in logs, arguments, environment, PAM_AUTHTOK, core dumps or persistent files. GDM/gnome-shell internal heap copies are outside the project's wipe guarantee; document that residual exposure rather than asserting it was eliminated.
- Break-glass console login still works.

**Exit:** trusted input and peer-binding gate passes on the pinned GDM package. Otherwise stop the local-login deliverable.

## Phase 2 — Implement proof verifier and local provider

**Priority:** P0  
**Dependencies:** Phases 0–1.

Implement challenge verification, vault envelopes, local worker, and bounded cancellation.

Tests:

- Valid cryptographic proof succeeds.
- The old 128-character fake signature fails.
- Modified ID, content, tags, timestamp, pubkey, purpose, UID, or generation fails.
- Replays and post-cancel responses fail.
- Wrong passphrase/tampered vault fail identically to the user.
- No home, session bus, Secret Service, or network is available.
- KDF concurrency and memory limits are enforced.

**Exit:** 100 consecutive offline decrypt/sign/verify transactions; zero proof acceptance in negative cases; cancellation cleanup within three seconds.

## Phase 3 — Harden and prove NIP-46 provider

**Priority:** P0  
**Dependencies:** Phase 0 and verifier from Phase 2; may proceed independently of home provisioning.

Land lifecycle changes and tests before broker integration.

Tests:

- Named external signer, pinned capability profile.
- Both equal and different signer transport/user pubkeys; always resolve and verify the enrolled user identity independently.
- NIP-44-only transport.
- No-network, explicit denial, timeout, intermediate auth response.
- Wrong outer sender/recipient, invalid signature, duplicate and late response.
- Relay connects late, disconnects, reconnects, rejects publish, closes subscription, or requests AUTH.
- Concurrent cancellation and response dispatch under sanitizers.
- Reboot retains pairing but not proof state.

**Exit:** at least one named candidate completes actual pre-login approval; no mocks count as external-client acceptance.

## Phase 4 — Authority, migration, and NSS

**Priority:** P0  
**Dependencies:** Phase 0; provider enrollment validation from Phases 2–3.

Land authority, administration, projection, and NSS together as an opt-in package.

Tests:

- Two concurrent enrollments cannot acquire the same UID/name/key.
- Exhausted ranges fail without modifying another identity.
- Disabled users remain name-resolvable but cannot authenticate.
- Unprivileged NSS lookup creates no file, journal, schema, or network traffic.
- Missing/corrupt projection returns UNAVAIL, not a fabricated user.
- Migration refuses malformed, conflicting, or ambiguous legacy rows.
- Atomic snapshot replacement under concurrent readers.
- Disk-full failure preserves the previous valid projection.

**Exit:** authority/projection consistency report passes; rollback keeps UID ownership intact.

## Phase 5 — Local homes and PAM account/session integration

**Priority:** P0  
**Dependencies:** Phases 2–4.

Land home readiness, account policy, and new PAM session semantics atomically.

Tests:

- Kill after every provisioning phase.
- Existing symlink, mount, wrong owner, unexpected directory, permission failure, and label failure.
- Enrollment retries are idempotent.
- No FUSE device, Blossom server, Homed1, or user signer installed.
- Authentication failure never opens a session.
- Disable between proof and session prevents session opening.
- Closing one of two sessions does not remove the home or disrupt the other.

**Exit:** ordinary local-home GDM login, logout, reboot, and offline local-provider login work.

## Phase 6 — Real GDM release gate

**Priority:** P0  
**Dependencies:** Phases 1–5.

Exercise the actual installed PAM stack:

- Local-key and external-signer login, plus lock/unlock after idle lock and suspend/resume. Determine the pinned desktop's PAM worker credentials and service names; unlock must use a fresh proof, never an old login receipt. If unlock's caller is not privileged, design a purpose-limited same-UID reauthentication operation before release; do not open BeginLogin to arbitrary user.sock callers or fall back to a nonexistent Unix password.
- Provider choice
- Unknown and disabled Nostr users.
- Local/Nostr/domain name isolation.
- Broker/provider/UI crash.
- Greeter restart and system reboot.
- Console recovery.
- User home ownership and logind environment.
- No automatic GNOME Keyring unlock claim.

**Measurable exit:**

- 100 automated successful login/logout cycles per provider using a controllable cryptographic signer.
- At least 20 observed approvals using each claimed real external client.
- Zero authentication success across negative/fault tests.
- No lingering provider workers after cancellation.
- NSS warm lookup p95 below 10 ms and cold lookup p95 below 100 ms on the recorded reference VM.
- Local cryptographic login completion within five seconds after passphrase submission on that VM, excluding deliberate failure delays.
- External deadline and unavailable-state reporting remain bounded by the declared 180-second transaction limit.

## Phase 7 — Standalone SMB bridge

**Priority:** P1, required for the full shipping goal  
**Dependencies:** Proof/provider contracts and Phase 0b standalone Samba/authority gate.

Implement issuance journal, Samba account adapter, delivery client, expiry supervisor, and dedicated Samba service.

Tests:

- Explorer and GNOME Files access approved shares with generated credentials.
- Disallowed shares remain inaccessible.
- Password is independent of Nostr keys.
- Wrong server/account/resource proof fails.
- Delivery token mismatch and replay fail.
- Kill after every journal/passdb transition.
- Expiry and disable prevent new login.
- Existing-session behavior is recorded after password rotation.
- Forced restart disconnects all sessions and prevents revoked reconnection.
- Cached old credentials fail after invalidation.
- Supervisor failure stops the dedicated server within 30 seconds.

**Exit:** standard-client interoperability and crash/revocation evidence, not just an API-level issuance test.

## Phase 8 — Winbind domain desktop route

**Priority:** P1, required for the full shipping goal  
**Dependencies:** Phase 0 PAM/range policy and Phase 0b domain gate; independent of SMB credential issuance.

Implement pinned join/configuration documentation and integration profile.

Tests:

- Join, online domain login, actual TGT/cache behavior, logout cleanup.
- Domain account disable and wrong password.
- DC unavailable with offline login disabled.
- Optional cached-login mode tested separately and explicitly labeled.
- Same short name across local, Nostr, and domain users.
- UID/GID range overlap rejection.
- Domain home ownership and idempotent creation.
- Broken trust or winbind outage does not prevent Nostr-local or break-glass login.

**Exit:** actual GDM domain login through pam_winbind. No Nostr-generated Kerberos claim.

## Phase 9 — Packaging, rollout, and security review

**Priority:** P0 release gate  
**Dependencies:** All required product phases.

- Build both supported build systems in clean environments.
- Require auth/PAM artifacts rather than silently skipping them.
- Run sanitizer, unit, integration, GDM, SMB-client, and domain suites.
- Verify installed files, permissions, systemd sandboxes, AppArmor policy, and dependency separation.
- Test upgrade from reviewed legacy data and rollback from every activation step.
- Publish incident procedures for lost local passphrase, stolen external signer, broker compromise, SMB credential leakage, and broken domain trust.
- Complete independent security/peer review before any implementation push under repository policy.

**Evidence artifacts:** package manifests, redacted state-transition logs, PAM policy graph and installed configuration, real GDM recordings, signed negative-test results, Samba client traces, migration reports, crash-recovery matrices, and version/capability records.

The release is complete only when **both key providers, both Samba pathways, and ordinary local homes** pass their respective gates. A passing roaming/FUSE test is not evidence for any of them.

## 7. Integration contracts that must not be inferred

### Worker launch and secret lifetime

Use systemd transient services with explicit inherited descriptor support verified on the pinned systemd build, or broker-spawned workers with equivalent uid/cgroup/seccomp restrictions; Phase 1 must freeze one working launch mechanism. Merely adding template units does not pass the worker socketpair to a process. Verify descriptor ownership/closure, unprivileged worker identity, per-worker memory/CPU limits and cancellation kill/reap. The local worker must either retain the decrypted key in protected memory until the broker sends its immutable challenge, or receive the challenge before decrypting; freeze the latter sequence for v1: select provider → construct challenge → receive passphrase → decrypt/sign/wipe. Challenge and whole-transaction deadlines both still apply. Do not imply a worker can wipe a key before signing.

Apply local authentication throttling in authority-owned state: at most five failed unlock/proof attempts per account in 15 minutes, bounded per-peer/global concurrency, and no permanent attacker-triggered lockout. Use generic pre-auth account error text, with detailed audit visible only to administrators. Test restart persistence of rate limits, clock changes and denial-of-service effects. Account policy is rechecked before issuing the receipt and again before session opening; authority generation changes cancel pending work. Existing logged-in sessions are not silently terminated by account disable—document and provide an explicit administrator session-termination procedure.

### Public callback compatibility

The NIP-46 header currently promises caller-owned callback results while implementation frees them. Prefer repairing the implementation to honor the published ownership contract (heap-owned result/error transferred once, callers free) over silently changing the contract to borrowed. Phase 3 must enumerate exact callback callers and tests, fix literal error-string paths, NULL-callback cleanup and double-free/leak cases atomically. If compatibility evidence instead justifies a new borrowed-result API, add a separately named API and retain the old contract. This supersedes the draft's instruction to simply document the current freeing behavior; the mismatch and all lifetime tests remain required.

### SMB server deployment and failure boundaries

The HTTPS endpoint is a new dependency, not an existing repository capability. Phase 0b must select a maintained distribution HTTP/TLS stack and a bounded adapter; do not write an HTTP parser or expose a root account-administration daemon directly to arbitrary network input. Use an unprivileged HTTPS frontend and a credential-checked local privileged authority, with duplicate-key rejection, 64-KiB request limits, fixed API routes, no redirects, no permissive CORS, TLS verification and explicit certificate rotation/expiry tests. The privileged authority revalidates the complete proof and account/resource policy itself. Administrative enroll/disable/revoke operations are local root-only, not anonymous HTTPS routes. Extend §4.4 with the selected frontend unit/config and adapter source once the feasibility gate pins their actual interfaces.

Credential fetch/acknowledgement always rechecks account enabled state, current generation and delivery-token hash. Rotation, revocation or expiry invalidates older delivery tokens; no previously prepared credential may be returned afterwards. Serialize issuance per account and bind the password envelope's AEAD associated data to server authority, account, generation and expiry. A challenge is one-use and expires after 120 seconds; pending challenges from a previous server boot are invalid. Return generic denial for unknown mappings; apply rate limits before expensive verification without allowing an unauthenticated caller to hold the sole issuance slot indefinitely.

Samba command invocation must always select the dedicated configuration/passdb explicitly; ambient defaults must never target the host's unrelated Samba database. Set `old password allowed period = 0` where supported by the pinned version and prove an old password fails after rotation; do not assume it does from a successful command exit. Standard clients require SMB2/3; disable SMB1, guest fallback and plaintext authentication, require signing and encrypted shares for the selected matrix, and test downgrade refusal.

Use durable intent plus reconciliation for cross-database updates, not a fictitious atomic transaction across SQLite and Samba. Acknowledged/expired password envelopes must not remain recoverable from SQLite WAL, free pages or ordinary backups: encrypt each delivery record with a per-record key held only for the delivery/reconciliation window, destroy that key at completion, and define crash recovery to disable/reissue rather than decrypt an expired record. Persistent escrow needed for restart recovery is an explicitly reviewed alternative with its own erasure/backup policy; do not claim deletion of a SQL row erases plaintext recovery capability. No indefinite password archive is permitted.

The credential authority is separate from each workstation authority. Disabling a workstation account or replacing its Nostr key does not automatically revoke server credentials. The operator runbook must disable/rotate the independent server mapping and, if required, force-disconnect its dedicated Samba instance. Test that old-key proofs cannot authorize a newly mapped identity and that old issued credentials follow the documented server revocation policy.

### Desktop lock and scope

Screen lock/unlock is a required GNOME usability gate with both providers, including suspend/resume and signer unavailability. Pin the actual PAM worker service/credentials; use fresh reauthentication proof and never require opening another home session to unlock. A successful initial login alone is insufficient. Standard local break-glass console login remains mandatory. General SSH/sudo support and automatic GNOME Keyring or encrypted-home unlock remain excluded and must be stated in release notes.

## 8. Verification commands and evidence ownership

Run these only in disposable Linux checkouts/VMs during implementation, not on the planning host. The existing root option is `ENABLE_NOSTR_HOMED`; proposed component switches must be added and documented in Phase 0/packaging before using them. A successful default build with this option off is not evidence that PAM exists.

```sh
cmake -S . -B _build-login -DENABLE_NOSTR_HOMED=ON
cmake --build _build-login
ctest --test-dir _build-login --output-on-failure
ctest --test-dir _build-login -N
DESTDIR="$PWD/_stage-login" cmake --install _build-login
```

Verify test discovery includes every required new suite and staged installation contains the broker, NSS ABI soname, correctly named PAM module, configuration and system units. Required acceptance cases may not pass through a skip code. Missing PAM headers must fail an auth-enabled configure. Test the no-FUSE package separately after build separation; the current Meson standalone build is not accepted until dependency discovery is repaired.

```sh
meson setup _build-login-meson gnome/nostr-homed
meson compile -C _build-login-meson
meson test -C _build-login-meson --print-errorlogs
```

For the new tests, both build systems must expose the same test inventory. Run separate ASan/UBSan and TSan configurations supported by the pinned compiler; do not combine incompatible sanitizers. Retain reports for malformed protocol inputs, NIP-46 response/cancel races, NSS concurrent reads and enrollment crash recovery.

Installed-VM manual checks: `getent passwd n_test`, `id n_test`, real GDM login/logout and screen lock/unlock, `loginctl session-status`, and journal inspection with test secret canaries. On the domain VM use `net ads testjoin`, `wbinfo -t`, `getent passwd 'DOMAIN\\user'`, `id 'DOMAIN\\user'`, and `klist` inside the authenticated user's session. On the standalone server run `testparm -s` against the dedicated configuration, then exercise Explorer and GNOME Files plus `smbclient` using an interactive password prompt (never a password argument). Inspect `smbstatus` before and after disable/expiry/forced restart. Record exact invocation variants from the pinned distribution manuals in the acceptance matrix.

The proposed acceptance scripts in §4.6 must produce a machine-readable case inventory with pass/fail/skip, pinned versions, timestamps and redacted evidence paths. Package/release owners sign off the installed-system matrix; security review owns proof/admission/secret handling; Samba review owns credential and session revocation. No tests or builds were run in this planning session.

## 9. Remaining product decisions and stop conditions

1. **SMB credential UX:** proposed initial behavior is Nostr-authorized issuance of a dedicated password for standard clients, not passwordless SMB. Confirm this product contract before Phase 7 implementation. If passwordless SMB or unified Nostr-to-AD identity is required, stop that workstream and specify a separately reviewed KDC/certificate federation program; do not hide it inside this estimate.
2. **Support matrix:** Ubuntu 24.04/GDM, a standalone Samba server, Samba AD DC, Windows Explorer and GNOME Files are proposed targets. Freeze exact supported, security-maintained builds and at least one compatible external signer in Phase 0. Microsoft AD interoperability is not certified by a Samba AD-only lab; add a real Microsoft DC gate before making that claim.
3. **External signer availability:** pre-paired NIP-46 with explicit per-login approval is required. A candidate requiring arbitrary browser interaction at the greeter is not silently accepted. Local-key success cannot waive this gate.

These are bounded approval/feasibility gates, not unspecified implementation details. No additional user interview is required to finish this plan.

## 10. Source references and design rationale

- [Samba smb.conf](https://www.samba.org/samba/docs/current/man-html/smb.conf.5.html): encrypted SMB auth is not PAM auth; account/session restrictions and existing SMB connections are separate concerns. The standalone credential bridge is this plan's design inference, not a Samba-provided Nostr integration.
- [pam_winbind](https://www.samba.org/samba/docs/current/man-html/pam_winbind.8.html) and [idmap_rid](https://www.samba.org/samba/docs/current/man-html/idmap_rid.8.html): domain authentication/cache and stable mapping contracts. Pin their distribution versions; test documented Kerberos-to-samlogon fallback rather than promising Kerberos-only operation.
- [smbpasswd](https://www.samba.org/samba/docs/current/man-html/smbpasswd.8.html) and [pdbedit](https://www.samba.org/samba/docs/current/man-html/pdbedit.8.html): supported account/password administration tools. Select and fixture-test one SID-inspection mechanism during the Samba gate.
- [MIT PKINIT configuration](https://web.mit.edu/kerberos/krb5-latest/doc/admin/pkinit.html): certificate/private-key and KDC-trust requirements underpin exclusion of magical signature-to-TGT conversion.
- [NIP-46](https://github.com/nostr-protocol/nips/blob/master/46.md) and [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md): remote-signing transport and canonical event contracts. The private authorization statement is an application protocol, not an existing Nostr login standard.
- [Linux-PAM conversation contract](https://man7.org/linux/man-pages/man3/pam_conv.3.html): hidden input and response-buffer ownership support the standard-PAM choice; actual GDM behavior still requires VM acceptance.
- [Amber source](https://github.com/greenart7c3/Amber): external-signer candidate, not a claim of verified compatibility with this proposed login protocol.

### Baseline corrections retained for review

The planning export's supported current-state, designs, files, edge cases, sequencing and verification are retained. The following corrections are intentional; the bounded critique is retained at `docs/reviews/nostr-linux-samba-login-2026-09-20.md`. Its findings are dispositions, not evidence of runtime acceptance:

- Replace the invented no-passphrase-through-PAM constraint and custom GDM patch with standard hidden PAM input. Preserve private-key isolation, secret cleanup, provider choice, cancellation and real-prelogin coverage; remove only attachment-capability/greeter-fork machinery that this simpler design replaces.
- Separate the private authority directory from the world-readable NSS projection. The export placed both under a directory specified as both 0700 and 0755; the private subdirectory resolves that contradiction.
- Retain roaming defects as evidence and follow-up scope, not a requirement to repair unrelated roaming functionality before a local-home release. Prevent the legacy subsystem from writing the new authority without silently changing its excluded product.
- Qualify library race findings as integration hazards until reproduction; preserve the ownership and negative tests. Existing app bead closures are not proof that every library path is fixed.
- Treat historical hosted-signer names and platform versions as candidates, not current availability/support promises. Add real-client and maintenance-version gates.
- The scaffold was input, not an immutable second deliverable. Its evidence and decisions are integrated here; tool wrappers and selected-file listings are omitted. Review F1 is resolved by supersession, not by restoring a second plan or committing unrelated work.
- Review F2–F12 are incorporated: owned roaming follow-up, endpoint ACL matrix, clarified priority axis, scheduled Phase 0b Samba gates, correct skip-file attribution, equal-or-different signer-key handling, explicit single-candidate contingency, project-owned secret-wipe boundaries, provider-choice grammar, WAITING_INPUT state, and both build systems' PAM omission.
- The callback contract now preserves published caller ownership rather than changing it to borrowed. The SMB delivery design uses volatile per-record keys and disables ambiguous generations after restart rather than claiming SQL deletion erases recoverable secrets. Both corrections preserve the baseline's compatibility, confidentiality, crash-recovery and negative-test requirements with explicit behavior.

**Final fidelity check (2026-09-20):** checked every baseline current-state subsection, all 15 design subsections, existing/new file and test tables, migration/version/rollback risks, and Phases 0–9. Supported content is retained, augmented, or corrected with the rationale above; no implementation-bearing section was reduced to a summary. The export was retained through the single design critique and this check, then removed as superseded. The critique remains as the workflow review artifact. Only planning documents were produced; implementation and acceptance remain outstanding.

