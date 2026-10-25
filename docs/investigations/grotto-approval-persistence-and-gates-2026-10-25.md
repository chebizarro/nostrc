# Investigation: Grotto approvals are not remembered and the two approval gates are confusing

Status: complete (2026-10-25). Read-only investigation; no source changes. Remediation tracked as nostrc-u6v55.17.

## Summary
Grotto *does* persist remembered approvals; it then deliberately refuses to use them for the kinds that dominate real client traffic. The evaluator checks the required-confirmation kind list (AUTH 22242, profile 0, contacts 3, relay list 10002, DM seal/wrap 13/1059, …) **before** it honours an explicit remembered allow or any trust tier (`nips/nip55l/src/glib/signer_permissions.c:420–421`), while the review dialog still offers "Remember" for those kinds and the daemon writes the row (`signer_approval_service.c:345–347, 954–962`). Two compounding defaults make the rest of the experience feel unremembered too: the Remember duration defaults to **600 s**, and the Full tier is defined as "everything that *reaches* the tier branch", so it can never cover the required kinds either. Four overlapping mechanisms (Remember, tier, required list, flow consent) with no explanation of precedence produce the "trusted for most messages or not" perception. Persistence, principals and revision fencing were tested by execution and are not at fault.

## Symptoms
- Owner report after using the new UX (master `9bb34e882`, G00–G14 landed): "nothing is ever actually saved or remembered from approval-to-approval" — each request prompts again even after choosing **Remember**.
- "The distinction between the two different approval gates is confusing: either an app is trusted for most messages or it isn't." The per-request **Remember** (scoped grant) and the per-client **trust tier** (Full/Medium/Low) coexist with a **required-confirmation kind** list and **flow-bound consent**; the net effect is a signer that is *more* intrusive than intended.
- Both must be fixed before Grotto can be released again (Preview 0.1.0).

## Initial hypotheses
- H1 **Policy, not persistence**: G03 made required-confirmation kinds (defaults: AUTH 22242, deletion 5, zap 9734, gift-wrap/DM kinds, contact list 3, relay list 10002, profile 0) bypass *remembered allows*, and Full trust also excludes them. Real clients emit AUTH on every relay (re)connect and kinds 0/3/10002 routinely → "Remember" is stored but dormant (G03 review F7) and no tier covers these kinds → every such request prompts.
- H2 **Principal instability**: the stored grant's principal (attested exe path+inode / `claimed:` / `nip46:`) differs between launches or between D-Bus senders, so lookups miss (e.g. Groundhog uses a private connection per operation; AppImage/relocated binaries; macOS UID-only bus where unidentified callers cannot receive remembered permissions).
- H3 **Persistence/evaluation bug**: grants written to `signer-permissions-v2.ini` are lost (write failure under profile guard / 0700 dir, legacy-file re-sanitisation on every load dropping rows, revision/invalidation cancelling, daemon restart with in-memory-only state) or the evaluator's join key (principal+identity+operation+argument digest) / identity pinning never matches a later request (identity resolved differently when selector empty).
- H4 **UX conflation**: the compact review shows "Remember" (legacy `ApproveRequest remember=true`, exact-kind) and "Change trust level" (Control1 tier) as separate actions with no explanation of which applies when; tiers auto-allow only the public-publishing set; so users cannot express "trust this app" in one place.

## Background / Prior Research
_(Phase 1.5 — git archaeology and design-decision trail)_

### Evaluation rules as they stand on master `fd3ad4308` (explore: nip55l archaeology)
- **Policy data** (`nips/nip55l/src/glib/signer_permission_policy.h:8–17`, unchanged since G03 `09749959f`): required-confirmation defaults = `22242` AUTH, `5` deletion, `9734` zap request, `4`, `13` seal, `14`, `15`, `1059` gift wrap, `3` follow list, `10002` relay list, `0` profile metadata. Public-publishing set = `1`, `6`, `16`, `7`, `30023`, `1111`. Header still says "Proposed §5.4 defaults awaiting owner sign-off" (`:5–7`) but the constructor installs them (`signer_permissions.c:215–223`); Grotto uses them when no user setting exists (`apps/grotto/daemon/grotto_control.c:115–135`).
- **Evaluation order** (`signer_permissions.c:397–435`): reload → scan rows (explicit deny wins; matching allows only set a flag) → invalid input → **`event` + required-confirmation kind ⇒ ASK, before the allow flag, first-party defaults or tiers are consulted** → missing principal / `connect` ⇒ ASK → matching allow or first-party default ⇒ ALLOW → tier rules → ASK.
- **Remember on a required kind**: the request is marked rememberable whenever a principal exists (`signer_approval_service.c:345–347`); `decide()` accepts Remember without checking required-kind membership (`:1159–1178`); the allow row is written after successful execution (`:954–962`); **on the next request the row is found and then overridden by the required-kind ASK** (`signer_permissions.c:413–427`). G03's own review recorded this as F7 "stored dormant" (`docs/reviews/g03-scoped-permissions-security-review-2026-10-12.md:90–95`) and it was never resolved. Only G11's ephemeral *flow* consent can bypass it (`signer_approval_service.c:790–800,1180–1198`).
- **Tiers** (`signer_permissions.c:416–435`): Full auto-allows everything that *reaches* the tier branch — i.e. never a required-confirmation kind, never `connect`, never conversation-key export; Medium = `get_public_key`/`get_relays`/`ping` + public-publishing kinds; Low = `get_public_key` only after consent.
- **Grant record** (`signer_approval_service.c:32–46,828–830,934–962`; `signer_permissions.c:58–65,115–127,166–176`): key = (requester principal, resolved npub or `""`, operation `"event"`|op string, kind or −1); decision/expiry not part of the key; TTL starts after execution; file `$XDG_CONFIG_HOME/gnostr/signer-permissions-v2.ini`, atomic 0600 write; a failed write fails the request *after* the backend ran (`:954–969`).
- **Nothing erases v2 rows** on revision bump, pause, or legacy re-sanitisation (`signer_permissions.c:211–254,307–317,343–372`; `signer_approval_service.c:1312–1322`); expired rows are merely inert.
- **Landing chronology**: G03 `09749959f` (2026-10-12) policy arrays + required-kind precedence + tiers; `7d3a98794` remediation; G11 `823925632`/`295a029b4` flow consent; no later commit changed the remembered-allow evaluation.


### Principal derivation and stability (explore: signer_caller trace)
- Principal formats (`nips/nip55l/src/glib/signer_caller.c:130–178,198–224,379–402,408–465,562–603`): `flatpak:<id>`, `app:<id>;exe:<path>`, `snap:<name>;exe:<path>`, `exe:<path>`, browser origin (via trusted bridge), `claimed:<app_id>` (non-Linux UID-only bus), `nip46:<hex>`; NULL = unidentified. **No inode in the persisted principal** — a rebuilt binary at the same path keeps its grants; only a changed exe path (AppImage relocation, launch context flipping `exe:` → `app:…;exe:`) changes it (`:422–453`). The inode check guards only bridge/approver recognition (`:334–359`).
- **macOS / UID-only bus**: callers become `claimed:<arg>` and **can** be remembered (`rememberable` only needs a non-NULL principal; `signer_approval_service.c:346–347,960–962`; `signer_permissions.c:95–111`) — documented exception in `docs/dbus-interface.md:269–281`. Linux UID-only callers get a NULL principal and cannot be remembered.
- **Identity pinning**: the grant stores the request's pinned npub (`signer_approval_service.c:828–830,931–933`); a later empty-selector request resolves the *current* default identity (`signer_ops.c:694–698,750–773`) — if the default changed, the exact-identity grant misses. Groundhog always passes an npub; gnostr derives one from its account; the **browser host forwards the page origin only on `signEvent`** — `GetPublicKey`/`GetRelays`/crypto calls carry the host's own principal (`apps/grotto/native-host/nm_router.c:64,67`, `nm_provider_nip07.c:150–193`).
- Groundhog's private connection per operation changes `conn_key`, not the principal (`gh-signer-nip55l.c:29,438–442`; `signer_service_g.c:139–150`); `EnableTypedApprovalErrors` is per-sender and irrelevant to matching. Joining is connection-scoped only for unidentified callers (`signer_approval_service.c:736–744,878–897`).
- **Conclusion for H2**: principal instability is not the mechanism for the reported symptom on Linux native installs; it is a secondary risk only for AppImage/relocated binaries, default-identity drift, and browser-bridge non-sign calls.


### What real consumers send (explore: operation inventory)
- **gnostr** (explicit npub on every SignEvent; `apps/gnostr/src/ipc/gnostr-signer-service.c:492–516,995–1012`): AUTH **22242** per relay challenge incl. after reconnect (`util/nip42_auth.c:73–107`; `nostr-gobject/src/nostr_relay.c:296–298,399–413`); profile **0** per save; contacts **3** per follow/unfollow; relay list **10002** per publish; deletion **5**; zap request **9734**; DMs: `NIP44Encrypt` ×2 + `SignEvent` **13** (seal) + **1059** (gift wrap, signed with the account) **per outgoing envelope** (`ui/gnostr-dm-service-send.c:612–642`; `util/nip59_giftwrap.c:255–265,298–323,381–415`); `NIP44Decrypt` ×2 per incoming wrap. Public set (1/6/7/1111) per user action.
- **Groundhog** (explicit npub; `gh-signer-nip55l.c:304–321,392–402`): `NIP44Encrypt` + `SignEvent 13` per destination per message incl. self-copy (`gh-nip17-envelope.c:24–39,195–200,237–257`; now covered by G11 flow consent), outer 1059 wrapped with a local ephemeral key; **10050** inbox list (not required), **10002** relay list (required), account AUTH 22242 only for own-inbox reads / own-list publication / self-wrap / groups (`gh-auth-policy.c:18–39`), ephemeral AUTH elsewhere; NIP-29 deletions 5; MLS key-package cleanup 5.
- **Desktop utilities** (`gnome/libnostr-publish/src/nostr-publish-signer-dbus.c:109–116,157–159` passes **empty identity** → resolves the daemon default): nostr-share 1/9/1063/30023/30617 + private share seals **13**; nostr-seal 1063 + Blossom auth; nostr-dav 31922/31923/30085 + deletions **5** + AUTH **22242** per challenge; nostr-settings 10002/10063 + AUTH **22242**; notify-daemon reads only.
- **Browser bridge**: page-driven pass-through of any kind (`browser-extension/nip07/src/page.js:58–68`; `nm_provider_nip07.c:162–192`), identity empty by default.
- **Net effect**: the kinds that dominate day-to-day use of a Nostr client — AUTH on every relay challenge, every DM, every profile/contacts/relay-list change — are *all* in the required-confirmation defaults, where neither Remember nor any tier applies. Only notes/reactions/reposts/comments can ever be auto-allowed, and only at Medium+ tier.

## Investigator Findings
### Execution scope and evidence (2026-10-25)

Investigated working tree based on `fd3ad43088435083e3f954367abb70474eec46c4`, Darwin 27.0.0 / arm64, GLib 2.90.0, GTK 4.24.1, libadwaita 1.10.0, libsecret 0.21.8.2. No product source was edited. The pre-existing icon/submodule/untracked-document changes were left alone. Tracking: `nostrc-jpr4g`; no component version bump (investigation/documentation only).

**Evidence boundary:** actual `grotto-daemon` process, real D-Bus adapter, production approval controller, real signing/encryption/decryption, private bus and throwaway profile. Native compact-dialog tests ran separately, including actual widget activation. This was not an interactive installed-client session, a GUI-to-daemon click-through, Linux attestation proof, or release qualification. The macOS branch of the repository's real-daemon integration test deliberately does not drive the GUI (`apps/grotto/tests/test-approval-integration.c:780–815,857–875`). Synthetic clocks below are explicitly labelled; no ten-minute wall-clock wait is claimed.

Commands used (repository cwd unless stated; tracker description abbreviated):

```sh
scripts/install-hooks.sh
bd onboard
bd prime
bd create 'Investigate Grotto approval persistence and gates by execution' --type task --priority 2 --description '...'
bd update nostrc-jpr4g --claim
cmake -S . -B /tmp/grotto-approval-investigation-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DENABLE_NIP55L=ON \
  -DNIP55L_TEST_TRUST_ENV=ON -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service \
  -DBUILD_GNOSTR=OFF -DBUILD_GROTTO=ON
cmake --build /tmp/grotto-approval-investigation-build --target \
  test_nip55l_approval_service test_signer_permissions test-approval-integration -j 6
ctest --test-dir /tmp/grotto-approval-investigation-build \
  -R '^(signer/permissions|signer/trust-tiers|nip55l_approval_service|signer/approval-integration)$' \
  --output-on-failure
cmake --build /tmp/grotto-approval-investigation-build --target test_nip55l_dbus_contract -j 6
cmake --build /tmp/grotto-approval-investigation-build --target test-approval-dialog -j 6
```

Configuration succeeded. It warned that `BUILD_GNOSTR`, `BUILD_GROTTO`, and `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND` were unused variables; Groundhog was not built. The effective NIP-55L secret backend was explicitly libsecret, not the login keychain (`nips/nip55l/CMakeLists.txt:48–89`). Builds succeeded, with Blueprint deprecation and duplicate-library linker warnings. No keychain default/search-list/settings mutation was performed.

Focused CTest output, retained in `/tmp/grotto-approval-focused-tests.log`:

```text
nip55l-test-homes                 Passed
signer/permissions               Passed
signer/trust-tiers               Passed
nip55l_approval_service          Passed
grotto-tests-test-homes           Passed
signer/approval-integration       Passed
100% tests passed, 0 tests failed out of 6
Total Test time (real) = 2.79 sec
```

Scratch evidence directory: `/tmp/grotto-approval-investigation/`. It contains the probe C files, compile/link command captures, Python build drivers, shell environment wrappers, and full logs. These are temporary evidence, not tracked test additions. `dbus-probe.c` includes the existing integration fixture with its original main renamed; its build driver derives the compiler/linker invocation from `ninja -t commands test-approval-integration`. `service-probe.c` similarly reuses the shared-service fixture. The private bus/lifeline helper is `tests/common/nostrc-test-bus.*`; the fixture's environment-key pattern is `nips/nip55l/tests/test_signer_approval_service.c:774–780`. Per-call consumer connections were retained by the helper until bus shutdown; changing connections did not change the observed principal.

Executed scratch commands:

```sh
python3 /tmp/grotto-approval-investigation/build-probe.py
sh /tmp/grotto-approval-investigation/run-probe.sh
# output: dbus-probe-identical.log; exit 0
python3 /tmp/grotto-approval-investigation/build-identity-probe.py
sh /tmp/grotto-approval-investigation/run-identity-probe.sh
# output: identity-probe.log; exit 0
python3 /tmp/grotto-approval-investigation/build-direct-probe.py
sh /tmp/grotto-approval-investigation/run-direct-probe.sh
# output: direct-probe.log; exit 0
python3 /tmp/grotto-approval-investigation/build-service-probe.py
TMPDIR=/tmp /tmp/grotto-approval-investigation/service-probe
# output: service-probe.log; exit 0
```

The shell wrappers set HOME and all XDG profile roots to scratch directories, `GSETTINGS_BACKEND=memory`, the scratch Grotto schema directory, and `NOSTRC_TEST_ISOLATION=1`. The daemon used public disposable fixture scalar 1; the two-identity run also used scalar 2 in the repository's fake Secret Service (`test_signer_dbus_contract --fake-secret-service plain <fixture-key-1> <fixture-key-2>`), on the same private bus. No real account secret was used.

### 1. Remember: actual second-request behavior and persisted contents

`dbus-probe-identical.log` records two byte-identical unsigned event requests for each kind, with fresh consumer connections but the same `claimed:probe-client` principal and identity A. Every returned signed event passed `nostr_event_check_signature()` and the expected-kind assertion. Crypto repeated the same plaintext/peer or ciphertext/peer, with successful decryption checked against `probe plaintext`. The named-principal crypto matrix uses the additive `NIP44EncryptForApp` / `NIP44DecryptForApp` variants; the direct methods were exercised separately below. D-Bus input signatures differ (`nips/nip55l/dbus/org.nostr.Signer.xml:122–146`).

Identity A throughout this matrix:

```text
npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
```

| Request / first decision | First request | Second identical request | Persisted allow after both calls |
|---|---|---|---|
| SignEvent 1, Remember Forever (`ttl=0`) | Prompt; approved | **No prompt**, signed | `event`, kind 1, expiry 0 |
| SignEvent 1, Remember default (`ttl=600`), after clearing the first row through Control1 | Prompt; approved | **No prompt**, signed | `event`, kind 1, expiry 1792890358 |
| SignEvent 22242, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 22242, expiry 0 |
| SignEvent 0, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 0, expiry 0 |
| SignEvent 3, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 3, expiry 0 |
| SignEvent 10002, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 10002, expiry 0 |
| SignEvent 13, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 13, expiry 0 |
| SignEvent 1059, Remember Forever | Prompt; approved | **Prompt again**, approved once | kind 1059, expiry 0 |
| NIP44EncryptForApp, Remember Forever | Prompt; approved | **No prompt**, encrypted | `nip44_encrypt`, kind −1, expiry 0 |
| NIP44DecryptForApp, Remember default | Prompt; approved | **No prompt**, plaintext verified | `nip44_decrypt`, kind −1, expiry 1792890358 |

All of these requests completed without a D-Bus/backend error. Required-kind second prompts occurred within the same second as their successful first approvals; they cannot be explained by expiration. Evaluation source agrees: explicit deny wins, then the required-kind branch returns ASK before remembered allow and tiers (`nips/nip55l/src/glib/signer_permissions.c:397–435`).

**Exact strings exposed by the daemon, not invented policy explanations:** pending metadata had `state='PENDING'`, `reason=''`, `reason_source='signer'`, `error_code=''`, `rememberable=true`; the service terminal snapshots had `state='SUCCEEDED'`. Explicit approvals emitted `ApprovalCompleted(request_id, true)`. Automatic allows had no approval prompt/completion signal; their Control1 activity row had `outcome='success'`, `error_code=''`. The snapshot's `reason` is caller context, not an evaluator explanation; this interface does not return a string such as “required kind overrides remembered allow” (`signer_approval_service.c:327–355`; `apps/grotto/src/event_history.c:850–860,982–1014`). The two-identity denial below returned the actual error text `GDBus.Error:org.nostr.Signer.Error.ApprovalDenied: user denied` and `ApprovalCompleted(..., false)`.

#### Captured `signer-permissions-v2.ini` after every matrix request

The full INI was printed immediately after **each** request. To avoid repeating nine largely identical files, the table below defines each full state from the literal final file printed immediately afterwards. `F(n)` means the header plus the first n complete record sections of that file. `F(1,forever)` is F(1) with its sole `expires-at` changed to `0`. Second calls left the entire captured INI unchanged. Hashes are the first 16 hex digits of SHA-256 of the captured INI block after stripping GLib's `# ` log framing.

| Calls, in execution order | Complete INI after first call | After second call | Captured INI SHA-256 prefix (both) |
|---|---|---|---|
| kind1-forever-first / second | F(1,forever) | same | `b47d972db769adc4` |
| low-after-first-consent GetPublicKey (single intervening call) | F(1,forever) | — | `b47d972db769adc4` |
| kind1-default-first / second | F(1) | same | `90fc6233b143f12e` |
| remember-22242-first / second | F(2) | same | `57601eae9848d1f0` |
| remember-0-first / second | F(3) | same | `4a5036e76ccb4345` |
| remember-3-first / second | F(4) | same | `52ee1a0cf745a2a5` |
| remember-10002-first / second | F(5) | same | `edf6dab3f106ea27` |
| remember-13-first / second | F(6) | same | `b74e362e7dae8a06` |
| remember-1059-first / second | F(7) | same | `29eb78bbe1e62c5d` |
| nip44-encrypt-first / second | F(8) | same | `4e2e84f377c81b07` |
| nip44-decrypt-first / second | F(9) | same | `303411aa155fb583` |

Literal F(9), also saved as `/tmp/grotto-approval-investigation/final-remember.ini`:

```ini
[permissions]
version=2
legacy-imported=true
legacy-complete=true

[record-0911271f36efa46e0ea362f95727fdb309010882c3aa245463a99c32ac406ce3]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=1
decision=allow
expires-at=1792890358

[record-9ad8c65bed4f495b91359148c7a1ae2185dc85b6d6643e5caf7c076b71d72bf8]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=22242
decision=allow
expires-at=0

[record-71de96f744500a0818ae6dfe95d3fac2bec4206895a8dcbb56f74e040d3f92f2]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=0
decision=allow
expires-at=0

[record-3758b37b786ebb165d68cff4b2dd73f11fc0ae339a0a35db2146c2b7e3f3b4a3]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=3
decision=allow
expires-at=0

[record-a7619d5a499541cdee7d87da696a4fbc18b1b6a34051505965c5df182a485d0b]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=10002
decision=allow
expires-at=0

[record-fe8e1101fc1e00a0821ab02f0c6e68d54372b871a27405dcb52bb288b67a1d8e]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=13
decision=allow
expires-at=0

[record-a3480320a00383ed16cfb60e337fefda769efb9b380467021753431651a0446f]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=event
event-kind=1059
decision=allow
expires-at=0

[record-976429d1afc05902a1e6456d14695ba8aebe94c09093eddee4e83d28f4a2403b]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=nip44_encrypt
event-kind=-1
decision=allow
expires-at=0

[record-7e51f272538945afbb9a30637d485fbf649429bd60878c230d0bbd3f53cb0083]
principal=claimed:probe-client
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=nip44_decrypt
event-kind=-1
decision=allow
expires-at=1792890358
```

For the tier matrix below, Control1 `SetPermissions(principal, identity, [], expected_permissions_revision)` cleared these explicit rows before each tier. The observed INI then contained only the `[permissions]` header; approve-once calls did not add rows. This prevents remembered rows from being mistaken for tier authority.

### 2. Default Remember really is 600 seconds

The native widget probe `dialog-default-probe.c` constructed the production dialog and its compiled Blueprint resource, populated ready daemon-shaped metadata, inspected the actual dropdown model, checked Remember without changing the dropdown, and clicked Approve or Deny. Both registered tests passed:

```text
DURATION 0 10 minutes
DURATION 1 1 hour
DURATION 2 24 hours
DURATION 3 Forever
DEFAULT selected=0 remember=0 label=Remember this decision for this scope
CALLBACK allow=1 remember=1 ttl=600
ok 1 /probe/remember-default-approve
CALLBACK allow=0 remember=1 ttl=600
ok 2 /probe/remember-default-deny
DIALOG_DEFAULT_EXIT=0
```

Source: initial dropdown selection and models at `apps/grotto/src/ui/approval_dialog.c:322–330`, Remember reset at `:518–524`, duration mapping `{600,3600,86400,0}` at `:124–145`. The controller forwards `(request_id, allow, remember, ttl)` to legacy `ApproveRequest` (`apps/grotto/src/approval-controller.c:767–772,799–806`); the D-Bus adapter passes TTL unchanged to the service (`nips/nip55l/src/glib/signer_service_g.c:286–308`). There is no separate implicit forever/default TTL in that adapter.

A **synthetic-wall-clock execution** in `service-probe.log` approved kind 1 with Remember/600 through the actual shared service. Its INI row had `expires-at=2000000600`, from fixture wall time 2000000000. Results:

```text
EVAL ttl-at-599 ALLOW
EVAL ttl-at-600 ASK
TTL600 next identical prompt=1
```

At exactly expiry the next identical service request announced `PENDING`; approving it once succeeded. The expired row remained on disk. New permission expiry is calculated as `now + ttl`, or zero for forever (`signer_permissions.c:671–681`); successful remembered approval writes after backend execution (`signer_approval_service.c:954–962`). Listing filters out expired rows (`signer_permissions.c:577–586`). Thus a user who checks Remember and returns after ten minutes **does get prompted again** at Low unless another effective allow covers the operation. This is a second, independent contributor to “nothing is remembered,” not an explanation for the immediate required-kind re-prompts.

### 3. What the saved-permissions UI actually reads and displays

**The current visible page is not on legacy ListGrants.** `page-client-detail.c:443–463` brackets Control1 `ListPermissions(principal)` with `GetStatus` and requires stable revision, epoch and service owner before accepting the rows. Control1 delegates to `signer_permissions_list()` (`apps/grotto/daemon/grotto_control.c:677–680`). The probe's actual ListPermissions response included kind 1 and every remembered required-kind row, each `decision='allow'`, `state='explicit'`, with the expiries above.

Rendering iterates all returned rows without a kind exclusion (`apps/grotto/src/ui/page-client-detail.c:410–427`). A kind-1 row is **“Allow · Text note (1)”**, with identity plus `Expires: <date>` or `Expires: Forever`. A required-kind allow uses exactly the same rendering. Only `state='needs-review'` gets **“Needs review · Not executable · ”**. `variant_row()` calls any non-review row `explicit`, not dormant (`nips/nip55l/src/glib/signer_permissions.c:565–575`). Therefore valid, unexpired kind-1 grants are visible on the selected client's Permissions page, but required-kind allows are displayed without an “ineffective/not in effect” marker. This is a source-grounded page trace backed by the actual Control1 row output, not a screenshot or an executed client-detail-page assertion.

`apps/grotto/src/signer_grants.c:168–178` still calls legacy `ListGrants`. Current source search found only its declaration, implementation and internal refresh-after-revoke reference (`signer_grants.h:24`; `signer_grants.c:82,168`); `main_app.c:8` only includes its header. The current client-selection route pushes `grotto_client_detail_new()` (`ui/signer-window.c:335–337`). No currently wired visible page was found calling the old list-box helper. Legacy listing really omits exact-kind rows (`signer_permissions.c:717–726`, `kind == -1` filter), but that is **not** why the present Permissions page would omit a kind-1 row.

### 4. Trust tiers: executed consumer-kind matrix

Set through **Control1 `SetClientTrust`**, not by writing client files. Each call succeeded, and ListClients confirmed the requested `full`, `medium`, or `low` value. Explicit rows were cleared before each tier; subsequent prompt approvals were once-only. `SetPermissions` replaces scoped rows, whereas `SetClientTrust` changes tiers (`apps/grotto/data/org.nostr.Grotto.Control1.xml:20–27`; `daemon/grotto_control.c:701–712`).

| Identical next request, absent explicit rows/flow consent | Full | Medium | Low after first consent |
|---|---|---|---|
| SignEvent 1 | No prompt | No prompt | Prompt |
| SignEvent 7 | No prompt | No prompt | Prompt |
| SignEvent 0 | **Prompt** | Prompt | Prompt |
| SignEvent 3 | **Prompt** | Prompt | Prompt |
| SignEvent 10002 | **Prompt** | Prompt | Prompt |
| SignEvent 22242 | **Prompt** | Prompt | Prompt |
| SignEvent 13 | **Prompt** | Prompt | Prompt |
| SignEvent 1059 | **Prompt** | Prompt | Prompt |
| NIP44EncryptForApp | No prompt | Prompt | Prompt |
| NIP44DecryptForApp | No prompt | Prompt | Prompt |
| GetPublicKeyForApp | No prompt | No prompt | **No prompt** |

Every permitted/explicitly approved request in this matrix returned success. This directly contradicts interpreting Full as “no more confirmations for this app”; it matches `signer_permissions.c:419–435`.

**Low consent is actually recorded.** Immediately after the first kind-1 approval, ListClients reported `trust-tier='low'` and nonzero `first-consented-at`; a GetPublicKeyForApp call then completed without prompting. A separate fresh direct-method run observed `GetPublicKey: prompt=1`, approval **without Remember**, then `GetPublicKey: prompt=0`; its INI still contained only the version header. It is durable client admission, not a remembered get-public-key permission. The production observer records explicit consent on SUCCEEDED or FAILED and invalidates permission policy (`grotto_control.c:213–225`); `grotto_clients.c:121–127,137–150` derives `consented` from the first-consent timestamp. The Low exception is `signer_permissions.c:434`. The FAILED branch was read, not separately reproduced here.

### 5. Compact-review vocabulary and next-request honesty table

Exact English strings are from the current Blueprint/implementation, not the older plan. The compact review offers **“Deny”**, **“Approve”**, **“Not Now”**, **“Remember this decision for this scope”**, and the duration choices above. Details contains **“Change Trust Level…”** and the explanation **“Changing trust is separate from remembering this decision. Lowering trust keeps explicit permissions.”** Tier choices are **“Low — ask for every request”**, **“Medium — ask for sensitive operations”**, **“Full — auto-sign permitted requests”** (`data/ui/approval-dialog.blp:67–85`; `src/ui/approval_dialog.c:322–330`). The Low label has the observed GetPublicKey exception.

For multiple grouped requests there are **“Previous”**, **“1 of 1”**, **“Next”**, **“Requests in this flow”**, row checkboxes, and **“Approve Selected Once”**; selections initially are empty (`approval-dialog.blp:41–57`; `approval_dialog.c:409–459`). Room-flow controls start unchecked:

- **“Allow NIP-44 encryption for this client and identity for 10 minutes”**.
- **“Allow encrypted-message seals within this flow's limit (up to 10 minutes)”** when kind 13 is required.
- Otherwise **“Allow encrypted-message seals for this client and identity for 10 minutes”**.

These appear only for a ready, rememberable `nip17-room-send` request with nonempty flow ID, size 1–1024, and encrypt/seal operation (`approval_dialog.c:525–542`).

The following next-request statements assume the same principal, resolved identity and operation/kind, before TTL expiry, with no intervening policy change. For once-only actions, the existing effective tier/rows still apply; “once” does not revoke old grants.

| User action / state | Daemon action now | Next identical request |
|---|---|---|
| Approve, Remember unchecked | ApproveRequest(true,false,0); executes once | Re-evaluates existing policy. At ungranted Low, prompts again except GetPublicKey after consent. |
| Approve + Remember, 10 minutes / 1 hour / 24 hours | Stores allow for principal + identity + operation/exact kind after successful backend execution; TTL 600 / 3600 / 86400 | Ordinary scopes auto-allow until expiry; **required kinds still prompt immediately**. |
| Approve + Remember + Forever | Extra **“Remember Forever?”** confirmation with **“Cancel” / “Remember Forever”**; confirm stores expiry 0 | Ordinary scopes auto-allow indefinitely; required kinds still prompt. Cancelling the confirmation submits no decision. |
| Deny, Remember unchecked | Denies current request only | Existing policy is unchanged; ordinarily prompts again, but a pre-existing effective allow can still auto-allow. No new Low admission. |
| Deny + Remember, any duration including Forever | Stores explicit **deny** for that scope | Denied without prompting until expiry/revocation, including required kinds and Full clients. The **“Deny once (Ctrl+D, Escape)”** tooltip is inaccurate when Remember is checked; native probe emitted `(allow=0,remember=1,ttl=600)`. |
| Not Now | Defers/closes review, withdraws individual notification; **no decision RPC** | Current request stays pending until answered/expiry. An identical outstanding call may join it; a later fresh call has no new grant. |
| Ordinary close/Escape, with decision callback still installed | Deny once | No new grant; same qualification as Deny once. |
| Change Trust Level… | Separate confirmation **“Change Client to low/medium/full Trust?”**, **“Cancel” / “Change Trust Level”**; calls SetClientTrust; does not itself approve the current request | Uses tier matrix above; existing explicit allows/denies remain. Lowering trust does not remove them. |
| Approve Selected Once | Approves only eligible checked pending requests, no Remember | No new saved permission; next request re-evaluates policy. Unselected requests remain waiting. |
| Approve with encryption flow checkbox selected | ApproveFlowRequest(...,encrypt=true,...,600); durable `nip44_encrypt` allow | Later encryption for same principal+identity is allowed across flows/peers until consent-time + 600; no decrypt grant. |
| Approve with seal flow checkbox selected, kind 13 required | Bounded in-memory flow consent, not a durable kind-13 allow | Allows eligible remaining slots only in that same flow/identity/transport and first-arrival window; other flow/identity/transport prompts. |
| Approve with seal checkbox selected, kind 13 not required | Durable exact-kind-13 allow | Same principal+identity can sign seals outside that flow until consent-time + 600. |
| Remember checked as well as a selected flow checkbox | Flow path takes precedence; controller sends fixed TTL 600 | Flow rules above, **not** the ordinary duration dropdown's selected TTL. |
| “Block” | **No such control exists in this compact dialog** | There is no separate compact-dialog client-wide Block operation. Remembered Deny is scope-specific. |

Source for action dispatch/confirmations: `approval_dialog.c:88–174,205–257`; controller `:767–806,847–907,962–974`. Expiry of an unanswered pending request is normally 300 seconds, separate from Remember's 600-second default (`signer_approval_service.c:704–710,1120–1122`).

Native execution: the existing `test-approval-dialog` suite ran with `GNOSTR_SKIP_UI_TESTS` unset, native GDK backend and GTK animations disabled, under `/tmp/grotto-approval-investigation/private-bus-run` (a scratch wrapper around `nostrc_test_bus_up/down`). All **17** tests passed, including approve-once, deny-once, close-denies, not-now, fixed-identity-remember, batch-selection, flow-unchecked/encrypt-only/seals-only/deny and pairing-preview. Output: `dialog-native-private-bus.log`, wrapper exit 0. The additional **2** native default-duration tests above passed in `dialog-default-probe.log`. This proves actual widget behavior; the next-request semantics combine daemon executions above with the cited dispatch/evaluator paths, not a claim that every honesty-table combination was independently clicked through against the daemon.

### 6. Secondary execution findings

#### (a) Empty selector: two live disposable identities reproduce the miss

`identity-probe.log` used both stored fixture identities on the private fake Secret Service. B was:

```text
npub1ccz8l9zpa47k6vz9gphftsrumpw80rjt3nhnefat4symjhrsnmjs38mnyd
```

Observed sequence (all successful except the deliberate deny):

```text
default-A-first                         prompt=1; approve + Remember Forever
default-A-second                        prompt=0
SetDefaultIdentityForLegacy(B)          succeeded
default-B-first-deny-once               prompt=1; ApprovalDenied: user denied
default-B-again-approve-remember         prompt=1; approve + Remember Forever
default-B-remembered                    prompt=0
pinned-A-while-default-B                prompt=0
SetDefaultIdentityForLegacy(A)          succeeded
default-A-return                        prompt=0
```

The first B request's metadata pinned B while the INI still contained only A's allow. Deny once left that file unchanged. Remembering B added a second kind-1 allow with B's exact npub and expiry 0; the A row remained unchanged. Explicitly selecting A while B was default used A's old grant. This isolates **identity resolution**, not principal drift or grant loss. The empty-selector utility call is real (`gnome/libnostr-publish/src/nostr-publish-signer-dbus.c:109–116`); configured default substitution precedes key resolution (`nips/nip55l/src/core/signer_ops.c:694–698`), and normalization resolves empty selectors to a pinned npub (`:750–773`). A different default identity therefore legitimately selects a different grant scope.

#### (b) Connect: stored allow is not pairing authority

The shared-service probe used a NIP-46 caller and `no_identity=true`, approved connect with Remember Forever, and observed `result='ack'`. The persisted record was:

```ini
[record-11bbff2e106217e3804c9d53f5bc9207127bba6bb291b5ca914694d5ef0ade33]
principal=nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
identity=
operation=connect
event-kind=-1
decision=allow
expires-at=0
```

The next identical unpaired connect **prompted again**. Denying it returned `org.nostr.Signer.Error.ApprovalDenied`; terminal snapshot was `state='DENIED'`. Passing the production context flag `existing_pairing=true` then returned `ack` with **no additional prompt**. Thus a remembered connect allow is ineffective by itself, while explicitly paired reconnection has a separate service exception (`signer_permissions.c:425–427`; `signer_approval_service.c:790–800`). This was a service/backend execution, **not a relay-backed NIP-46 pairing session**.

#### (c) Flow persistence and the two different expiry anchors

The controlled-clock evaluator used principal `exe:/probe/flow`, identity `identity-A`, transport 0, flow size 2; first arrival at monotonic 1000 s / wall 2000000000 s. Consent was deliberately delayed **120 s**, then both encryption and seals were selected with TTL 600.

With the **default required-kind policy**, actual results were:

```text
FLOW initial=ASK
FLOW default-required durable=[nip44_encrypt, kind=-1, allow, expires-at=2000000720, state=explicit]
FLOW same=ALLOW other-flow=ASK other-transport=ASK other-identity=ASK
EVAL encrypt-different-flow-no-context ALLOW
FLOW at-first+599=ALLOW
FLOW at-first+600=ASK
EVAL encrypt-at-first+600 ALLOW
EVAL encrypt-at-consent+600 ASK
```

**No durable kind-13 row was written in this default-required case.** Encryption's durable key omits flow ID, peer and transport; its expiry is wall-clock **consent time + 600**. Required-seal authority is in-memory, scoped to principal + identity + transport + operation + kind + flow ID, with the size/request-slot budget, and expires at **first arrival + 600**, leaving only 480 s after this delayed consent. Source: keys/observed window `signer_permissions.c:437–441,500–502`; admission budget `:531–539`; persistence and deadline assignment `:623–655`. The header's unqualified “TTL is measured from first request arrival” (`signer_permissions.h:54–59`) does not describe the durable encryption row's actual expiry calculation.

A second controlled run removed kind 13 from required confirmation before consenting. It wrote **both** durable `nip44_encrypt` and `event/kind=13` allows with `expires-at=2000001720` (wall consent 2000001120 + 600). Seal evaluation **without any flow context** then returned ALLOW; the other identity returned ASK. Therefore the hypothesized durable kind-13 flow-consent row is conditional on kind 13 **not being required**, not what the default policy writes. No decrypt permission is created by these flow controls.

#### (d) macOS direct calls have a different, shared empty-claim principal

The direct-method run uncovered an additional platform-specific scope distinction. Named SignEvent / ForApp calls used `claimed:probe-client`, but literal `NIP44Encrypt`, `NIP44Decrypt`, and `GetPublicKey` calls on this machine produced **`principal='claimed:'`**, not NULL; `verified=false`, `rememberable=true`. A pre-existing named ForApp encryption grant did **not** cover the direct encryption call. Approving direct encryption and decryption with Remember/600 created separate `claimed:` operation rows, and each second direct call auto-allowed. This is persistence working with a different principal, not a failure to save. A subsequent extension (`direct-probe.log`, exit 0) explicitly set the named `claimed:probe-client` to Full; direct empty-claim encryption still prompted. After clearing the empty-claim explicit rows and setting **`claimed:` itself** to Full, direct encryption and decryption both auto-allowed; setting that same principal to Medium made both prompt. Thus the crypto tier results are also confirmed for the literal three-string methods, provided trust is assigned to their actual principal.

The initial direct persistence capture is retained as `direct-probe-before-trust.log`; its exact direct-call INI progression was: header plus the named `record-976429d1afc05902a1e6456d14695ba8aebe94c09093eddee4e83d28f4a2403b` encryption/Forever row printed in F(9), then the first record below after direct encryption; direct decryption appended the second record. Each second direct call left the complete file unchanged. Captured INI hash prefixes were `21ebbe5b2b80756d` after each encryption call and `b260102bd97ab3c5` after each decryption call. Both direct first decisions used Remember/600.

```ini
[record-659f6a9c762f9f781a479018f741ec7feb051c4d8d264bccb90b0edb33c75e6e]
principal=claimed:
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=nip44_encrypt
event-kind=-1
decision=allow
expires-at=1792890557

[record-e86bda4a504b80cf2dc23938d80e150677b6de11f2954bf838df694151e520ea]
principal=claimed:
identity=npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d
operation=nip44_decrypt
event-kind=-1
decision=allow
expires-at=1792890557
```

The fresh direct GetPublicKey run also went from prompt to no prompt after approval without Remember, recording Low's first-consent timestamp while leaving the permission file header-only. Source: UID-only platform exception `signer_caller.c:562–573`; the adapter substitutes even an empty claimed string `signer_service_g.c:140–144`; claim principal construction `signer_caller.c:394–400`. All claimless operations on this UID-only path use that same empty-claim principal, not per-executable identity. The daemon itself logged:

```text
nostr-signer: the bus does not report caller PIDs on this platform; callers are identified by their claimed app_id only and any same-user process may answer approvals
```

This is a macOS authority/UX constraint, not proof of the same behavior on Linux. The real utility encryptor uses the direct method (`gnome/libnostr-publish/src/nostr-publish-signer-dbus.c:157–159`); Groundhog also selects direct NIP44 method names (`gnome/groundhog/src/identity/gh-signer-nip55l.c:392–395`). Whether any particular installed client changes between a named and empty claim must be checked at that client's call site; it is not inferred for every consumer.

### 7. Existing assertions that encode the current policy

- **G03:** `test_tiers`, registered as **`/trust-tiers/live-policy`** (`nips/nip55l/tests/test_signer_permissions.c:525`). After setting Full at `:344`, the loop writes a remembered allow for **every default required kind** at `:357–359` and explicitly **asserts ASK at `:360`**. It then writes deny and asserts DENY at `:361–362`. A custom-required kind 1 plus remembered allow, including first-party default, is asserted ASK at `:370–373`. This test really encodes required-kind-overrides-Remember-and-Full; the executed `signer/trust-tiers` CTest passed it.
- The same test also asserts remembered connect stays ASK (`:349–352`) and explicit conversation-key allow is separate from Full (`:347,353–355`). These are distinct policy ceilings, not evidence that Full is universally ineffective.
- **G07:** `run_flow`, `apps/grotto/tests/test-approval-integration.c:609–612`, asserts a remembered kind-1 request returns without another pending prompt. Its required follow-list request at `:692–695` is a **different scope**, not a same-kind remembered-allow override assertion. The GUI helper changes to **Medium**, verifies Medium and continued pending at `:562–563`; that is not Full-plus-Remember coverage. No same-kind required-allow precedence assertion was identified in this bounded G07 spot-check. On this machine that GUI-only helper was not executed by the real-daemon integration test.

### Constraints established for any later fix (not redesign recommendations)

- The required-kind allow rows above are real persisted state, including expiry 0, and are rendered as ordinary explicit allows. Changing evaluation precedence can make these **previously dormant allows become executable without a fresh user decision**.
- Full clients currently do **not** auto-authorize required kinds. Removing that ceiling would silently expand existing Full clients' authority even where no explicit row exists. Medium's allow set is a different contract.
- Explicit denies currently win over remembered allows, tiers and required-kind ASK. Lowering trust retains explicit rows. Any change must account for those existing persisted meanings, not treat tier changes as row deletion.
- Identity-specific grants must not be mistaken for grants to a newly selected default identity. Paired reconnect is not authority from the stored connect row. Required-seal flow budgets are not equivalent to a durable, identity-wide seal allow.
- Remember's default expiry, pending-request expiry, ephemeral flow expiry and durable flow-encryption expiry are four separate timings. The executed flow case demonstrates they do not all start at consent or all start at first arrival.

### Source-targeting aid (not behavioral evidence)

Source discovery used three read-only explore probes plus Jev `find-lines`, `filter-search`, and `verify-claim`. Root-session usage was 89 requests / 774,598 input tokens (excluding explore-child usage). A page-client-detail verification score of 0.41 was inconclusive, so its load-bearing lines 410–465 were read directly. A target-name search reduced 5,356 hits to 12 candidates and identified `test-approval-integration`; source ranges and executions, not model relevance scores, support the findings.

### Preserved failed setup attempts and remaining limits

These are diagnostic failures, not silently converted into passing tests:

- First scratch build command named nonexistent `test-grotto-approval-integration`; Ninja refused with `unknown target`. Correct repository target is `test-approval-integration`; the initial refusal is retained in `/tmp/grotto-approval-build.log`.
- First custom probe launch used the real HOME; the isolation guard aborted with **“HOME is the real home /Users/bizarro; refusing to run against the real profile”** before bus/backend work. Later launches used explicit scratch HOME/XDG roots.
- Initial AUTH fixture omitted the required relay tag. The controller refused approval with **“Request cannot accept this decision.”** The preserved `dbus-probe-invalid-auth-fixture.log` records this. The corrected fixture supplied relay/challenge tags; reaction fixtures supplied an event reference. This is the preview guard working (`approval_dialog.c:462–498`), not a permission persistence failure.
- Initial custom crypto tuple incorrectly used four strings with the three-string direct method; the daemon returned **InvalidArgs: Type of message, “(ssss)”, does not match expected type “(sss)”**. That log is `dbus-probe-wrong-crypto-signature.log`. Correct named and direct signatures were subsequently exercised separately.
- Initial service probe tried to fetch metadata after terminal cleanup; the probe stopped after printing its first INI. Its partial output is `service-probe-terminal-info-error.log`; the corrected probe retained the announced pending snapshot and completed with exit 0. This was a scratch-harness lifetime error, not a daemon finding.
- The first native-dialog launch via stock `dbus-run-session` failed before the test with **“Failed to start message bus: Check-in failed: No such process”**, exit 127 (`dialog-native.log`). Running through the repository's helper-managed private bus succeeded; the failed receipt remains.
- No hosted CI, installed utility/relay session, ten-minute real-time wait, daemon-restart UI session, client-detail screenshot, or relay-backed connect handshake was run. Native dialog tests and the real-daemon/controller tests are separate proofs; neither is labelled an end-user release acceptance run.


## Investigation Log

### Phase 2 — context_builder / oracle initial assessment (chat `grotto-signer-ux-plan-DCF6A3`)
**Hypothesis:** H1 (policy precedence) is the primary cause; H2/H3 secondary.
**Findings (oracle, static, 12 files):** confirms the precedence chain `signer_permissions.c:397–400 → 413–417 → 420–421 (required ⇒ ASK before allow flag) → 427 → 429–435`; rememberability is principal-based only (`signer_approval_service.c:345–347`); dialog submits Remember+TTL without policy awareness (`approval_dialog.c:112–149`); allow persisted after execution (`:954–962`, scope `:828–830`). New points: (a) `connect` Remember rows are written but pairing is governed by separate admission state (`signer_permissions.c:425–427`; `signer_approval_service.c:796–799`); (b) room/flow consent also writes **durable** `nip44_encrypt` and (when 13 is not required) kind-13 allows with consent-time expiry while the ephemeral seal authority uses first-arrival expiry (`signer_permissions.c:624–655,641–654`); (c) the legacy `list_grants()` listing omits exact-kind event rows — tests expect this — so a management surface using it would show remembered grants as absent; (d) default Remember TTL is 600 s and expiry is not extended by use; (e) Full trust is broader than the public set (any non-required valid kind incl. custom) but can never cover required kinds; (f) dormant required-kind rows are listed as plain `"explicit"` with no "not in effect" marker (`:566–576`), creating a latent hazard: removing a kind from the required list silently activates them. Secondary hypotheses assessed: principal change via private connections — not demonstrated (`:1071–1073`); default-identity drift — genuine conditional miss; write failure — surfaces as FAILED after the backend ran (`:959–962`); legacy sanitation / revision fencing — not causes (`:344–373`, `:990–1010`).
**Conclusion:** H1 confirmed statically; execution proof delegated to the pair investigator; migration hazard noted (do not simply reorder the check — dormant allows would go live).


## Root Cause

**RC1 — Required-confirmation precedence silently voids explicit remembered allows (proven by execution).**
`signer_permissions_evaluate()` scans the v2 rows and merely sets an `allow` flag (`signer_permissions.c:413–418`), then returns `ASK` for any `event` whose kind is in the required list *before* that flag is consulted (`:420–421`); `allow || first_party_default` is only reached afterwards (`:427`), and tiers after that (`:429–435`). Defaults (`signer_permission_policy.h:8–17`): 22242, 5, 9734, 4, 13, 14, 15, 1059, 3, 10002, 0 — still labelled "proposed, awaiting owner sign-off" (`:5–7`) but installed unconditionally (`signer_permissions.c:215–223`). The daemon marks every principal-bearing request `rememberable` (`signer_approval_service.c:347`), the dialog submits Remember+TTL without policy knowledge (`approval_dialog.c:112–149`), and the row is written after a successful execution (`signer_approval_service.c:954–962`). Execution matrix (Investigator Findings §1): kind 1 + Remember → second request silent; 22242/0/3/10002/13/1059 + Remember Forever → row persisted with `expires-at=0`, second request prompts **within the same second**. The behaviour is encoded as intended by `/trust-tiers/live-policy` (`nips/nip55l/tests/test_signer_permissions.c:357–362`) and was flagged as F7 "stored dormant" in the G03 review and never resolved.

**RC2 — "Trust this app" cannot mean what users expect.** Full auto-allows only operations that survive RC1's gate (`signer_permissions.c:429–435`); the executed tier matrix (Findings §4) shows Full still prompting for 0/3/10002/22242/13/1059 — i.e. every profile/contacts/relay-list change, every relay AUTH challenge and every DM envelope that gnostr, Groundhog, nostr-dav and nostr-settings send (Background: consumer inventory). Medium additionally prompts for NIP-44 crypto. So the only tier that changes anything covers notes/reactions/reposts, which is why "either an app is trusted for most messages or it isn't" reads as a false choice.

**RC3 — Remember defaults to 10 minutes.** `approval_dialog.c:322–325` preselects index 0 of `{10 minutes, 1 hour, 24 hours, Forever}` → TTL 600 (`:124–145`); expiry is absolute from execution time (`signer_permissions.c:671–681`) and a controlled-clock run prompts again at exactly t+600 (Findings §2). A user who remembers a non-required kind and returns later is re-prompted and — because expired rows are filtered from listing (`:577–586`) — sees no trace of the choice.

**RC4 — Four mechanisms, no precedence story, and the UI hides ineffectiveness.** The compact review offers Remember (scoped grant), "Change Trust Level…" (tier, separate confirmation, does not approve the request), flow checkboxes (one ephemeral, one *durable* `nip44_encrypt` row with a different expiry anchor — Findings §6c), and a `connect` Remember that is never pairing authority (`signer_permissions.c:425–427`; `signer_approval_service.c:796–799`; Findings §6b). The Permissions page (`page-client-detail.c:410–427`, fed by Control1 `ListPermissions`) renders dormant required-kind allows identically to live ones because the daemon labels every non-review row `explicit` (`signer_permissions.c:565–575`). The honesty table in Findings §5 lists every control and its real next-request effect.

**Latent hazard created by RC1:** the dormant rows are real, permanent (`expires-at=0`) authority waiting for the required list to change; removing a kind from the list — or naively reordering the evaluator — activates them without a user decision.

### Eliminated hypotheses
- **H2 principal instability** — the persisted principal has no inode; rebuilds at the same path keep grants; Groundhog's per-operation private connection changes `conn_key` only (`signer_caller.c:198–224`; `signer_approval_service.c:1071–1073`); execution with fresh connections per call matched (Findings §1). Residual, platform-specific: on macOS all claimless direct calls share principal `claimed:` (Findings §6d) — a dev-only authority caveat, not the reported symptom.
- **H3 persistence/evaluation bug** — atomic 0600 writes, commit-after-save, no erasure by revision bump/pause/legacy re-sanitisation (`signer_permissions.c:201–209, 344–373, 617–622`); write failure surfaces as `FAILED`, not a silent miss; the G03-era revision-cancellation defect is fixed (`signer_approval_service.c:990–1010`). INI snapshots after every matrix request show rows intact.
- **Default-identity drift** — genuine but correct: a grant pinned to npub A does not cover a request resolving to B after `SetDefaultIdentityForLegacy(B)` (Findings §6a; `signer_ops.c:694–698, 750–773`); the fix is explanation, not aliasing.

## Recommendations
**Owner decisions (2026-10-25), superseding the plan's §5.4 and the oracle's conservative variant:** (a) **there is no "always-confirm" class** — a user's permission is the authority; once granted for an app it is remembered until revoked, for every operation and kind; (b) the former required-confirmation list survives only as **ask-by-default** (no tier auto-allows those kinds until the user has said yes once for that app), and even that is a defaults question, not a gate; (c) initial `connect` remains a pairing decision (admission of a new client) — the only unconditional prompt; (d) **`NIP44DeriveConversationKey` / `nip44_conversation_key` is removed** — it is outside NIP-07/NIP-55, which scope approvals to `nip44_encrypt`/`nip44_decrypt`, and exporting a conversation key defeats the signer's purpose. Tracked as **nostrc-u6v55.17** (release blocker). Ship 1–6 together: reordering the evaluator alone would activate the dormant permanent rows.

1. **Precedence rewrite** (`nips/nip55l/src/glib/signer_permissions.c:signer_permissions_evaluate()`): reload/validate → explicit deny (wins) → unidentified principal / initial `connect` ⇒ ASK → **explicit live allow ⇒ ALLOW** → bounded flow consent → first-party defaults and tier preset → ASK. Delete the required-kind branch at `:420–421`; rename `signer_default_required_confirmation_kinds` to an **ask-by-default** list in `signer_permission_policy.h` consumed only by the tier branch, and remove the "proposed, awaiting sign-off" marker. Keep AUTH 22242 grants keyed per canonical relay URL (one daemon parser shared with the `auth_relay` summary; never challenge reuse; no transport-provenance claim).
2. **Remove conversation-key export**: drop `NIP44DeriveConversationKey` and `NIP44DeriveConversationKeyForApp` from `nips/nip55l/dbus/org.nostr.Signer.xml`, `signer_service_g.c`, `signer_ops.c`, the `nip44_conversation_key` operation from `signer_permissions.c`/`signer_approval_service.c`/`signer_gate.h`, and its UI/history/grants references (`apps/grotto/src/ui/approval_dialog.c`, `page-history.c`, `signer_grants.c`, `approval-controller.c`); switch nostr-seal to the existing `NIP44Decrypt` stanza path (`gnome/nostr-seal/src/nseal-signer.c:88–123`, `gtk-main.c`, `nseal-private.h`, `CMakeLists.txt`, `debian/control`); migration deletes existing `nip44_conversation_key` rows; update `docs/proposals/55L.md` (method table, 0.3.0 row, §Request kinds) and `docs/dbus-interface.md`. Version: breaking removal from the unreleased nip55l 0.7.0 surface — record per AGENTS.md (0.x breaking ⇒ MINOR with release-note call-out, or fold with the same call-out if 0.7.0 is confirmed untagged).
3. **Tiers as versioned capability presets**: Full = reads, publishing, profile/contacts/relay-list, messaging crypto + seal/wrap signing, relay AUTH, deletions, zap requests — i.e. all routine operations — and *not* unknown/custom kinds (narrowing today's Full); Medium unchanged; Low renamed "Ask unless permitted" (`signer_permissions.c:429–435`; `grotto_clients.c:121–150`). Store a preset version per client.
4. **Default duration**: "Allow once" stays preselected; the reusable choice defaults to **until revoked**, 24 h as the temporary alternative; drop the always-visible TTL dropdown and the "Remember Forever?" confirmation in favour of explicit action wording naming scope + identity (+ relay for AUTH); duration editing lives on the client Permissions page (`approval_dialog.c:112–149, 322–330`; `approval-controller.c:767–806`).
5. **One vocabulary, 1:1 with daemon actions**: *Allow once* · *Always allow this app to <op/kind> as <identity>* · *Trust this app for routine operations* (one atomic approver-only decision that records the preset **and** completes the request — new decision mode in `signer_approval_service.c`/Control1, not `SetClientTrust` + `ApproveRequest` sequenced) · *Deny once* · *Block this operation for this app* · *Block app…*; pairing offers "Pair application"; fix the "Deny once" tooltip when a reusable deny is selected (`approval_dialog.c:do_finish()/set_info()`, `approval-dialog.blp`, `approval-controller.c:decision_checked()/decide_async()/change_tier()`).
6. **Migration (versioned, restart-idempotent, before new semantics apply)**: existing allow rows for formerly-required kinds → a new `policy-review` state preserving scope, provenance and `expires-at=0` indefinitely (not the 90-day `review` state); on the next matching review offer *Activate saved permission / Allow once / Discard*; AUTH kind-only rows offer the current relay; existing Full clients keep their old ceiling under "Previous access policy" until the user accepts one explicit upgrade.
7. **Daemon reports effectiveness** (`signer_permissions_list()` / Control1 `ListPermissions`: `state`/`reason` = active | inactive: pending policy review | expired) so `page-client-detail.c:410–427` marks rows without reconstructing policy in GTK.
8. **Flow consent fully ephemeral**: no durable rows from the flow checkboxes (`signer_permissions.c:624–655`), one anchor (first arrival + ≤600 s) shown as real remaining time; clients holding routine access never see the flow prompt.
9. **Pending requests**: after a grant/preset change re-evaluate eligible pending requests and execute those now authorised (`permissions_changed()`); define which `signer_permissions_replace()` edits invalidate overlapping flows/queued work.
10. **Identity drift copy**: keep exact pinned npubs; explain in the review when the request resolves to a different identity than the saved grant.
11. **macOS dev path**: refuse reusable consent for the empty `claimed:` principal; label named claims unverifiable (`signer_caller.c:562–573`; `signer_service_g.c:140–144`).
12. **Tests**: rewrite `/trust-tiers/live-policy` (`test_signer_permissions.c:344–373`): remembered allow ⇒ ALLOW for every kind; Full preset ⇒ ALLOW for routine kinds; deny ⇒ DENY; connect allow rejected at write; conversation-key operation gone (contract test asserts the method is absent). Add: real-daemon repeat matrix across restart for every routine kind; AUTH relay isolation/canonicalisation/freshness; migration (expiry-0 rows inactive until activated; denies effective; old Full gains nothing silently); until-revoked across restart + exact 24 h boundary; flow no-disk-rows; identity A→B explanation; pending-request retirement; UI honesty (every offered reusable choice suppresses the next matching request); macOS empty-claim refusal; nostr-seal opens a sealed file via `NIP44Decrypt` only.

## Preventive Measures
- **Contract tests for UI honesty**: every control the review offers must have a test proving its advertised next-request effect against the real daemon (the honesty table in Findings §5 is the spec); "rememberable" must be computed from policy, not from principal presence.
- **Policy lists are owner decisions, not defaults smuggled in a header**: the `signer_permission_policy.h` "proposed, awaiting sign-off" comment should have blocked G07/G10 — add a static check that fails CI while a policy header carries a "proposed" marker.
- **Daemon owns effectiveness**: management APIs return why a row is or isn't in effect; GTK never infers policy.
- **Security-review findings need an owner and a bead**: G03's F7 was recorded as "low, confirmed" and closed with the item; low-severity *UX-contract* findings should be filed as beads, not left in review prose.
- **Dogfood the release matrix with a real client before Preview promotion**: a 30-minute session with gnostr + Groundhog on a native install would have surfaced RC1–RC3 immediately; add it to `docs/testing/grotto-manual-acceptance.md` as a gating step.
