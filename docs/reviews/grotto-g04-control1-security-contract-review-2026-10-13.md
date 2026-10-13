# G04 Control1 — focused security/contract review

## Context / Scope

**Verdict: REQUEST CHANGES for G04. Split `ab5794e91` out of the G04 branch/PR; retain its fixes in a separately reviewed Groundhog/CI change, not a revert.**

Reviewed `master` **`f050a6f624815409c0f964a3ed2cce7c92b56a0a`** against `origin/grotto/g04-control1` / worktree HEAD **`ab5794e9113582e2fec8a045f4a590527af5fdd0`**. Master is the merge base; the range contains exactly `70619bc63` and `ab5794e91`. All references below are to that branch snapshot.

Contract: `docs/plans/grotto-signer-ux-2026-10-12.md` §5.3, including the owner-confirmed amendment (lines 157–177); §5.4 client records (199); §5.10 client/configuration/activity storage (319); §5.12 pause rule (348); §9 `signer/clients` and `signer/activity` (532–533). Only the seven requested topics were examined. The amended G05/G06 bunker and G09 mnemonic/rotation deferrals are **not findings**. No implementation edits, tracker changes, commits, pushes or merges were performed; this report is the sole worktree change.

## Findings — severity ordered

### F1 — P2, CONFIRMED: corrupt/unsupported activity storage bypasses the in-memory retention bound

**Evidence:** `apps/grotto/src/event_history.c:883–888, 927–928, 968–970, 984–1014`; the timer also refuses to schedule when unwritable (`873`).

A corrupt/unsupported file makes `activity_writable=FALSE`. Each terminal request still inserts an entry, but `activity_save()` returns **before** `activity_prune()`. Without a `ListActivity` call, neither the 10,000-entry bound nor age cleanup applies to these accumulated records. Signing intentionally continues when logging fails, so this can turn a degraded logger into growing daemon memory use. The unsupported file itself correctly remains untouched.

**Local reproduction:** a temporary executable compiled the branch's actual `event_history.c`, loaded a version-2 envelope, then recorded 10,001 terminal snapshots. It printed `healthy=0 records-before-list=10001 sequence=10001`; only listing reduced the count to 10,000. This is not a hypothetical write-failure scenario.

**Recommendation:** enforce in-memory bounds independently of persistence availability and preserve read-only handling of unsupported files. Test repeated recording after both corrupt and unsupported-schema loads, with no intervening list calls.

### F2 — P2, CONFIRMED: permission-created clients never acquire operation metadata on automatic success

**Evidence:** `apps/grotto/daemon/grotto_control.c:115–123, 254–255`; `apps/grotto/daemon/grotto_clients.c:122–132`.

`SetPermissions` legitimately creates a known client, but passes NULL identity and records neither admission nor use. Its `first-consented-at` remains zero. Later automatically allowed calls are not explicit approval decisions, and `approval_changed()` updates successful calls only when `was_consented` is true. Consequently this supported creation path keeps an empty selected identity and `last-used-at=0` even after real successful operations. This violates the client-record truth required by §5.4; it does **not** accidentally elevate trust.

**Local reproduction:** a temporary private-bus probe used the real Control1 dispatcher to create an exact `get_public_key` grant for a known npub, then submitted the real shared-gate operation. The operation succeeded without a prompt; `ListClients` still returned `identity='' last-used-at=0 first-consented-at=0`. No test assertion was changed.

**Recommendation:** distinguish “record exists” from “Low-tier admission has been consented.” Update observed identity/transport/last use on successful calls for already-known permission-created clients without granting admission or creating records from arbitrary failures. Add a permission-created-client case to `signer/clients`.

### F3 — P3, PLAUSIBLE / conditional: revision checking is not a cross-process file transaction

**Evidence:** `apps/grotto/daemon/grotto_control.c:246–252`; `nips/nip55l/src/glib/signer_permissions.c:145–156, 425–430, 454–487`.

After the final reload/revision comparison, replacement copies and serializes the records before atomic rename. There is no file lock or disk-level compare-and-swap across that interval. A separate writer to the v2 file could commit an update in this window and have it overwritten without `Conflict`. Atomic replacement prevents torn writes, not competing-writer loss.

This is **not a demonstrated lost update between Control1 and ordinary `ApproveRequest(remember=true)`**: both mutate G03 permissions on the same owning main context, and the worker does not persist grants. The specified in-daemon race is protected (details below). The cross-process limitation is inherited from G03 and relevant only if independent writers are permitted. Do not call it a confirmed G04 authorization failure.

**Recommendation:** retain/document single-daemon ownership; if another v2 writer is supported, add a real transaction spanning reload/check/write and an interleaving test. Do not add locking merely to fix a race absent from the stated single-owner model.

## Authorization / information disclosure

### Every Control1 method

`apps/grotto/daemon/grotto_control.c:202–213` freshly identifies the bus sender and applies the shared check **before dispatch or unavailable-method responses**. Neither the approver's bus-name ownership nor a supplied label grants authority. Export completion re-attests again at `179–183`.

| Methods | Authorization and disclosure assessment |
|---|---|
| `GetStatus` | Same-UID caller allowed. Returned builder at `71–104` contains capabilities, epoch/revisions, paused/pending count, coarse bunker/key state and store health; **no identity, principal, request payload, key, pairing token or connect URI**. It constructs pending snapshots internally but returns only their count. |
| `SetPaused(true)` / `SetPaused(false)` | True is same-UID/open; false requires approver (`207–213`, `279–281`). This implements §5.12's asymmetry. |
| `ListRequests`, `ListClients`, `ListPermissions`, `ListActivity` | Approver-only (`218–239`, `282–285`). `ListRequests` intentionally returns authorized pending previews/reasons, not a content-free response: §5.3 requires full snapshots. No such data enters coarse status or broadcast signals. |
| `SetPermissions`, `RevokeClient`, `SetClientLabel`, `SetClientTrust`, `SetDefaultIdentityForLegacy`, `ClearActivity` | Approver-only (`240–296`); no secret-bearing result. Persistence errors are fixed messages, not forwarded file/backend/user text (`129–140`). |
| `ExportIdentity`, `ExportRawIdentity` | Approver-only on entry **and** delivery (`179–192`, `312–329`). Encrypted export defaults to NIP-49; `nsec` cannot be smuggled through it. Only the separate raw method intentionally returns a private key. |
| `ConfigureBunker`, `CreatePairing`, `AcceptConnectURI`, `CancelPairing`, `ResetBunkerEndpoint`, `RotateIdentity` | Approver-only even though unavailable (`297–332`); fixed typed errors, truthful false capabilities. No pairing token/raw connect URI is returned by these stubs. |

The canonical XML defines exactly these methods (`apps/grotto/data/org.nostr.Grotto.Control1.xml:9–37`). Signals carry numbers only (`grotto_control.c:28–46`), not request contents. Authorized request snapshots use inherited preview redaction for private-message/seal/gift-wrap/connect kinds and do not copy encryption/decryption arguments into previews (`nips/nip55l/src/glib/signer_approval_service.c:205–245, 408–448`). Relay endpoints in a future paired-client record are intended client metadata, not a raw pairing/connect URI.

**Important, CONFIRMED inherited platform limitation:** `signer_caller_is_approver()` accepts unattested same-UID callers on UID-only buses (`nips/nip55l/src/glib/signer_caller.c:364–375, 563–573`). Thus on that documented macOS compatibility path, an arbitrary same-UID process can use the new raw-export method; re-attestation does not repair this. Reliable approver isolation holds on PID-attested Linux, not universally. This follows the spec's explicit inherited exception (plan `145`, interface docs `201–206`), rather than a new Linux check bypass. Its consequence now includes private-key disclosure and must not be hidden behind an unqualified “exports gated” claim. The Linux impersonator test covers all methods and pause asymmetry (`test-control-untrusted.c:18–39`); its execution is skipped on macOS (`test-clients.c:218–230`).

### Exposure of the internal helper; export wiping

The helper's declaration/implementation already exists unchanged on master. Grotto directly includes `signer_caller.h` and links nip55l; its source include is PRIVATE (`apps/grotto/CMakeLists.txt:309–318`). No alternate approver algorithm, new public authorization wrapper or helper ABI change was introduced.

However, four formerly internal headers are newly **installed** under `libexec/grotto/private/nip55l` (`nips/nip55l/CMakeLists.txt:225–230`). The installed footprint has widened even though these are explicitly unsupported private interfaces, absent from PUBLIC include directories. Under AGENTS.md's broad installed-header definition this deserves surface/version accounting; “no installed-surface change” would be inaccurate. Separately, `nostr_nip55l_export_identity()` **is** a new supported C surface (`include/nostr/nip55l/signer_ops.h:166–176`), deliberately leaving approver authorization to the daemon. The manifest records its MINOR-class unreleased fold. This documentation-only review needs no bump.

Owned export passphrase/result copies are wiped on worker completion, successful delivery and destruction (including shutdown/auth-failure paths), at `grotto_control.c:161–192`. The custody exporter resolves into a secure buffer and frees it after encoding/encryption; error output is wiped (`nips/nip55l/src/core/signer_ops.c:805–833`). Exports create no grant/activity entry. D-Bus argument/reply buffers necessarily retain transient copies outside this wipe mechanism; docs explicitly acknowledge this (`docs/dbus-interface.md:100–105`).

**Plausible portability caveat, not a reproduced leak:** inherited resolver hex cleanup (`signer_ops.c:497–513`) and NIP-49 normalized-password/KDF cleanup (`nips/nip49/src/core/nip49.c:169–186`) use ordinary `memset`, not the project's non-elidable wipe primitive. Temporary Apple Clang and GCC 15 `-O2` assembly inspections retained these NIP-49 wipes on this machine; this is not a proof for every optimized/Linux build. Do not certify every intermediate buffer as universally securely erased. Delivery re-attestation, error-path cleanup and wiping still lack dedicated counterfactual tests.

## Client store and G03 revisions

- **Creation boundary passes**, apart from F2's observation bug: only explicit successful/backend-failed consent, successful already-admitted clients, remembered denials, or successful management permission creation reach record writes (`grotto_control.c:106–125, 254–257`). Arbitrary failed requests do not create records. Remembered denial/permission-only records do not imply Low admission.
- **Principal encoding / atomicity pass:** SHA-256 record IDs avoid principal-as-section injection; NIP-46 principals must equal the canonical helper result, and browser origins use the shared validator (`grotto_clients.c:17–31, 75–90`). Candidate copies are committed using consistent/durable 0600 replacement before in-memory state/revision changes (`38–49`). New directories are 0700; failed tier writes leave prior live authority unchanged. Existing directory permissions are not forcibly repaired.
- **Live trust wiring passes:** Control1 installs `grotto_clients_trust` into G03 (`grotto_control.c:359–360`). Unknown/invalid principals return Low **with consent false** (`grotto_clients.c:104–110`); G03 only auto-allows Low public-key reads when consent is true (`signer_permissions.c:365–372`). Successful tier changes call `signer_permissions_policy_changed()` (`grotto_control.c:264–266`), invalidating the admission revision; failed writes do not. The lowering test uses the actual gate/backend barrier (`test-clients.c:156–174`).
- **Conflict semantics pass for the specified writer model:** external token = randomized epoch base + G03's permission revision (`grotto_control.c:48–50, 343–349`), not `clients_revision`. Reload precedes the Control1 comparison; the delegated G03 replacement rechecks its own revision after its reload (`signer_permissions.c:458–459`). BUSY maps to typed Control1 `Conflict` (`grotto_control.c:129–140`). Remembered writes persist on owner-thread completion (`signer_approval_service.c:779–781`, `signer_permissions.c:443–452`), so they invalidate stale editor tokens rather than interleave inside a synchronous replacement. Tests cover stale replacement, remember mutation and a new epoch (`test-clients.c:41–60`).
- Array snapshots do not carry their revision. Clients must bracket `ListPermissions` with `GetStatus` and discard owner/epoch or revision changes, as documented (`docs/dbus-interface.md:107–120`). Reading a stale array and only then taking a newer revision is unsafe client behavior, not protected by expected_revision. No paired-snapshot GUI implementation is assessed here. F3 is the separate disk-writer window.
- The permissions/client files are not one transaction: permission persistence can succeed before client persistence fails (`grotto_control.c:251–256`). The error/revision and re-read requirement are honestly documented (`docs/dbus-interface.md:153–160`); do not assume rollback after `PersistenceFailed`.

## Activity and contract-test checks

Activity writing explicitly selects only request ID, transport, principal, public identity, operation, outcome, sanitized error code, sequence/timestamp and optional kind (`event_history.c:848–852, 984–1014`). It never serializes the full snapshot or imports legacy preview history. Error codes allow only bounded alphanumeric/dot/underscore names. No preview/reason/flow/URI/ciphertext/decrypted output enters this writer.

All terminal states map to the required outcomes, including last-continuation disconnect as `expired-caller` (`event_history.c:972–982`; `signer_approval_service.c:1031–1048`). Joined requests produce one terminal record; a surviving continuation prevents a false aggregate disconnect outcome. Pre-request rejections (already paused, oversized/invalid submission arguments) return before a Request exists and are not logged (`signer_approval_service.c:867–878`); existing tests do not establish universal failed-attempt auditing.

Healthy stores enforce age/count on save, load and list, with a one-shot age source (`event_history.c:858–880, 964–966, 1017–1020`). F1 is the unhealthy exception. Activity sequence is persistent and distinct from epoch-bound snapshot revisions; clear retains the high-water mark, so clear/prune may signal the same sequence (`grotto_control.c:33–49`; `event_history.c:891, 996, 1031–1040`).

Logging is observational: successful transport results are completed before terminal notification, and logging errors never replace them (`signer_approval_service.c:783–794`; `grotto_control.c:125`). Permission-persistence failure intentionally differs and can block remembered success (`signer_approval_service.c:779–781`).

**Contract-test fix passes:** the diff adds only two `seed_fixture_permissions` calls plus a comment (`nips/nip55l/tests/test_signer_dbus_contract.c:2393–2396, 2765`). No assertion, expected value, skip or failure path is removed/relaxed. The existing helper converts fixture wildcard allows to the fresh runtime identity and exact text-note scope (`433–451`), matching G03's identity-specific permissions. StoreKey-derived identity and restart identity assertions remain (`2413–2417, 2781–2785`). This is fixture provisioning, not weakening an assertion.

## `ab5794e91`: SPLIT, not revert

**Sanitizer check is correct for its stated guarantee.** `.github/workflows/groundhog-ci.yml:420–432` and `scripts/linux-gate.sh:234–242` compute `expected − registered` using sorted `comm -23`. A dropped **listed** test still appears in `missing` and fails. Additional fixture setup tests no longer cause a false failure. The actual CI execution retains the same anchored regex and skipped/not-run rejection (`groundhog-ci.yml:434–439`). This broadens tolerated extras, not omissions; it does **not** hide a removed registration while its name remains listed. Removing a name from both the list and registration could evade either old or new check; that is not a regression introduced here.

A local CTest listing selected `signer/clients`, `signer/activity` **and** their setup fixture. Applying the subset calculation yielded no missing test; deleting `signer/activity` from that registered result yielded precisely `signer/activity` as missing. This corroborates the logic, not the full Linux sanitizer job or the commit's specific CTest-3.28 history.

**DM free is correct.** `deferred_ids` owns keys with `g_free` and values with `job_free` (`gnome/groundhog/src/app/gh-dm-inbox.c:1626`). Both insertion paths allocate a separate key (`475, 553`); `Job.wrap_id` is separately owned and freed by `job_free` (`158–166`). `g_hash_table_iter_steal()` suppresses both destroy callbacks. The value moves to carry and only the independently allocated key is freed (`984–990`); later table destruction cannot free the removed key again. If carry is full, nothing is stolen/freed and table ownership remains intact. No double free with this steal.

These are justified Groundhog/CI fixes, but unrelated to G04's signer contract. **Move the whole follow-up commit to a separate Groundhog/CI PR; it need not be split internally or reverted.** This review does not certify the full Groundhog ASan/LSan suite as passing.

## Coverage gaps against §9

**Confirmed gaps, not proof that the uncovered production behavior is broken:**

- `signer/clients` enumerates transports/origins using fabricated `SignerCaller` structs and transport enum values (`test-clients.c:64–74`), not real NIP-5F peer credentials/browser-bridge assertions/NIP-46 authentication. Origin-separated row listing is checked, but authority isolation across origins is not tested through actual transport identification.
- “Identity switching” calls `grotto_clients_record()` directly with `npub-second-identity`, not a second real stored identity through the signer's selector/gate (`test-clients.c:89–96`). The real-daemon test checks only `GetStatus` (`238–246`). F2's management-permission creation → automatic-operation path is absent.
- `signer/activity` tests all six outcomes and a canary, pagination, clear/restart sequence, startup cap/age pruning and ordinary write failure (`test-activity.c:18–59, 64–106`). It does not append past the cap while an unsupported/corrupt schema is read-only (F1), nor exercise an idle retention deadline. Schema-load sanitation does not apply the writer's error-name character filter (`event_history.c:950–954`); tests do not cover that asymmetry.
- Outcome success/write-failure checks use `get_public_key`, not a verified sign-event result or real encrypt/decrypt result (`test-activity.c:23–24, 69–71`). The explicit logging-independence architecture is sound, but this falls short of directly proving a cryptographic result remains identical under failed logging. Invalid pre-request argument/pause rejection auditing is also untested.
- Export tests cover raw/encrypted success, default format/NFKC, rejected nsec/mnemonic/empty input and no activity entry (`test-clients.c:192–212`). They do not hold an export worker while the approver exits/execs, test shutdown delivery suppression, or inspect wipe paths. Linux-only method-denial checks were not exercised by this macOS run.

## Verification / Recommendations

Fresh temporary CMake build at `/tmp/g04-review.1e19ci/build`, **`NIP55L_SECRET_BACKEND=libsecret`**, test-trust overrides enabled; Groundhog disabled. No login-keychain backend test binaries were run and no developer keychain configuration was changed. Built `test-clients`, `test-activity`, `test_nip55l_dbus_contract` (and their dependencies).

- `ctest ... -R '^signer/(clients|activity)$'`: **3/3 CTest entries passed**, including setup; the Linux-only impersonator subcase is skipped on macOS.
- `ctest ... -R '^nip55l(_dbus_contract|_fake_secret_service_contract|_dbus_contract_fail_teardown)$'`: **4/4 passed**, including setup and the private fake Secret Service contract.
- Two additional temporary probes reproduced F1 and F2 against actual branch code.
- `git diff --check master origin/grotto/g04-control1`: passed.
- No full workspace gate, Linux caller-attestation acceptance or Groundhog sanitizer run was performed. Focused green tests do not negate the reproduced defects.
- Navigation-only Jev line triage: 25 requests, 203,389 input tokens, approximately $0.00854. Its answered-range suggestions narrowed long-file reads; findings were verified directly, and no relevance threshold determined an approval.

**Before accepting G04:** fix F1/F2 and add their regression cases; retain the documented single-owner/revision read discipline and explicit UID-only limitation. Keep the authorization checks, metadata whitelist and logging/result separation. Split the justified follow-up commit into its own component-scoped change. No expansion into bunker, GUI, Shell or custody redesign is requested.
